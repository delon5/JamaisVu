/* JamaisVu: Diagnostics "Display test" - see display_test.c */
#ifndef JV_DISPLAY_TEST_H
#define JV_DISPLAY_TEST_H

#include <stdint.h>

typedef struct {
  uint32_t fb_paddr; /* the frame the game showed (physically contiguous) */
  uint32_t pitch, width, height; /* pixels */
  uint32_t hw_info, hw_flags0;   /* for the LCD backlight setup */
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

/* hardware waits that gave up (see JV_WAIT_WHILE in utils.h) */
uint32_t jv_display_wait_timeouts(void);

#endif
