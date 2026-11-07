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

int slot_total(int64_t free_bytes, const int *exists, int n) {
  uint64_t creatable = 0;
  int highest = 0, count = 0;

  if (free_bytes > (int64_t)JV_FREE_SPACE_MARGIN)
    creatable = ((uint64_t)free_bytes - JV_FREE_SPACE_MARGIN) / JV_SLOT_SIZE;
  for (int i = 0; i < n; i++)
    if (exists[i]) {
      highest = i + 1;
      count++;
    }
  uint64_t total = (uint64_t)count + creatable;
  if (total < (uint64_t)highest)
    total = (uint64_t)highest;
  if (total > (uint64_t)n)
    total = (uint64_t)n;
  if (total > JV_MAX_SLOTS)
    total = JV_MAX_SLOTS;
  return (int)total;
}

int app_versions_match(const char *saved, const char *installed) {
  int i;
  if (!saved || !installed || !saved[0] || !installed[0])
    return 0;
  for (i = 0; saved[i] && installed[i]; i++)
    if (saved[i] != installed[i])
      return 0;
  return saved[i] == installed[i];
}
