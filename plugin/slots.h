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
/* 4: thumbnail stored as RGB565 (28,800 bytes); 2 and 3 store it as
 * RGBA8888 (57,600 bytes) and are still read. */
#define SLOT_INFO_VERSION 4

#define JV_CFG_FILES 2
#define JV_CFG_ABSENT     0 /* the file does not exist */
#define JV_CFG_PRESENT    1 /* md5 is valid */
#define JV_CFG_UNREADABLE 2 /* exists but could not be read (or too big) */
#define JV_CFG_UNKNOWN    3 /* snapshot made before the check existed */

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

/* Slot info written before the config.txt check (version 2): the same
 * fields without cfg_*. Every later version must keep reading every older
 * one, so an update never hides or invalidates existing snapshots. */
typedef struct {
  uint32_t magic;
  uint32_t version;
  char titleid[16];
  char title[128];
  uint16_t year;
  uint8_t month, day, hour, minute, second, pad;
  uint32_t thumb_w, thumb_h;
  uint32_t has_thumb;
  char app_ver[8];
  uint32_t crc;
} SlotInfoV2;


#define JV_ERR_SLOT_NO_SPACE (-100)
#define JV_ERR_SLOT_IO       (-101)
#define JV_ERR_SLOT_CANCELLED (-102)

/* Path of slotN.bin as the payload's FatFs sees the Memory Card ("ux0:"
 * is just FatFs' volume name there, whatever the OS mount is). */
void slot_payload_path(char *out, int len, int slot);

/* Makes sure slotN.bin exists on the Memory Card with exactly JV_SLOT_SIZE
 * bytes, using the OS's own exFAT driver (so all filesystem metadata is
 * written by the OS). Creating it writes zeros, which takes a while;
 * progress is reported in percent. */
/* progress (may be NULL) returns nonzero to cancel: the partial file is
 * removed and JV_ERR_SLOT_CANCELLED returned. */
int slot_prepare(int slot, int (*progress)(int pct));

/* Writes a fresh random token at the start of slotN.bin through the OS and
 * flushes it; the payload verifies it. */
int slot_stamp(int slot, uint8_t nonce[16]);

/* thumb: JV_THUMB_W * JV_THUMB_H RGB565 pixels (as stored in v4 files) */
int slot_info_write(int slot, SlotInfo *info, const uint16_t *thumb);
int slot_info_read(int slot, SlotInfo *info, uint16_t *thumb);

/* slotN.jvf: physical address of the game's frame when the snapshot was
 * taken, for the progress screen during a load (drawn there once the
 * memory restore starts). Bound to the slot info by its CRC, so a slot
 * saved again by a build without this file never uses an old record.
 * Older slots have none: their load shows no progress while restoring. */
#define SLOT_FRAME_MAGIC 0x3146564Au /* "JVF1" */
typedef struct {
  uint32_t magic;
  uint32_t info_crc; /* SlotInfo.crc of the slot it belongs to */
  uint32_t paddr, pitch, width, height;
  uint32_t crc;      /* of the fields above */
} SlotFrame;
int slot_frame_write(int slot, uint32_t info_crc, uint32_t paddr,
                     uint32_t pitch, uint32_t width, uint32_t height);
int slot_frame_read(int slot, uint32_t info_crc, SlotFrame *f);

/* slotN.jve: the environment the snapshot was taken in, bound to the slot
 * info by its CRC like slotN.jvf (slots saved before it have none: not
 * checked).
 *   - The plugin folders ux0:tai and ur0:tai: every file's name and
 *     content (one level of subfolders too), except JamaisVu's own files
 *     (updating JamaisVu must never lock existing slots). A snapshot holds
 *     every plugin that was loaded: with any plugin file changed, it can't
 *     be loaded (like the config.txt check).
 *   - The LiveArea's app database (ur0:shell/db/app.db): a load restores
 *     the LiveArea's memory too, so after apps were installed, removed or
 *     moved its app list is older than the card's. A warning after the
 *     load, not a block (yet), until logs show how often app.db changes. */
#define JV_TAI_DIRS 2
extern const char *const jv_tai_dirs[JV_TAI_DIRS];
typedef struct {
  uint32_t state;    /* JV_CFG_PRESENT / _ABSENT / _UNREADABLE */
  uint32_t files;    /* files hashed */
  uint32_t bytes;    /* their total size */
  uint8_t md5[16];   /* XOR of md5(relative name, NUL, content) per file */
} TaiPrint;
#define SLOT_ENV_MAGIC 0x3145564Au /* "JVE1" */
typedef struct {
  uint32_t magic;
  uint32_t info_crc; /* SlotInfo.crc of the slot it belongs to */
  uint32_t appdb_state, appdb_size;
  uint8_t appdb_md5[16];
  TaiPrint tai[JV_TAI_DIRS];
  uint32_t crc;      /* of the fields above */
} SlotEnv;
/* app.db now; returns JV_CFG_PRESENT/_ABSENT/_UNREADABLE */
int appdb_fingerprint(uint8_t md5[16], uint32_t *size);
/* the plugin folders now */
void tai_fingerprint(TaiPrint out[JV_TAI_DIRS]);
/* the same, re-read only for a folder whose listing (file names, sizes,
 * times) changed since the last call: for the slot list, not save/load */
void tai_fingerprint_quick(TaiPrint out[JV_TAI_DIRS]);
/* 1 if every folder equals the record; else 0, *which = the first that
 * differs */
int tai_matches(const TaiPrint rec[JV_TAI_DIRS],
                const TaiPrint now[JV_TAI_DIRS], int *which);
int slot_env_write(int slot, const SlotEnv *e);
int slot_env_read(int slot, uint32_t info_crc, SlotEnv *e);
int slot_info_remove(int slot);

/* slotN.jvi is valid and slotN.bin has the right size */
int slot_is_loadable(int slot);

/* slotN.bin exists with the right size (space already reserved) */
int slot_file_exists(int slot);

/* Free bytes on the Memory Card, or < 0 if unknown */
int64_t slot_free_space(void);

/* Game name from param.sfo (ASCII only; falls back to the title ID). */
void lookup_title(const char *titleid, char *out, int len);

/* A UTF-8 name as printable ASCII (TM/(R) spelled out, other non-ASCII
 * left out). Returns 0 if it has at least 2 Latin letters, else -1. */
int title_to_ascii(const uint8_t *p, uint32_t vlen, char *out, int len);

/* The game's name as written in its param.sfo (UTF-8, e.g. Japanese),
 * cleaned of control characters. Returns -1 if not installed/readable. */
int lookup_title_utf8(const char *titleid, char *out, int len);

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
/* the same against an already computed fingerprint (one read for many
 * slots) */
int config_matches_fp(const SlotInfo *info,
                      const uint8_t state[JV_CFG_FILES],
                      const uint8_t md5[JV_CFG_FILES][16], int *which);

int slots_mkdirs(void);    /* data/JamaisVu on the Memory Card */

#endif
