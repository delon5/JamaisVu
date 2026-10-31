/* JamaisVu savestate plugin
 *
 * Copyright (C) 2020 TheFloW
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <psp2kern/kernel/cpu.h>
#include <psp2kern/kernel/modulemgr.h>
#include <psp2kern/kernel/suspend.h>
#include <psp2kern/kernel/sysmem.h>
#include <psp2kern/kernel/threadmgr.h>
#include <psp2kern/io/fcntl.h>
#include <psp2kern/io/dirent.h>
#include <psp2kern/io/stat.h>
#include <psp2kern/ctrl.h>
#include <psp2kern/power.h>
#include <taihen.h>

#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#include "compat.h"
#include "main.h"
#include "fs.h"
#include "offsets.h"
#include "slots.h"
#include "menu.h"

#define JV_VERSION "2.0-dev"

extern int resume_function(void);

extern uint8_t _binary_payload_JamaisVu_payload_bin_start;
extern uint8_t _binary_payload_JamaisVu_payload_bin_size;

int (* _ksceKernelFindClassByName)(const char *name, SceClass **cls);
int (* _ksceGUIDGetUIDVectorByClass)(SceClass *cls, int vis_level,
                                     SceUID *vector, int num, int *ret_num);

static tai_hook_ref_t ksceSysconSendCommandRef;

static uint32_t *g_rtc_ctx;

uint32_t ori_resume_func_vaddr;

PayloadArguments pargs;
uint32_t pargs_size = sizeof(PayloadArguments);

static int g_armed = 0;
static const char *g_disabled_reason = "not started";
static const FwOffsets *g_fw = NULL;
static char g_status[1024];

int jv_armed(void) {
  return g_armed;
}

const char *jv_disabled_reason(void) {
  return g_disabled_reason;
}

/* Virtual -> physical address of a kernel object (ATS1CPR). 0 on fault. */
static uint32_t va2pa(const volatile void *va) {
  uint32_t par;
  __asm__ volatile ("mcr p15, 0, %1, c7, c8, 0\n\t"
                    "isb\n\t"
                    "mrc p15, 0, %0, c7, c4, 0\n\t"
                    : "=r" (par) : "r" (va));
  if (par & 1)
    return 0;
  return (par & 0xfffff000) | ((uint32_t)va & 0xfff);
}

static void *pa2va(uint32_t pa) {
  uint32_t va = 0;
  uint32_t vaddr;
  uint32_t paddr;
  uint32_t i;

  for (i = 0; i < 0x100000; i++) {
    vaddr = i << 12;
    __asm__ volatile ("mcr p15, 0, %1, c7, c8, 0\n\t"
                      "mrc p15, 0, %0, c7, c4, 0\n\t"
                      : "=r" (paddr) : "r" (vaddr));
    if ((pa & 0xfffff000) == (paddr & 0xfffff000)) {
      va = vaddr + (pa & 0xfff);
      break;
    }
  }

  return (void *)va;
}

static void map_memory(void) {
  uint32_t *ttb;
  uint32_t ttbcr;
  uint32_t ttbr0;
  uint32_t ttbr_mask[2];

  __asm__ volatile ("mrc p15, 0, %0, c2, c0, 2\n\t"
                    "mrc p15, 0, %1, c2, c0, 0\n\t"
                    : "=r"(ttbcr), "=r"(ttbr0));

  ttbr_mask[0] = 0xffffffff << (14 - (ttbcr & 0x7));
  ttbr_mask[1] = 0xffffffff << 14;

  ttb = (uint32_t *)pa2va(ttbr0 & ttbr_mask[0]);

  ttb[0x1c0] = 0x1c000000 | 0x91402;
  ttb[0x1f0] = 0x1f000000 | 0x91402;

  __asm__ volatile ("dmb sy");
  __asm__ volatile ("mcr p15, 0, %0, c7, c14, 1"
                    :: "r" (&ttb[0x1c0]) : "memory");
  __asm__ volatile ("mcr p15, 0, %0, c7, c14, 1"
                    :: "r" (&ttb[0x1f0]) : "memory");
  __asm__ volatile ("dsb sy\n\t"
                    "mcr p15, 0, r0, c8, c7, 0\n\t"
                    "dsb sy\n\t"
                    "isb sy" ::: "memory");
}

static int get_ttbs_and_titleids(void) {
  uint32_t ttbcr;
  uint32_t ttbr0;
  uint32_t ttbr_mask[2];
  SceUID vector[JV_MAX_PROCESSES];
  SceClass *cls;
  int i;
  int res;

  res = _ksceKernelFindClassByName("SceUIDProcessClass", &cls);
  if (res < 0)
    return res;

  res = _ksceGUIDGetUIDVectorByClass(cls, 8, vector,
                                     sizeof(vector) / sizeof(SceUID),
                                     (int *)&pargs.process_num);
  if (res < 0) {
    pargs.process_num = 0;
    return res;
  }
  if (pargs.process_num > JV_MAX_PROCESSES)
    pargs.process_num = JV_MAX_PROCESSES;

  __asm__ volatile ("mrc p15, 0, %0, c2, c0, 2\n\t"
                    "mrc p15, 0, %1, c2, c0, 0\n\t"
                    : "=r"(ttbcr), "=r"(ttbr0));

  ttbr_mask[0] = 0xffffffff << (14 - (ttbcr & 0x7));
  ttbr_mask[1] = 0xffffffff << 14;

  for (i = 0; i < pargs.process_num; i++) {
    SceKernelProcessContext *ctx;
    ksceKernelGetPidContext(vector[i], &ctx);
    pargs.ttbs[i] = ctx->TTBR1 & ttbr_mask[1];

    ksceKernelGetProcessTitleId(vector[i], pargs.titleid[i], 32);
  }

  pargs.ttbs[i] = ttbr0 & ttbr_mask[0];

  return 0;
}

static void get_fattime(void) {
  SceDateTime time;
  ksceRtcGetCurrentClock(&time);
  pargs.fattime = ((time.year - 1980) << 25) |
                   (time.month  << 21) |
                   (time.day    << 16) |
                   (time.hour   << 11) |
                   (time.minute << 5)  |
                   (time.second << 0);
}

static int ksceSysconSendCommandPatched(int cmd, void *buffer, uint32_t size) {
  SceSysconResumeContext *resume_ctx;

  if (pargs.mode != JAMAISVU_MODE_NONE) {
    if (cmd == SCE_SYSCON_CMD_RESET_DEVICE && size == sizeof(uintptr_t)) {
      if (*(uintptr_t *)buffer != 0) {
        resume_ctx = (SceSysconResumeContext *)pa2va(*(uintptr_t *)buffer);
        ori_resume_func_vaddr = resume_ctx->resume_func_vaddr;
        resume_ctx->resume_func_vaddr = (uint32_t)&resume_function;
      }
    }
  }

  return TAI_CONTINUE(int, ksceSysconSendCommandRef, cmd, buffer, size);
}

static int savestate_system_event(int resume, int eventid,
                                  void *args, void *opt) {
  if (pargs.mode != JAMAISVU_MODE_NONE) {
    if (!resume) {
      if (eventid == 0x4000) {
        memcpy(pargs.rtc_ctx, g_rtc_ctx, SCE_RTC_CTX_SIZE);

        map_memory();
        get_fattime();

        if (pargs.mode == JAMAISVU_MODE_SAVE)
          get_ttbs_and_titleids();

        /* NOTE: writes are deliberately NOT blocked here any more. This
         * image is about to be discarded (load) or kept (save); either way
         * its final flush must reach the card so the card is consistent
         * before the payload looks at it. Blocking happens inside the
         * restored image instead (the payload arms the flag). */
      }
    } else {
      if (eventid == 0x10000) {
        memcpy(&pargs, (void *)PAYLOAD_ARGS_PADDR, sizeof(PayloadArguments));
        memcpy(g_rtc_ctx, pargs.rtc_ctx, SCE_RTC_CTX_SIZE);
      } else if (eventid == 0x100000) {
        int stale = 0;

        if (pargs.mode == JAMAISVU_MODE_LOAD && pargs.result == JV_OK) {
          /* We are the restored image and the payload has armed
           * FS_WRITES_DROPPED. Re-read ux0 from the card, then allow
           * writes again - unless files that are still open no longer
           * exist, in which case ux0 stays read-only until reboot rather
           * than risk writing through stale cluster chains. */
          stale = update_filesystem();
          fs_set_write_mode(stale ? FS_WRITES_READ_ONLY : FS_WRITES_ALLOWED);
        } else if (fs_get_write_mode() == FS_WRITES_DROPPED) {
          fs_set_write_mode(FS_WRITES_ALLOWED);
        }

        menu_on_resumed(pargs.mode, pargs.result, pargs.slot, stale);
        pargs.mode = JAMAISVU_MODE_NONE;
      }
    }
  }

  return 0;
}

/* Starts a save or load of a slot: fills in the payload arguments and
 * requests the soft reset during which the payload runs. */
int jv_request(int mode, int slot) {
  uint32_t flag_pa;

  if (!g_armed || slot < 0 || slot >= JV_NUM_SLOTS)
    return -1;

  flag_pa = va2pa(fs_write_mode_addr());
  if (flag_pa == 0)
    return -1;

  pargs.slot = slot;
  slot_state_path(pargs.path, sizeof(pargs.path), slot);
  pargs.slot_size = JV_SLOT_SIZE;
  pargs.ignore_flag_paddr = flag_pa;
  pargs.result = JV_ERR_ARGS;
  pargs.applied = 0;
  __sync_synchronize();
  pargs.mode = mode;
  __sync_synchronize();

  int res = kscePowerRequestSoftReset();
  if (res < 0) {
    pargs.mode = JAMAISVU_MODE_NONE;
    return res;
  }
  return 0;
}

static void get_sysmem_nids(void) {
  int res;

  res = module_get_export_func(KERNEL_PID, "SceSysmem",
                               0x63A519E5,
                               0x62989905,
                               (uintptr_t *)&_ksceKernelFindClassByName);
  if (res < 0)
    res = module_get_export_func(KERNEL_PID, "SceSysmem",
                                 0x02451F0F,
                                 0x7D87F706,
                                 (uintptr_t *)&_ksceKernelFindClassByName);

  res = module_get_export_func(KERNEL_PID, "SceSysmem",
                               0x63A519E5,
                               0xEC7D36EF,
                               (uintptr_t *)&_ksceGUIDGetUIDVectorByClass);
  if (res < 0)
    res = module_get_export_func(KERNEL_PID, "SceSysmem",
                                 0x02451F0F,
                                 0x52137FA3,
                                 (uintptr_t *)&_ksceGUIDGetUIDVectorByClass);
}

static int get_rtc_offsets(const FwOffsets *o) {
  tai_module_info_t info;

  memset(&info, 0, sizeof(tai_module_info_t));
  info.size = sizeof(tai_module_info_t);
  if (taiGetModuleInfoForKernel(KERNEL_PID, "SceRtc", &info) < 0)
    return -1;

  return module_get_offset(KERNEL_PID, info.modid, 1, o->rtc_ctx,
                           (uintptr_t *)&g_rtc_ctx);
}

static uint32_t module_nid(const char *name) {
  tai_module_info_t info;
  memset(&info, 0, sizeof(info));
  info.size = sizeof(info);
  if (taiGetModuleInfoForKernel(KERNEL_PID, name, &info) < 0)
    return 0;
  return info.module_nid;
}

static int patch_syscon(void) {
  return taiHookFunctionExportForKernel(KERNEL_PID,
                                        &ksceSysconSendCommandRef,
                                        "SceSyscon",
                                        TAI_ANY_LIBRARY,
                                        0xE26488B9,
                                        ksceSysconSendCommandPatched);
}

static int arm(void) {
  const char *why;

  g_fw = offsets_find(module_nid("SceIofilemgr"));
  if (!offsets_usable(g_fw, &why)) {
    g_disabled_reason = why;
    return -1;
  }

  get_sysmem_nids();
  if (!_ksceKernelFindClassByName || !_ksceGUIDGetUIDVectorByClass) {
    g_disabled_reason = "SceSysmem functions not found";
    return -1;
  }

  if (get_rtc_offsets(g_fw) < 0 || !g_rtc_ctx) {
    g_disabled_reason = "SceRtc context not found";
    return -1;
  }

  if (fs_init(g_fw) < 0) {
    g_disabled_reason = "could not hook the exFAT driver";
    return -1;
  }

  if (patch_syscon() < 0) {
    g_disabled_reason = "could not hook SceSyscon";
    return -1;
  }

  if (ksceKernelRegisterSysEventHandler("savestate_system_event",
                                        savestate_system_event, NULL) < 0) {
    g_disabled_reason = "could not register the system event handler";
    return -1;
  }

  g_disabled_reason = "armed";
  return 0;
}

void _start() __attribute__ ((weak, alias ("module_start")));
int module_start(SceSize args, void *argp) {
  memset(&pargs, 0, sizeof(PayloadArguments));

  g_armed = (arm() == 0);

  snprintf(g_status, sizeof(g_status),
           "JamaisVu " JV_VERSION "\n"
           "status: %s\n"
           "firmware table: %s\n"
           "SceIofilemgr NID: 0x%08X\n"
           "SceExfatfs NID:   0x%08X\n"
           "SceRtc NID:       0x%08X\n"
           "SceSysmem NID:    0x%08X\n"
           "SceCtrl NID:      0x%08X\n"
           "SceDisplay NID:   0x%08X\n"
           "\n"
           "If status is not \"armed\", JamaisVu changes nothing on this\n"
           "system. See docs/porting-3.60.md.\n",
           g_armed ? "armed" : g_disabled_reason,
           g_fw ? g_fw->name : "none",
           (unsigned)module_nid("SceIofilemgr"),
           (unsigned)module_nid("SceExfatfs"),
           (unsigned)module_nid("SceRtc"),
           (unsigned)module_nid("SceSysmem"),
           (unsigned)module_nid("SceCtrl"),
           (unsigned)module_nid("SceDisplay"));

  menu_init(g_status);

  return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize args, void *argp) {
  return SCE_KERNEL_STOP_CANCEL;
}
