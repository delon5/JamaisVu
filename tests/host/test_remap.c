/* ✕/○ convention: PSV-Shell-Plus profile and reVita INI parsing.
 * The PSV-Shell struct below is copied from its src/profile.h (+ oc.h,
 * bt.h); run.sh builds this test with and without -fshort-enums so both
 * possible file layouts are covered. */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "remap.h"

static int checks, failures;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; \
  printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
  printf("\n"); } } while (0)

typedef enum { OC_CPU, OC_GPU_ES4, OC_BUS, OC_GPU_XBAR, OC_MAX } psvs_oc_device_t;
typedef enum { OC_MODE_DEFAULT, OC_MODE_MANUAL, OC_MODE_MAX } psvs_oc_mode_t;
typedef enum { BT_T0, BT_T1, BT_T2 } psvs_bt_touch_t;
typedef enum { BT_M0, BT_M1 } psvs_bt_motion_t;
typedef struct psvs_app_profile_t {
  char ver[8];
  psvs_oc_mode_t mode[OC_MAX];
  int manual_freq[OC_MAX];
  bool swap_buttons;
  bool disable_L3R3;
  psvs_bt_touch_t bt_touch;
  psvs_bt_motion_t bt_motion;
} psvs_app_profile_t;

static const char *k_revita_swap =
  "[PROFILE]\nNAME=PCSG01179\nVERSION=1\n"
  "\n[RULE:0]\nPROPAGATE=FALSE\nTURBO=0\nSTICKY=FALSE\nDISABLED=FALSE\n"
  "TRIGGER_ACTION_TYPE=BUTTON\nTRIGGER_BUTTONS=CROSS,\n"
  "EMU_ACTION_TYPE=BUTTON\nEMU_BUTTONS=CIRCLE,\n"
  "\n[RULE:1]\nPROPAGATE=FALSE\nTURBO=0\nSTICKY=FALSE\nDISABLED=FALSE\n"
  "TRIGGER_ACTION_TYPE=BUTTON\nTRIGGER_BUTTONS=CIRCLE,\n"
  "EMU_ACTION_TYPE=BUTTON\nEMU_BUTTONS=CROSS,\n";

int main(void) {
  psvs_app_profile_t p;
  printf("remap (PSV-Shell-Plus profile %zu bytes, reVita INI)\n", sizeof(p));

  memset(&p, 0, sizeof(p));
  memcpy(p.ver, "PSVP0100", 8);
  p.mode[OC_CPU] = OC_MODE_MANUAL;
  p.manual_freq[OC_CPU] = 444;
  p.disable_L3R3 = true;
  p.bt_touch = BT_T2;
  CHECK(psvs_profile_swaps((const uint8_t *)&p, sizeof(p)) == 0, "swap off");
  p.swap_buttons = true;
  CHECK(psvs_profile_swaps((const uint8_t *)&p, sizeof(p)) == 1, "swap on");
  memcpy(p.ver, "PSVP0099", 8);
  CHECK(psvs_profile_swaps((const uint8_t *)&p, sizeof(p)) == -1,
        "unknown version");

  CHECK(revita_ini_swaps(k_revita_swap, strlen(k_revita_swap)) == 1,
        "reVita ✕<->○ rules");
  {
    char buf[1024];
    /* CRLF line endings */
    int o = 0;
    for (const char *s = k_revita_swap; *s; s++) {
      if (*s == '\n') buf[o++] = '\r';
      buf[o++] = *s;
    }
    CHECK(revita_ini_swaps(buf, o) == 1, "CRLF");
  }
  {
    const char *t = "[RULE:0]\nDISABLED=TRUE\nTRIGGER_ACTION_TYPE=BUTTON\n"
                    "TRIGGER_BUTTONS=CROSS,\nEMU_ACTION_TYPE=BUTTON\n"
                    "EMU_BUTTONS=CIRCLE,\n";
    CHECK(revita_ini_swaps(t, strlen(t)) == 0, "disabled rule ignored");
  }
  {
    const char *t = "[RULE:0]\nDISABLED=FALSE\nTRIGGER_ACTION_TYPE=BUTTON\n"
                    "TRIGGER_BUTTONS=CROSS,L1,\nEMU_ACTION_TYPE=BUTTON\n"
                    "EMU_BUTTONS=CIRCLE,\n"
                    "[RULE:1]\nDISABLED=FALSE\nTRIGGER_ACTION_TYPE=BUTTON\n"
                    "TRIGGER_BUTTONS=CROSS,\nEMU_ACTION_TYPE=BUTTON\n"
                    "EMU_BUTTONS=TRIANGLE,\n"
                    "[RULE:2]\nDISABLED=FALSE\nTRIGGER_ACTION_TYPE=FRONT_TOUCH_ZONE\n"
                    "TRIGGER_ACTION=TOUCH_ZONE_L\nEMU_ACTION_TYPE=BUTTON\n"
                    "EMU_BUTTONS=CIRCLE,\n";
    CHECK(revita_ini_swaps(t, strlen(t)) == 0,
          "combos, other buttons and touch triggers are not a swap");
  }
  {
    const char *t = "[PROFILE]\nTRIGGER_BUTTONS=CROSS,\nEMU_BUTTONS=CIRCLE,\n";
    CHECK(revita_ini_swaps(t, strlen(t)) == 0, "keys outside [RULE] ignored");
  }

  CHECK(title_confirms_with_circle("PCSG01179"), "Japanese title");
  CHECK(title_confirms_with_circle("PCSH00123"), "Asian title");
  CHECK(!title_confirms_with_circle("PCSE00120"), "US title");
  CHECK(!title_confirms_with_circle("PCSB00245"), "EU title");
  CHECK(menu_swap_ox("PCSG01179", 0, 0) == 1, "JP game -> O confirms");
  CHECK(menu_swap_ox("PCSG01179", 1, -1) == 0, "JP game + PSV-Shell swap -> X");
  CHECK(menu_swap_ox("PCSG01179", -1, 1) == 0, "JP game + reVita swap -> X");
  CHECK(menu_swap_ox("PCSE00120", 1, 0) == 1, "US game + swap -> O");
  CHECK(menu_swap_ox("PCSE00120", -1, -1) == 0, "US game -> X");
  CHECK(menu_swap_ox("PCSG01179", 1, 1) == 1, "two swaps cancel out");

  printf("%d checks, %d failures\n", checks, failures);
  return failures != 0;
}
