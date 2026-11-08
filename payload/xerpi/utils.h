/* JamaisVu: what lcd.c needs from libbaremetal's utils.h, now provided by
 * payload/display/utils.h (JamaisVu's own), plus the bit reversal in the
 * form lcd.c uses it (rbit(x) >> 24). */
#ifndef UTILS_H
#define UTILS_H

#include "../display/utils.h"

static inline uint32_t rbit(uint32_t x)
{
	uint32_t xrev;
	asm volatile("rbit %0, %1\n\t" : "=r"(xrev) : "r"(x));
	return xrev;
}

#endif
