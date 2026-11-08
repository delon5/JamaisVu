/* JamaisVu savestate plugin
 *
 * Clocks and resets for the display path: the "pervasive" block at
 * 0xE3100000 (misc), 0xE3101000 (resets), 0xE3102000 (clock gates) and
 * 0xE3103000 (base clocks), per the Vita developer wiki's Pervasive page.
 * Each block is an array of 32-bit registers; a gate or reset is one bit
 * per device in the register at the device's offset.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdint.h>
#include "vdisp.h"
#include "utils.h"

#define PV_MISC    0xE3100000u
#define PV_RESET   0xE3101000u
#define PV_GATE    0xE3102000u
#define PV_BASECLK 0xE3103000u

/* device offsets within the gate and reset blocks */
#define PV_DEV_DSI(path) (0x80u + 4u * (path))
#define PV_DEV_I2C(bus)  (0x110u + 4u * (bus))

/* a read-modify-write that is complete before the next access */
static void pv_bits(uintptr_t addr, uint32_t bits, int set) {
	uint32_t v = read32(addr);
	v = set ? (v | bits) : (v & ~bits);
	write32(v, addr);
	dmb();
	(void)read32(addr);
	dsb();
}

uint32_t vd_soc_revision(void) {
	return read32(PV_MISC);
}

void vd_clk_i2c_on(int bus) {
	pv_bits(PV_GATE + PV_DEV_I2C(bus), 1, 1);  /* clock on */
	pv_bits(PV_RESET + PV_DEV_I2C(bus), 1, 0); /* out of reset */
}

/* the link's four clocks on and its three resets released */
void vd_clk_link_on(int path) {
	pv_bits(PV_GATE + PV_DEV_DSI(path), 0xF, 1);
	pv_bits(PV_RESET + PV_DEV_DSI(path), 7, 0);
}

void vd_clk_link_off(int path) {
	pv_bits(PV_RESET + PV_DEV_DSI(path), 7, 1);
	pv_bits(PV_GATE + PV_DEV_DSI(path), 0xF, 0);
}

/* the link's own power/enable word in the misc block (0 = on) */
void vd_clk_link_misc(int path, int on) {
	write32(on ? 0 : 1, PV_MISC + (path ? 0x148u : 0x140u));
	dmb();
}

void vd_clk_hdmi(int on) {
	write32(on ? 1 : 0, PV_BASECLK + 0x1D0);
	dsb();
}

/* The link's pixel clock PLL: one register block per path in the base
 * clock area (0x180 for the panel, 0x100 for HDMI). Only the two rates
 * this payload shows are known: the panel's and 720p60's. */
typedef struct {
	uint32_t pixelclock;
	uint32_t mode;    /* word 9: bit 4 = the PLL words below apply */
	uint32_t divider; /* word 8 */
	uint32_t pll[3];  /* words 0xC..0xE */
	uint32_t post[3]; /* words 0x10..0x12 */
} VdPll;

static const VdPll k_pll[] = {
	{ 2243100, 0x10, 2, { 0x1A, 0xC01, 0x08100301 }, { 0x1001F, 0, 0x04180301 } },
	{ 2967033, 0x10, 0, { 0x31, 0xC00, 0x04180301 }, { 0x6004F, 0, 0x08100301 } },
};

void vd_clk_link_pll(int path, uint32_t pixelclock) {
	volatile uint32_t *r = (volatile uint32_t *)(PV_BASECLK + 0x180u - (uint32_t)path * 0x80u);
	const VdPll *p = 0;
	for (unsigned i = 0; i < ARRAY_SIZE(k_pll); i++)
		if (k_pll[i].pixelclock == pixelclock)
			p = &k_pll[i];
	if (!p)
		return;

	r[2] = 1; /* output held */
	r[1] = 1; /* divider held */
	(void)r[1];
	dsb();

	r[9] = p->mode;
	r[0] = 1; /* PLL held while programmed */
	(void)r[0];
	dsb();
	if (p->mode & 0x10) {
		r[0xC] = p->pll[0];
		r[0xD] = p->pll[1];
		r[0xE] = p->pll[2];
		r[0] = 0; /* PLL running */
		(void)r[0];
		dsb();
		delay(1000); /* lock */
	}

	r[0x10] = p->post[0];
	r[0x11] = p->post[1];
	r[0x12] = p->post[2];
	r[8] = p->divider;
	r[1] = 0;
	(void)r[1];
	dsb();
	delay(1000);

	r[2] = 0; /* output on */
	(void)r[2];
	dsb();
}
