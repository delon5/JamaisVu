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

static uint32_t whole[MENU_W * MENU_H], banded[MENU_W * MENU_H];
static uint32_t band[MENU_W * MENU_H];
static uint32_t thumbs[JV_NUM_SLOTS][JV_THUMB_W * JV_THUMB_H];

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

int main(void) {
  MenuModel m;
  int rc = 0;

  for (int i = 0; i < JV_NUM_SLOTS; i++)
    for (int p = 0; p < JV_THUMB_W * JV_THUMB_H; p++)
      thumbs[i][p] = (uint32_t)(p * 2654435761u) ^ (uint32_t)i;

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
  return rc;
}
