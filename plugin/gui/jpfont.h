/* JamaisVu savestate plugin
 *
 * Japanese game names: glyphs read on demand from jpfont.bin (made by
 * tools/gen_jpfont.py) into a small set the menu draws from. Plain C, no
 * SDK dependencies: file access goes through a callback, so the same
 * code runs in the host tests.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __JV_JPFONT_H__
#define __JV_JPFONT_H__

#include <stdint.h>
#include "font.h"

#define EXT_MAX_GLYPHS 160
#define EXT_BITMAP_SIZE (64 * 1024)

/* Glyphs for code points >= 0x80, drawn next to an ASCII Font on the
 * same baseline (8-bit coverage, like Font). Sorted by code point. */
typedef struct {
  int n;
  uint32_t used; /* bytes of bitmap in use */
  uint32_t cp[EXT_MAX_GLYPHS];
  FontGlyph g[EXT_MAX_GLYPHS];
  uint8_t bitmap[EXT_BITMAP_SIZE];
} ExtGlyphs;

/* Reads len bytes at offset off; returns the number read, < 0 on error. */
typedef int (*JfRead)(void *ctx, uint32_t off, void *buf, uint32_t len);

/* Decodes one UTF-8 code point and advances *s (invalid bytes: U+FFFD).
 * Returns 0 at the end of the string. */
uint32_t utf8_next(const char **s);

/* Collects the code points >= 0x80 of the given strings that are not in
 * ext yet, then loads them. Glyphs the file does not have are skipped;
 * when ext is full, the rest is skipped. Returns the number of glyphs
 * in ext, or < 0 if the file is missing or not a jpfont.bin. */
int jpfont_load(JfRead rd, void *ctx, const char *const *strs, int nstrs,
                ExtGlyphs *ext);

/* The glyph for cp, or NULL */
const FontGlyph *ext_glyph(const ExtGlyphs *ext, uint32_t cp);

#endif
