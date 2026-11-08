/* JamaisVu savestate plugin
 *
 * The PS TV's HDMI output: an Analog Devices ADV7533 DSI-to-HDMI bridge
 * on I2C bus 1, main map at 7-bit address 0x3D (0x7A written), its CEC
 * and DSI map programmed to 0x3E (0x7C). The register names follow the
 * ADV7533 programming guide; the values are what the PS TV's own system
 * programs, read back on hardware (the timing generator in particular:
 * 720p60 with htotal 1650, hsync 40, hfp 110, hbp 220, vtotal 750,
 * vsync 5, vfp 5, vbp 20). The bridge's hot-plug sensing is switched on
 * through the system controller (short command 0x886).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdint.h>
#include "vdisp.h"
#include "utils.h"
#include "../gpio.h"
#include "../syscon.h"

#define BUS       1
#define MAIN      0x7A
#define CEC       0x7C

/* main map */
#define M_COLOUR        0x18 /* bit 7: colour space converter */
#define M_PACKETS       0x40 /* bit 7: general control packet */
#define M_POWER         0x41 /* bit 6: power down */
#define M_STATUS        0x42 /* bit 6: hot-plug detected */
#define M_DEPTH         0x49
#define M_VIDEO_CODE    0x3C /* CEA video code sent to the TV */
#define M_OUTPUT_FMT    0x4C /* low 4 bits: output colour depth */
#define M_AVI1          0x56 /* AVI infoframe byte 2: aspect / active format */
#define M_INT1          0x97 /* bit 6: HDCP BKSV received */
#define M_HDCP_HDMI     0xAF
#define M_POWER2        0xD6 /* bits 7..6: hot-plug source */
#define M_CEC_ADDR      0xE1
#define M_V1P2          0xE4

/* CEC/DSI map */
#define C_HDMI_ENABLE   0x03
#define C_CLOCK_DIV     0x16
#define C_LANES         0x1C
#define C_TIMING_GEN    0x27 /* internal timing generator */
#define C_TEST_MODE     0x55

static void wr(uint8_t dev, uint8_t reg, uint8_t val) {
	uint8_t b[2] = { reg, val };
	i2c_transfer_write(BUS, dev, b, 2);
}

static uint8_t rd(uint8_t dev, uint8_t reg) {
	uint8_t v = 0;
	i2c_transfer_write_read(BUS, dev, &reg, 1, dev, &v, 1);
	return v;
}

static void mask(uint8_t dev, uint8_t reg, uint8_t clear, uint8_t set) {
	wr(dev, reg, (uint8_t)((rd(dev, reg) & ~clear) | set));
}

typedef struct { uint8_t reg, val; } RegVal;

/* the maker's required settings in the main map */
static const RegVal k_main_fixed[] = {
	{ 0x16, 0x20 }, { 0x9A, 0xE0 }, { 0xBA, 0x70 },
	{ 0xDE, 0x82 }, { 0xE4, 0x40 }, { 0xE5, 0x80 },
};

/* and in the CEC/DSI map */
static const RegVal k_cec_fixed[] = {
	{ 0x15, 0xD0 }, { 0x17, 0xD0 }, { 0x24, 0x20 }, { 0x57, 0x11 },
	{ 0x05, 0xC8 },
};

/* the timing generator, 1280x720p60: total, sync, front porch, back
 * porch, horizontal then vertical, each a 12-bit value over two bytes */
static const RegVal k_timing_720p[] = {
	{ 0x28, 0x67 }, { 0x29, 0x20 }, /* htotal 1650 */
	{ 0x2A, 0x02 }, { 0x2B, 0x80 }, /* hsync 40 */
	{ 0x2C, 0x06 }, { 0x2D, 0xE0 }, /* hfp 110 */
	{ 0x2E, 0x0D }, { 0x2F, 0xC0 }, /* hbp 220 */
	{ 0x30, 0x2E }, { 0x31, 0xE0 }, /* vtotal 750 */
	{ 0x32, 0x00 }, { 0x33, 0x50 }, /* vsync 5 */
	{ 0x34, 0x00 }, { 0x35, 0x50 }, /* vfp 5 */
	{ 0x36, 0x01 }, { 0x37, 0x40 }, /* vbp 20 */
};

static void write_all(uint8_t dev, const RegVal *t, unsigned n) {
	for (unsigned i = 0; i < n; i++)
		wr(dev, t[i].reg, t[i].val);
}

/* hot-plug sensing through the system controller */
static void hpd_sense(int on) {
	syscon_common_write((uint32_t)on, 0x886, 2);
}

int hdmi_init(void) {
	vd_clk_hdmi(1);
	hpd_sense(1);
	gpio_set_port_mode(0, GPIO_PORT_HDMI_BRIDGE, GPIO_PORT_MODE_OUTPUT);
	gpio_port_clear(0, GPIO_PORT_HDMI_BRIDGE);
	wr(MAIN, M_CEC_ADDR, CEC); /* where the second map answers */
	return 0;
}

int hdmi_get_hpd_state(void) {
	return (rd(MAIN, M_STATUS) & BIT(6)) != 0;
}

int hdmi_connect(void) {
	uint8_t status = 0;

	mask(MAIN, M_POWER, BIT(6), 0);           /* powered up */
	mask(MAIN, M_POWER2, 0xC0, BIT(6));       /* hot-plug from the HPD pin */
	write_all(MAIN, k_main_fixed, ARRAY_SIZE(k_main_fixed));
	write_all(CEC, k_cec_fixed, ARRAY_SIZE(k_cec_fixed));

	wr(CEC, C_LANES, 3 << 4);      /* 3 DSI lanes */
	wr(CEC, C_CLOCK_DIV, 4 << 3);  /* pixel clock divider for 3 lanes */

	/* the bridge regenerates the timing itself (with it off the picture
	 * came out as four squashed copies with every other line dark) */
	write_all(CEC, k_timing_720p, ARRAY_SIZE(k_timing_720p));
	wr(CEC, C_TIMING_GEN, 0xD0);

	/* RGB straight through: the display controller already sends RGB
	 * (with the converter on, red came out pink, green teal) */
	mask(MAIN, M_COLOUR, BIT(7), 0);
	wr(MAIN, M_VIDEO_CODE, 4);     /* CEA VIC 4: 720p60 */
	wr(MAIN, M_PACKETS, BIT(7));   /* general control packet on */
	wr(MAIN, M_DEPTH, 0);          /* 8 bits per colour */
	mask(MAIN, M_HDCP_HDMI, 0, BIT(1));   /* HDMI, not DVI */
	mask(MAIN, M_OUTPUT_FMT, 0x0F, 0x04); /* 24 bits per pixel */
	mask(MAIN, M_AVI1, 0x1F, 0x2A);       /* 16:9, centred */
	mask(MAIN, M_V1P2, 0, 0xC0);          /* internal 1.2 V on */
	mask(CEC, C_TEST_MODE, BIT(7), 0);    /* test pattern off */
	wr(CEC, C_HDMI_ENABLE, 0x89);         /* output on */
	wr(MAIN, M_HDCP_HDMI, 0x96);          /* HDCP on */

	/* the TV's HDCP key: bounded, a TV that never answers must not hang
	 * the payload (counted in the timeouts) */
	JV_WAIT_WHILE(!((status = rd(MAIN, M_INT1)) & BIT(6)));
	wr(MAIN, M_INT1, (uint8_t)((status & ~0xBFu) | BIT(6)));
	return 0;
}

/* the bridge powered down, hot-plug sensing and the clock off again: the
 * system sets them up itself when it wakes */
void hdmi_off(void) {
	mask(MAIN, M_POWER, 0, BIT(6));
	hpd_sense(0);
	vd_clk_hdmi(0);
}
