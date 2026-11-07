/* JamaisVu savestate plugin
 *
 * Copyright (C) 2020 TheFloW
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "main.h"
#include "state.h"
#include "pervasive.h"
#include "gpio.h"
#include "syscon.h"
#include "utils.h"
#include "guard.h"
#include "msif/ms.h"
#include "msif/msif.h"
#include "msif/msif_auth.h"
#include "xerpi/display_test.h"

#include "ff.h"

/* Must live in .data (not .bss): the other cores spin on it while core 0
 * is still clearing .bss. */
uint32_t g_sync_point __attribute__((section(".data"))) = 0;

PayloadArguments *pargs;

void kscePervasiveMsifSetClock(int clock) {
  pervasive_msif_set_clock(clock);
}

/* Time base for every timeout and delay: the CPU's own cycle counter
 * (PMU). The original used the SoC timer at 0xe20b6000 as a 1 MHz clock;
 * that held on 3.65, but on 3.60 the payload never got past its first
 * delay, i.e. that timer does not run at 1 MHz there when the payload
 * runs. The cycle counter does not depend on how the OS left any timer.
 * Its rate is the CPU clock: the plugin passes the clock at the request
 * (PayloadArguments.cpu_mhz; PSV-Shell-Plus runs some consoles at 500);
 * without it JV_CPU_MHZ, the highest the system itself uses, so that at a
 * lower clock every wait only gets longer, never shorter. */
#define JV_CPU_MHZ 444
static uint32_t g_cpu_mhz = JV_CPU_MHZ;
#define SOC_TIMER  0xe20b6000

static uint32_t g_pmcr_saved, g_pmcnten_saved;
static uint32_t g_cyc_last, g_cyc_rem, g_us;

static inline uint32_t pmu_cycles(void) {
  uint32_t v;
  __asm__ volatile("mrc p15, 0, %0, c9, c13, 0" : "=r"(v)); /* PMCCNTR */
  return v;
}

static void pmu_start(void) {
  uint32_t v;
  __asm__ volatile("mrc p15, 0, %0, c9, c12, 0" : "=r"(g_pmcr_saved));
  __asm__ volatile("mrc p15, 0, %0, c9, c12, 1" : "=r"(g_pmcnten_saved));
  v = (g_pmcr_saved | 1) & ~(1u << 3);           /* enable, no /64 */
  __asm__ volatile("mcr p15, 0, %0, c9, c12, 0" :: "r"(v));
  __asm__ volatile("mcr p15, 0, %0, c9, c12, 1" :: "r"(1u << 31));
  __asm__ volatile("isb");
  g_cyc_last = pmu_cycles();
  g_cyc_rem = 0;
  g_us = 0;
}

static void pmu_stop(void) {
  /* give the OS the PMU back as it was */
  if (!(g_pmcnten_saved & (1u << 31)))
    __asm__ volatile("mcr p15, 0, %0, c9, c12, 2" :: "r"(1u << 31));
  __asm__ volatile("mcr p15, 0, %0, c9, c12, 0" :: "r"(g_pmcr_saved & ~6u));
  __asm__ volatile("isb");
}

/* Microseconds since pmu_start(), wrapping at 32 bits: compare differences
 * (jv_time_us() - start), never sums. Must be called at least every few
 * seconds (all wait loops call it continuously). */
uint32_t jv_time_us(void) {
  uint32_t c = pmu_cycles();
  g_cyc_rem += c - g_cyc_last;
  g_cyc_last = c;
  if (g_cyc_rem >= g_cpu_mhz) {
    g_us += g_cyc_rem / g_cpu_mhz;
    g_cyc_rem %= g_cpu_mhz;
  }
  return g_us;
}

/* Diagnostics: SoC timer ticks during ~10 ms of CPU time (about 10000 if
 * it really is a 1 MHz clock). Reported in the log. */
static uint32_t measure_soc_timer(void) {
  uint32_t t0 = REG32(SOC_TIMER);
  uint32_t start = jv_time_us();
  while (jv_time_us() - start < 10000)
    ;
  return REG32(SOC_TIMER) - t0;
}

DWORD get_fattime(void) {
  return pargs->fattime;
}

static void cdram_enable(void) {
  register uint32_t r12 __asm__("r12") = 0x117;
  __asm__ volatile("smc #0\n\t" :: "r"(r12));
}

static void l2_flush(void) {
  register uint32_t r0 __asm__("r0") = 1;
  register uint32_t r12 __asm__("r12") = 0x16a;
  __asm__ volatile("smc #0\n\t" : "+r"(r0) : "r"(r12));
}

static void delay_ms(uint32_t ms) {
  uint32_t start = jv_time_us();
  while (jv_time_us() - start < ms * 1000)
    ;
}

/* A load failed after the card or memory had already been modified. The
 * memory image is incomplete, so resuming the OS could make it write to
 * the card with inconsistent state. Stop here instead and blink the game
 * card LED; the user has to hold the power button to switch off. */
static void __attribute__((noreturn)) halt_blink(void) {
  guard_deny_all();
  while (1) {
    gpio_port_set(0, GPIO_PORT_GAMECARD_LED);
    delay_ms(150);
    gpio_port_clear(0, GPIO_PORT_GAMECARD_LED);
    delay_ms(150);
  }
}

/* ---- Persistent trace (JvTraceSector in JamaisVu.h) -------------------
 * One sector of data/JamaisVu/trace.bin, found and signed by the plugin.
 * It is only ever written after reading it back from the card with the
 * expected magic and nonce, so no other sector can be hit. This single
 * verified sector is the one write that does not go through the slot
 * guard (the guard covers the slot file). */
static uint32_t g_trace_lba; /* 0 = not found / not available */
static JvTraceSector g_trace_buf __attribute__((aligned(64)));
static uint8_t g_trace_mbr[512] __attribute__((aligned(64)));

static void trace_flush(void) {
  JvTraceSector *t = &g_trace_buf;

  if (!g_trace_lba)
    return;
  t->written = 1;
  t->mode = pargs->mode;
  t->debug_mode = pargs->debug_mode;
  t->slot = pargs->slot;
  t->result = pargs->result;
  t->detail = pargs->detail;
  t->stage = pargs->stage;
  t->soc_timer_10ms = pargs->soc_timer_10ms;
  t->trace_n = pargs->trace_n;
  memcpy(t->trace_stage, pargs->trace_stage, sizeof(t->trace_stage));
  memcpy(t->trace_ms, pargs->trace_ms, sizeof(t->trace_ms));
  t->fault_type = pargs->fault_type;
  t->fault_addr = pargs->fault_addr;
  t->fault_lr = pargs->fault_lr;
  ms_write_sector(t, (int)g_trace_lba, 1);
}

/* The plugin knows the sector relative to the exFAT partition; try the
 * card start and every partition in the MBR, and accept only the sector
 * that carries this run's magic + nonce. */
static int trace_locate(void) {
  const master_block_t *mbr = (const master_block_t *)g_trace_mbr;
  uint32_t rel = pargs->trace_rel_lba;

  if (rel == 0 || ms_read_sector(g_trace_mbr, 0, 1) != 0)
    return -1;

  for (int i = -1; i < 0x10; i++) {
    uint32_t base = i < 0 ? 0 : mbr->partitions[i].off;
    if (i >= 0 && base == 0)
      continue;
    if (base + rel < base)
      continue;
    if (ms_read_sector(&g_trace_buf, (int)(base + rel), 1) != 0)
      continue;
    if (memcmp(g_trace_buf.magic, JV_TRACE_MAGIC, 8) == 0 &&
        memcmp(g_trace_buf.nonce, pargs->trace_nonce, 16) == 0) {
      g_trace_lba = base + rel;
      return 0;
    }
  }
  return -1;
}

/* Diagnostics: record step n (a JV_STAGE_*) with a timestamp, in the
 * arguments (read by the plugin after resuming) and in the persistent
 * trace sector once the card is usable. */
static void screen_step(int n);

void stage_mark(int n) {
  if (n != JV_STAGE_DONE)
    pargs->stage = n; /* the last real step, reported if a run fails */
  if (pargs->trace_n < sizeof(pargs->trace_stage)) {
    pargs->trace_stage[pargs->trace_n] = (uint8_t)n;
    pargs->trace_ms[pargs->trace_n] = jv_time_us() / 1000;
    pargs->trace_n++;
  }
  trace_flush();
  if (jv_screen_is_on())
    screen_step(n);
}

static uint32_t g_old_vbar;
static volatile int g_in_exception;

void payload_cleanup(void);
void __attribute__((noreturn)) jv_recover(void); /* start.S */

/* Called from the exception vectors in start.S: the payload crashed.
 * Record the fault, then return to the OS like a normal failed run, so the
 * plugin can log it. Only a load that already changed memory or the card
 * cannot go back (the image is incomplete): blink the LED fast and wait
 * for the user to switch off. */
void __attribute__((noreturn)) jv_exception(uint32_t type, uint32_t far,
                                            uint32_t lr) {
  if (!g_in_exception) {
    g_in_exception = 1;
    pargs->fault_type = type;
    pargs->fault_addr = far;
    pargs->fault_lr = lr;
    pargs->result = JV_ERR_CRASH;
    pargs->detail = (int32_t)type;
    guard_deny_all();
    if (!g_ms_busy)
      trace_flush(); /* not while the card controller is mid-transfer */
    if (!(pargs->mode == JAMAISVU_MODE_LOAD && pargs->applied))
      jv_recover();
  }

  /* the LED may not be switched on yet if the fault came early */
  if (pargs->stage < JV_STAGE_SYSCON)
    syscon_init();
  syscon_common_write(0x80 | 0x40, 0x891, 2);
  gpio_set_port_mode(0, GPIO_PORT_GAMECARD_LED, GPIO_PORT_MODE_OUTPUT);
  while (1) {
    gpio_port_set(0, GPIO_PORT_GAMECARD_LED);
    for (volatile int i = 0; i < 200000; i++)
      ;
    gpio_port_clear(0, GPIO_PORT_GAMECARD_LED);
    for (volatile int i = 0; i < 200000; i++)
      ;
  }
}

extern char jv_vectors[];

static uint32_t vbar_get(void) {
  uint32_t v;
  __asm__ volatile("mrc p15, 0, %0, c12, c0, 0" : "=r"(v));
  return v;
}

/* Instruction cache, MMU still off (ARMv7: instruction fetches are then
 * cacheable). Data accesses stay uncached, so nothing changes for the
 * data the payload reads and writes. start.S left SCTLR = 0; the cache
 * is switched off and emptied again before the OS resumes, so it gets
 * SCTLR exactly as before. */
static void icache_on(void) {
  uint32_t v;
  __asm__ volatile("mcr p15, 0, %0, c7, c5, 0\n\tdsb\n\tisb" :: "r"(0));
  __asm__ volatile("mrc p15, 0, %0, c1, c0, 0" : "=r"(v));
  v |= 1u << 12; /* SCTLR.I */
  __asm__ volatile("mcr p15, 0, %0, c1, c0, 0\n\tisb" :: "r"(v));
}

static void icache_off(void) {
  uint32_t v;
  __asm__ volatile("mrc p15, 0, %0, c1, c0, 0" : "=r"(v));
  v &= ~(1u << 12);
  __asm__ volatile("mcr p15, 0, %0, c1, c0, 0\n\tisb" :: "r"(v));
  __asm__ volatile("mcr p15, 0, %0, c7, c5, 0\n\t" /* ICIALLU */
                   "mcr p15, 0, %0, c7, c5, 6\n\t" /* BPIALL */
                   "dsb\n\tisb" :: "r"(0));
}

static void vbar_set(uint32_t v) {
  __asm__ volatile("mcr p15, 0, %0, c12, c0, 0\n\tisb" :: "r"(v));
}

static void display_mark(int step) {
  stage_mark(JV_STAGE_DISP_I2C + step);
}

static void display_args(JvDisplayArgs *a) {
  a->fb_paddr = pargs->disp_fb_paddr;
  a->pitch = pargs->disp_fb_pitch;
  a->width = pargs->disp_fb_width;
  a->height = pargs->disp_fb_height;
  a->hw_info = pargs->disp_hw_info;
  a->hw_flags0 = pargs->disp_hw_flags0;
  a->vic = pargs->disp_vic;
  a->disp_type = pargs->disp_type;
}

/* Progress screen during a save: the bar follows the slot file position
 * (5..99 %), the line says how much memory is written. Called after every
 * chunk written, so it only draws when something visible changes. */
static uint32_t g_prog_total, g_prog_shown_mb = 0xffffffff;
static int g_prog_shown_pct = -1;

void progress_total(uint32_t total) {
  g_prog_total = total;
  pargs->mem_bytes = total;
  pargs->mem_ms = jv_time_us() / 1000; /* start; the end is subtracted */
}

static char *put_u(char *p, uint32_t v) {
  char tmp[11];
  int n = 0;
  do {
    tmp[n++] = (char)('0' + v % 10);
    v /= 10;
  } while (v);
  while (n)
    *p++ = tmp[--n];
  return p;
}

/* At most one progress update per second: the screen is drawn with the
 * menu's renderer, uncached (MMU off), and updating every MB made saves
 * ~10 s and loads ~13 s slower on hardware (log.txt, build bcac666). */
static int progress_due(void) {
  static uint32_t last;
  static int started;
  uint32_t now = jv_time_us();
  if (started && now - last < 1000000)
    return 0;
  started = 1;
  last = now;
  return 1;
}

void progress_at(uint32_t pos) {
  if (!jv_screen_is_on() || g_prog_total == 0)
    return;
  if (pos > g_prog_total)
    pos = g_prog_total;
  if (pos < g_prog_total && !progress_due())
    return;
  int pct = 5 + (int)((uint64_t)pos * 94 / g_prog_total);
  uint32_t mb = pos >> 20;
  if (pct == g_prog_shown_pct && mb == g_prog_shown_mb)
    return;
  static char line[64];
  char *p = line;
  const char *w = "Writing memory: ";
  while (*w)
    *p++ = *w++;
  p = put_u(p, mb);
  *p++ = ' ';
  *p++ = '/';
  *p++ = ' ';
  p = put_u(p, g_prog_total >> 20);
  w = " MB";
  while (*w)
    *p++ = *w++;
  *p = '\0';
  jv_screen_status(mb != g_prog_shown_mb ? line : NULL,
                   pct != g_prog_shown_pct ? pct : -1);
  g_prog_shown_pct = pct;
  g_prog_shown_mb = mb;
}

/* Progress screen: what each step means on screen, and how far along */
static int g_screen_save; /* a real save (not a dry run) */
static int g_screen_load; /* a load */

/* Loads: the screen moves to the snapshot's own frame when the memory
 * restore starts (the restore overwrites memory, including the frame on
 * screen; the snapshot's frame is display memory before and after). Once
 * the restore has passed that frame, it is drawn again completely. */
static uint32_t g_load_total, g_load_frame_end;
static int g_load_redrawn;

static char *put_u(char *p, uint32_t v);

void progress_load_begin(uint32_t total) {
  g_load_total = total;
  pargs->mem_bytes = total;
  pargs->mem_ms = jv_time_us() / 1000;
  if (!jv_screen_is_on())
    return;
  uint32_t y = pargs->disp_fb2_paddr, pitch = pargs->disp_fb2_pitch;
  if (y && jv_screen_switch(y, pitch) == 0) {
    g_load_frame_end = y + pitch * pargs->disp_fb_height * 4;
    g_load_redrawn = 0;
    jv_screen_status("Restoring memory", 5);
    return;
  }
  /* no frame of the snapshot known (older slot): nowhere safe to draw
   * while memory is replaced - dark until done */
  jv_screen_status("Restoring memory - the screen stays dark until done",
                   5);
  uint32_t t0 = jv_time_us();
  while (jv_time_us() - t0 < 1500000)
    ;
  jv_screen_end();
}

void progress_load_at(uint32_t paddr_end, uint32_t done) {
  static uint32_t shown_mb = 0xffffffff;
  if (!jv_screen_is_on() || g_load_total == 0)
    return;
  if (!g_load_redrawn && paddr_end >= g_load_frame_end) {
    jv_screen_redraw(); /* the restore just wrote over it */
    g_load_redrawn = 1;
    shown_mb = 0xffffffff;
  }
  uint32_t mb = done >> 20;
  if (mb == shown_mb || (done < g_load_total && !progress_due()))
    return;
  shown_mb = mb;
  static char line[64];
  char *p = line;
  const char *w = "Restoring memory: ";
  while (*w)
    *p++ = *w++;
  p = put_u(p, mb);
  *p++ = ' ';
  *p++ = '/';
  *p++ = ' ';
  p = put_u(p, g_load_total >> 20);
  w = " MB";
  while (*w)
    *p++ = *w++;
  *p = '\0';
  jv_screen_status(line, 5 + (int)((uint64_t)done * 94 / g_load_total));
}

static void screen_step(int n) {
  if (g_screen_load) {
    if (n == JV_STAGE_FS_MOUNT)
      jv_screen_status("Checking the slot", 3);
    return;
  }
  if (g_screen_save) {
    switch (n) {
      case JV_STAGE_FS_MOUNT:
        jv_screen_status("Reading the Memory Card", 2);
        break;
      case JV_STAGE_SLOT_FOUND:
        jv_screen_status("Slot file found", 3);
        break;
      case JV_STAGE_SLOT_TOKEN:
        jv_screen_status("Slot file checked", 4);
        break;
      case JV_STAGE_VERIFY:
        jv_screen_status("Written - reading back", 0);
        break;
      case JV_STAGE_WRITING:
        jv_screen_status("Writing savedata", 5);
        break;
    }
    return;
  }
  switch (n) {
    case JV_STAGE_FS_MOUNT:
      jv_screen_status("Reading the Memory Card", 45);
      break;
    case JV_STAGE_SLOT_FOUND:
      jv_screen_status("Slot file found", 65);
      break;
    case JV_STAGE_SLOT_TOKEN:
      jv_screen_status("Slot file checked", 85);
      break;
  }
}

/* holds the result on screen for a moment, then switches off */
static void screen_finish(int result) {
  if (!jv_screen_is_on())
    return;
  const char *msg = "Stopped - details in log.txt";
  if (result == JV_TEST_DONE)
    msg = pargs->debug_mode == JV_TEST_VERIFY
              ? "Write test done - details in log.txt"
              : "Dry run passed - nothing was written";
  else if (result == JV_OK && g_screen_save)
    msg = "Saved";
  else if (result == JV_OK && g_screen_load)
    msg = "Loaded";
  else if (g_screen_load && !pargs->applied)
    msg = "Not loaded - nothing was changed (details in log.txt)";
  jv_screen_status(msg, result >= 0 ? 100 : -1);
  uint32_t t0 = jv_time_us();
  while (jv_time_us() - t0 < 500000) /* the game shows its own message */
    ;
  jv_screen_end();
}

static int display_test(void) {
  JvDisplayArgs a;
  display_args(&a);
  int r = jv_display_test(&a, display_mark, jv_time_us);
  /* detail: hardware waits that timed out (0 = every device answered) */
  pargs->detail = (int32_t)jv_display_wait_timeouts();
  if (r < 0)
    return JV_ERR_ARGS;
  return JV_TEST_DONE;
}

static int run(void) {
  static FATFS fs;
  FRESULT fres;
  int res;

  res = msproal_start();
  if (res != 0) {
    pargs->detail = res;
    return JV_ERR_CARD;
  }
  stage_mark(JV_STAGE_MC_START);
  res = msproal_mount();
  if (res != 0) {
    pargs->detail = res;
    return JV_ERR_CARD;
  }
  stage_mark(JV_STAGE_MC_MOUNT);
  res = msif_auth(NULL);
  if (res != 0) {
    pargs->detail = res;
    return msif_auth_hw_fault() ? JV_ERR_HW : JV_ERR_CARD;
  }
  stage_mark(JV_STAGE_MC_AUTH);

  /* card usable: from here on every step is also saved to trace.bin */
  if (trace_locate() == 0) {
    gpio_port_clear(0, GPIO_PORT_GAMECARD_LED); /* "trace is recording" */
    trace_flush();
  }

  /* Diagnostics: the display test runs once trace.bin records every step
   * (if the display code hangs, the log still shows how far it got). It
   * needs no filesystem and writes nothing else. */
  if (pargs->debug_mode == JV_TEST_DISPLAY)
    return display_test();

  /* Progress screen (step 1: dry runs only) - from here on, so every
   * display step is in trace.bin too. If the frame is unusable it stays
   * off and the run goes on exactly as without it. */
  if (pargs->disp_fb_paddr && pargs->mode == JAMAISVU_MODE_SAVE &&
      (pargs->debug_mode == JV_TEST_DRYRUN ||
       pargs->debug_mode == JV_TEST_VERIFY ||
       pargs->debug_mode == JV_TEST_NONE)) {
    static char title[] = "Saving slot ?";
    JvDisplayArgs a;
    display_args(&a);
    g_screen_save = pargs->debug_mode == JV_TEST_NONE;
    title[sizeof(title) - 2] = (char)('1' + (pargs->slot % 9));
    const char *t = g_screen_save ? title
                    : pargs->debug_mode == JV_TEST_VERIFY
                          ? "Write test - saving, then reading back"
                          : "Dry run - nothing is written";
    if (jv_screen_begin(&a, display_mark, t) == 0)
      jv_screen_status("Memory Card ready", g_screen_save ? 1 : 30);
  } else if (pargs->disp_fb_paddr && pargs->mode == JAMAISVU_MODE_LOAD &&
             pargs->debug_mode == JV_TEST_NONE) {
    static char title[] = "Loading slot ?";
    JvDisplayArgs a;
    display_args(&a);
    g_screen_load = 1;
    title[sizeof(title) - 2] = (char)('1' + (pargs->slot % 9));
    if (jv_screen_begin(&a, display_mark, title) == 0)
      jv_screen_status("Memory Card ready", 1);
  }

  fres = f_mount(&fs, "ux0:", 1);
  if (fres != FR_OK) {
    pargs->detail = fres;
    return JV_ERR_CARD;
  }
  stage_mark(JV_STAGE_FS_MOUNT);

  if (pargs->mode == JAMAISVU_MODE_LOAD)
    res = load_state(&fs, pargs->path);
  else if (pargs->mode == JAMAISVU_MODE_SAVE)
    res = save_state(&fs, pargs->path);
  else
    res = JV_ERR_ARGS;

  f_unmount("ux0:");
  return res;
}

/* The cycle counter every timeout depends on. If it does not advance, a
 * missing answer from the hardware would wait forever, so give up. */
static int timer_running(void) {
  uint32_t c0 = pmu_cycles();
  for (int i = 0; i < 1000; i++)
    if (pmu_cycles() != c0)
      return 1;
  return 0;
}

static int payload_main(void) {
  pargs->result = JV_ERR_CARD;
  pargs->applied = 0;
  pargs->detail = 0;
  pargs->soc_timer_10ms = 0;
  pargs->trace_n = 0;
  if (pargs->refuse) {
    /* the plugin found at suspend that the snapshot would be incomplete:
     * nothing is touched, the reason goes back as detail */
    pargs->result = JV_ERR_ARGS;
    pargs->detail = (int32_t)pargs->refuse;
    return 0;
  }
  pargs->fault_type = 0;
  pargs->fault_addr = 0;
  pargs->fault_lr = 0;
  stage_mark(JV_STAGE_ENTRY);
  pargs->path[sizeof(pargs->path) - 1] = '\0';

  /* Diagnostics: hand control straight back, touching no hardware. Tests
   * the soft reset / resume path on its own. */
  if (pargs->debug_mode == JV_TEST_RESET) {
    pargs->result = JV_TEST_DONE;
    return 0;
  }

  /* No write may reach the card unless explicitly allowed. */
  guard_deny_all();

  if (!timer_running()) {
    pargs->result = JV_ERR_TIMER;
    return 0;
  }
  pargs->soc_timer_10ms = measure_soc_timer();

  l2_flush();
  stage_mark(JV_STAGE_L2_FLUSH);
  syscon_init();
  if (syscon_fault()) {
    pargs->detail = syscon_fault();
    pargs->result = JV_ERR_HW;
    return 0;
  }
  stage_mark(JV_STAGE_SYSCON);
  cdram_enable();
  stage_mark(JV_STAGE_CDRAM);

  // Turn on Game Card LED
  syscon_common_write(0x80 | 0x40, 0x891, 2);
  gpio_set_port_mode(0, GPIO_PORT_GAMECARD_LED, GPIO_PORT_MODE_OUTPUT);
  gpio_port_set(0, GPIO_PORT_GAMECARD_LED);
  /* The LED stays on until trace.bin is recording (after card
   * authentication) or the run ends. (It used to be held 1.5 s so it could
   * be counted during the 3.60 bring-up; trace.bin records every step now,
   * and the hold kept the screen black that much longer.) */

  syscon_msif_set_power(1);
  pervasive_reset_enter_msif();
  pervasive_msif_set_clock(4);
  pervasive_reset_exit_msif();
  pervasive_clock_enable_msif();
  if (syscon_fault()) {
    pargs->detail = syscon_fault();
    pargs->result = JV_ERR_HW;
  } else {
    stage_mark(JV_STAGE_MC_POWER);
    pargs->mem_bytes = 0;
    pargs->result = run();
    if (pargs->mem_bytes) /* start time -> duration */
      pargs->mem_ms = jv_time_us() / 1000 - pargs->mem_ms;
    if (pargs->mode == JAMAISVU_MODE_LOAD && pargs->applied &&
        pargs->result != JV_OK) {
      /* cannot go back (halt_blink below): the message stays up */
      jv_screen_status("Load failed - hold the POWER button to switch off",
                       -1);
    } else {
      screen_finish(pargs->result);
    }
  }

  guard_deny_all();
  stage_mark(JV_STAGE_DONE); /* saved to trace.bin while the card is on */
  pervasive_clock_disable_msif();
  syscon_msif_set_power(0);
  gpio_port_clear(0, GPIO_PORT_GAMECARD_LED);

  if (pargs->mode == JAMAISVU_MODE_LOAD && pargs->applied &&
      pargs->result != JV_OK)
    halt_blink();

  return 0;
}

int main(void) {
  uint32_t old_vbar;

  pargs = (PayloadArguments *)PAYLOAD_ARGS_PADDR;
  g_cpu_mhz = pargs->cpu_mhz >= 100 && pargs->cpu_mhz <= 2000
                  ? pargs->cpu_mhz : JV_CPU_MHZ;
  g_ms_write_max = pargs->write_sectors >= 1 && pargs->write_sectors <= 255
                       ? (int)pargs->write_sectors : 128;
  *(volatile uint32_t *)REGSNAP_PADDR = 0; /* filled by the PS TV test */
  g_in_exception = 0;
  g_trace_lba = 0;

  /* catch faults (see jv_exception) instead of hanging silently */
  old_vbar = vbar_get();
  g_old_vbar = old_vbar;
  vbar_set((uint32_t)jv_vectors);
  pmu_start();
  pargs->icache_on = 0;
  pargs->delay_unit_ns = jv_delay_calibrate(); /* before the cache is on */
  icache_on();
  pargs->icache_on = 1;
  payload_main();
  icache_off();
  pmu_stop();
  vbar_set(old_vbar);
  return 0;
}

/* After a crash (jv_recover in start.S, on a fresh stack): undo what
 * main() set up, then start.S resumes the OS exactly like after main(). */
void payload_cleanup(void) {
  guard_deny_all();
  if (jv_screen_is_on())
    jv_screen_end(); /* bounded: cannot hang */
  stage_mark(JV_STAGE_DONE);
  if (pargs->stage >= JV_STAGE_MC_POWER) {
    pervasive_clock_disable_msif();
    syscon_msif_set_power(0);
  }
  if (pargs->stage >= JV_STAGE_CDRAM)
    gpio_port_clear(0, GPIO_PORT_GAMECARD_LED);
  icache_off();
  pmu_stop();
  vbar_set(g_old_vbar);
}
