/* JamaisVu savestate plugin
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stddef.h>
#include <string.h>

#include "menu_view.h"

/* Palette */
#define C_PANEL      RGBA(244, 246, 250, 232)
#define C_PANEL_EDGE RGBA(255, 255, 255, 255)
#define C_HEADER     RGBA(226, 231, 238, 236)
#define C_FOOTER     RGBA(226, 231, 238, 236)
#define C_RULE       RGBA(40, 52, 70, 46)
#define C_TEXT       RGBA(18, 22, 30, 255)
#define C_TEXT_DIM   RGBA(88, 98, 114, 255)
#define C_LOGO_A     RGBA(52, 64, 88, 255)
#define C_LOGO_B     RGBA(34, 168, 214, 255)
#define C_ACCENT     RGBA(34, 178, 226, 255)
#define C_ACCENT_GLOW RGBA(34, 178, 226, 70)
#define C_EMPTY      RGBA(214, 220, 228, 255)
#define C_EMPTY_EDGE RGBA(160, 170, 184, 255)
#define C_THUMB_EDGE RGBA(20, 28, 40, 90)
#define C_BADGE      RGBA(18, 22, 30, 190)
#define C_BADGE_SEL  RGBA(34, 178, 226, 255)
#define C_WHITE      RGBA(255, 255, 255, 255)
#define C_ERROR      RGBA(200, 40, 50, 255)
#define C_BAR_BG     RGBA(200, 208, 220, 255)

#define HEADER_H   52
#define THUMB_Y    70
#define THUMB_GAP  12
#define THUMBS_X   ((MENU_W - (JV_NUM_SLOTS * JV_THUMB_W + \
                              (JV_NUM_SLOTS - 1) * THUMB_GAP)) / 2)
#define TITLE_Y    184
#define INFO_Y     218
#define FOOTER_Y   252
#define RULE_Y     172

/* Bounded string append (kernel plugins have no strcat/strcpy). */
static void sappend(char *dst, int size, const char *src) {
  int n = 0;
  while (n < size - 1 && dst[n])
    n++;
  while (n < size - 1 && *src)
    dst[n++] = *src++;
  dst[n] = '\0';
}

static void u_to_dec(char *out, unsigned v, int digits) {
  for (int i = digits - 1; i >= 0; i--) {
    out[i] = '0' + v % 10;
    v /= 10;
  }
  out[digits] = '\0';
}

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
#define LOGO_COL   32 /* column width, 1/8 px (4 px)            */
#define LOGO_HALF  18 /* half triangle side, 1/8 px (~2.3 px)  */
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

static void draw_counter(Canvas *c, const MenuModel *m) {
  char buf[32] = "SLOT ";
  char num[8];
  u_to_dec(num, m->selected + 1, 2); /* "SLOT 01/05" */
  sappend(buf, sizeof(buf), num);
  sappend(buf, sizeof(buf), "/");
  u_to_dec(num, JV_NUM_SLOTS, 2);
  sappend(buf, sizeof(buf), num);
  int w = cv_text_width(&font_medium, buf);
  cv_text(c, &font_medium, MENU_W - 24 - w,
          (HEADER_H - font_medium.height) / 2 + 1, buf, C_TEXT);
}

static void draw_slot(Canvas *c, const MenuModel *m, int i) {
  const MenuSlot *s = &m->slots[i];
  int x = THUMBS_X + i * (JV_THUMB_W + THUMB_GAP);
  int y = THUMB_Y;
  int sel = (i == m->selected);
  char num[2] = { (char)('1' + i), '\0' };

  if (sel) {
    cv_round_rect(c, x - 9, y - 9, JV_THUMB_W + 18, JV_THUMB_H + 18, 10,
                  C_ACCENT_GLOW);
    cv_round_rect(c, x - 5, y - 5, JV_THUMB_W + 10, JV_THUMB_H + 10, 7,
                  C_ACCENT);
  } else {
    cv_round_rect(c, x - 1, y - 1, JV_THUMB_W + 2, JV_THUMB_H + 2, 3,
                  C_THUMB_EDGE);
  }

  if (s->used && s->thumb) {
    cv_image(c, x, y, s->thumb, JV_THUMB_W, JV_THUMB_H);
  } else {
    cv_fill(c, x, y, JV_THUMB_W, JV_THUMB_H, C_EMPTY);
    /* dashed inner outline */
    for (int k = 4; k < JV_THUMB_W - 4; k += 8) {
      cv_fill(c, x + k, y + 4, 4, 1, C_EMPTY_EDGE);
      cv_fill(c, x + k, y + JV_THUMB_H - 5, 4, 1, C_EMPTY_EDGE);
    }
    for (int k = 4; k < JV_THUMB_H - 4; k += 8) {
      cv_fill(c, x + 4, y + k, 1, 4, C_EMPTY_EDGE);
      cv_fill(c, x + JV_THUMB_W - 5, y + k, 1, 4, C_EMPTY_EDGE);
    }
    if (s->locked) {
      /* padlock */
      int lx = x + JV_THUMB_W / 2, ly = y + JV_THUMB_H / 2 - 14;
      cv_round_frame(c, lx - 8, ly, 16, 18, 8, 3, C_TEXT_DIM);
      cv_round_rect(c, lx - 12, ly + 10, 24, 18, 4, C_TEXT_DIM);
      cv_text_centered(c, &font_small, lx, y + JV_THUMB_H - 26, "LOCKED",
                       C_TEXT_DIM, JV_THUMB_W);
    } else {
      cv_text_centered(c, &font_small, x + JV_THUMB_W / 2,
                       y + (JV_THUMB_H - font_small.height) / 2,
                       s->used ? (m->refreshing ? "LOADING" : "NO PREVIEW")
                               : s->prepared ? "READY" : "EMPTY",
                       C_TEXT_DIM,
                       JV_THUMB_W);
    }
  }

  /* slot number badge */
  cv_round_rect(c, x + 6, y + 6, 22, 22, 6, sel ? C_BADGE_SEL : C_BADGE);
  cv_text_centered(c, &font_small, x + 17, y + 7, num, C_WHITE, 22);
}

static int hint(Canvas *c, int x, int y, IconType icon, const char *label,
                IconType icon2, int two) {
  int cy = y + font_small.height / 2;
  cv_round_rect(c, x, cy - 11, two ? 44 : 22, 22, 11, RGBA(255, 255, 255, 255));
  cv_icon(c, icon, x + 11, cy, 12, C_TEXT);
  if (two)
    cv_icon(c, icon2, x + 33, cy, 12, C_TEXT);
  x += (two ? 44 : 22) + 8;
  x += cv_text(c, &font_small, x, y, label, C_TEXT);
  return x + 26;
}

static int hints_width(const char *const *labels, const int *two, int n) {
  int w = 0;
  for (int i = 0; i < n; i++)
    w += (two[i] ? 44 : 22) + 8 + cv_text_width(&font_small, labels[i]) + 26;
  return w - 26;
}

/* ✕ = confirm / ○ = back, swapped for games that use the other way */
static IconType ok_icon(const MenuModel *m) {
  return m->swap_ox ? ICON_CIRCLE : ICON_CROSS;
}
static IconType back_icon(const MenuModel *m) {
  return m->swap_ox ? ICON_CROSS : ICON_CIRCLE;
}

static void draw_footer(Canvas *c, const MenuModel *m) {
  int y = FOOTER_Y + (MENU_H - FOOTER_Y - font_small.height) / 2;
  const char *labels[6];
  IconType icons[6];
  int two[6] = { 0, 0, 0, 0, 0, 0 };
  int n = 0;
  const MenuSlot *cur = &m->slots[m->selected];

  switch (m->mode) {
    case MV_BROWSE:
      labels[n] = "Select"; icons[n] = ICON_LEFT; two[n] = 1; n++;
      if (m->can_save && !cur->locked) {
        labels[n] = "Save"; icons[n] = ok_icon(m); n++;
      } else if (!m->can_save && !cur->locked && !cur->prepared) {
        labels[n] = "Prepare"; icons[n] = ok_icon(m); n++;
      }
      if (m->can_load && m->slots[m->selected].used &&
          !m->slots[m->selected].outdated &&
          !m->slots[m->selected].config_changed) {
        labels[n] = "Load"; icons[n] = ICON_SQUARE; n++;
      }
      if (!m->can_save && m->prepare_count > 1) {
        labels[n] = "Prepare all"; icons[n] = ICON_TRIANGLE; n++;
      }
      labels[n] = "Close";  icons[n] = back_icon(m); n++;
      break;
    case MV_CONFIRM_PREPARE:
    case MV_CONFIRM_SAVE:
    case MV_CONFIRM_LOAD:
      labels[n] = "Confirm"; icons[n] = ok_icon(m); n++;
      labels[n] = "Cancel";  icons[n] = back_icon(m); n++;
      break;
    case MV_MESSAGE:
      labels[n] = "Close";   icons[n] = back_icon(m); n++;
      break;
    case MV_DIAG:
      labels[n] = "Reset test"; icons[n] = ok_icon(m); n++;
      labels[n] = "Dry run";    icons[n] = ICON_SQUARE; n++;
      labels[n] = "Back";       icons[n] = back_icon(m); n++;
      break;
    case MV_BUSY:
      if (m->can_cancel) {
        labels[n] = "Cancel"; icons[n] = back_icon(m); n++;
        break;
      }
      cv_text_centered(c, &font_small, MENU_W / 2, y,
                       "Please wait - do not turn off the system",
                       C_TEXT_DIM, MENU_W - 40);
      return;
  }

  int x = (MENU_W - hints_width(labels, two, n)) / 2;
  for (int i = 0; i < n; i++)
    x = hint(c, x, y, icons[i], labels[i], ICON_RIGHT, two[i]);
}

static void draw_details(Canvas *c, const MenuModel *m) {
  const MenuSlot *s = &m->slots[m->selected];
  char prompt[96];
  char num[2] = { (char)('1' + m->selected), '\0' };

  switch (m->mode) {
    case MV_BROWSE:
      if (s->used) {
        char info[96];
        info[0] = '\0';
        if (s->config_changed) {
          sappend(info, sizeof(info), "Plugin config (tai/config.txt) "
                  "changed - this snapshot can't be loaded");
        } else if (s->outdated) {
          sappend(info, sizeof(info), "Game updated: v");
          sappend(info, sizeof(info), s->app_ver);
          sappend(info, sizeof(info), " -> v");
          sappend(info, sizeof(info),
                  s->installed_ver[0] ? s->installed_ver : "?");
          sappend(info, sizeof(info), " - this snapshot can't be loaded");
        } else {
          sappend(info, sizeof(info), s->titleid);
          if (s->app_ver[0]) {
            sappend(info, sizeof(info), "   v");
            sappend(info, sizeof(info), s->app_ver);
          }
          if (s->date[0]) {
            sappend(info, sizeof(info), "   ");
            sappend(info, sizeof(info), s->date);
          }
        }
        cv_text_centered_utf8(c, &font_large, m->ext, MENU_W / 2, TITLE_Y,
                         s->title[0] ? s->title : s->titleid, C_TEXT,
                         MENU_W - 48);
        cv_text_centered(c, &font_small, MENU_W / 2, INFO_Y, info,
                         s->outdated || s->config_changed ? C_ERROR
                                                          : C_TEXT_DIM,
                         MENU_W - 48);
      } else if (s->locked) {
        cv_text_centered(c, &font_large, MENU_W / 2, TITLE_Y,
                         "Not enough free space", C_TEXT_DIM, MENU_W - 48);
        cv_text_centered(c, &font_small, MENU_W / 2, INFO_Y,
                         "Each slot needs 704 MB free on the Memory Card",
                         C_TEXT_DIM, MENU_W - 48);
      } else {
        const char *info;
        if (m->can_save)
          info = s->prepared ? (m->swap_ox ? "Press O to save your game here"
                                            : "Press X to save your game here")
                             : m->swap_ox ? "Press O to save your game here (the "
                                            "first save prepares the slot)"
                             : "Press X to save your game here (the first "
                               "save prepares the slot)";
        else
          info = s->prepared ? "Ready - start a game to save here"
                             : m->swap_ox ? "Press O to prepare this slot now (704 "
                                            "MB), so saving starts right away"
                             : "Press X to prepare this slot now (704 MB), "
                               "so saving starts right away";
        cv_text_centered(c, &font_large, MENU_W / 2, TITLE_Y,
                         s->prepared ? "Empty slot - ready" : "Empty slot",
                         C_TEXT_DIM, MENU_W - 48);
        cv_text_centered(c, &font_small, MENU_W / 2, INFO_Y, info,
                         C_TEXT_DIM, MENU_W - 48);
      }
      break;

    case MV_CONFIRM_SAVE:
      prompt[0] = 0, sappend(prompt, sizeof(prompt), s->used ? "Overwrite slot " : "Save to slot ");
      sappend(prompt, sizeof(prompt), num);
      sappend(prompt, sizeof(prompt), "?");
      cv_text_centered(c, &font_large, MENU_W / 2, TITLE_Y, prompt, C_TEXT,
                       MENU_W - 48);
      cv_text_centered(c, &font_small, MENU_W / 2, INFO_Y,
                       s->used ? "The existing snapshot will be replaced."
                               : "The screen stays black for about a minute.",
                       C_TEXT_DIM, MENU_W - 48);
      break;

    case MV_CONFIRM_PREPARE: {
      char info[96];
      prompt[0] = 0;
      info[0] = 0;
      if (m->prepare_all && m->prepare_count > 1) {
        char cnt[2] = { (char)('0' + m->prepare_count), '\0' };
        sappend(prompt, sizeof(prompt), "Prepare ");
        sappend(prompt, sizeof(prompt), cnt);
        sappend(prompt, sizeof(prompt), " slots?");
        sappend(info, sizeof(info), "Uses 704 MB each on the Memory Card. "
                "Takes a while per slot.");
      } else {
        sappend(prompt, sizeof(prompt), "Prepare slot ");
        sappend(prompt, sizeof(prompt), m->prepare_all ? "" : num);
        sappend(prompt, sizeof(prompt), "?");
        sappend(info, sizeof(info), "Uses 704 MB on the Memory Card. "
                "Takes a while.");
      }
      cv_text_centered(c, &font_large, MENU_W / 2, TITLE_Y, prompt, C_TEXT,
                       MENU_W - 48);
      cv_text_centered(c, &font_small, MENU_W / 2, INFO_Y, info,
                       C_TEXT_DIM, MENU_W - 48);
      break;
    }

    case MV_CONFIRM_LOAD:
      prompt[0] = 0, sappend(prompt, sizeof(prompt), "Load slot ");
      sappend(prompt, sizeof(prompt), num);
      sappend(prompt, sizeof(prompt), "?");
      cv_text_centered(c, &font_large, MENU_W / 2, TITLE_Y, prompt, C_TEXT,
                       MENU_W - 48);
      cv_text_centered(c, &font_small, MENU_W / 2, INFO_Y,
                       m->savedata_rolls_back
                           ? "Progress since this snapshot will be lost."
                           : "Progress will be lost. Savedata on ux0 is "
                             "not rolled back.",
                       C_TEXT_DIM, MENU_W - 48);
      break;

    case MV_BUSY:
      cv_text_centered(c, &font_large, MENU_W / 2, TITLE_Y, m->message,
                       C_TEXT, MENU_W - 48);
      if (m->progress >= 0) {
        int bw = 420, bx = (MENU_W - bw) / 2, by = INFO_Y + 6;
        int p = m->progress > 100 ? 100 : m->progress;
        cv_round_rect(c, bx, by, bw, 8, 4, C_BAR_BG);
        if (p > 0)
          cv_round_rect(c, bx, by, bw * p / 100 < 8 ? 8 : bw * p / 100, 8, 4,
                        C_ACCENT);
      }
      break;

    case MV_DIAG:
      cv_text_centered(c, &font_large, MENU_W / 2, TITLE_Y,
                       "Diagnostics - nothing is written", C_TEXT,
                       MENU_W - 48);
      cv_text_centered(c, &font_small, MENU_W / 2, INFO_Y,
                       "Dry run uses the selected slot; it must be empty",
                       C_TEXT_DIM, MENU_W - 48);
      break;

    case MV_MESSAGE:
      cv_text_centered(c, &font_large, MENU_W / 2, TITLE_Y + 16, m->message,
                       m->is_error ? C_ERROR : C_TEXT, MENU_W - 48);
      break;
  }
}

void menu_view_render(Canvas *c, const MenuModel *m) {
  cv_clear(c, 0);

  /* panel */
  cv_round_rect(c, 0, 0, MENU_W, MENU_H, 12, C_PANEL_EDGE);
  cv_round_rect(c, 1, 1, MENU_W - 2, MENU_H - 2, 11, C_PANEL);

  /* header band */
  cv_fill(c, 1, 12, MENU_W - 2, HEADER_H - 12, C_HEADER);
  cv_round_rect(c, 1, 1, MENU_W - 2, 24, 11, C_HEADER);
  cv_fill(c, 1, HEADER_H, MENU_W - 2, 1, C_RULE);
  draw_logo(c, 24, 13); /* letters + rising particles centred in the header */
  draw_counter(c, m);

  for (int i = 0; i < JV_NUM_SLOTS; i++)
    draw_slot(c, m, i);

  cv_fill(c, 24, RULE_Y, MENU_W - 48, 1, C_RULE);
  draw_details(c, m);

  /* footer band */
  cv_fill(c, 1, FOOTER_Y, MENU_W - 2, 1, C_RULE);
  cv_fill(c, 1, FOOTER_Y + 1, MENU_W - 2, MENU_H - FOOTER_Y - 13, C_FOOTER);
  cv_round_rect(c, 1, MENU_H - 25, MENU_W - 2, 24, 11, C_FOOTER);
  draw_footer(c, m);
}

void menu_view_render_toast(Canvas *c, const char *msg, int is_error) {
  cv_clear(c, 0);
  cv_round_rect(c, 0, 0, TOAST_W, TOAST_H, 12, C_PANEL_EDGE);
  cv_round_rect(c, 1, 1, TOAST_W - 2, TOAST_H - 2, 11, C_PANEL);
  cv_round_rect(c, 14, (TOAST_H - 12) / 2, 12, 12, 6,
                is_error ? C_ERROR : C_ACCENT);
  cv_text_centered(c, &font_small, TOAST_W / 2 + 10,
                   (TOAST_H - font_small.height) / 2, msg,
                   is_error ? C_ERROR : C_TEXT, TOAST_W - 60);
}
