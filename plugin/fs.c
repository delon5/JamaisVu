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
#include "storage.h"
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

/* FS_WRITES_READ_ONLY applies to these drives only, by name (ed20f94
 * kept a file system index, and the card's fresh mount on uma0 took the
 * index ux0 had: uma0 became read-only instead). Drives land here when
 * open files on them could not be looked up again, or when they could
 * not be re-read after a load (yet). Before ed20f94 it applied to every
 * exFAT drive, the system's own ur0/vd0/tm0 too. */
#define RO_MAX 6
static char g_ro_names[RO_MAX][8];
static volatile int g_ro_n;

/* drives not re-read at the wake-up (their device was away, or the
 * remount failed): no look-ups on them, which were what rebooted the
 * PS TV (ed20f94: remount of its USB ux0 failed, then the look-ups) */
static uint32_t g_skip_mask;
/* of those, the ones fs_refresh_late() re-reads once the device is back */
static uint32_t g_late_mask;

static int (*g_dev_state)(const char *root);

void fs_set_dev_check(int (*fn)(const char *root)) {
  g_dev_state = fn;
}

static int name_eq(const char *a, const char *b) {
  for (int k = 0; k < 7; k++) {
    if (a[k] != b[k])
      return 0;
    if (!a[k])
      return 1;
  }
  return 1;
}

static int ro_find(const char *name) {
  if (!name)
    return -1;
  for (int i = 0; i < g_ro_n; i++)
    if (name_eq(g_ro_names[i], name))
      return i;
  return -1;
}

static void ro_add(const char *name) {
  if (!name || ro_find(name) >= 0 || g_ro_n >= RO_MAX)
    return;
  int k;
  for (k = 0; k < 7 && name[k]; k++)
    g_ro_names[g_ro_n][k] = name[k];
  g_ro_names[g_ro_n][k] = 0;
  g_ro_n++;
}

static void ro_remove(const char *name) {
  int i = ro_find(name);
  if (i < 0)
    return;
  for (; i < g_ro_n - 1; i++)
    memcpy(g_ro_names[i], g_ro_names[i + 1], sizeof(g_ro_names[i]));
  g_ro_n--;
}

int fs_read_only_count(void) {
  return g_ro_n;
}

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

static void *fs_at(uint32_t i) {
  return (void *)(g_shared_resource + g_off->fs_stride * i);
}

/* the drive's name ("ux0:") as the exFAT driver has it now, or NULL */
static const char *fs_name(int i) {
  if (i < 0 || !g_off || (uint32_t)i >= g_off->fs_count)
    return NULL;
  void *drive = *(uintptr_t **)(fs_at((uint32_t)i) + 0xac);
  return drive ? *(char **)(drive + 0xc0) : NULL;
}

/* file system index of an fnode, -1 if unknown */
static int fnode_fs(void *fnode) {
  return fs_index(*(uintptr_t **)(fnode + 0x20c));
}

/* only: 0 = every drive not skipped, else just these drives */
static int fs_wanted(int fsi, uint32_t only) {
  if (only)
    return fsi >= 0 && fsi < 32 && (only & (1u << fsi));
  return !(fsi >= 0 && fsi < 32 && (g_skip_mask & (1u << fsi)));
}

static void ro_mask_record(void) {
  uint16_t m = 0;
  for (uint32_t i = 0; g_off && i < g_off->fs_count && i < 16; i++)
    if (ro_find(fs_name((int)i)) >= 0)
      m |= (uint16_t)(1u << i);
  trace_wake_fs()->ro_mask = m;
}

static void invalidate_file_caches(uint32_t only) {
  uint32_t num = *(uint32_t *)(g_shared_resource + g_off->sr_num_files);
  void *files = *(uintptr_t **)(g_shared_resource + g_off->sr_files);
  for (uint32_t i = 0; i < num; i++) {
    FileCache *filep = (FileCache *)(files + 0x38 * i);
    if (filep->fnode && fs_wanted(fnode_fs((void *)filep->fnode), only)) {
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

  uint32_t one = (uint32_t)((ksceKernelGetSystemTimeWide() - t0) / 1000);
  JvWakeFs *fw = trace_wake_fs();
  if (one > fw->slow_ms) {
    fw->slow_ms = one > 0xFFFF ? 0xFFFF : (uint16_t)one;
    fw->slow_drive = (uint8_t)(fsi < 0 ? 0xFF : fsi);
    fw->slow_res = res;
  }
  if (rec) {
    uint32_t ms = rec->ms + one;
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
static uint32_t update_fnodes(uint32_t *skipped, uint32_t only) {
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
    if (!*(uintptr_t **)(fnode + 0x20c) || !fs_wanted(fnode_fs(fnode), only))
      continue;
    if (update_fnode(fnode, &looked_up) != FNODE_UNUSED)
      used++;
  }
  return used;
}

/* Returns the number of files that are still open but no longer exist on
 * the card. Their cached cluster chains are stale; writing through them
 * could overwrite other files. */
static int update_vnodes(uint32_t only) {
  int stale_open = 0;
  int num = *g_num_vnodes;
  for (int i = 0; i < num; i++) {
    void *vp = (void *)(*g_vnodes + *g_vnode_size * i);
    void *nodeData = *(uintptr_t **)(vp + 0x48);
    uint32_t refCount = *(uint32_t *)(vp + 0x58);
    void *fnode = *(uintptr_t **)(vp + 0x60);

    if (fnode && fnode == nodeData && fs_wanted(fnode_fs(fnode), only)) {
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
          const char *nm = fs_name(fnode_fs(fnode));
          ro_add(nm ? nm : "?");
        }
      } else if (res != FNODE_UNUSED) {
        *(uint64_t *)(vp + 0x80) = *(uint64_t *)(fnode + 0x218);
      }
    }
  }
  return stale_open;
}

static void invalidate_buff_caches(uint32_t only) {
  int num = *g_num_vnodes;
  for (int i = 0; i < num; i++) {
    void *vp = (void *)(*g_vnodes + *g_vnode_size * i);
    void *file = *(uintptr_t **)(vp + 0x5c);
    void *fnode = *(uintptr_t **)(vp + 0x60);
    int exfat = fnode && fnode == *(uintptr_t **)(vp + 0x48);
    if (only && !exfat)
      continue;
    if (exfat && !fs_wanted(fnode_fs(fnode), only))
      continue; /* a drive left alone (see check_devices) */
    if (file) {
      invalidate_buff_cache(file, vp);
      *(uint32_t *)(file + 0xc) = 8;
    }
  }
}

/* Remounts file system i in place (the restored image's view of the
 * drive is dropped, the drive is read again). 1 if there is no drive. */
static int remount_one(uint32_t i) {
  void *fileSystem = fs_at(i);
  uint32_t flags = *(uint32_t *)(fileSystem + 0xb0);
  void *drive = *(uintptr_t **)(fileSystem + 0xac);
  void *trash_fnode = *(uintptr_t **)(fileSystem + 0x114);
  if (!drive)
    return 1;
  int mr = ksceKernelLockFastMutex(drive + 0x1e0);
  if (mr < 0)
    return mr;
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
  mr = UVFAT_Mount(g_shared_resource, assignName, 1, args);

  ksceKernelUnlockFastMutex(drive + 0x1e0);
  return mr;
}

static void mount_failed(uint32_t i, int mr) {
  JvWakeFs *f = trace_wake_fs();
  if (i < 16)
    f->mount_fail |= (uint16_t)(1u << i);
  if (!f->mount_err)
    f->mount_err = mr;
}

/* how long the wake-up may wait for a device to come back (ms) */
#define FS_DEV_WAIT_MS 3000

/* Before anything is dropped: which drives can be re-read now? A USB
 * drive (ux0 through usbmc) may not be back yet - its remount failed
 * (ed20f94). Such a drive is left completely alone (no cache dropped, no
 * remount, no look-ups: the restored image's own view of it stays as it
 * was) and read-only; fs_refresh_late() re-reads it once it is back. */
static int root_is(const char *name, const char *root) {
  int n = (int)strlen(root) - 1; /* without the ':' */
  return name && strncmp(name, root, n) == 0 &&
         (name[n] == ':' || name[n] == '\0');
}

/* PS TV: by the drive's name only - nothing of the mount table or the
 * devices is touched during the wake-up there (7a523c1 died 20 ms into
 * this check without reading any device; what it still did was look the
 * drives up in the mount table, ux0's entry being the USB stick that is
 * going away then) */
static int dev_state_by_name(const char *name) {
  const char *mc = storage_mc_root();
  int card_elsewhere = mc && !root_is(mc, "ux0:");
  if (root_is(name, "ux0:"))
    return card_elsewhere ? STORAGE_DEV_NOT_READY : STORAGE_DEV_UNCHECKED;
  if (root_is(name, "uma0:") || root_is(name, "xmc0:"))
    return card_elsewhere ? STORAGE_DEV_CARD_LATER : STORAGE_DEV_NOT_READY;
  return STORAGE_DEV_UNCHECKED;
}

static void check_devices(void) {
  JvWakeFs *f = trace_wake_fs();
  int64_t t0 = ksceKernelGetSystemTimeWide();
  int64_t next_tick = t0 + 500 * 1000;
  trace_wake(JV_WAKE_FS_DEV_CHECK);
  for (uint32_t i = 0; i < g_off->fs_count && i < 32; i++) {
    const char *name = fs_name((int)i);
    if (!*(uintptr_t **)(fs_at(i) + 0xac))
      continue;
    JvWakeFsDrive *rec = fs_rec((int)i);
    if (rec && name)
      for (int k = 0; k < 4 && name[k]; k++)
        rec->name[k] = name[k];

    /* once SceSdstor has let the USB storage go (PS TV), it is not read
     * again until it is back - in fs_refresh_late() */
    int st = g_storage_usb_drops ? dev_state_by_name(name)
             : g_dev_state ? g_dev_state(name) : STORAGE_DEV_UNCHECKED;
    while (st == STORAGE_DEV_NOT_READY && !g_storage_usb_gone_t &&
           !g_storage_usb_drops &&
           ksceKernelGetSystemTimeWide() - t0 < FS_DEV_WAIT_MS * 1000LL) {
      ksceKernelDelayThread(50 * 1000);
      if (ksceKernelGetSystemTimeWide() >= next_tick) {
        trace_wake(JV_WAKE_FS_DEV_WAIT);
        next_tick += 500 * 1000;
      }
      if (g_storage_usb_gone_t)
        break; /* stays NOT_READY: re-read later */
      st = g_dev_state(name);
    }
    uint32_t w = (uint32_t)((ksceKernelGetSystemTimeWide() - t0) / 1000);
    f->wait_ms = w > 0xFFFF ? 0xFFFF : (uint16_t)w;
    if (st == STORAGE_DEV_UNCHECKED || st == STORAGE_DEV_READY)
      continue;
    g_skip_mask |= 1u << i;
    if (i < 16)
      f->deferred |= (uint16_t)(1u << i);
    if (st == STORAGE_DEV_CARD_LATER)
      continue; /* the card on uma0/xmc0: mounted afresh by the worker */
    /* NOT_READY: re-read later once it is back; UNKNOWN (no way to tell
     * when it is back): left as it is until restart */
    if (st == STORAGE_DEV_NOT_READY)
      g_late_mask |= 1u << i;
    ro_add(name ? name : "?");
  }
  trace_wake(JV_WAKE_FS_DEV_DONE);
}

static void remount_filesystems(void) {
  for (uint32_t i = 0; i < g_off->fs_count && i < 32; i++) {
    if (!*(uintptr_t **)(fs_at(i) + 0xac) || (g_skip_mask & (1u << i)))
      continue;
    const char *name = fs_name((int)i);
    trace_wake((uint8_t)(JV_WAKE_FS_MOUNT + (i > 0x3F ? 0x3F : i)));
    int mr = remount_one(i);
    if (mr < 0) {
      /* never look files up on a drive whose remount failed */
      mount_failed(i, mr);
      g_skip_mask |= 1u << i;
      ro_add(name ? name : "?");
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
  g_ro_n = 0;
  g_skip_mask = 0;
  g_late_mask = 0;
  JvWakeFs *f = trace_wake_fs();
  f->n = (uint8_t)(g_off->fs_count > JV_WAKE_FS_MAX ? JV_WAKE_FS_MAX
                                                     : g_off->fs_count);
  check_devices();
  invalidate_buff_caches(0);
  tm->ms[0] = fs_ms_since(&t);
  invalidate_file_caches(0);
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
  int stale_open = update_vnodes(0);
  tm->ms[3] = fs_ms_since(&t);
  trace_wake(JV_WAKE_FS_FNODES);
  tm->fnodes = update_fnodes(&tm->fnodes_skipped, 0);
  tm->ms[4] = fs_ms_since(&t);
  f->stale = (uint16_t)(stale_open > 0xFFFF ? 0xFFFF : stale_open);
  ro_mask_record();
  return stale_open;
}

int fs_refresh_late(uint32_t timeout_ms) {
  JvWakeFs *f = trace_wake_fs();
  int done = 0;
  if (!g_late_mask || !g_dev_state)
    return 0;
  int64_t t0 = ksceKernelGetSystemTimeWide();
  for (uint32_t i = 0; i < g_off->fs_count && i < 32; i++) {
    uint32_t bit = 1u << i;
    if (!(g_late_mask & bit))
      continue;
    const char *name = fs_name((int)i);
    /* PS TV: not before the USB storage is back */
    if (g_storage_usb_gone_t || g_storage_usb_drops) {
      storage_wait_usb_settled(timeout_ms);
      trace_wake(JV_WAKE_USB_SETTLED);
    }
    int st = g_dev_state(name);
    while (st == STORAGE_DEV_NOT_READY &&
           ksceKernelGetSystemTimeWide() - t0 < timeout_ms * 1000LL) {
      ksceKernelDelayThread(100 * 1000);
      st = g_dev_state(name);
    }
    uint32_t w = (uint32_t)((ksceKernelGetSystemTimeWide() - t0) / 1000);
    f->late_ms = w > 0xFFFF ? 0xFFFF : (uint16_t)w;
    if (st == STORAGE_DEV_NOT_READY)
      continue; /* still away: stays read-only, nothing looked up */
    g_late_mask &= ~bit;

    if (mgmt_patch_apply() < 0)
      continue;
    int mr = remount_one(i);
    mgmt_patch_remove();
    if (mr < 0) {
      mount_failed(i, mr);
      continue;
    }
    g_skip_mask &= ~bit;
    invalidate_buff_caches(bit);
    invalidate_file_caches(bit);
    int stale = update_vnodes(bit);
    uint32_t skipped;
    update_fnodes(&skipped, bit);
    if (i < 16)
      f->late_ok |= (uint16_t)bit;
    f->late_stale = (uint8_t)(stale > 255 ? 255 : stale);
    if (!stale)
      ro_remove(name);
    done++;
  }
  ro_mask_record();
  if (!g_ro_n && g_ignore_writedevice == FS_WRITES_READ_ONLY)
    fs_set_write_mode(FS_WRITES_ALLOWED);
  return done;
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
    const char *nm = fs_name(fsi);
    if (!nm || ro_find(nm) >= 0) {
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
  if (g_ignore_writedevice != FS_WRITES_READ_ONLY)
    return 0;
  for (int i = 0; i < g_ro_n && n < len - 10; i++) {
    int r = snprintf(out + n, len - n, "%s%s", n ? ", " : "", g_ro_names[i]);
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
