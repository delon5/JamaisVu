/* JamaisVu savestate plugin
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __CRC32_H__
#define __CRC32_H__

#include <stdint.h>

/* Standard CRC-32 (IEEE 802.3, reflected, init/xorout 0xffffffff).
 * Usage: c = crc32_update(0, buf, len); c = crc32_update(c, more, n); */
uint32_t crc32_update(uint32_t crc, const void *buf, uint32_t len);

#endif
