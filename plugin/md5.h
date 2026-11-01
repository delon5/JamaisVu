/* JamaisVu savestate plugin
 *
 * MD5 (RFC 1321) for fingerprinting config.txt.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __JV_MD5_H__
#define __JV_MD5_H__

#include <stdint.h>

typedef struct {
  uint32_t h[4];
  uint64_t len;
  uint8_t buf[64];
  uint32_t fill;
} Md5;

void md5_init(Md5 *m);
void md5_update(Md5 *m, const void *data, uint32_t len);
void md5_final(Md5 *m, uint8_t out[16]);

#endif
