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

/* Free-running 32-bit microsecond counter: it wraps every ~71 minutes, so
 * all timeouts compare differences (jv_time_us() - start), never sums. */
uint32_t jv_time_us(void) {
  return REG32(0xe20b6000);
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

/* The microsecond timer every timeout depends on. If it does not advance,
 * a missing answer from the hardware would wait forever, so give up. */
static int timer_running(void) {
  uint32_t t0 = jv_time_us();
  for (int i = 0; i < 1000000; i++)
    if (jv_time_us() != t0)
      return 1;
  return 0;
}

static int payload_main(void) {
  pargs->result = JV_ERR_CARD;
  pargs->applied = 0;
  pargs->stage = JV_STAGE_ENTRY;
  pargs->detail = 0;
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
  payload_main();
  vbar_set(old_vbar);
  return 0;
}
