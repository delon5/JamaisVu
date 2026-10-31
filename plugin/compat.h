/* JamaisVu savestate plugin
 *
 * Builds against both older and current VitaSDK headers (several kernel
 * headers moved and some functions were renamed in 2022/2023).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __JV_COMPAT_H__
#define __JV_COMPAT_H__

#include <stdint.h>
#include <psp2kern/kernel/sysmem.h>

#if __has_include(<psp2kern/kernel/syscon.h>)
#include <psp2kern/kernel/syscon.h>
#else
#include <psp2kern/syscon.h>
#endif

#if __has_include(<psp2kern/kernel/sysroot.h>)
#include <psp2kern/kernel/sysroot.h>
#endif

#if __has_include(<psp2kern/kernel/sysmem/data_transfers.h>)
#include <psp2kern/kernel/sysmem/data_transfers.h>
#endif

/* Old SDK: ksceKernelMemcpyUserToKernel(void *, uintptr_t, size_t) is a
 * function. New SDK: it is a macro for ksceKernelCopyFromUser(void *,
 * const void *, SceSize). */
static inline int jv_copy_from_user(void *dst, const void *src, SceSize len) {
#ifdef ksceKernelMemcpyUserToKernel
  return ksceKernelCopyFromUser(dst, src, len);
#else
  return ksceKernelMemcpyUserToKernel(dst, (uintptr_t)src, len);
#endif
}

static inline int jv_copy_to_user(void *dst, const void *src, SceSize len) {
#ifdef ksceKernelMemcpyKernelToUser
  return ksceKernelCopyToUser(dst, src, len);
#else
  return ksceKernelMemcpyKernelToUser((uintptr_t)dst, src, len);
#endif
}

#endif
