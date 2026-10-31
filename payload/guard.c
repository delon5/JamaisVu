/* JamaisVu savestate plugin
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include "guard.h"
#include "msif/ms.h"

static SectorRange g_ranges[GUARD_MAX_RANGES];
static int g_num_ranges = 0;
static int g_violations = 0;

void guard_deny_all(void) {
  g_num_ranges = 0;
}

int guard_allow(uint32_t start, uint32_t count) {
  if (count == 0 || start + count < start)
    return -1;
  if (g_num_ranges >= GUARD_MAX_RANGES)
    return -1;
  g_ranges[g_num_ranges].start = start;
  g_ranges[g_num_ranges].count = count;
  g_num_ranges++;
  return 0;
}

int guard_is_allowed(uint32_t sector, uint32_t count) {
  if (count == 0 || sector + count < sector)
    return 0;

  /* Every sector must be covered. Ranges may be adjacent, so walk forward
   * through the request, consuming whichever range covers the next sector. */
  while (count > 0) {
    int found = 0;
    for (int i = 0; i < g_num_ranges; i++) {
      uint32_t s = g_ranges[i].start;
      uint32_t e = s + g_ranges[i].count;
      if (sector >= s && sector < e) {
        uint32_t n = e - sector;
        if (n > count)
          n = count;
        sector += n;
        count -= n;
        found = 1;
        break;
      }
    }
    if (!found)
      return 0;
  }

  return 1;
}

int guard_violations(void) {
  return g_violations;
}

int guarded_write_sectors(const void *buf, uint32_t sector, uint32_t count) {
  if (!guard_is_allowed(sector, count)) {
    g_violations++;
    return -1;
  }

  if (ms_write_sector(buf, (int)sector, (int)count) != 0)
    return -1;

  return 0;
}
