/* JamaisVu savestate plugin
 *
 * Persistent trace (data/JamaisVu/trace.bin), see trace.c.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __JV_TRACE_H__
#define __JV_TRACE_H__

#include <stdint.h>
#include "../JamaisVu.h"

const char *jv_stage_name(uint32_t stage);
const char *jv_result_text(int32_t res);

/* Writes trace.bin with a fresh nonce and finds its sector on the card
 * (relative to the card device the OS mounts). 0 = ready. */
int trace_prepare(uint32_t *rel_lba, uint8_t nonce[16], const char **why);

/* At boot: if the last run left a record in trace.bin, formats it into
 * out, marks it consumed and returns 1. */
int trace_collect(char *out, int len);

/* After a normal resume (the result is logged from the arguments). */
void trace_consumed(void);

void trace_from_args(JvTraceSector *t, const PayloadArguments *pa);

/* Steps after the wake-up, written straight into trace.bin's sector on
 * the card (through the card's raw device, that one sector only), so a
 * freeze after the payload returned is in the next boot's log.
 * trace_wake_begin at the first resume event (pargs from the payload),
 * trace_wake per step, trace_wake_end once the result is logged. */
enum {
  JV_WAKE_RESUME = 1,     /* first resume event */
  JV_WAKE_RESUMED,        /* last resume event */
  JV_WAKE_FS_CACHES,      /* load: buffer + file caches dropped */
  JV_WAKE_FS_PATCH,       /* load: remount patch applied */
  JV_WAKE_FS_VNODES,      /* load: open files checked */
  JV_WAKE_FS_FNODES,      /* load: cached files checked */
  JV_WAKE_MENU,           /* result handed to the menu */
  JV_WAKE_WORKER,         /* worker thread awake */
  JV_WAKE_CLOCKS,         /* clocks restored and logged */
  JV_WAKE_SLOT_INFO,      /* save: writing the slot info */
  JV_WAKE_SLOT_INFO_DONE, /* save: slot info written */
  JV_WAKE_REPORT,         /* result message */
  JV_WAKE_FS_MOUNT = 0x40 /* + n: load, remounting filesystem n */
};
void trace_wake_begin(const PayloadArguments *pa);
void trace_wake(uint8_t step);
/* steps are only written to the card once this is on: not during the
 * early resume events (the card's driver may not be back yet) - from the
 * load's filesystem refresh (which uses the card anyway) or the worker */
void trace_wake_io(int on);
/* returns how many steps were written and the last write's result */
int trace_wake_end(int *last_ret);
int trace_format(char *out, int len, const JvTraceSector *t);

#endif
