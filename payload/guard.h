/* JamaisVu savestate plugin
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __GUARD_H__
#define __GUARD_H__

#include <stdint.h>

/* Sector write guard.
 *
 * Every write the payload makes to the Memory Card goes through
 * guarded_write_sectors(). A write is only passed to the card driver if
 * every sector it touches lies inside one of the currently allowed ranges.
 * The default (and the state after guard_deny_all()) allows nothing.
 *
 *  - save: only the sectors of the pre-allocated slot file are allowed, so
 *    a save cannot change any filesystem metadata or any other file.
 *  - load: only the exFAT FAT/bitmap/data area of ux0 is allowed while the
 *    savedata is restored (never the partition table or boot sectors), and
 *    exactly one sector while the mediaid sector is restored.
 */

typedef struct {
  uint32_t start; /* first sector */
  uint32_t count; /* number of sectors */
} SectorRange;

#define GUARD_MAX_RANGES 4096

void guard_deny_all(void);
int guard_allow(uint32_t start, uint32_t count);
int guard_is_allowed(uint32_t sector, uint32_t count);
int guard_violations(void);

/* Returns 0 on success, < 0 on error (including a blocked write). */
int guarded_write_sectors(const void *buf, uint32_t sector, uint32_t count);

#endif
