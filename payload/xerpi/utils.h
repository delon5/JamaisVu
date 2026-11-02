#ifndef UTILS_H
#define UTILS_H

#include <stdint.h>

#define BIT(x)		(1 << (x))
#define ARRAY_SIZE(x)	(sizeof(x) / sizeof((x)[0]))

#define dmb() asm volatile("dmb\n\t" ::: "memory")
#define dsb() asm volatile("dsb\n\t" ::: "memory")
#define wfe() asm volatile("wfe\n\t" ::: "memory")

typedef uint32_t u32;

static inline uint32_t rbit(uint32_t x)
{
	uint32_t xrev;
	asm volatile("rbit %0, %1\n\t" : "=r"(xrev) : "r"(x));
	return xrev;
}

static inline uint8_t read8(uintptr_t addr)
{
	return *(volatile uint8_t *)addr;
}

static inline uint16_t read16(uintptr_t addr)
{
	return *(volatile uint16_t *)addr;
}

static inline uint32_t read32(uintptr_t addr)
{
	return *(volatile uint32_t *)addr;
}

static inline void write8(uint8_t val, uintptr_t addr)
{
	*(volatile uint8_t *)addr = val;
}

static inline void write16(uint16_t val, uintptr_t addr)
{
	*(volatile uint16_t *)addr = val;
}

static inline void write32(uint32_t val, uintptr_t addr)
{
	*(volatile uint32_t *)addr = val;
}

static inline uint64_t be_uint64_t_load(const void *addr)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	return __builtin_bswap64(*(uint64_t *)addr);
#else
	return *(uint64_t *)addr;
#endif
}

static inline void be_uint64_t_store(void *addr, uint64_t val)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	*(uint64_t *)addr = __builtin_bswap64(val);
#else
	*(uint64_t *)addr = val;
#endif
}

static inline uint32_t smc(uint32_t cmd, uint32_t arg1,
			   uint32_t arg2, uint32_t arg3,
			   uint32_t arg4)
{
	register uint32_t r0 asm("r0") = arg1;
	register uint32_t r1 asm("r1") = arg1;
	register uint32_t r2 asm("r2") = arg1;
	register uint32_t r3 asm("r3") = arg1;
	register uint32_t r12 asm("r12") = cmd;

	asm volatile(
		"smc #0\n\t"
		: "+r"(r0), "+r"(r1), "+r"(r2), "+r"(r3)
		: "r"(r12)
	);

	return r0;
}

void delay(uint32_t n);

/* JamaisVu: delay() and the waits below used to count loop iterations.
 * With the instruction cache on (payload main()) loops run several times
 * faster, so they are now timed with the cycle counter instead, keeping
 * the lengths they had with the cache off: jv_delay_calibrate() measures
 * delay()'s unit once, before the cache is switched on. */
uint32_t jv_time_us(void);
uint32_t jv_delay_calibrate(void);
uint32_t jv_wait_limit_us(uint32_t polls);

/* JamaisVu: every busy-wait on the hardware gives up after JV_WAIT_MAX
 * polls (a fraction of a second) and is counted, so a device that never
 * answers cannot hang the payload with the screen on (seen on hardware). */
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
uint32_t get_cpu_id(void);

#endif
