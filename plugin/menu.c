/* JamaisVu savestate plugin
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <psp2kern/kernel/modulemgr.h>
#include <psp2kern/kernel/sysmem.h>
#include <psp2kern/kernel/threadmgr.h>
#include <psp2kern/io/fcntl.h>
#include <psp2kern/io/stat.h>
#include <psp2kern/ctrl.h>
#include <psp2kern/display.h>
#include <taihen.h>

#include <stdio.h>
#include <string.h>

#include "compat.h"
#include "main.h"
#include "menu.h"
#include "slots.h"
#include "storage.h"
#include "trace.h"
#include "gui/menu_view.h"

int ksceRtcGetCurrentClockLocalTime(SceDateTime *time);

#define EVF_WORK    1
#define EVF_RESUMED 2

enum { OP_NONE, OP_REFRESH, OP_SAVE, OP_LOAD, OP_TEST_RESET, OP_TEST_DRY,
       OP_TEST_DRY_FPU,
       OP_PREPARE, OP_PREPARE_ALL };

#define THUMB_PX (JV_THUMB_W * JV_THUMB_H)
#define PENDING  JV_NUM_SLOTS /* thumbnail index of the live capture */

static SceUID g_evf = -1;
static SceUID g_mtx = -1;
static volatile int g_op, g_op_slot;
static volatile int g_busy;

/* Memory for the overlay: panel, toast, thumbnails, one framebuffer row */
static SceUID g_mem_blk = -1;
static uint32_t *g_panel_px, *g_toast_px, *g_rowbuf;
static uint32_t *g_thumbs[JV_NUM_SLOTS + 1];

/* Menu model: written by the input and worker threads under g_mtx,
 * read lock-free by the display hook (seqlock: odd = being written). */
static MenuModel g_model;
static volatile uint32_t g_model_seq;
static volatile int g_menu_open;
static volatile int64_t g_block_input_until;

static char g_toast_msg[128];
static volatile int g_toast_err;
static volatile int64_t g_toast_until;
static volatile uint32_t g_toast_seq;

/* Live capture taken when the menu opens. Saving is only allowed once a
 * frame drawn by a game has been captured: in the LiveArea only the shell
 * draws, so the menu offers Load but never Save. */
static volatile int g_capture_req, g_capture_ok;
static char g_capture_tid[16];

/* Result of the last payload run (set on resume) */
static volatile uint32_t g_res_mode, g_res_slot;
static volatile int32_t g_res_result;
static volatile int g_res_stale;
static volatile uint32_t g_res_debug, g_res_stage;
static volatile int32_t g_res_detail;
static PayloadArguments g_res_pa; /* copy of the last run's results */
static volatile int g_rearm_input;

static const char *g_status_text;

/* L1+L2 data cache clean+invalidate for a VA range (SceCpuForDriver
 * 0x364E68A4, resolved by NID because its name differs between SDKs). The
 * display reads framebuffers straight from memory, so every row we write
 * must leave the CPU caches, and rows we read must not come from stale
 * cache lines. */
static int (*g_dcache_clean_inv)(void *addr, unsigned int size);

static void fb_cache_sync(void *addr, unsigned int size) {
  if (!g_dcache_clean_inv || !size)
    return;
  uintptr_t a = (uintptr_t)addr & ~(uintptr_t)63;
  uintptr_t e = ((uintptr_t)addr + size + 63) & ~(uintptr_t)63;
  g_dcache_clean_inv((void *)a, e - a);
}

static int64_t now_us(void) {
  return ksceKernelGetSystemTimeWide();
}

/* ------------------------------------------------------------------------ */
/* Model                                                                    */
/* ------------------------------------------------------------------------ */

static void model_begin(void) {
  ksceKernelLockMutex(g_mtx, 1, NULL);
  g_model_seq++;
  __sync_synchronize();
}

static void model_end(void) {
  __sync_synchronize();
  g_model_seq++;
  ksceKernelUnlockMutex(g_mtx, 1);
}

static void set_message(MenuMode mode, const char *msg, int progress,
                        int is_error) {
  model_begin();
  g_model.mode = mode;
  strncpy(g_model.message, msg, sizeof(g_model.message) - 1);
  g_model.message[sizeof(g_model.message) - 1] = '\0';
  g_model.progress = progress;
  g_model.is_error = is_error;
  model_end();
}

static void toast(const char *msg, int is_error) {
  strncpy(g_toast_msg, msg, sizeof(g_toast_msg) - 1);
  g_toast_msg[sizeof(g_toast_msg) - 1] = '\0';
  g_toast_err = is_error;
  __sync_synchronize();
  g_toast_seq++;
  /* errors stay up longer, so they can be read (and photographed) */
  g_toast_until = now_us() + (is_error ? 12000000 : 4000000);
}

#define result_text jv_result_text

/* ------------------------------------------------------------------------ */
/* Display hook: thumbnail capture and overlay                              */
/* ------------------------------------------------------------------------ */

static tai_hook_ref_t g_display_ref;
static SceUID g_display_hook = -1;

static int valid_game_titleid(const char *tid) {
  if (!tid[0] || strncmp(tid, "main", 4) == 0 || strncmp(tid, "NPXS", 4) == 0)
    return 0;
  for (int i = 0; tid[i]; i++) {
    char c = tid[i];
    if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) || i >= 15)
      return 0;
  }
  return 1;
}

static void capture(const SceDisplayFrameBuf *p) {
  char tid[32];
  memset(tid, 0, sizeof(tid));
  if (ksceKernelGetProcessTitleId(ksceKernelGetProcessId(), tid,
                                  sizeof(tid)) < 0 ||
      !valid_game_titleid(tid))
    return; /* not the game (e.g. the shell); wait for its next frame */

  uint32_t *thumb = g_thumbs[PENDING];
  int w = p->width, h = p->height;

  for (int ty = 0; ty < JV_THUMB_H; ty++) {
    int sy = (2 * ty + 1) * h / (2 * JV_THUMB_H);
    const uint32_t *src = (const uint32_t *)p->base + sy * p->pitch;
    fb_cache_sync((void *)src, w * 4);
    if (jv_copy_from_user(g_rowbuf, src, w * 4) < 0)
      return;
    for (int tx = 0; tx < JV_THUMB_W; tx++) {
      int x0 = tx * w / JV_THUMB_W, x1 = (tx + 1) * w / JV_THUMB_W;
      uint32_t r = 0, g = 0, b = 0, n = x1 - x0;
      for (int x = x0; x < x1; x++) {
        uint32_t px = g_rowbuf[x];
        r += px & 0xff;
        g += (px >> 8) & 0xff;
        b += (px >> 16) & 0xff;
      }
      thumb[ty * JV_THUMB_W + tx] =
          RGBA(r / n, g / n, b / n, 255);
    }
  }

  strncpy(g_capture_tid, tid, sizeof(g_capture_tid) - 1);
  g_capture_ok = 1;
  g_capture_req = 0;
}

/* Blends canvas c, laid out at (dx,dy) on a 960x544 screen, onto the
 * framebuffer, scaling to the framebuffer's resolution. */
static void compose(const SceDisplayFrameBuf *p, const Canvas *c, int dx,
                    int dy) {
  static int xmap[JV_SCREEN_W];
  int w = p->width, h = p->height;
  int fx0 = dx * w / JV_SCREEN_W, fy0 = dy * h / JV_SCREEN_H;
  int fw = c->w * w / JV_SCREEN_W, fh = c->h * h / JV_SCREEN_H;

  if (fw <= 0 || fh <= 0 || fx0 + fw > w || fy0 + fh > h)
    return;

  for (int i = 0; i < fw; i++)
    xmap[i] = i * c->w / fw;

  for (int j = 0; j < fh; j++) {
    int sy = j * c->h / fh;
    uint32_t *dst = (uint32_t *)p->base + (fy0 + j) * p->pitch + fx0;
    fb_cache_sync(dst, fw * 4);
    if (jv_copy_from_user(g_rowbuf, dst, fw * 4) < 0)
      return;
    cv_compose_row(g_rowbuf, &c->px[sy * c->w], xmap, fw);
    if (jv_copy_to_user(dst, g_rowbuf, fw * 4) < 0)
      return;
    fb_cache_sync(dst, fw * 4);
  }
}

static void draw_overlays(const SceDisplayFrameBuf *p) {
  static MenuModel local;
  static uint32_t rendered_seq = 1; /* odd: nothing rendered yet */
  static uint32_t rendered_toast = 0xffffffff;

  if (g_menu_open) {
    uint32_t s1 = g_model_seq;
    if (!(s1 & 1) && s1 != rendered_seq) {
      __sync_synchronize();
      memcpy(&local, &g_model, sizeof(local));
      __sync_synchronize();
      if (g_model_seq == s1) {
        Canvas c = { g_panel_px, MENU_W, MENU_H };
        menu_view_render(&c, &local);
        /* thumbnails may have changed while rendering: redo next frame */
        rendered_seq = (g_model_seq == s1) ? s1 : 1;
      }
    }
    if (!(rendered_seq & 1)) {
      Canvas c = { g_panel_px, MENU_W, MENU_H };
      compose(p, &c, MENU_X, MENU_Y);
    }
  }

  if (now_us() < g_toast_until) {
    Canvas c = { g_toast_px, TOAST_W, TOAST_H };
    if (rendered_toast != g_toast_seq) {
      rendered_toast = g_toast_seq;
      menu_view_render_toast(&c, g_toast_msg, g_toast_err);
    }
    compose(p, &c, TOAST_X, TOAST_Y);
  }
}

static int display_hook(int head, int index, const SceDisplayFrameBuf *p,
                        int sync) {
  if (g_mem_blk >= 0 && p && p->base && p->pixelformat == 0 &&
      p->width >= 480 && p->width <= 960 && p->height >= 272 &&
      p->height <= 544 && p->pitch >= p->width) {
    if (g_capture_req)
      capture(p);
    if (g_menu_open || now_us() < g_toast_until)
      draw_overlays(p);
  }

  return TAI_CONTINUE(int, g_display_ref, head, index, p, sync);
}

/* ------------------------------------------------------------------------ */
/* Controller: hide input from games while the menu is open                 */
/* ------------------------------------------------------------------------ */

static int input_blocked(void) {
  return g_menu_open || now_us() < g_block_input_until;
}

static int mask_input(SceCtrlData *pad_data, int count, int negative) {
  if (count <= 0 || !pad_data || !input_blocked())
    return count;

  for (int i = 0; i < count && i < 64; i++) {
    SceCtrlData d;
    if (jv_copy_from_user(&d, &pad_data[i],
                                     sizeof(d)) < 0)
      break;
    d.buttons = negative ? 0xffffffff : 0;
    d.lx = d.ly = d.rx = d.ry = 128;
    memset(&d.up, 0, 12); /* pressure-sensitive buttons */
    jv_copy_to_user(&pad_data[i], &d, sizeof(d));
  }
  return count;
}

#define CTRL_HOOKS(X)                              \
  X(0, 0xA9C3CED6, 0) /* PeekBufferPositive     */ \
  X(1, 0x15F81E8C, 0) /* PeekBufferPositive2    */ \
  X(2, 0xA59454D3, 0) /* PeekBufferPositiveExt  */ \
  X(3, 0x860BF292, 0) /* PeekBufferPositiveExt2 */ \
  X(4, 0x67E7AB83, 0) /* ReadBufferPositive     */ \
  X(5, 0xC4226A3E, 0) /* ReadBufferPositive2    */ \
  X(6, 0xE2D99296, 0) /* ReadBufferPositiveExt  */ \
  X(7, 0xA7178860, 0) /* ReadBufferPositiveExt2 */ \
  X(8, 0x104ED1A7, 1) /* PeekBufferNegative     */ \
  X(9, 0x81A89660, 1) /* PeekBufferNegative2    */ \
  X(10, 0x15F96FB0, 1) /* ReadBufferNegative    */ \
  X(11, 0x27A0C5FB, 1) /* ReadBufferNegative2   */

#define N_CTRL_HOOKS 12
static tai_hook_ref_t g_ctrl_refs[N_CTRL_HOOKS];
static SceUID g_ctrl_hooks[N_CTRL_HOOKS];

#define DEFINE_CTRL_HOOK(idx, nid, neg)                                   \
  static int ctrl_hook_##idx(int port, SceCtrlData *pad, int count) {     \
    int ret = TAI_CONTINUE(int, g_ctrl_refs[idx], port, pad, count);      \
    return mask_input(pad, ret, neg);                                     \
  }
CTRL_HOOKS(DEFINE_CTRL_HOOK)

#define INSTALL_CTRL_HOOK(idx, nid, neg)                                  \
  g_ctrl_hooks[idx] = taiHookFunctionExportForKernel(                     \
      KERNEL_PID, &g_ctrl_refs[idx], "SceCtrl", 0xD197E3C7, nid,           \
      ctrl_hook_##idx);

/* ------------------------------------------------------------------------ */
/* Input thread                                                             */
/* ------------------------------------------------------------------------ */

static void start_op(int op, int slot) {
  if (g_busy)
    return;
  g_busy = 1;
  g_op = op;
  g_op_slot = slot;
  ksceKernelSetEventFlag(g_evf, EVF_WORK);
}

static void open_menu(void) {
  g_capture_ok = 0;
  g_capture_req = 1;
  model_begin();
  g_model.mode = MV_BROWSE;
  g_model.can_load = 1;
  g_model.can_save = 0; /* until a game frame is captured */
  g_model.progress = -1;
  model_end();
  g_menu_open = 1;
  start_op(OP_REFRESH, 0);
}

static void close_menu(void) {
  g_menu_open = 0;
  g_capture_req = 0;
  g_block_input_until = now_us() + 300000;
}

static void confirm_prepare(int all) {
  model_begin();
  g_model.prepare_all = all;
  model_end();
  set_message(MV_CONFIRM_PREPARE, "", -1, 0);
}

static void handle_buttons(uint32_t held, uint32_t pressed) {
  int combo = (held & SCE_CTRL_SELECT) && (held & SCE_CTRL_UP) &&
              (pressed & (SCE_CTRL_SELECT | SCE_CTRL_UP));

  if (!g_menu_open) {
    if (!combo)
      return;
    /* On firmware without verified offsets the menu still opens (preview
     * mode: browse, thumbnails, input), but save/load are refused. */
    if (!g_busy)
      open_menu();
    if (!jv_armed()) {
      static char msg[128];
      snprintf(msg, sizeof(msg), "Preview only - save/load disabled: %s",
               jv_disabled_reason());
      toast(msg, 1);
    } else if (jv_armed_untested()) {
      toast("TEST BUILD - firmware not hardware-tested, back up your card",
            1);
    }
    return;
  }

  ksceKernelLockMutex(g_mtx, 1, NULL);
  MenuMode mode = g_model.mode;
  int sel = g_model.selected;
  int used = g_model.slots[sel].used;
  int locked = g_model.slots[sel].locked;
  int outdated = g_model.slots[sel].outdated;
  int prepared = g_model.slots[sel].prepared;
  int prepare_count = g_model.prepare_count;
  ksceKernelUnlockMutex(g_mtx, 1);

  if (combo && mode != MV_BUSY) {
    close_menu();
    return;
  }

  switch (mode) {
    case MV_BROWSE:
      if (pressed & (SCE_CTRL_LEFT | SCE_CTRL_RIGHT)) {
        model_begin();
        if (pressed & SCE_CTRL_LEFT)
          g_model.selected = (sel + JV_NUM_SLOTS - 1) % JV_NUM_SLOTS;
        else
          g_model.selected = (sel + 1) % JV_NUM_SLOTS;
        model_end();
      } else if ((pressed & (SCE_CTRL_CROSS | SCE_CTRL_SQUARE |
                             SCE_CTRL_TRIANGLE)) && !jv_armed()) {
        static char msg[128];
        snprintf(msg, sizeof(msg), "Disabled on this firmware: %s",
                 jv_disabled_reason());
        set_message(MV_MESSAGE, msg, -1, 1);
      } else if (pressed & SCE_CTRL_CROSS) {
        if (locked)
          set_message(MV_MESSAGE, "Not enough free space - each slot needs "
                      "704 MB", -1, 1);
        else if (g_capture_ok)
          set_message(MV_CONFIRM_SAVE, "", -1, 0);
        else if (!prepared)
          /* LiveArea: create the slot file now, so a save later in a
           * game starts right away */
          confirm_prepare(0);
        else
          set_message(MV_MESSAGE, "This slot is ready - start a game to "
                      "save here", -1, 0);
      } else if ((pressed & SCE_CTRL_TRIANGLE) && !g_capture_ok &&
                 prepare_count > 1) {
        confirm_prepare(1);
      } else if ((pressed & SCE_CTRL_SQUARE) && used && outdated) {
        set_message(MV_MESSAGE, "The game was updated since this snapshot "
                    "- it can't be loaded", -1, 1);
      } else if ((pressed & SCE_CTRL_SQUARE) && used) {
        set_message(MV_CONFIRM_LOAD, "", -1, 0);
      } else if (pressed & SCE_CTRL_START) {
        set_message(MV_DIAG, "", -1, 0);
      } else if (pressed & SCE_CTRL_CIRCLE) {
        close_menu();
      }
      break;
    case MV_DIAG:
      if (pressed & SCE_CTRL_CROSS)
        start_op(OP_TEST_RESET, sel);
      else if (pressed & SCE_CTRL_SQUARE)
        start_op(OP_TEST_DRY, sel);
      else if (pressed & SCE_CTRL_TRIANGLE)
        start_op(OP_TEST_DRY_FPU, sel);
      else if (pressed & SCE_CTRL_CIRCLE)
        set_message(MV_BROWSE, "", -1, 0);
      break;
    case MV_CONFIRM_PREPARE:
      if (pressed & SCE_CTRL_CROSS)
        start_op(g_model.prepare_all ? OP_PREPARE_ALL : OP_PREPARE, sel);
      else if (pressed & SCE_CTRL_CIRCLE)
        set_message(MV_BROWSE, "", -1, 0);
      break;
    case MV_CONFIRM_SAVE:
    case MV_CONFIRM_LOAD:
      if (pressed & SCE_CTRL_CROSS)
        start_op(mode == MV_CONFIRM_SAVE ? OP_SAVE : OP_LOAD, sel);
      else if (pressed & SCE_CTRL_CIRCLE)
        set_message(MV_BROWSE, "", -1, 0);
      break;
    case MV_MESSAGE:
      if (pressed & (SCE_CTRL_CIRCLE | SCE_CTRL_CROSS))
        set_message(MV_BROWSE, "", -1, 0);
      break;
    case MV_BUSY:
      break;
  }
}

static int input_thread(SceSize args, void *argp) {
  uint32_t prev = 0xffffffff; /* require a release before the first press */
  SceCtrlData pad;

  while (1) {
    ksceKernelDelayThread(16 * 1000);

    memset(&pad, 0, sizeof(pad));
    if (ksceCtrlPeekBufferPositive(0, &pad, 1) < 0)
      continue;

    if (g_rearm_input) {
      g_rearm_input = 0;
      prev = 0xffffffff;
    }

    /* enable "Save" once the display hook has captured a game frame */
    if (g_menu_open && g_capture_ok && !g_model.can_save) {
      model_begin();
      g_model.can_save = 1;
      model_end();
    }

    uint32_t pressed = pad.buttons & ~prev;
    prev = pad.buttons;
    if (pressed)
      handle_buttons(pad.buttons, pressed);
  }
  return 0;
}

/* ------------------------------------------------------------------------ */
/* Worker thread                                                            */
/* ------------------------------------------------------------------------ */

static SlotInfo g_pending_info;
static int g_progress_slot;

static const char *g_progress_note = " (first use only)";

static void prepare_progress(int pct) {
  static char msg[96];
  snprintf(msg, sizeof(msg), "Preparing slot %d%s  %d%%",
           g_progress_slot + 1, g_progress_note, pct);
  set_message(MV_BUSY, msg, pct, 0);
}

static void refresh_slots(void) {
  static SlotInfo info;
  int exists[JV_NUM_SLOTS], locked[JV_NUM_SLOTS];

  const char *mc = storage_detect(); /* read-only */

  model_begin();
  g_model.savedata_rolls_back = mc && strcmp(mc, "ux0:") == 0;
  model_end();

  /* free-space safeguard: only as many new slots as fit (slot_policy.h) */
  for (int i = 0; i < JV_NUM_SLOTS; i++)
    exists[i] = mc && slot_file_exists(i);
  if (!mc) {
    for (int i = 0; i < JV_NUM_SLOTS; i++)
      locked[i] = 1;
  } else {
    slot_policy(slot_free_space(), exists, locked, JV_NUM_SLOTS);
  }

  int prepare_count = 0;
  for (int i = 0; i < JV_NUM_SLOTS; i++)
    if (!exists[i] && !locked[i])
      prepare_count++;
  model_begin();
  g_model.prepare_count = prepare_count;
  model_end();

  for (int i = 0; i < JV_NUM_SLOTS; i++) {
    int ok = exists[i] && slot_info_read(i, &info, g_thumbs[i]) == 0;

    model_begin();
    MenuSlot *s = &g_model.slots[i];
    memset(s, 0, sizeof(*s));
    s->locked = locked[i];
    s->prepared = exists[i];
    if (ok) {
      s->used = 1;
      strncpy(s->titleid, info.titleid, sizeof(s->titleid) - 1);
      strncpy(s->title, info.title, sizeof(s->title) - 1);
      snprintf(s->date, sizeof(s->date), "%04d-%02d-%02d  %02d:%02d",
               info.year, info.month, info.day, info.hour, info.minute);
      s->thumb = info.has_thumb ? g_thumbs[i] : NULL;
      memcpy(s->app_ver, info.app_ver, sizeof(s->app_ver));
      s->app_ver[sizeof(s->app_ver) - 1] = '\0';
      if (lookup_app_ver(info.titleid, s->installed_ver,
                         sizeof(s->installed_ver)) < 0)
        s->installed_ver[0] = '\0';
      s->outdated = !app_versions_match(s->app_ver, s->installed_ver);
    }
    model_end();
  }
}

/* Why save/load cannot run right now, or NULL */
static const char *storage_problem(void) {
  static char msg[112];
  if (!storage_mc_root()) {
    snprintf(msg, sizeof(msg), "Unavailable: %s", storage_reason());
    return msg;
  }
  return NULL;
}

#define stage_name jv_stage_name

/* Every run is logged to data/JamaisVu/log.txt on the Memory Card (kept
 * below 64 KB), readable in VitaShell: a line when it starts, and the
 * result with the payload's step trace when the system is back. If the
 * system never came back, the next boot recovers the trace from trace.bin
 * (see trace.c). */
static void log_append(const char *text) {
  static char path[64], line[160];
  SceDateTime t;
  SceIoStat st;

  if (!storage_mc_root() || slots_mkdirs() < 0)
    return;
  jv_file_path(path, sizeof(path), "log.txt");

  int flags = SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND;
  if (ksceIoGetstat(path, &st) >= 0 && st.st_size > 64 * 1024)
    flags = SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC;

  memset(&t, 0, sizeof(t));
  ksceRtcGetCurrentClockLocalTime(&t);
  int n = snprintf(line, sizeof(line), "%04d-%02d-%02d %02d:%02d:%02d ",
                   t.year, t.month, t.day, t.hour, t.minute, t.second);

  SceUID fd = ksceIoOpen(path, flags, 0666);
  if (fd < 0)
    return;
  ksceIoWrite(fd, line, n);
  ksceIoWrite(fd, text, strlen(text));
  ksceIoClose(fd);
  slots_sync();
}

static void log_result(const char *msg) {
  static char text[1200];
  static JvTraceSector t;

  trace_from_args(&t, &g_res_pa);
  int n = trace_format(text, sizeof(text), &t);
  snprintf(text + n, sizeof(text) - n, "    message: %s\r\n", msg);
  log_append(text);
  trace_consumed(); /* already logged from the arguments */
}

static void report(const char *msg, int is_error) {
  toast(msg, is_error);
  log_result(msg);
}

static void after_resume(void) {
  static char msg[128];

  if (g_res_debug != JV_TEST_NONE) {
    /* diagnostics: report and never touch the slot info */
    if (g_res_result == JV_TEST_DONE)
      snprintf(msg, sizeof(msg), "%s passed - reached stage %u (%s)",
               g_res_debug == JV_TEST_RESET ? "Reset test" : "Dry run",
               (unsigned)g_res_stage, stage_name(g_res_stage));
    else
      /* codes first: the end of a long message is cut off on screen */
      snprintf(msg, sizeof(msg), "Stopped: stage %u, code %d/%d - %s (%s)",
               (unsigned)g_res_stage, (int)g_res_result, (int)g_res_detail,
               result_text(g_res_result), stage_name(g_res_stage));
    report(msg, g_res_result != JV_TEST_DONE);
    return;
  }

  if (g_res_mode == JAMAISVU_MODE_SAVE) {
    if (g_res_result == JV_OK) {
      int r = slot_info_write(g_res_slot, &g_pending_info, g_thumbs[PENDING]);
      if (r == 0)
        snprintf(msg, sizeof(msg), "Saved to slot %u", (unsigned)g_res_slot + 1);
      else
        snprintf(msg, sizeof(msg), "Saved, but the slot info could not be "
                 "written - slot %u stays empty", (unsigned)g_res_slot + 1);
      report(msg, r != 0);
    } else {
      snprintf(msg, sizeof(msg), "Save failed: %s",
               result_text(g_res_result));
      report(msg, 1);
    }
  } else if (g_res_mode == JAMAISVU_MODE_LOAD) {
    if (g_res_result == JV_OK) {
      if (g_res_stale)
        snprintf(msg, sizeof(msg), "Loaded slot %u - Memory Card is "
                 "read-only until restart", (unsigned)g_res_slot + 1);
      else
        snprintf(msg, sizeof(msg), "Loaded slot %u", (unsigned)g_res_slot + 1);
      report(msg, g_res_stale);
    } else {
      snprintf(msg, sizeof(msg), "Load failed: %s - nothing was changed",
               result_text(g_res_result));
      report(msg, 1);
    }
  }
}

static void run_and_wait(int mode, int slot, const uint8_t *nonce,
                         int debug_mode, int variant) {
  ksceKernelClearEventFlag(g_evf, ~EVF_RESUMED);
  close_menu();

  static char start[200];
  static uint8_t tnonce[16];
  uint32_t trel = 0;
  const char *why = "";
  int tok = trace_prepare(&trel, tnonce, &why) == 0;
  snprintf(start, sizeof(start), "starting %s%s slot=%d build=%s payload=%s; "
           "trace.bin: %s\r\n", mode == JAMAISVU_MODE_LOAD ? "load" : "save",
           debug_mode == JV_TEST_RESET ? " (reset test)" :
           debug_mode == JV_TEST_DRYRUN ? " (dry run)" : "", slot + 1,
           jv_build_id(),
           variant == JV_PAYLOAD_FPU ? "B (fpu build)" : "A (general-regs)",
           why);
  log_append(start);

  if (jv_request(mode, slot, nonce, debug_mode, trel,
                 tok ? tnonce : NULL, variant) < 0) {
    toast("Could not start - the system refused to reset", 1);
    return;
  }

  unsigned int out;
  ksceKernelWaitEventFlag(g_evf, EVF_RESUMED,
                          SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR_PAT,
                          &out, NULL);

  /* NOTE: after a successful load, this point is reached by the worker of
   * the *restored* image, which was waiting here when the snapshot was
   * taken. after_resume() looks only at the payload's result. */
  after_resume();
}

static void do_save(int slot) {
  static char msg[64];
  int res;

  /* never snapshot the LiveArea / shell: a game frame must have been
   * captured while the menu was open */
  if (!g_capture_ok) {
    set_message(MV_MESSAGE, "Start a game first - the LiveArea cannot be "
                "saved", -1, 1);
    return;
  }

  storage_detect();
  if (storage_problem()) {
    set_message(MV_MESSAGE, storage_problem(), -1, 1);
    return;
  }
  slots_mkdirs();

  g_progress_slot = slot;
  snprintf(msg, sizeof(msg), "Preparing slot %d...", slot + 1);
  set_message(MV_BUSY, msg, -1, 0);

  res = slot_prepare(slot, prepare_progress);
  if (res < 0) {
    static char err[128];
    snprintf(err, sizeof(err), "Cannot save: %s", result_text(res));
    set_message(MV_MESSAGE, err, -1, 1);
    return;
  }

  /* Re-check right before saving that a game (not the LiveArea / shell)
   * is drawing the screen, e.g. in case the PS button was used while the
   * menu was open. This also refreshes the thumbnail. */
  g_capture_ok = 0;
  g_capture_req = 1;
  for (int i = 0; i < 50 && !g_capture_ok; i++)
    ksceKernelDelayThread(10 * 1000);
  g_capture_req = 0;
  if (!g_capture_ok) {
    set_message(MV_MESSAGE, "Start a game first - the LiveArea cannot be "
                "saved", -1, 1);
    return;
  }

  /* the slot stays "empty" until the save has fully succeeded */
  slot_info_remove(slot);

  memset(&g_pending_info, 0, sizeof(g_pending_info));
  strncpy(g_pending_info.titleid, g_capture_tid,
          sizeof(g_pending_info.titleid) - 1);
  lookup_title(g_capture_tid, g_pending_info.title,
               sizeof(g_pending_info.title));
  g_pending_info.has_thumb = 1;

  /* the snapshot is only valid for this exact game version */
  if (lookup_app_ver(g_capture_tid, g_pending_info.app_ver,
                     sizeof(g_pending_info.app_ver)) < 0) {
    set_message(MV_MESSAGE, "Cannot save: the game's version could not be "
                "read", -1, 1);
    return;
  }

  SceDateTime t;
  memset(&t, 0, sizeof(t));
  ksceRtcGetCurrentClockLocalTime(&t);
  g_pending_info.year = t.year;
  g_pending_info.month = t.month;
  g_pending_info.day = t.day;
  g_pending_info.hour = t.hour;
  g_pending_info.minute = t.minute;
  g_pending_info.second = t.second;

  static uint8_t nonce[16];
  if (slot_stamp(slot, nonce) < 0) {
    set_message(MV_MESSAGE, "Cannot save: could not write the slot file",
                -1, 1);
    return;
  }

  snprintf(msg, sizeof(msg), "Saving to slot %d...", slot + 1);
  set_message(MV_BUSY, msg, -1, 0);
  slots_sync();

  run_and_wait(JAMAISVU_MODE_SAVE, slot, nonce, JV_TEST_NONE,
               JV_PAYLOAD_DEFAULT);
}

static void do_load(int slot) {
  static char msg[96];
  static SlotInfo info;

  storage_detect();
  if (storage_problem()) {
    set_message(MV_MESSAGE, storage_problem(), -1, 1);
    return;
  }

  if (!slot_is_loadable(slot) ||
      slot_info_read(slot, &info, g_thumbs[PENDING]) < 0) {
    snprintf(msg, sizeof(msg), "Slot %d is empty or damaged", slot + 1);
    set_message(MV_MESSAGE, msg, -1, 1);
    return;
  }

  /* only into exactly the game version the snapshot was taken from */
  {
    static char installed[8];
    if (lookup_app_ver(info.titleid, installed, sizeof(installed)) < 0)
      installed[0] = '\0';
    if (!app_versions_match(info.app_ver, installed)) {
      snprintf(msg, sizeof(msg), "Game version changed (v%.7s -> v%s) - "
               "this snapshot can't be loaded", info.app_ver,
               installed[0] ? installed : "?");
      set_message(MV_MESSAGE, msg, -1, 1);
      return;
    }
  }

  snprintf(msg, sizeof(msg), "Loading slot %d...", slot + 1);
  set_message(MV_BUSY, msg, -1, 0);
  slots_sync();

  run_and_wait(JAMAISVU_MODE_LOAD, slot, NULL, JV_TEST_NONE,
               JV_PAYLOAD_DEFAULT);
}

/* status.txt lives on the Memory Card like everything else JamaisVu
 * writes; it is only rewritten when its content changed. */
/* Diagnostics. Neither test writes anything: the reset test returns from
 * the payload before touching any hardware, the dry run stops right before
 * the first write (the slot file is only created/stamped, like before a
 * save, and the selected slot must be empty). */
static void do_test(int dry, int slot, int variant) {
  static uint8_t nonce[16];

  if (!jv_armed()) {
    set_message(MV_MESSAGE, "Tests need an armed build", -1, 1);
    return;
  }
  storage_detect();
  if (storage_problem()) {
    set_message(MV_MESSAGE, storage_problem(), -1, 1);
    return;
  }

  if (dry) {
    if (slot_is_loadable(slot)) {
      set_message(MV_MESSAGE, "Dry run: select an empty slot", -1, 1);
      return;
    }
    slots_mkdirs();
    g_progress_slot = slot;
    int res = slot_prepare(slot, prepare_progress);
    if (res == 0)
      res = slot_stamp(slot, nonce);
    if (res < 0) {
      static char err[128];
      snprintf(err, sizeof(err), "Cannot test: %s", result_text(res));
      set_message(MV_MESSAGE, err, -1, 1);
      return;
    }
  }

  set_message(MV_BUSY, !dry ? "Reset test..." :
              variant == JV_PAYLOAD_FPU ? "Dry run B..." : "Dry run A...",
              -1, 0);
  slots_sync();
  run_and_wait(JAMAISVU_MODE_SAVE, slot, dry ? nonce : NULL,
               dry ? JV_TEST_DRYRUN : JV_TEST_RESET, variant);
}

/* Creates slot files ahead of time (from the LiveArea), so that a save
 * later in a game does not have to. Only the OS writes here, exactly as
 * when a save prepares its slot, and the free-space safeguard is checked
 * again before each slot. */
static void do_prepare(int all, int slot) {
  static char msg[96];
  int done = 0;

  storage_detect();
  if (storage_problem()) {
    set_message(MV_MESSAGE, storage_problem(), -1, 1);
    return;
  }
  slots_mkdirs();
  g_progress_note = "";

  for (int i = all ? 0 : slot; i < (all ? JV_NUM_SLOTS : slot + 1); i++) {
    refresh_slots(); /* current free space -> which slots may be created */
    ksceKernelLockMutex(g_mtx, 1, NULL);
    int prepared = g_model.slots[i].prepared;
    int locked = g_model.slots[i].locked;
    ksceKernelUnlockMutex(g_mtx, 1);
    if (prepared)
      continue;
    if (locked) {
      if (!all) {
        g_progress_note = " (first use only)";
        set_message(MV_MESSAGE, "Not enough free space - each slot needs "
                    "704 MB", -1, 1);
        return;
      }
      continue;
    }

    g_progress_slot = i;
    prepare_progress(0);
    int res = slot_prepare(i, prepare_progress);
    if (res < 0) {
      g_progress_note = " (first use only)";
      snprintf(msg, sizeof(msg), "Slot %d: %s", i + 1, result_text(res));
      set_message(MV_MESSAGE, msg, -1, 1);
      return;
    }
    done++;
  }
  g_progress_note = " (first use only)";

  if (done == 0)
    snprintf(msg, sizeof(msg), all ? "Nothing to prepare" :
             "Slot %d is already ready", slot + 1);
  else if (!all)
    snprintf(msg, sizeof(msg), "Slot %d is ready", slot + 1);
  else
    snprintf(msg, sizeof(msg), "%d slot%s ready", done, done == 1 ? "" : "s");
  set_message(MV_MESSAGE, msg, -1, 0);
}

static void write_status(void) {
  static char path[64], old[1024];

  /* the Memory Card may not be mounted yet when the plugin starts */
  for (int i = 0; i < 60 && !storage_detect(); i++)
    ksceKernelDelayThread(1000 * 1000);
  if (!storage_mc_root() || slots_mkdirs() < 0)
    return;

  jv_file_path(path, sizeof(path), "status.txt");
  int len = strlen(g_status_text), n = -1;
  SceUID fd = ksceIoOpen(path, SCE_O_RDONLY, 0);
  if (fd >= 0) {
    n = ksceIoRead(fd, old, sizeof(old));
    ksceIoClose(fd);
  }
  if (n == len && memcmp(old, g_status_text, len) == 0)
    return;

  fd = ksceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
  if (fd < 0)
    return;
  ksceIoWrite(fd, g_status_text, len);
  ksceIoClose(fd);
  slots_sync();
}

static int worker_thread(SceSize args, void *argp) {
  static char prev[1200];

  write_status();
  /* a run that never got back to the system: its trace is in trace.bin */
  if (trace_collect(prev, sizeof(prev) - 120)) {
    static char text[1300];
    snprintf(text, sizeof(text), "PREVIOUS RUN DID NOT RETURN (recovered "
             "from trace.bin):\r\n    %s", prev);
    log_append(text);
  }

  while (1) {
    unsigned int out;
    if (ksceKernelWaitEventFlag(g_evf, EVF_WORK,
                                SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR_PAT,
                                &out, NULL) < 0)
      continue;

    switch (g_op) {
      case OP_REFRESH:
        refresh_slots();
        break;
      case OP_SAVE:
        do_save(g_op_slot);
        break;
      case OP_LOAD:
        do_load(g_op_slot);
        break;
      case OP_TEST_RESET:
        do_test(0, g_op_slot, JV_PAYLOAD_DEFAULT);
        break;
      case OP_TEST_DRY:
        do_test(1, g_op_slot, JV_PAYLOAD_DEFAULT);
        break;
      case OP_TEST_DRY_FPU:
        do_test(1, g_op_slot, JV_PAYLOAD_FPU);
        break;
      case OP_PREPARE:
        do_prepare(0, g_op_slot);
        break;
      case OP_PREPARE_ALL:
        do_prepare(1, g_op_slot);
        break;
    }

    if (g_menu_open)
      refresh_slots();
    g_op = OP_NONE;
    g_busy = 0;
  }
  return 0;
}

/* ------------------------------------------------------------------------ */

void menu_on_resumed(const PayloadArguments *pa, int stale_open_files) {
  memcpy(&g_res_pa, pa, sizeof(g_res_pa));
  g_res_debug = pa->debug_mode;
  g_res_stage = pa->stage;
  g_res_detail = pa->detail;
  g_res_mode = pa->mode;
  g_res_result = pa->result;
  g_res_slot = pa->slot;
  g_res_stale = stale_open_files;
  g_menu_open = 0;
  g_capture_req = 0;
  g_rearm_input = 1;
  g_block_input_until = now_us() + 500000;
  if (g_evf >= 0)
    ksceKernelSetEventFlag(g_evf, EVF_RESUMED);
}

int menu_init(const char *status_text) {
  SceUID thid;
  void *base;

  g_status_text = status_text;

  if (module_get_export_func(KERNEL_PID, "SceSysmem", 0x40ECDB0E, 0x364E68A4,
                             (uintptr_t *)&g_dcache_clean_inv) < 0)
    g_dcache_clean_inv = NULL;

  uint32_t size = MENU_W * MENU_H * 4 + TOAST_W * TOAST_H * 4 +
                  (JV_NUM_SLOTS + 1) * THUMB_PX * 4 + JV_SCREEN_W * 4;
  size = (size + 0xfff) & ~0xfff;

  g_mem_blk = ksceKernelAllocMemBlock("JamaisVuMenu",
                                      SCE_KERNEL_MEMBLOCK_TYPE_KERNEL_RW,
                                      size, NULL);
  if (g_mem_blk >= 0 && ksceKernelGetMemBlockBase(g_mem_blk, &base) >= 0) {
    uint32_t *p = base;
    g_panel_px = p;  p += MENU_W * MENU_H;
    g_toast_px = p;  p += TOAST_W * TOAST_H;
    for (int i = 0; i <= JV_NUM_SLOTS; i++) {
      g_thumbs[i] = p;
      p += THUMB_PX;
    }
    g_rowbuf = p;
  } else {
    g_mem_blk = -1; /* no overlay, but save/load still work */
  }

  g_evf = ksceKernelCreateEventFlag("JamaisVuEvf", 0, 0, NULL);
  g_mtx = ksceKernelCreateMutex("JamaisVuMenu", 0, 0, NULL);
  if (g_evf < 0 || g_mtx < 0)
    return -1;

  memset(&g_model, 0, sizeof(g_model));
  g_model.progress = -1;
  g_model.can_load = 1;

  g_display_hook = taiHookFunctionExportForKernel(KERNEL_PID, &g_display_ref,
                                                  "SceDisplay",
                                                  TAI_ANY_LIBRARY,
                                                  0x16466675, display_hook);

  CTRL_HOOKS(INSTALL_CTRL_HOOK)

  thid = ksceKernelCreateThread("JamaisVuInput", input_thread, 0x3C, 0x2000,
                                0, 0x10000, NULL);
  if (thid >= 0)
    ksceKernelStartThread(thid, 0, NULL);

  thid = ksceKernelCreateThread("JamaisVuWorker", worker_thread, 0x60,
                                0x4000, 0, 0x10000, NULL);
  if (thid >= 0)
    ksceKernelStartThread(thid, 0, NULL);

  return 0;
}
