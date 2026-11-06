/* JamaisVu host test: the menu drawn band by band (as the plugin's render
 * thread does) must be exactly the image drawn in one go.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "menu_view.h"
#include "jpfont.h"

static ExtGlyphs ext;

static int file_read(void *ctx, uint32_t off, void *buf, uint32_t len) {
  FILE *f = ctx;
  if (fseek(f, off, SEEK_SET) != 0)
    return -1;
  return (int)fread(buf, 1, len, f);
}

static int mem_read(void *ctx, uint32_t off, void *buf, uint32_t len) {
  const uint8_t *m = ctx; /* 64-byte fake file */
  if (off >= 64)
    return 0;
  if (len > 64 - off)
    len = 64 - off;
  memcpy(buf, m + off, len);
  return (int)len;
}

#define CHECK(cond, what)                                   \
  do {                                                      \
    if (cond) {                                             \
      printf("  %-58s ok\n", what);                         \
    } else {                                                \
      printf("  %-58s FAILED\n", what);                     \
      rc = 1;                                               \
    }                                                       \
  } while (0)

static uint32_t whole[MENU_W * MENU_H], banded[MENU_W * MENU_H];
static uint32_t band[MENU_W * MENU_H];
static uint16_t thumbs[JV_NUM_SLOTS][JV_THUMB_W * JV_THUMB_H]; /* RGB565 */

static int check(const char *name, const MenuModel *m) {
  static const int sizes[] = { 1, 7, 20, 64, MENU_H };
  Canvas c = { whole, MENU_W, MENU_H, 0 };
  int bad = 0;

  menu_view_render(&c, m);
  for (unsigned k = 0; k < sizeof(sizes) / sizeof(sizes[0]); k++) {
    int rows = sizes[k];
    memset(banded, 0xa5, sizeof(banded));
    for (int y0 = 0; y0 < MENU_H; y0 += rows) {
      int n = MENU_H - y0 < rows ? MENU_H - y0 : rows;
      Canvas b = { band, MENU_W, n, y0 };
      memset(band, 0x5a, sizeof(band)); /* stale content must not matter */
      menu_view_render(&b, m);
      memcpy(&banded[y0 * MENU_W], band, n * MENU_W * 4);
    }
    if (memcmp(whole, banded, sizeof(whole)) != 0) {
      printf("  %s, bands of %d rows: DIFFERENT\n", name, rows);
      bad = 1;
    }
  }
  if (!bad)
    printf("  %s: identical\n", name);
  return bad;
}

/* UTF-8 decoding, jpfont.bin loading, Japanese names in bands */
static int japanese(const char *font_path, MenuModel *m) {
  int rc = 0;
  /* Catherine FB, Trails, an invalid byte, a cut-off character */
  static const char *names[] = {
    "\xe3\x82\xad\xe3\x83\xa3\xe3\x82\xb5\xe3\x83\xaa\xe3\x83\xb3"
    "\xe3\x83\xbb\xef\xbc\xa6\xef\xbc\xa2",
    "\xe8\x8b\xb1\xe9\x9b\x84\xe4\xbc\x9d\xe8\xaa\xac the 3rd Evolution",
    "bad \xff byte \xe3\x82",
  };
  const char *s = names[0];
  CHECK(utf8_next(&s) == 0x30AD && s == names[0] + 3, "UTF-8: first character");
  s = names[2] + 4;
  CHECK(utf8_next(&s) == 0xFFFD && *s == ' ', "UTF-8: invalid byte");
  s = names[2] + 11;
  CHECK(utf8_next(&s) == 0xFFFD && *s == '\0', "UTF-8: cut-off character at the end");

  static uint8_t junk[64] = "JVJF";
  memset(&ext, 0, sizeof(ext));
  CHECK(jpfont_load(mem_read, junk, names, 1, &ext) < 0 && ext.n == 0,
        "not a font file: refused, nothing loaded");

  FILE *f = fopen(font_path, "rb");
  CHECK(f != NULL, "jpfont.bin present");
  if (!f)
    return 1;
  int n = jpfont_load(file_read, f, names, 3, &ext);
  CHECK(n == 12, "glyphs loaded for the names (8 kana/letters + 4 kanji)");
  CHECK(ext_glyph(&ext, 0x30AD) && ext_glyph(&ext, 0xFF26) &&
        ext_glyph(&ext, 0x82F1), "kana, full-width and kanji found");
  CHECK(ext_glyph(&ext, 0x4E00) == NULL, "glyph not asked for: not loaded");
  int sorted = 1;
  for (int i = 1; i < ext.n; i++)
    sorted &= ext.cp[i - 1] < ext.cp[i];
  CHECK(sorted, "set sorted by code point");
  uint32_t used = ext.used;
  CHECK(jpfont_load(file_read, f, names, 3, &ext) == n && ext.used == used,
        "loading again adds nothing");

  /* fill the set: many different kanji */
  static char many[EXT_MAX_GLYPHS * 2 * 3 + 1];
  int k = 0;
  for (uint32_t cp = 0x4E00; k < (int)sizeof(many) - 4; cp++) {
    many[k++] = (char)(0xe0 | (cp >> 12));
    many[k++] = (char)(0x80 | ((cp >> 6) & 0x3f));
    many[k++] = (char)(0x80 | (cp & 0x3f));
  }
  const char *m1[] = { many };
  n = jpfont_load(file_read, f, m1, 1, &ext);
  CHECK(n > 12 && n <= EXT_MAX_GLYPHS && ext.used <= EXT_BITMAP_SIZE,
        "full set: stops at its limits");
  fclose(f);

  memset(&ext, 0, sizeof(ext));
  f = fopen(font_path, "rb");
  jpfont_load(file_read, f, names, 3, &ext);
  fclose(f);
  m->ext = &ext;
  strcpy(m->slots[0].title, names[0]);
  strcpy(m->slots[1].title, names[1]);
  /* longer than the panel: must be cut at a character, with "..." */
  static char longname[128];
  longname[0] = '\0';
  while (strlen(longname) + 3 < sizeof(longname) - 1)
    strcat(longname, "\xe8\x8b\xb1");
  strcpy(m->slots[2].title, longname);
  m->slots[2].used = 1;
  m->mode = MV_BROWSE;
  m->is_error = 0;
  for (int i = 0; i < 3; i++) {
    char name[48];
    m->selected = i;
    snprintf(name, sizeof(name), "Japanese name, slot %d", i + 1);
    rc |= check(name, m);
  }
  m->ext = NULL;
  return rc;
}

int main(int argc, char **argv) {
  MenuModel m;
  int rc = 0;

  for (int i = 0; i < JV_NUM_SLOTS; i++)
    for (int p = 0; p < JV_THUMB_W * JV_THUMB_H; p++)
      thumbs[i][p] = (uint16_t)((p * 2654435761u) >> 7 ^ (uint32_t)i);

  memset(&m, 0, sizeof(m));
  m.can_load = m.can_save = 1;
  m.progress = -1;
  for (int i = 0; i < JV_NUM_SLOTS; i++) {
    m.slots[i].used = i < 3;
    m.slots[i].prepared = i == 3;
    m.slots[i].thumb = i < 3 ? thumbs[i] : NULL;
    strcpy(m.slots[i].titleid, "PCSG00490");
    strcpy(m.slots[i].title, "The Legend of Heroes: Trails of Cold Steel II");
    strcpy(m.slots[i].date, "2026-11-01  17:24");
    strcpy(m.slots[i].app_ver, "01.03");
    strcpy(m.slots[i].installed_ver, "01.03");
  }

  printf("=== menu drawn in bands == drawn whole ===\n");
  for (int s = 0; s < JV_NUM_SLOTS; s++) {
    char name[32];
    m.selected = s;
    snprintf(name, sizeof(name), "browse, slot %d", s + 1);
    rc |= check(name, &m);
  }
  m.selected = 1;
  m.swap_ox = 1;
  rc |= check("browse, O confirms", &m);
  m.mode = MV_CONFIRM_SAVE;
  rc |= check("confirm save", &m);
  m.mode = MV_BUSY;
  m.progress = 42;
  strcpy(m.message, "Preparing slot 4");
  m.can_cancel = 1;
  rc |= check("busy with progress", &m);
  m.mode = MV_MESSAGE;
  m.is_error = 1;
  strcpy(m.message, "Something went wrong");
  rc |= check("error message", &m);
  m.mode = MV_DIAG;
  rc |= check("diagnostics", &m);

  if (argc > 1) {
    printf("=== Japanese game names ===\n");
    rc |= japanese(argv[1], &m);
  }
  return rc;
}
