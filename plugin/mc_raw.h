/* JamaisVu savestate plugin
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __MC_RAW_H__
#define __MC_RAW_H__

#include <stdint.h>

/* User partition of the official Memory Card (same name StorageMgr uses) */
#define MC_RAW_DEVICE "sdstor0:xmc-lp-ign-userext"

int mc_raw_open(void);  /* < 0: no Memory Card (or not readable) */
void mc_raw_close(void);
int mc_raw_read(void *buf, uint32_t sector, uint32_t count);

#endif
