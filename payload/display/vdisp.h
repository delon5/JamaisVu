/* JamaisVu savestate plugin
 *
 * The display while the system sleeps: JamaisVu's own bring-up of the
 * PCH-1000's OLED panel and the PS TV's HDMI bridge, written for JamaisVu
 * from the hardware's register maps (the Vita developer wiki: Pervasive,
 * DSI, IFTU, I2C, SceOled, SceHdmi, and the system's own register setup
 * read back on hardware). The PCH-2000 LCD still uses the panel file in
 * payload/xerpi (see its NOTICE.md); it drives the same link and
 * controller through this API.
 *
 * Only two things are ever shown: the console's own 960x544 picture on
 * its panel, or that picture scaled to 1280x720 over HDMI. Everything is
 * written for exactly those, progressive, 24 bits per pixel.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef JV_VDISP_H
#define JV_VDISP_H

#include <stdint.h>

/* the two display paths: the panel (OLED or LCD) and the HDMI bridge;
 * each has its own link (DSI) and display controller (IFTU) */
#define VD_PANEL 0
#define VD_HDMI  1

/* --- timings: the link and the controller are programmed from these --- */
typedef struct {
	uint16_t htotal, vtotal;     /* whole line / frame, in pixels / lines */
	uint16_t hfp, hsw, hbp;      /* front porch, sync, back porch */
	uint16_t vfp, vsw, vbp;
	uint32_t pixelclock;         /* link PLL setting, see vd_clk_link_pll */
} VdTiming;

/* the console's panel: 960x544 at 60 Hz. The two LCD variants (the
 * PCH-2000 panel wants the second) and the OLED's */
extern const VdTiming vd_timing_panel;     /* OLED, and LCD timing A */
extern const VdTiming vd_timing_panel_b;   /* LCD timing B */
extern const VdTiming vd_timing_hdmi_720p; /* 1280x720p 60 Hz (CEA VIC 4) */

/* --- clocks and resets (the "pervasive" block) --------------------------- */
uint32_t vd_soc_revision(void);
void vd_clk_i2c_on(int bus);
void vd_clk_link_pll(int path, uint32_t pixelclock);
void vd_clk_link_on(int path);
void vd_clk_link_off(int path);
void vd_clk_hdmi(int on);

/* --- the link (DSI) ------------------------------------------------------ */
/* the link up and sending the timing; stop undoes it */
void vd_link_start(int path, const VdTiming *t);
void vd_link_stop(int path);

/* --- the display controller (IFTU) ------------------------------------- */
#define VD_PLANE_A 0
#define VD_PLANE_B 1
#define VD_CFG_0 0
#define VD_CFG_1 1
/* a 960x544 A8B8G8R8 picture at paddr (rows of exactly 960 pixels),
 * placed at (dst_x, dst_y) in a dst_w x dst_h output; HDMI scales it up
 * (scale: the source step the system uses, 0 for 1:1) */
void vd_plane_picture(int path, int plane, int cfg, uint32_t paddr,
                      uint32_t dst_x, uint32_t dst_y, uint32_t dst_w,
                      uint32_t dst_h, uint32_t scale);
void vd_plane_enable(int path, int plane, int cfg, int on);
void vd_plane_colour(int path, int plane, int hdmi); /* alpha, colour matrices */
void vd_plane_select(int path, int plane, int cfg);
void vd_bus_enable(int path);

/* --- I2C bus 1: the LCD backlight controller and the HDMI bridge ------- */
void i2c_init_bus(int bus);
void i2c_transfer_write(int bus, uint8_t addr, const uint8_t *buf, int n);
void i2c_transfer_write_read(int bus, uint8_t waddr, const uint8_t *wbuf,
                             int wn, uint8_t raddr, uint8_t *rbuf, int rn);

/* --- the PCH-1000 OLED panel (SPI bus 2) ------------------------------- */
int oled_init(void);
void oled_off(void);

/* --- the PS TV's HDMI bridge (ADV7533 on I2C bus 1) -------------------- */
int hdmi_init(void);           /* bridge powered, addresses set */
int hdmi_get_hpd_state(void);  /* a TV is plugged in */
int hdmi_connect(void);        /* 720p60 out, HDCP started */
void hdmi_off(void);

#endif
