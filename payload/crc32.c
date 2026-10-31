/* JamaisVu savestate plugin
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include "crc32.h"

static uint32_t crc_table[256];
static int crc_table_ready = 0;

static void crc32_init(void) {
  for (uint32_t i = 0; i < 256; i++) {
    uint32_t c = i;
    for (int k = 0; k < 8; k++)
      c = (c & 1) ? (0xedb88320u ^ (c >> 1)) : (c >> 1);
    crc_table[i] = c;
  }
  crc_table_ready = 1;
}

uint32_t crc32_update(uint32_t crc, const void *buf, uint32_t len) {
  const uint8_t *p = buf;

  if (!crc_table_ready)
    crc32_init();

  crc = ~crc;
  while (len--)
    crc = crc_table[(crc ^ *p++) & 0xff] ^ (crc >> 8);
  return ~crc;
}
