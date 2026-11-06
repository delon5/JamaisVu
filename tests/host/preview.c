/* JamaisVu menu preview
 *
 * Renders the in-game menu with the plugin's own drawing code
 * (plugin/gui) over a 960x544 background and writes raw RGBA frames.
 * tests/host/preview.sh turns them into PNGs.
 *
 * usage: preview bg.rgba thumbs.rgba outdir [jpfont.bin]
 *   bg.rgba     960*544*4 bytes
 *   thumbs.rgba JV_NUM_SLOTS * 160*90*4 bytes
 *   jpfont.bin  Japanese glyphs (tools/gen_jpfont.py): adds frames 18-19
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "menu_view.h"

static uint32_t bg[JV_SCREEN_W * JV_SCREEN_H];
static uint32_t fb[JV_SCREEN_W * JV_SCREEN_H];
static uint32_t panel_px[MENU_W * MENU_H];
static uint32_t toast_px[TOAST_W * TOAST_H];
static uint32_t thumbs32[JV_NUM_SLOTS][JV_THUMB_W * JV_THUMB_H];
static uint16_t thumbs[JV_NUM_SLOTS][JV_THUMB_W * JV_THUMB_H]; /* RGB565 */

static uint32_t ext_mem[EXT_SET_SIZE(EXT_BITMAP_SIZE) / 4];

static int file_read(void *ctx, uint32_t off, void *buf, uint32_t len) {
  FILE *f = ctx;
  if (fseek(f, off, SEEK_SET) != 0)
    return -1;
  return (int)fread(buf, 1, len, f);
}

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

/* JV_PREVIEW_16BIT=1: as the plugin stores the menu image - colours as
 * RGB565 (nearest shade), alpha kept in the corner rows */
static void as_stored(Canvas *c) {
  const char *e = getenv("JV_PREVIEW_16BIT");
  if (!e || e[0] != '1')
    return;
  for (int y = 0; y < MENU_H; y++)
    for (int x = 0; x < MENU_W; x++)
      c->px[y * c->w + x] = rgba_as_565(c->px[y * c->w + x]);
}

static void frame(const char *dir, const char *name, const MenuModel *m) {
  Canvas c = { panel_px, MENU_W, MENU_H, 0 };
  memcpy(fb, bg, sizeof(fb));
  menu_view_render(&c, m);
  as_stored(&c);
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
  if (fread(thumbs32, 4, sizeof(thumbs32) / 4, f) != sizeof(thumbs32) / 4)
    return 1;
  fclose(f);
  for (int i = 0; i < JV_NUM_SLOTS; i++)
    for (int p = 0; p < JV_THUMB_W * JV_THUMB_H; p++)
      thumbs[i][p] = rgba_to_565(thumbs32[i][p]);

  MenuModel m;
  memset(&m, 0, sizeof(m));
  m.can_load = 1;
  m.can_save = 1;
  m.savedata_rolls_back = 1;
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
    strcpy(m.slots[i].app_ver, "01.02");
    strcpy(m.slots[i].installed_ver, "01.02");
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
  m.can_cancel = 1;
  frame(argv[3], "5_preparing_slot", &m);
  m.can_cancel = 0;

  /* opened from the LiveArea: load only */
  m.mode = MV_BROWSE;
  m.can_save = 0;
  m.progress = -1;
  m.selected = 2;
  frame(argv[3], "8_livearea_empty_slot", &m);
  m.selected = 0;
  frame(argv[3], "9_livearea_load", &m);

  /* LiveArea: prepare slot files ahead of time (slot 5 already ready) */
  m.slots[4].prepared = 1;
  m.prepare_count = 1;
  m.selected = 4;
  frame(argv[3], "12_livearea_ready_slot", &m);
  m.slots[4].prepared = 0;
  m.prepare_count = 2;
  m.selected = 2;
  frame(argv[3], "13_livearea_prepare", &m);
  m.mode = MV_CONFIRM_PREPARE;
  m.prepare_all = 1;
  frame(argv[3], "14_confirm_prepare_all", &m);
  m.mode = MV_BUSY;
  strcpy(m.message, "Preparing slot 3  42%");
  m.progress = 42;
  m.can_cancel = 1;
  frame(argv[3], "15_preparing_from_livearea", &m);
  m.can_cancel = 0;
  m.mode = MV_BROWSE;
  m.progress = -1;
  m.prepare_all = 0;
  m.prepare_count = 0;
  m.can_save = 1;

  /* free-space safeguard: ~1.5 GB free -> slots 3 and 5 locked */
  m.slots[2].locked = 1;
  m.slots[4].locked = 1;
  m.selected = 4;
  frame(argv[3], "10_locked_slot", &m);
  m.slots[2].locked = 0;
  m.slots[4].locked = 0;

  /* Japanese/Asian game: ○ confirms, ✕ goes back */
  m.swap_ox = 1;
  m.selected = 0;
  frame(argv[3], "16_japanese_game_buttons", &m);
  m.mode = MV_CONFIRM_LOAD;
  frame(argv[3], "17_japanese_game_confirm", &m);
  m.mode = MV_BROWSE;
  m.swap_ox = 0;

  /* game updated since the snapshot: cannot be loaded */
  strcpy(m.slots[3].installed_ver, "01.03");
  m.slots[3].outdated = 1;
  m.selected = 3;
  frame(argv[3], "11_outdated_slot", &m);
  m.slots[3].outdated = 0;

  /* Japanese game names (glyphs read from jpfont.bin, as on the Vita) */
  if (argc > 4) {
    FILE *jf = fopen(argv[4], "rb");
    static const char *jp[] = {
      "\xe3\x82\xad\xe3\x83\xa3\xe3\x82\xb5\xe3\x83\xaa\xe3\x83\xb3"
      "\xe3\x83\xbb\xef\xbc\xa6\xef\xbc\xa2", /* Catherine FB */
      "\xe8\x8b\xb1\xe9\x9b\x84\xe4\xbc\x9d\xe8\xaa\xac "
      "\xe7\xa9\xba\xe3\x81\xae\xe8\xbb\x8c\xe8\xb7\xa1 the 3rd Evolution",
      "\xe9\x9b\xb6\xe3\x81\xae\xe8\xbb\x8c\xe8\xb7\xa1 Evolution",
    };
    if (!jf || jpfont_load(file_read, jf, jp, 3, ext_init(ext_mem, EXT_BITMAP_SIZE)) < 0) {
      fprintf(stderr, "jpfont.bin not usable\n");
      return 1;
    }
    fclose(jf);
    m.ext = (const ExtGlyphs *)ext_mem;
    strcpy(m.slots[0].title, jp[0]);
    strcpy(m.slots[0].titleid, "PCSG01179");
    strcpy(m.slots[1].title, jp[1]);
    strcpy(m.slots[1].titleid, "PCSG00490");
    m.selected = 0;
    frame(argv[3], "18_japanese_name", &m);
    m.selected = 1;
    frame(argv[3], "19_japanese_latin_name", &m);
    m.ext = NULL;
  }

  /* toast after resuming */
  Canvas t = { toast_px, TOAST_W, TOAST_H, 0 };
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
