/* From xerpi's vita-libbaremetal (libbaremetal/src/utils.c), see NOTICE.md */
#include <stdint.h>
#include "utils.h"

volatile uint32_t g_jv_wait_timeouts; /* JamaisVu, see JV_WAIT_WHILE */

/* ns one delay() unit took as a counted loop with the I-cache off;
 * 0 = not measured yet (delay() counts, as before) */
static uint32_t g_delay_unit_ns;

static void delay_counted(uint32_t n)
{
	volatile uint32_t i, j;

	for (i = 0; i < n; i++)
		for (j = 0; j < 200; j++)
			;
}

/* Call once with the I-cache still off. */
uint32_t jv_delay_calibrate(void)
{
	uint32_t t0 = jv_time_us();
	delay_counted(20);
	uint32_t ns = (jv_time_us() - t0) * 50; /* us * 1000 / 20 */
	if (ns < 1000)
		ns = 1000;
	g_delay_unit_ns = ns;
	return ns;
}

void delay(uint32_t n)
{
	if (!g_delay_unit_ns) {
		delay_counted(n);
		return;
	}
	uint64_t us = ((uint64_t)n * g_delay_unit_ns + 999) / 1000;
	while (us) { /* jv_time_us() differences: chunks well below 2^32 */
		uint32_t chunk = us > 1000000 ? 1000000 : (uint32_t)us;
		uint32_t t0 = jv_time_us();
		while (jv_time_us() - t0 < chunk)
			;
		us -= chunk;
	}
}

/* A poll of a JV_WAIT_WHILE loop took about as long as one of delay()'s
 * 200 inner iterations (both one uncached load, an add and a branch);
 * twice that as the limit, at least 1 ms. */
uint32_t jv_wait_limit_us(uint32_t polls)
{
	uint32_t unit = g_delay_unit_ns ? g_delay_unit_ns : 200000;
	uint64_t us = (uint64_t)polls * unit / 200 / 1000 * 2;
	if (us < 1000)
		us = 1000;
	if (us > 10000000)
		us = 10000000;
	return (uint32_t)us;
}
