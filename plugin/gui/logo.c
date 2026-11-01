/* JamaisVu savestate plugin
 *
 * The JamaisVu logo ("cube pixel" letters), shared by the in-game menu and
 * the payload's progress screen so both look the same.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stddef.h>
#include <stdint.h>

#include "logo.h"

#define C_LOGO_A     RGBA(52, 64, 88, 255)
#define C_LOGO_B     RGBA(34, 168, 214, 255)

/* --- Logo ----------------------------------------------------------------
 *
 * "Cube pixel" letters on an isometric triangle lattice (vertical edges and
 * 30 degree slants, hexagonal tops and bottoms), dissolving upwards.
 *
 * Lattice coordinates: x in columns, y in half triangle sides. Triangle
 * T(x,y) points right if (x+y) is even: (x,y) (x,y+2) (x+1,y+1), otherwise
 * left: (x+1,y) (x+1,y+2) (x,y+1). A glyph is a set of "add" polygons minus
 * "sub" polygons; a triangle belongs to the glyph if its centroid does.
 */
/* g_logo_scale: 1 in the menu; the payload's progress screen draws the
 * same logo larger (menu_view_logo) */
static int g_logo_scale = 1;
#define LOGO_COL   (32 * g_logo_scale) /* column width, 1/8 px (4 px)  */
#define LOGO_HALF  (18 * g_logo_scale) /* half triangle side, 1/8 px   */
#define LOGO_SHRINK 84 /* triangle size in % (the rest is the gap) */

typedef struct { int8_t x, y; } LPt;
typedef struct { uint8_t n; LPt p[16]; } LPoly;
typedef struct {
  char ch;
  uint8_t width, nadd, nsub;
  LPoly poly[4]; /* adds first, then subs */
} LatticeGlyph;

/* 6 columns wide, 14 half-sides tall, pointed (hexagonal) top and bottom */
#define HEX6 { 6, { {0,4}, {3,1}, {6,4}, {6,12}, {3,15}, {0,12} } }

static const LatticeGlyph logo_glyphs[] = {
  { 'J', 6, 1, 1, { HEX6,
      { 7, { {-1,-5}, {4,-5}, {4,10}, {3,11}, {2,10}, {2,8}, {-1,5} } } } },
  { 'A', 6, 1, 2, { HEX6,
      { 6, { {2,6}, {3,5}, {4,6}, {4,7}, {3,8}, {2,7} } },
      { 5, { {2,11}, {3,10}, {4,11}, {4,20}, {2,20} } } } },
  { 'M', 8, 1, 2, {
      { 12, { {0,4}, {2,2}, {4,4}, {6,2}, {8,4}, {8,12}, {6,14}, {6,12},
              {4,14}, {2,12}, {2,14}, {0,12} } },
      { 4, { {2,6}, {3,7}, {3,20}, {2,20} } },
      { 4, { {5,7}, {6,6}, {6,20}, {5,20} } } } },
  { 'I', 4, 1, 2, {
      { 6, { {0,4}, {2,2}, {4,4}, {4,12}, {2,14}, {0,12} } },
      { 4, { {-1,5}, {1,7}, {1,9}, {-1,11} } },
      { 4, { {5,5}, {3,7}, {3,9}, {5,11} } } } },
  { 'S', 6, 1, 2, { HEX6,
      { 4, { {2,6}, {3,5}, {7,9}, {7,11} } },
      { 4, { {-1,7}, {-1,9}, {3,13}, {4,12} } } } },
  { 'V', 6, 1, 3, { HEX6,
      { 5, { {2,-5}, {4,-5}, {4,9}, {3,10}, {2,9} } },
      { 4, { {-2,10}, {0,10}, {2,14}, {-2,16} } },
      { 4, { {8,10}, {6,10}, {4,14}, {8,16} } } } },
  { 'U', 6, 1, 1, { HEX6,
      { 5, { {2,-5}, {4,-5}, {4,10}, {3,11}, {2,10} } } } },
};

static const LatticeGlyph *logo_glyph(char ch) {
  for (size_t i = 0; i < sizeof(logo_glyphs) / sizeof(logo_glyphs[0]); i++)
    if (logo_glyphs[i].ch == ch)
      return &logo_glyphs[i];
  return NULL;
}

/* point (px,py) in units of 1/3 lattice step */
static int lpoly_inside(const LPoly *poly, int px, int py) {
  int c = 0;
  for (int i = 0, j = poly->n - 1; i < poly->n; j = i++) {
    int x1 = poly->p[j].x * 3, y1 = poly->p[j].y * 3;
    int x2 = poly->p[i].x * 3, y2 = poly->p[i].y * 3;
    if ((y1 > py) != (y2 > py)) {
      int lhs = (px - x1) * (y2 - y1);
      int rhs = (py - y1) * (x2 - x1);
      if (y2 > y1 ? lhs < rhs : lhs > rhs)
        c = !c;
    }
  }
  return c;
}

static int glyph_has(const LatticeGlyph *g, int x, int y) {
  int px = x * 3 + (((x + y) & 1) ? 2 : 1);
  int py = y * 3 + 3;
  int in = 0;
  for (int i = 0; i < g->nadd; i++)
    in |= lpoly_inside(&g->poly[i], px, py);
  if (!in)
    return 0;
  for (int i = 0; i < g->nsub; i++)
    if (lpoly_inside(&g->poly[g->nadd + i], px, py))
      return 0;
  return 1;
}

static uint32_t logo_rand(uint32_t *seed) {
  *seed = *seed * 1103515245u + 12345u;
  return (*seed >> 16) & 0x7fff;
}

/* ox, oy: canvas position of lattice origin, in 1/8 px */
static void logo_tri(Canvas *c, int ox, int oy, int x, int y, int shrink,
                     uint32_t color) {
  int vx[3], vy[3];
  if (((x + y) & 1) == 0) {
    vx[0] = x;     vy[0] = y;
    vx[1] = x;     vy[1] = y + 2;
    vx[2] = x + 1; vy[2] = y + 1;
  } else {
    vx[0] = x + 1; vy[0] = y;
    vx[1] = x + 1; vy[1] = y + 2;
    vx[2] = x;     vy[2] = y + 1;
  }
  int px[3], py[3], cx = 0, cy = 0;
  for (int i = 0; i < 3; i++) {
    px[i] = ox + vx[i] * LOGO_COL;
    py[i] = oy + vy[i] * LOGO_HALF;
    cx += px[i];
    cy += py[i];
  }
  cx /= 3;
  cy /= 3;
  for (int i = 0; i < 3; i++) {
    px[i] = cx + (px[i] - cx) * shrink / 100;
    py[i] = cy + (py[i] - cy) * shrink / 100;
  }
  cv_triangle(c, px[0], py[0], px[1], py[1], px[2], py[2], color);
}

/* Draws a word; x, y = top-left in pixels. Returns the width in pixels. */
static int draw_logo_word(Canvas *c, int x, int y, const char *word,
                          uint32_t color, uint32_t seed) {
  int ox = x * 8, oy = y * 8;
  int col0 = 0;

  for (; *word; word++) {
    const LatticeGlyph *g = logo_glyph(*word);
    if (!g)
      continue;

    /* glyph-local coordinates: triangle orientation depends on the
     * parity of the column, so letters must not share a lattice */
    int gx = ox + col0 * LOGO_COL;
    for (int cx = 0; cx < g->width; cx++) {
      int top = -1;
      for (int cy = -2; cy < 20; cy++) {
        if (!glyph_has(g, cx, cy))
          continue;
        if (top < 0)
          top = cy;
        /* the top of every column dissolves... */
        int depth = cy - top;
        int drop = depth == 0 ? 35 : depth <= 2 ? 12 : 0;
        if ((int)(logo_rand(&seed) % 100) >= drop)
          logo_tri(c, gx, oy, cx, cy, LOGO_SHRINK, color);
      }
      if (top < 0)
        continue;
      /* ...into loose triangles and finer dust drifting upwards */
      static const int chance[3] = { 50, 28, 12 };
      for (int up = 1; up <= 3; up++) {
        if ((int)(logo_rand(&seed) % 100) < chance[up - 1])
          logo_tri(c, gx, oy, cx, top - up * 2 +
                   (int)(logo_rand(&seed) & 1), LOGO_SHRINK, color);
      }
      for (int k = 0; k < 4; k++) {
        if ((int)(logo_rand(&seed) % 100) < 55)
          logo_tri(c, gx, oy, cx, top - 1 - (int)(logo_rand(&seed) % 6),
                   40, color);
      }
    }

    col0 += g->width + 1;
  }

  return (col0 - 1) * LOGO_COL / 8;
}

static void draw_logo(Canvas *c, int x, int y) {
  int w = draw_logo_word(c, x, y, "JAMAIS", C_LOGO_A, 0x1234);
  draw_logo_word(c, x + w + 2 * LOGO_COL / 8, y, "VU", C_LOGO_B, 0x9876);
}

int logo_draw(Canvas *c, int x, int y, int scale) {
  static uint32_t scratch[1];
  Canvas none = { scratch, 0, 0, 0 }; /* measuring: draws nothing */
  int w;
  g_logo_scale = scale;
  w = draw_logo_word(&none, 0, 0, "JAMAIS", 0, 0x1234);
  w += 2 * LOGO_COL / 8 + draw_logo_word(&none, 0, 0, "VU", 0, 0x9876);
  if (c)
    draw_logo(c, x, y);
  g_logo_scale = 1;
  return w;
}
