/* JamaisVu savestate plugin
 *
 * Copyright (C) 2020 TheFloW
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __STATE_H__
#define __STATE_H__

#include "ff.h"

/* Both return JV_OK or a negative JV_ERR_* code. load_state() sets
 * pargs->applied once it has started modifying the card or memory. */
int save_state(FATFS *fs, const char *path);
int load_state(FATFS *fs, const char *path);

#endif
