/* JamaisVu savestate plugin
 *
 * The payload uses the menu's renderer (plugin/gui/render.c) for its
 * progress screen, ASCII text only. These stand in for jpfont.c (Japanese
 * glyphs and its 32 KB buffer), which the payload has no room for: the
 * renderer's UTF-8 functions then treat every byte as one character and
 * draw only ASCII.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stddef.h>
#include "jpfont.h"

uint32_t utf8_next(const char **s) {
  uint32_t c = (uint8_t)**s;
  if (c)
    (*s)++;
  return c;
}

const FontGlyph *ext_glyph(const ExtGlyphs *ext, uint32_t cp) {
  (void)ext;
  (void)cp;
  return NULL;
}
