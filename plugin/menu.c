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
#include <psp2kern/registrymgr.h>
#include <taihen.h>

#include <stdio.h>
#include <string.h>

#include "compat.h"
#include "main.h"
#include "menu.h"
#include "slots.h"
#include "storage.h"
#include "trace.h"
#include "hwdump.h"
#include "remap.h"
#include "prof.h"
#include "lockskip.h"
#include "appdb.h"
#include "fs.h"
#include "gui/menu_view.h"

int ksceRtcGetCurrentClockLocalTime(SceDateTime *time);

#define EVF_WORK    1
#define EVF_RESUMED 2

enum { OP_NONE, OP_REFRESH, OP_SAVE, OP_LOAD, OP_TEST_RESET, OP_TEST_DRY,
       OP_PREPARE, OP_PREPARE_ALL, OP_TEST_DISPLAY };

#define THUMB_PX (JV_THUMB_W * JV_THUMB_H)
#define PENDING  JV_NUM_SLOTS /* thumbnail index of the live capture */

static SceUID g_evf = -1;
static SceUID g_mtx = -1;
static volatile int g_op, g_op_slot;
static volatile int g_busy;
static volatile int g_cancel_req; /* ○ while preparing a slot */

/* Memory for the overlay: panel, toast, thumbnails, one framebuffer row */
/* Overlay buffers (menu panel, message banner, thumbnails, one row) are
 * only reserved while the menu or a message is shown, like reVita does:
 * during gameplay JamaisVu holds none of this memory. The display hook
 * counts itself in g_overlay_users, so the memory is only freed once no
 * frame is being drawn with it. */
static SceUID g_mem_blk = -1;
static SceUID g_ovl_mtx = -1;
static volatile int g_overlay_ready;
static volatile int g_overlay_users;
static volatile uint32_t g_overlay_gen; /* +1 per acquire: buffers are new */
static uint32_t *g_panel_px, *g_toast_px, *g_rowbuf, *g_band_px;

/* Widest frame the menu is drawn on: 960 on the handhelds; the PS TV's
 * home screen may use a larger frame for the TV (up to 1920x1088), so
 * there frames up to that size are accepted (the row buffer is sized for
 * it everywhere; it is only one row). */
#define JV_MAX_FB_W 1920
static int g_fb_max_w = 960, g_fb_max_h = 544;
static SlotEnv g_pending_env; /* plugin folders + app.db when the current
                                 save started (slotN.jve) */
static int g_dolce; /* PS TV: also controller port 1 */

/* The menu image is drawn by its own low-priority thread, not in the
 * game's display call: in Trails a redraw took ~200 ms there, and every
 * ◀/▶ stalled the game for that long. It draws BAND_ROWS rows at a time
 * into a small buffer and copies each finished band into the panel, so a
 * frame never shows a half-drawn band (and no second 1 MB panel buffer
 * is needed). g_panel_seq/g_panel_gen: model and buffers the panel shows
 * (odd seq = nothing drawn yet). */
#define BAND_ROWS 20
static SceUID g_render_sema = -1;
static int g_render_thread_ok;
static volatile uint32_t g_panel_seq = 1, g_panel_gen;
static uint32_t *g_thumbs[JV_NUM_SLOTS + 1];
static void model_clear_thumbs(void);

/* Glyphs for Japanese (non-ASCII) game names, read from jpfont.bin on
 * the Memory Card when the slot list is read; part of the overlay memory.
 * Two sets: the worker fills the one the model does not point to, then
 * switches the model to it. Drawing counts itself in g_ext_users[set]
 * (after taking its copy of the model, then re-checking the model), so a
 * set is only refilled once nothing draws with it. (One set, with the
 * worker waiting for drawing to finish, failed in games: the menu is drawn
 * at the lowest priority and the worker gave up waiting - no Japanese
 * names in game, only in the LiveArea.) */
static ExtGlyphs *g_ext_set[2];
static volatile int g_ext_users[2];

/* which set m->ext points to, or -1 */
static int ext_index(const ExtGlyphs *e) {
  if (!e)
    return -1;
  return e == g_ext_set[0] ? 0 : e == g_ext_set[1] ? 1 : -1;
}

/* Menu model: written by the input and worker threads under g_mtx,
 * read lock-free by the display hook (seqlock: odd = being written). */
static MenuModel g_model;
static volatile uint32_t g_model_seq;
static volatile int g_menu_open;
static volatile int64_t g_block_input_until;
static volatile int64_t g_lockskip_until; /* 0 = no patch to remove */

static char g_toast_msg[128];
static volatile int g_toast_err;
static volatile int64_t g_toast_until;
static volatile uint32_t g_toast_seq;

/* Live capture taken when the menu opens. Saving is only allowed once a
 * frame drawn by a game has been captured: in the LiveArea only the shell
 * draws, so the menu offers Load but never Save. */
static volatile int g_capture_req, g_capture_ok;
static char g_capture_tid[16];

/* ✕/○ convention of what is running (see remap.c). In the LiveArea the
 * console's own setting applies; in a game, its region and any ✕/○ swap
 * set for it in PSV-Shell-Plus or reVita. */
static int g_system_circle_enter;
static volatile int g_swap_ox;
static char g_swap_tid[16]; /* title the current g_swap_ox was decided for */
static void log_append(const char *text);

/* The input thread never touches the Memory Card: in heavy games (Trails)
 * a log write + sync could block it for seconds, and the menu did not
 * react. Its log lines go into this queue, and the "JamaisVuIo" thread
 * writes them; it also reads the remap plugins' profiles (decide_buttons).
 * Entries are NUL-separated strings. */
#define IOEV_LOG    1
#define IOEV_DECIDE 2
static SceUID g_io_evf = -1, g_logq_mtx = -1, g_log_mtx = -1;
static char g_logq[4096];
static int g_logq_len;
static char g_decide_tid[16];

static void log_queue(const char *text) {
  int n = (int)strlen(text) + 1;
  if (g_logq_mtx < 0 || g_io_evf < 0)
    return;
  ksceKernelLockMutex(g_logq_mtx, 1, NULL);
  if (g_logq_len + n <= (int)sizeof(g_logq)) {
    memcpy(g_logq + g_logq_len, text, n);
    g_logq_len += n;
  }
  ksceKernelUnlockMutex(g_logq_mtx, 1);
  ksceKernelSetEventFlag(g_io_evf, IOEV_LOG);
}

/* The system events recorded by main.c, as runs: S/R (suspend/resume
 * phase), id, ms of the first..last one, count. System time stops while
 * the Vita sleeps, so S and R are relative to their own first event. */
static void format_events(char *buf, int cap, const char *title) {
  int m = snprintf(buf, cap, "%s", title);
  uint32_t cnt = g_ev_n < JV_EV_MAX ? g_ev_n : JV_EV_MAX;
  for (uint32_t i = 0; i < cnt && m < cap - 32; i++) {
    uint32_t id = g_ev_id[i];
    int r = (id & 0x80000000u) != 0;
    int64_t base = r ? g_ev_r0 : g_ev_t0;
    m += snprintf(buf + m, cap - m, " %s%X@%d", r ? "R" : "S",
                  (unsigned)(id & 0x7FFFFFFFu),
                  base ? (int)((g_ev_t[i] - base) / 1000) : -1);
    if (g_ev_cnt[i] > 1 && m < cap - 32)
      m += snprintf(buf + m, cap - m, "..%d(x%u)",
                    base ? (int)((g_ev_last[i] - base) / 1000) : -1,
                    (unsigned)g_ev_cnt[i]);
  }
  snprintf(buf + m, cap - m, "\r\n");
}

static int read_small(const char *path, void *buf, int cap) {
  SceUID fd = ksceIoOpen(path, SCE_O_RDONLY, 0);
  if (fd < 0)
    return -1;
  int n = ksceIoRead(fd, buf, cap);
  ksceIoClose(fd);
  return n;
}

/* Progress screen on the PCH-1000 (OLED) and the PS TV (HDMI): only once
 * the Diagnostics display test has run through on this console without a
 * hardware wait giving up. Recorded in data/JamaisVu/display_ok.txt
 * ("type N" per display that passed); delete the file to reset. */
static int g_disp_ok_mask = -1; /* bit per JV_DISP_*, -1 = not read yet */

static int display_ok(uint32_t type) {
  if (type == JV_DISP_LCD)
    return 1; /* PCH-2000: tested on hardware */
  if (g_disp_ok_mask < 0) {
    static char path[96], buf[64];
    g_disp_ok_mask = 0;
    jv_file_path(path, sizeof(path), "display_ok.txt");
    int n = read_small(path, buf, sizeof(buf) - 1);
    if (n > 0) {
      buf[n] = '\0';
      for (int t = JV_DISP_OLED; t <= JV_DISP_HDMI; t++) {
        char key[8] = "type 0";
        key[5] = (char)('0' + t);
        if (strstr(buf, key))
          g_disp_ok_mask |= 1 << t;
      }
    }
  }
  return (g_disp_ok_mask >> type) & 1;
}

static void display_ok_record(uint32_t type) {
  static char path[96], buf[64];
  if (type != JV_DISP_OLED && type != JV_DISP_HDMI || display_ok(type))
    return;
  g_disp_ok_mask |= 1 << type;
  int n = 0;
  for (int t = JV_DISP_OLED; t <= JV_DISP_HDMI; t++)
    if ((g_disp_ok_mask >> t) & 1)
      n += snprintf(buf + n, sizeof(buf) - n, "type %d\r\n", t);
  jv_file_path(path, sizeof(path), "display_ok.txt");
  SceUID fd = ksceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
  if (fd >= 0) {
    ksceIoWrite(fd, buf, n);
    ksceIoClose(fd);
  }
}

static const char *disp_name(uint32_t type) {
  return type == JV_DISP_LCD ? "LCD, PCH-2000" :
         type == JV_DISP_OLED ? "OLED, PCH-1000" :
         type == JV_DISP_HDMI ? "HDMI, PS TV" : "none";
}

/* read-only: ur0:data/PSVshell/profiles/<TITLEID>, else "global" */
static int psvs_swap_for(const char *tid) {
  static char path[96];
  static uint8_t buf[128];
  snprintf(path, sizeof(path), "ur0:data/PSVshell/profiles/%s", tid);
  int n = read_small(path, buf, sizeof(buf));
  if (n < 0)
    n = read_small("ur0:data/PSVshell/profiles/global", buf, sizeof(buf));
  return n > 0 ? psvs_profile_swaps(buf, n) : -1;
}

/* read-only: ur0:data/reVita/Profile/<TITLEID>.INI, else GLOBAL.INI */
static int revita_swap_for(const char *tid) {
  static char path[96];
  static char buf[8192]; /* reVita's own profile buffer is ~4 KB */
  snprintf(path, sizeof(path), "ur0:data/reVita/Profile/%s.INI", tid);
  int n = read_small(path, buf, sizeof(buf));
  if (n < 0)
    n = read_small("ur0:data/reVita/Profile/GLOBAL.INI", buf, sizeof(buf));
  return n > 0 ? revita_ini_swaps(buf, n) : -1;
}

static void decide_buttons(const char *tid) {
  static char line[192];
  int psvs = psvs_swap_for(tid), revita = revita_swap_for(tid);
  int swap = menu_swap_ox(tid, psvs, revita);
  if (strncmp(g_swap_tid, tid, sizeof(g_swap_tid)) == 0)
    g_swap_ox = swap; /* still the same game: refine the quick guess */
  snprintf(line, sizeof(line), "menu buttons for %s: confirm with %s "
           "(%s game, PSV-Shell swap %s, reVita swap %s)\r\n", tid,
           swap ? "O" : "X",
           title_confirms_with_circle(tid) ? "O-confirm" : "X-confirm",
           psvs < 0 ? "n/a" : psvs ? "on" : "off",
           revita < 0 ? "n/a" : revita ? "on" : "off");
  log_append(line);
}

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

/* What the game hands to the display while JamaisVu draws on it, for
 * log.txt. Dead Nation: blocks of the game over the menu (and over
 * reVita's popup) - the game tears by itself (user, 16d519c: the same with
 * and without frames held back), so it writes into frames that are on
 * screen and anything drawn over them is damaged. Left as a known issue;
 * the system layer (5765736) and holding frames (979e26c, 16d519c) were
 * tried and removed. */
static struct {
  uint32_t frames, same_as_prev, copy_fail, cleans, sync_counts[3];
  int clean_ret;
  uint32_t w, h, pitch, nbases;
  uintptr_t prev, bases[4];
  /* drawn-over check: menu pixels read back from the previous frame when
   * the next one arrives (that frame is still on screen until the flip) */
  uint32_t chk_frames, chk_hit, chk_px, chk_changed, chk_us_max;
  /* the cache flush after drawing, in 32 KB pieces: what each returned */
  uint32_t fl_pieces, fl_ret0, fl_ret1, fl_other;
  int fl_other_val;
  /* the system overlay layer (index 1) of the primary display while the
   * menu is open in a game: could the menu live there instead? */
  uint32_t l0_calls, l1_calls, l1_set;
  uintptr_t l1_base;
  uint32_t l1_fmt, l1_w, l1_h, l1_pitch;
} g_fbd;

/* where the last menu compose put a few of its pixels, and their values */
#define CHK_N 32
static struct {
  uintptr_t base;
  int n;
  int64_t t;
  uint32_t off[CHK_N], val[CHK_N];
} g_chk;

static int fb_cache_sync(void *addr, unsigned int size) {
  if (!g_dcache_clean_inv || !size)
    return -1;
  uintptr_t a = (uintptr_t)addr & ~(uintptr_t)63;
  uintptr_t e = ((uintptr_t)addr + size + 63) & ~(uintptr_t)63;
  return g_dcache_clean_inv((void *)a, e - a);
}

/* Display test: where the frame on screen is in physical memory. The
 * display hook runs in the game's thread, so its addresses translate
 * with the CPU (privileged read translation, like main.c's va2pa). The
 * payload draws there with the MMU off, so the whole frame must be one
 * physically contiguous block - checked page by page. */
static volatile int g_fb_rec; /* recorded for this menu session */
static volatile uint32_t g_fb_pa, g_fb_pitch, g_fb_w, g_fb_h;
/* why g_fb_pa is 0: the frame's address, its first translation, and the
 * offset where the translation failed or stopped being contiguous */
static volatile uint32_t g_fb_va, g_fb_pa0, g_fb_bad_off;
static volatile int g_fb_game; /* recorded from a game's frame */
static volatile uint32_t g_disp_vic; /* display test timing, see input */

/* the frame recorded for the save in progress (written to slotN.jvf when
 * the save succeeds) */
static struct {
  int valid;
  uint32_t pa, pitch, w, h;
} g_save_frame;


/* Four ways to translate a game address, tried in order; the log shows
 * each one's raw result (on hardware the CPU translation alone failed for
 * 0x62320000 in 1001 Spikes, even with the domains open). */
static int (*g_va2pa_hw)(const void *va, uintptr_t *pa); /* VAtoPA */
static int (*g_va2pa_sw)(const void *va, void **pa);      /* VAtoPABySW */
static volatile uint32_t g_fb_par_priv, g_fb_par_user;
static volatile int32_t g_fb_hw_ret, g_fb_sw_ret;
static volatile uint32_t g_fb_hw_pa, g_fb_sw_pa;
static volatile int g_fb_method; /* 0 none, 1-4 as below */

/* CPU translation (ATS1CPR / ATS1CUR); returns the raw PAR */
static uint32_t ats(uintptr_t va, int user) {
  uint32_t par;
  if (user)
    __asm__ volatile("mcr p15, 0, %0, c7, c8, 2\n\tisb" :: "r"(va));
  else
    __asm__ volatile("mcr p15, 0, %0, c7, c8, 0\n\tisb" :: "r"(va));
  __asm__ volatile("mrc p15, 0, %0, c7, c4, 0" : "=r"(par));
  return par;
}

static uint32_t par_to_pa(uint32_t par, uintptr_t va) {
  return (par & 1) ? 0 : ((par & 0xfffff000) | (va & 0xfff));
}

/* physical address of va by method m, 0 if it fails; methods 1-2 need the
 * domains open (see record_frame) */
static uint32_t translate(int m, uintptr_t va) {
  if (m == 1 || m == 2)
    return par_to_pa(ats(va, m == 2), va);
  if (m == 3 && g_va2pa_hw) {
    uintptr_t pa = 0;
    return g_va2pa_hw((const void *)va, &pa) >= 0 ? (uint32_t)pa : 0;
  }
  if (m == 4 && g_va2pa_sw) {
    void *pa = NULL;
    return g_va2pa_sw((const void *)va, &pa) >= 0 ? (uint32_t)(uintptr_t)pa
                                                    : 0;
  }
  return 0;
}

static int valid_game_titleid(const char *tid);

static void record_frame(const SceDisplayFrameBuf *p) {
  uintptr_t base = (uintptr_t)p->base;
  uint32_t size = p->pitch * p->height * 4;
  uint32_t dacr, cpsr;
  char tid[32];

  memset(tid, 0, sizeof(tid));
  g_fb_game = ksceKernelGetProcessTitleId(ksceKernelGetProcessId(), tid,
                                          sizeof(tid)) >= 0 &&
              valid_game_titleid(tid);
  g_fb_pa = 0;
  g_fb_va = base;
  g_fb_pa0 = 0;
  g_fb_bad_off = 0;
  g_fb_method = 0;

  /* CPU translations with the domains open (the kernel keeps the user
   * domain at "no access"), interrupts off, no kernel calls meanwhile */
  __asm__ volatile("mrs %0, cpsr\n\tcpsid if" : "=r"(cpsr) :: "memory");
  __asm__ volatile("mrc p15, 0, %0, c3, c0, 0" : "=r"(dacr));
  __asm__ volatile("mcr p15, 0, %0, c3, c0, 0\n\tisb" :: "r"(0xFFFF0000u)
                   : "memory");
  g_fb_par_priv = ats(base, 0);
  g_fb_par_user = ats(base, 1);
  __asm__ volatile("mcr p15, 0, %0, c3, c0, 0\n\tisb" :: "r"(dacr) : "memory");
  __asm__ volatile("msr cpsr_c, %0" :: "r"(cpsr) : "memory");

  /* the kernel's own translations */
  {
    uintptr_t pa = 0;
    void *spa = NULL;
    g_fb_hw_ret = g_va2pa_hw ? g_va2pa_hw((const void *)base, &pa) : -1;
    g_fb_hw_pa = (uint32_t)pa;
    g_fb_sw_ret = g_va2pa_sw ? g_va2pa_sw((const void *)base, &spa) : -1;
    g_fb_sw_pa = (uint32_t)(uintptr_t)spa;
  }

  int m = 0;
  if (!(g_fb_par_priv & 1))
    m = 1;
  else if (!(g_fb_par_user & 1))
    m = 2;
  else if (g_fb_hw_ret >= 0 && g_fb_hw_pa)
    m = 3;
  else if (g_fb_sw_ret >= 0 && g_fb_sw_pa)
    m = 4;
  g_fb_method = m;

  if (m) {
    int open = m <= 2;
    if (open) {
      __asm__ volatile("mrs %0, cpsr\n\tcpsid if" : "=r"(cpsr) :: "memory");
      __asm__ volatile("mrc p15, 0, %0, c3, c0, 0" : "=r"(dacr));
      __asm__ volatile("mcr p15, 0, %0, c3, c0, 0\n\tisb"
                       :: "r"(0xFFFF0000u) : "memory");
    }
    uint32_t pa0 = translate(m, base), off;
    for (off = 4096 - (base & 0xfff); pa0 && off < size; off += 4096)
      if (translate(m, base + off) != pa0 + off)
        break;
    if (open) {
      __asm__ volatile("mcr p15, 0, %0, c3, c0, 0\n\tisb" :: "r"(dacr)
                       : "memory");
      __asm__ volatile("msr cpsr_c, %0" :: "r"(cpsr) : "memory");
    }
    g_fb_pa0 = pa0;
    if (pa0 && off >= size)
      g_fb_pa = pa0;
    else
      g_fb_bad_off = off;
  }
  g_fb_pitch = p->pitch;
  g_fb_w = p->width;
  g_fb_h = p->height;
  __sync_synchronize();
  g_fb_rec = 1;
}

static void fbd_frame(const SceDisplayFrameBuf *p, int sync) {
  uintptr_t b = (uintptr_t)p->base;
  g_fbd.frames++;
  if (b == g_fbd.prev)
    g_fbd.same_as_prev++;
  g_fbd.prev = b;
  uint32_t k;
  for (k = 0; k < g_fbd.nbases && k < 4; k++)
    if (g_fbd.bases[k] == b)
      break;
  if (k == g_fbd.nbases) {
    if (k < 4)
      g_fbd.bases[k] = b;
    g_fbd.nbases++;
  }
  g_fbd.sync_counts[sync == 0 ? 0 : sync == 1 ? 1 : 2]++;
  g_fbd.w = p->width;
  g_fbd.h = p->height;
  g_fbd.pitch = p->pitch;
}

static int64_t now_us(void) {
  return ksceKernelGetSystemTimeWide();
}

volatile JvProfStat g_prof[PROF_N];

uint32_t prof_now(void) {
  return (uint32_t)ksceKernelGetSystemTimeWide();
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

static void model_clear_thumbs(void) {
  model_begin();
  g_model.ext = NULL;
  for (int i = 0; i < JV_NUM_SLOTS; i++)
    g_model.slots[i].thumb = NULL;
  model_end();
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

/* what each memory pool returned the last time (for log.txt) */
static int g_ovl_alloc_ret[3], g_ovl_alloc_type;

static int overlay_acquire(void) {
  int res = 0;
  void *base;

  if (g_ovl_mtx >= 0)
    ksceKernelLockMutex(g_ovl_mtx, 1, NULL);
  if (!g_overlay_ready) {
    uint32_t size = MENU_W * MENU_H * 4 + TOAST_W * TOAST_H * 4 +
                    (JV_NUM_SLOTS + 1) * THUMB_PX * 4 + JV_MAX_FB_W * 4 +
                    MENU_W * BAND_ROWS * 4 + 2 * ((sizeof(ExtGlyphs) + 3) & ~3u);
    size = (size + 0xfff) & ~0xfff;
    /* The kernel root pool (KERNEL_RW) can be full with many kernel
     * plugins (PCH-1000 log: "not enough free kernel memory"); then the
     * root pool's physically contiguous part, then kernel memory from the
     * user main pool, each only for as long as the menu is shown. */
    static const SceKernelMemBlockType types[3] = {
      0x1020D006, /* KERNEL_ROOT_RW (= KERNEL_RW) */
      0x1080D006, /* KERNEL_ROOT_PHYCONT_RW (2 MB worked in Dead Nation) */
      0x10C0D006, /* KERNEL_ROOT_UMAIN_RW */
    };
    g_mem_blk = -1;
    for (int i = 0; i < 3 && g_mem_blk < 0; i++) {
      g_mem_blk = ksceKernelAllocMemBlock("JamaisVuMenu", types[i], size,
                                          NULL);
      g_ovl_alloc_ret[i] = g_mem_blk;
      g_ovl_alloc_type = i;
    }
    if (g_mem_blk >= 0 && ksceKernelGetMemBlockBase(g_mem_blk, &base) >= 0) {
      uint32_t *p = base;
      g_panel_px = p;  p += MENU_W * MENU_H;
      g_toast_px = p;  p += TOAST_W * TOAST_H;
      for (int i = 0; i <= JV_NUM_SLOTS; i++) {
        g_thumbs[i] = p;
        p += THUMB_PX;
      }
      g_rowbuf = p;    p += JV_MAX_FB_W;
      g_band_px = p;   p += MENU_W * BAND_ROWS;
      for (int k = 0; k < 2; k++) {
        g_ext_set[k] = (ExtGlyphs *)p;
        g_ext_set[k]->n = 0;
        g_ext_set[k]->used = 0;
        p += ((sizeof(ExtGlyphs) + 3) & ~3u) / 4;
      }
      g_overlay_gen++;
      __sync_synchronize();
      g_overlay_ready = 1;
    } else {
      if (g_mem_blk >= 0)
        ksceKernelFreeMemBlock(g_mem_blk);
      g_mem_blk = -1;
      res = -1;
    }
  }
  if (g_ovl_mtx >= 0)
    ksceKernelUnlockMutex(g_ovl_mtx, 1);
  return res;
}

static void overlay_release(void) {
  if (g_ovl_mtx >= 0)
    ksceKernelLockMutex(g_ovl_mtx, 1, NULL);
  if (g_overlay_ready) {
    g_overlay_ready = 0;
    __sync_synchronize();
    /* wait for a frame that is still drawing with the buffers */
    for (int i = 0; i < 500 && g_overlay_users; i++)
      ksceKernelDelayThread(1000);
    if (g_overlay_users == 0) {
      model_clear_thumbs(); /* the model must not point into freed memory */
      ksceKernelFreeMemBlock(g_mem_blk);
      g_mem_blk = -1;
      g_panel_px = g_toast_px = g_rowbuf = g_band_px = NULL;
      g_ext_set[0] = g_ext_set[1] = NULL;
      for (int i = 0; i <= JV_NUM_SLOTS; i++)
        g_thumbs[i] = NULL;
    } else {
      g_overlay_ready = 1; /* still in use: try again later */
    }
  }
  if (g_ovl_mtx >= 0)
    ksceKernelUnlockMutex(g_ovl_mtx, 1);
}

static void toast(const char *msg, int is_error) {
  if (overlay_acquire() < 0)
    return; /* no memory for the banner; log.txt still has the result */
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
/* Which canvas rows need blending (rounded corners, translucent edges);
 * every other row is fully opaque and is written to the framebuffer
 * without reading it first. Reading the game's framebuffer back is very
 * slow in some games (Trails: 8-47 ms per frame with the old per-row
 * read-modify-write), and it runs in the game's own rendering thread. */
static uint8_t g_panel_blend[MENU_H], g_toast_blend[TOAST_H];

static void scan_blend_rows(const Canvas *c, uint8_t *rows) {
  for (int y = 0; y < c->h; y++) {
    const uint32_t *r = &c->px[y * c->w];
    int blend = 0;
    for (int x = 0; x < c->w && !blend; x++)
      blend = (r[x] >> 24) != 255;
    rows[y] = (uint8_t)blend;
  }
}

/* Puts canvas c, laid out at (dx,dy) on a 960x544 screen, onto the
 * framebuffer, scaling to the framebuffer's resolution. */
static void compose(const SceDisplayFrameBuf *p, const Canvas *c,
                    const uint8_t *blend_rows, int dx, int dy, int record) {
  static int xmap[JV_MAX_FB_W];
  int w = p->width, h = p->height;
  int fx0 = dx * w / JV_SCREEN_W, fy0 = dy * h / JV_SCREEN_H;
  int fw = c->w * w / JV_SCREEN_W, fh = c->h * h / JV_SCREEN_H;

  if (fw <= 0 || fh <= 0 || fx0 + fw > w || fy0 + fh > h)
    return;

  for (int i = 0; i < fw; i++)
    xmap[i] = i * c->w / fw;

  /* time split for log.txt: reading the game's rows, building our rows,
   * copying them into the frame, the final cache clean */
  uint32_t t_read = 0, t_build = 0, t_write = 0, t;
  for (int j = 0; j < fh; j++) {
    int sy = j * c->h / fh;
    const uint32_t *src = &c->px[sy * c->w];
    uint32_t *dst = (uint32_t *)p->base + (fy0 + j) * p->pitch + fx0;
    t = prof_now();
    if (blend_rows[sy]) {
      /* only these few rows read the game's pixels */
      fb_cache_sync(dst, fw * 4);
      if (jv_copy_from_user(g_rowbuf, dst, fw * 4) < 0)
        return;
      uint32_t t1 = prof_now();
      t_read += t1 - t;
      t = t1;
      cv_compose_row(g_rowbuf, src, xmap, fw);
    } else if (fw == c->w) {
      /* opaque row at full size (the usual case): every pixel already has
       * alpha 255, so the panel row is exactly what goes on screen - copy
       * it straight from there. Building a copy of each row first cost
       * ~9.5 ms per frame in Trails (log.txt, compose-build). */
      uint32_t t1 = prof_now();
      t_build += t1 - t;
      if (jv_copy_to_user(dst, src, fw * 4) < 0) {
        g_fbd.copy_fail++;
        return;
      }
      t_write += prof_now() - t1;
      continue;
    } else {
      for (int i = 0; i < fw; i++)
        g_rowbuf[i] = 0xff000000u | (src[xmap[i]] & 0x00ffffffu);
    }
    uint32_t t1 = prof_now();
    t_build += t1 - t;
    if (jv_copy_to_user(dst, g_rowbuf, fw * 4) < 0) {
      g_fbd.copy_fail++;
      return;
    }
    t_write += prof_now() - t1;
  }
  /* one cache clean for the whole area (was two per row) */
  t = prof_now();
  uint32_t *first = (uint32_t *)p->base + fy0 * p->pitch + fx0;
  /* flushed in 32 KB pieces, every result counted for log.txt (checked
   * for Dead Nation's blocks: every piece returned 1; the blocks are the
   * game's own tearing, see g_fbd) */
  {
    uintptr_t a = (uintptr_t)first, e = a + ((fh - 1) * p->pitch + fw) * 4;
    while (a < e) {
      uint32_t n = e - a > 0x8000 ? 0x8000 : (uint32_t)(e - a);
      int r = fb_cache_sync((void *)a, n);
      g_fbd.fl_pieces++;
      if (r == 0)
        g_fbd.fl_ret0++;
      else if (r == 1)
        g_fbd.fl_ret1++;
      else {
        g_fbd.fl_other++;
        g_fbd.fl_other_val = r;
      }
      g_fbd.clean_ret = r;
      a += n;
    }
  }
  g_fbd.cleans++;
  /* drawn-over check: 8 opaque rows x 4 columns, values as written */
  if (record && fw == c->w) {
    int n = 0;
    for (int k = 1; k <= 8; k++) {
      int j = k * fh / 9, sy = j * c->h / fh;
      if (blend_rows[sy])
        continue;
      for (int q = 1; q <= 4 && n < CHK_N; q++) {
        int x = q * fw / 5;
        g_chk.off[n] = (uint32_t)((fy0 + j) * p->pitch + fx0 + x);
        g_chk.val[n] = c->px[sy * c->w + x];
        n++;
      }
    }
    g_chk.base = (uintptr_t)p->base;
    g_chk.t = now_us();
    __sync_synchronize();
    g_chk.n = n;
  }
  prof_add(PROF_C_SYNC, t);
  prof_add_us(PROF_C_READ, t_read);
  prof_add_us(PROF_C_BUILD, t_build);
  prof_add_us(PROF_C_WRITE, t_write);
}

/* The render thread: brings the panel up to date with the model. */
static void render_panel(void) {
  static MenuModel local;
  static uint8_t blend[BAND_ROWS];

  __sync_fetch_and_add(&g_overlay_users, 1);
  __sync_synchronize();
  for (;;) {
    if (!g_overlay_ready || !g_menu_open)
      break;
    uint32_t gen = g_overlay_gen, s1 = g_model_seq;
    if (s1 & 1) { /* being written: look again shortly */
      ksceKernelDelayThread(1000);
      continue;
    }
    if (s1 == g_panel_seq && gen == g_panel_gen)
      break; /* up to date */
    __sync_synchronize();
    memcpy(&local, &g_model, sizeof(local));
    __sync_synchronize();
    int xi = ext_index(local.ext);
    if (xi >= 0)
      __sync_fetch_and_add(&g_ext_users[xi], 1);
    __sync_synchronize();
    if (g_model_seq != s1) { /* changed meanwhile (maybe its glyph set) */
      if (xi >= 0)
        __sync_fetch_and_sub(&g_ext_users[xi], 1);
      continue;
    }

    uint32_t t0 = prof_now();
    int stopped = 0;
    for (int y0 = 0; y0 < MENU_H; y0 += BAND_ROWS) {
      if (!g_overlay_ready || !g_menu_open) { /* closing: stop early */
        stopped = 1;
        break;
      }
      int rows = MENU_H - y0 < BAND_ROWS ? MENU_H - y0 : BAND_ROWS;
      Canvas b = { g_band_px, MENU_W, rows, y0 };
      menu_view_render(&b, &local);
      scan_blend_rows(&b, blend);
      memcpy(&g_panel_px[y0 * MENU_W], g_band_px, rows * MENU_W * 4);
      memcpy(&g_panel_blend[y0], blend, rows);
      __sync_synchronize();
    }
    if (xi >= 0)
      __sync_fetch_and_sub(&g_ext_users[xi], 1);
    if (stopped)
      break;
    prof_add(PROF_RENDER, t0);
    g_panel_seq = s1;
    __sync_synchronize();
    g_panel_gen = gen; /* the panel is complete: it may be shown */
  }
  __sync_synchronize();
  __sync_fetch_and_sub(&g_overlay_users, 1);
}

static int render_thread(SceSize args, void *argp) {
  (void)args;
  (void)argp;
  for (;;) {
    if (ksceKernelWaitSema(g_render_sema, 1, NULL) < 0)
      ksceKernelDelayThread(100000);
    render_panel();
  }
  return 0;
}

static void draw_overlays(const SceDisplayFrameBuf *p) {
  static MenuModel local;
  static uint32_t rendered_seq = 1; /* odd: nothing rendered yet */
  static uint32_t rendered_toast = 0xffffffff;
  static uint32_t gen = 0;

  if (gen != g_overlay_gen) { /* fresh buffers: nothing rendered in them */
    gen = g_overlay_gen;
    rendered_seq = 1;
    rendered_toast = 0xffffffff;
  }

  if (g_menu_open && g_render_sema >= 0) {
    /* drawn by render_thread; here only put on the frame */
    if (g_panel_seq != g_model_seq || g_panel_gen != g_overlay_gen)
      ksceKernelSignalSema(g_render_sema, 1);
    if (g_panel_gen == g_overlay_gen && !(g_panel_seq & 1)) {
      uint32_t t0 = prof_now();
      Canvas c = { g_panel_px, MENU_W, MENU_H, 0 };
      compose(p, &c, g_panel_blend, MENU_X, MENU_Y, 1);
      prof_add(PROF_COMPOSE, t0);
    }
  } else if (g_menu_open) { /* no render thread: draw here */
    uint32_t s1 = g_model_seq;
    if (!(s1 & 1) && s1 != rendered_seq) {
      __sync_synchronize();
      memcpy(&local, &g_model, sizeof(local));
      __sync_synchronize();
      int xi = ext_index(local.ext);
      if (xi >= 0)
        __sync_fetch_and_add(&g_ext_users[xi], 1);
      __sync_synchronize();
      if (g_model_seq == s1) {
        uint32_t t0 = prof_now();
        Canvas c = { g_panel_px, MENU_W, MENU_H, 0 };
        menu_view_render(&c, &local);
        scan_blend_rows(&c, g_panel_blend);
        prof_add(PROF_RENDER, t0);
        /* the render is complete (of the model at s1): show it now; if the
         * model changed meanwhile, the next frame renders again */
        rendered_seq = s1;
      }
      if (xi >= 0)
        __sync_fetch_and_sub(&g_ext_users[xi], 1);
    }
    if (!(rendered_seq & 1)) {
      uint32_t t0 = prof_now();
      Canvas c = { g_panel_px, MENU_W, MENU_H, 0 };
      compose(p, &c, g_panel_blend, MENU_X, MENU_Y, 1);
      prof_add(PROF_COMPOSE, t0);
    }
  }

  if (now_us() < g_toast_until) {
    Canvas c = { g_toast_px, TOAST_W, TOAST_H, 0 };
    if (rendered_toast != g_toast_seq) {
      rendered_toast = g_toast_seq;
      menu_view_render_toast(&c, g_toast_msg, g_toast_err);
      scan_blend_rows(&c, g_toast_blend);
    }
    compose(p, &c, g_toast_blend, TOAST_X, TOAST_Y, 0);
  }
}

/* Which frames get the overlay, decided by what is in the foreground (as
 * reVita does), not by which process submitted the frame: only the
 * primary display; with a game running (an exclusive process) its layer
 * 0, otherwise (LiveArea) the shell's layer 1. Deciding by the caller
 * made the menu flicker in the LiveArea: some layer-0 frames there do not
 * come from SceShell's process. */
int ksceAppMgrIsExclusiveProcessRunning(void);

static int overlay_target(int head, int index) {
  if (head != ksceDisplayGetPrimaryHead())
    return 0;
  if (ksceAppMgrIsExclusiveProcessRunning())
    return index == 0;
  return index == 1;
}

/* time of the first frame on the display after a save/load (0 = waiting) */
static volatile int64_t g_t_first_frame;
static volatile int g_wait_first_frame;
/* the first frame on any head/index (to tell "the game drew late" from
 * "frames came, but not on the head/index the overlay uses") */
static volatile int64_t g_t_first_any;
static volatile int g_first_any_head, g_first_any_index;
static volatile int g_wait_first_any;

/* Reads back the menu pixels of the frame drawn last time. Called when the
 * next frame arrives: the old one is still on screen until the flip, so a
 * game that has finished with it must not have written there. */
static void drawn_over_check(uintptr_t next_base) {
  int n = g_chk.n;
  if (!n || !g_chk.base || g_chk.base == next_base)
    return;
  uint32_t changed = 0;
  for (int i = 0; i < n; i++) {
    uint32_t v;
    uint32_t *a = (uint32_t *)g_chk.base + g_chk.off[i];
    fb_cache_sync(a, 4);
    if (jv_copy_from_user(&v, a, 4) < 0) {
      g_chk.n = 0;
      return;
    }
    if (v != g_chk.val[i])
      changed++;
  }
  uint32_t us = (uint32_t)(now_us() - g_chk.t);
  g_fbd.chk_frames++;
  g_fbd.chk_px += n;
  g_fbd.chk_changed += changed;
  if (changed)
    g_fbd.chk_hit++;
  if (us > g_fbd.chk_us_max)
    g_fbd.chk_us_max = us;
  g_chk.n = 0;
}

static int display_hook(int head, int index, const SceDisplayFrameBuf *p,
                        int sync) {
  uint32_t t0 = prof_now();
  if (g_menu_open && head == ksceDisplayGetPrimaryHead() &&
      ksceAppMgrIsExclusiveProcessRunning()) {
    if (index == 0)
      g_fbd.l0_calls++;
    else if (index == 1) {
      g_fbd.l1_calls++;
      if (p && p->base) {
        g_fbd.l1_set++;
        g_fbd.l1_base = (uintptr_t)p->base;
        g_fbd.l1_fmt = p->pixelformat;
        g_fbd.l1_w = p->width;
        g_fbd.l1_h = p->height;
        g_fbd.l1_pitch = p->pitch;
      }
    }
  }
  if (g_wait_first_any && p && p->base) {
    g_t_first_any = ksceKernelGetSystemTimeWide();
    g_first_any_head = head;
    g_first_any_index = index;
    g_wait_first_any = 0;
  }
  if (g_wait_first_frame && p && p->base && overlay_target(head, index)) {
    g_t_first_frame = ksceKernelGetSystemTimeWide();
    g_wait_first_frame = 0;
  }
  if (g_overlay_ready && p && p->base && p->pixelformat == 0 &&
      overlay_target(head, index) &&
      p->width >= 480 && p->width <= g_fb_max_w && p->height >= 272 &&
      p->height <= g_fb_max_h && p->pitch >= p->width) {
    __sync_fetch_and_add(&g_overlay_users, 1);
    __sync_synchronize();
    if (g_overlay_ready) { /* re-check: a release may have started */
      if (g_capture_req)
        capture(p);
      if (g_menu_open || now_us() < g_toast_until) {
        if (g_menu_open) {
          drawn_over_check((uintptr_t)p->base);
          fbd_frame(p, sync);
          if (!g_fb_rec)
            record_frame(p);
        }
        draw_overlays(p);
      }
    }
    __sync_fetch_and_sub(&g_overlay_users, 1);
  }
  prof_add(PROF_DISPLAY, t0);

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
    uint32_t t0 = prof_now();                                             \
    int ret = TAI_CONTINUE(int, g_ctrl_refs[idx], port, pad, count);      \
    uint32_t t1 = prof_now();                                             \
    ret = mask_input(pad, ret, neg);                                      \
    prof_add(PROF_CTRL, t1);                                              \
    prof_add(PROF_CTRL_TOTAL, t0);                                        \
    return ret;                                                           \
  }
CTRL_HOOKS(DEFINE_CTRL_HOOK)

#define INSTALL_CTRL_HOOK(idx, nid, neg)                                  \
  g_ctrl_hooks[idx] = taiHookFunctionExportForKernel(                     \
      KERNEL_PID, &g_ctrl_refs[idx], "SceCtrl", 0xD197E3C7, nid,           \
      ctrl_hook_##idx);

/* ------------------------------------------------------------------------ */
/* Input thread                                                             */
/* ------------------------------------------------------------------------ */

/* A press that arrives while a job runs (e.g. the slot refresh right after
 * the menu opened, which is slow in heavy games) is kept and runs right
 * after it, instead of being dropped (one is kept). */
static volatile int g_next_op = OP_NONE, g_next_slot;

static void start_op(int op, int slot) {
  ksceKernelLockMutex(g_mtx, 1, NULL);
  if (g_busy) {
    if (g_next_op == OP_NONE && op != OP_REFRESH) {
      g_next_slot = slot;
      g_next_op = op;
    }
    ksceKernelUnlockMutex(g_mtx, 1);
    return;
  }
  g_busy = 1;
  g_cancel_req = 0;
  g_op = op;
  g_op_slot = slot;
  ksceKernelUnlockMutex(g_mtx, 1);
  ksceKernelSetEventFlag(g_evf, EVF_WORK);
}

/* Diagnostics: counts and own time of everything JamaisVu runs during
 * gameplay, since the previous report; written when the menu opens. */
static uint32_t g_perf_since; /* set in menu_init: first report = since boot */

static void perf_report(void) {
  static JvProfStat last[PROF_N];
  uint32_t last_t = g_perf_since;
  static char line[640];
  static const char *const names[PROF_N] = {
    "display", "ctrl", "ctrl-incl-original", "syscon", "exfat-write", "input",
    "render", "compose", "compose-read", "compose-build", "compose-write",
    "compose-cacheclean"
  };
  uint32_t now = prof_now();
  uint32_t secs = (now - last_t) / 1000000;
  int n = snprintf(line, sizeof(line), "perf over %u s (calls / ms of "
                   "JamaisVu's own time; menu drawn %s):", (unsigned)secs,
                   g_render_thread_ok ? "in background" : "in display hook");
  for (int i = 0; i < PROF_N && n < (int)sizeof(line) - 48; i++) {
    uint32_t c = g_prof[i].calls - last[i].calls;
    uint32_t us = g_prof[i].us - last[i].us;
    n += snprintf(line + n, sizeof(line) - n, " %s %u/%u", names[i],
                  (unsigned)c, (unsigned)(us / 1000));
    last[i].calls = g_prof[i].calls;
    last[i].us = g_prof[i].us;
  }
  snprintf(line + n, sizeof(line) - n, "\r\n");
  g_perf_since = now;
  log_queue(line);

  if (g_fbd.frames) {
    snprintf(line, sizeof(line),
             "    frames with the menu: %u, %ux%u pitch %u, buffers %u "
             "(%08X %08X %08X %08X), same buffer twice in a row %u, sync "
             "immediate/next-frame/other %u/%u/%u; cache clean %s, last "
             "result 0x%08X (%u); copy failures %u\r\n",
             (unsigned)g_fbd.frames, (unsigned)g_fbd.w, (unsigned)g_fbd.h,
             (unsigned)g_fbd.pitch, (unsigned)g_fbd.nbases,
             (unsigned)g_fbd.bases[0], (unsigned)g_fbd.bases[1],
             (unsigned)g_fbd.bases[2], (unsigned)g_fbd.bases[3],
             (unsigned)g_fbd.same_as_prev, (unsigned)g_fbd.sync_counts[0],
             (unsigned)g_fbd.sync_counts[1], (unsigned)g_fbd.sync_counts[2],
             g_dcache_clean_inv ? "resolved" : "MISSING",
             (unsigned)g_fbd.clean_ret, (unsigned)g_fbd.cleans,
             (unsigned)g_fbd.copy_fail);
    log_queue(line);
    snprintf(line, sizeof(line),
             "    menu drawn over by the game before the next frame: %u of %u "
             "frames (%u of %u pixels changed, checked up to %u ms after "
             "drawing); system layer 1 while in the game: %u calls (%u with "
             "a frame, last %08X fmt %X %ux%u pitch %u), game layer 0: %u "
             "calls; cache flush pieces %u: returned 0 x%u, 1 x%u, other x%u "
             "(last other 0x%08X)\r\n",
             (unsigned)g_fbd.chk_hit, (unsigned)g_fbd.chk_frames,
             (unsigned)g_fbd.chk_changed, (unsigned)g_fbd.chk_px,
             (unsigned)(g_fbd.chk_us_max / 1000), (unsigned)g_fbd.l1_calls,
             (unsigned)g_fbd.l1_set, (unsigned)g_fbd.l1_base,
             (unsigned)g_fbd.l1_fmt, (unsigned)g_fbd.l1_w,
             (unsigned)g_fbd.l1_h, (unsigned)g_fbd.l1_pitch,
             (unsigned)g_fbd.l0_calls, (unsigned)g_fbd.fl_pieces,
             (unsigned)g_fbd.fl_ret0, (unsigned)g_fbd.fl_ret1,
             (unsigned)g_fbd.fl_other, (unsigned)g_fbd.fl_other_val);
    log_queue(line);
    memset(&g_fbd, 0, sizeof(g_fbd));
  }
}

static int open_menu(void) {
  static int warned;
  perf_report();
  int had = g_overlay_ready;
  if (overlay_acquire() < 0) {
    /* like reVita's "Buy more RAM": refuse rather than take memory the
     * system needs; noted once per boot */
    if (!warned) {
      static char line[200];
      warned = 1;
      snprintf(line, sizeof(line), "menu not opened: not enough free kernel "
               "memory for the overlay (~1.6 MB) in any pool (root 0x%08X, "
               "root contiguous 0x%08X, user main 0x%08X)\r\n",
               (unsigned)g_ovl_alloc_ret[0], (unsigned)g_ovl_alloc_ret[1],
               (unsigned)g_ovl_alloc_ret[2]);
      log_queue(line);
    }
    return -1;
  }
  if (!had && g_ovl_alloc_type > 0) {
    static char line[200];
    snprintf(line, sizeof(line), "menu memory from the %s pool (root 0x%08X"
             "%s)\r\n", g_ovl_alloc_type == 1 ? "root contiguous" :
             "user main", (unsigned)g_ovl_alloc_ret[0],
             g_ovl_alloc_type == 2 ? ", root contiguous refused too" : "");
    log_queue(line);
  }
  g_capture_ok = 0;
  g_fb_rec = 0;
  g_swap_ox = 0; /* ✕ confirms, always (see handle_buttons) */
  g_swap_tid[0] = '\0';
  g_capture_req = 1;
  model_begin();
  g_model.mode = MV_BROWSE;
  g_model.can_load = 1;
  g_model.can_save = 0; /* until a game frame is captured */
  g_model.progress = -1;
  model_end();
  g_menu_open = 1;
  start_op(OP_REFRESH, 0);
  return 0;
}

static void close_menu(void) {
  if (g_menu_open)
    perf_report(); /* this menu session's numbers, frames included */
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

/* Menu combo: START + UP (SELECT + UP clashed with another plugin). Inside
 * the menu, SELECT opens Diagnostics, so that pressing START to close the
 * menu never triggers anything. */
#define JV_COMBO_BUTTON SCE_CTRL_START
#define JV_DIAG_BUTTON  SCE_CTRL_SELECT

static void handle_buttons(uint32_t held, uint32_t pressed) {
  int combo = (held & JV_COMBO_BUTTON) && (held & SCE_CTRL_UP) &&
              (pressed & (JV_COMBO_BUTTON | SCE_CTRL_UP));
  /* START + DOWN only closes the menu */
  int close_combo = (held & JV_COMBO_BUTTON) && (held & SCE_CTRL_DOWN) &&
                    (pressed & (JV_COMBO_BUTTON | SCE_CTRL_DOWN));

  if (!g_menu_open) {
    if (!combo)
      return;
    /* On firmware without verified offsets the menu still opens (preview
     * mode: browse, thumbnails, input), but save/load are refused. */
    if (g_busy || open_menu() < 0)
      return;
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

  /* ✕ always confirms and ○ goes back (user: "just default to x and not
   * even attempt at the remap" - following the game's region and the
   * remap plugins showed ○ first and then ✕ in a remapped Japanese game).
   * The detection below is kept but switched off. */
  if (0 && g_capture_ok &&
      strncmp(g_swap_tid, g_capture_tid, sizeof(g_swap_tid))) {
    strncpy(g_swap_tid, g_capture_tid, sizeof(g_swap_tid) - 1);
    /* instant, no file access: the game's region; the io thread then
     * checks PSV-Shell-Plus / reVita and corrects it if they swap */
    g_swap_ox = title_confirms_with_circle(g_swap_tid);
    ksceKernelLockMutex(g_logq_mtx, 1, NULL);
    strncpy(g_decide_tid, g_swap_tid, sizeof(g_decide_tid) - 1);
    ksceKernelUnlockMutex(g_logq_mtx, 1);
    if (g_io_evf >= 0)
      ksceKernelSetEventFlag(g_io_evf, IOEV_DECIDE);
  }
  if (g_swap_ox) {
    uint32_t ox = pressed & (SCE_CTRL_CROSS | SCE_CTRL_CIRCLE);
    if (ox == SCE_CTRL_CROSS || ox == SCE_CTRL_CIRCLE)
      pressed ^= SCE_CTRL_CROSS | SCE_CTRL_CIRCLE;
  }
  if (g_model.swap_ox != g_swap_ox) {
    model_begin();
    g_model.swap_ox = g_swap_ox;
    model_end();
  }

  ksceKernelLockMutex(g_mtx, 1, NULL);
  MenuMode mode = g_model.mode;
  int sel = g_model.selected;
  int used = g_model.slots[sel].used;
  int locked = g_model.slots[sel].locked;
  int outdated = g_model.slots[sel].outdated;
  int cfg_changed = g_model.slots[sel].config_changed;
  int prepared = g_model.slots[sel].prepared;
  int prepare_count = g_model.prepare_count;
  ksceKernelUnlockMutex(g_mtx, 1);

  if ((combo || close_combo) && mode != MV_BUSY) {
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
      } else if ((pressed & SCE_CTRL_SQUARE) && used && cfg_changed) {
        set_message(MV_MESSAGE, "Plugins (tai folder or config.txt) changed "
                    "since this snapshot - it can't be loaded", -1, 1);
      } else if ((pressed & SCE_CTRL_SQUARE) && used && outdated) {
        set_message(MV_MESSAGE, "The game was updated since this snapshot "
                    "- it can't be loaded", -1, 1);
      } else if ((pressed & SCE_CTRL_SQUARE) && used) {
        set_message(MV_CONFIRM_LOAD, "", -1, 0);
      } else if (pressed & JV_DIAG_BUTTON) {
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
      else if (pressed & SCE_CTRL_TRIANGLE) {
        /* PCH-2000 LCD: timing B (vic 0x20). A/B on hardware: timing A
         * (vic 0, libbaremetal's choice, the OLED timing) left garbage
         * along the screen edges, B is clean. L + triangle still runs A. */
        g_disp_vic = (held & SCE_CTRL_LTRIGGER) ? 0 : 0x20;
        start_op(OP_TEST_DISPLAY, sel);
      }
      else if (pressed & SCE_CTRL_CIRCLE)
        set_message(MV_BROWSE, "", -1, 0);
      break;
    case MV_CONFIRM_PREPARE:
      if (pressed & SCE_CTRL_CROSS) {
        int all = g_model.prepare_all;
        set_message(MV_BUSY, "Please wait...", -1, 0); /* before: no race */
        start_op(all ? OP_PREPARE_ALL : OP_PREPARE, sel);
      }
      else if (pressed & SCE_CTRL_CIRCLE)
        set_message(MV_BROWSE, "", -1, 0);
      break;
    case MV_CONFIRM_SAVE:
    case MV_CONFIRM_LOAD:
      if (pressed & SCE_CTRL_CROSS) {
        /* visible at once, even if the job waits for the slot refresh;
         * set before starting it, so the job's own messages win */
        set_message(MV_BUSY, "Please wait...", -1, 0);
        start_op(mode == MV_CONFIRM_SAVE ? OP_SAVE : OP_LOAD, sel);
      }
      else if (pressed & SCE_CTRL_CIRCLE)
        set_message(MV_BROWSE, "", -1, 0);
      break;
    case MV_MESSAGE:
      if (pressed & (SCE_CTRL_CIRCLE | SCE_CTRL_CROSS))
        set_message(MV_BROWSE, "", -1, 0);
      break;
    case MV_BUSY:
      if ((pressed & SCE_CTRL_CIRCLE) && g_model.can_cancel)
        g_cancel_req = 1;
      break;
  }
}

static int input_thread(SceSize args, void *argp) {
  uint32_t prev = 0xffffffff; /* require a release before the first press */
  SceCtrlData pad;

  while (1) {
    ksceKernelDelayThread(16 * 1000);
    uint32_t t0 = prof_now();

    memset(&pad, 0, sizeof(pad));
    if (ksceCtrlPeekBufferPositive(0, &pad, 1) < 0)
      continue;
    if (g_dolce) {
      /* PS TV: a DualShock may report on port 1 rather than 0 */
      SceCtrlData pad1;
      memset(&pad1, 0, sizeof(pad1));
      if (ksceCtrlPeekBufferPositive(1, &pad1, 1) >= 0)
        pad.buttons |= pad1.buttons;
    }

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

    if (g_ev_normal_ready && g_ev_r0 &&
        ksceKernelGetSystemTimeWide() - g_ev_r0 > 3000000) {
      /* a sleep JamaisVu did not request (power button): for comparison
       * with the request -> sleep time of a save; logged 3 s after the
       * wake-up began, whether or not the last resume event came */
      static char el[1536];
      g_ev_normal_ready = 0;
      format_events(el, sizeof(el), "normal sleep/wake, system events (S: "
                    "ms after the first suspend event, R: ms after the first "
                    "resume event):");
      log_queue(el);
    }

    if (g_lockskip_until && now_us() >= g_lockskip_until) {
      g_lockskip_until = 0;
      lockskip_release();
      log_queue("lock screen skip: off again (normal sleep locks)\r\n");
    }

    /* nothing shown and nothing running: give the overlay memory back */
    if (g_overlay_ready && !g_menu_open && !g_busy && !g_capture_req &&
        now_us() >= g_toast_until)
      overlay_release();

    uint32_t pressed = pad.buttons & ~prev;
    prev = pad.buttons;
    prof_add(PROF_INPUT, t0); /* the poll itself, not the menu actions */
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

/* Progress of a slot being prepared; ○ cancels (see handle_buttons). */
static int prepare_progress(int pct) {
  static char msg[96];
  snprintf(msg, sizeof(msg), "Preparing slot %d%s  %d%%",
           g_progress_slot + 1, g_progress_note, pct);
  model_begin();
  g_model.can_cancel = 1;
  model_end();
  set_message(MV_BUSY, msg, pct, 0);
  return g_cancel_req;
}

static void prepare_done(void) {
  model_begin();
  g_model.can_cancel = 0;
  model_end();
}

static int jf_read(void *ctx, uint32_t off, void *buf, uint32_t len) {
  SceUID fd = *(SceUID *)ctx;
  if (ksceIoLseek(fd, off, SCE_SEEK_SET) != (SceOff)off)
    return -1;
  return ksceIoRead(fd, buf, len);
}

/* characters of s that can be drawn: ASCII letters/digits, or in ext */
static int drawable_chars(const char *s, const ExtGlyphs *ext) {
  int n = 0;
  uint32_t cp;
  while ((cp = utf8_next(&s)) != 0) {
    if (cp < 0x80)
      n += (cp | 0x20) - 'a' < 26u || cp - '0' < 10u;
    else
      n += ext_glyph(ext, cp) != NULL;
  }
  return n;
}

/* The names the LiveArea shows (ur0:shell/db/app.db, opened read-only):
 * a game renamed by the user has its new name there. */
static int livearea_names(const char *const *ids, int n,
                          char names[][APPDB_NAME_LEN]) {
  SceUID fd = ksceIoOpen(APPDB_PATH, SCE_O_RDONLY, 0);
  if (fd < 0) {
    for (int i = 0; i < n; i++)
      names[i][0] = '\0';
    return -1;
  }
  int r = appdb_names(jf_read, &fd, ids, n, names);
  ksceIoClose(fd);
  return r;
}

/* Slot names: the LiveArea's name for the game (so a renamed game shows
 * its new name), else the game's param.sfo TITLE. A name in plain Latin
 * letters is shown as it is; one with Japanese (or other non-ASCII)
 * characters needs their glyphs from jpfont.bin. Without them - or for
 * games no longer installed - the slot keeps its stored name, as before.
 * Worker thread (file I/O). Returns the glyph set to draw with, or NULL. */
static const ExtGlyphs *resolve_names(MenuSlot *fresh) {
  static char names[JV_NUM_SLOTS][APPDB_NAME_LEN];
  static char ascii[sizeof(fresh[0].title)];
  static char path[96];
  static int warned;
  const char *ids[JV_NUM_SLOTS], *strs[JV_NUM_SLOTS];
  int has[JV_NUM_SLOTS], n = 0;

  for (int i = 0; i < JV_NUM_SLOTS; i++)
    ids[i] = fresh[i].used ? fresh[i].titleid : "";
  livearea_names(ids, JV_NUM_SLOTS, names);

  for (int i = 0; i < JV_NUM_SLOTS; i++) {
    has[i] = 0;
    if (!fresh[i].used)
      continue;
    if (!names[i][0] &&
        lookup_title_utf8(fresh[i].titleid, names[i], sizeof(names[i])) < 0)
      continue;
    for (const char *c = names[i]; *c; c++)
      if ((uint8_t)*c >= 0x80)
        has[i] = 1;
    if (!has[i]) {
      /* Latin letters only: no glyphs needed */
      if (title_to_ascii((const uint8_t *)names[i], strlen(names[i]), ascii,
                         sizeof(ascii)) == 0)
        memcpy(fresh[i].title, ascii, sizeof(fresh[i].title));
      continue;
    }
    strs[n++] = names[i];
  }
  if (n == 0 || !g_ext_set[0])
    return NULL;

  /* fill the set the model does not use; drawing that started with it
   * before the last switch has to finish first (normally long done) */
  int t = ext_index(g_model.ext) == 0 ? 1 : 0;
  ExtGlyphs *ext = g_ext_set[t];
  for (int i = 0; i < 3000 && g_ext_users[t]; i++)
    ksceKernelDelayThread(1000);
  if (g_ext_users[t])
    return (ExtGlyphs *)g_model.ext; /* keep what is shown */
  ext->n = 0;
  ext->used = 0;

  jv_file_path(path, sizeof(path), "jpfont.bin");
  SceUID fd = ksceIoOpen(path, SCE_O_RDONLY, 0);
  int r = -1;
  if (fd >= 0) {
    r = jpfont_load(jf_read, &fd, strs, n, ext);
    ksceIoClose(fd);
  }
  if (r < 0) {
    if (!warned) {
      warned = 1;
      log_queue("Japanese game names: data/JamaisVu/jpfont.bin missing or "
                "unreadable - names shown without those characters\r\n");
    }
    return NULL;
  }
  for (int i = 0; i < JV_NUM_SLOTS; i++)
    if (has[i] && drawable_chars(names[i], ext) >= 2) {
      memcpy(fresh[i].title, names[i], sizeof(fresh[i].title) - 1);
      fresh[i].title[sizeof(fresh[i].title) - 1] = '\0';
    }
  return ext;
}

static void refresh_slots(void) {
  static SlotInfo info;
  int exists[JV_NUM_SLOTS], locked[JV_NUM_SLOTS];

  const char *mc = storage_detect(); /* read-only */

  model_begin();
  g_model.savedata_rolls_back = mc && strcmp(mc, "ux0:") == 0;
  g_model.refreshing = 1; /* thumbnails still loading */
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

  /* current plugin configuration, read once for all slots */
  static uint8_t cfg_state[JV_CFG_FILES];
  static uint8_t cfg_md5[JV_CFG_FILES][16];
  static TaiPrint tai_now[JV_TAI_DIRS];
  static SlotEnv env;
  config_fingerprint(cfg_state, cfg_md5);
  tai_fingerprint(tai_now);

  int prepare_count = 0;
  for (int i = 0; i < JV_NUM_SLOTS; i++)
    if (!exists[i] && !locked[i])
      prepare_count++;
  model_begin();
  g_model.prepare_count = prepare_count;
  model_end();

  /* All file reading happens here, without the model lock: the input
   * thread takes that lock for every press, and in heavy games (Trails)
   * a param.sfo read could take seconds - the menu stopped responding. */
  static MenuSlot fresh[JV_NUM_SLOTS];
  for (int i = 0; i < JV_NUM_SLOTS; i++) {
    MenuSlot *s = &fresh[i];
    int ok = exists[i] && slot_info_read(i, &info, g_thumbs[i]) == 0;

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
      s->config_changed = !config_matches_fp(&info, cfg_state, cfg_md5, NULL) ||
                          (slot_env_read(i, info.crc, &env) == 0 &&
                           !tai_matches(env.tai, tai_now, NULL));
    }
  }

  const ExtGlyphs *ext = resolve_names(fresh);

  /* then one short update under the lock */
  model_begin();
  memcpy(g_model.slots, fresh, sizeof(fresh));
  g_model.ext = ext;
  g_model.refreshing = 0;
  model_end();
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
static void log_append_locked(const char *text);

/* worker and io thread only (never the input thread) */
static int g_log_trace_once;

static void log_append(const char *text) {
  int locked = 0;
  if (g_log_mtx >= 0) {
    if (g_log_trace_once) {
      /* first write after a wake-up: does another thread of the restored
       * image hold the log? (a4ce776/b879a47 stopped before the card was
       * touched) - at most 2 s, then on without it */
      SceUInt32 to = 2000000;
      locked = ksceKernelLockMutex(g_log_mtx, 1, &to) >= 0;
      trace_wake(locked ? JV_WAKE_W_LOCK : JV_WAKE_W_LOCK_TIMEOUT);
    } else {
      locked = ksceKernelLockMutex(g_log_mtx, 1, NULL) >= 0;
    }
  }
  log_append_locked(text);
  if (locked)
    ksceKernelUnlockMutex(g_log_mtx, 1);
}

/* lines that could not be written because the card was not mounted
 * (usbmc's uma0 drops out after a wake-up): kept, with their time, and
 * written ahead of the next line that can be */
static char g_log_backlog[4096];
static int g_log_backlog_len;

static void log_backlog_add(const char *stamp, int n, const char *text) {
  int tl = (int)strlen(text);
  if (n + tl > (int)sizeof(g_log_backlog) - g_log_backlog_len)
    return; /* full: the newest lines are dropped */
  memcpy(g_log_backlog + g_log_backlog_len, stamp, n);
  memcpy(g_log_backlog + g_log_backlog_len + n, text, tl);
  g_log_backlog_len += n + tl;
}

/* the first log write after a wake-up records each file call in
 * trace.bin (both consoles with the card on uma0 stopped there after a
 * load, a4ce776: card-ready, then nothing); g_log_trace_once above */

static void log_append_locked(const char *text) {
  static char path[64], line[160];
  SceDateTime t;
  SceIoStat st;
  int tr = g_log_trace_once;
  g_log_trace_once = 0;

  memset(&t, 0, sizeof(t));
  ksceRtcGetCurrentClockLocalTime(&t);
  if (tr)
    trace_wake(JV_WAKE_W_TIME);
  int n = snprintf(line, sizeof(line), "%04d-%02d-%02d %02d:%02d:%02d ",
                   t.year, t.month, t.day, t.hour, t.minute, t.second);

  if (!storage_mc_root() || slots_mkdirs() < 0) {
    log_backlog_add(line, n, text);
    return;
  }
  if (tr)
    trace_wake(JV_WAKE_W_MKDIR);
  jv_file_path(path, sizeof(path), "log.txt");

  int flags = SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND;
  if (ksceIoGetstat(path, &st) >= 0 && st.st_size > 64 * 1024)
    flags = SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC;
  if (tr)
    trace_wake(JV_WAKE_W_STAT);

  SceUID fd = ksceIoOpen(path, flags, 0666);
  if (fd < 0) {
    log_backlog_add(line, n, text);
    return;
  }
  if (tr)
    trace_wake(JV_WAKE_W_OPEN);
  if (g_log_backlog_len) {
    ksceIoWrite(fd, g_log_backlog, g_log_backlog_len);
    g_log_backlog_len = 0;
  }
  ksceIoWrite(fd, line, n);
  ksceIoWrite(fd, text, strlen(text));
  if (tr)
    trace_wake(JV_WAKE_W_WRITE);
  ksceIoClose(fd);
  if (tr)
    trace_wake(JV_WAKE_W_CLOSE);
  slots_sync();
  if (tr)
    trace_wake(JV_WAKE_W_SYNC);
}

static int g_trace_keep;

static void log_result(const char *msg) {
  static char text[1200];
  static JvTraceSector t;

  trace_wake(JV_WAKE_REPORT);
  int wret = 0, wn = trace_wake_end(&wret);
  trace_from_args(&t, &g_res_pa);
  int n = trace_format(text, sizeof(text), &t);
  n += snprintf(text + n, sizeof(text) - n, "    message: %s\r\n", msg);
  if (wn >= 0 && n < (int)sizeof(text))
    snprintf(text + n, sizeof(text) - n, "    wake-up steps written to "
             "trace.bin: %d (last result 0x%08X)\r\n", wn, (unsigned)wret);
  log_append(text);
  trace_wake(JV_WAKE_REPORT_LOGGED);
  /* after a load the record stays open a little longer (run_and_wait):
   * the PCH-1000 (91bd42b) restarted after "report", when the 32 steps
   * were full */
  if (!g_trace_keep)
    trace_consumed(); /* already logged from the arguments */
}

static void report(const char *msg, int is_error) {
  toast(msg, is_error);
  log_result(msg);
}

/* After a wake-up the card may be unmounted for a moment (usbmc: uma0 is
 * dropped and not mounted again). Waits until it is usable again, mounting
 * it back itself when needed (the io thread's watch is off while a
 * save/load runs). 1 = usable. */
static int card_wait(int ms) {
  for (int waited = 0; waited <= ms; waited += 100) {
    const char *root = NULL;
    if (storage_card_unmounted(&root))
      storage_remount_card(&root);
    const char *mc = storage_detect();
    SceIoStat st;
    if (mc && ksceIoGetstat(mc, &st) >= 0)
      return 1;
    ksceKernelDelayThread(100 * 1000);
  }
  return 0;
}

static int g_card_remount_ret = 1;
static const char *g_card_remount_root;

static void after_resume(void) {
  static char msg[128];

  trace_wake_io(1);
  trace_wake(JV_WAKE_WORKER);

  /* put the clocks back to the snapshot's (or, after a save, to what they
   * were before) and record it */
  {
    static char clk[192], line[224];
    jv_clocks_restore(clk, sizeof(clk));
    trace_wake(JV_WAKE_CLOCKS_SET);
    /* After a load with the card on uma0/xmc0 (usbmc/StorageMgr): the
     * restored image's filesystem there no longer works with the card the
     * payload restarted - the first file call hung (98c6bf6: w-time, then
     * nothing; the PCH-2000 with the card on ux0 loads fine). Mount it
     * afresh before anything touches it. */
    if (g_res_debug == JV_TEST_NONE && g_res_mode == JAMAISVU_MODE_LOAD &&
        g_res_result == JV_OK) {
      const char *root = NULL;
      trace_wake(JV_WAKE_CARD_REMOUNT);
      g_card_remount_ret = storage_force_remount_card(&root);
      g_card_remount_root = root;
      trace_wake(JV_WAKE_CARD_REMOUNTED);
    }
    /* the card may be away for a moment after the wake-up (usbmc's uma0):
     * no file access until it is usable again (at most 10 s) */
    if (card_wait(10000))
      trace_wake(JV_WAKE_CARD_READY);
    snprintf(line, sizeof(line), "    %s\r\n", clk);
    g_log_trace_once = 1;
    log_append(line);
    if (g_card_remount_root) {
      snprintf(line, sizeof(line), "    after the load: Memory Card on %s "
               "mounted afresh, result 0x%08X\r\n", g_card_remount_root,
               (unsigned)g_card_remount_ret);
      log_append(line);
      g_card_remount_root = NULL;
    }
    trace_wake(JV_WAKE_CLOCKS);
  }

  if (g_res_debug != JV_TEST_NONE) {
    /* display test through all its steps, no hardware wait gave up (the
     * payload reports their count as detail): from now on saves and loads
     * show the progress screen on this console (OLED / PS TV) */
    if (g_res_debug == JV_TEST_DISPLAY && g_res_result == JV_TEST_DONE &&
        g_res_detail == 0)
      display_ok_record(g_res_pa.disp_type);
    /* PS TV: our HDMI registers during the test picture (payload copy) */
    if (g_res_debug == JV_TEST_DISPLAY && g_regsnap_ok) {
      g_regsnap_ok = 0;
      hwdump_regs(log_append, "ours", g_regsnap);
    }
    /* diagnostics: report and never touch the slot info */
    if (g_res_result == JV_TEST_DONE)
      snprintf(msg, sizeof(msg), "%s passed - reached stage %u (%s)",
               g_res_debug == JV_TEST_RESET ? "Reset test" :
               g_res_debug == JV_TEST_DISPLAY ? "Display test" : "Dry run",
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
      trace_wake(JV_WAKE_SLOT_INFO);
      int r = slot_info_write(g_res_slot, &g_pending_info, g_thumbs[PENDING]);
      if (r != 0) {
        /* PS TV (8fdbce8): uma0 dropped out right after the wake-up, the
         * slot info could not be written ("save failed") and the card was
         * back 5 s later - wait for it and try once more */
        static char rl[128];
        int back = card_wait(10000);
        int r1 = r;
        if (back)
          r = slot_info_write(g_res_slot, &g_pending_info, g_thumbs[PENDING]);
        snprintf(rl, sizeof(rl), "    slot info: first write %d, card %s, "
                 "second write %d\r\n", r1, back ? "back" : "still missing",
                 back ? r : r1);
        log_append(rl);
      }
      trace_wake(JV_WAKE_SLOT_INFO_DONE);
      if (r == 0) {
        g_pending_env.info_crc = g_pending_info.crc;
        slot_env_write(g_res_slot, &g_pending_env);
      }
      if (r == 0 && g_save_frame.valid &&
          slot_frame_write(g_res_slot, g_pending_info.crc, g_save_frame.pa,
                           g_save_frame.pitch, g_save_frame.w,
                           g_save_frame.h) == 0) {
        static char fl[96];
        snprintf(fl, sizeof(fl), "    slot frame recorded: 0x%08X pitch %u\r\n",
                 (unsigned)g_save_frame.pa, (unsigned)g_save_frame.pitch);
        log_append(fl);
      }
      g_save_frame.valid = 0;
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
      else if (g_res_pa.appdb_changed == 2)
        snprintf(msg, sizeof(msg), "Loaded slot %u - apps changed since this "
                 "save: restart if the LiveArea looks out of date",
                 (unsigned)g_res_slot + 1);
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
           debug_mode == JV_TEST_DRYRUN ? " (dry run)" :
           debug_mode == JV_TEST_DISPLAY ? " (display test)" : "", slot + 1,
           jv_build_id(), variant == JV_PAYLOAD_DEFAULT ? "default" : "?",
           why);
  log_append(start);
  {
    static char clk[96], line[128];
    jv_clocks_remember(clk, sizeof(clk));
    snprintf(line, sizeof(line), "    clocks before: %s\r\n", clk);
    log_append(line);
  }
  /* PS TV: the system's own HDMI setup while its picture is on screen
   * (read only), to compare with the progress screen's */
  if (jv_disp_type() == JV_DISP_HDMI &&
      (debug_mode == JV_TEST_DISPLAY || debug_mode == JV_TEST_NONE))
    hwdump_hdmi(log_append);

  /* no lock screen on the way back (both the save and the load path:
   * the snapshot is taken with this in place, too) */
  {
    static char ls[128], line[160];
    /* a release still pending from the last wake-up (10 s) would remove
     * this patch again before the sleep: seen at 21:58:49, one second
     * after a load started within 10 s of the save before it */
    g_lockskip_until = 0;
    lockskip_apply(ls, sizeof(ls));
    snprintf(line, sizeof(line), "    %s\r\n", ls);
    log_append(line);
  }

  if (jv_request(mode, slot, nonce, debug_mode, trel,
                 tok ? tnonce : NULL, variant) < 0) {
    lockskip_release();
    toast("Could not start - the system refused to reset", 1);
    return;
  }

  /* the first frame is watched for from the wake-up on (menu_on_resumed):
   * armed here it caught a frame from before the sleep (-4028 ms) */
  g_t_first_frame = 0;
  g_wait_first_frame = 0;
  g_t_first_any = 0;
  g_wait_first_any = 0;
  unsigned int out;
  ksceKernelWaitEventFlag(g_evf, EVF_RESUMED,
                          SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR_PAT,
                          &out, NULL);

  /* the shell has decided on the lock screen by then; afterwards normal
   * sleep/wake locks again (removed by the input thread) */
  g_lockskip_until = now_us() + 10000000;

  /* NOTE: after a successful load, this point is reached by the worker of
   * the *restored* image, which was waiting here when the snapshot was
   * taken. after_resume() looks only at the payload's result. */
  /* loads: the step record stays open until 10 s after the timing lines */
  g_trace_keep = g_res_debug == JV_TEST_NONE && g_res_mode == JAMAISVU_MODE_LOAD;
  trace_wake_keep(g_trace_keep);
  after_resume();
  int64_t t_after = ksceKernelGetSystemTimeWide();

  /* Timing, after everything else (clocks, result, slot info): how long
   * the Vita took to wake and show the first frame (up to 10 s). After a
   * load this is the snapshot's JamaisVu: "request -> sleep" would be the
   * save's, so only the wake-up is logged. */
  for (int i = 0; i < 1000 && g_wait_first_frame; i++)
    ksceKernelDelayThread(10000);
  g_wait_first_frame = 0;
  g_wait_first_any = 0;
  trace_wake(JV_WAKE_FIRST_FRAME);
  {
    static char tl[768];
    int64_t now = ksceKernelGetSystemTimeWide();
    int n = 0;
    if (g_res_mode == JAMAISVU_MODE_SAVE)
      n = snprintf(tl, sizeof(tl), "    timing: request -> sleep %d ms;",
                   (int)((g_t_suspend - g_t_request) / 1000));
    else
      n = snprintf(tl, sizeof(tl), "    timing:");
    if (g_res_pa.delay_unit_ns)
      n += snprintf(tl + n, sizeof(tl) - n, " payload I-cache %s, delay "
                    "unit %u us;", g_res_pa.icache_on ? "on" : "off",
                    (unsigned)(g_res_pa.delay_unit_ns / 1000));
    if (g_res_pa.mem_bytes && g_res_pa.mem_ms)
      n += snprintf(tl + n, sizeof(tl) - n, " memory %u MB in %u ms (%u MB/s);",
                    (unsigned)(g_res_pa.mem_bytes >> 20),
                    (unsigned)g_res_pa.mem_ms,
                    (unsigned)((uint64_t)(g_res_pa.mem_bytes >> 20) * 1000 /
                               g_res_pa.mem_ms));
    snprintf(tl + n, sizeof(tl) - n, " wake -> system resumed %d ms; -> "
             "first frame %s%d ms\r\n",
             (int)((g_t_resumed - g_t_resume) / 1000),
             g_t_first_frame ? "" : "none within 10 s, waited ",
             (int)(((g_t_first_frame ? g_t_first_frame : now) - g_t_resume)
                   / 1000));
    log_append(tl);
    if (g_t_first_any)
      snprintf(tl, sizeof(tl), "    timing after wake: first frame on any "
               "screen %d ms (head %d index %d, game %d); JamaisVu's own "
               "work after the wake-up done at %d ms\r\n",
               (int)((g_t_first_any - g_t_resume) / 1000), g_first_any_head,
               g_first_any_index, ksceAppMgrIsExclusiveProcessRunning() ? 1 : 0,
               (int)((t_after - g_t_resume) / 1000));
    else
      snprintf(tl, sizeof(tl), "    timing after wake: no frame on any "
               "screen; JamaisVu's own work after the wake-up done at %d ms"
               "\r\n", (int)((t_after - g_t_resume) / 1000));
    log_append(tl);
    if (g_res_mode == JAMAISVU_MODE_SAVE && g_ev_n) {
      /* After a load the restored image holds its own save's events: not
       * shown. */
      static char el[1536];
      format_events(el, sizeof(el), "    system events (S: ms after the "
                    "request, R: ms after the first resume event):");
      log_append(el);
    }
    if (g_t_fs_done) {
      const FsUpdateTiming *f = &g_fs_update_timing;
      snprintf(tl, sizeof(tl), "    timing ux0 re-read after the load: "
               "%d -> %d ms after the wake-up; buffer caches %u ms, file "
               "caches %u ms, remount %u ms, open files %u ms (%u vnodes), "
               "file entries %u ms (%u looked up, %u already done)\r\n",
               (int)((g_t_resumed - g_t_resume) / 1000),
               (int)((g_t_fs_done - g_t_resume) / 1000),
               (unsigned)f->ms[0], (unsigned)f->ms[1], (unsigned)f->ms[2],
               (unsigned)f->ms[3], (unsigned)f->vnodes, (unsigned)f->ms[4],
               (unsigned)f->fnodes, (unsigned)f->fnodes_skipped);
      log_append(tl);
    }
  }
  trace_wake(JV_WAKE_TIMING);
  if (g_trace_keep) {
    for (int i = 0; i < 10; i++) {
      ksceKernelDelayThread(1000 * 1000);
      trace_wake(JV_WAKE_ALIVE);
    }
    g_trace_keep = 0;
    trace_wake_keep(0);
    trace_consumed();
  }
}

static int display_frame_check(char *why, int len, int for_test);

/* The frame on screen right now (the display hook records the next one;
 * the menu is open, so frames keep coming). Up to 0.3 s. */
static int fresh_frame(void) {
  g_fb_rec = 0;
  __sync_synchronize();
  for (int i = 0; i < 30 && !g_fb_rec; i++)
    ksceKernelDelayThread(10000);
  return g_fb_rec;
}

/* Display memory (CDRAM, 0x20000000-0x27FFFFFF): the only place a load may
 * draw into the snapshot's own frame */
static int frame_in_cdram(uint32_t pa, uint32_t pitch, uint32_t h) {
  return pa >= 0x20000000u && (uint64_t)pa + (uint64_t)pitch * h * 4 <=
                                  0x28000000u;
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
  prepare_done();
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
  /* stored with the slot (shown if the game is uninstalled later): the
   * LiveArea's name if it has Latin letters (e.g. renamed to English),
   * else the game's own */
  {
    static char la[1][APPDB_NAME_LEN];
    const char *one[1] = { g_capture_tid };
    if (livearea_names(one, 1, la) <= 0 ||
        title_to_ascii((const uint8_t *)la[0], strlen(la[0]),
                       g_pending_info.title,
                       sizeof(g_pending_info.title)) < 0)
      lookup_title(g_capture_tid, g_pending_info.title,
                   sizeof(g_pending_info.title));
  }
  g_pending_info.has_thumb = 1;

  /* the snapshot is only valid for this exact game version */
  if (lookup_app_ver(g_capture_tid, g_pending_info.app_ver,
                     sizeof(g_pending_info.app_ver)) < 0) {
    set_message(MV_MESSAGE, "Cannot save: the game's version could not be "
                "read", -1, 1);
    return;
  }

  /* the plugin configuration the snapshot is taken with */
  config_fingerprint(g_pending_info.cfg_state, g_pending_info.cfg_md5);
  for (int i = 0; i < JV_CFG_FILES; i++) {
    if (g_pending_info.cfg_state[i] == JV_CFG_UNREADABLE) {
      static char err[96];
      snprintf(err, sizeof(err), "Cannot save: %s could not be read",
               jv_cfg_paths[i]);
      set_message(MV_MESSAGE, err, -1, 1);
      return;
    }
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

  /* the plugin folders and the LiveArea's app database now: recorded
   * with the slot (slotN.jve) once the save succeeded, compared when it is
   * loaded (see slots.h, SlotEnv) */
  {
    static char line[224];
    int64_t t0 = ksceKernelGetSystemTimeWide();
    memset(&g_pending_env, 0, sizeof(g_pending_env));
    tai_fingerprint(g_pending_env.tai);
    int64_t t1 = ksceKernelGetSystemTimeWide();
    g_pending_env.appdb_state = (uint32_t)appdb_fingerprint(
        g_pending_env.appdb_md5, &g_pending_env.appdb_size);
    int64_t t2 = ksceKernelGetSystemTimeWide();
    snprintf(line, sizeof(line), "    environment: ux0:tai %u files %u KB, "
             "ur0:tai %u files %u KB (%d ms); app.db %u KB (%d ms)\r\n",
             (unsigned)g_pending_env.tai[0].files,
             (unsigned)(g_pending_env.tai[0].bytes >> 10),
             (unsigned)g_pending_env.tai[1].files,
             (unsigned)(g_pending_env.tai[1].bytes >> 10),
             (int)((t1 - t0) / 1000), (unsigned)(g_pending_env.appdb_size >> 10),
             (int)((t2 - t1) / 1000));
    log_append(line);
    jv_set_appdb_changed(0);
  }

  /* progress screen during the save (PCH-2000): the game's frame as it is
   * now, if it passes the same checks as the display test; otherwise no
   * screen. Remembered for slotN.jvf (loads draw there). */
  {
    static char why[160];
    g_save_frame.valid = 0;
    fresh_frame();
    if (display_frame_check(why, sizeof(why), 0)) {
      jv_set_display_frame(g_fb_pa, g_fb_pitch, g_fb_w, g_fb_h, 0x20);
      g_save_frame.valid = frame_in_cdram(g_fb_pa, g_fb_pitch, g_fb_h);
      g_save_frame.pa = g_fb_pa;
      g_save_frame.pitch = g_fb_pitch;
      g_save_frame.w = g_fb_w;
      g_save_frame.h = g_fb_h;
    } else {
      jv_set_display_frame(0, 0, 0, 0, 0);
    }
  }
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

  /* only with the plugin configuration the snapshot was taken with: the
   * config.txt files (every slot) and, for slots that recorded them, every
   * plugin file in ux0:tai and ur0:tai (slotN.jve) */
  static SlotEnv env;
  int have_env = slot_env_read(slot, info.crc, &env) == 0;
  {
    static TaiPrint now[JV_TAI_DIRS];
    int which = 0;
    if (!config_matches(&info, &which)) {
      snprintf(msg, sizeof(msg), "%s changed since this snapshot - it "
               "can't be loaded", jv_cfg_paths[which]);
      set_message(MV_MESSAGE, msg, -1, 1);
      return;
    }
    if (have_env) {
      tai_fingerprint(now);
      if (!tai_matches(env.tai, now, &which)) {
        static char line[160];
        snprintf(msg, sizeof(msg), "Plugins in %s changed since this "
                 "snapshot - it can't be loaded", jv_tai_dirs[which]);
        snprintf(line, sizeof(line), "load refused: %s %u files %u bytes "
                 "now, %u files %u bytes at the save\r\n", jv_tai_dirs[which],
                 (unsigned)now[which].files, (unsigned)now[which].bytes,
                 (unsigned)env.tai[which].files,
                 (unsigned)env.tai[which].bytes);
        log_append(line);
        set_message(MV_MESSAGE, msg, -1, 1);
        return;
      }
    }
  }

  snprintf(msg, sizeof(msg), "Loading slot %d...", slot + 1);
  set_message(MV_BUSY, msg, -1, 0);
  slots_sync();

  /* the LiveArea's app database: changed since the save? (a warning after
   * the load, not a block - see slots.h) */
  {
    static char line[128];
    uint8_t md5[16];
    uint32_t size = 0, v = 0;
    const char *what = "no record (older slot)";
    if (have_env && env.appdb_state == JV_CFG_PRESENT) {
      int st = appdb_fingerprint(md5, &size);
      v = (st == JV_CFG_PRESENT && !memcmp(md5, env.appdb_md5, 16)) ? 1 : 2;
      what = v == 1 ? "unchanged since the save" : "CHANGED since the save";
    }
    jv_set_appdb_changed(v);
    snprintf(line, sizeof(line), "    app.db: %s (%u bytes now, %u then); "
             "plugin folders: %s\r\n", what, (unsigned)size,
             (unsigned)(have_env ? env.appdb_size : 0),
             have_env ? "unchanged" : "no record (older slot)");
    log_append(line);
  }

  /* progress screen: the frame on screen now (game or LiveArea) until the
   * memory restore starts, then the snapshot's own frame from slotN.jvf
   * (none for older slots: the screen then goes dark while restoring) */
  {
    static char why[160], line[160];
    static SlotFrame sf;
    fresh_frame();
    if (display_frame_check(why, sizeof(why), 0)) {
      jv_set_display_frame(g_fb_pa, g_fb_pitch, g_fb_w, g_fb_h, 0x20);
      if (slot_frame_read(slot, info.crc, &sf) == 0 && sf.width == 960 &&
          sf.height == 544 && sf.pitch >= 960 && sf.pitch <= 1024 &&
          !(sf.paddr & 0xff) && frame_in_cdram(sf.paddr, sf.pitch, 544)) {
        jv_set_display_frame2(sf.paddr, sf.pitch);
        snprintf(line, sizeof(line), "    slot frame: 0x%08X pitch %u\r\n",
                 (unsigned)sf.paddr, (unsigned)sf.pitch);
      } else {
        jv_set_display_frame2(0, 0);
        snprintf(line, sizeof(line), "    slot frame: none (older slot) - "
                 "screen dark while restoring\r\n");
      }
      log_append(line);
    } else {
      jv_set_display_frame(0, 0, 0, 0, 0);
      jv_set_display_frame2(0, 0);
    }
  }
  run_and_wait(JAMAISVU_MODE_LOAD, slot, NULL, JV_TEST_NONE,
               JV_PAYLOAD_DEFAULT);
}

/* status.txt lives on the Memory Card like everything else JamaisVu
 * writes; it is only rewritten when its content changed. */
/* Diagnostics. Neither test writes anything: the reset test returns from
 * the payload before touching any hardware, the dry run stops right before
 * the first write (the slot file is only created/stamped, like before a
 * save, and the selected slot must be empty). */
/* Display test / progress screen: is the recorded frame usable? Logs the
 * details either way; why = the reason if not. */
/* for_test: the Diagnostics display test itself (any display); otherwise
 * a save/load/dry run, which shows the progress screen on the OLED or the
 * PS TV only after that test has passed on this console. */
static int display_frame_check(char *why, int len, int for_test) {
  uint32_t hw = jv_hw_info(), type = jv_disp_type();
  /* the frame the payload draws into: the game's 960x544 frame; on the
   * PS TV (HDMI) also a larger one (home screen), as long as it holds the
   * 960x544 screen */
  int size_ok = type == JV_DISP_HDMI
      ? g_fb_w <= 1920 && g_fb_h <= 1088 && g_fb_pitch >= g_fb_w &&
        g_fb_pitch <= 2048 && g_fb_pitch * g_fb_h >= 960u * 544
      : g_fb_w == 960 && g_fb_h == 544 && g_fb_pitch >= 960 &&
        g_fb_pitch <= 1024;
  why[0] = '\0';
  if (!for_test && !display_ok(type))
    snprintf(why, len, "Progress screen off until Diagnostics > Display test "
             "has passed on this console (%s)", disp_name(type));
  else if (!g_fb_rec)
    snprintf(why, len, "Display test: no frame seen yet - try again");
  else if (!size_ok)
    snprintf(why, len, "Display test: frame %ux%u pitch %u is not supported "
             "yet (%s)", (unsigned)g_fb_w, (unsigned)g_fb_h,
             (unsigned)g_fb_pitch, disp_name(type));
  else if (g_fb_pa0 == 0)
    snprintf(why, len, "Display test: frame address 0x%08X could not be "
             "translated", (unsigned)g_fb_va);
  else if (g_fb_pa == 0)
    snprintf(why, len, "Display test: frame not in one piece of memory (PA "
             "0x%08X, breaks at +0x%X)", (unsigned)g_fb_pa0,
             (unsigned)g_fb_bad_off);
  else if (g_fb_pa < 0x20000000u || g_fb_pa >= 0x60000000u ||
           (g_fb_pa & 0xff)) /* 256 bytes: Dead Nation's frames start at
                                 * e.g. 0x2000A700, inside one block */
    snprintf(why, len, "Display test: frame at an unexpected address (PA "
             "0x%08X)", (unsigned)g_fb_pa);

  static char line[256];
  if (g_fb_rec) {
    snprintf(line, sizeof(line), "display: frame VA 0x%08X: CPU priv PAR "
             "0x%08X, CPU user PAR 0x%08X, VAtoPA %s 0x%08X/0x%08X, BySW "
             "%s 0x%08X/0x%08X, used method %d\r\n", (unsigned)g_fb_va,
             (unsigned)g_fb_par_priv, (unsigned)g_fb_par_user,
             g_va2pa_hw ? "ret" : "missing", (unsigned)g_fb_hw_ret,
             (unsigned)g_fb_hw_pa, g_va2pa_sw ? "ret" : "missing",
             (unsigned)g_fb_sw_ret, (unsigned)g_fb_sw_pa, g_fb_method);
    log_append(line);
  }
  if (why[0]) {
    snprintf(line, sizeof(line), "%s\r\n", why); /* one log line */
    log_append(line);
    return 0;
  }
  snprintf(line, sizeof(line), "display: frame at 0x%08X, %ux%u pitch %u "
           "(%s), hardware 0x%08X, display %s, primary head %d\r\n",
           (unsigned)g_fb_pa, (unsigned)g_fb_w, (unsigned)g_fb_h,
           (unsigned)g_fb_pitch, g_fb_game ? "game" : "LiveArea",
           (unsigned)hw, disp_name(type), ksceDisplayGetPrimaryHead());
  log_append(line);
  return 1;
}

/* kind: 0 reset test, 1 dry run, 2 display test */
static void do_test(int kind, int slot, int variant) {
  static uint8_t nonce[16];
  int dry = kind == 1;

  if (!jv_armed()) {
    set_message(MV_MESSAGE, "Tests need an armed build", -1, 1);
    return;
  }
  storage_detect();
  if (storage_problem()) {
    set_message(MV_MESSAGE, storage_problem(), -1, 1);
    return;
  }

  /* Display test and dry run: the frame to draw into. The display test
   * refuses without one; a dry run then just runs without its progress
   * screen, as before. */
  if (kind == 2 || kind == 1) {
    static char why[160];
    if (display_frame_check(why, sizeof(why), kind == 2)) {
      jv_set_display_frame(g_fb_pa, g_fb_pitch, g_fb_w, g_fb_h,
                           kind == 2 ? g_disp_vic : 0x20);
    } else {
      jv_set_display_frame(0, 0, 0, 0, 0);
      if (kind == 2) {
        set_message(MV_MESSAGE, why, -1, 1);
        return;
      }
    }
  }

  if (dry) {
    if (slot_is_loadable(slot)) {
      set_message(MV_MESSAGE, "Dry run: select an empty slot", -1, 1);
      return;
    }
    slots_mkdirs();
    g_progress_slot = slot;
    int res = slot_prepare(slot, prepare_progress);
    prepare_done();
    if (res == 0)
      res = slot_stamp(slot, nonce);
    if (res < 0) {
      static char err[128];
      snprintf(err, sizeof(err), "Cannot test: %s", result_text(res));
      set_message(MV_MESSAGE, err, -1, 1);
      return;
    }
  }

  set_message(MV_BUSY, kind == 2 ? "Display test..." :
              dry ? "Dry run..." : "Reset test...", -1, 0);
  slots_sync();
  run_and_wait(JAMAISVU_MODE_SAVE, slot, dry ? nonce : NULL,
               kind == 2 ? JV_TEST_DISPLAY :
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
    prepare_done();
    if (res == JV_ERR_SLOT_CANCELLED) {
      g_progress_note = " (first use only)";
      if (done > 0)
        snprintf(msg, sizeof(msg), "Cancelled - %d slot%s already prepared "
                 "%s kept", done, done == 1 ? "" : "s",
                 done == 1 ? "is" : "are");
      else
        snprintf(msg, sizeof(msg), "Cancelled");
      set_message(MV_MESSAGE, msg, -1, 0);
      return;
    }
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
      case OP_TEST_DISPLAY:
        do_test(2, g_op_slot, JV_PAYLOAD_DEFAULT);
        break;
        break;
      case OP_PREPARE:
        do_prepare(0, g_op_slot);
        break;
      case OP_PREPARE_ALL:
        do_prepare(1, g_op_slot);
        break;
    }

    /* after a job that changed slots (not after a refresh itself) */
    if (g_menu_open && g_op != OP_REFRESH)
      refresh_slots();

    ksceKernelLockMutex(g_mtx, 1, NULL);
    if (g_next_op != OP_NONE) {
      /* a press queued while this job ran: run it now, still busy */
      g_op = g_next_op;
      g_op_slot = g_next_slot;
      g_next_op = OP_NONE;
      g_cancel_req = 0;
      ksceKernelUnlockMutex(g_mtx, 1);
      ksceKernelSetEventFlag(g_evf, EVF_WORK);
      continue;
    }
    g_op = OP_NONE;
    g_busy = 0;
    ksceKernelUnlockMutex(g_mtx, 1);
  }
  return 0;
}

/* ------------------------------------------------------------------------ */

/* Timing: watch for the first frame from the wake-up event on. Armed in
 * menu_on_resumed it only started after the remount of a load, so frames
 * during the remount could not have been seen. */
void menu_arm_first_frame(void) {
  g_t_first_frame = 0;
  g_t_first_any = 0;
  g_wait_first_frame = 1;
  g_wait_first_any = 1;
}

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

/* Who mounts and unmounts ux0/uma0/xmc0 (observation only, the calls go
 * through unchanged): usbmc's uma0 card mount is dropped after wake-ups
 * and screenshots; the caller's module and process, and the time since
 * the last wake-up, are logged. */
static tai_hook_ref_t g_umount_ref, g_mount_ref;
static SceUID g_umount_hook = -1, g_mount_hook = -1;
static int (*g_mod_by_addr)(SceUID pid, const void *addr, uint32_t *nid,
                            const void **text, void *name);

/* the hooks only record (no lookups or locks in the system's own mount
 * calls); the io thread formats the events */
#define MOUNT_EV_N 16
static struct {
  uint8_t unmount;
  int id, arg, ret, ms;
  uintptr_t lr;
  SceUID pid;
} g_mount_ev[MOUNT_EV_N];
static volatile uint32_t g_mount_ev_w, g_mount_ev_r;

/* SceSdstor's code: after a wake-up it force-unmounts mount 0xF00, the
 * system's USB storage mount - with usbmc that is the official card, and
 * a load's filesystem refresh crashed (PS TV) or froze (PCH-1000) walking
 * its files while they were freed (66481a6: unmount at 855 ms, refresh of
 * open files from 897 ms, never finished). Those unmounts are refused
 * while 0xF00/0xE00 is the card: the card was never removed. */
static uintptr_t g_sdstor_lo, g_sdstor_hi;
static volatile int g_card_on[2]; /* 0xF00, 0xE00 are the card (cached) */

static void mount_card_flags_update(void) {
  g_card_on[0] = storage_mount_is_card(0xF00);
  g_card_on[1] = storage_mount_is_card(0xE00);
}

static void mount_note(int unmount, int id, int arg, int ret, uintptr_t lr) {
  if (id != 0x800 && id != 0xF00 && id != 0xE00)
    return;
  uint32_t w = __sync_fetch_and_add(&g_mount_ev_w, 1);
  if (w - g_mount_ev_r >= MOUNT_EV_N)
    return; /* full */
  int64_t now = ksceKernelGetSystemTimeWide();
  int k = w % MOUNT_EV_N;
  g_mount_ev[k].unmount = (uint8_t)unmount;
  g_mount_ev[k].id = id;
  g_mount_ev[k].arg = arg;
  g_mount_ev[k].ret = ret;
  g_mount_ev[k].lr = lr;
  g_mount_ev[k].pid = ksceKernelGetProcessId();
  g_mount_ev[k].ms = g_t_resume ? (int)((now - g_t_resume) / 1000) : -1;
}

/* io thread */
static void mount_events_log(void) {
  static char line[220];
  uint32_t w = g_mount_ev_w;
  if (w - g_mount_ev_r > MOUNT_EV_N) /* the hooks dropped the newest */
    w = g_mount_ev_r + MOUNT_EV_N;
  while (g_mount_ev_r != w) {
    int k = g_mount_ev_r % MOUNT_EV_N;
    char name[0x1C + 4];
    const void *text = NULL;
    memset(name, 0, sizeof(name));
    if (!g_mod_by_addr ||
        g_mod_by_addr(0x10005, (const void *)g_mount_ev[k].lr, NULL, &text,
                      name) < 0) {
      strcpy(name, "?");
      text = NULL;
    }
    int id = g_mount_ev[k].id;
    snprintf(line, sizeof(line), "mount table: %s 0x%X (%s) arg %d -> 0x%08X,"
             " called from %s+0x%X (process 0x%X), %d ms after the last "
             "wake-up\r\n", g_mount_ev[k].unmount == 2
                 ? "REFUSED unmount (the card stays mounted)"
                 : g_mount_ev[k].unmount ? "unmount" : "mount", id,
             id == 0x800 ? "ux0:" : id == 0xF00 ? "uma0:" : "xmc0:",
             g_mount_ev[k].arg, (unsigned)g_mount_ev[k].ret, name,
             (unsigned)(g_mount_ev[k].lr - (uintptr_t)text),
             (unsigned)g_mount_ev[k].pid, g_mount_ev[k].ms);
    log_append(line);
    g_mount_ev_r++;
  }
  g_mount_ev_r = g_mount_ev_w > g_mount_ev_r + MOUNT_EV_N ? g_mount_ev_w
                                                         : g_mount_ev_r;
}

static int umount_hook(int id, int a2, int a3, int a4) {
  uintptr_t lr = (uintptr_t)__builtin_return_address(0);
  /* only in the first 15 s after one of JamaisVu's own wake-ups (the
   * save/load path): at shutdown/reboot or when the card is pulled out,
   * SceSdstor's unmount must happen (a4ce776-98c6bf6 refused it at any
   * time: the PCH-1000 restarted by itself when rebooted from Settings) */
  int64_t since = g_t_resume ? ksceKernelGetSystemTimeWide() - g_t_resume
                             : -1;
  if (since >= 0 && since < 15000000 &&
      g_sdstor_hi && lr >= g_sdstor_lo && lr < g_sdstor_hi &&
      ((id == 0xF00 && g_card_on[0]) || (id == 0xE00 && g_card_on[1]))) {
    mount_note(2, id, a2, 0, lr); /* refused, reported as done */
    return 0;
  }
  int ret = TAI_CONTINUE(int, g_umount_ref, id, a2, a3, a4);
  mount_note(1, id, a2, ret, lr);
  return ret;
}

static int mount_hook(int id, const char *path, int perm, int a4, int a5,
                      int a6) {
  uintptr_t lr = (uintptr_t)__builtin_return_address(0);
  int ret = TAI_CONTINUE(int, g_mount_ref, id, path, perm, a4, a5, a6);
  mount_note(0, id, perm, ret, lr);
  return ret;
}

static void mount_watch_install(void) {
  tai_module_info_t ti;
  memset(&ti, 0, sizeof(ti));
  ti.size = sizeof(ti);
  if (taiGetModuleInfoForKernel(KERNEL_PID, "SceSdstor", &ti) >= 0) {
    /* SceModulemgrForKernel, not in the stubs this links: by NID */
    int (*get_info)(SceUID pid, SceUID modid, SceKernelModuleInfo *info) =
        NULL;
    SceKernelModuleInfo mi;
    memset(&mi, 0, sizeof(mi));
    mi.size = sizeof(mi);
    if (module_get_export_func(KERNEL_PID, "SceKernelModulemgr",
                               TAI_ANY_LIBRARY, 0xD269F915,
                               (uintptr_t *)&get_info) >= 0 && get_info &&
        get_info(KERNEL_PID, ti.modid, &mi) >= 0 &&
        mi.segments[0].vaddr && mi.segments[0].memsz) {
      g_sdstor_lo = (uintptr_t)mi.segments[0].vaddr;
      g_sdstor_hi = g_sdstor_lo + mi.segments[0].memsz;
    }
  }
  {
    static char line[160];
    snprintf(line, sizeof(line), "card mount guard: SceSdstor code %s "
             "0x%08X-0x%08X, card on uma0 %d, on xmc0 %d\r\n",
             g_sdstor_hi ? "at" : "NOT FOUND", (unsigned)g_sdstor_lo,
             (unsigned)g_sdstor_hi, storage_mount_is_card(0xF00),
             storage_mount_is_card(0xE00));
    log_queue(line);
  }
  mount_card_flags_update();
  if (module_get_export_func(KERNEL_PID, "SceKernelModulemgr",
                             TAI_ANY_LIBRARY, 0x8309E043,
                             (uintptr_t *)&g_mod_by_addr) < 0)
    g_mod_by_addr = NULL;
  g_umount_hook = taiHookFunctionExportForKernel(
      KERNEL_PID, &g_umount_ref, "SceIofilemgr", TAI_ANY_LIBRARY, 0x20574100,
      umount_hook);
  g_mount_hook = taiHookFunctionExportForKernel(
      KERNEL_PID, &g_mount_ref, "SceIofilemgr", TAI_ANY_LIBRARY, 0xD070BC48,
      mount_hook);
}

/* The Memory Card back on uma0/xmc0 when it was left unmounted after a
 * sleep (usbmc / StorageMgr mount it there only at boot): checked by the
 * io thread every 2 s (the input thread never touches the card); acted on
 * when it is still missing at the next check, so the system's own remount
 * after a wake-up is not raced. */
static void card_watch(void) {
  static int64_t next;
  static int missing, failures;
  int64_t t = now_us();
  const char *root = NULL;
  if (t < next || g_busy)
    return;
  next = t + 2000000;
  mount_card_flags_update();
  if (!storage_card_unmounted(&root)) {
    missing = 0;
    return;
  }
  if (++missing < 2)
    return;
  int r = storage_remount_card(&root);
  int ok = storage_detect() != NULL;
  if (ok || failures < 3) {
    static char line[160];
    snprintf(line, sizeof(line), "Memory Card was not mounted anywhere "
             "(its mount %s was down): mounted it again, result 0x%08X, %s\r\n",
             root ? root : "?", (unsigned)r, ok ? "found" : "still missing");
    log_append(line); /* after storage_detect: written if found */
  }
  failures = ok ? 0 : failures + 1;
  missing = 0;
  if (!ok)
    next = t + (failures > 3 ? 30000000 : 5000000);
}

static int io_thread(SceSize args, void *argp) {
  static char batch[sizeof(g_logq)];
  static char tid[16];

  while (1) {
    unsigned int out = 0;
    SceUInt32 timeout = 2000000;
    int w = ksceKernelWaitEventFlag(g_io_evf, IOEV_LOG | IOEV_DECIDE,
                                    SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR_PAT,
                                    &out, &timeout);
    card_watch();
    mount_events_log();
    if (w < 0)
      continue;

    if (out & IOEV_DECIDE) {
      ksceKernelLockMutex(g_logq_mtx, 1, NULL);
      memcpy(tid, g_decide_tid, sizeof(tid));
      tid[sizeof(tid) - 1] = '\0';
      ksceKernelUnlockMutex(g_logq_mtx, 1);
      if (tid[0])
        decide_buttons(tid);
    }

    ksceKernelLockMutex(g_logq_mtx, 1, NULL);
    int n = g_logq_len;
    memcpy(batch, g_logq, n);
    g_logq_len = 0;
    ksceKernelUnlockMutex(g_logq_mtx, 1);
    for (int i = 0; i < n;) {
      log_append(batch + i);
      i += (int)strlen(batch + i) + 1;
    }
  }
  return 0;
}

int menu_init(const char *status_text) {
  int assign = 1;
  /* /CONFIG/SYSTEM/button_assign: 0 = ○ confirms (Japan), 1 = ✕ confirms */
  if (ksceRegMgrGetKeyInt("/CONFIG/SYSTEM", "button_assign", &assign) >= 0)
    g_system_circle_enter = (assign == 0);
  SceUID thid;

  g_status_text = status_text;
  g_perf_since = prof_now();

  if (jv_disp_type() == JV_DISP_HDMI) {
    g_dolce = 1;
    g_fb_max_w = 1920;
    g_fb_max_h = 1088;
  }

  /* SceSysmemForDriver ksceKernelVAtoPA / ksceKernelVAtoPABySW (display
   * test: where the game's frame is in physical memory) */
  if (module_get_export_func(KERNEL_PID, "SceSysmem", 0x6F25E18A, 0x8D160E65,
                             (uintptr_t *)&g_va2pa_hw) < 0)
    g_va2pa_hw = NULL;
  if (module_get_export_func(KERNEL_PID, "SceSysmem", 0x6F25E18A, 0x65419BD3,
                             (uintptr_t *)&g_va2pa_sw) < 0)
    g_va2pa_sw = NULL;

  if (module_get_export_func(KERNEL_PID, "SceSysmem", 0x40ECDB0E, 0x364E68A4,
                             (uintptr_t *)&g_dcache_clean_inv) < 0)
    g_dcache_clean_inv = NULL;

  /* overlay memory is reserved on demand (overlay_acquire) */
  g_ovl_mtx = ksceKernelCreateMutex("JamaisVuOverlay", 0, 0, NULL);

  g_io_evf = ksceKernelCreateEventFlag("JamaisVuIoEvf", 0, 0, NULL);
  g_logq_mtx = ksceKernelCreateMutex("JamaisVuLogQ", 0, 0, NULL);
  mount_watch_install(); /* logs through the queue: after it exists */
  g_log_mtx = ksceKernelCreateMutex("JamaisVuLog", 0, 0, NULL);
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

  if (g_io_evf >= 0 && g_logq_mtx >= 0 && g_log_mtx >= 0) {
    thid = ksceKernelCreateThread("JamaisVuIo", io_thread, 0x70, 0x4000, 0,
                                  0x10000, NULL);
    if (thid >= 0)
      ksceKernelStartThread(thid, 0, NULL);
  }

  /* lowest priority (below the game's threads), any of the game cores;
   * without it the display hook draws the menu itself, as before */
  g_render_sema = ksceKernelCreateSema("JamaisVuRender", 0, 0, 1, NULL);
  if (g_render_sema >= 0) {
    thid = ksceKernelCreateThread("JamaisVuRender", render_thread, 0xBF,
                                  0x4000, 0, 0x70000, NULL);
    if (thid < 0)
      thid = ksceKernelCreateThread("JamaisVuRender", render_thread, 0xBF,
                                    0x4000, 0, 0x10000, NULL);
    if (thid < 0 || ksceKernelStartThread(thid, 0, NULL) < 0) {
      ksceKernelDeleteSema(g_render_sema);
      g_render_sema = -1;
    }
  }
  g_render_thread_ok = g_render_sema >= 0;

  return 0;
}
