/* JamaisVu savestate plugin
 *
 * The PCH-1000's OLED panel. It is set up over SPI bus 2 (0xE0A20000,
 * per the Vita developer wiki's SPI Registers page: 0x00 the receive
 * FIFO, 0x04 the transmit FIFO, 0x08 control, 0x10 status/start, 0x28
 * and 0x2C the FIFO counts) with 9-bit frames: a data/command bit
 * followed by the byte, least significant bit first, packed into the
 * controller's 16-bit transmit words. The command sequences are the
 * panel's own, as the system's SceOled driver sends them (the wiki's
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
#include "../spi.h"
#include "../gpio.h"
#include "../pervasive.h"

#define SPI_BUS 2
#define SPI_REGS ((volatile uint32_t *)0xE0A20000u)

/* word offsets */
#define R_RX        0x0 /* receive FIFO */
#define R_TX        0x1 /* transmit FIFO */
#define R_CONTROL   0x2
#define R_STATUS    0x4 /* 1 written: transfer on; bit 0 reads back busy */
#define R_RX_COUNT  0xA
#define R_TX_COUNT  0xB
#define TX_FULL     0x7F

/* step of a panel script: a command with n data bytes, or a pause of n
 * ticks (one tick = the gap the system leaves between commands) */
#define CMD(c, n) (c), (n)
#define PAUSE(n)  0xFE, (n)
#define END       0xFF, 0

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

/* 9-bit frames packed into the 16-bit transmit words */
typedef struct {
	uint32_t bits;
	int n;
} Packer;

static void push_frame(Packer *p, uint32_t byte, int is_data) {
	p->bits |= ((bitrev8(byte) << 1) | (is_data ? 1u : 0u)) << p->n;
	p->n += 9;
	while (p->n > 15) {
		JV_WAIT_WHILE(SPI_REGS[R_TX_COUNT] == TX_FULL);
		SPI_REGS[R_TX] = p->bits;
		p->bits >>= 16;
		p->n -= 16;
	}
}

static void drain_rx(void) {
	for (int n = 0; SPI_REGS[R_RX_COUNT] && n < 4096; n++)
		(void)SPI_REGS[R_RX];
}

static void send(uint8_t cmd, const uint8_t *data, int n) {
	Packer p = { 0, 0 };

	pervasive_clock_enable_spi(SPI_BUS);
	drain_rx();
	SPI_REGS[R_CONTROL] = 0x30001;
	SPI_REGS[R_STATUS] = 1;

	push_frame(&p, cmd, 0);
	for (int i = 0; i < n; i++)
		push_frame(&p, data[i], 1);
	while (p.n > 0) /* padded out with empty command frames */
		push_frame(&p, 0, 0);

	JV_WAIT_WHILE(SPI_REGS[R_TX_COUNT]); /* sent */
	drain_rx();
	SPI_REGS[R_STATUS] = 0;
	JV_WAIT_WHILE(SPI_REGS[R_STATUS] & 1);
	SPI_REGS[R_CONTROL] = 0x30001;
	(void)SPI_REGS[R_CONTROL];
	dsb();
	pervasive_clock_disable_spi(SPI_BUS);
}

#define TICK 100 /* delay units between commands */

static void run(const uint8_t *s) {
	for (;;) {
		uint8_t c = s[0], n = s[1];
		if (c == 0xFF)
			return;
		if (c == 0xFE) {
			delay(TICK * n);
			s += 2;
			continue;
		}
		send(c, s + 2, n);
		delay(TICK);
		s += 2 + n;
	}
}

int oled_init(void) {
	spi_init(SPI_BUS);
	pervasive_clock_disable_spi(SPI_BUS); /* clocked only while sending */
	/* the panel's enable line */
	gpio_set_port_mode(0, GPIO_PORT_OLED_LCD, GPIO_PORT_MODE_OUTPUT);
	gpio_port_set(0, GPIO_PORT_OLED_LCD);

	run(k_setup_1);
	run(k_setup_2);
	run(k_on);
	run(k_colour_and_brightness);
	return 0;
}

/* display off and sleep, then the enable line low: as before oled_init */
void oled_off(void) {
	run(k_off);
	gpio_port_clear(0, GPIO_PORT_OLED_LCD);
}
