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
/* processes in the last snapshot request (diagnostics) */
uint32_t jv_last_process_num(void);
/* the last job's savedata: which folders the plugin selected, how many
 * records the payload backed up / put back and from / into which folders */
void jv_last_savedata(uint32_t *selected, uint32_t *files, uint32_t *folders);
/* number of kernel objects of a class (e.g. "SceUIDProcessClass"), < 0 if
 * the class is unknown (diagnostics: counted, nothing changed) */
int jv_count_class(const char *name);
/* clock speeds: record before a save/load, put back after it (see main.c) */
void jv_clocks_remember(char *out, int len);
void jv_clocks_restore(char *out, int len);
#define JV_PAYLOAD_DEFAULT 0 /* the only payload (variants were for A/B tests) */
/* Display test (Diagnostics): the frame to draw into; the hardware info
 * word (PCH-2000: (info & 0xFF0000) == 0x800000) */
void jv_set_display_frame(uint32_t paddr, uint32_t pitch, uint32_t width,
                          uint32_t height, uint32_t vic);
uint32_t jv_hw_info(void);
uint32_t jv_disp_type(void); /* JV_DISP_* */
/* system time at: save/load requested, sleep event, wake event, system
 * resumed (main.c); the first game frame after it is timed in menu.c */
extern volatile int64_t g_t_request, g_t_suspend, g_t_resume, g_t_resumed;
/* PS TV display test: the payload's HDMI register copy (hwdump.c) */
extern uint32_t g_regsnap[JV_REGSNAP_WORDS];
extern volatile int g_regsnap_ok;
extern volatile int64_t g_t_fs_done;
#define JV_EV_MAX 64
extern volatile int g_ev_open, g_ev_normal, g_ev_normal_ready;
extern int64_t g_ev_t0, g_ev_r0;
/* first resume event of the last wake-up, any kind of sleep (0 = none) */
extern volatile int64_t g_last_wake_t;
extern volatile uint32_t g_ev_n;
extern uint32_t g_ev_id[JV_EV_MAX];
extern int64_t g_ev_t[JV_EV_MAX], g_ev_last[JV_EV_MAX];
extern uint32_t g_ev_cnt[JV_EV_MAX];
/* loads: the snapshot's own frame (slotN.jvf), drawn into once the memory
 * restore starts; 0 = none */
void jv_set_display_frame2(uint32_t paddr, uint32_t pitch);
void jv_set_appdb_changed(uint32_t v); /* 0 unknown, 1 same, 2 changed */
int jv_request(int mode, int slot, const uint8_t *nonce, int debug_mode,
               uint32_t trace_rel_lba, const uint8_t *trace_nonce,
               int payload_variant);

extern int (* _ksceKernelFindClassByName)(const char *name, SceClass **cls);
extern int (* _ksceGUIDGetUIDVectorByClass)(SceClass *cls, int vis_level,
                                            SceUID *vector, int num, int *ret_num);

#endif
