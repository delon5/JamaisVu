/* JamaisVu savestate plugin
 *
 * The display controller (IFTU) that reads a picture from memory and
 * feeds it to a link: one per path, at 0xE5020000 (panel) and 0xE5030000
 * (HDMI). Each has two planes (0x1000 apart), each plane two
 * configurations (0x200 and 0x300 into the plane) of which one is
 * selected at a time, and a control block 0x2000 into the controller.
 * Per the Vita developer wiki's IFTU Registers page (which names them:
 * the control block's enable, plane control (bit 0: alpha blending),
 * config selectors and blending words; the plane's alpha value, the
 * "alpha blending off" bit, the colour converter's enable, adds and
 * S3.9 matrices; the configuration's source address, format, size,
 * disable bit, extra bytes per source row, destination format, size,
 * inverse scale (16.16) and position) and the system's own setup read
 * back on hardware.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdint.h>
#include "vdisp.h"
#include "utils.h"

#define IFTU_PLANE(path, plane) \
	(0xE5020000u + (uint32_t)(path) * 0x10000u + (uint32_t)(plane) * 0x1000u)
#define IFTU_CONTROL(path) (0xE5022000u + (uint32_t)(path) * 0x10000u)
#define IFTU_CONFIG(path, plane, cfg) \
	(IFTU_PLANE(path, plane) + 0x200u + (uint32_t)(cfg) * 0x100u)

/* control block */
#define CTL_ENABLE          0x00 /* bit 0: the controller runs */
#define CTL_ALPHA_ENABLE    0x04 /* bit 0: alpha blending unit on */
#define CTL_PLANE_SELECT(p) (0x10 + (p) * 8) /* which configuration shows */
#define CTL_BLEND           0x20

/* plane */
#define PL_HDMI_UNK004  0x004
#define PL_HDMI_UNK030  0x030
#define PL_ALPHA        0x08C
#define PL_ALPHA_MODE   0x0A0 /* bit 0: alpha blending off (fully opaque) */
#define PL_CSC_ENABLE   0x100
#define PL_CSC0         0x104 /* 2 offsets, then the 3x3 matrix */
#define PL_CSC1         0x130 /* 2 offsets, the matrix, 4 more words */

/* configuration */
#define CF_PADDR     0x00
#define CF_SRC_X     0x24
#define CF_SRC_Y     0x28
#define CF_FORMAT    0x40
#define CF_SRC_W     0x44
#define CF_SRC_H     0x48
#define CF_DISABLE   0x4C /* 1: this configuration off */
#define CF_ROW_EXTRA 0x54 /* bytes between one source row and the next */
#define CF_OUT_FMT   0xA0
#define CF_OUT_W     0xA4
#define CF_OUT_H     0xA8
#define CF_STEP_X    0xC0 /* source step per output pixel, 16.16 */
#define CF_STEP_Y    0xC4
#define CF_OUT_X     0xC8
#define CF_OUT_Y     0xCC

#define FORMAT_A8B8G8R8 0x10
#define OUT_FMT_DEFAULT 0x2000

void vd_bus_enable(int path) {
	uintptr_t c = IFTU_CONTROL(path);
	write32(0, c + CTL_BLEND);
	write32(1, c + CTL_ENABLE);
	write32(1, c + CTL_ALPHA_ENABLE);
	dmb();
}

void vd_plane_select(int path, int plane, int cfg) {
	write32((uint32_t)cfg, IFTU_CONTROL(path) + CTL_PLANE_SELECT(plane));
	dmb();
}

/* The plane opaque, with the colour conversion the system programs for
 * its own picture: a YCbCr-style matrix on the first stage and a plain
 * gain on the second. The HDMI path's planes want two more words the
 * system sets (without them the scaled picture came out slanted). */
void vd_plane_colour(int path, int plane, int hdmi) {
	uintptr_t p = IFTU_PLANE(path, plane);
	static const uint32_t csc0[11] = {
		0x40, 0x202,
		0x254, 0x000, 0x395,
		0x254, 0xF93, 0xEF0,
		0x254, 0x439, 0x000,
	};
	static const uint32_t csc1[15] = {
		0x40, 0x40,
		0x1B7, 0x000, 0x000,
		0x000, 0x1B7, 0x000,
		0x000, 0x000, 0x1B7,
		0x3AC, 0x40, 0x3AC, 0x40,
	};
	write32(0, p + PL_ALPHA);
	write32(1, p + PL_ALPHA_MODE);
	for (unsigned i = 0; i < ARRAY_SIZE(csc0); i++)
		write32(csc0[i], p + PL_CSC0 + 4 * i);
	for (unsigned i = 0; i < ARRAY_SIZE(csc1); i++)
		write32(csc1[i], p + PL_CSC1 + 4 * i);
	write32(1, p + PL_CSC_ENABLE);
	if (hdmi) {
		write32(0x101, p + PL_HDMI_UNK004);
		write32(0x1F6100, p + PL_HDMI_UNK030);
	}
	dmb();
}

void vd_plane_picture(int path, int plane, int cfg, uint32_t paddr,
                      uint32_t dst_x, uint32_t dst_y, uint32_t dst_w,
                      uint32_t dst_h, uint32_t scale) {
	uintptr_t c = IFTU_CONFIG(path, plane, cfg);
	uint32_t step = scale ? scale : 0x10000;
	write32(paddr, c + CF_PADDR);
	write32(0, c + CF_SRC_X);
	write32(0, c + CF_SRC_Y);
	write32(FORMAT_A8B8G8R8, c + CF_FORMAT);
	write32(960, c + CF_SRC_W); /* rows of exactly 960 pixels */
	write32(544, c + CF_SRC_H);
	write32(0, c + CF_DISABLE);
	write32(OUT_FMT_DEFAULT, c + CF_OUT_FMT);
	write32(dst_w, c + CF_OUT_W);
	write32(dst_h, c + CF_OUT_H);
	write32(step, c + CF_STEP_X);
	write32(step, c + CF_STEP_Y);
	write32(dst_x, c + CF_OUT_X);
	write32(dst_y, c + CF_OUT_Y);
	if (scale)
		write32(0, c + CF_ROW_EXTRA); /* rows of exactly 960 (the system:
		                                 0x100 for rows of 1024) */
	dmb();
}

void vd_plane_enable(int path, int plane, int cfg, int on) {
	write32(on ? 0 : 1, IFTU_CONFIG(path, plane, cfg) + CF_DISABLE);
	dmb();
}
