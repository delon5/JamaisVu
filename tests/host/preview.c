/* JamaisVu menu preview
 *
 * Renders the in-game menu with the plugin's own drawing code
 * (plugin/gui) over a 960x544 background and writes raw RGBA frames.
 * tests/host/preview.sh turns them into PNGs.
 *
 * usage: preview bg.rgba thumbs.rgba outdir
 *   bg.rgba     960*544*4 bytes
 *   thumbs.rgba JV_NUM_SLOTS * 160*90*4 bytes
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "menu_view.h"

static uint32_t bg[JV_SCREEN_W * JV_SCREEN_H];
static uint32_t fb[JV_SCREEN_W * JV_SCREEN_H];
static uint32_t panel_px[MENU_W * MENU_H];
static uint32_t toast_px[TOAST_W * TOAST_H];
static uint32_t thumbs[JV_NUM_SLOTS][JV_THUMB_W * JV_THUMB_H];

static void compose(const Canvas *c, int x0, int y0) {
  static int xmap[JV_SCREEN_W];
  for (int i = 0; i < c->w; i++)
    xmap[i] = i;
  for (int y = 0; y < c->h; y++)
    cv_compose_row(&fb[(y0 + y) * JV_SCREEN_W + x0], &c->px[y * c->w], xmap,
                   c->w);
}

static void emit(const char *dir, const char *name) {
  char path[512];
  snprintf(path, sizeof(path), "%s/%s.rgba", dir, name);
  FILE *f = fopen(path, "wb");
  fwrite(fb, 4, JV_SCREEN_W * JV_SCREEN_H, f);
  fclose(f);
}

static void frame(const char *dir, const char *name, const MenuModel *m) {
  Canvas c = { panel_px, MENU_W, MENU_H };
  memcpy(fb, bg, sizeof(fb));
  menu_view_render(&c, m);
  compose(&c, MENU_X, MENU_Y);
  emit(dir, name);
}

int main(int argc, char **argv) {
  if (argc < 4)
    return 2;
  FILE *f = fopen(argv[1], "rb");
  if (fread(bg, 4, JV_SCREEN_W * JV_SCREEN_H, f) != JV_SCREEN_W * JV_SCREEN_H)
    return 1;
  fclose(f);
  f = fopen(argv[2], "rb");
  if (fread(thumbs, 4, sizeof(thumbs) / 4, f) != sizeof(thumbs) / 4)
    return 1;
  fclose(f);

  MenuModel m;
  memset(&m, 0, sizeof(m));
  m.can_load = 1;
  m.can_save = 1;
  m.progress = -1;

  const char *titles[] = { "Persona 4 Golden", "Gravity Rush",
                           "", "Uncharted: Golden Abyss", "" };
  const char *ids[] = { "PCSE00120", "PCSA00011", "", "PCSA00029", "" };
  const char *dates[] = { "2026-10-31  21:40", "2026-10-29  18:05", "",
                          "2026-10-12  09:31", "" };
  for (int i = 0; i < JV_NUM_SLOTS; i++) {
    m.slots[i].used = titles[i][0] != 0;
    strcpy(m.slots[i].title, titles[i]);
    strcpy(m.slots[i].titleid, ids[i]);
    strcpy(m.slots[i].date, dates[i]);
    m.slots[i].thumb = m.slots[i].used ? thumbs[i] : NULL;
  }

  m.mode = MV_BROWSE;
  m.selected = 0;
  frame(argv[3], "1_browse", &m);

  m.selected = 2;
  frame(argv[3], "2_empty_slot", &m);

  m.selected = 3;
  m.mode = MV_CONFIRM_SAVE;
  frame(argv[3], "3_confirm_overwrite", &m);

  m.mode = MV_CONFIRM_LOAD;
  m.selected = 1;
  frame(argv[3], "4_confirm_load", &m);

  m.mode = MV_BUSY;
  m.selected = 2;
  strcpy(m.message, "Preparing slot 3 (first use only)  42%");
  m.progress = 42;
  frame(argv[3], "5_preparing_slot", &m);

  /* opened from the LiveArea: load only */
  m.mode = MV_BROWSE;
  m.can_save = 0;
  m.progress = -1;
  m.selected = 2;
  frame(argv[3], "8_livearea_empty_slot", &m);
  m.selected = 0;
  frame(argv[3], "9_livearea_load", &m);
  m.can_save = 1;

  /* free-space safeguard: ~1.5 GB free -> slots 3 and 5 locked */
  m.slots[2].locked = 1;
  m.slots[4].locked = 1;
  m.selected = 4;
  frame(argv[3], "10_locked_slot", &m);
  m.slots[2].locked = 0;
  m.slots[4].locked = 0;

  /* toast after resuming */
  Canvas t = { toast_px, TOAST_W, TOAST_H };
  memcpy(fb, bg, sizeof(fb));
  menu_view_render_toast(&t, "Saved to slot 3", 0);
  compose(&t, TOAST_X, TOAST_Y);
  emit(argv[3], "6_toast_saved");

  memcpy(fb, bg, sizeof(fb));
  menu_view_render_toast(&t, "Load failed: this snapshot is damaged - nothing was changed", 1);
  compose(&t, TOAST_X, TOAST_Y);
  emit(argv[3], "7_toast_error");
  return 0;
}
