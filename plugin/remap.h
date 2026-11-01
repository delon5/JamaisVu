/* JamaisVu savestate plugin
 *
 * ✕/○ convention of the running game, see remap.c. Pure functions (no SDK),
 * also built by the host tests.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __JV_REMAP_H__
#define __JV_REMAP_H__

#include <stdint.h>

/* PSV-Shell-Plus profile file contents: 1 = swaps ✕/○, 0 = no, -1 = not
 * a profile this code understands. */
int psvs_profile_swaps(const uint8_t *buf, int len);

/* reVita profile INI text: 1 if an enabled rule maps ✕ -> ○ or ○ -> ✕. */
int revita_ini_swaps(const char *text, int len);

/* Japanese / Asian title IDs, whose games confirm with ○. */
int title_confirms_with_circle(const char *tid);

/* 1 if the menu should confirm with ○ (physical) in this game: the game's
 * convention, flipped once for each plugin that swaps ✕/○ for it. */
int menu_swap_ox(const char *tid, int psvs_swap, int revita_swap);

#endif
