/* JamaisVu savestate plugin
 *
 * The I2C controllers at 0xE0500000 (bus 0) and 0xE0510000 (bus 1), per
 * the Vita developer wiki's I2C Registers page (byte offsets): 0x00 the
 * write FIFO, 0x04 the read FIFO, 0x08/0x0C set to 1 before a transfer,
 * 0x10 the 7-bit device address, 0x14 the transfer flags (the byte count
 * in bits 8.., the operation in the low bits), 0x18 the speed, 0x1C busy,
 * 0x28 the interrupt status (bit 15: the device did not acknowledge;
 * written back to clear), 0x2C the interrupt control. Bus 1 carries the
 * PS TV's HDMI bridge (0x7A/0x7C) and the LCD's brightness controller.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdint.h>
#include "vdisp.h"
#include "utils.h"

#define I2C_BASE(bus) ((bus) == 0 ? 0xE0500000u : 0xE0510000u)

/* word offsets */
#define R_TX       0x00 /* write FIFO */
#define R_RX       0x01 /* read FIFO */
#define R_PREP_A   0x02 /* 0x08: 1 before a transfer */
#define R_PREP_B   0x03 /* 0x0C */
#define R_ADDR     0x04 /* 0x10: 7-bit device address */
#define R_CMD      0x05 /* 0x14: transfer flags */
#define R_SPEED    0x06 /* 0x18 */
#define R_BUSY     0x07 /* 0x1C */
#define R_IRQ      0x0A /* 0x28: interrupt status */
#define R_IRQ_CTL  0x0B /* 0x2C: interrupt control */

#define CMD_WRITE      0x02 /* start, send the FIFO */
#define CMD_STOP       0x04
#define CMD_RESTART    0x05 /* repeated start */
#define CMD_RESET      0x07 /* bus reset */
#define CMD_READ       0x13 /* receive into the FIFO */
#define CMD_COUNT(n)   ((uint32_t)(n) << 8)

static void wait_idle(volatile uint32_t *r) {
	JV_WAIT_WHILE(r[R_BUSY]);
}

static void start(volatile uint32_t *r, uint8_t addr) {
	r[R_PREP_A] = 1;
	r[R_PREP_B] = 1;
	r[R_ADDR] = addr >> 1; /* 8-bit write address -> 7-bit */
}

void i2c_init_bus(int bus) {
	volatile uint32_t *r = (volatile uint32_t *)I2C_BASE(bus);
	r[R_IRQ_CTL] = 0x100F70F;
	r[R_PREP_A] = 1;
	r[R_PREP_B] = 1;
	r[R_CMD] = CMD_RESET;
	dsb();
	wait_idle(r);
	r[R_IRQ] = r[R_IRQ];
	r[R_IRQ_CTL] = 0x1000000;
	r[R_SPEED] = 4;
}

void i2c_transfer_write(int bus, uint8_t addr, const uint8_t *buf, int n) {
	volatile uint32_t *r = (volatile uint32_t *)I2C_BASE(bus);
	start(r, addr);
	for (int i = 0; i < n; i++)
		r[R_TX] = buf[i];
	r[R_CMD] = CMD_COUNT(n) | CMD_WRITE;
	wait_idle(r);
	r[R_CMD] = CMD_STOP;
	wait_idle(r);
}

void i2c_transfer_write_read(int bus, uint8_t waddr, const uint8_t *wbuf,
                             int wn, uint8_t raddr, uint8_t *rbuf, int rn) {
	volatile uint32_t *r = (volatile uint32_t *)I2C_BASE(bus);
	(void)raddr; /* the same device: a repeated start keeps the address */
	start(r, waddr);
	for (int i = 0; i < wn; i++)
		r[R_TX] = wbuf[i];
	r[R_CMD] = CMD_COUNT(wn) | CMD_WRITE;
	wait_idle(r);
	r[R_CMD] = CMD_RESTART;
	wait_idle(r);
	r[R_CMD] = CMD_COUNT(rn) | CMD_READ;
	wait_idle(r);
	for (int i = 0; i < rn; i++)
		rbuf[i] = (uint8_t)r[R_RX];
	r[R_CMD] = CMD_STOP;
	wait_idle(r);
}
