/* JamaisVu savestate plugin
 *
 * Which physical button confirms in the running game. Japanese and Asian
 * games confirm with ○; button-remapping plugins can swap ✕ and ○ for a
 * game. Their profiles are read (never written) so the menu's confirm and
 * back buttons match what the player uses in that game:
 *
 * - PSV-Shell-Plus: ur0:data/PSVshell/profiles/<TITLEID> (else "global"),
 *   a raw psvs_app_profile_t starting with "PSVP0100"; swap_buttons
 *   follows two arrays of 4 enums / 4 ints.
 * - reVita: ur0:data/reVita/Profile/<TITLEID>.INI (else GLOBAL.INI), with
 *   [RULE:n] sections; an enabled BUTTON rule CROSS -> CIRCLE (or the
 *   reverse) swaps them.
 *
 * reVita also remaps kernel controller reads, but only for calls from the
 * shell or the running game; JamaisVu reads from a kernel thread, so it
 * always sees the physical buttons.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <string.h>

#include "remap.h"

#define PSVS_VER "PSVP0100"

int psvs_profile_swaps(const uint8_t *buf, int len) {
  int off;

  if (len < 8 || memcmp(buf, PSVS_VER, 8) != 0)
    return -1;
  /* char ver[8]; enum mode[4]; int manual_freq[4]; bool swap_buttons; ...
   * The enums are 4 bytes (52-byte file) or 1 byte (32-byte file). */
  if (len == 52)
    off = 8 + 4 * 4 + 4 * 4;
  else if (len == 32)
    off = 8 + 4 + 4 * 4;
  else
    return -1;
  return buf[off] ? 1 : 0;
}

/* ---- reVita INI ---------------------------------------------------------- */

#define B_CROSS  1
#define B_CIRCLE 2
#define B_OTHER  4

typedef struct {
  int in_rule;
  int disabled;
  int trig_button, emu_button; /* *_ACTION_TYPE == BUTTON */
  int trig, emu;               /* B_* masks */
} Rule;

static int starts(const char *s, int n, const char *p) {
  int l = (int)strlen(p);
  return n >= l && memcmp(s, p, l) == 0;
}

static int button_mask(const char *s, int n) {
  int mask = 0;
  int i = 0;
  while (i < n) {
    int j = i;
    while (j < n && s[j] != ',')
      j++;
    int l = j - i;
    while (l > 0 && (s[i + l - 1] == ' ' || s[i + l - 1] == '\r'))
      l--;
    if (l == 5 && memcmp(s + i, "CROSS", 5) == 0)
      mask |= B_CROSS;
    else if (l == 6 && memcmp(s + i, "CIRCLE", 6) == 0)
      mask |= B_CIRCLE;
    else if (l > 0)
      mask |= B_OTHER;
    i = j + 1;
  }
  return mask;
}

static int rule_swaps(const Rule *r) {
  return r->in_rule && !r->disabled && r->trig_button && r->emu_button &&
         ((r->trig == B_CROSS && r->emu == B_CIRCLE) ||
          (r->trig == B_CIRCLE && r->emu == B_CROSS));
}

int revita_ini_swaps(const char *text, int len) {
  Rule r;
  int swaps = 0;

  memset(&r, 0, sizeof(r));
  int i = 0;
  while (i < len) {
    int j = i;
    while (j < len && text[j] != '\n')
      j++;
    const char *l = text + i;
    int n = j - i;
    while (n > 0 && (l[n - 1] == '\r' || l[n - 1] == ' '))
      n--;

    if (n > 0 && l[0] == '[') {
      swaps |= rule_swaps(&r);
      memset(&r, 0, sizeof(r));
      r.in_rule = starts(l, n, "[RULE:");
    } else if (r.in_rule) {
      if (starts(l, n, "DISABLED="))
        r.disabled = starts(l + 9, n - 9, "TRUE");
      else if (starts(l, n, "TRIGGER_ACTION_TYPE="))
        r.trig_button = n - 20 == 6 && starts(l + 20, n - 20, "BUTTON");
      else if (starts(l, n, "EMU_ACTION_TYPE="))
        r.emu_button = n - 16 == 6 && starts(l + 16, n - 16, "BUTTON");
      else if (starts(l, n, "TRIGGER_BUTTONS="))
        r.trig = button_mask(l + 16, n - 16);
      else if (starts(l, n, "EMU_BUTTONS="))
        r.emu = button_mask(l + 12, n - 12);
    }
    i = j + 1;
  }
  swaps |= rule_swaps(&r);
  return swaps;
}

int title_confirms_with_circle(const char *tid) {
  static const char *const k_prefixes[] = {
    "PCSG", "PCSC", "VLJM", "VLJS", /* Japan */
    "PCSH", "PCSD", "VCAS", "VLAS", /* Asia  */
  };
  for (unsigned i = 0; i < sizeof(k_prefixes) / sizeof(k_prefixes[0]); i++)
    if (strncmp(tid, k_prefixes[i], 4) == 0)
      return 1;
  return 0;
}

int menu_swap_ox(const char *tid, int psvs_swap, int revita_swap) {
  return title_confirms_with_circle(tid) ^ (psvs_swap > 0) ^
         (revita_swap > 0);
}
