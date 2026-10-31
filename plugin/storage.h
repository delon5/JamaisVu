/* JamaisVu savestate plugin
 *
 * Where savestates live.
 *
 * The payload can only talk to the official Memory Card, but users mount
 * it under different names (ux0, or uma0/xmc0 when an SD2Vita, PSVSD or
 * USB stick is ux0). JamaisVu prefers uma0 for its slots:
 *
 *  - uma0 is the Memory Card      -> DIRECT:  slots on uma0
 *  - uma0 is other storage        -> ARCHIVE: the payload saves into one
 *    (SD2Vita/PSVSD/USB)                      working file on the Memory
 *                                             Card, the plugin copies it
 *                                             to a slot on uma0 (and back
 *                                             before a load)
 *  - no uma0                      -> DIRECT:  slots on the Memory Card
 *
 * Which mount is the Memory Card is detected, not guessed: the plugin
 * writes a probe file through each candidate mount and looks for it by
 * reading the Memory Card's raw block device.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __STORAGE_H__
#define __STORAGE_H__

#include <stdint.h>

typedef enum {
  STORAGE_NONE = 0,  /* no usable Memory Card: save/load unavailable */
  STORAGE_DIRECT,
  STORAGE_ARCHIVE,
} StorageMode;

/* Re-detects the layout (cheap; called whenever the menu opens). */
StorageMode storage_detect(void);

StorageMode storage_mode(void);
const char *storage_mc_root(void);   /* mount of the Memory Card, or NULL */
const char *storage_slot_root(void); /* where slotN.bin/.jvi live, or NULL */
const char *storage_reason(void);    /* why the mode is NONE */

/* ARCHIVE: after a successful save into the working file, copy the used
 * part of it (read raw from the Memory Card) to slotN on the slot root.
 * The previous slotN is only replaced once the copy is complete. */
int storage_archive_from_work(int slot, uint32_t *len, uint32_t *crc,
                              void (*progress)(int pct));

/* ARCHIVE: before a load, copy slotN back into the working file and
 * verify it by reading the Memory Card raw (crc must match the slot). */
int storage_restore_to_work(int slot, uint32_t len, uint32_t crc,
                            void (*progress)(int pct));

#define JV_ERR_NO_MC          (-104) /* no usable Memory Card            */
#define JV_ERR_ARCHIVE_BAD    (-105) /* slot copy damaged / not valid     */
#define JV_ERR_ARCHIVE_VERIFY (-106) /* card read-back did not match      */

#endif
