/* JamaisVu savestate plugin
 *
 * Copyright (C) 2020 TheFloW
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __SHARED_H__
#define __SHARED_H__

#define PAYLOAD_PADDR      0x1c000000
#define STACK_PADDR        0x1c100000
#define PAYLOAD_ARGS_PADDR 0x1c080000
#define SCE_RESUME_PADDR   0x1f000000
#define CONTEXT_PADDR      0x1f001000
#define SPRAM_PADDR        0x1f840000
/* PS TV display test: the payload's copy of the HDMI display registers
 * while its picture is on screen (JamaisVu.h: JV_REGSNAP_*) */
#define REGSNAP_PADDR      0x1c088000

#endif
