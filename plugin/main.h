/* JamaisVu savestate plugin
 *
 * Copyright (C) 2020 TheFloW
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __MAIN_H__
#define __MAIN_H__

#include <psp2kern/kernel/sysmem.h>
#include "../JamaisVu.h"

int ksceRtcGetCurrentClock(SceDateTime *time);

int module_get_export_func(SceUID pid, const char *modname, uint32_t libnid,
                           uint32_t funcnid, uintptr_t *func);
int module_get_offset(SceUID pid, SceUID modid, int segidx,
                      size_t offset, uintptr_t *addr);

int jv_armed(void);
int jv_armed_untested(void);
const char *jv_disabled_reason(void);
const char *jv_build_id(void);
int jv_request(int mode, int slot, const uint8_t *nonce, int debug_mode,
               uint32_t trace_rel_lba, const uint8_t *trace_nonce);

extern int (* _ksceKernelFindClassByName)(const char *name, SceClass **cls);
extern int (* _ksceGUIDGetUIDVectorByClass)(SceClass *cls, int vis_level,
                                            SceUID *vector, int num, int *ret_num);

#endif
