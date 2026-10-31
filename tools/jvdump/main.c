/* jvdump - dumps the system modules JamaisVu needs offsets from.
 *
 * Read-only: it hooks nothing and patches nothing. At boot it waits for
 * ux0, then writes the loaded (already decrypted) code and data segments
 * of a few kernel modules to ux0:data/JamaisVu/dump/, together with
 * info.txt (module NIDs, segment addresses and sizes), and finally
 * done.txt. Remove it from config.txt afterwards.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <psp2kern/kernel/modulemgr.h>
#include <psp2kern/kernel/sysmem.h>
#include <psp2kern/kernel/threadmgr.h>
#include <psp2kern/io/fcntl.h>
#include <psp2kern/io/stat.h>
#include <taihen.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define DUMP_DIR "ux0:data/JamaisVu/dump"

int module_get_offset(SceUID pid, SceUID modid, int segidx, size_t offset,
                      uintptr_t *addr);

static const char *const g_modules[] = {
  "SceExfatfs",
  "SceIofilemgr",
  "SceRtc",
  "SceSysmem",
  "SceCtrl",
};

static char g_info[4096];
static int g_info_len;

static void info(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void info(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(g_info + g_info_len, sizeof(g_info) - g_info_len, fmt, ap);
  va_end(ap);
  if (n > 0 && g_info_len + n < (int)sizeof(g_info))
    g_info_len += n;
}

static int write_file(const char *path, const void *buf, uint32_t size) {
  SceUID fd = ksceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
  if (fd < 0)
    return fd;
  int res = 0;
  const uint8_t *p = buf;
  while (size > 0) {
    uint32_t n = size > 0x10000 ? 0x10000 : size;
    if (ksceIoWrite(fd, p, n) != (int)n) {
      res = -1;
      break;
    }
    p += n;
    size -= n;
  }
  ksceIoClose(fd);
  return res;
}

/* taiHEN's module_get_offset() accepts offsets up to the segment's memsz,
 * so the size can be found by bisection without SDK-version-specific
 * module info structures. */
static uint32_t segment_size(SceUID modid, int seg) {
  uintptr_t addr;
  uint32_t lo = 0, hi = 0x01000000; /* 16 MiB upper bound */
  if (module_get_offset(KERNEL_PID, modid, seg, 0, &addr) < 0)
    return 0;
  while (lo < hi) {
    uint32_t mid = lo + (hi - lo + 1) / 2;
    if (module_get_offset(KERNEL_PID, modid, seg, mid, &addr) >= 0)
      lo = mid;
    else
      hi = mid - 1;
  }
  return lo;
}

static void dump_module(const char *name) {
  tai_module_info_t ti;
  static char path[128];

  memset(&ti, 0, sizeof(ti));
  ti.size = sizeof(ti);
  if (taiGetModuleInfoForKernel(KERNEL_PID, name, &ti) < 0) {
    info("%s: not found\n", name);
    return;
  }

  info("%s: module_nid=0x%08X modid=0x%08X\n", name,
       (unsigned)ti.module_nid, (unsigned)ti.modid);

  for (int seg = 0; seg < 2; seg++) {
    uintptr_t base;
    if (module_get_offset(KERNEL_PID, ti.modid, seg, 0, &base) < 0) {
      info("  seg%d: none\n", seg);
      continue;
    }
    uint32_t size = segment_size(ti.modid, seg);
    snprintf(path, sizeof(path), DUMP_DIR "/%s_%08X_seg%d.bin", name,
             (unsigned)ti.module_nid, seg);
    int res = size ? write_file(path, (const void *)base, size) : -1;
    info("  seg%d: vaddr=0x%08X size=0x%X %s\n", seg, (unsigned)base,
         (unsigned)size, res < 0 ? "WRITE FAILED" : "ok");
  }
}

static int dump_thread(SceSize args, void *argp) {
  SceIoStat st;

  for (int i = 0; i < 120; i++) { /* wait up to 2 minutes for ux0 */
    if (ksceIoGetstat("ux0:", &st) >= 0)
      break;
    ksceKernelDelayThread(1000 * 1000);
  }
  ksceKernelDelayThread(3 * 1000 * 1000);

  ksceIoMkdir("ux0:data", 0777);
  ksceIoMkdir("ux0:data/JamaisVu", 0777);
  ksceIoMkdir(DUMP_DIR, 0777);
  ksceIoRemove(DUMP_DIR "/done.txt");

  info("jvdump 1.0\n");
  for (size_t i = 0; i < sizeof(g_modules) / sizeof(g_modules[0]); i++)
    dump_module(g_modules[i]);

  write_file(DUMP_DIR "/info.txt", g_info, g_info_len);
  write_file(DUMP_DIR "/done.txt", "done\n", 5);
  ksceIoSync("ux0:", 0);
  return 0;
}

void _start() __attribute__((weak, alias("module_start")));
int module_start(SceSize args, void *argp) {
  SceUID thid = ksceKernelCreateThread("jvdump", dump_thread, 0x64, 0x4000, 0,
                                       0x10000, NULL);
  if (thid >= 0)
    ksceKernelStartThread(thid, 0, NULL);
  return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize args, void *argp) {
  return SCE_KERNEL_STOP_SUCCESS;
}
