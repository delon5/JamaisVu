/* JamaisVu savestate plugin
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <psp2kern/io/stat.h>
#include <psp2kern/io/fcntl.h>

#include <stddef.h>
#include <stdio.h>
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
static const char *volatile g_mc_root = NULL;
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
  /* the result is published at the end, root last: other threads read
   * storage_mc_root() meanwhile (the io thread's card watch, the
   * worker), and a NULL seen during a detection sent their write to a
   * device-less path */
  if (!g_lookup) {
    g_reason = "the mount table is not available on this firmware";
    g_mc_blkdev = NULL;
    g_mc_root = NULL;
    return NULL;
  }

  for (size_t i = 0; i < N_MOUNTS; i++) {
    if (mount_is_mc(k_mounts[i].id) && mounted(k_mounts[i].root)) {
      g_mc_blkdev = mc_blkdev_of(k_mounts[i].id);
      __sync_synchronize();
      g_mc_root = k_mounts[i].root;
      g_reason = "ok";
      return g_mc_root;
    }
  }
  g_mc_blkdev = NULL;
  g_mc_root = NULL;

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

int storage_mount_is_card(int id) {
  return mount_is_mc(id);
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

const char *storage_card_root(void) {
  for (size_t i = 1; i < N_MOUNTS; i++)
    if (mount_is_mc(k_mounts[i].id) && mounted(k_mounts[i].root))
      return k_mounts[i].root;
  return NULL;
}

int storage_force_remount_card(const char **root) {
  if (root)
    *root = NULL;
  /* ux0 is the system's; only a card moved to uma0/xmc0 by a storage
   * plugin, and only where it is mounted now */
  for (size_t i = 1; i < N_MOUNTS; i++) {
    if (!mount_is_mc(k_mounts[i].id) || !mounted(k_mounts[i].root))
      continue;
    if (root)
      *root = k_mounts[i].root;
    if (!g_mount || !g_umount)
      return -1;
    g_umount(k_mounts[i].id, 0, 0, 0);
    g_umount(k_mounts[i].id, 1, 0, 0);
    return g_mount(k_mounts[i].id, NULL, 0, 0, 0, 0);
  }
  return 1;
}

/* The device name that works for the "is it back?" check, per mount,
 * found by storage_dev_calibrate() at start while the drive certainly
 * works: usbmc's ux0 names sdstor0:uma-pp-act-a, which cannot be opened
 * directly (0x80010013 on both consoles, c8d049a). "" = none found. */
static char g_cal_name[N_MOUNTS][40];

static int probe_read(const char *blk, int *open_ret) {
  static uint8_t sec[512] __attribute__((aligned(64)));
  SceUID fd = ksceIoOpen(blk, SCE_O_RDONLY, 0);
  int r = -1;
  if (fd >= 0) {
    r = ksceIoRead(fd, sec, sizeof(sec));
    ksceIoClose(fd);
  }
  if (open_ret)
    *open_ret = fd;
  return r;
}

/* mount i if root is one of ours and lies on a removable device that is
 * not the card on uma0/xmc0; -1 otherwise (*card = 1 for that card) */
static int removable_mount(const char *root, const char **blk1,
                           const char **blk2, int *card) {
  *card = 0;
  if (!root || !g_lookup)
    return -1;
  for (size_t i = 0; i < N_MOUNTS; i++) {
    if (strcmp(root, k_mounts[i].root) != 0)
      continue;
    JvIoMountPoint *mp = g_lookup(k_mounts[i].id);
    if (!mp || mp->id != k_mounts[i].id || !mp->dev)
      return -1;
    const char *b = mp->dev->blkdev ? mp->dev->blkdev : mp->dev->blkdev2;
    if (!b || strncmp(b, "sdstor0:int-", 12) == 0)
      return -1;
    /* the card on uma0/xmc0 is mounted afresh after a load anyway; its
     * in-place re-read crashed the PS TV (f3caa8d: remount of uma0 3.7 s
     * after the wake-up, once SceSdstor had set the card up again) */
    if (i > 0 && mount_is_mc(k_mounts[i].id)) {
      *card = 1;
      return -1;
    }
    *blk1 = b;
    *blk2 = mp->dev->blkdev2;
    return (int)i;
  }
  return -1;
}

int storage_dev_calibrate(const char *root, char *out, int len) {
  const char *b1 = NULL, *b2 = NULL;
  int card, n = 0;
  int i = removable_mount(root, &b1, &b2, &card);
  out[0] = 0;
  if (i < 0)
    return snprintf(out, len, "%s: %s", root ? root : "-",
                    card ? "the Memory Card (mounted afresh after loads)"
                         : "not checked (internal storage or not mounted)");
  g_cal_name[i][0] = 0;

  /* the mount's own names, then the whole device under its usual names */
  static char cand[5][40];
  int nc = 0;
  snprintf(cand[nc++], sizeof(cand[0]), "%s", b1);
  if (b2 && strcmp(b2, b1) != 0)
    snprintf(cand[nc++], sizeof(cand[0]), "%s", b2);
  const char *dash = strchr(b1, '-');
  int pl = dash ? (int)(dash - b1) : (int)strlen(b1);
  snprintf(cand[nc++], sizeof(cand[0]), "%.*s-lp-act-entire", pl, b1);
  snprintf(cand[nc++], sizeof(cand[0]), "%.*s-lp-ign-entire", pl, b1);

  n = snprintf(out, len, "%s:", root);
  for (int k = 0; k < nc; k++) {
    int fd = 0;
    int r = probe_read(cand[k], &fd);
    if (n < len - 1) {
      int w = snprintf(out + n, len - n, " %s open 0x%08X read 0x%08X;",
                       cand[k], (unsigned)fd, (unsigned)r);
      if (w > 0)
        n = n + w >= len ? len - 1 : n + w;
    }
    if (r == 512 && !g_cal_name[i][0])
      snprintf(g_cal_name[i], sizeof(g_cal_name[i]), "%s", cand[k]);
  }
  if (n < len - 1) {
    int w = snprintf(out + n, len - n, " -> %s",
                     g_cal_name[i][0] ? g_cal_name[i]
                                      : "none works: no check after loads");
    if (w > 0)
      n = n + w >= len ? len - 1 : n + w;
  }
  return n;
}

/* Can the device behind this drive be read? (one sector, writes nothing) */
int storage_dev_state(const char *root) {
  const char *b1 = NULL, *b2 = NULL;
  int card;
  int i = removable_mount(root, &b1, &b2, &card);
  if (card)
    return STORAGE_DEV_CARD_LATER;
  if (i < 0)
    return STORAGE_DEV_UNCHECKED;
  if (!g_cal_name[i][0])
    return STORAGE_DEV_UNKNOWN;
  return probe_read(g_cal_name[i], NULL) == 512 ? STORAGE_DEV_READY
                                                : STORAGE_DEV_NOT_READY;
}
