/* JamaisVu: Diagnostics "Display test" - see display_test.c */
#ifndef JV_DISPLAY_TEST_H
#define JV_DISPLAY_TEST_H

#include <stdint.h>

typedef struct {
  uint32_t fb_paddr; /* the frame the game showed (physically contiguous) */
  uint32_t pitch, width, height; /* pixels */
  uint32_t hw_info, hw_flags0;   /* for the LCD backlight setup */
  uint32_t vic;                  /* display timing: 0 (A) or 0x20 (B) */
} JvDisplayArgs;

/* Switches the PCH-2000 LCD on, shows a test screen with a moving progress
 * bar for a few seconds, switches it off again. mark() records each step.
 * Returns 0, or < 0 if the arguments are unusable. */
int jv_display_test(const JvDisplayArgs *a, void (*mark)(int step),
                    uint32_t (*now_us)(void));

/* steps passed to mark() */
#define JV_DISP_STEP_I2C   0
#define JV_DISP_STEP_DSI   1
#define JV_DISP_STEP_DRAWN 2
#define JV_DISP_STEP_LCD   3
#define JV_DISP_STEP_PLANE 4
#define JV_DISP_STEP_SHOWN 5
#define JV_DISP_STEP_LCD_OFF   6
#define JV_DISP_STEP_PLANE_OFF 7
#define JV_DISP_STEP_OFF   8 /* display link stopped: all done */

/* Progress screen for saves, loads and dry runs: switches the display on
 * with a title (returns < 0 and does nothing if the frame is unusable),
 * updates the status line and the bar (percent < 0: bar unchanged, line
 * NULL: line unchanged), switches it off. Drawn into the given frame
 * only. */
int jv_screen_begin(const JvDisplayArgs *a, void (*mark)(int step),
                    const char *title);
void jv_screen_status(const char *line, int percent);
/* draws the whole screen again (after something overwrote the frame) */
void jv_screen_redraw(void);
/* continues on another frame (same size, pitch 960-1024); < 0 if unusable */
int jv_screen_switch(uint32_t paddr, uint32_t pitch);
void jv_screen_end(void);
int jv_screen_is_on(void);

/* hardware waits that gave up (see JV_WAIT_WHILE in utils.h) */
uint32_t jv_display_wait_timeouts(void);

#endif
