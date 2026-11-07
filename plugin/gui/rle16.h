/* JamaisVu savestate plugin
 *
 * Lossless row compression for the 16-bit menu image. The menu is flat
 * colour, text and the slot thumbnails; a row is stored as a sequence of
 *   literal pixels      (short stretches of text and edges),
 *   repeated pixels     (the flat areas),
 *   references          (pixels equal to a stretch of a thumbnail that is
 *                        still in memory: the thumbnails are not stored
 *                        twice).
 * The encoder compares every pixel it refers to, so decoding gives back
 * exactly the row that was encoded, whatever the source. No SDK
 * dependencies: the host tests use it as it is.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __JV_RLE16_H__
#define __JV_RLE16_H__

#include <stdint.h>

/* A stretch of a row that may equal part of a reference image (a
 * thumbnail): row pixels x0 .. x0+len-1 are compared with
 * base[off + 0 .. len-1]. idx names the image for the decoder. */
typedef struct {
  const uint16_t *base;
  uint32_t off;
  int x0, len;
  uint8_t idx;
} Rle16Ref;

/* Encodes w pixels of src into dst (at most cap bytes). Returns the
 * number of bytes written, or -1 if cap is too small. Never longer than
 * RLE16_MAX_BYTES(w). */
int rle16_encode_row(const uint16_t *src, int w, const Rle16Ref *refs,
                     int nrefs, uint8_t *dst, int cap);

/* Decodes w pixels into dst from src (srclen bytes). refs[i] is the
 * reference image idx == i, each refpx pixels long. Returns 0, or -1 for
 * a malformed stream (the rest of dst is left zero); never reads or
 * writes out of bounds either way. */
int rle16_decode_row(const uint8_t *src, int srclen, uint16_t *dst, int w,
                     const uint16_t *const *refs, int nrefs, uint32_t refpx);

/* the longest encoding of a w-pixel row (all literals) */
#define RLE16_MAX_BYTES(w) (2 * (w) + ((w) + 127) / 128 + 8)

#endif
