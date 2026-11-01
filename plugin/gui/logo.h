/* JamaisVu savestate plugin
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __JV_LOGO_H__
#define __JV_LOGO_H__

#include "render.h"

/* Draws the logo scale times as large (1 = the menu's size), top-left at
 * x, y; returns its width in pixels. c may be NULL to only measure. */
int logo_draw(Canvas *c, int x, int y, int scale);

#endif
