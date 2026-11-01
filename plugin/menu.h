/* JamaisVu savestate plugin
 *
 * In-game slot menu (START + UP): input, overlay drawing, and the worker
 * thread that prepares slots and starts save/load.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __MENU_H__
#define __MENU_H__

#include <stdint.h>
#include "../JamaisVu.h"

/* Installs the display and controller hooks and starts the threads.
 * status_text is written to data/JamaisVu/status.txt on the Memory Card
 * once it is available. */
int menu_init(const char *status_text);

/* Called from the resume system event once the payload has finished. */
void menu_on_resumed(const PayloadArguments *pa, int stale_open_files);

#endif
