/* JamaisVu savestate plugin
 *
 * Layout of the on-screen slot menu. Pure function of a MenuModel, no SDK
 * dependencies (also used by the host preview).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __JV_MENU_VIEW_H__
#define __JV_MENU_VIEW_H__

#include "render.h"

#define JV_NUM_SLOTS 5
#define JV_THUMB_W   160
#define JV_THUMB_H   90

/* Design resolution: the menu is laid out for 960x544 and scaled to the
 * game's framebuffer size when drawn. */
#define JV_SCREEN_W 960
#define JV_SCREEN_H 544

#define MENU_W 880
#define MENU_H 300
#define MENU_X ((JV_SCREEN_W - MENU_W) / 2)
#define MENU_Y ((JV_SCREEN_H - MENU_H) / 2)

#define TOAST_W 560
#define TOAST_H 56
#define TOAST_X ((JV_SCREEN_W - TOAST_W) / 2)
#define TOAST_Y 24

typedef struct {
  int used;
  int locked;             /* not enough free space to create this slot */
  int prepared;           /* slot file exists: saving starts right away */
  char titleid[16];
  char title[128];         /* UTF-8 */
  char date[32];          /* preformatted, e.g. "2026-10-31  21:40" */
  char app_ver[8];        /* game version of the snapshot, e.g. "01.02" */
  char installed_ver[8];  /* game version installed now ("" = unknown) */
  int outdated;           /* versions differ: the slot cannot be loaded */
  int config_changed;     /* tai/config.txt differs: cannot be loaded */
  const uint32_t *thumb;  /* JV_THUMB_W * JV_THUMB_H pixels, or NULL */
} MenuSlot;

typedef enum {
  MV_BROWSE,
  MV_CONFIRM_SAVE,
  MV_CONFIRM_LOAD,
  MV_BUSY,
  MV_MESSAGE,
  MV_DIAG,      /* diagnostics: reset test / dry run */
  MV_CONFIRM_PREPARE, /* create slot file(s) ahead of time */
} MenuMode;

typedef struct {
  MenuSlot slots[JV_NUM_SLOTS];
  int selected;
  MenuMode mode;
  char message[128];  /* BUSY / MESSAGE text */
  int progress;       /* BUSY: 0..100, or -1 for none */
  int can_cancel;     /* BUSY: ○ cancels (preparing a slot) */
  int swap_ox;        /* ○ confirms, ✕ goes back (Japanese/Asian games) */
  int refreshing;     /* slot list being re-read: missing thumbs = loading */
  int is_error;       /* MESSAGE: red text */
  int can_load;       /* 0 hides the load hint */
  int can_save;       /* 0 in the LiveArea: no game on screen to save */
  int prepare_count;  /* slots that could be prepared now */
  int prepare_all;    /* CONFIRM_PREPARE: all of them, not just the selected */
  const ExtGlyphs *ext; /* glyphs for non-ASCII game names, or NULL */
  int savedata_rolls_back; /* 0 when slots are on uma0: ux0 savedata is
                              not part of the snapshot */
} MenuModel;

void menu_view_render(Canvas *c, const MenuModel *m);
/* The menu's logo, scale times as large, top-left at x, y; returns its
 * width in pixels (c may be NULL to only measure) */
int menu_view_logo(Canvas *c, int x, int y, int scale);
void menu_view_render_toast(Canvas *c, const char *msg, int is_error);

#endif
