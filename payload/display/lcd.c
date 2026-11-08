/* JamaisVu savestate plugin
 *
 * The PCH-2000's LCD panel and its backlight. The panel takes standard
 * MIPI DCS commands over the panel SPI channel (panel_spi.c): sleep
 * out/in (0x11/0x10), display on/off (0x29/0x28), and the panel maker's
 * registers behind the access-protect command 0xB0. Which display-on
 * sequence it wants depends on the panel's own identification, read
 * with read_DDB_start (0xA1; the system's SceLcd driver does the same,
 * see the Vita developer wiki). The backlight is a controller on I2C
 * bus 1 at 0xC8 whose setup registers differ with the console's
 * hardware revision (the kernel's hardware info) and audio codec. The
 * panel's power lines are GPIO bus 0 port 0 and bus 1 port 7.
 *
 * The register values are the ones the system programs; the bounded
 * waits are JamaisVu's.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdint.h>
#include "vdisp.h"
#include "utils.h"
#include "panel_spi.h"
#include "../gpio.h"

#define BL_ADDR 0xC8 /* the backlight controller, I2C bus 1 */
#define PANEL_POWER_PORT 7 /* GPIO bus 1 */

/* the three display-on sequences, chosen by the panel's "supplier
 * elective data" from its DDB: below 0x25, below 0x32, from 0x32 */
static const uint8_t k_on_a[] = {
	PAUSE(4),
	CMD(0x11, 0), /* sleep out */
	PAUSE(13),
	CMD(0xB0, 1), 0x00,
	CMD(0xC0, 7), 0x03, 0x1F, 0x02, 0x23, 0x02, 0x07, 0x07,
	CMD(0xB0, 1), 0x03,
	CMD(0x29, 0), /* display on */
	PAUSE(1),
	END
};

static const uint8_t k_on_b[] = {
	PAUSE(4),
	CMD(0x11, 0),
	PAUSE(13),
	CMD(0xB0, 1), 0x00,
	CMD(0xC0, 7), 0x03, 0x21, 0x02, 0x23, 0x02, 0x07, 0x07,
	CMD(0xB0, 1), 0x03,
	CMD(0x29, 0),
	PAUSE(1),
	END
};

static const uint8_t k_on_c[] = {
	PAUSE(4),
	CMD(0x11, 0),
	PAUSE(13),
	CMD(0xB0, 1), 0x00,
	CMD(0xBB, 3), 0x08, 0xFF, 0x01,
	CMD(0xB0, 1), 0x03,
	PAUSE(4),
	CMD(0x29, 0),
	PAUSE(1),
	END
};

static const uint8_t k_off[] = {
	CMD(0x28, 0), /* display off */
	PAUSE(8),
	CMD(0x10, 0), /* sleep in */
	PAUSE(6),
	END
};

/* colour mode 0, and the maker's register 0xB8 as the system sets it */
static const uint8_t k_setup[] = {
	CMD(0xB0, 1), 0x04,
	CMD(0xC9, 1), 0x00,
	CMD(0xB0, 1), 0x03,
	CMD(0xB0, 1), 0x00,
	CMD(0xB8, 5), 0x00, 0x1A, 0x18, 0x02, 0x40,
	CMD(0xB0, 1), 0x03,
	END
};

/* the backlight controller's setup: register, value pairs */
typedef struct { uint8_t reg, val; } BlReg;

static const BlReg k_bl_rev_a[] = { /* hardware info >= 0x901000 */
	{ 0x01, 0x01 }, { 0x03, 0x05 }, { 0x05, 0xFF }, { 0x07, 0x72 },
};
static const BlReg k_bl_rev_b[] = { /* 0x804000, and 0x804400..0x900FFF */
	{ 0x01, 0x11 }, { 0x03, 0x01 }, { 0x05, 0xFF }, { 0x07, 0x72 },
};
static const BlReg k_bl_conexant[] = { /* consoles with the Conexant audio codec */
	{ 0x01, 0x11 }, { 0x03, 0x01 }, { 0x04, 0x00 }, { 0x05, 0xFF },
	{ 0x06, 0x06 }, { 0x07, 0x72 },
};
static const BlReg k_bl_other[] = { /* below 0x804000, or 0x804100..0x8043FF */
	{ 0x01, 0x11 }, { 0x03, 0x05 }, { 0x05, 0xFF }, { 0x07, 0x72 },
};

/* the system's brightness curve: PWM for 17 steps across its range */
static const uint8_t k_pwm[17] = {
	0x1F, 0x25, 0x2B, 0x32, 0x3A, 0x43, 0x4D, 0x58, 0x64, 0x72, 0x81,
	0x93, 0xA6, 0xB6, 0xCB, 0xE3, 0xFF,
};

static void bl_write(uint8_t reg, uint8_t val) {
	uint8_t b[2] = { reg, val };
	i2c_transfer_write(1, BL_ADDR, b, 2);
}

static void bl_setup(uint32_t hw_info, int codec_conexant) {
	const BlReg *t;
	unsigned n;
	uint32_t rev = hw_info & 0xFFFF00;

	if (codec_conexant) {
		t = k_bl_conexant; n = ARRAY_SIZE(k_bl_conexant);
	} else if (rev == 0x804000 || rev > 0x8043FF) {
		if (rev < 0x901000) {
			t = k_bl_rev_b; n = ARRAY_SIZE(k_bl_rev_b);
		} else {
			t = k_bl_rev_a; n = ARRAY_SIZE(k_bl_rev_a);
		}
	} else {
		t = k_bl_other; n = ARRAY_SIZE(k_bl_other);
	}
	for (unsigned i = 0; i < n; i++)
		bl_write(t[i].reg, t[i].val);
}

/* brightness 0..65535 as the system's API takes it */
static void bl_brightness(uint32_t brightness) {
	uint32_t pwm = brightness < 2 ? 0x19 : k_pwm[(brightness - 2) * 16 / 65534];
	bl_write(0x0E, 1);   /* backlight on */
	bl_write(0x05, pwm); /* PWM */
}

static void bl_off(void) {
	bl_write(0x05, 0);
	bl_write(0x0E, 0);
}

/* the panel's Device Descriptor Block: supplier id, then the supplier's
 * elective data (which the display-on sequence depends on) */
static uint16_t read_ddb_elective(void) {
	static const uint8_t zero[6];
	uint8_t ddb[6];
	panel_send(0xA1, zero, 6); /* read_DDB_start, 6 bytes clocked out */
	panel_read(ddb, 6, 9);
	return (uint16_t)(ddb[2] | (ddb[3] << 8));
}

int lcd_init(uint32_t hw_info, int codec_conexant) {
	uint16_t elective;

	panel_spi_init(PANEL_LCD);
	gpio_set_port_mode(0, GPIO_PORT_OLED_LCD, GPIO_PORT_MODE_OUTPUT);
	gpio_set_port_mode(1, PANEL_POWER_PORT, GPIO_PORT_MODE_OUTPUT);
	gpio_port_set(0, GPIO_PORT_OLED_LCD);
	gpio_port_set(1, PANEL_POWER_PORT);

	elective = read_ddb_elective();
	panel_run(elective < 0x25 ? k_on_a : elective < 0x32 ? k_on_b : k_on_c);
	panel_run(k_setup);

	bl_setup(hw_info, codec_conexant);
	bl_brightness(65536 / 2);
	return 0;
}

/* backlight off first (the panel's display-off alone leaves it lit over
 * an undefined picture: a broken-looking moment on hardware), then the
 * panel to sleep and its power lines low, as before lcd_init */
void lcd_off(void) {
	bl_off();
	delay(2000);
	panel_run(k_off);
	gpio_port_clear(1, PANEL_POWER_PORT);
	gpio_port_clear(0, GPIO_PORT_OLED_LCD);
}
