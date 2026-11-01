/* JamaisVu savestate plugin
 *
 * Lock screen skip for the wake-up after a save or load.
 *
 * A save or load suspends and resumes the system, and on resume SceShell
 * shows the lock screen. NoLockScreen (VitaTweaks, TheFloW) removes it for
 * good by replacing two instructions in SceShell with "movs r0, #1; nop".
 * JamaisVu uses the same two locations, but only around its own suspend:
 * applied just before the reset is requested, and removed ~10 s after the
 * system is back, so ordinary sleep/wake still shows the lock screen.
 *
 * After a load the running system is the snapshot, which was taken with
 * the patch in place (it is applied before a save, too) and whose
 * JamaisVu still holds the patch handles; it removes them the same way.
 *
 * Only exact SceShell builds are patched (by module NID); on any other
 * the lock screen simply appears as before. Nothing is written to any
 * storage: this changes SceShell's code in memory only.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <psp2kern/kernel/sysroot.h>
#include <psp2kern/kernel/sysmem.h>
#include <psp2kern/kernel/threadmgr.h>
#include <taihen.h>

#include <stdio.h>
#include <stdint.h>

#include "lockskip.h"

typedef struct {
  uint32_t nid;
  uint32_t off[2];
  const char *name;
} ShellBuild;

/* retail SceShell builds and offsets, as in NoLockScreen */
static const ShellBuild k_builds[] = {
  { 0x0552F692, { 0x2354D2, 0x2361D2 }, "3.60" },
  { 0x5549BF1F, { 0x23556E, 0x23626E }, "3.65" },
  { 0x34B4D82E, { 0x23556E, 0x23626E }, "3.67" },
  { 0x12DAC0F3, { 0x23556E, 0x23626E }, "3.68" },
  { 0x0703C828, { 0x23556E, 0x23626E }, "3.69" },
  { 0x2053B5A5, { 0x23556E, 0x23626E }, "3.70" },
  { 0xF476E785, { 0x23556E, 0x23626E }, "3.71" },
  { 0x939FFBE9, { 0x23556E, 0x23626E }, "3.72" },
  { 0x734D476A, { 0x23556E, 0x23626E }, "3.73" },
  { 0x51CB6207, { 0x23556E, 0x23626E }, "3.74" },
};

static SceUID g_inject[2] = { -1, -1 };
static SceUID g_lock_mtx = -1;

static void lock(void) {
  if (g_lock_mtx < 0)
    g_lock_mtx = ksceKernelCreateMutex("JamaisVuLockSkip", 0, 0, NULL);
  if (g_lock_mtx >= 0)
    ksceKernelLockMutex(g_lock_mtx, 1, NULL);
}

static void unlock(void) {
  if (g_lock_mtx >= 0)
    ksceKernelUnlockMutex(g_lock_mtx, 1);
}

static void release_locked(void) {
  for (int i = 1; i >= 0; i--) {
    if (g_inject[i] >= 0)
      taiInjectReleaseForKernel(g_inject[i]);
    g_inject[i] = -1;
  }
}

int lockskip_apply(char *out, int len) {
  static const uint32_t movs_r0_1_nop = 0xBF002001;
  tai_module_info_t info;
  int res = -1;

  lock();
  release_locked(); /* never stack patches */

  SceUID pid = ksceKernelSysrootGetShellPid();
  info.size = sizeof(info);
  if (pid < 0 || taiGetModuleInfoForKernel(pid, "SceShell", &info) < 0) {
    snprintf(out, len, "lock screen skip: SceShell not found");
    goto done;
  }

  const ShellBuild *b = NULL;
  for (unsigned i = 0; i < sizeof(k_builds) / sizeof(k_builds[0]); i++)
    if (k_builds[i].nid == info.module_nid)
      b = &k_builds[i];
  if (!b) {
    snprintf(out, len, "lock screen skip: unknown SceShell 0x%08X - the lock "
             "screen will show", (unsigned)info.module_nid);
    goto done;
  }

  /* For a user process taiHEN reports the module ID as the process sees
   * it; injecting needs the kernel's ID for it (taiHEN's own user-side
   * taiInjectData converts it the same way). Without this: 0x8002D082
   * (LIBRARYDB_NO_MOD), seen in log.txt on a 3.60 PCH-2000. */
  SceUID kmodid = ksceKernelKernelUidForUserUid(pid, info.modid);
  if (kmodid < 0) {
    snprintf(out, len, "lock screen skip: SceShell module ID not found "
             "(0x%08X)", (unsigned)kmodid);
    goto done;
  }

  for (int i = 0; i < 2; i++)
    g_inject[i] = taiInjectDataForKernel(pid, kmodid, 0, b->off[i],
                                         &movs_r0_1_nop,
                                         sizeof(movs_r0_1_nop));
  if (g_inject[0] < 0 || g_inject[1] < 0) {
    int exists = g_inject[0] == (SceUID)0x90010005 ||
                 g_inject[1] == (SceUID)0x90010005; /* TAI_ERROR_PATCH_EXISTS */
    snprintf(out, len, "lock screen skip: not applied (0x%08X/0x%08X)%s",
             (unsigned)g_inject[0], (unsigned)g_inject[1],
             exists ? " - already patched (NoLockScreen installed?)" : "");
    release_locked();
    goto done;
  }
  snprintf(out, len, "lock screen skip: on (SceShell %s)", b->name);
  res = 0;

done:
  unlock();
  return res;
}

void lockskip_release(void) {
  lock();
  release_locked();
  unlock();
}
