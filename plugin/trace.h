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
int trace_format(char *out, int len, const JvTraceSector *t);

#endif
