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

#include <stddef.h>
#include <stdint.h>
#include "font.h"

#define EXT_MAX_GLYPHS 128
#define EXT_BITMAP_SIZE (48 * 1024) /* the most a set holds: ~100 glyphs */

/* Glyphs for code points >= 0x80, drawn next to an ASCII Font on the
 * same baseline (8-bit coverage, like Font). Sorted by code point. */
typedef struct {
  int n;
  uint32_t used; /* bytes of bitmap in use */
  uint32_t cap;  /* bytes of bitmap (sized to the glyphs needed) */
  uint32_t cp[EXT_MAX_GLYPHS];
  FontGlyph g[EXT_MAX_GLYPHS];
  uint8_t bitmap[]; /* cap bytes */
} ExtGlyphs;

/* bytes of a set with cap bytes of bitmap */
#define EXT_SET_SIZE(cap) \
  ((uint32_t)((offsetof(ExtGlyphs, bitmap) + (cap) + 3) & ~3u))

/* an empty set in mem (EXT_SET_SIZE(cap) bytes) */
static inline ExtGlyphs *ext_init(void *mem, uint32_t cap) {
  ExtGlyphs *e = mem;
  e->n = 0;
  e->used = 0;
  e->cap = cap > EXT_BITMAP_SIZE ? EXT_BITMAP_SIZE : cap;
  return e;
}

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

/* Bitmap bytes a fresh set needs for the code points >= 0x80 of the given
 * strings that the file has (each counted once, as many as a set holds);
 * < 0 if the file is missing or not a jpfont.bin. */
int jpfont_measure(JfRead rd, void *ctx, const char *const *strs, int nstrs,
                   uint32_t *bytes);

/* 1 if ext has every code point >= 0x80 of the given strings (invalid
 * bytes, U+FFFD, are not asked for) */
int ext_has_all(const ExtGlyphs *ext, const char *const *strs, int nstrs);

/* The glyph for cp, or NULL */
const FontGlyph *ext_glyph(const ExtGlyphs *ext, uint32_t cp);

#endif
