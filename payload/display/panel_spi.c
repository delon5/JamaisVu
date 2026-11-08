/* JamaisVu savestate plugin
 *
 * The panels' command channel: SPI bus 2 (0xE0A20000, per the Vita
 * developer wiki's SPI Registers page: 0x00 the receive FIFO, 0x04 the
 * transmit FIFO, 0x08 control, 0x10 status/start, 0x28 and 0x2C the FIFO
 * counts) with 9-bit frames: a data/command bit after the byte sent
 * least significant bit first, packed into the controller's 16-bit
 * transmit words. The system's SceOled and SceLcd drivers end a
 * transfer slightly differently from each other; both ways are kept.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdint.h>
#include "panel_spi.h"
#include "utils.h"
#include "../spi.h"
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

static int g_kind;

void panel_spi_init(int kind) {
	g_kind = kind;
	spi_init(SPI_BUS);
	pervasive_clock_disable_spi(SPI_BUS);
}

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

void panel_send(uint8_t cmd, const uint8_t *data, int n) {
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

	if (g_kind == PANEL_LCD)
		SPI_REGS[R_STATUS] = 1;
	JV_WAIT_WHILE(SPI_REGS[R_TX_COUNT]); /* sent */
	if (g_kind == PANEL_OLED)
		drain_rx();
	SPI_REGS[R_STATUS] = 0;
	JV_WAIT_WHILE(SPI_REGS[R_STATUS] & 1);
	if (g_kind == PANEL_OLED) {
		SPI_REGS[R_CONTROL] = 0x30001;
		(void)SPI_REGS[R_CONTROL];
	}
	dsb();
	pervasive_clock_disable_spi(SPI_BUS);
}

/* The answer sits in the receive FIFO as 16-bit words; the first
 * header_bits are the echo of the command frame, then come the bytes,
 * each least significant bit first. */
void panel_read(uint8_t *buf, int n, int header_bits) {
	uint32_t acc = 0;
	int bits = 0, got = 0;

	pervasive_clock_enable_spi(SPI_BUS);
	while (got < n) {
		JV_WAIT_WHILE(SPI_REGS[R_RX_COUNT] == 0);
		acc |= SPI_REGS[R_RX] << bits;
		bits += 16;
		if (header_bits) {
			bits -= header_bits;
			acc >>= header_bits;
			header_bits = 0;
			if (bits == 7)
				continue;
		}
		while (bits >= 8 && got < n) {
			buf[got++] = (uint8_t)bitrev8(acc & 0xFF);
			acc >>= 8;
			bits -= 8;
		}
	}
	pervasive_clock_disable_spi(SPI_BUS);
}

#define TICK 100 /* delay units between commands */

void panel_run(const uint8_t *s) {
	for (;;) {
		uint8_t c = s[0], n = s[1];
		if (c == 0xFF)
			return;
		if (c == 0xFE) {
			delay(TICK * n);
			s += 2;
			continue;
		}
		panel_send(c, s + 2, n);
		delay(TICK);
		s += 2 + n;
	}
}
