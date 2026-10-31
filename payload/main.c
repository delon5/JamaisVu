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

#include "ff.h"

/* Must live in .data (not .bss): the other cores spin on it while core 0
 * is still clearing .bss. */
uint32_t g_sync_point __attribute__((section(".data"))) = 0;

PayloadArguments *pargs;

void kscePervasiveMsifSetClock(int clock) {
  pervasive_msif_set_clock(clock);
}

uint64_t ksceKernelGetSystemTimeWide(void) {
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
  uint32_t start = (uint32_t)ksceKernelGetSystemTimeWide();
  while ((uint32_t)ksceKernelGetSystemTimeWide() - start < ms * 1000)
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

static int run(void) {
  static FATFS fs;
  int res;

  if (msproal_start() != 0)
    return JV_ERR_CARD;
  pargs->stage = JV_STAGE_MC_START;
  if (msproal_mount() != 0)
    return JV_ERR_CARD;
  pargs->stage = JV_STAGE_MC_MOUNT;

  if (f_mount(&fs, "ux0:", 1) != FR_OK)
    return JV_ERR_CARD;
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

int main(void) {
  pargs = (PayloadArguments *)PAYLOAD_ARGS_PADDR;
  pargs->result = JV_ERR_CARD;
  pargs->applied = 0;
  pargs->stage = JV_STAGE_ENTRY;
  pargs->path[sizeof(pargs->path) - 1] = '\0';

  /* Diagnostics: hand control straight back, touching no hardware. Tests
   * the soft reset / resume path on its own. */
  if (pargs->debug_mode == JV_TEST_RESET) {
    pargs->result = JV_TEST_DONE;
    return 0;
  }

  /* No write may reach the card unless explicitly allowed. */
  guard_deny_all();

  l2_flush();
  pargs->stage = JV_STAGE_L2_FLUSH;
  syscon_init();
  pargs->stage = JV_STAGE_SYSCON;
  cdram_enable();
  pargs->stage = JV_STAGE_CDRAM;

  // Turn on Game Card LED
  syscon_common_write(0x80 | 0x40, 0x891, 2);
  gpio_set_port_mode(0, GPIO_PORT_GAMECARD_LED, GPIO_PORT_MODE_OUTPUT);
  gpio_port_set(0, GPIO_PORT_GAMECARD_LED);

  syscon_msif_set_power(1);
  pervasive_reset_enter_msif();
  pervasive_msif_set_clock(4);
  pervasive_reset_exit_msif();
  pervasive_clock_enable_msif();
  pargs->stage = JV_STAGE_MC_POWER;

  pargs->result = run();

  guard_deny_all();
  pervasive_clock_disable_msif();
  syscon_msif_set_power(0);

  if (pargs->mode == JAMAISVU_MODE_LOAD && pargs->applied &&
      pargs->result != JV_OK)
    halt_blink();

  return 0;
}
