/* JamaisVu savestate plugin
 *
 * Diagnostics "Display test": the first step towards showing save/load
 * progress on screen. The payload runs while Vita OS is suspended, with
 * the display switched off; this switches the console's display on (the
 * OLED, the HDMI bridge: JamaisVu's own code in this folder; the PCH-2000
 * LCD: xerpi's panel file in payload/xerpi, see its NOTICE.md), shows a
 * test screen for a few seconds, and switches it off again.
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
#include "vdisp.h"
#include "utils.h"
#include "../xerpi/lcd.h"
#include "../../JamaisVu.h"
#include "../../shared.h"

uint32_t g_disp_hw_info, g_disp_hw_flags0; /* read by sysroot.h */

extern unsigned char msx_font[]; /* 8x8, font_data.c */

#define RGB(r, g, b) (0xFF000000u | ((b) << 16) | ((g) << 8) | (r))

static volatile uint32_t *g_fb;
static uint32_t g_vic, g_type;
static int g_pitch, g_w, g_h;
/* where the picture goes: the display path (link and controller) of the
 * console's display, and the picture's place on the output */
static int g_path = VD_PANEL;
static uint32_t g_dst_x, g_dst_y;
static uint32_t g_out_w, g_out_h; /* plane output: 960x544, HDMI 1280x720 */
static int g_hdmi_tv; /* PS TV: a TV answered (hot-plug detect) */

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

/* the source step the system programs when it scales 960x544 up to
 * 1280x720 (16.16; 66481a6 readout), 0 = unscaled */
static uint32_t scale_step(void) {
	return g_type == JV_DISP_HDMI ? 0xC16C : 0;
}

static void plane_setup(uint32_t paddr) {
	for (int p = 0; p < 2; p++) {
		vd_plane_colour(g_path, p, g_type == JV_DISP_HDMI);
		for (int c = 0; c < 2; c++)
			vd_plane_picture(g_path, p, c, paddr, g_dst_x, g_dst_y,
			                 g_out_w, g_out_h, scale_step());
	}
	vd_plane_select(g_path, VD_PLANE_A, VD_CFG_0);
	vd_plane_enable(g_path, VD_PLANE_B, VD_CFG_0, 0);
	vd_plane_enable(g_path, VD_PLANE_B, VD_CFG_1, 0);
	vd_bus_enable(g_path);
}

/* Only what this was written for. The screen is always laid out as
 * 960x544 with rows of 960 inside the frame's memory, so the frame must
 * hold that much (the plugin checked it is one physically contiguous
 * block of pitch x height x 4 bytes):
 *   LCD/OLED: exactly the game's 960x544 frame (pitch 960..1024);
 *   HDMI: any frame up to 1920x1088 that is big enough (the PS TV home
 *   screen may use a larger one). */
static int args_ok(const JvDisplayArgs *a) {
	uint64_t bytes = (uint64_t)a->pitch * a->height * 4;
	if (a->disp_type == JV_DISP_LCD || a->disp_type == JV_DISP_OLED) {
		if (a->width != 960 || a->height != 544 || a->pitch < 960 ||
		    a->pitch > 1024)
			return 0;
	} else if (a->disp_type == JV_DISP_HDMI) {
		if (a->width > 1920 || a->height > 1088 || a->pitch < a->width ||
		    a->pitch > 2048 || bytes < 960u * 544 * 4)
			return 0;
	} else {
		return 0;
	}
	return !(a->fb_paddr & 0xFF) && a->fb_paddr >= 0x20000000u &&
	       (uint64_t)a->fb_paddr + bytes <= 0x60000000u;
}

/* Switches the display on showing whatever draw() put into the frame. */
static void hw_on(const JvDisplayArgs *a, void (*draw)(void)) {
	const VdTiming *t;

	g_type = a->disp_type;
	if (g_type == JV_DISP_HDMI) {
		/* 1280x720p 60 Hz; the system's own HDMI setting (480p/720p/
		 * 1080i) comes back when it wakes */
		g_vic = 0;
		t = &vd_timing_hdmi_720p;
		g_path = VD_HDMI;
		/* the whole 1280x720 output, the picture scaled up as the
		 * system does (from 4 px in; 66481a6 readout) */
		g_dst_x = 4;
		g_dst_y = 0;
		g_out_w = 1280;
		g_out_h = 720;
	} else {
		/* LCD: timing B (clean on the PCH-2000; with timing A, garbage
		 * along the edges there). OLED: timing A */
		g_vic = g_type == JV_DISP_LCD && a->vic == 0x20 ? 0x20 : 0;
		t = g_vic ? &vd_timing_panel_b : &vd_timing_panel;
		g_path = VD_PANEL;
		g_dst_x = g_dst_y = 0;
		g_out_w = 960;
		g_out_h = 544;
	}

	g_fb = (volatile uint32_t *)(uintptr_t)a->fb_paddr;
	/* The display controller is programmed for rows exactly as wide as
	 * the picture, but games may use a wider pitch (1001 Spikes: 1024 -
	 * diagonal stripes on hardware when the rows were read as 960). The
	 * screen covers the whole frame anyway, so lay it out with rows of
	 * 960; the frame is big enough (pitch >= width). */
	g_pitch = 960; /* the screen is always 960x544, rows of 960 (HDMI: the
	                  frame may be larger; only its first 2 MB are used) */
	g_w = 960;
	g_h = 544;
	g_disp_hw_info = a->hw_info;
	g_disp_hw_flags0 = a->hw_flags0;

	/* I2C bus 1: the LCD backlight controller / the PS TV's HDMI bridge
	 * (the OLED panel needs none) */
	if (g_type != JV_DISP_OLED) {
		vd_clk_i2c_on(1);
		i2c_init_bus(1);
	}
	mark(JV_DISP_STEP_I2C);

	/* the display link: its pixel clock, clocks and resets, then the
	 * link itself sending the timing */
	vd_clk_link_pll(g_path, t->pixelclock);
	vd_clk_link_on(g_path);
	vd_link_start(g_path, t);
	mark(JV_DISP_STEP_DSI);

	draw(); /* before the panel lights up: no stale picture */
	mark(JV_DISP_STEP_DRAWN);

	if (g_type == JV_DISP_LCD) {
		lcd_init();
	} else if (g_type == JV_DISP_OLED) {
		oled_init();
	} else {
		hdmi_init();
		/* no TV answering: the link runs, nothing is shown */
		g_hdmi_tv = hdmi_get_hpd_state();
		if (g_hdmi_tv)
			hdmi_connect();
	}
	mark(JV_DISP_STEP_LCD);

	plane_setup(a->fb_paddr);
	mark(JV_DISP_STEP_PLANE);

	/* PS TV: every HDMI display register as our picture leaves them, for
	 * the plugin to log next to the system's own (the picture is still
	 * wrong while every register we write matches the system) */
	if (g_type == JV_DISP_HDMI) {
		static const JvRegRange r[] = JV_REGSNAP_RANGES;
		volatile uint32_t *d = (volatile uint32_t *)REGSNAP_PADDR;
		uint32_t k = 1;
		for (uint32_t i = 0; i < sizeof(r) / sizeof(r[0]); i++)
			for (uint32_t w = 0; w < r[i].words; w++)
				d[k++] = *(volatile uint32_t *)(r[i].paddr + 4 * w);
		d[0] = JV_REGSNAP_MAGIC;
	}
}

/* back to how it was: panel and backlight off, link stopped - each step
 * recorded on its own (on hardware the link stop never confirmed: its
 * bounded wait gives up after ~1 s) */
static void hw_off(void) {
	/* black first, kept for a couple of frames, so whatever happens while
	 * the panel shuts down shows nothing */
	fill(0, 0, g_w, g_h, RGB(0, 0, 0));
	delay(6000);
	if (g_type == JV_DISP_LCD)
		lcd_off();
	else if (g_type == JV_DISP_OLED)
		oled_off();
	else
		hdmi_off();
	mark(JV_DISP_STEP_LCD_OFF);
	vd_plane_enable(g_path, VD_PLANE_A, VD_CFG_0, 0);
	vd_plane_enable(g_path, VD_PLANE_A, VD_CFG_1, 0);
	mark(JV_DISP_STEP_PLANE_OFF);
	vd_link_stop(g_path);
	vd_clk_link_off(g_path);
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
	centred(470, 3, C_TEXT, g_type == JV_DISP_OLED ? "OLED" :
	        g_type == JV_DISP_HDMI ? "HDMI 720p" :
	        g_vic == 0x20 ? "LCD - timing B" : "LCD - timing A");
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

/* Drawn with the menu's own renderer, font and logo (plugin/gui), in the
 * menu's colours, so it looks like part of the same thing. */
#include "render.h"
#include <stddef.h>
#include "logo.h"

#define P_BG     RGBA(244, 246, 250, 255) /* menu panel */
#define P_TEXT   RGBA(18, 22, 30, 255)
#define P_DIM    RGBA(88, 98, 114, 255)
#define P_BARBG  RGBA(200, 208, 220, 255)
#define P_ACCENT RGBA(34, 178, 226, 255)

#define PB_X 180
#define PB_Y 350
#define PB_W 600
#define PB_H 16
#define LOGO_SCALE 3

static const char *g_title;
static const char *g_line = "";
static int g_screen_on, g_pct;
static JvDisplayArgs g_args;

static Canvas canvas(void) {
	Canvas c = { (uint32_t *)g_fb, g_w, g_h, 0 };
	return c;
}

static void draw_line(void) {
	Canvas c = canvas();
	cv_fill(&c, 0, 290, g_w, font_small.height + 4, P_BG);
	cv_text_centered(&c, &font_small, g_w / 2, 292, g_line, P_DIM, g_w - 80);
}

static void draw_bar(void) {
	Canvas c = canvas();
	int w = PB_W * g_pct / 100;
	cv_fill(&c, PB_X - 2, PB_Y - 2, PB_W + 4, PB_H + 4, P_BG);
	cv_round_rect(&c, PB_X, PB_Y, PB_W, PB_H, PB_H / 2, P_BARBG);
	if (w > 0)
		cv_round_rect(&c, PB_X, PB_Y, w < PB_H ? PB_H : w, PB_H, PB_H / 2,
		              P_ACCENT);
}

/* Everything but the logo: quick enough to draw before the panel lights
 * up (the logo took most of the 1.3 s the first picture needed). */
static void draw_quick(void) {
	Canvas c = canvas();
	cv_clear(&c, P_BG);
	cv_text_centered(&c, &font_large, g_w / 2, 236, g_title, P_TEXT, g_w - 80);
	cv_text_centered(&c, &font_small, g_w / 2, 420,
	                 "Do not turn off the system", P_DIM, g_w - 80);
	draw_line();
	draw_bar();
}

static void draw_logo_now(void) {
	Canvas c = canvas();
	int lw = logo_draw(NULL, 0, 0, LOGO_SCALE);
	logo_draw(&c, (g_w - lw) / 2, 110, LOGO_SCALE);
}

static void draw_progress(void) {
	draw_quick();
	draw_logo_now();
}

int jv_screen_begin(const JvDisplayArgs *a, void (*markfn)(int step),
                    const char *title) {
	if (g_screen_on || !args_ok(a))
		return -1;
	g_mark = markfn;
	g_title = title;
	g_line = "";
	g_pct = 0;
	g_args = *a;
	/* panel on as soon as the plain screen is drawn; the logo follows
	 * while it is already showing */
	hw_on(a, draw_quick);
	draw_logo_now();
	g_screen_on = 1;
	return 0;
}

void jv_screen_status(const char *line, int percent) {
	if (!g_screen_on)
		return;
	if (line) {
		g_line = line;
		draw_line();
	}
	if (percent >= 0) {
		if (percent > 100)
			percent = 100;
		if (percent > g_pct) { /* never goes back */
			g_pct = percent;
			draw_bar();
		}
	}
}

void jv_screen_redraw(void) {
	if (g_screen_on)
		draw_progress();
}

/* Moves the screen to another frame: drawn there completely first, then
 * the display controller's second configuration is pointed at it and
 * selected (the switch happens at a frame boundary). */
int jv_screen_switch(uint32_t paddr, uint32_t pitch) {
	JvDisplayArgs b;
	if (!g_screen_on)
		return -1;
	b = g_args;
	b.fb_paddr = paddr;
	b.pitch = pitch;
	/* the snapshot's frame is a game's 960x544 frame (the plugin records
	 * no other), whatever the first frame was (PS TV home screen) */
	b.width = 960;
	b.height = 544;
	if (!args_ok(&b))
		return -1;
	g_fb = (volatile uint32_t *)(uintptr_t)paddr;
	draw_progress();

	vd_plane_picture(g_path, VD_PLANE_A, VD_CFG_1, paddr, g_dst_x, g_dst_y,
	                 g_out_w, g_out_h, scale_step());
	vd_plane_select(g_path, VD_PLANE_A, VD_CFG_1);
	g_args = b;
	return 0;
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
