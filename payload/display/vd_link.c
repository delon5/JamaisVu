/* JamaisVu savestate plugin
 *
 * The display link (DSI) at 0xE5050000 (panel) and 0xE5060000 (HDMI
 * bridge): an array of 32-bit registers (word offsets here; the wiki's
 * DSI Registers page lists byte offsets). Words 0x00..0x1C hold the
 * frame timing the link generates, 0x140 (0x500) is the command FIFO the
 * link's own sequencer runs (one program per output: it clocks out the
 * sync, blank and pixel packets of every line), 0x145/0x147 (0x514,
 * 0x51C: transfer control) clear and start it, 0x204..0x20F (0x810..)
 * hold the PHY settings, 0x20E (0x838) selects "HS clock only", and
 * 0x240..0x253 (0x900.., 0x940..) the lane controls. Per that page and
 * the system's own setup read back on hardware (the three timings below
 * are the ones it uses).
 *
 * Written for the two outputs this payload shows, both progressive at 24
 * bits per pixel: the console's panel (2 lanes) and 720p60 over HDMI
 * (3 lanes).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdint.h>
#include "vdisp.h"
#include "utils.h"

void vd_clk_link_misc(int path, int on); /* vd_clocks.c */

#define DSI_BASE(path) (0xE5050000u + (uint32_t)(path) * 0x10000u)

/* 960x544 at 60 Hz: the panel's own timing (OLED, LCD timing A) and the
 * LCD's second one (the PCH-2000 wants it); 1280x720p60 (CEA VIC 4) */
const VdTiming vd_timing_panel     = { 1050, 594,  20,  66,   4, 4, 4, 42, 2243100 };
const VdTiming vd_timing_panel_b   = { 1375, 567,  20, 391,   4, 4, 4, 15, 2243100 };
const VdTiming vd_timing_hdmi_720p = { 1650, 750, 110,  40, 220, 5, 5, 20, 2967033 };

/* PHY settings, words 0x204..0x20C; the panel's differ between the two
 * SoC revisions (misc revision word, bits 8..16 above 0xFF = the later) */
static const uint32_t k_phy_panel_early[9] = { 0x2BCF, 6, 0x0F05, 1, 0x806, 0x4906, 0x16, 3, 0x20 };
static const uint32_t k_phy_panel_late[9]  = { 0x2BCF, 6, 0x1007, 1, 0x808, 0x4906, 0x16, 3, 0x20 };
static const uint32_t k_phy_hdmi[9]        = { 0x39F5, 7, 0x1507, 5, 0x507, 0x4871, 0x0B, 0, 0x20 };

static int late_soc(void) {
	return (vd_soc_revision() & 0x1FF00) > 0xFF;
}

static uint32_t hactive(const VdTiming *t) {
	return t->htotal - t->hfp - t->hsw - t->hbp;
}

/* The sequencer's program for the panel link. Each word is one step:
 * 0x28.. waits, 0x10.. sends a short packet, 0x40.. a long one with its
 * byte count in the low bits, 0x04.. repeats the following block for the
 * number of lines in the low half-word, 0x0C000000 ends the program.
 * Line lengths are in bytes: pixels x 3 (24 bpp). */
static int program_panel(const VdTiming *t, uint32_t *w) {
	uint32_t hact = hactive(t);
	int n = 0;
	w[n++] = 0x0407FFFF;
	w[n++] = 0xA3000082;
	w[n++] = 0x28000004;
	w[n++] = 0x10000001;
	w[n++] = 0x400B4019;
	w[n++] = 0x04020000 | ((t->vsw + t->vbp - 2u) & 0xFFFF);  /* sync + back porch lines */
	w[n++] = 0x28000001;
	w[n++] = 0x10000021;
	w[n++] = 0x400B4019;
	w[n++] = 0x04030000 | ((t->vtotal - 1u - t->vsw - t->vbp - t->vfp) & 0xFFFF); /* active lines */
	w[n++] = 0x28000001;
	w[n++] = 0x10000021;
	w[n++] = 0x40000019 | ((t->hbp * 3u * 256u - 0xA00u) & 0xFFFFFF);
	w[n++] = 0x4000003E | ((hact * 3u * 256u) & 0xFFFFFF);
	w[n++] = 0x04020000 | ((t->vfp - 1u) & 0xFFFF);          /* front porch lines */
	w[n++] = 0x28000001;
	w[n++] = 0x10000021;
	w[n++] = 0x400B4019;
	w[n++] = 0x0C000000;
	return n;
}

/* The same for the HDMI bridge link (3 lanes, no blanking packets with
 * payload: the bridge regenerates the timing itself) */
static int program_hdmi(const VdTiming *t, uint32_t *w) {
	uint32_t hact = hactive(t);
	uint32_t vact = t->vtotal - t->vfp - t->vsw - t->vbp;
	int n = 0;
	w[n++] = 0x040DFFFF;
	w[n++] = 0xA30000A4;
	w[n++] = 0x28000004;
	w[n++] = 0x10000001;
	w[n++] = 0x28000002;
	w[n++] = 0x10000031;
	w[n++] = 0x04030000 | ((t->vsw - 2u) & 0xFFFF);   /* sync lines */
	w[n++] = 0x28000001;
	w[n++] = 0x10000021;
	w[n++] = 0x28000002;
	w[n++] = 0x10000031;
	w[n++] = 0x28000008;
	w[n++] = 0x10000011;
	w[n++] = 0x28000002;
	w[n++] = 0x10000031;
	w[n++] = 0x04030000 | ((t->vbp - 2u) & 0xFFFF);   /* back porch lines */
	w[n++] = 0x28000001;
	w[n++] = 0x10000021;
	w[n++] = 0x28000002;
	w[n++] = 0x10000031;
	w[n++] = 0x04050000 | ((vact - 1u) & 0xFFFF);     /* active lines */
	w[n++] = 0x28000001;
	w[n++] = 0x10000021;
	w[n++] = 0x28000002;
	w[n++] = 0x10000031;
	w[n++] = 0x28000010;
	w[n++] = 0x4000003E | ((hact * 3u * 256u) & 0xFFFFFF);
	w[n++] = 0x04030000 | ((t->vfp - 1u) & 0xFFFF);   /* front porch lines */
	w[n++] = 0x28000001;
	w[n++] = 0x10000021;
	w[n++] = 0x28000002;
	w[n++] = 0x10000031;
	w[n++] = 0x0C000000;
	return n;
}

/* the frame timing the link generates: words 0x01..0x1C, all in link
 * clocks (clocks per pixel: 6 on the 2-lane panel link, 4 on the 3-lane
 * HDMI link at 24 bpp); vertical values in lines */
static void timing_words(volatile uint32_t *d, int path, const VdTiming *t) {
	uint32_t cpp = path ? 4 : 6;
	uint32_t hsync_end = t->hbp + t->hsw;
	uint32_t hact = hactive(t);
	uint32_t hfp_start = hact + hsync_end;
	uint32_t htotal_clk = (t->htotal * cpp) >> 1;
	uint32_t vact_start = t->vbp + t->vsw;
	uint32_t half = (t->vtotal + 1u) >> 1;

	d[0x01] = 0; /* progressive */
	d[0x02] = htotal_clk;
	d[0x03] = t->vtotal;
	d[0x04] = 0x10001;
	d[0x05] = (((hsync_end * cpp) >> 1) << 16) |
	          ((((hfp_start * cpp) >> 1) + 1u) & 0xFFFF);
	d[0x07] = ((vact_start << 16) & 0x1FFF0000) |
	          ((t->vtotal + 1u - t->vfp) & 0x1FFF);
	d[0x09] = (((t->hsw * cpp) >> 1) << 16) | 1;
	d[0x0B] = 0x10001;
	d[0x0C] = (((uint32_t)t->vsw << 16) & 0x1FFF0000) | htotal_clk;
	d[0x0F] = 1;
	d[0x10] = (((half + vact_start - 8u) << 16) & 0x1FFF0000) |
	          ((vact_start - 8u) & 0x1FFF);
	d[0x17] = 0x10001;
	d[0x18] = ((vact_start << 16) & 0x1FFF0000) | 1;
	d[0x1B] = (((vact_start + (t->vtotal >> 1)) << 16) & 0x1FFF0000) |
	          0x40000001;
	d[0x1C] = 0;
}

void vd_link_start(int path, const VdTiming *t) {
	volatile uint32_t *d = (volatile uint32_t *)DSI_BASE(path);
	int lanes = path ? 3 : 2;
	const uint32_t *phy = path ? k_phy_hdmi
	                           : late_soc() ? k_phy_panel_late : k_phy_panel_early;
	uint32_t program[40];
	int n, i;

	vd_clk_link_misc(path, 1);
	d[0x15] = 0;  /* interrupts off */
	d[0x146] = 1; /* sequencer stopped */

	/* the lanes: the later SoC wants mode 2 in each lane's control word;
	 * every lane's drive setting */
	if (late_soc())
		for (i = 0; i <= lanes; i++)
			d[0x240 + i] = (d[0x240 + i] & ~3u) | 2u;
	for (i = 0; i <= lanes; i++)
		d[0x250 + i] = 0x200;

	for (i = 0; i < 9; i++)
		d[0x204 + i] = phy[i];
	d[0x20F] = phy[1] | (phy[1] << 16);

	d[0x20D] = path ? 0xF : 7; /* lanes enabled: clock + data */
	d[0x20E] = 0;
	d[0x201] = 1;
	d[0x145] = 0xFFFFFFFF; /* sequencer state cleared */

	n = path ? program_hdmi(t, program) : program_panel(t, program);
	for (i = 0; i < n; i++)
		d[0x140] = program[i];
	d[0x147] = 1; /* sequencer running */

	timing_words(d, path, t);
	d[0x14] = d[0x14]; /* pending interrupts acknowledged */
	d[0x15] = 2;
	d[0x20E] = path ? 1 : 0;
	d[0x142] = 0xFFFFFFFF;
	d[0x00] = 1; /* the link's frame generator on */
	dmb();
}

void vd_link_stop(int path) {
	volatile uint32_t *d = (volatile uint32_t *)DSI_BASE(path);
	int lanes = path ? 3 : 2;

	/* the lanes' idle state: never confirmed on the PCH-2000 (every run
	 * timed out here), so ~10 ms at most and not counted as a timeout */
	JV_WAIT_WHILE_QUIET_N((d[0x12] & 0xF) != 0, 20000);

	d[0x145] = 0xFFFFFFFF;
	for (int i = 0; i <= lanes; i++)
		d[0x250 + i] = 1;
	d[0x20D] = 0;
	d[0x201] = 1;
	dsb();

	vd_clk_link_misc(path, 0);
}
