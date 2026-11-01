@ JamaisVu savestate plugin
@
@ Copyright (C) 2020 TheFloW
@
@ This software may be modified and distributed under the terms
@ of the MIT license.  See the LICENSE file for details.
@

#include "../shared.h"

  .syntax unified
  .cpu cortex-a9
  .text
  .align 4

  .global _start
  .type _start, %function
_start:
  cpsid if
  clrex

  mov r0, #0
  bic r0, #0x40000000
  bic r0, #0x10000000
  bic r0, #0x20000000
  bic r0, #0x2000000
  bic r0, #0x2000
  bic r0, #0x1000
  bic r0, #0x800
  bic r0, #0x400
  bic r0, #2
  bic r0, #4
  bic r0, #1
  mcr p15, 0, r0, c1, c0, 0  @ [>] SCTLR (System Control Register)

  mov r0, #0
  mcr p15, 0, r0, c7, c1, 0  @ [>] ICIALLUIS (Instruction Cache Invalidate All to PoU, Inner Shareable)
  dsb
  mcr p15, 0, r0, c7, c1, 6  @ [>] BPIALLIS (Branch Predictor Invalidate All, Inner Shareable)
  isb
  mcr p15, 0, r0, c7, c5, 0  @ [>] ICIALLU (Instruction Cache Invalidate All to PoU)
  dsb

1:
  mov r1, #0
2:
  orr r2, r1, r0
  mcr p15, 0, r2, c7, c6, 2  @ [>] DCISW (Data Cache line Invalidate by Set/Way)
  add r1, #0x40000000
  cmp r1, #0
  bne 2b
  add r0, #0x20
  cmp r0, #0x2000
  bne 1b

  dsb
  mcr p15, 0, r0, c7, c5, 6  @ [>] BPIALL (Branch Predictor Invalidate All)
  isb
  mcr p15, 0, r0, c8, c3, 0  @ [>] TLBIALLIS (TLB Invalidate All, Inner Shareable)
  mov r0, #0
  mcr p15, 0, r0, c7, c5, 0  @ [>] ICIALLU (Instruction Cache Invalidate All to PoU)
  mov r0, #0
  mcr p15, 0, r0, c8, c7, 0  @ [>] TLBIALL (TLB Invalidate All)
  mcr p15, 0, r0, c2, c0, 2  @ [>] TTBCR (Translation Table Base Control Register)
  mov r0, #0
  mcr p15, 0, r0, c7, c5, 6  @ [>] BPIALL (Branch Predictor Invalidate All)
  isb

  mrc p15, 0, r0, c0, c0, 5
  and r0, #0xf

  cpsid aif, #0x1f
  ldr sp, =STACK_PADDR

  cmp r0, #0
  bne 1f

  @ Core 0: clear .bss. The resume code only copies .text/.data, so
  @ without this every zero-initialised global would hold stale memory.
  ldr r1, =__bss_start
  ldr r2, =__bss_end
  mov r3, #0
3:
  cmp r1, r2
  bhs 4f
  str r3, [r1], #4
  b 3b
4:
  dsb

  bl main

  .global jv_after_main
  .type jv_after_main, %function
jv_after_main:
1:
  ldr r1, =g_sync_point
  bl cpus_sync

  ldr lr, =SCE_RESUME_PADDR
  bx lr

@ Exception vectors for core 0 while main() runs (VBAR is pointed here and
@ restored afterwards). With the MMU off, the OS's own vectors are not
@ reachable, so a fault used to hang silently; now it ends in
@ jv_exception(), which blinks the game card LED fast.
  .align 5
  .global jv_vectors
jv_vectors:
  b jv_exc_reset
  b jv_exc_undef
  b jv_exc_swi
  b jv_exc_pabt
  b jv_exc_dabt
  b jv_exc_reset
  b jv_exc_irq
  b jv_exc_fiq

jv_exc_reset:
  mov r0, #0
  b jv_exc_common
jv_exc_undef:
  mov r0, #1
  b jv_exc_common
jv_exc_swi:
  mov r0, #2
  b jv_exc_common
jv_exc_pabt:
  mov r0, #3
  mrc p15, 0, r1, c6, c0, 2  @ IFAR
  b jv_exc_common2
jv_exc_dabt:
  mov r0, #4
  mrc p15, 0, r1, c6, c0, 0  @ DFAR
  b jv_exc_common2
jv_exc_irq:
  mov r0, #5
  b jv_exc_common
jv_exc_fiq:
  mov r0, #6
  b jv_exc_common
jv_exc_common:
  mov r1, #0
jv_exc_common2:
  mov r2, lr
  cpsid aif, #0x1f           @ SYS mode, interrupts off
  ldr sp, =jv_exc_stack_top
  bl jv_exception            @ never returns
3:
  b 3b

  .bss
  .align 3
jv_exc_stack:
  .space 2048
jv_exc_stack_top:
  .text

@ Back from a crash: fresh stack, undo main()'s setup, then resume the OS
@ exactly as after a normal return from main().
@ It is ARM code called from Thumb C (jv_exception): .type %function makes
@ the linker use BLX. Without it the call was a plain BL, the CPU ran this
@ ARM code in Thumb state and crashed again (seen on hardware as the LED
@ flashing after a crash).
  .global jv_recover
  .type jv_recover, %function
jv_recover:
  cpsid aif, #0x1f
  ldr sp, =STACK_PADDR
  bl payload_cleanup
  b jv_after_main

  .global cpus_sync
  .type cpus_sync, %function
cpus_sync:
  mrc p15, 0, r0, c0, c0, 5  @ [<] MPIDR (Multiprocessor Affinity Register)
  ands r0, #0xf
  cmp r0, #0
  it eq
  streq r0, [r1]

1:
  ldrb r2, [r1]
  cmp r0, r2
  itt ne
  wfene
  bne 1b
  ldrh r2, [r1]
  adds r2, #1
  adds r2, #0x100
  strh r2, [r1]
  dsb
  sev

1:
  ldrb r2, [r1, #1]
  cmp r2, #4
  itt ne
  wfene
  bne 1b

  bx lr
