/* JamaisVu savestate plugin
 *
 * Diagnostics "Display test": the first step towards showing save/load
 * progress on screen. The payload runs while Vita OS is suspended, with
 * the display switched off; this switches the PCH-2000's LCD on with
 * xerpi's libbaremetal code (this folder, see NOTICE.md), shows a test
 * screen for a few seconds, and switches it off again.
 *
 * It draws only into the frame the game was showing (its address comes
 * from the plugin): display memory the game redraws anyway. No other
 * memory is touched and nothing is written to the Memory Card except the
 * usual trace.bin step record.
 *
 * Written for JamaisVu; MIT licence like the rest of JamaisVu.
 */

#include <stdint.h>
#include "display_test.h"
#include "pervasive.h"
#include "dsi.h"
#include "iftu.h"
#include "lcd.h"
#include "i2c.h"
#include "utils.h"

uint32_t g_disp_hw_info, g_disp_hw_flags0; /* read by sysroot.h */

extern unsigned char msx_font[]; /* 8x8, font_data.c */

/* colour conversion matrices as in libbaremetal's display.c */
static const struct iftu_csc_params csc0 = {
	0x40, 0x202, 0x3FF,
	   0,     0,     0,
	{
		{0x254,     0, 0x395},
		{0x254, 0xF93, 0xEF0},
		{0x254, 0x439,     0}
	}
};

static const struct iftu_csc_params csc1 = {
	0x40,  0x40, 0x3AC,
	0x40, 0x3AC,  0x40,
	{
		{0x1B7,     0,     0},
		{    0, 0x1B7,     0},
		{    0,     0, 0x1B7}
	}
};

#define RGB(r, g, b) (0xFF000000u | ((b) << 16) | ((g) << 8) | (r))

static volatile uint32_t *g_fb;
static uint32_t g_vic;
static int g_pitch, g_w, g_h;

static void fill(int x, int y, int w, int h, uint32_t c) {
	for (int j = y; j < y + h && j < g_h; j++)
		for (int i = x; i < x + w && i < g_w; i++)
			g_fb[j * g_pitch + i] = c;
}

static void text(int x, int y, int s, uint32_t c, const char *str) {
	for (; *str; str++, x += 8 * s) {
		unsigned char ch = (unsigned char)*str;
		if (ch < 0x20 || ch > 0x7E)
			continue;
		const unsigned char *g = &msx_font[ch * 8];
		for (int r = 0; r < 8; r++)
			for (int b = 0; b < 8; b++)
				if (g[r] & (0x80 >> b))
					fill(x + b * s, y + r * s, s, s, c);
	}
}

static int text_w(const char *s, int scale) {
	int n = 0;
	while (s[n])
		n++;
	return n * 8 * scale;
}

#define BAR_X 180
#define BAR_Y 330
#define BAR_W 600
#define BAR_H 28

static void (*g_mark)(int step);
static void mark(int step) {
	if (g_mark)
		g_mark(step);
}

static void plane_setup(uint32_t paddr) {
	static const enum iftu_plane planes[2] = { IFTU_PLANE_A, IFTU_PLANE_B };
	static const enum iftu_plane_config cfgs[2] = { IFTU_PLANE_CONFIG_0,
	                                                IFTU_PLANE_CONFIG_1 };
	struct iftu_plane_fb_config fb;

	fb.paddr = paddr;
	fb.pixelformat = IFTU_FB_PIXELFORMAT_A8B8G8R8;
	fb.width = g_w;
	fb.height = g_h;
	for (int p = 0; p < 2; p++)
		for (int c = 0; c < 2; c++) {
			iftu_plane_set_alpha(IFTU_BUS_0, planes[p], 256);
			iftu_plane_set_csc0(IFTU_BUS_0, planes[p], &csc0);
			iftu_plane_set_csc1(IFTU_BUS_0, planes[p], &csc1);
			iftu_plane_set_csc_enabled(IFTU_BUS_0, planes[p], true);
			iftu_plane_config_set_config(IFTU_BUS_0, planes[p], cfgs[c], &fb,
			                             0, 0, g_w, g_h);
		}
	iftu_bus_plane_config_select(IFTU_BUS_0, IFTU_PLANE_A, IFTU_PLANE_CONFIG_0);
	iftu_plane_config_set_enabled(IFTU_BUS_0, IFTU_PLANE_B, IFTU_PLANE_CONFIG_0, false);
	iftu_plane_config_set_enabled(IFTU_BUS_0, IFTU_PLANE_B, IFTU_PLANE_CONFIG_1, false);
	iftu_bus_alpha_blending_control(IFTU_BUS_0, 0);
	iftu_bus_enable(IFTU_BUS_0);
}

/* only what this was written for: a 960x544 frame in CDRAM or main
 * memory, as the plugin checked */
static int args_ok(const JvDisplayArgs *a) {
	return a->width == 960 && a->height == 544 && a->pitch >= 960 &&
	       a->pitch <= 1024 && !(a->fb_paddr & 0xFFF) &&
	       a->fb_paddr >= 0x20000000u &&
	       (uint64_t)a->fb_paddr + (uint64_t)a->pitch * a->height * 4 <=
	           0x60000000u;
}

/* Switches the display on showing whatever draw() put into the frame. */
static void hw_on(const JvDisplayArgs *a, void (*draw)(void)) {
	uint32_t pixelclock = 0;

	/* 0x20 for the PCH-2000 LCD (clean on hardware; 0 = the OLED timing
	 * libbaremetal used, garbage along the edges); nothing else */
	uint32_t vic = a->vic == 0x20 ? 0x20 : 0;

	g_fb = (volatile uint32_t *)(uintptr_t)a->fb_paddr;
	/* The display controller reads rows exactly as wide as the picture
	 * (libbaremetal's iftu.c: "TODO: Properly use pitch instead of
	 * width"), but games may use a wider pitch (1001 Spikes: 1024 -
	 * diagonal stripes on hardware). The screen covers the whole frame
	 * anyway, so lay it out with rows of 960; the frame is big enough
	 * (pitch >= width). */
	g_pitch = a->width;
	g_w = a->width;
	g_h = a->height;
	g_vic = vic;
	g_disp_hw_info = a->hw_info;
	g_disp_hw_flags0 = a->hw_flags0;

	/* I2C bus 1: the LCD backlight controller */
	pervasive_clock_enable_i2c(1);
	pervasive_reset_exit_i2c(1);
	i2c_init_bus(1);
	mark(JV_DISP_STEP_I2C);

	/* display link, 960x544 (as libbaremetal's display_init_lcd) */
	dsi_get_pixelclock_for_vic(vic, 24, &pixelclock);
	pervasive_dsi_set_pixelclock(0, pixelclock);
	pervasive_clock_enable_dsi(0, 0xF);
	pervasive_reset_exit_dsi(0, 7);
	dsi_init();
	dsi_start_master(DSI_BUS_OLED_LCD, vic);
	dsi_start_display(DSI_BUS_OLED_LCD, vic, 1);
	mark(JV_DISP_STEP_DSI);

	draw(); /* before the panel lights up: no stale picture */
	mark(JV_DISP_STEP_DRAWN);

	lcd_init();
	mark(JV_DISP_STEP_LCD);

	plane_setup(a->fb_paddr);
	mark(JV_DISP_STEP_PLANE);
}

/* back to how it was: panel and backlight off, link stopped - each step
 * recorded on its own (on hardware the link stop never confirmed: its
 * bounded wait gives up after ~1 s) */
static void hw_off(void) {
	/* black first, kept for a couple of frames, so whatever happens while
	 * the panel shuts down shows nothing */
	fill(0, 0, g_w, g_h, RGB(0, 0, 0));
	delay(6000);
	lcd_off();
	mark(JV_DISP_STEP_LCD_OFF);
	iftu_plane_config_set_enabled(IFTU_BUS_0, IFTU_PLANE_A, IFTU_PLANE_CONFIG_0, false);
	iftu_plane_config_set_enabled(IFTU_BUS_0, IFTU_PLANE_A, IFTU_PLANE_CONFIG_1, false);
	mark(JV_DISP_STEP_PLANE_OFF);
	dsi_stop_master(DSI_BUS_OLED_LCD);
	pervasive_reset_enter_dsi(0, 7);
	pervasive_clock_disable_dsi(0, 0xF);
	mark(JV_DISP_STEP_OFF);
}

#define C_BG    RGB(16, 20, 28)
#define C_TITLE RGB(34, 178, 226)
#define C_TEXT  RGB(240, 240, 240)
#define C_DIM   RGB(170, 180, 195)
#define C_FRAME RGB(90, 100, 115)

static void centred(int y, int scale, uint32_t c, const char *s) {
	fill(0, y, g_w, 8 * scale, C_BG);
	text((g_w - text_w(s, scale)) / 2, y, scale, c, s);
}

static void bar_frame(void) {
	fill(BAR_X - 4, BAR_Y - 4, BAR_W + 8, BAR_H + 8, C_FRAME);
	fill(BAR_X, BAR_Y, BAR_W, BAR_H, C_BG);
}

/* --- Diagnostics display test ------------------------------------------- */

static void draw_test(void) {
	fill(0, 0, g_w, g_h, C_BG);
	text((g_w - text_w("JamaisVu", 5)) / 2, 120, 5, C_TITLE, "JamaisVu");
	centred(200, 3, C_TEXT, "Display test - nothing is written");
	centred(250, 2, C_DIM, "If you can read this, the screen works.");
	centred(470, 3, C_TEXT, g_vic == 0x20 ? "Timing B" : "Timing A");
	bar_frame();
	/* colour check: red, green, blue, white */
	static const uint32_t sw[4] = { RGB(220, 40, 40), RGB(40, 200, 60),
	                                RGB(40, 90, 230), RGB(255, 255, 255) };
	for (int k = 0; k < 4; k++)
		fill(330 + k * 80, 400, 60, 40, sw[k]);
}

int jv_display_test(const JvDisplayArgs *a, void (*markfn)(int step),
                    uint32_t (*now_us)(void)) {
	if (!args_ok(a))
		return -1;
	g_mark = markfn;
	hw_on(a, draw_test);

	/* ~6 s: a bar filling up, like save/load progress would */
	uint32_t t0 = now_us();
	for (;;) {
		uint32_t el = now_us() - t0;
		if (el >= 6000000)
			break;
		fill(BAR_X, BAR_Y, (int)((uint64_t)BAR_W * el / 6000000), BAR_H,
		     C_TITLE);
		delay(2000);
	}
	fill(BAR_X, BAR_Y, BAR_W, BAR_H, C_TITLE);
	mark(JV_DISP_STEP_SHOWN);

	hw_off();
	g_mark = 0;
	return 0;
}

/* --- Progress screen for saves, loads and dry runs ---------------------- */

static const char *g_title;
static int g_screen_on, g_last_fill;

static void draw_progress(void) {
	fill(0, 0, g_w, g_h, C_BG);
	text((g_w - text_w("JamaisVu", 5)) / 2, 110, 5, C_TITLE, "JamaisVu");
	centred(190, 3, C_TEXT, g_title);
	bar_frame();
	centred(420, 2, C_DIM, "Do not turn off the system");
	g_last_fill = 0;
}

int jv_screen_begin(const JvDisplayArgs *a, void (*markfn)(int step),
                    const char *title) {
	if (g_screen_on || !args_ok(a))
		return -1;
	g_mark = markfn;
	g_title = title;
	hw_on(a, draw_progress);
	g_screen_on = 1;
	return 0;
}

void jv_screen_status(const char *line, int percent) {
	if (!g_screen_on)
		return;
	if (line)
		centred(260, 2, C_DIM, line);
	if (percent >= 0) {
		if (percent > 100)
			percent = 100;
		int w = BAR_W * percent / 100;
		if (w < g_last_fill) /* never goes back */
			w = g_last_fill;
		fill(BAR_X + g_last_fill, BAR_Y, w - g_last_fill, BAR_H, C_TITLE);
		g_last_fill = w;
	}
}

void jv_screen_end(void) {
	if (!g_screen_on)
		return;
	g_screen_on = 0;
	hw_off();
	g_mark = 0;
}

int jv_screen_is_on(void) {
	return g_screen_on;
}

uint32_t jv_display_wait_timeouts(void) {
	return g_jv_wait_timeouts;
}
