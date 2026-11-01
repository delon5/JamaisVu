/* From xerpi's vita-libbaremetal (libbaremetal/src/utils.c), see NOTICE.md */
#include <stdint.h>
#include "utils.h"

void delay(uint32_t n)
{
	volatile uint32_t i, j;

	for (i = 0; i < n; i++)
		for (j = 0; j < 200; j++)
			;
}
