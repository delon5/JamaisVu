/* JamaisVu savestate plugin
 *
 * Diagnostics: how often each piece of JamaisVu that runs during gameplay
 * is called and how long its own work takes. Reported in log.txt each
 * time the menu opens (menu.c, perf_report).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __JV_PROF_H__
#define __JV_PROF_H__

#include <stdint.h>

enum {
  PROF_DISPLAY,    /* display hook, JamaisVu's part             */
  PROF_CTRL,       /* controller hooks, JamaisVu's part         */
  PROF_CTRL_TOTAL, /* controller hooks incl. the original read  */
  PROF_SYSCON,     /* syscon command hook, JamaisVu's part      */
  PROF_WRITE,      /* exFAT write hook, JamaisVu's part         */
  PROF_INPUT,      /* menu input thread, one poll               */
  PROF_RENDER,     /* drawing the menu image (on model change)  */
  PROF_COMPOSE,    /* putting the menu on a game frame          */
  PROF_N
};

typedef struct {
  uint32_t calls;
  uint32_t us;
} JvProfStat;

extern volatile JvProfStat g_prof[PROF_N];

uint32_t prof_now(void);

static inline void prof_add(int i, uint32_t t0) {
  __sync_fetch_and_add(&g_prof[i].calls, 1);
  __sync_fetch_and_add(&g_prof[i].us, prof_now() - t0);
}

#endif
