/* JamaisVu savestate plugin
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __PHYS_H__
#define __PHYS_H__

#include <stdint.h>

/* The payload runs with the MMU off, so physical addresses are used as
 * pointers directly. The host test harness provides its own mapping. */
#ifdef JV_HOST_TEST
void *jv_phys(uint32_t paddr, uint32_t size);
#else
static inline void *jv_phys(uint32_t paddr, uint32_t size) {
  (void)size;
  return (void *)(uintptr_t)paddr;
}
#endif

#endif
