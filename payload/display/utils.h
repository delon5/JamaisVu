/* JamaisVu savestate plugin
 *
 * Small helpers for the display code: register access, barriers, timed
 * delays and bounded waits. Every busy-wait on the hardware gives up
 * after a fraction of a second and is counted, so a device that never
 * answers cannot hang the payload with the screen on (seen on hardware).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef JV_DISP_UTILS_H
#define JV_DISP_UTILS_H

#include <stdint.h>

#define BIT(x) (1u << (x))
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

#define dmb() asm volatile("dmb\n\t" ::: "memory")
#define dsb() asm volatile("dsb\n\t" ::: "memory")

static inline uint32_t read32(uintptr_t addr) {
	return *(volatile uint32_t *)addr;
}

static inline void write32(uint32_t val, uintptr_t addr) {
	*(volatile uint32_t *)addr = val;
}

/* a byte with its bits mirrored (bit 7 <-> bit 0) */
static inline uint32_t bitrev8(uint32_t x) {
	uint32_t r;
	asm volatile("rbit %0, %1\n\t" : "=r"(r) : "r"(x));
	return r >> 24;
}

/* delay(n): n units of ~160 ns as the loops used to run with the
 * instruction cache off. jv_delay_calibrate() measures the unit once,
 * before the cache is switched on, so the lengths stay the same with it
 * on; until then delay() counts. */
void delay(uint32_t n);
uint32_t jv_time_us(void);
uint32_t jv_delay_calibrate(void);
uint32_t jv_wait_limit_us(uint32_t polls);

#define JV_WAIT_MAX 2000000u
extern volatile uint32_t g_jv_wait_timeouts;
#define JV_WAIT_WHILE(cond) JV_WAIT_WHILE_N(cond, JV_WAIT_MAX)
/* the same, for a wait that is expected to run out (not counted) */
#define JV_WAIT_WHILE_QUIET_N(cond, max)                      \
	do {                                                  \
		uint32_t jv_t0_ = jv_time_us();               \
		uint32_t jv_lim_ = jv_wait_limit_us(max);     \
		while ((cond) && jv_time_us() - jv_t0_ < jv_lim_) \
			;                                     \
	} while (0)
#define JV_WAIT_WHILE_N(cond, max)                            \
	do {                                                  \
		uint32_t jv_t0_ = jv_time_us();               \
		uint32_t jv_lim_ = jv_wait_limit_us(max);     \
		while (cond) {                                \
			if (jv_time_us() - jv_t0_ >= jv_lim_) {   \
				g_jv_wait_timeouts++;         \
				break;                        \
			}                                     \
		}                                             \
	} while (0)

#endif
