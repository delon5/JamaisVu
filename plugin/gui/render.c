/* JamaisVu savestate plugin
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stddef.h>
#include "render.h"

static inline uint32_t div255(uint32_t v) {
  return (v + 1 + (v >> 8)) >> 8; /* exact for v <= 255 * 255 */
}

/* x / d for d in 1..255 without a divide instruction (the Cortex-A9 has
 * none, and the menu blends ~280k pixels per redraw). */
static uint32_t inv_tab[256];

static inline uint32_t div_small(uint32_t x, uint32_t d) {
  if (!inv_tab[1]) {
    for (uint32_t i = 1; i < 256; i++)
      inv_tab[i] = ((1u << 24) + i - 1) / i;
  }
  return (uint32_t)(((uint64_t)x * inv_tab[d]) >> 24);
}

/* Straight-alpha "source over destination". cov scales the source alpha. */
static inline uint32_t blend(uint32_t dst, uint32_t src, uint32_t cov) {
  uint32_t sa = div255((src >> 24) * cov);
  uint32_t da, oa, r, g, b, dw;

  if (sa == 0)
    return dst;
  if (sa == 255)
    return (src & 0x00ffffff) | 0xff000000;

  da = dst >> 24;
  dw = div255(da * (255 - sa));
  oa = sa + dw;
  if (oa == 0)
    return 0;

  if (da == 0)
    return (src & 0x00ffffff) | (sa << 24);

  r = div_small((src & 0xff) * sa + (dst & 0xff) * dw, oa);
  g = div_small(((src >> 8) & 0xff) * sa + ((dst >> 8) & 0xff) * dw, oa);
  b = div_small(((src >> 16) & 0xff) * sa + ((dst >> 16) & 0xff) * dw, oa);
  if (r > 255) r = 255;
  if (g > 255) g = 255;
  if (b > 255) b = 255;

  return r | (g << 8) | (b << 16) | (oa << 24);
}

static inline void put(Canvas *c, int x, int y, uint32_t color, uint32_t cov) {
  if ((unsigned)x >= (unsigned)c->w || (unsigned)y >= (unsigned)c->h)
    return;
  uint32_t *p = &c->px[y * c->w + x];
  *p = blend(*p, color, cov);
}

void cv_clear(Canvas *c, uint32_t color) {
  for (int i = 0; i < c->w * c->h; i++)
    c->px[i] = color;
}

void cv_fill(Canvas *c, int x, int y, int w, int h, uint32_t color) {
  for (int j = y; j < y + h; j++)
    for (int i = x; i < x + w; i++)
      put(c, i, j, color, 255);
}

/* Coverage (0..255) of pixel (px,py) by a rounded rectangle, using 4x4
 * supersampling at the corners only. */
static uint32_t rrect_cov(int px, int py, int x, int y, int w, int h, int r) {
  int cx, cy;

  if (px < x || py < y || px >= x + w || py >= y + h)
    return 0;

  if (px < x + r)
    cx = x + r;
  else if (px >= x + w - r)
    cx = x + w - r;
  else
    return 255;

  if (py < y + r)
    cy = y + r;
  else if (py >= y + h - r)
    cy = y + h - r;
  else
    return 255;

  uint32_t hits = 0;
  for (int sy = 0; sy < 4; sy++) {
    for (int sx = 0; sx < 4; sx++) {
      /* sample position in 1/8 pixel units relative to the corner centre */
      int dx = (px - cx) * 8 + sx * 2 + 1;
      int dy = (py - cy) * 8 + sy * 2 + 1;
      if (dx * dx + dy * dy <= r * r * 64)
        hits++;
    }
  }
  return hits * 255 / 16;
}

void cv_round_rect(Canvas *c, int x, int y, int w, int h, int r,
                   uint32_t color) {
  for (int j = y; j < y + h; j++)
    for (int i = x; i < x + w; i++) {
      uint32_t cov = rrect_cov(i, j, x, y, w, h, r);
      if (cov)
        put(c, i, j, color, cov);
    }
}

void cv_round_frame(Canvas *c, int x, int y, int w, int h, int r, int t,
                    uint32_t color) {
  int ri = r > t ? r - t : 0;
  for (int j = y; j < y + h; j++)
    for (int i = x; i < x + w; i++) {
      int outer = rrect_cov(i, j, x, y, w, h, r);
      int inner = rrect_cov(i, j, x + t, y + t, w - 2 * t, h - 2 * t, ri);
      int cov = outer - inner;
      if (cov > 0)
        put(c, i, j, color, cov);
    }
}

void cv_image(Canvas *c, int x, int y, const uint32_t *img, int iw, int ih) {
  for (int j = 0; j < ih; j++)
    for (int i = 0; i < iw; i++)
      put(c, x + i, y + j, img[j * iw + i] | 0xff000000, 255);
}

static const FontGlyph *glyph(const Font *f, char ch) {
  unsigned char u = (unsigned char)ch;
  if (u < f->first || u > f->last)
    u = '?';
  return &f->glyphs[u - f->first];
}

int cv_text_width(const Font *f, const char *s) {
  int w = 0;
  while (*s)
    w += glyph(f, *s++)->advance;
  return w;
}

int cv_text(Canvas *c, const Font *f, int x, int y, const char *s,
            uint32_t color) {
  int pen = x;
  while (*s) {
    const FontGlyph *g = glyph(f, *s++);
    const uint8_t *bm = f->bitmap + g->offset;
    for (int j = 0; j < g->h; j++)
      for (int i = 0; i < g->w; i++) {
        uint32_t cov = bm[j * g->w + i];
        if (cov)
          put(c, pen + g->x_off + i, y + g->y_off + j, color, cov);
      }
    pen += g->advance;
  }
  return pen - x;
}

/* Centred text; if wider than max_w it is cut and ends in "..." */
void cv_text_centered(Canvas *c, const Font *f, int cx, int y, const char *s,
                      uint32_t color, int max_w) {
  char buf[160];
  int n = 0;

  while (s[n] && n < (int)sizeof(buf) - 4) {
    buf[n] = s[n];
    n++;
  }
  buf[n] = '\0';

  if (cv_text_width(f, buf) > max_w) {
    int dots = cv_text_width(f, "...");
    while (n > 0) {
      buf[--n] = '\0';
      if (cv_text_width(f, buf) + dots <= max_w)
        break;
    }
    while (n > 0 && buf[n - 1] == ' ')
      buf[--n] = '\0';
    buf[n] = '.';
    buf[n + 1] = '.';
    buf[n + 2] = '.';
    buf[n + 3] = '\0';
  }

  cv_text(c, f, cx - cv_text_width(f, buf) / 2, y, buf, color);
}

/* --- PlayStation button icons ------------------------------------------ */
/* Each shape is evaluated with 4x4 supersampling in 1/8 pixel units. */

static int abs_i(int v) { return v < 0 ? -v : v; }

/* distance (in 1/8 px) from point p to segment ab, squared */
static int seg_dist2(int px, int py, int ax, int ay, int bx, int by) {
  int abx = bx - ax, aby = by - ay;
  int apx = px - ax, apy = py - ay;
  int len2 = abx * abx + aby * aby;
  int t = len2 ? (apx * abx + apy * aby) * 1024 / len2 : 0;
  if (t < 0) t = 0;
  if (t > 1024) t = 1024;
  int qx = ax + abx * t / 1024 - px;
  int qy = ay + aby * t / 1024 - py;
  return qx * qx + qy * qy;
}

static int icon_hit(IconType type, int dx, int dy, int r, int lw) {
  /* dx, dy, r, lw in 1/8 px; origin = icon centre */
  int lw2 = (lw / 2) * (lw / 2);
  switch (type) {
    case ICON_CIRCLE: {
      int d2 = dx * dx + dy * dy;
      int ro = r, ri = r - lw;
      return d2 <= ro * ro && d2 >= ri * ri;
    }
    case ICON_CROSS:
      return seg_dist2(dx, dy, -r, -r, r, r) <= lw2 ||
             seg_dist2(dx, dy, -r, r, r, -r) <= lw2;
    case ICON_SQUARE: {
      int ax = abs_i(dx), ay = abs_i(dy);
      int s = r * 85 / 100;
      return ax <= s && ay <= s && (ax >= s - lw || ay >= s - lw);
    }
    case ICON_TRIANGLE: {
      int tx = 0, ty = -r, lx = -r * 95 / 100, ly = r * 7 / 10,
          rx = r * 95 / 100, ry = r * 7 / 10;
      return seg_dist2(dx, dy, tx, ty, lx, ly) <= lw2 ||
             seg_dist2(dx, dy, lx, ly, rx, ry) <= lw2 ||
             seg_dist2(dx, dy, rx, ry, tx, ty) <= lw2;
    }
    case ICON_LEFT:
    case ICON_RIGHT: {
      int sx = type == ICON_LEFT ? -dx : dx;
      /* filled arrow head */
      return sx <= r && sx >= -r / 3 && abs_i(dy) * 4 <= (r - sx) * 3;
    }
  }
  return 0;
}

void cv_icon(Canvas *c, IconType type, int cx, int cy, int size,
             uint32_t color) {
  int r = size * 8 / 2;
  int lw = size * 8 / 6;
  if (lw < 12)
    lw = 12;

  for (int y = cy - size; y <= cy + size; y++)
    for (int x = cx - size; x <= cx + size; x++) {
      int hits = 0;
      for (int sy = 0; sy < 4; sy++)
        for (int sx = 0; sx < 4; sx++)
          hits += icon_hit(type, (x - cx) * 8 + sx * 2 - 3,
                           (y - cy) * 8 + sy * 2 - 3, r, lw);
      if (hits)
        put(c, x, y, color, hits * 255 / 16);
    }
}

static int edge(int ax, int ay, int bx, int by, int px, int py) {
  return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

void cv_triangle(Canvas *c, int x0, int y0, int x1, int y1, int x2, int y2,
                 uint32_t color) {
  int minx = x0, maxx = x0, miny = y0, maxy = y0;
  minx = x1 < minx ? x1 : minx;
  maxx = x1 > maxx ? x1 : maxx;
  minx = x2 < minx ? x2 : minx;
  maxx = x2 > maxx ? x2 : maxx;
  miny = y1 < miny ? y1 : miny;
  maxy = y1 > maxy ? y1 : maxy;
  miny = y2 < miny ? y2 : miny;
  maxy = y2 > maxy ? y2 : maxy;

  /* consistent winding */
  if (edge(x0, y0, x1, y1, x2, y2) < 0) {
    int tx = x1, ty = y1;
    x1 = x2; y1 = y2;
    x2 = tx; y2 = ty;
  }

  for (int py = miny >> 3; py <= (maxy + 7) >> 3; py++)
    for (int px = minx >> 3; px <= (maxx + 7) >> 3; px++) {
      int hits = 0;
      for (int sy = 0; sy < 4; sy++)
        for (int sx = 0; sx < 4; sx++) {
          int qx = px * 8 + sx * 2 + 1, qy = py * 8 + sy * 2 + 1;
          if (edge(x0, y0, x1, y1, qx, qy) >= 0 &&
              edge(x1, y1, x2, y2, qx, qy) >= 0 &&
              edge(x2, y2, x0, y0, qx, qy) >= 0)
            hits++;
        }
      if (hits)
        put(c, px, py, color, hits * 255 / 16);
    }
}

void cv_compose_row(uint32_t *dst, const uint32_t *src_row, const int *xmap,
                    int n) {
  for (int i = 0; i < n; i++) {
    uint32_t s = src_row[xmap[i]];
    uint32_t a = s >> 24;
    if (a == 0)
      continue;
    if (a == 255) {
      dst[i] = (dst[i] & 0xff000000) | (s & 0x00ffffff);
      continue;
    }
    uint32_t d = dst[i];
    uint32_t ia = 255 - a;
    uint32_t r = div255((s & 0xff) * a + (d & 0xff) * ia);
    uint32_t g = div255(((s >> 8) & 0xff) * a + ((d >> 8) & 0xff) * ia);
    uint32_t b = div255(((s >> 16) & 0xff) * a + ((d >> 16) & 0xff) * ia);
    dst[i] = (d & 0xff000000) | r | (g << 8) | (b << 16);
  }
}
