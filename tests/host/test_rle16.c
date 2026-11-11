/* JamaisVu host test: the menu image's row compression gives back exactly
 * what went in, stays within its size bound, and a damaged stream never
 * reads or writes out of bounds.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "rle16.h"

#define W 880
#define THUMB_W 160
#define THUMB_PX (THUMB_W * 90)

static int fails;
#define CHECK(cond, what)                       \
  do {                                          \
    if (cond)                                   \
      printf("  %-62s ok\n", what);             \
    else {                                      \
      printf("  %-62s FAILED\n", what);         \
      fails++;                                  \
    }                                           \
  } while (0)

static uint32_t seed = 12345;
static uint32_t rnd(void) {
  seed = seed * 1103515245u + 12345u;
  return seed >> 8;
}

static uint16_t thumbs[5][THUMB_PX];
static const uint16_t *thumb_ptrs[5];

/* tables for the 32-bit decoder: any three will do, each channel in its
 * own bits as in render.h */
static uint32_t t_r5[32], t_g6[64], t_b5[32];
static uint32_t conv32(uint16_t v) {
  return t_r5[v >> 11] | t_g6[(v >> 5) & 63] | t_b5[v & 31] | 0xff000000u;
}

/* encode, decode, compare; returns the encoded size or -1 */
static int roundtrip(const uint16_t *row, const Rle16Ref *refs, int nrefs,
                     const char *what) {
  static uint8_t enc[RLE16_MAX_BYTES(W) + 64];
  static uint16_t dec[W];
  memset(enc, 0xAA, sizeof(enc));
  int n = rle16_encode_row(row, W, refs, nrefs, enc, sizeof(enc));
  if (n < 0 || n > RLE16_MAX_BYTES(W)) {
    printf("  %-62s FAILED (size %d, bound %d)\n", what, n,
           RLE16_MAX_BYTES(W));
    fails++;
    return -1;
  }
  memset(dec, 0x55, sizeof(dec));
  int r = rle16_decode_row(enc, n, dec, W, thumb_ptrs, 5, THUMB_PX);
  if (r != 0 || memcmp(dec, row, sizeof(dec)) != 0) {
    printf("  %-62s FAILED (decode %d, pixels differ)\n", what, r);
    fails++;
    return -1;
  }
  /* straight to 32 bits: the same pixels, converted */
  {
    static uint32_t dec32[W + 2];
    int same = 1;
    dec32[0] = dec32[W + 1] = 0x12345678u;
    int r32 = rle16_decode_row_rgba(enc, n, dec32 + 1, W, thumb_ptrs, 5,
                                    THUMB_PX, t_r5, t_g6, t_b5);
    for (int x = 0; x < W; x++)
      same &= dec32[1 + x] == conv32(row[x]);
    if (r32 != 0 || !same || dec32[0] != 0x12345678u ||
        dec32[W + 1] != 0x12345678u) {
      printf("  %-62s FAILED (32-bit decode %d, pixels %s)\n", what, r32,
             same ? "written out of bounds" : "differ");
      fails++;
      return -1;
    }
  }
  /* an exact cap must still work; one byte less must be refused */
  CHECK(rle16_encode_row(row, W, refs, nrefs, enc, n) == n &&
            rle16_encode_row(row, W, refs, nrefs, enc, n - 1) == -1,
        "  encoder respects its cap");
  return n;
}

int main(void) {
  static uint16_t row[W];
  Rle16Ref refs[5];
  int n;
  for (int i = 0; i < 32; i++) {
    t_r5[i] = (uint32_t)(i * 8 + 3);
    t_b5[i] = (uint32_t)(i * 8 + 5) << 16;
  }
  for (int i = 0; i < 64; i++)
    t_g6[i] = (uint32_t)(i * 4 + 1) << 8;

  for (int t = 0; t < 5; t++) {
    for (int i = 0; i < THUMB_PX; i++)
      thumbs[t][i] = (uint16_t)rnd();
    thumb_ptrs[t] = thumbs[t];
  }

  printf("=== menu image row compression ===\n");

  /* flat row */
  for (int x = 0; x < W; x++)
    row[x] = 0x1234;
  n = roundtrip(row, NULL, 0, "flat row");
  CHECK(n > 0 && n <= 8, "flat row: a few bytes");

  /* noise: the bound holds, nothing worse than literals */
  for (int x = 0; x < W; x++)
    row[x] = (uint16_t)rnd();
  n = roundtrip(row, NULL, 0, "random row");
  CHECK(n > 0 && n <= RLE16_MAX_BYTES(W), "random row within the bound");

  /* alternating pairs: the worst case for run splitting */
  for (int x = 0; x < W; x++)
    row[x] = (uint16_t)((x / 2) & 1 ? 0xFFFF : (x & 1) * 0x0F0F);
  n = roundtrip(row, NULL, 0, "alternating short runs");
  CHECK(n > 0 && n <= RLE16_MAX_BYTES(W), "alternating runs within bound");

  /* text-like: flat with scattered pixels */
  for (int x = 0; x < W; x++)
    row[x] = (rnd() % 7 == 0) ? (uint16_t)rnd() : 0x4208;
  n = roundtrip(row, NULL, 0, "text-like row");
  printf("  text-like row: %d bytes of %d\n", n, W * 2);

  /* thumbnails referenced: five 160-pixel stretches, flat in between */
  for (int x = 0; x < W; x++)
    row[x] = 0x4208;
  for (int t = 0; t < 5; t++) {
    int x0 = 10 + t * 172;
    refs[t].base = thumbs[t];
    refs[t].off = 37 * THUMB_W; /* row 37 of each */
    refs[t].x0 = x0;
    refs[t].len = THUMB_W;
    refs[t].idx = (uint8_t)t;
    memcpy(row + x0, thumbs[t] + 37 * THUMB_W, THUMB_W * 2);
  }
  n = roundtrip(row, refs, 5, "row with five thumbnail stretches");
  CHECK(n > 0 && n < 60, "thumbnails stored as references, not pixels");
  printf("  five thumbnail stretches: %d bytes of %d\n", n, W * 2);

  /* partial matches: text over part of a thumbnail */
  row[10 + 20] = 0x1111;
  row[10 + 21] = 0x2222;
  row[10 + 2 * 172 + 159] = 0x3333; /* the last pixel of the third */
  n = roundtrip(row, refs, 5, "thumbnails partly drawn over");
  CHECK(n > 0 && n < 100, "partly drawn-over thumbnails mostly referenced");

  /* a reference whose base is NULL is ignored */
  refs[1].base = NULL;
  n = roundtrip(row, refs, 5, "a missing thumbnail falls back to pixels");
  CHECK(n > 0 && n > 320, "missing thumbnail stored as pixels");
  refs[1].base = thumbs[1];

  /* the same with the references given but nothing matching */
  for (int x = 0; x < W; x++)
    row[x] = (uint16_t)rnd();
  n = roundtrip(row, refs, 5, "random row with references offered");

  /* damaged streams: decode must fail cleanly, never crash */
  {
    static uint8_t enc[RLE16_MAX_BYTES(W)];
    static uint16_t dec[W];
    for (int x = 0; x < W; x++)
      row[x] = 0x4208;
    memcpy(row + 10, thumbs[0] + 37 * THUMB_W, THUMB_W * 2);
    int len = rle16_encode_row(row, W, refs, 1, enc, sizeof(enc));
    int ok = 1, ok32 = 1;
    static uint32_t dec32[W];
    for (int cut = 0; cut < len; cut++) {
      int r = rle16_decode_row(enc, cut, dec, W, thumb_ptrs, 5, THUMB_PX);
      if (r != -1)
        ok = 0;
      /* the 32-bit decoder: refused alike, and the same row comes out
       * (what was decoded, then opaque black) */
      r = rle16_decode_row_rgba(enc, cut, dec32, W, thumb_ptrs, 5, THUMB_PX,
                                t_r5, t_g6, t_b5);
      if (r != -1)
        ok32 = 0;
      for (int x = 0; x < W; x++)
        if (dec32[x] != conv32(dec[x]))
          ok32 = 0;
    }
    CHECK(ok, "every truncation of a stream is refused");
    CHECK(ok32, "the 32-bit decoder refuses them alike, same pixels");
    /* reference beyond the thumbnail */
    uint8_t b1[] = { 0xC1, 0, 0xFF, 0xFF, 10 };
    CHECK(rle16_decode_row(b1, 5, dec, W, thumb_ptrs, 5, THUMB_PX) == -1,
          "reference past the end of a thumbnail refused");
    uint8_t b2[] = { 0xC1, 7, 0, 0, 10 };
    CHECK(rle16_decode_row(b2, 5, dec, W, thumb_ptrs, 5, THUMB_PX) == -1,
          "reference to a missing thumbnail refused");
    uint8_t b3[] = { 0xC0, 0xFF, 0xFF, 0x34, 0x12 };
    CHECK(rle16_decode_row(b3, 5, dec, W, thumb_ptrs, 5, THUMB_PX) == -1,
          "repeat longer than the row refused");
    uint8_t b4[] = { 0xC2 };
    CHECK(rle16_decode_row(b4, 1, dec, W, thumb_ptrs, 5, THUMB_PX) == -1,
          "unknown operation refused");
    /* random garbage, many times: no crash, no out-of-bounds (ASan) */
    for (int t = 0; t < 2000; t++) {
      int l = 1 + (int)(rnd() % 64);
      for (int i = 0; i < l; i++)
        enc[i] = (uint8_t)rnd();
      rle16_decode_row(enc, l, dec, W, thumb_ptrs, 5, THUMB_PX);
      rle16_decode_row_rgba(enc, l, dec32, W, thumb_ptrs, 5, THUMB_PX, t_r5,
                            t_g6, t_b5);
    }
    CHECK(1, "2000 random streams decoded without fault");
  }

  if (fails) {
    printf("RLE16 TESTS FAILED (%d)\n", fails);
    return 1;
  }
  printf("rle16 tests passed\n");
  return 0;
}
