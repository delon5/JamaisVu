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

#define JV_DIR "ux0:data/JamaisVu"

/* Every slot file has exactly this size. DRAM (512 MiB) + CDRAM (128 MiB)
 * is the most memory a snapshot can contain; the rest is headroom for the
 * savedata backup. A save that does not fit fails cleanly. */
#define JV_SLOT_SIZE 0x2C000000u /* 704 MiB */

/* Free space that must remain on ux0 after creating a slot file. */
#define JV_FREE_SPACE_MARGIN (64ull * 1024 * 1024)

#define SLOT_INFO_MAGIC   0x4956534a /* "JSVI" */
#define SLOT_INFO_VERSION 1

typedef struct {
  uint32_t magic;
  uint32_t version;
  char titleid[16];
  char title[128];
  uint16_t year;
  uint8_t month, day, hour, minute, second, pad;
  uint32_t thumb_w, thumb_h;
  uint32_t has_thumb;
  uint32_t crc; /* crc32 of this struct up to crc, then the thumbnail */
} SlotInfo;

#define JV_ERR_SLOT_NO_SPACE (-100)
#define JV_ERR_SLOT_IO       (-101)

void slot_state_path(char *out, int len, int slot);

/* Makes sure slotN.bin exists with JV_SLOT_SIZE bytes, using the OS's own
 * exFAT driver (so all filesystem metadata is written by the OS). The
 * first call for a slot writes zeros, which takes a while; progress is
 * reported in percent. */
int slot_prepare(int slot, void (*progress)(int pct));

int slot_info_write(int slot, SlotInfo *info, const uint32_t *thumb);
int slot_info_read(int slot, SlotInfo *info, uint32_t *thumb);
int slot_info_remove(int slot);

/* slotN.jvi is valid and slotN.bin has the right size */
int slot_is_loadable(int slot);

/* Game name from param.sfo (ASCII only; falls back to the title ID). */
void lookup_title(const char *titleid, char *out, int len);

int jv_mkdirs(void);

#endif
