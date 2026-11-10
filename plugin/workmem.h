/* JamaisVu savestate plugin
 *
 * Memory for work that is done now and then - a buffer for reading files,
 * a picture carried across a save - taken when the work starts and given
 * back when it ends.
 *
 * Not from the kernel's own pool (ScePhyMemPartKD) where another has room:
 * on the PCH-1000 that pool had 2.9 MB of its 36 MB free when JamaisVu
 * started, 430 KB in the LiveArea and 200-260 KB with PCSE00120 running
 * (ce9b5f5), and the console's two kernel crashes ended with it empty.
 * JamaisVu's own code and fixed buffers sit in that pool for as long as
 * the console is on, so a buffer that is only needed for a moment is not
 * kept there.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __JV_WORKMEM_H__
#define __JV_WORKMEM_H__

#include <psp2kern/kernel/sysmem.h>
#include <stdint.h>

/* size bytes (rounded up to pages) from: root contiguous, user main,
 * common dialog, game - and, with kernel_too, the kernel's own pool last.
 * NULL (*uid < 0) when none has it. */
static inline void *jv_work_alloc(const char *name, uint32_t size,
                                  SceUID *uid, int kernel_too) {
  static const SceKernelMemBlockType types[] = {
    0x1080D006, /* KERNEL_ROOT_PHYCONT_RW */
    0x10C0D006, /* KERNEL_ROOT_UMAIN_RW */
    0x10A0D006, /* KERNEL_ROOT_CDIALOG_RW */
    0x1050D006, /* KERNEL_ROOT_GAME_RW */
    0x1020D006, /* KERNEL_ROOT_RW: the kernel's own */
  };
  int n = (int)(sizeof(types) / sizeof(types[0])) - (kernel_too ? 0 : 1);
  size = (size + 0xfff) & ~0xfffu;
  for (int i = 0; i < n; i++) {
    void *base = NULL;
    SceUID u = ksceKernelAllocMemBlock(name, types[i], size, NULL);
    if (u < 0)
      continue;
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
