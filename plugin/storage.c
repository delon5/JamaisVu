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
static const char *g_reason = "not detected yet";

void storage_set_mount_lookup(MountLookupFn fn) {
  g_lookup = fn;
}

const char *storage_mc_root(void) {
  return g_mc_root;
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

const char *storage_detect(void) {
  g_mc_root = NULL;

  if (!g_lookup) {
    g_reason = "the mount table is not available on this firmware";
    return NULL;
  }

  for (size_t i = 0; i < N_MOUNTS; i++) {
    if (mount_is_mc(k_mounts[i].id) && mounted(k_mounts[i].root)) {
      g_mc_root = k_mounts[i].root;
      g_reason = "ok";
      return g_mc_root;
    }
  }

  g_reason = "no official Memory Card found (insert one, or mount it as "
             "ux0, uma0 or xmc0)";
  return NULL;
}
