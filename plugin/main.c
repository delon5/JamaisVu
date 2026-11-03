/* JamaisVu savestate plugin
 *
 * Copyright (C) 2020 TheFloW
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <psp2kern/kernel/cpu.h>
#include <psp2kern/kernel/modulemgr.h>
#include <psp2kern/kernel/suspend.h>
#include <psp2kern/kernel/sysmem.h>
#include <psp2kern/kernel/threadmgr.h>
#include <psp2kern/io/fcntl.h>
#include <psp2kern/io/dirent.h>
#include <psp2kern/io/stat.h>
#include <psp2kern/ctrl.h>
#include <psp2kern/power.h>
#include <taihen.h>

#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#include "compat.h"
#include "main.h"
#include "fs.h"
#include "offsets.h"
#include "slots.h"
#include "menu.h"
#include "prof.h"
#include "storage.h"
#include "trace.h"

#define JV_VERSION "2.0-dev"
#ifndef JV_BUILD_ID
#define JV_BUILD_ID "unknown"
#endif

/* Clock speeds (MHz) when a save/load was requested: CPU, bus, GPU,
 * GPU crossbar. They live in the plugin's memory, so they are part of a
 * snapshot: after a load, the plugin inside the restored image still has
 * the snapshot's values and puts the hardware back to them (the payload
 * restores memory only, not clock registers). */
static int g_clocks[4];
static int g_clocks_valid;

/* ScePowerForDriver (0x1590166F), resolved by NID as PSV-Shell-Plus does
 * (not all of these are in the SDK headers). The GPU clock is the "ES4"
 * pair (both values set to the same frequency). */
static int (*p_get_arm)(void), (*p_get_bus)(void), (*p_get_xbar)(void);
static int (*p_get_gpu)(int *a, int *b);
static int (*p_set_arm)(int), (*p_set_bus)(int), (*p_set_xbar)(int);
static int (*p_set_gpu)(int a, int b);

static void clocks_resolve(void) {
  static int done;
  if (done)
    return;
  done = 1;
#define RES(var, nid) \
  if (module_get_export_func(KERNEL_PID, "ScePower", 0x1590166F, nid, \
                             (uintptr_t *)&var) < 0) \
    var = NULL
  RES(p_get_arm, 0xABC6F88F);
  RES(p_get_bus, 0x478FE6F5);
  RES(p_get_gpu, 0x475BCC82);
  RES(p_get_xbar, 0x0A750DEE);
  RES(p_set_arm, 0x74DB5AE5);
  RES(p_set_bus, 0xB8D7B3FB);
  RES(p_set_gpu, 0x264C24FC);
  RES(p_set_xbar, 0xA7739DBE);
#undef RES
}

static void clocks_read(int c[4]) {
  int a = 0, b = 0;
  clocks_resolve();
  c[0] = p_get_arm ? p_get_arm() : 0;
  c[1] = p_get_bus ? p_get_bus() : 0;
  if (p_get_gpu)
    p_get_gpu(&a, &b);
  c[2] = a;
  c[3] = p_get_xbar ? p_get_xbar() : 0;
}

static void clocks_fmt(char *out, int len, const int c[4]) {
  snprintf(out, len, "cpu %d bus %d gpu %d xbar %d", c[0], c[1], c[2], c[3]);
}

void jv_clocks_remember(char *out, int len) {
  clocks_read(g_clocks);
  g_clocks_valid = 1;
  clocks_fmt(out, len, g_clocks);
}

void jv_clocks_restore(char *out, int len) {
  static char a[64], b[64], c[64];
  int now[4], after[4];

  clocks_read(now);
  clocks_fmt(a, sizeof(a), now);
  if (!g_clocks_valid) {
    snprintf(out, len, "clocks now %s (none recorded)", a);
    return;
  }
  if (now[0] != g_clocks[0] && g_clocks[0] > 0 && p_set_arm)
    p_set_arm(g_clocks[0]);
  if (now[1] != g_clocks[1] && g_clocks[1] > 0 && p_set_bus)
    p_set_bus(g_clocks[1]);
  if (now[2] != g_clocks[2] && g_clocks[2] > 0 && p_set_gpu)
    p_set_gpu(g_clocks[2], g_clocks[2]);
  if (now[3] != g_clocks[3] && g_clocks[3] > 0 && p_set_xbar)
    p_set_xbar(g_clocks[3]);
  clocks_read(after);
  clocks_fmt(b, sizeof(b), g_clocks);
  clocks_fmt(c, sizeof(c), after);
  snprintf(out, len, "clocks at request: %s; after resume: %s; now: %s", b,
           a, c);
}

const char *jv_build_id(void) {
#if defined(JAMAISVU_TESTING)
  return JV_BUILD_ID "-testing";
#else
  return JV_BUILD_ID;
#endif
}

extern int resume_function(void);

extern uint8_t _binary_payload_JamaisVu_payload_bin_start;
extern uint8_t _binary_payload_JamaisVu_payload_bin_size;

/* The payload resume.S copies to PAYLOAD_PADDR (see jv_request). */
uint32_t payload_src, payload_len;

int (* _ksceKernelFindClassByName)(const char *name, SceClass **cls);
int (* _ksceGUIDGetUIDVectorByClass)(SceClass *cls, int vis_level,
                                     SceUID *vector, int num, int *ret_num);

static tai_hook_ref_t ksceSysconSendCommandRef;

static uint32_t *g_rtc_ctx;

uint32_t ori_resume_func_vaddr;

PayloadArguments pargs;
uint32_t pargs_size = sizeof(PayloadArguments);

static int g_armed = 0;
static const char *g_disabled_reason = "not started";
static const FwOffsets *g_fw = NULL;
static char g_status[1024];

int jv_armed(void) {
  return g_armed;
}

/* armed on offsets that are not hardware-verified yet (testing build) */
int jv_armed_untested(void) {
  return g_armed && g_fw && g_fw->verified < VERIFIED_HARDWARE;
}

const char *jv_disabled_reason(void) {
  return g_disabled_reason;
}

/* Virtual -> physical address of a kernel object (ATS1CPR). 0 on fault. */
static uint32_t va2pa(const volatile void *va) {
  uint32_t par;
  __asm__ volatile ("mcr p15, 0, %1, c7, c8, 0\n\t"
                    "isb\n\t"
                    "mrc p15, 0, %0, c7, c4, 0\n\t"
                    : "=r" (par) : "r" (va));
  if (par & 1)
    return 0;
  return (par & 0xfffff000) | ((uint32_t)va & 0xfff);
}

static void *pa2va(uint32_t pa) {
  uint32_t va = 0;
  uint32_t vaddr;
  uint32_t paddr;
  uint32_t i;

  for (i = 0; i < 0x100000; i++) {
    vaddr = i << 12;
    __asm__ volatile ("mcr p15, 0, %1, c7, c8, 0\n\t"
                      "mrc p15, 0, %0, c7, c4, 0\n\t"
                      : "=r" (paddr) : "r" (vaddr));
    if ((pa & 0xfffff000) == (paddr & 0xfffff000)) {
      va = vaddr + (pa & 0xfff);
      break;
    }
  }

  return (void *)va;
}

static void map_memory(void) {
  uint32_t *ttb;
  uint32_t ttbcr;
  uint32_t ttbr0;
  uint32_t ttbr_mask[2];

  __asm__ volatile ("mrc p15, 0, %0, c2, c0, 2\n\t"
                    "mrc p15, 0, %1, c2, c0, 0\n\t"
                    : "=r"(ttbcr), "=r"(ttbr0));

  ttbr_mask[0] = 0xffffffff << (14 - (ttbcr & 0x7));
  ttbr_mask[1] = 0xffffffff << 14;

  ttb = (uint32_t *)pa2va(ttbr0 & ttbr_mask[0]);

  ttb[0x1c0] = 0x1c000000 | 0x91402;
  ttb[0x1f0] = 0x1f000000 | 0x91402;

  __asm__ volatile ("dmb sy");
  __asm__ volatile ("mcr p15, 0, %0, c7, c14, 1"
                    :: "r" (&ttb[0x1c0]) : "memory");
  __asm__ volatile ("mcr p15, 0, %0, c7, c14, 1"
                    :: "r" (&ttb[0x1f0]) : "memory");
  __asm__ volatile ("dsb sy\n\t"
                    "mcr p15, 0, r0, c8, c7, 0\n\t"
                    "dsb sy\n\t"
                    "isb sy" ::: "memory");
}

static int get_ttbs_and_titleids(void) {
  uint32_t ttbcr;
  uint32_t ttbr0;
  uint32_t ttbr_mask[2];
  SceUID vector[JV_MAX_PROCESSES];
  SceClass *cls;
  int i;
  int res;

  res = _ksceKernelFindClassByName("SceUIDProcessClass", &cls);
  if (res < 0)
    return res;

  res = _ksceGUIDGetUIDVectorByClass(cls, 8, vector,
                                     sizeof(vector) / sizeof(SceUID),
                                     (int *)&pargs.process_num);
  if (res < 0) {
    pargs.process_num = 0;
    return res;
  }
  if (pargs.process_num > JV_MAX_PROCESSES)
    pargs.process_num = JV_MAX_PROCESSES;

  __asm__ volatile ("mrc p15, 0, %0, c2, c0, 2\n\t"
                    "mrc p15, 0, %1, c2, c0, 0\n\t"
                    : "=r"(ttbcr), "=r"(ttbr0));

  ttbr_mask[0] = 0xffffffff << (14 - (ttbcr & 0x7));
  ttbr_mask[1] = 0xffffffff << 14;

  for (i = 0; i < pargs.process_num; i++) {
    SceKernelProcessContext *ctx;
    ksceKernelGetPidContext(vector[i], &ctx);
    pargs.ttbs[i] = ctx->TTBR1 & ttbr_mask[1];

    ksceKernelGetProcessTitleId(vector[i], pargs.titleid[i], 32);
  }

  pargs.ttbs[i] = ttbr0 & ttbr_mask[0];

  return 0;
}

static void get_fattime(void) {
  SceDateTime time;
  ksceRtcGetCurrentClock(&time);
  pargs.fattime = ((time.year - 1980) << 25) |
                   (time.month  << 21) |
                   (time.day    << 16) |
                   (time.hour   << 11) |
                   (time.minute << 5)  |
                   (time.second << 0);
}

static int ksceSysconSendCommandPatched(int cmd, void *buffer, uint32_t size) {
  SceSysconResumeContext *resume_ctx;
  uint32_t t0 = prof_now();

  if (pargs.mode != JAMAISVU_MODE_NONE) {
    if (cmd == SCE_SYSCON_CMD_RESET_DEVICE && size == sizeof(uintptr_t)) {
      if (*(uintptr_t *)buffer != 0) {
        resume_ctx = (SceSysconResumeContext *)pa2va(*(uintptr_t *)buffer);
        ori_resume_func_vaddr = resume_ctx->resume_func_vaddr;
        resume_ctx->resume_func_vaddr = (uint32_t)&resume_function;
      }
    }
  }

  prof_add(PROF_SYSCON, t0);
  return TAI_CONTINUE(int, ksceSysconSendCommandRef, cmd, buffer, size);
}

/* Timing around the payload (system time, which stops while the system
 * sleeps): request -> sleep event, wake event -> system resumed. */
volatile int64_t g_t_request, g_t_suspend, g_t_resume, g_t_resumed;
volatile int64_t g_t_fs_done; /* load: ux0 re-read (0 = not done) */
/* Diagnostics: every system event during a save/load (suspend and resume
 * phases) with its time, to see where request -> sleep goes (3.9 s). */
volatile uint32_t g_ev_n;
uint32_t g_ev_id[JV_EV_MAX];   /* event id, bit 31 = resume phase */
int64_t g_ev_t[JV_EV_MAX];     /* first time of a run of the same event */
int64_t g_ev_last[JV_EV_MAX];  /* last time of that run */
uint32_t g_ev_cnt[JV_EV_MAX];  /* how often in a row (0x401 repeats every
                                * 10 ms: ac9acfb filled the list at 280 ms) */

/* recording state: open from the request (or, for a sleep JamaisVu did not
 * ask for, the first suspend event) until the 0x100000 resume event */
volatile int g_ev_open, g_ev_normal, g_ev_normal_ready;
int64_t g_ev_t0, g_ev_r0; /* first suspend / first resume event */

static int savestate_system_event(int resume, int eventid,
                                  void *args, void *opt) {
  {
    uint32_t id = (uint32_t)eventid | (resume ? 0x80000000u : 0);
    int64_t now = ksceKernelGetSystemTimeWide();
    if (!resume && pargs.mode == JAMAISVU_MODE_NONE &&
        (!g_ev_open ||
         (g_ev_n && (g_ev_id[g_ev_n - 1] & 0x80000000u)))) {
      /* a normal sleep (power button / auto standby): recorded too, to
       * compare with the 3.6 s of 0x401 seen before every save */
      g_ev_n = 0;
      g_ev_t0 = now;
      g_ev_r0 = 0;
      g_ev_normal = 1;
      g_ev_open = 1;
    }
    if (!g_ev_open)
      goto ev_done;
    if (resume && !g_ev_r0) {
      g_ev_r0 = now;
      /* logged by menu.c 3 s after the first resume event: a sleep from
       * the LiveArea with no game open may never send 0x100000 (3566f60
       * logged nothing for one) */
      if (g_ev_normal && pargs.mode == JAMAISVU_MODE_NONE)
        g_ev_normal_ready = 1;
    }
    uint32_t n = g_ev_n;
    if (n && g_ev_id[n - 1] == id) {
      g_ev_cnt[n - 1]++;
      g_ev_last[n - 1] = now;
    } else if (n < JV_EV_MAX) {
      g_ev_id[n] = id;
      g_ev_t[n] = g_ev_last[n] = now;
      g_ev_cnt[n] = 1;
      g_ev_n = n + 1;
    }
    if (resume && eventid == 0x100000)
      g_ev_open = 0;
  }
ev_done:
  if (pargs.mode != JAMAISVU_MODE_NONE) {
    if (!resume) {
      if (eventid == 0x4000) {
        g_t_suspend = ksceKernelGetSystemTimeWide();
        memcpy(pargs.rtc_ctx, g_rtc_ctx, SCE_RTC_CTX_SIZE);

        map_memory();
        get_fattime();

        if (pargs.mode == JAMAISVU_MODE_SAVE)
          get_ttbs_and_titleids();

        /* NOTE: writes are deliberately NOT blocked here any more. This
         * image is about to be discarded (load) or kept (save); either way
         * its final flush must reach the card so the card is consistent
         * before the payload looks at it. Blocking happens inside the
         * restored image instead (the payload arms the flag). */
      }
    } else {
      if (eventid == 0x10000) {
        g_t_resume = ksceKernelGetSystemTimeWide();
        menu_arm_first_frame();
        memcpy(&pargs, (void *)PAYLOAD_ARGS_PADDR, sizeof(PayloadArguments));
        memcpy(g_rtc_ctx, pargs.rtc_ctx, SCE_RTC_CTX_SIZE);
        trace_wake_begin(&pargs);
        trace_wake(JV_WAKE_RESUME);
      } else if (eventid == 0x100000) {
        g_t_resumed = ksceKernelGetSystemTimeWide();
        g_t_fs_done = 0;
        int stale = 0;

        if (pargs.mode == JAMAISVU_MODE_LOAD && pargs.result == JV_OK) {
          trace_wake_io(1); /* the refresh below reads the card anyway */
          trace_wake(JV_WAKE_RESUMED);
          /* We are the restored image and the payload has armed
           * FS_WRITES_DROPPED. Re-read ux0 from the card, then allow
           * writes again - unless files that are still open no longer
           * exist, in which case ux0 stays read-only until reboot rather
           * than risk writing through stale cluster chains. */
          stale = update_filesystem();
          g_t_fs_done = ksceKernelGetSystemTimeWide();
          fs_set_write_mode(stale ? FS_WRITES_READ_ONLY : FS_WRITES_ALLOWED);
        } else {
          trace_wake(JV_WAKE_RESUMED);
          if (fs_get_write_mode() == FS_WRITES_DROPPED)
            fs_set_write_mode(FS_WRITES_ALLOWED);
        }

        trace_wake(JV_WAKE_MENU);
        menu_on_resumed(&pargs, stale);
        pargs.mode = JAMAISVU_MODE_NONE;
      }
    }
  }

  return 0;
}

/* Display test: the frame the game showed (see menu.c, record_frame) */
void jv_set_display_frame(uint32_t paddr, uint32_t pitch, uint32_t width,
                          uint32_t height, uint32_t vic) {
  pargs.disp_type = paddr ? jv_disp_type() : JV_DISP_NONE;
  pargs.disp_vic = vic;
  pargs.disp_fb_paddr = paddr;
  pargs.disp_fb_pitch = pitch;
  pargs.disp_fb_width = width;
  pargs.disp_fb_height = height;
}

/* SceSysrootForDriver (0x2ED7F97A) GetHardwareInfo / GetHardwareFlags,
 * resolved by NID: not every SDK declares them */
static uint32_t (*g_hw_info_fn)(void);
static int (*g_hw_flags_fn)(void *flags);

static void resolve_hw_fns(void) {
  if (!g_hw_info_fn &&
      module_get_export_func(KERNEL_PID, "SceSysmem", 0x2ED7F97A, 0x930B1342,
                             (uintptr_t *)&g_hw_info_fn) < 0)
    g_hw_info_fn = NULL;
  if (!g_hw_flags_fn &&
      module_get_export_func(KERNEL_PID, "SceSysmem", 0x2ED7F97A, 0x46E72428,
                             (uintptr_t *)&g_hw_flags_fn) < 0)
    g_hw_flags_fn = NULL;
}

void jv_set_appdb_changed(uint32_t v) {
  pargs.appdb_changed = v;
}

void jv_set_display_frame2(uint32_t paddr, uint32_t pitch) {
  pargs.disp_fb2_paddr = paddr;
  pargs.disp_fb2_pitch = pitch;
}

uint32_t jv_hw_info(void) {
  resolve_hw_fns();
  return g_hw_info_fn ? g_hw_info_fn() : 0;
}

/* SceSblAIMgrForDriver: 1 on a PS TV ("Dolce") */
int ksceSblAimgrIsDolce(void);

/* The console's display, for the payload's progress screen:
 *   PS TV -> HDMI; handheld with a PCH-2000 board (hardware info 0x80xxxx
 *   and up, e.g. 0x00822238) -> LCD; any other handheld -> OLED. */
uint32_t jv_disp_type(void) {
  static int type = -1;
  if (type < 0) {
    if (ksceSblAimgrIsDolce())
      type = JV_DISP_HDMI;
    else if ((jv_hw_info() & 0xFF0000) >= 0x800000)
      type = JV_DISP_LCD;
    else
      type = JV_DISP_OLED;
  }
  return (uint32_t)type;
}

/* Starts a save or load of a slot: fills in the payload arguments and
 * requests the soft reset during which the payload runs. */
int jv_request(int mode, int slot, const uint8_t *nonce, int debug_mode,
               uint32_t trace_rel_lba, const uint8_t *trace_nonce,
               int payload_variant) {
  uint32_t flag_pa;

  if (!g_armed || slot < 0 || slot >= JV_NUM_SLOTS)
    return -1;

  flag_pa = va2pa(fs_write_mode_addr());
  if (flag_pa == 0)
    return -1;

  /* resume_memcpy copies whole words until the length reaches 0 */
  (void)payload_variant;
  payload_src = (uint32_t)&_binary_payload_JamaisVu_payload_bin_start;
  payload_len = (uint32_t)&_binary_payload_JamaisVu_payload_bin_size;
  payload_len = (payload_len + 3) & ~3u;

  pargs.slot = slot;
  slot_payload_path(pargs.path, sizeof(pargs.path), slot);
  /* savedata lives on ux0: only back it up if ux0 is the Memory Card
   * (JamaisVu never writes to any other storage) */
  if (!storage_mc_root())
    return -1;
  pargs.backup_savedata = strcmp(storage_mc_root(), "ux0:") == 0;
  pargs.slot_size = JV_SLOT_SIZE;
  if (nonce)
    memcpy(pargs.slot_nonce, nonce, sizeof(pargs.slot_nonce));
  else
    memset(pargs.slot_nonce, 0, sizeof(pargs.slot_nonce));
  pargs.ignore_flag_paddr = flag_pa;
  pargs.debug_mode = debug_mode;
  pargs.result = JV_ERR_ARGS;
  pargs.applied = 0;
  pargs.stage = 0;
  pargs.detail = 0;
  pargs.soc_timer_10ms = 0;
  pargs.trace_n = 0;
  pargs.fault_type = pargs.fault_addr = pargs.fault_lr = 0;
  if (debug_mode == JV_TEST_DISPLAY || debug_mode == JV_TEST_DRYRUN ||
      debug_mode == JV_TEST_NONE) {
    uint8_t flags[16];
    memset(flags, 0, sizeof(flags));
    resolve_hw_fns();
    if (g_hw_flags_fn)
      g_hw_flags_fn(flags);
    pargs.disp_hw_info = jv_hw_info();
    pargs.disp_hw_flags0 = flags[0];
  } else {
    pargs.disp_fb_paddr = 0; /* reset test: no screen */
    pargs.disp_type = JV_DISP_NONE;
  }
  if (!(debug_mode == JV_TEST_NONE && mode == JAMAISVU_MODE_LOAD))
    pargs.disp_fb2_paddr = 0; /* the snapshot's frame: loads only */
  pargs.trace_rel_lba = trace_nonce ? trace_rel_lba : 0;
  if (trace_nonce)
    memcpy(pargs.trace_nonce, trace_nonce, sizeof(pargs.trace_nonce));
  else
    memset(pargs.trace_nonce, 0, sizeof(pargs.trace_nonce));
  __sync_synchronize();
  pargs.mode = mode;
  __sync_synchronize();

  pargs.mem_bytes = pargs.mem_ms = 0;
  pargs.icache_on = pargs.delay_unit_ns = 0;
  g_ev_open = 0;
  g_ev_n = 0;
  g_t_request = ksceKernelGetSystemTimeWide();
  g_ev_t0 = g_t_request;
  g_ev_r0 = 0;
  g_ev_normal = 0;
  g_ev_normal_ready = 0;
  g_ev_open = 1;
  g_t_suspend = g_t_resume = g_t_resumed = 0;
  int res = kscePowerRequestSoftReset();
  if (res < 0) {
    pargs.mode = JAMAISVU_MODE_NONE;
    return res;
  }
  return 0;
}

static void get_sysmem_nids(void) {
  int res;

  res = module_get_export_func(KERNEL_PID, "SceSysmem",
                               0x63A519E5,
                               0x62989905,
                               (uintptr_t *)&_ksceKernelFindClassByName);
  if (res < 0)
    res = module_get_export_func(KERNEL_PID, "SceSysmem",
                                 0x02451F0F,
                                 0x7D87F706,
                                 (uintptr_t *)&_ksceKernelFindClassByName);

  res = module_get_export_func(KERNEL_PID, "SceSysmem",
                               0x63A519E5,
                               0xEC7D36EF,
                               (uintptr_t *)&_ksceGUIDGetUIDVectorByClass);
  if (res < 0)
    res = module_get_export_func(KERNEL_PID, "SceSysmem",
                                 0x02451F0F,
                                 0x52137FA3,
                                 (uintptr_t *)&_ksceGUIDGetUIDVectorByClass);
}

static int get_rtc_offsets(const FwOffsets *o) {
  tai_module_info_t info;

  memset(&info, 0, sizeof(tai_module_info_t));
  info.size = sizeof(tai_module_info_t);
  if (taiGetModuleInfoForKernel(KERNEL_PID, "SceRtc", &info) < 0)
    return -1;

  return module_get_offset(KERNEL_PID, info.modid, 1, o->rtc_ctx,
                           (uintptr_t *)&g_rtc_ctx);
}

static uint32_t module_nid(const char *name) {
  tai_module_info_t info;
  memset(&info, 0, sizeof(info));
  info.size = sizeof(info);
  if (taiGetModuleInfoForKernel(KERNEL_PID, name, &info) < 0)
    return 0;
  return info.module_nid;
}

static int patch_syscon(void) {
  return taiHookFunctionExportForKernel(KERNEL_PID,
                                        &ksceSysconSendCommandRef,
                                        "SceSyscon",
                                        TAI_ANY_LIBRARY,
                                        0xE26488B9,
                                        ksceSysconSendCommandPatched);
}

static int arm(void) {
  const char *why;

  g_fw = offsets_find(module_nid("SceIofilemgr"));
  if (!offsets_usable(g_fw, module_nid("SceExfatfs"), &why)) {
    g_disabled_reason = why;
    return -1;
  }

  get_sysmem_nids();
  if (!_ksceKernelFindClassByName || !_ksceGUIDGetUIDVectorByClass) {
    g_disabled_reason = "SceSysmem functions not found";
    return -1;
  }

  if (get_rtc_offsets(g_fw) < 0 || !g_rtc_ctx) {
    g_disabled_reason = "SceRtc context not found";
    return -1;
  }

  if (fs_init(g_fw) < 0) {
    g_disabled_reason = "could not hook the exFAT driver";
    return -1;
  }

  /* the system's mount table tells which mount is the Memory Card */
  {
    tai_module_info_t info;
    uintptr_t fn = 0;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    if (taiGetModuleInfoForKernel(KERNEL_PID, "SceIofilemgr", &info) < 0 ||
        module_get_offset(KERNEL_PID, info.modid, 0,
                          g_fw->iofilemgr_find_mount_point, &fn) < 0 ||
        !fn) {
      g_disabled_reason = "mount table lookup not found";
      return -1;
    }
    storage_set_mount_lookup((MountLookupFn)fn);
  }

  /* ksceIoMount / ksceIoUmount, to mount the card again on uma0/xmc0
   * after a sleep (usbmc only does it at boot) */
  {
    uintptr_t mnt = 0, umnt = 0;
    if (module_get_export_func(KERNEL_PID, "SceIofilemgr", TAI_ANY_LIBRARY,
                               0xD070BC48, &mnt) >= 0 &&
        module_get_export_func(KERNEL_PID, "SceIofilemgr", TAI_ANY_LIBRARY,
                               0x20574100, &umnt) >= 0)
      storage_set_mount_ops((MountFn)mnt, (UmountFn)umnt);
  }

  if (patch_syscon() < 0) {
    g_disabled_reason = "could not hook SceSyscon";
    return -1;
  }

  if (ksceKernelRegisterSysEventHandler("savestate_system_event",
                                        savestate_system_event, NULL) < 0) {
    g_disabled_reason = "could not register the system event handler";
    return -1;
  }

  g_disabled_reason = "armed";
  return 0;
}

void _start() __attribute__ ((weak, alias ("module_start")));
int module_start(SceSize args, void *argp) {
  memset(&pargs, 0, sizeof(PayloadArguments));

  g_armed = (arm() == 0);

  snprintf(g_status, sizeof(g_status),
           "JamaisVu " JV_VERSION "\n"
           "status: %s\n"
           "firmware table: %s (verification level %d of 2)\n"
           "build: %s\n"
           "SceIofilemgr NID: 0x%08X\n"
           "SceExfatfs NID:   0x%08X\n"
           "SceRtc NID:       0x%08X\n"
           "SceSysmem NID:    0x%08X\n"
           "SceCtrl NID:      0x%08X\n"
           "SceDisplay NID:   0x%08X\n"
           "hardware info:    0x%08X\n"
           "display:          %s (PS TV check %d)\n"
           "\n"
           "If status is not \"armed\", JamaisVu changes nothing on this\n"
           "system. See docs/porting-3.60.md.\n",
           g_armed ? "armed" : g_disabled_reason,
           g_fw ? g_fw->name : "none", g_fw ? g_fw->verified : 0,
           jv_build_id(),
           (unsigned)module_nid("SceIofilemgr"),
           (unsigned)module_nid("SceExfatfs"),
           (unsigned)module_nid("SceRtc"),
           (unsigned)module_nid("SceSysmem"),
           (unsigned)module_nid("SceCtrl"),
           (unsigned)module_nid("SceDisplay"),
           (unsigned)jv_hw_info(),
           jv_disp_type() == JV_DISP_HDMI ? "HDMI (PS TV)" :
           jv_disp_type() == JV_DISP_OLED ? "OLED (PCH-1000)" :
           "LCD (PCH-2000)", ksceSblAimgrIsDolce());

  menu_init(g_status);

  return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize args, void *argp) {
  return SCE_KERNEL_STOP_CANCEL;
}
