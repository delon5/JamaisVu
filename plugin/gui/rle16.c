/* JamaisVu savestate plugin
 *
 * Lossless row compression for the 16-bit menu image, see rle16.h.
 *
 * Stream: a sequence of operations, each a one-byte code
 *   0x00..0x7F  literal: code+1 pixels follow (1..128), 2 bytes each
 *   0x80..0xBF  repeat: the next pixel (2 bytes) (code & 0x3F) + 2 times
 *   0xC0        repeat long: count (2 bytes, LE), then the pixel (2 bytes)
 *   0xC1        reference: image index (1), pixel offset (2, LE), count (1)
 * Pixels are little-endian RGB565 as the image holds them.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include "rle16.h"

#define OP_REPEAT 0x80
#define OP_REPEAT_MAX 65
#define OP_REPEAT_LONG 0xC0
#define OP_REF 0xC1
#define LIT_MAX 128
#define REF_MIN 3   /* a reference pays from 3 pixels (5 bytes vs 6) */
#define REF_MAX 255

/* the longest match of src[x..] with the reference that covers x, and
 * which one (-1: none covers x or none matches REF_MIN pixels) */
static int ref_match(const uint16_t *src, int w, int x, const Rle16Ref *refs,
                     int nrefs, int *which) {
  int best = 0, bi = -1;
  for (int i = 0; i < nrefs; i++) {
    const Rle16Ref *r = &refs[i];
    if (!r->base || x < r->x0 || x >= r->x0 + r->len)
      continue;
    int lim = r->x0 + r->len - x;
    if (lim > w - x)
      lim = w - x;
    if (lim > REF_MAX)
      lim = REF_MAX;
    const uint16_t *p = r->base + r->off + (x - r->x0);
    int m = 0;
    while (m < lim && src[x + m] == p[m])
      m++;
    if (m > best) {
      best = m;
      bi = i;
    }
  }
  if (best < REF_MIN)
    return 0;
  *which = bi;
  return best;
}

static int run_len(const uint16_t *src, int w, int x) {
  int n = 1;
  while (x + n < w && src[x + n] == src[x] && n < 65535)
    n++;
  return n;
}

static inline void put16(uint8_t *d, uint16_t v) {
  d[0] = (uint8_t)v;
  d[1] = (uint8_t)(v >> 8);
}

int rle16_encode_row(const uint16_t *src, int w, const Rle16Ref *refs,
                     int nrefs, uint8_t *dst, int cap) {
  int x = 0, o = 0;
  while (x < w) {
    int which, m = ref_match(src, w, x, refs, nrefs, &which);
    if (m) {
      if (o + 5 > cap)
        return -1;
      const Rle16Ref *r = &refs[which];
      dst[o++] = OP_REF;
      dst[o++] = r->idx;
      put16(dst + o, (uint16_t)(r->off + (uint32_t)(x - r->x0)));
      o += 2;
      dst[o++] = (uint8_t)m;
      x += m;
      continue;
    }
    int run = run_len(src, w, x);
    if (run >= 2) {
      if (run <= OP_REPEAT_MAX) {
        if (o + 3 > cap)
          return -1;
        dst[o++] = (uint8_t)(OP_REPEAT | (run - 2));
      } else {
        if (o + 5 > cap)
          return -1;
        dst[o++] = OP_REPEAT_LONG;
        put16(dst + o, (uint16_t)run);
        o += 2;
      }
      put16(dst + o, src[x]);
      o += 2;
      x += run;
      continue;
    }
    /* literals, up to where a run of 3 or a reference match starts */
    int e = x + 1;
    while (e < w && e - x < LIT_MAX) {
      if (e + 2 < w && src[e] == src[e + 1] && src[e] == src[e + 2])
        break;
      if (ref_match(src, w, e, refs, nrefs, &which) >= REF_MIN + 1)
        break;
      e++;
    }
    int n = e - x;
    if (o + 1 + 2 * n > cap)
      return -1;
    dst[o++] = (uint8_t)(n - 1);
    for (int i = 0; i < n; i++, o += 2)
      put16(dst + o, src[x + i]);
    x = e;
  }
  return o;
}

static inline uint16_t get16(const uint8_t *s) {
  return (uint16_t)(s[0] | (s[1] << 8));
}

int rle16_decode_row(const uint8_t *src, int srclen, uint16_t *dst, int w,
                     const uint16_t *const *refs, int nrefs, uint32_t refpx) {
  int x = 0, i = 0;
  while (x < w) {
    if (i >= srclen)
      goto bad;
    uint8_t op = src[i++];
    if (op < OP_REPEAT) {
      int n = op + 1;
      if (n > w - x || i + 2 * n > srclen)
        goto bad;
      for (int k = 0; k < n; k++, i += 2)
        dst[x + k] = get16(src + i);
      x += n;
    } else if (op < OP_REPEAT_LONG) {
      int n = (op & 0x3F) + 2;
      if (n > w - x || i + 2 > srclen)
        goto bad;
      uint16_t v = get16(src + i);
      i += 2;
      for (int k = 0; k < n; k++)
        dst[x + k] = v;
      x += n;
    } else if (op == OP_REPEAT_LONG) {
      if (i + 4 > srclen)
        goto bad;
      int n = get16(src + i);
      uint16_t v = get16(src + i + 2);
      i += 4;
      if (n < 1 || n > w - x)
        goto bad;
      for (int k = 0; k < n; k++)
        dst[x + k] = v;
      x += n;
    } else if (op == OP_REF) {
      if (i + 4 > srclen)
        goto bad;
      int idx = src[i], n = src[i + 3];
      uint32_t off = get16(src + i + 1);
      i += 4;
      if (idx >= nrefs || !refs[idx] || n < 1 || n > w - x ||
          off + (uint32_t)n > refpx)
        goto bad;
      const uint16_t *p = refs[idx] + off;
      for (int k = 0; k < n; k++)
        dst[x + k] = p[k];
      x += n;
    } else {
      goto bad;
    }
  }
  return 0;
bad:
  for (; x < w; x++)
    dst[x] = 0;
  return -1;
}
