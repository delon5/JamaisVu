/* JamaisVu savestate plugin
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stddef.h>
#include <string.h>

#include "jpfont.h"

#define JF_BLOCK 64
#define JF_MAX_BLOCKS 256
#define JF_ENTRY 16

static uint32_t rd32(const uint8_t *p) {
  return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint32_t utf8_next(const char **s) {
  const uint8_t *p = (const uint8_t *)*s;
  uint32_t c = p[0];
  int n;

  if (c == 0)
    return 0;
  if (c < 0x80) {
    *s += 1;
    return c;
  }
  if ((c & 0xe0) == 0xc0) {
    n = 1;
    c &= 0x1f;
  } else if ((c & 0xf0) == 0xe0) {
    n = 2;
    c &= 0x0f;
  } else if ((c & 0xf8) == 0xf0) {
    n = 3;
    c &= 0x07;
  } else {
    *s += 1;
    return 0xfffd;
  }
  for (int i = 1; i <= n; i++) {
    if ((p[i] & 0xc0) != 0x80) {
      *s += i; /* stop before the byte that does not belong */
      return 0xfffd;
    }
    c = (c << 6) | (p[i] & 0x3f);
  }
  *s += n + 1;
  return c;
}

const FontGlyph *ext_glyph(const ExtGlyphs *ext, uint32_t cp) {
  if (!ext)
    return NULL;
  int lo = 0, hi = ext->n - 1;
  if (hi >= EXT_MAX_GLYPHS)
    hi = EXT_MAX_GLYPHS - 1;
  while (lo <= hi) {
    int mid = (lo + hi) / 2;
    if (ext->cp[mid] == cp) {
      const FontGlyph *g = &ext->g[mid];
      /* never point outside the bitmap, whatever is in the set */
      if (g->offset > ext->cap ||
          (uint32_t)g->w * g->h > ext->cap - g->offset)
        return NULL;
      return g;
    }
    if (ext->cp[mid] < cp)
      lo = mid + 1;
    else
      hi = mid - 1;
  }
  return NULL;
}

static void insert(ExtGlyphs *ext, uint32_t cp, const FontGlyph *g) {
  int i = ext->n;
  while (i > 0 && ext->cp[i - 1] > cp) {
    ext->cp[i] = ext->cp[i - 1];
    ext->g[i] = ext->g[i - 1];
    i--;
  }
  ext->cp[i] = cp;
  ext->g[i] = *g;
  ext->n++;
}

static int wanted_has(const uint32_t *w, int n, uint32_t cp) {
  for (int i = 0; i < n; i++)
    if (w[i] == cp)
      return 1;
  return 0;
}

int ext_has_all(const ExtGlyphs *ext, const char *const *strs, int nstrs) {
  for (int k = 0; k < nstrs; k++) {
    const char *s = strs[k];
    uint32_t cp;
    while (s && (cp = utf8_next(&s)) != 0)
      if (cp >= 0x80 && cp != 0xfffd && !ext_glyph(ext, cp))
        return 0; /* (a letter jpfont.bin lacks counts as new: the set is
                   * then simply loaded again, the names stay right) */
  }
  return 1;
}

/* loads into ext, or with ext NULL only adds up the bitmap bytes */
static int jf_run(JfRead rd, void *ctx, const char *const *strs, int nstrs,
                  ExtGlyphs *ext, uint32_t *bytes) {
  static uint8_t hdr[32];
  static uint32_t index[JF_MAX_BLOCKS];
  static uint8_t block[JF_BLOCK * JF_ENTRY];
  static uint8_t packed[(255 * 255 + 1) / 2];
  static uint32_t want[EXT_MAX_GLYPHS];
  int nwant = 0;

  /* code points still missing */
  for (int k = 0; k < nstrs; k++) {
    const char *s = strs[k];
    uint32_t cp;
    while (s && (cp = utf8_next(&s)) != 0) {
      if (cp < 0x80 || ext_glyph(ext, cp) || wanted_has(want, nwant, cp))
        continue;
      if ((ext ? ext->n : 0) + nwant >= EXT_MAX_GLYPHS)
        break;
      want[nwant++] = cp;
    }
  }

  if (rd(ctx, 0, hdr, sizeof(hdr)) != (int)sizeof(hdr) ||
      memcmp(hdr, "JVJF", 4) != 0 || hdr[4] != 1 || hdr[5] != 0)
    return -1;
  uint32_t count = rd32(hdr + 8), nblocks = rd32(hdr + 12);
  uint32_t index_off = rd32(hdr + 16), entries_off = rd32(hdr + 20);
  uint32_t data_off = rd32(hdr + 24), data_size = rd32(hdr + 28);
  if (nblocks == 0 || nblocks > JF_MAX_BLOCKS ||
      count > nblocks * JF_BLOCK || count <= (nblocks - 1) * JF_BLOCK)
    return -1;
  if (bytes)
    *bytes = 0;
  if (nwant == 0)
    return ext ? ext->n : 0;
  if (rd(ctx, index_off, index, nblocks * 4) != (int)(nblocks * 4))
    return -1;
  for (uint32_t b = 0; b < nblocks; b++) {
    const uint8_t *p = (const uint8_t *)&index[b];
    index[b] = rd32(p); /* file is little endian */
  }

  int cur = -1;
  for (int k = 0; k < nwant; k++) {
    uint32_t cp = want[k];
    /* last block starting at or before cp */
    int lo = 0, hi = (int)nblocks - 1, b = -1;
    while (lo <= hi) {
      int mid = (lo + hi) / 2;
      if (index[mid] <= cp) {
        b = mid;
        lo = mid + 1;
      } else {
        hi = mid - 1;
      }
    }
    if (b < 0)
      continue;
    uint32_t first = (uint32_t)b * JF_BLOCK;
    uint32_t n = count - first < JF_BLOCK ? count - first : JF_BLOCK;
    if (b != cur) {
      if (rd(ctx, entries_off + first * JF_ENTRY, block, n * JF_ENTRY) !=
          (int)(n * JF_ENTRY))
        return -1;
      cur = b;
    }
    for (uint32_t e = 0; e < n; e++) {
      const uint8_t *p = &block[e * JF_ENTRY];
      if (rd32(p) != cp)
        continue;
      FontGlyph g;
      uint32_t off = rd32(p + 4);
      g.w = p[8];
      g.h = p[9];
      g.x_off = (int8_t)p[10];
      g.y_off = (int8_t)p[11];
      g.advance = p[12];
      uint32_t npx = (uint32_t)g.w * g.h, nbytes = (npx + 1) / 2;
      if (off > data_size || nbytes > data_size - off)
        break; /* a damaged entry: skip this glyph */
      if (!ext) { /* measuring */
        if (npx <= EXT_BITMAP_SIZE - *bytes)
          *bytes += npx;
        break;
      }
      if (npx > ext->cap - ext->used)
        break; /* full: skip this glyph */
      if (npx && rd(ctx, data_off + off, packed, nbytes) != (int)nbytes)
        return -1;
      g.offset = ext->used;
      for (uint32_t i = 0; i < npx; i++) {
        uint8_t v = (i & 1) ? packed[i / 2] & 15 : packed[i / 2] >> 4;
        ext->bitmap[ext->used + i] = (uint8_t)(v * 17);
      }
      ext->used += npx;
      insert(ext, cp, &g);
      break;
    }
  }
  return ext ? ext->n : 0;
}

int jpfont_load(JfRead rd, void *ctx, const char *const *strs, int nstrs,
                ExtGlyphs *ext) {
  return jf_run(rd, ctx, strs, nstrs, ext, NULL);
}

int jpfont_measure(JfRead rd, void *ctx, const char *const *strs, int nstrs,
                   uint32_t *bytes) {
  uint32_t b = 0;
  int r = jf_run(rd, ctx, strs, nstrs, NULL, &b);
  *bytes = b;
  return r < 0 ? r : 0;
}
