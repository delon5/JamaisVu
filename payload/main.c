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
 * Its rate is the CPU clock; JV_CPU_MHZ is the highest the Vita uses, so
 * at a lower clock every wait only gets longer, never shorter. */
#define JV_CPU_MHZ 444
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
  if (g_cyc_rem >= JV_CPU_MHZ) {
    g_us += g_cyc_rem / JV_CPU_MHZ;
    g_cyc_rem %= JV_CPU_MHZ;
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

/* Diagnostics (dry run only): the game card LED shows which step is
 * running, so a hang can be located without any log. Before step n the
 * LED goes off briefly, blinks n times, then stays on during the step.
 * The last count seen before the LED stays on for good is the step that
 * hung. */
void stage_mark(int n) {
  if (pargs->debug_mode != JV_TEST_DRYRUN)
    return;
  gpio_port_clear(0, GPIO_PORT_GAMECARD_LED);
  delay_ms(900);
  for (int i = 0; i < n; i++) {
    gpio_port_set(0, GPIO_PORT_GAMECARD_LED);
    delay_ms(250);
    gpio_port_clear(0, GPIO_PORT_GAMECARD_LED);
    delay_ms(300);
  }
  delay_ms(300);
  gpio_port_set(0, GPIO_PORT_GAMECARD_LED);
}

/* Called from the exception vectors in start.S: the payload crashed. It
 * cannot return safely, so blink the LED fast until the user switches
 * off. */
void __attribute__((noreturn)) jv_exception(uint32_t type, uint32_t far,
                                            uint32_t lr) {
  pargs->detail = (int32_t)(0xE0000000u | (type << 24) | (far & 0xffffff));
  (void)lr;
  guard_deny_all();
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

static void vbar_set(uint32_t v) {
  __asm__ volatile("mcr p15, 0, %0, c12, c0, 0\n\tisb" :: "r"(v));
}

static int run(void) {
  static FATFS fs;
  FRESULT fres;
  int res;

  stage_mark(2);
  res = msproal_start();
  if (res != 0) {
    pargs->detail = res;
    return JV_ERR_CARD;
  }
  pargs->stage = JV_STAGE_MC_START;
  stage_mark(3);
  res = msproal_mount();
  if (res != 0) {
    pargs->detail = res;
    return JV_ERR_CARD;
  }
  pargs->stage = JV_STAGE_MC_MOUNT;
  stage_mark(4);
  res = msif_auth(NULL);
  if (res != 0) {
    pargs->detail = res;
    return msif_auth_hw_fault() ? JV_ERR_HW : JV_ERR_CARD;
  }
  pargs->stage = JV_STAGE_MC_AUTH;

  stage_mark(5);
  fres = f_mount(&fs, "ux0:", 1);
  if (fres != FR_OK) {
    pargs->detail = fres;
    return JV_ERR_CARD;
  }
  pargs->stage = JV_STAGE_FS_MOUNT;

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
  pargs->stage = JV_STAGE_ENTRY;
  pargs->detail = 0;
  pargs->soc_timer_10ms = 0;
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
  pargs->stage = JV_STAGE_L2_FLUSH;
  syscon_init();
  if (syscon_fault()) {
    pargs->detail = syscon_fault();
    pargs->result = JV_ERR_HW;
    return 0;
  }
  pargs->stage = JV_STAGE_SYSCON;
  cdram_enable();
  pargs->stage = JV_STAGE_CDRAM;

  // Turn on Game Card LED
  syscon_common_write(0x80 | 0x40, 0x891, 2);
  gpio_set_port_mode(0, GPIO_PORT_GAMECARD_LED, GPIO_PORT_MODE_OUTPUT);
  gpio_port_set(0, GPIO_PORT_GAMECARD_LED);

  stage_mark(1);
  syscon_msif_set_power(1);
  pervasive_reset_enter_msif();
  pervasive_msif_set_clock(4);
  pervasive_reset_exit_msif();
  pervasive_clock_enable_msif();
  if (syscon_fault()) {
    pargs->detail = syscon_fault();
    pargs->result = JV_ERR_HW;
  } else {
    pargs->stage = JV_STAGE_MC_POWER;
    pargs->result = run();
  }

  guard_deny_all();
  pervasive_clock_disable_msif();
  syscon_msif_set_power(0);

  if (pargs->mode == JAMAISVU_MODE_LOAD && pargs->applied &&
      pargs->result != JV_OK)
    halt_blink();

  return 0;
}

int main(void) {
  uint32_t old_vbar;

  pargs = (PayloadArguments *)PAYLOAD_ARGS_PADDR;

  /* catch faults (see jv_exception) instead of hanging silently */
  old_vbar = vbar_get();
  vbar_set((uint32_t)jv_vectors);
  pmu_start();
  payload_main();
  pmu_stop();
  vbar_set(old_vbar);
  return 0;
}
