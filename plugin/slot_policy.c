/* JamaisVu savestate plugin
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include "slot_policy.h"

int slot_policy(int64_t free_bytes, const int *exists, int *locked, int n) {
  uint64_t new_slots = 0;
  int usable = 0;

  if (free_bytes > (int64_t)JV_FREE_SPACE_MARGIN)
    new_slots = ((uint64_t)free_bytes - JV_FREE_SPACE_MARGIN) / JV_SLOT_SIZE;

  for (int i = 0; i < n; i++) {
    if (exists[i]) {
      locked[i] = 0;
    } else if (new_slots > 0) {
      locked[i] = 0;
      new_slots--;
    } else {
      locked[i] = 1;
    }
    usable += !locked[i];
  }

  return usable;
}
