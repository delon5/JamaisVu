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
/* clock speeds: record before a save/load, put back after it (see main.c) */
void jv_clocks_remember(char *out, int len);
void jv_clocks_restore(char *out, int len);
#define JV_PAYLOAD_DEFAULT 0 /* the only payload (variants were for A/B tests) */
/* Display test (Diagnostics): the frame to draw into; the hardware info
 * word (PCH-2000: (info & 0xFF0000) == 0x800000) */
void jv_set_display_frame(uint32_t paddr, uint32_t pitch, uint32_t width,
                          uint32_t height, uint32_t vic);
uint32_t jv_hw_info(void);
/* loads: the snapshot's own frame (slotN.jvf), drawn into once the memory
 * restore starts; 0 = none */
void jv_set_display_frame2(uint32_t paddr, uint32_t pitch);
int jv_request(int mode, int slot, const uint8_t *nonce, int debug_mode,
               uint32_t trace_rel_lba, const uint8_t *trace_nonce,
               int payload_variant);

extern int (* _ksceKernelFindClassByName)(const char *name, SceClass **cls);
extern int (* _ksceGUIDGetUIDVectorByClass)(SceClass *cls, int vis_level,
                                            SceUID *vector, int num, int *ret_num);

#endif
