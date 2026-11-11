/* JamaisVu savestate plugin
 *
 * Memory for work that is done now and then - a buffer for reading files,
 * a picture carried across a save - taken when the work starts and given
 * back when it ends.
 *
 * The kernel's own pool (ScePhyMemPartKD) had 2.9 MB of its 36 MB free
 * on the PCH-1000 when JamaisVu started, 430 KB in the LiveArea and
 * 200-260 KB with PCSE00120 running (ce9b5f5), and that console's two
 * kernel crashes ended with it empty. JamaisVu's own code and fixed
 * buffers sit in that pool for as long as the console is on, so a buffer
 * that is only needed for a moment is not a fixed one - it is taken where
 * taking it hurts least at that moment (see jv_work_alloc).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __JV_WORKMEM_H__
#define __JV_WORKMEM_H__

#include <psp2kern/kernel/sysmem.h>
#include <stdint.h>

static inline int jv_work_pool_has(SceKernelMemBlockType type,
                                   uint32_t size) {
  SceUID u = ksceKernelAllocMemBlock("JamaisVuProbe", type, size, NULL);
  if (u < 0)
    return 0;
  ksceKernelFreeMemBlock(u);
  return 1;
}

/* size bytes (rounded up to pages), from, in this order:
 *  - the system's spare memory, outside every game's budget, while it
 *    has 8 MB more to give; the common dialog pool, while it has 4 MB
 *    more (a game touches it only while a system dialog is open);
 *  - the kernel's own pool, while half a megabyte more stays free in it
 *    (plenty on a console with few plugins; on the PCH-1000 it rarely
 *    has that, and the next ones are used);
 *  - user main, common dialog, game;
 *  - root contiguous, last of the game's pools and only while it keeps a
 *    2 MB block if it had one: that is where a game has its frame-sized
 *    buffers, and Dead Nation ended within 3 s whenever JamaisVu held
 *    part of a block it had given up for a moment (menu.c, HOLE_BYTES).
 *    It used to be asked first;
 *  - with kernel_too, the kernel's pool whatever is left in it.
 * NULL (*uid < 0) when none has it. */
static inline void *jv_work_alloc(const char *name, uint32_t size,
                                  SceUID *uid, int kernel_too) {
  /* first of all the system's spare memory - outside every game's budget
   * (menu.c, SPARE_POOL) - while it has 8 MB more to give */
  size = (size + 0xfff) & ~0xfffu;
  for (int k = 0; k < 2; k++) {
    /* then the common dialog pool, while it has 4 MB more to give */
    SceKernelMemBlockType t = k ? 0x10A0D006 : 0x1050D006;
    void *sb = NULL;
    SceUID su = ksceKernelAllocMemBlock(name, t, size, NULL);
    if (su >= 0) {
      if (jv_work_pool_has(t, k ? 0x400000 : 0x800000) &&
          ksceKernelGetMemBlockBase(su, &sb) >= 0 && sb) {
        *uid = su;
        return sb;
      }
      ksceKernelFreeMemBlock(su);
    }
  }
  static const SceKernelMemBlockType types[] = {
    0x1020D006, /* KERNEL_ROOT_RW: the kernel's own, with the margin */
    0x10C0D006, /* KERNEL_ROOT_UMAIN_RW */
    0x10A0D006, /* KERNEL_ROOT_CDIALOG_RW */
    0x1050D006, /* KERNEL_ROOT_GAME_RW */
    0x1080D006, /* KERNEL_ROOT_PHYCONT_RW */
    0x1020D006, /* KERNEL_ROOT_RW again: whatever is left */
  };
  int n = (int)(sizeof(types) / sizeof(types[0])) - (kernel_too ? 0 : 1);
  for (int i = 0; i < n; i++) {
    void *base = NULL;
    int had2 = i == 4 ? jv_work_pool_has(types[i], 0x200000) : 0;
    if (i == 4 && !had2)
      continue; /* the game has it full */
    SceUID u = ksceKernelAllocMemBlock(name, types[i], size, NULL);
    if (u < 0)
      continue;
    if ((i == 0 && !jv_work_pool_has(types[i], 0x80000)) ||
        (i == 4 && !jv_work_pool_has(types[i], 0x200000))) {
      ksceKernelFreeMemBlock(u);
      continue;
    }
    if (ksceKernelGetMemBlockBase(u, &base) >= 0 && base) {
      *uid = u;
      return base;
    }
    ksceKernelFreeMemBlock(u);
  }
  *uid = -1;
  return NULL;
}

static inline void jv_work_free(SceUID *uid) {
  if (*uid >= 0)
    ksceKernelFreeMemBlock(*uid);
  *uid = -1;
}

#endif
