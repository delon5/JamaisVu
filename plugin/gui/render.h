/* JamaisVu savestate plugin
 *
 * Tiny software renderer for the in-game menu. Plain C, no SDK
 * dependencies, so the exact same code also runs in the host preview
 * (tests/host/preview.c).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __JV_RENDER_H__
#define __JV_RENDER_H__

#include <stdint.h>
#include "font.h"

/* Pixels are 0xAABBGGRR, i.e. the byte order R,G,B,A of the Vita's
 * SCE_DISPLAY_PIXELFORMAT_A8B8G8R8 framebuffers. Straight (non
 * premultiplied) alpha. */
#define RGBA(r, g, b, a) \
  ((uint32_t)(r) | ((uint32_t)(g) << 8) | ((uint32_t)(b) << 16) | \
   ((uint32_t)(a) << 24))

typedef struct {
  uint32_t *px;
  int w, h; /* stride == w */
} Canvas;

typedef enum {
  ICON_CROSS,
  ICON_CIRCLE,
  ICON_SQUARE,
  ICON_TRIANGLE,
  ICON_LEFT,
  ICON_RIGHT,
} IconType;

void cv_clear(Canvas *c, uint32_t color);
void cv_fill(Canvas *c, int x, int y, int w, int h, uint32_t color);
void cv_round_rect(Canvas *c, int x, int y, int w, int h, int r, uint32_t color);
void cv_round_frame(Canvas *c, int x, int y, int w, int h, int r, int t,
                    uint32_t color);
void cv_image(Canvas *c, int x, int y, const uint32_t *img, int iw, int ih);
int cv_text(Canvas *c, const Font *f, int x, int y, const char *s,
            uint32_t color);
int cv_text_width(const Font *f, const char *s);
void cv_text_centered(Canvas *c, const Font *f, int cx, int y, const char *s,
                      uint32_t color, int max_w);
void cv_icon(Canvas *c, IconType type, int cx, int cy, int size,
             uint32_t color);

/* Anti-aliased filled triangle. Coordinates in 1/8 pixel units. */
void cv_triangle(Canvas *c, int x0, int y0, int x1, int y1, int x2, int y2,
                 uint32_t color);

/* Blends n source pixels over an opaque destination row. xmap[i] is the
 * source column for destination column i (nearest-neighbour scaling). */
void cv_compose_row(uint32_t *dst, const uint32_t *src_row, const int *xmap,
                    int n);

#endif
