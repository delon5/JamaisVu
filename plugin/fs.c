/* JamaisVu savestate plugin
 *
 * Copyright (C) 2020 TheFloW
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <psp2kern/kernel/threadmgr.h>
#include <psp2kern/io/fcntl.h>
#include <taihen.h>

#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#include "main.h"
#include "trace.h"
#include "fs.h"
#include "prof.h"

int ksceVopInactive(void *vp);
/* Resolved at runtime: its stub name differs between VitaSDK versions
 * (ksceVfsFreeVnode / vfsFreeVnode). */
static int (* vfs_free_vnode)(void *vp);

static int (* UVFAT_Mount)(void *sharedResource, const char *assignName,
                           int mode, uint32_t *args);
static int (* UVFAT_SearchFile_common)(void *sharedResource, void *file,
                                       void *fileSystem, void *fnode,
                                       void *parent_fnode);
static void *(* UVFAT_GetParentFnode)(void *sharedResource, void *fileSystem,
                                      const char *path);
static int (* UVFAT_ReleaseFnode)(void *sharedResource, void *fnode);
static int (* cleanup_cluster_cache_area)(void *drive);
static int (* invalidate_cluster_cache_data)(void *fnode);
static int (* FreeFpCache)(void *filep);
static int (* sfree)(void *p, int size);

static int (* clear_ncache)(void *nc, int unk);
static int (* invalidate_buff_cache)(void *file, void *vp);

static uint32_t *g_num_vnodes, *g_vnode_size;
static uintptr_t *g_vnodes;

static void *g_shared_resource;
static const FwOffsets *g_off;

/* Write blocking for the exFAT driver (see fs.h). Must stay a plain
 * 32-bit global: the payload sets it through its physical address inside
 * the restored memory image. */
volatile uint32_t g_ignore_writedevice = FS_WRITES_ALLOWED;

static tai_hook_ref_t UVFAT_WriteDevice_Ref;
static SceUID g_write_hook = -1;
static SceUID g_mgmt_patch = -1;
static SceUID g_exfat_modid = -1;

#define SCE_ERROR_ERRNO_EROFS 0x8001001E

/* FS_WRITES_READ_ONLY applies to these drives only (bit = file system
 * index): the ones with open files that could not be looked up again.
 * Before, it applied to every exFAT drive - the system's own ur0/vd0/tm0
 * too - and after loads on the PCH-1000 (an open file on one drive could
 * not be looked up) even log.txt on the card was refused, then the
 * console restarted. */
static volatile uint32_t g_ro_mask;

/* file system index of a fileSystem (or of its drive), -1 if neither */
static int fs_index(const void *p) {
  if (!g_shared_resource || !g_off || !p)
    return -1;
  uintptr_t base = (uintptr_t)g_shared_resource;
  uintptr_t a = (uintptr_t)p;
  if (a >= base && (a - base) % g_off->fs_stride == 0 &&
      (a - base) / g_off->fs_stride < g_off->fs_count)
    return (int)((a - base) / g_off->fs_stride);
  return -1;
}

static int fs_index_any(const void *p) {
  int i = fs_index(p);
  if (i >= 0 || !g_off)
    return i;
  for (uint32_t k = 0; k < g_off->fs_count && k < 32; k++) {
    void *fsys = (void *)(g_shared_resource + g_off->fs_stride * k);
    if (*(uintptr_t **)(fsys + 0xac) && *(void **)(fsys + 0xac) == p)
      return (int)k;
  }
  return -1;
}

static JvWakeFsDrive *fs_rec(int i) {
  return (i >= 0 && i < JV_WAKE_FS_MAX) ? &trace_wake_fs()->drive[i] : NULL;
}

static void invalidate_file_caches(void) {
  uint32_t num = *(uint32_t *)(g_shared_resource + g_off->sr_num_files);
  void *files = *(uintptr_t **)(g_shared_resource + g_off->sr_files);
  for (uint32_t i = 0; i < num; i++) {
    FileCache *filep = (FileCache *)(files + 0x38 * i);
    if (filep->fnode) {
      FreeFpCache(filep);
      filep->Cluster = -2;
      filep->ClusterIndex = -1;
      filep->CacheIndex = -1;
      filep->CachedCluster = -2;
      filep->unk_34 = -1;
    }
  }
}

/* Returns < 0 if the file no longer exists, FNODE_UNUSED for empty slots */
#define FNODE_UNUSED 1

/* Which fnodes update_vnodes() has already looked up again: update_fnodes()
 * skips them instead of repeating the same lookup on the card (3.60 log,
 * build 3ddc43d: 1024 vnodes 2091 ms, then 559 fnodes 1906 ms, while the
 * game waits for the wake-up to finish). Indices beyond the bitmap are
 * simply looked up twice, as before. */
#define FNODE_DONE_MAX 4096
static uint32_t g_fnode_done[FNODE_DONE_MAX / 32];

static int fnode_index(const void *fnode) {
  uintptr_t base = (uintptr_t)g_shared_resource + g_off->sr_fnodes;
  uintptr_t p = (uintptr_t)fnode;
  if (p < base || (p - base) % g_off->fnode_size)
    return -1;
  uint32_t i = (p - base) / g_off->fnode_size;
  int num = *(int *)(g_shared_resource + g_off->sr_num_fnodes);
  if (num < 0 || i >= (uint32_t)num || i >= FNODE_DONE_MAX)
    return -1;
  return (int)i;
}

/* *looked_up = 1 once the file was searched on the card again and its
 * cluster caches were reset (not if the drive lock failed). */
static int update_fnode(void *fnode, int *looked_up) {
  int res;

  *looked_up = 0;

  void *fileSystem = *(uintptr_t **)(fnode + 0x20c);
  if (!fileSystem)
    return FNODE_UNUSED;

  void *drive = *(uintptr_t **)(fileSystem + 0xac);
  int fsi = fs_index(fileSystem);
  JvWakeFsDrive *rec = fs_rec(fsi);
  int64_t t0 = ksceKernelGetSystemTimeWide();

  res = ksceKernelLockFastMutex(drive + 0x1e0);
  if (res < 0)
    return res;

  static SearchFile file;
  memcpy(file.path, fnode, 0x208);

  file.isRoot = *(uint16_t *)(file.path + 0x00) == '/' &&
                *(uint16_t *)(file.path + 0x02) == '\0';

  void **pFileSystem = fileSystem;
  void *temp = UVFAT_GetParentFnode(g_shared_resource, fileSystem, file.path);
  res = UVFAT_SearchFile_common(g_shared_resource, &file, &pFileSystem, fnode,
                                temp);
  UVFAT_ReleaseFnode(g_shared_resource, temp);

  invalidate_cluster_cache_data(fnode);
  *(uint32_t *)(fnode + 0x280) = -2;
  *(uint32_t *)(fnode + 0x284) = -2;
  *(uint32_t *)(fnode + 0x288) = 0;
  *(uint32_t *)(fnode + 0x28c) = 0;

  ksceKernelUnlockFastMutex(drive + 0x1e0);
  *looked_up = 1;

  if (rec) {
    uint32_t ms = rec->ms + (uint32_t)((ksceKernelGetSystemTimeWide() - t0) /
                                       1000);
    rec->ms = ms > 0xFFFF ? 0xFFFF : (uint16_t)ms;
    if (rec->lookups < 255)
      rec->lookups++;
    if (res < 0 && rec->fails < 255)
      rec->fails++;
  }
  if (res < 0) {
    JvWakeFs *f = trace_wake_fs();
    if (!f->first_err) {
      f->first_err = res;
      f->first_err_drive = (uint8_t)(fsi < 0 ? 0xFF : fsi);
    }
  }

  return res;
}

/* returns how many fnodes were looked up again on the card; *skipped:
 * how many update_vnodes() had already done */
static uint32_t update_fnodes(uint32_t *skipped) {
  uint32_t used = 0;
  int looked_up;
  int num = *(int *)(g_shared_resource + g_off->sr_num_fnodes);
  *skipped = 0;
  for (int i = 0; i < num; i++) {
    if (i < FNODE_DONE_MAX && (g_fnode_done[i / 32] & (1u << (i % 32)))) {
      (*skipped)++;
      continue;
    }
    void *fnode = (void *)(g_shared_resource + g_off->sr_fnodes +
                           i * g_off->fnode_size);
    if (update_fnode(fnode, &looked_up) != FNODE_UNUSED)
      used++;
  }
  return used;
}

/* Returns the number of files that are still open but no longer exist on
 * the card. Their cached cluster chains are stale; writing through them
 * could overwrite other files. */
static int update_vnodes(void) {
  int stale_open = 0;
  int num = *g_num_vnodes;
  for (int i = 0; i < num; i++) {
    void *vp = (void *)(*g_vnodes + *g_vnode_size * i);
    void *nodeData = *(uintptr_t **)(vp + 0x48);
    uint32_t refCount = *(uint32_t *)(vp + 0x58);
    void *fnode = *(uintptr_t **)(vp + 0x60);

    if (fnode && fnode == nodeData) {
      int looked_up;
      int res = update_fnode(fnode, &looked_up);
      int idx = fnode_index(fnode);
      if (looked_up && idx >= 0)
        g_fnode_done[idx / 32] |= 1u << (idx % 32);
      if (res < 0) {
        if (refCount == 0) {
          void *nc = *(uintptr_t **)(vp + 0x70);
          if (nc)
            clear_ncache(nc, 0);
          ksceVopInactive(vp);
          vfs_free_vnode(vp);
        } else {
          stale_open++;
          void *fsys = *(uintptr_t **)(fnode + 0x20c);
          int fsi = fs_index(fsys);
          /* a drive we cannot tell: all of them, as before */
          g_ro_mask |= fsi >= 0 && fsi < 32 ? 1u << fsi : 0xFFFFFFFFu;
        }
      } else if (res != FNODE_UNUSED) {
        *(uint64_t *)(vp + 0x80) = *(uint64_t *)(fnode + 0x218);
      }
    }
  }
  return stale_open;
}

static void invalidate_buff_caches(void) {
  int num = *g_num_vnodes;
  for (int i = 0; i < num; i++) {
    void *vp = (void *)(*g_vnodes + *g_vnode_size * i);
    void *file = *(uintptr_t **)(vp + 0x5c);
    if (file) {
      invalidate_buff_cache(file, vp);
      *(uint32_t *)(file + 0xc) = 8;
    }
  }
}

static void remount_filesystems(void) {
  for (uint32_t i = 0; i < g_off->fs_count; i++) {
    void *fileSystem = (void *)(g_shared_resource + g_off->fs_stride * i);
    uint32_t flags = *(uint32_t *)(fileSystem + 0xb0);
    void *drive = *(uintptr_t **)(fileSystem + 0xac);
    void *trash_fnode = *(uintptr_t **)(fileSystem + 0x114);
    if (drive) {
      trace_wake((uint8_t)(JV_WAKE_FS_MOUNT + (i > 0x3F ? 0x3F : i)));
      if (ksceKernelLockFastMutex(drive + 0x1e0) >= 0) {
        char *assignName = *(char **)(drive + 0xc0);

        if (trash_fnode) {
          UVFAT_ReleaseFnode(g_shared_resource, trash_fnode);
          *(uintptr_t **)(fileSystem + 0x114) = NULL;
        }

        cleanup_cluster_cache_area(drive);

        if (*(uintptr_t **)(fileSystem + 0x12c)) {
          sfree(*(uintptr_t **)(fileSystem + 0x12c),
                *(uint32_t *)(fileSystem + 0x420) + 0x200);
          *(uint32_t *)(fileSystem + 0x128) = 0;
          *(uintptr_t **)(fileSystem + 0x12c) = NULL;
        }

        uint32_t args[2];
        args[0] = (uint32_t)drive;
        args[1] = flags;
        int mr = UVFAT_Mount(g_shared_resource, assignName, 1, args);
        JvWakeFsDrive *rec = fs_rec((int)i);
        if (rec && assignName)
          for (int k = 0; k < 4 && assignName[k]; k++)
            rec->name[k] = assignName[k];
        if (mr < 0 && i < 16)
          trace_wake_fs()->mount_fail |= (uint16_t)(1u << i);

        ksceKernelUnlockFastMutex(drive + 0x1e0);
      }
    }
  }
}

/* The 2-NOP patch in UVFAT_ReadManagementArea lets UVFAT_Mount re-read
 * the management area on a remount. It is only applied around the remount
 * after a load: as a permanent patch (the original did that) it changed
 * the driver for everything else too, and games doing many file lookups
 * (e.g. through rePatch) became very slow on 3.60. */
static int mgmt_patch_apply(void) {
  uint32_t nop_nop_opcode = 0xbf00bf00;
  if (g_exfat_modid < 0)
    return -1;
  g_mgmt_patch = taiInjectDataForKernel(KERNEL_PID, g_exfat_modid, 0,
                                        g_off->exfat_read_mgmt_patch,
                                        &nop_nop_opcode, 4);
  return g_mgmt_patch < 0 ? -1 : 0;
}

static void mgmt_patch_remove(void) {
  if (g_mgmt_patch >= 0)
    taiInjectReleaseForKernel(g_mgmt_patch);
  g_mgmt_patch = -1;
}

FsUpdateTiming g_fs_update_timing;

static uint32_t fs_ms_since(int64_t *t) {
  int64_t now = ksceKernelGetSystemTimeWide();
  uint32_t ms = (uint32_t)((now - *t) / 1000);
  *t = now;
  return ms;
}

int update_filesystem(void) {
  FsUpdateTiming *tm = &g_fs_update_timing;
  int64_t t = ksceKernelGetSystemTimeWide();
  memset(tm, 0, sizeof(*tm));
  tm->vnodes = (uint32_t)*g_num_vnodes;
  memset(g_fnode_done, 0, sizeof(g_fnode_done));
  g_ro_mask = 0;
  JvWakeFs *f = trace_wake_fs();
  f->n = (uint8_t)(g_off->fs_count > JV_WAKE_FS_MAX ? JV_WAKE_FS_MAX
                                                     : g_off->fs_count);
  invalidate_buff_caches();
  tm->ms[0] = fs_ms_since(&t);
  invalidate_file_caches();
  tm->ms[1] = fs_ms_since(&t);
  trace_wake(JV_WAKE_FS_CACHES);
  if (mgmt_patch_apply() < 0) {
    /* cannot remount safely: keep the card read-only until restart */
    return 1;
  }
  trace_wake(JV_WAKE_FS_PATCH);
  remount_filesystems();
  mgmt_patch_remove();
  tm->ms[2] = fs_ms_since(&t);
  trace_wake(JV_WAKE_FS_VNODES);
  int stale_open = update_vnodes();
  tm->ms[3] = fs_ms_since(&t);
  trace_wake(JV_WAKE_FS_FNODES);
  tm->fnodes = update_fnodes(&tm->fnodes_skipped);
  tm->ms[4] = fs_ms_since(&t);
  f->stale = (uint16_t)(stale_open > 0xFFFF ? 0xFFFF : stale_open);
  f->ro_mask = (uint16_t)g_ro_mask;
  return stale_open;
}

static int UVFAT_WriteDevice_Patched(void *fileSystem, int unused,
                                     uint64_t sector, int size, void *buf) {
  uint32_t mode = g_ignore_writedevice;
  prof_add(PROF_WRITE, prof_now());

  if (mode == FS_WRITES_DROPPED)
    return 0; /* restored OS before remount: stale caches, discard */
  if (mode == FS_WRITES_READ_ONLY) {
    int fsi = fs_index_any(fileSystem);
    JvWakeFs *f = trace_wake_fs();
    if (fsi < 0) {
      if (f->writes_unknown < 255)
        f->writes_unknown++;
      return SCE_ERROR_ERRNO_EROFS;
    }
    if (fsi >= 32 || (g_ro_mask & (1u << fsi))) {
      if (f->writes_refused < 0xFFFF)
        f->writes_refused++;
      return SCE_ERROR_ERRNO_EROFS;
    }
  }

  return TAI_CONTINUE(int, UVFAT_WriteDevice_Ref, fileSystem, unused,
                                                  sector, size, buf);
}

void fs_set_write_mode(uint32_t mode) {
  g_ignore_writedevice = mode;
  trace_wake_fs()->write_mode = (uint8_t)mode;
}

int fs_read_only_names(char *out, int len) {
  int n = 0;
  out[0] = 0;
  if (g_ignore_writedevice != FS_WRITES_READ_ONLY || !g_off)
    return 0;
  for (uint32_t i = 0; i < g_off->fs_count && i < 32 && n < len - 8; i++) {
    if (!(g_ro_mask & (1u << i)))
      continue;
    void *fsys = (void *)(g_shared_resource + g_off->fs_stride * i);
    void *drive = *(uintptr_t **)(fsys + 0xac);
    const char *nm = drive ? *(char **)(drive + 0xc0) : NULL;
    int r = snprintf(out + n, len - n, "%s%s", n ? ", " : "",
                     nm && nm[0] ? nm : "?");
    if (r > 0)
      n = n + r >= len ? len - 1 : n + r;
  }
  return n;
}

uint32_t fs_get_write_mode(void) {
  return g_ignore_writedevice;
}

volatile uint32_t *fs_write_mode_addr(void) {
  return &g_ignore_writedevice;
}

static int get_offset(SceUID modid, int seg, uint32_t off, void *out) {
  return module_get_offset(KERNEL_PID, modid, seg, off, (uintptr_t *)out);
}

int fs_init(const FwOffsets *o) {
  tai_module_info_t io_info, ex_info;
  int res = 0;

  g_off = o;

  memset(&io_info, 0, sizeof(io_info));
  io_info.size = sizeof(io_info);
  if (taiGetModuleInfoForKernel(KERNEL_PID, "SceIofilemgr", &io_info) < 0)
    return -1;

  memset(&ex_info, 0, sizeof(ex_info));
  ex_info.size = sizeof(ex_info);
  if (taiGetModuleInfoForKernel(KERNEL_PID, "SceExfatfs", &ex_info) < 0)
    return -1;

  res |= get_offset(io_info.modid, 0, o->iofilemgr_clear_ncache, &clear_ncache);
  res |= get_offset(io_info.modid, 0, o->iofilemgr_invalidate_buff_cache,
                    &invalidate_buff_cache);
  res |= get_offset(io_info.modid, 1, o->iofilemgr_num_vnodes, &g_num_vnodes);
  res |= get_offset(io_info.modid, 1, o->iofilemgr_vnodes, &g_vnodes);
  res |= get_offset(io_info.modid, 1, o->iofilemgr_vnode_size, &g_vnode_size);

  res |= get_offset(ex_info.modid, 0, o->exfat_mount, &UVFAT_Mount);
  res |= get_offset(ex_info.modid, 0, o->exfat_search_file_common,
                    &UVFAT_SearchFile_common);
  res |= get_offset(ex_info.modid, 0, o->exfat_get_parent_fnode,
                    &UVFAT_GetParentFnode);
  res |= get_offset(ex_info.modid, 0, o->exfat_release_fnode,
                    &UVFAT_ReleaseFnode);
  res |= get_offset(ex_info.modid, 0, o->exfat_cleanup_cluster_cache,
                    &cleanup_cluster_cache_area);
  res |= get_offset(ex_info.modid, 0, o->exfat_invalidate_cluster_cache,
                    &invalidate_cluster_cache_data);
  res |= get_offset(ex_info.modid, 0, o->exfat_free_fp_cache, &FreeFpCache);
  res |= get_offset(ex_info.modid, 0, o->exfat_sfree, &sfree);
  res |= get_offset(ex_info.modid, 1, o->exfat_shared_resource,
                    &g_shared_resource);
  if (res < 0)
    return -1;

  if (module_get_export_func(KERNEL_PID, "SceIofilemgr", 0x40FD29C7,
                             0x21D57633, (uintptr_t *)&vfs_free_vnode) < 0)
    return -1;

  g_write_hook = taiHookFunctionOffsetForKernel(KERNEL_PID,
                                                &UVFAT_WriteDevice_Ref,
                                                ex_info.modid, 0,
                                                o->exfat_write_device, 1,
                                                UVFAT_WriteDevice_Patched);
  if (g_write_hook < 0)
    return -1;

  /* The UVFAT_ReadManagementArea patch is only applied during a remount
   * (mgmt_patch_apply). Check here that it can be applied, then remove it
   * again, so an unsuitable driver is refused at startup as before. */
  g_exfat_modid = ex_info.modid;
  if (mgmt_patch_apply() < 0) {
    taiHookReleaseForKernel(g_write_hook, UVFAT_WriteDevice_Ref);
    g_write_hook = -1;
    g_exfat_modid = -1;
    return -1;
  }
  mgmt_patch_remove();

  return 0;
}
