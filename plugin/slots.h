/* JamaisVu savestate plugin
 *
 * Slot files on ux0:
 *   ux0:data/JamaisVu/slotN.bin  savestate (pre-allocated, fixed size)
 *   ux0:data/JamaisVu/slotN.jvi  slot info: game, date, thumbnail.
 *                                Written only after a successful save, so
 *                                it doubles as the "this slot is valid"
 *                                marker.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __SLOTS_H__
#define __SLOTS_H__

#include <stdint.h>
#include "gui/menu_view.h"


/* All files live on the official Memory Card (storage.h). */
void slots_sync(void);

/* <Memory Card>data/JamaisVu/slotN.<ext> and <Memory Card>data/JamaisVu/<name> */
void slot_path(char *out, int len, int slot, const char *ext);
void jv_file_path(char *out, int len, const char *name);

int64_t slot_file_size(const char *path); /* -1 if missing */

/* Every slot file has exactly JV_SLOT_SIZE bytes (slot_policy.h). DRAM
 * (512 MiB) + CDRAM (128 MiB) is the most memory a snapshot can contain;
 * the rest is headroom for the savedata backup. A save that does not fit
 * fails cleanly. */
#include "slot_policy.h"

#define SLOT_INFO_MAGIC   0x4956534a /* "JSVI" */
#define SLOT_INFO_VERSION 3

#define JV_CFG_FILES 2
#define JV_CFG_ABSENT     0 /* the file does not exist */
#define JV_CFG_PRESENT    1 /* md5 is valid */
#define JV_CFG_UNREADABLE 2 /* exists but could not be read (or too big) */

typedef struct {
  uint32_t magic;
  uint32_t version;
  char titleid[16];
  char title[128];
  uint16_t year;
  uint8_t month, day, hour, minute, second, pad;
  uint32_t thumb_w, thumb_h;
  uint32_t has_thumb;
  char app_ver[8];    /* game version when the snapshot was taken */
  /* taiHEN plugin configuration when the snapshot was taken (see
   * config_fingerprint): ux0:tai/config.txt and ur0:tai/config.txt */
  uint8_t cfg_state[JV_CFG_FILES];
  uint8_t cfg_pad[4 - JV_CFG_FILES];
  uint8_t cfg_md5[JV_CFG_FILES][16];
  uint32_t crc; /* crc32 of this struct up to crc, then the thumbnail */
} SlotInfo;

#define JV_ERR_SLOT_NO_SPACE (-100)
#define JV_ERR_SLOT_IO       (-101)

/* Path of slotN.bin as the payload's FatFs sees the Memory Card ("ux0:"
 * is just FatFs' volume name there, whatever the OS mount is). */
void slot_payload_path(char *out, int len, int slot);

/* Makes sure slotN.bin exists on the Memory Card with exactly JV_SLOT_SIZE
 * bytes, using the OS's own exFAT driver (so all filesystem metadata is
 * written by the OS). Creating it writes zeros, which takes a while;
 * progress is reported in percent. */
int slot_prepare(int slot, void (*progress)(int pct));

/* Writes a fresh random token at the start of slotN.bin through the OS and
 * flushes it; the payload verifies it. */
int slot_stamp(int slot, uint8_t nonce[16]);

int slot_info_write(int slot, SlotInfo *info, const uint32_t *thumb);
int slot_info_read(int slot, SlotInfo *info, uint32_t *thumb);
int slot_info_remove(int slot);

/* slotN.jvi is valid and slotN.bin has the right size */
int slot_is_loadable(int slot);

/* slotN.bin exists with the right size (space already reserved) */
int slot_file_exists(int slot);

/* Free bytes on the Memory Card, or < 0 if unknown */
int64_t slot_free_space(void);

/* Game name from param.sfo (ASCII only; falls back to the title ID). */
void lookup_title(const char *titleid, char *out, int len);

/* Installed game version (param.sfo APP_VER of the update if one is
 * installed, else of the game). 0 on success, < 0 if it cannot be read. */
int lookup_app_ver(const char *titleid, char *out, int len);

/* MD5 of ux0:tai/config.txt and ur0:tai/config.txt (read-only). A snapshot
 * holds every plugin that was loaded; it may only be loaded again with the
 * same plugin configuration. */
extern const char *const jv_cfg_paths[JV_CFG_FILES];
void config_fingerprint(uint8_t state[JV_CFG_FILES],
                        uint8_t md5[JV_CFG_FILES][16]);
/* 1 if the current configuration equals the one stored in info; else 0
 * and *which is the index of the first file that differs. */
int config_matches(const SlotInfo *info, int *which);

int slots_mkdirs(void);    /* data/JamaisVu on the Memory Card */

#endif
