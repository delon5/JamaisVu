/* JamaisVu savestate plugin
 *
 * Where savestates live: on the official Memory Card, and nowhere else.
 *
 * JamaisVu only ever writes to the official Memory Card. The payload can
 * only reach that card anyway, and keeping every other file there too
 * means an SD2Vita, PSVSD, USB stick or the internal storage is never
 * written by JamaisVu. Use a Memory Card dedicated to savestates and the
 * worst a failure can cost is savestates.
 *
 * The card may be mounted as ux0, or as uma0/xmc0 when a storage plugin
 * put an SD2Vita/PSVSD/USB stick on ux0. Which mount it is, is read from
 * the system's own mount table (sceIoFindMountPoint): every mount names
 * the block device behind it, and the Memory Card slot is
 * "sdstor0:xmc-...". Storage plugins (StorageMgr, YAMT, gamesd, usbmc)
 * remap mounts by changing exactly these entries, so the table reflects
 * the current setup. Detection only reads; it never writes anything.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __STORAGE_H__
#define __STORAGE_H__

#include <stdint.h>

/* Mount table entry and device, as used by StorageMgr/YAMT (0x38 bytes per
 * entry; layout checked against a 3.60 SceIofilemgr dump). */
typedef struct {
  const char *dev;      /* "ux0:" */
  const char *dev2;     /* "exfatux0" */
  const char *blkdev;   /* "sdstor0:xmc-lp-ign-userext" */
  const char *blkdev2;
  int id;
} JvIoDevice;

typedef struct {
  int id;
  const char *dev_unix;
  int unk;
  int dev_major;
  int dev_minor;
  const char *dev_filesystem;
  int unk2;
  JvIoDevice *dev;
  int unk3;
  JvIoDevice *dev2;
  int unk4[4];
} JvIoMountPoint;

typedef JvIoMountPoint *(*MountLookupFn)(int id);

/* main.c passes sceIoFindMountPoint once the firmware offsets are known */
void storage_set_mount_lookup(MountLookupFn fn);

/* Finds the mount of the official Memory Card (read-only, cheap).
 * Returns "ux0:", "uma0:" or "xmc0:", or NULL (see storage_reason()). */
const char *storage_detect(void);

const char *storage_mc_root(void);  /* result of the last detection */
const char *storage_reason(void);   /* why there is no usable card */

#endif
