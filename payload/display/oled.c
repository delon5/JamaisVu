/* JamaisVu savestate plugin
 *
 * The PCH-1000's OLED panel, set up over the panel SPI channel
 * (panel_spi.c). The command sequences are the panel's own, as the
 * system's SceOled driver sends them (the Vita developer wiki's
 * SceOled page names them: PASSWD1/2 unlock the maker's registers,
 * GLOBAL_PARAMETER, MAIN_POWER_SEQUENCE, SET_POWER_CONTROL_1/2,
 * SET_DISPLAY_1/5, GTCON, TEMP_SWIRE, ELVSS_CON, SET_PANEL_CONTROL,
 * SCR_CONTROL, SET_NORMAL_GAMMA_CONTROL, SET_GAMMA_CURVE), plus the
 * standard sleep-out/display-on and display-off/sleep-in commands. The
 * panel's enable line is GPIO bus 0 port 0.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdint.h>
#include "vdisp.h"
#include "utils.h"
#include "panel_spi.h"
#include "../gpio.h"

static const uint8_t k_setup_1[] = {
	CMD(0xF0, 2), 0x5A, 0x5A,       /* PASSWD1, PASSWD2: unlocked */
	CMD(0xF1, 2), 0x5A, 0x5A,
	CMD(0xB0, 1), 0x02,             /* GLOBAL_PARAMETER */
	CMD(0xF3, 1), 0x3B,             /* MAIN_POWER_SEQUENCE */
	CMD(0xF4, 4), 0x33, 0x42, 0x00, 0x08, /* SET_POWER_CONTROL_1 */
	CMD(0xF5, 5), 0x00, 0x06, 0x26, 0x35, 0x03, /* SET_POWER_CONTROL_2 */
	CMD(0xF6, 1), 0x02,             /* SET_DISPLAY_5 */
	CMD(0xC6, 10), 0x0B, 0x00, 0x00, 0x3C, 0x00, 0x22, 0x00, 0x00, 0x00, 0x00, /* SET_DISPLAY_1 */
	CMD(0xF7, 1), 0x20,             /* GTCON */
	END
};

static const uint8_t k_setup_2[] = {
	CMD(0xF5, 5), 0x00, 0x06, 0x27, 0x35, 0x03, /* SET_POWER_CONTROL_2 */
	CMD(0xB2, 4), 0x06, 0x06, 0x06, 0x06, /* TEMP_SWIRE */
	CMD(0xB1, 3), 0x07, 0x00, 0x10,       /* ELVSS_CON */
	CMD(0xF8, 19), 0x7F, 0x7A, 0x89, 0x67, 0x26, 0x38, 0x00, 0x00, 0x09, /* SET_PANEL_CONTROL */
	               0x67, 0x70, 0x88, 0x7A, 0x76, 0x05, 0x09, 0x23, 0x23,
	               0x23,
	END
};

static const uint8_t k_on[] = {
	PAUSE(4),
	CMD(0x11, 0),  /* sleep out */
	PAUSE(13),
	CMD(0x29, 0),  /* display on */
	PAUSE(1),
	END
};

static const uint8_t k_off[] = {
	CMD(0x28, 0),  /* display off */
	PAUSE(1),
	CMD(0x10, 0),  /* sleep in */
	PAUSE(6),
	END
};

static const uint8_t k_colour_and_brightness[] = {
	CMD(0xB3, 1), 0x00, /* SCR_CONTROL: colour mode 0 */
	CMD(0xF9, 22), 0x01, 0x79, 0x78, 0x8D, 0xD9, 0xDF, 0xD5, 0xCB, 0xCF, /* SET_NORMAL_GAMMA_CONTROL */
	               0xC5, 0xE5, 0xE0, 0xE4, 0xDC, 0xB8, 0xD4, 0xFA, 0xED,
	               0xE6, 0x2F, 0x00, 0x2F,
	CMD(0xF9, 1), 0x00,
	CMD(0x26, 1), 0x00, /* SET_GAMMA_CURVE */
	CMD(0xB2, 1), 0x15, /* TEMP_SWIRE: brightness */
	END
};

int oled_init(void) {
	panel_spi_init(PANEL_OLED);
	/* the panel's enable line */
	gpio_set_port_mode(0, GPIO_PORT_OLED_LCD, GPIO_PORT_MODE_OUTPUT);
	gpio_port_set(0, GPIO_PORT_OLED_LCD);

	panel_run(k_setup_1);
	panel_run(k_setup_2);
	panel_run(k_on);
	panel_run(k_colour_and_brightness);
	return 0;
}

/* display off and sleep, then the enable line low: as before oled_init */
void oled_off(void) {
	panel_run(k_off);
	gpio_port_clear(0, GPIO_PORT_OLED_LCD);
}
