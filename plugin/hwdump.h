/* JamaisVu savestate plugin
 *
 * Read-only dump of the system's live HDMI setup (PS TV), see hwdump.c.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __JV_HWDUMP_H__
#define __JV_HWDUMP_H__

/* Logs the display controller, display link and HDMI bridge registers
 * through log (one call per line). Reads only. */
void hwdump_hdmi(void (*log)(const char *));

#include <stdint.h>
/* logs the non-zero words of a JV_REGSNAP_RANGES snapshot (who = "system"
 * or "ours") */
void hwdump_regs(void (*log)(const char *), const char *who,
                 const uint32_t *words);

#endif
