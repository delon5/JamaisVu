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
  uint32_t slot; /* 0-based slot index, informational for the plugin */
  /* 1 if the Memory Card holding the slot files is the system's ux0, i.e.
   * the running games' savedata lives on it and can be backed up/restored.
   * 0 when the Memory Card is mounted as uma0 (ux0 = SD2Vita/PSVSD/USB):
   * the payload cannot reach that savedata, and must not touch the
   * Memory Card's own (unused) savedata folder either. */
  uint32_t backup_savedata;
  /* Exact size the plugin pre-allocated for the slot file. The payload
   * refuses to save into a file of any other size. */
  uint32_t slot_size;
  /* Random token the plugin wrote (through the OS) at the start of the
   * slot file just before a save. The payload only writes to the file it
   * finds on the Memory Card if it contains the same token, i.e. if it is
   * the very file the OS sees as ux0 (not e.g. an SD2Vita/USB ux0). */
  uint8_t slot_nonce[16];
  /* Physical address of the plugin's write-blocking flag. Stored in the
   * savestate so the payload can arm the flag inside the restored image. */
  uint32_t ignore_flag_paddr;
  /* Diagnostics (JV_TEST_*): 0 for a normal save/load */
  uint32_t debug_mode;
  /* Filled in by the payload. */
  int32_t result;
  uint32_t applied; /* load: 1 once the payload started modifying anything */
  uint32_t stage;   /* last JV_STAGE_* reached (diagnostics) */
  int32_t detail;   /* low-level error code behind result (diagnostics) */
  uint32_t soc_timer_10ms; /* SoC timer ticks in 10 ms (diagnostics) */
  /* Diagnostics: steps reached (JV_STAGE_*) with the time in ms since the
   * payload started, and the CPU fault if the payload crashed. */
  uint32_t trace_n;
  uint8_t trace_stage[24];
  uint32_t trace_ms[24];
  uint32_t fault_type; /* 1 undef, 2 svc, 3 prefetch abort, 4 data abort */
  uint32_t fault_addr; /* DFAR / IFAR */
  uint32_t fault_lr;   /* return address of the exception */
  /* Persistent trace (see JvTraceSector): sector of data/JamaisVu/trace.bin
   * relative to the start of the Memory Card's exFAT partition, and the
   * random signature the plugin wrote there. 0 = not available. */
  uint32_t trace_rel_lba;
  uint8_t trace_nonce[16];
  /* Diagnostics "Display test" (JV_TEST_DISPLAY): the frame the game was
   * showing (physical address, physically contiguous) and the hardware
   * info the LCD backlight setup needs. Appended: the layout above stays. */
  uint32_t disp_fb_paddr;
  uint32_t disp_fb_pitch, disp_fb_width, disp_fb_height;
  uint32_t disp_hw_info, disp_hw_flags0;
  /* display timing (DSI "vic"): 0 = A (libbaremetal's choice for the
   * LCD), 0x20 = B (the other 960x544 timing) - A/B test of the edges */
  uint32_t disp_vic;
  /* Loads: the snapshot's own frame (recorded when it was saved), where
   * the progress screen continues once memory is being restored. 0 = none:
   * the screen goes dark for the restore. */
  uint32_t disp_fb2_paddr, disp_fb2_pitch;
  /* Filled in by the payload: memory written (save) or restored (load), in
   * bytes, and the ms the memory part took - for comparing speeds. */
  uint32_t mem_bytes, mem_ms;
  /* Filled in by the payload: 1 if it ran with the instruction cache on,
   * and the measured length of one xerpi delay() unit (ns, cache off). */
  uint32_t icache_on;
  uint32_t delay_unit_ns;
  /* Which display the progress screen drives (JV_DISP_* in
   * payload/xerpi/display_test.h): 1 LCD (PCH-2000), 2 OLED (PCH-1000),
   * 3 HDMI (PS TV); 0 = none, the screen stays dark. */
  uint32_t disp_type;
  /* Loads (set by the plugin before the request, read after it by the
   * restored image): the LiveArea's app database compared with the one
   * recorded at save time - 0 unknown (no record), 1 same, 2 changed. */
  uint32_t appdb_changed;
} PayloadArguments;

/* Register snapshot (REGSNAP_PADDR in shared.h): the PS TV's HDMI display
 * registers, read in these ranges in this order - by the payload while
 * its display test picture is on screen, by the plugin while the
 * system's own picture is (to compare all of them, not only the ones
 * JamaisVu writes: the display controller is reset during sleep). Word 0
 * is JV_REGSNAP_MAGIC once the payload has filled it. */
#define JV_REGSNAP_MAGIC 0x4752564Au /* "JVRG" */
typedef struct {
  uint32_t paddr, words;
} JvRegRange;
#define JV_REGSNAP_RANGES {                                       \
  { 0xE5030000, 256 }, /* IFTU bus 1 plane A: 0x000-0x3FC */     \
  { 0xE5032000,  32 }, /* IFTU bus 1 control: 0x2000-0x207C */  \
  { 0xE5060000,  64 }, /* DSI bus 1: 0x000-0x0FC */              \
  { 0xE5060500,   8 }, /* DSI 0x140-0x147 (word index) */        \
  { 0xE5060800,  16 }, /* DSI 0x200-0x20F */                     \
  { 0xE5060900,  20 }, /* DSI 0x240-0x253 */                     \
}
#define JV_REGSNAP_WORDS (256 + 32 + 64 + 8 + 16 + 20)

/* First sector of data/JamaisVu/trace.bin on the Memory Card. The plugin
 * writes it (through the OS) with a fresh nonce before every run and
 * finds where it lies on the card. Once the card is authenticated, the
 * payload checks magic + nonce and then rewrites this one sector after
 * every step, so the progress survives even a forced power-off; the
 * plugin copies it into log.txt on the next boot. */
#define JV_TRACE_MAGIC "JVTRACE1"
typedef struct {
  char magic[8];
  uint8_t nonce[16];
  uint32_t written;       /* 1 once the payload has written it */
  uint32_t mode, debug_mode, slot;
  int32_t result;         /* JV_ERR_CARD until the run ends */
  int32_t detail;
  uint32_t stage;
  uint32_t soc_timer_10ms;
  uint32_t trace_n;
  uint8_t trace_stage[24];
  uint32_t trace_ms[24];
  uint32_t fault_type, fault_addr, fault_lr;
  /* Written by the plugin, not the payload (which leaves these 0): its own
   * steps after the wake-up (JV_WAKE_*, ms since the first resume event),
   * so a freeze after the payload returned still shows how far the
   * system got (PCH-1000 with the card on uma0: froze twice). */
  uint32_t wake_n;
  uint8_t wake_step[32];
  uint16_t wake_ms[32];
  uint8_t pad[512 - 8 - 16 - 4 * 9 - 24 - 4 * 24 - 4 * 3 - 4 - 32 - 2 * 32];
} JvTraceSector;
_Static_assert(sizeof(JvTraceSector) == 512, "one sector");

enum JamaisVuModes {
  JAMAISVU_MODE_NONE,
  JAMAISVU_MODE_LOAD,
  JAMAISVU_MODE_SAVE,
};

/* Diagnostics: neither test writes anything */
#define JV_TEST_NONE   0
#define JV_TEST_RESET  1 /* payload returns immediately: soft reset only */
#define JV_TEST_DRYRUN 2 /* runs a save up to (not including) the first write */
/* Which display the progress screen drives (PayloadArguments.disp_type) */
#ifndef JV_DISP_LCD
#define JV_DISP_NONE 0
#define JV_DISP_LCD  1 /* PCH-2000: LCD on display link 0 */
#define JV_DISP_OLED 2 /* PCH-1000: OLED on display link 0 */
#define JV_DISP_HDMI 3 /* PS TV: HDMI bridge on display link 1, 720p */
#endif

#define JV_TEST_DISPLAY 3 /* switches the screen on, test picture, off again */

/* Payload progress (PayloadArguments.stage) */
#define JV_STAGE_ENTRY        1
#define JV_STAGE_L2_FLUSH     2
#define JV_STAGE_SYSCON       3
#define JV_STAGE_CDRAM        4
#define JV_STAGE_MC_POWER     5
#define JV_STAGE_MC_START     6
#define JV_STAGE_MC_MOUNT     7
#define JV_STAGE_MC_AUTH      8
#define JV_STAGE_FS_MOUNT     9
#define JV_STAGE_SLOT_FOUND   10
#define JV_STAGE_SLOT_TOKEN   11
#define JV_STAGE_WRITING      12
#define JV_STAGE_DONE         13 /* payload about to return to the OS */
/* Display test steps (JV_TEST_DISPLAY), in order */
#define JV_STAGE_DISP_I2C     14
#define JV_STAGE_DISP_DSI     15
#define JV_STAGE_DISP_DRAWN   16
#define JV_STAGE_DISP_LCD     17
#define JV_STAGE_DISP_PLANE   18
#define JV_STAGE_DISP_SHOWN   19
#define JV_STAGE_DISP_LCD_OFF 20
#define JV_STAGE_DISP_PLANE_OFF 21
#define JV_STAGE_DISP_OFF     22
/* Loads, pass 2: savedata restored to the card, then memory */
#define JV_STAGE_LOAD_SAVEDATA 23
#define JV_STAGE_LOAD_MEMORY  24

/* Payload result codes (PayloadArguments.result) */
#define JV_TEST_DONE            1 /* a diagnostics run finished */
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
#define JV_ERR_NOT_MC        (-12) /* ux0 is not the official Memory Card  */
#define JV_ERR_TIMER         (-13) /* CPU cycle counter is not running     */
#define JV_ERR_HW            (-14) /* syscon / crypto engine did not answer */
#define JV_ERR_CRASH         (-15) /* the payload hit a CPU exception      */

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
/* Snapshot format. Snapshots must keep loading after a plugin update:
 * changing this requires the load path to still accept every older
 * version (tests/host/test_state.c fails the build otherwise).
 * A restored memory image also contains the plugin build that made the
 * snapshot, and that old plugin reads PayloadArguments after the load:
 * PayloadArguments is append-only (existing fields never move or change
 * meaning; the layout is pinned in tests/host/test_state.c). */
#define SAVESTATE_VERSION 2

#define SAVESTATE_MEDIAID_OFFSET 0x200
#define SAVESTATE_FILES_OFFSET   0x400

#define SAVEDATA_PREFIX "ux0:user/00/savedata/"

#define DRAM_VRAM_START 0x20000000u
#define DRAM_VRAM_END   0x60000000u
#define DRAM_START      0x40000000u
#define SPRAM_SIZE      (80 * 1024)

#endif
