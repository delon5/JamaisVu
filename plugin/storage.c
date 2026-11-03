/* JamaisVu savestate plugin
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <psp2kern/io/stat.h>

#include <stddef.h>
#include <string.h>

#include "storage.h"

/* Mounts the Memory Card can have. If several of them are the Memory Card
 * and mounted, the first one wins. */
static const struct {
  const char *root;
  int id;
} k_mounts[] = {
  { "ux0:",  0x800 },
  { "uma0:", 0xF00 },
  { "xmc0:", 0xE00 },
};
#define N_MOUNTS (sizeof(k_mounts) / sizeof(k_mounts[0]))

#define MC_BLKDEV_PREFIX "sdstor0:xmc-"

static MountLookupFn g_lookup = NULL;
static const char *g_mc_root = NULL;
static const char *g_mc_blkdev = NULL;
static const char *g_reason = "not detected yet";

void storage_set_mount_lookup(MountLookupFn fn) {
  g_lookup = fn;
}

const char *storage_mc_root(void) {
  return g_mc_root;
}

const char *storage_mc_blkdev(void) {
  return g_mc_blkdev;
}

const char *storage_reason(void) {
  return g_reason;
}

static int mounted(const char *root) {
  SceIoStat st;
  memset(&st, 0, sizeof(st));
  return ksceIoGetstat(root, &st) >= 0;
}

static int is_mc_blkdev(const char *name) {
  return name &&
         strncmp(name, MC_BLKDEV_PREFIX, sizeof(MC_BLKDEV_PREFIX) - 1) == 0;
}

static int is_mc_device(const JvIoDevice *d) {
  return d && (is_mc_blkdev(d->blkdev) || is_mc_blkdev(d->blkdev2));
}

/* Is this mount, according to the system's mount table, the Memory Card?
 * Both device pointers must agree (StorageMgr sets both when it remaps). */
static int mount_is_mc(int id) {
  JvIoMountPoint *mp = g_lookup ? g_lookup(id) : NULL;
  if (!mp || mp->id != id || !is_mc_device(mp->dev))
    return 0;
  if (mp->dev2 && mp->dev2 != mp->dev && !is_mc_device(mp->dev2))
    return 0;
  return 1;
}

static const char *mc_blkdev_of(int id) {
  JvIoMountPoint *mp = g_lookup ? g_lookup(id) : NULL;
  if (!mp || !mp->dev)
    return NULL;
  if (is_mc_blkdev(mp->dev->blkdev))
    return mp->dev->blkdev;
  if (is_mc_blkdev(mp->dev->blkdev2))
    return mp->dev->blkdev2;
  return NULL;
}

const char *storage_detect(void) {
  g_mc_root = NULL;
  g_mc_blkdev = NULL;

  if (!g_lookup) {
    g_reason = "the mount table is not available on this firmware";
    return NULL;
  }

  for (size_t i = 0; i < N_MOUNTS; i++) {
    if (mount_is_mc(k_mounts[i].id) && mounted(k_mounts[i].root)) {
      g_mc_root = k_mounts[i].root;
      g_mc_blkdev = mc_blkdev_of(k_mounts[i].id);
      g_reason = "ok";
      return g_mc_root;
    }
  }

  g_reason = "no official Memory Card found (insert one, or mount it as "
             "ux0, uma0 or xmc0)";
  return NULL;
}

static MountFn g_mount = NULL;
static UmountFn g_umount = NULL;

void storage_set_mount_ops(MountFn mount, UmountFn umount) {
  g_mount = mount;
  g_umount = umount;
}

int storage_card_unmounted(const char **root) {
  /* the card can be mounted only once: if any of its mounts is up (e.g.
   * uma0), the others (xmc0, the system's default entry for it) stay
   * down - 1b41e43 tried xmc0 every 4 s, 0x80010078 */
  for (size_t i = 0; i < N_MOUNTS; i++)
    if (mount_is_mc(k_mounts[i].id) && mounted(k_mounts[i].root))
      return 0;
  /* the card belongs on ux0 (no storage plugin moved it): the system
   * mounts it there itself */
  if (mount_is_mc(k_mounts[0].id))
    return 0;
  for (size_t i = 1; i < N_MOUNTS; i++)
    if (mount_is_mc(k_mounts[i].id) && !mounted(k_mounts[i].root)) {
      if (root)
        *root = k_mounts[i].root;
      return 1;
    }
  return 0;
}

int storage_remount_card(const char **root) {
  if (root)
    *root = NULL;
  if (!storage_card_unmounted(NULL))
    return 1;
  /* ux0 is left to the system: it mounts its own card there */
  for (size_t i = 1; i < N_MOUNTS; i++) {
    if (!mount_is_mc(k_mounts[i].id) || mounted(k_mounts[i].root))
      continue;
    if (root)
      *root = k_mounts[i].root;
    if (!g_mount || !g_umount)
      return -1;
    /* as usbmc does at boot: both unmount kinds, then mount by id (the
     * mount table entry still points at the Memory Card) */
    g_umount(k_mounts[i].id, 0, 0, 0);
    g_umount(k_mounts[i].id, 1, 0, 0);
    return g_mount(k_mounts[i].id, NULL, 0, 0, 0, 0);
  }
  return 1;
}
