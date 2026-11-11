/* JamaisVu savestate plugin
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __JV_FONT_H__
#define __JV_FONT_H__

#include <stdint.h>

typedef struct {
  int16_t advance;
  int16_t x_off; /* from pen position */
  int16_t y_off; /* from top of the line */
  uint16_t w, h;
  uint32_t offset; /* into bitmap */
} FontGlyph;

typedef struct {
  int height;
  int ascent;
  int first, last;
  const FontGlyph *glyphs;
  /* 4-bit coverage (0..15), two pixels a byte, the left one in the low
   * half; every row of a glyph starts on a byte. It was 8 bits a pixel,
   * 48 KB for the three sizes; the Japanese glyphs (jpfont.bin) have
   * always been stored with these 16 levels. */
  const uint8_t *bitmap;
} Font;

/* coverage 0..255 of pixel i in a glyph row of the packed bitmap */
static inline uint32_t font_cov4(const uint8_t *row, int i) {
  uint32_t b = row[i >> 1];
  return ((i & 1) ? b >> 4 : b & 15) * 17;
}
#define FONT_ROW_BYTES(w) (((w) + 1) / 2)

extern const Font font_large;  /* titles */
extern const Font font_medium; /* header */
extern const Font font_small;  /* hints, info */

#endif
