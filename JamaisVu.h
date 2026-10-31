/* JamaisVu savestate plugin
 *
 * Copyright (C) 2020 TheFloW
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __JAMAISVU_H__
#define __JAMAISVU_H__

#include <stdint.h>
#include "shared.h"

#define ALIGN(x, align) (((x) + ((align) - 1)) & ~((align) - 1))
#define MIN(x, y) (((x) < (y)) ? (x) : (y))
#define MAX(x, y) (((x) > (y)) ? (x) : (y))

#define SCE_RTC_CTX_SIZE 0x1c0

typedef struct SceSysconResumeContext {
  uint32_t size;
  uint32_t unk;
  uint32_t buff_vaddr;
  uint32_t resume_func_vaddr;
  uint32_t SCTLR;
  uint32_t ACTLR;
  uint32_t CPACR;
  uint32_t TTBR0;
  uint32_t TTBR1;
  uint32_t TTBCR;
  uint32_t DACR;
  uint32_t PRRR;
  uint32_t NMRR;
  uint32_t VBAR;
  uint32_t CONTEXTIDR;
  uint32_t TPIDRURW;
  uint32_t TPIDRURO;
  uint32_t TPIDRPRW;
  uint64_t unk2;
  uint64_t current_time;
  uint64_t alarm_tick;
  uint64_t system_time;
} SceSysconResumeContext;

/* Payload <-> plugin interface. Copied to PAYLOAD_ARGS_PADDR before the
 * payload runs and copied back by the plugin on resume. Only fixed-size
 * 32-bit fields so the layout is identical on every build (including the
 * host test harness). */
#define JV_MAX_PROCESSES 32

typedef struct {
  uint32_t mode;
  uint32_t fattime;
  uint32_t process_num;
  uint32_t ttbs[JV_MAX_PROCESSES + 1]; /* + 1 for the kernel TTB */
  char titleid[JV_MAX_PROCESSES][32];
  char rtc_ctx[SCE_RTC_CTX_SIZE];
  char path[256];
  /* Exact size the plugin pre-allocated for the slot file. The payload
   * refuses to save into a file of any other size. */
  uint32_t slot_size;
  /* Physical address of the plugin's write-blocking flag. Stored in the
   * savestate so the payload can arm the flag inside the restored image. */
  uint32_t ignore_flag_paddr;
  /* Filled in by the payload. */
  int32_t result;
  uint32_t applied; /* load: 1 once the payload started modifying anything */
} PayloadArguments;

enum JamaisVuModes {
  JAMAISVU_MODE_NONE,
  JAMAISVU_MODE_LOAD,
  JAMAISVU_MODE_SAVE,
};

/* Payload result codes (PayloadArguments.result) */
#define JV_OK                   0
#define JV_ERR_CARD          (-1)  /* Memory Card init / mount failed      */
#define JV_ERR_NO_SLOT       (-2)  /* slot file missing or wrong size      */
#define JV_ERR_FRAGMENTED    (-3)  /* slot file has too many fragments     */
#define JV_ERR_TOO_BIG       (-4)  /* state does not fit in the slot       */
#define JV_ERR_IO            (-5)  /* read/write error                      */
#define JV_ERR_BAD_STATE     (-6)  /* state file failed validation         */
#define JV_ERR_OTHER_CARD    (-7)  /* state was made on another card       */
#define JV_ERR_GUARD         (-8)  /* write blocked by the sector guard    */
#define JV_ERR_MEDIAID       (-9)  /* mediaid partition not found          */
#define JV_ERR_SAVEDATA      (-10) /* savedata backup failed               */
#define JV_ERR_ARGS          (-11) /* invalid arguments from the plugin    */

typedef struct {
  uint32_t addr; /* physical address (or special value) */
  uint32_t size;
} Segment;

/* One savedata entry: header, NUL-terminated path padded to 4 bytes,
 * then (files only) data_len bytes of content padded to 4 bytes. */
typedef struct {
  uint8_t  is_root;
  uint8_t  fattrib;
  uint16_t ftime;
  uint16_t fdate;
  uint16_t path_len; /* including NUL, multiple of 4 */
  uint32_t data_len;
  uint32_t reserved;
} SaveStateFile;

/* Savestate file layout (version 2):
 *   [0x000, 0x200)            SaveStateHeader (rest of the sector is zero)
 *   [0x200, 0x400)            copy of the mediaid sector
 *   [files_offset, +files_size)    SaveStateFile records (savedata backup)
 *   [segtab_offset, +segtab_size)  Segment table
 *   zero padding up to data_offset (sector aligned)
 *   [data_offset, +data_size)       memory contents, in segment table order
 *
 * body_crc covers [0x200, data_offset). The header is written last, so an
 * interrupted save never produces a loadable file. */
typedef struct {
  uint32_t magic;
  uint32_t version;
  uint32_t header_size;
  uint32_t file_num;
  uint32_t segs_num;
  uint32_t mediaid_sector;
  uint32_t mbr_crc;
  uint32_t files_offset;
  uint32_t files_size;
  uint32_t segtab_offset;
  uint32_t segtab_size;
  uint32_t data_offset;
  uint32_t data_size;
  uint32_t ignore_flag_paddr;
  uint32_t body_crc;
  uint32_t header_crc; /* crc32 of all preceding header bytes */
} SaveStateHeader;

#define SAVESTATE_MAGIC   0x54535653
#define SAVESTATE_VERSION 2

#define SAVESTATE_MEDIAID_OFFSET 0x200
#define SAVESTATE_FILES_OFFSET   0x400

#define SAVEDATA_PREFIX "ux0:user/00/savedata/"

#define DRAM_VRAM_START 0x20000000u
#define DRAM_VRAM_END   0x60000000u
#define DRAM_START      0x40000000u
#define SPRAM_SIZE      (80 * 1024)

#endif
