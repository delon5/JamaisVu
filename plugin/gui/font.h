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
  const uint8_t *bitmap; /* 8-bit coverage */
} Font;

extern const Font font_large;  /* titles */
extern const Font font_medium; /* header */
extern const Font font_small;  /* hints, info */

#endif
