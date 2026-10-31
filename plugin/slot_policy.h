/* JamaisVu savestate plugin
 *
 * Which slots may be used, given the free space on ux0. Pure function so
 * it can be unit tested on the host (tests/host/test_slot_policy.c).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __SLOT_POLICY_H__
#define __SLOT_POLICY_H__

#include <stdint.h>

#ifndef JV_SLOT_SIZE /* host tests use smaller values */
#define JV_SLOT_SIZE 0x2C000000u /* 704 MiB per slot file */
#endif

/* Free space that must remain on ux0 after creating a slot file. */
#ifndef JV_FREE_SPACE_MARGIN
#define JV_FREE_SPACE_MARGIN (64ull * 1024 * 1024)
#endif

/*
 * A slot whose file already exists is always usable (its space is already
 * taken). Every other slot needs a new 704 MiB file, so the number of new
 * slots that can be opened is
 *
 *     (free - JV_FREE_SPACE_MARGIN) / JV_SLOT_SIZE
 *
 * e.g. 1 GB free -> 1 slot, 1.5 GB -> 2 slots, 2.2 GB -> 3 slots. They are
 * handed out to the lowest-numbered slots without a file; the rest are
 * locked.
 *
 * exists[i]: slot i already has a full-size file. free_bytes < 0 means the
 * free space is unknown, in which case only existing slots are usable.
 * Writes locked[i] and returns the number of usable slots.
 */
int slot_policy(int64_t free_bytes, const int *exists, int *locked, int n);

/*
 * A snapshot may only be loaded into exactly the game version it was taken
 * from (param.sfo APP_VER, e.g. "01.02"). Both versions must be known and
 * identical; anything else (an update, a downgrade, an unreadable
 * param.sfo) refuses the load.
 */
int app_versions_match(const char *saved, const char *installed);

#endif
