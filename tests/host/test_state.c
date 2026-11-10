/* JamaisVu host test harness
 *
 * Runs the payload's save/load code (payload/state.c, guard.c, crc32.c,
 * FatFs) on a PC against an in-memory Memory Card image that holds a real
 * exFAT filesystem (made by mkfs.exfat), and checks:
 *
 *  - save only changes sectors that belong to the pre-allocated slot file
 *  - load restores savedata, memory and the mediaid sector exactly
 *  - load leaves everything else on the card alone
 *  - corrupted / malicious / foreign state files are rejected before a
 *    single sector or byte of memory is changed
 *  - the filesystem stays consistent (images are dumped for fsck.exfat)
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "main.h"
#include "state.h"
#include "guard.h"
#include "crc32.h"
#include "phys.h"
#include "ms.h"
#include "ff.h"

/* Compatibility guard: snapshots saved by earlier builds (format 2) must
 * still load after an update. If the format changes, keep a load path for
 * version 2 and extend this check instead of just bumping the number.
 * Format 4 is format 2 plus a checksum sector after the memory contents;
 * "old-format slot (version 2) still loads" below loads a format 2 slot. */
_Static_assert(SAVESTATE_VERSION == 4,
               "snapshot format changed: old slots must stay loadable");

/* PayloadArguments is read after a load by the plugin build *inside the
 * snapshot* (the restored memory), which may be older than the payload.
 * Fields may only be appended; these offsets must never change. */
#define JV_PIN(f, off) \
  _Static_assert(offsetof(PayloadArguments, f) == (off), \
                 "PayloadArguments." #f " moved: old snapshots would break")
JV_PIN(mode, 0);
JV_PIN(fattime, 4);
JV_PIN(process_num, 8);
JV_PIN(ttbs, 12);
JV_PIN(titleid, 144);
JV_PIN(rtc_ctx, 1168);
JV_PIN(path, 1616);
JV_PIN(slot, 1872);
JV_PIN(backup_savedata, 1876);
JV_PIN(slot_size, 1880);
JV_PIN(slot_nonce, 1884);
JV_PIN(ignore_flag_paddr, 1900);
JV_PIN(debug_mode, 1904);
JV_PIN(result, 1908);
JV_PIN(applied, 1912);
JV_PIN(stage, 1916);
JV_PIN(detail, 1920);
JV_PIN(soc_timer_10ms, 1924);
JV_PIN(trace_n, 1928);
JV_PIN(trace_stage, 1932);
JV_PIN(trace_ms, 1956);
JV_PIN(fault_type, 2052);
JV_PIN(fault_addr, 2056);
JV_PIN(fault_lr, 2060);
JV_PIN(trace_rel_lba, 2064);
JV_PIN(trace_nonce, 2068);
JV_PIN(disp_fb_paddr, 2084);
JV_PIN(disp_hw_flags0, 2104);
JV_PIN(disp_vic, 2108);
JV_PIN(disp_fb2_paddr, 2112);
JV_PIN(mem_bytes, 2120);
JV_PIN(icache_on, 2128);
JV_PIN(delay_unit_ns, 2132);
JV_PIN(disp_type, 2136);
JV_PIN(appdb_changed, 2140);
JV_PIN(ttbcr_n, 2144);
JV_PIN(cpu_mhz, 2148);
JV_PIN(refuse, 2152);
JV_PIN(write_sectors, 2156);
JV_PIN(verify_bad, 2160);
JV_PIN(verify_screen, 2164);
JV_PIN(verify_first, 2168);
JV_PIN(verify_ms, 2172);
JV_PIN(savedata_files, 2176);
JV_PIN(savedata_rewritten, 2184);
JV_PIN(data_sum_state, 2188);
JV_PIN(data_sum_ms, 2192);
JV_PIN(savedata_repaired, 2196);
JV_PIN(save_unmapped, 2200);
_Static_assert(sizeof(PayloadArguments) >= 2084,
               "PayloadArguments shrank: old snapshots would break");

/* ------------------------------------------------------------------------ */
/* Fake hardware                                                            */
/* ------------------------------------------------------------------------ */

#define PART_START   0x800u
#define MEDIAID_SEC  0x400u

static uint8_t *card;
static uint32_t card_sectors;
static uint8_t *card_before;

static int fail_writes_after = -1; /* fault injection */
static int writes_done = 0;
/* the card writes of a run, in order (power-cut test) */
#define WLOG_N 4096
static struct { uint32_t sector, count; } wlog[WLOG_N];

int ms_read_sector(void *buf, int sector, int count) {
  if (sector < 0 || count < 0 || (uint32_t)sector + count > card_sectors)
    return -1;
  memcpy(buf, card + (size_t)sector * 512, (size_t)count * 512);
  return 0;
}

int ms_write_sector(const void *buf, int sector, int count) {
  if (sector < 0 || count < 0 || (uint32_t)sector + count > card_sectors)
    return -1;
  if (fail_writes_after >= 0 && writes_done >= fail_writes_after)
    return -1;
  if (writes_done < WLOG_N) {
    wlog[writes_done].sector = (uint32_t)sector;
    wlog[writes_done].count = (uint32_t)count;
  }
  writes_done++;
  memcpy(card + (size_t)sector * 512, buf, (size_t)count * 512);
  return 0;
}

uint64_t ksceKernelGetSystemTimeWide(void) { return 0; }
void stage_mark(int n) { (void)n; }
uint32_t jv_time_us(void) { return 0; }
void progress_total(uint32_t total) { (void)total; }
void progress_at(uint32_t pos) { (void)pos; }
void progress_load_begin(uint32_t total) { (void)total; }
void progress_load_at(uint32_t paddr_end, uint32_t done) {
  (void)paddr_end;
  (void)done;
}
void kscePervasiveMsifSetClock(int clock) { (void)clock; }

DWORD get_fattime(void) {
  return ((2026 - 1980) << 25) | (10 << 21) | (31 << 16) | (12 << 11);
}

/* Fake physical memory: a few regions at the physical addresses the
 * payload would see. */
typedef struct {
  uint32_t pa;
  uint32_t size;
  uint8_t *mem;
} Region;

#define TTB_PA      0x50000000u
#define L2_PA       0x50004000u
#define FLAG_PA     0x40000100u

/* DRAM from 0x40204000 to 0x40300000 is RAM that no translation table
 * maps (memory handed to the codec engine is like that): fill_pattern
 * leaves it alone, the save_unmapped test fills it itself */
#define GAP_PA      0x40204000u
#define GAP_END     0x40300000u

static Region regions[] = {
  { 0x40000000u, 0x310000, NULL }, /* two 1MB sections + 4 small pages
                                      (adjacent, so coalesced), the gap,
                                      one large page via L2 */
  { 0x20000000u, 0x100000, NULL }, /* VRAM section                    */
  { TTB_PA,      0x5000,   NULL }, /* L1 + L2 tables (not mapped)     */
  { CONTEXT_PADDR, sizeof(SceSysconResumeContext), NULL },
  { SPRAM_PADDR, SPRAM_SIZE, NULL },
};
#define N_REGIONS (sizeof(regions) / sizeof(regions[0]))

void *jv_phys(uint32_t pa, uint32_t size) {
  for (size_t i = 0; i < N_REGIONS; i++) {
    if (pa >= regions[i].pa &&
        (uint64_t)pa + size <= (uint64_t)regions[i].pa + regions[i].size)
      return regions[i].mem + (pa - regions[i].pa);
  }
  return NULL;
}

static PayloadArguments args;
PayloadArguments *pargs = &args;

static uint8_t g_nonce[16];

/* ------------------------------------------------------------------------ */
/* Helpers                                                                  */
/* ------------------------------------------------------------------------ */

static int failures = 0;
static int checks = 0;

#define CHECK(cond, ...)                                    \
  do {                                                      \
    checks++;                                               \
    if (!(cond)) {                                          \
      failures++;                                           \
      printf("  FAIL %s:%d: ", __FILE__, __LINE__);         \
      printf(__VA_ARGS__);                                  \
      printf("\n");                                         \
    }                                                       \
  } while (0)

static void fill_pattern(uint32_t seed) {
  for (size_t i = 0; i < N_REGIONS; i++) {
    if (regions[i].pa == TTB_PA)
      continue;
    uint32_t x = seed ^ regions[i].pa;
    for (uint32_t j = 0; j < regions[i].size; j++) {
      x = x * 1103515245u + 12345u;
      if (regions[i].pa + j >= GAP_PA && regions[i].pa + j < GAP_END)
        continue; /* not mapped */
      regions[i].mem[j] = x >> 16;
    }
  }
}

static void setup_page_tables(void) {
  uint32_t *l1 = jv_phys(TTB_PA, 0x4000);
  uint32_t *l2 = jv_phys(L2_PA, 0x400);
  memset(l1, 0, 0x4000);
  memset(l2, 0, 0x400);

  l1[0x400] = 0x40000000u | 0b10;          /* section 0x40000000 */
  l1[0x401] = 0x40100000u | 0b10;          /* section 0x40100000 */
  l1[0x200] = 0x20000000u | 0b10;          /* section 0x20000000 */
  l1[0x402] = L2_PA | 0b01;                /* L2 for VA 0x40200000 */
  l1[0x010] = 0x10000000u | 0b10;          /* outside DRAM: ignored */

  for (int i = 0; i < 4; i++)
    l2[i] = (0x40200000u + i * 0x1000) | 0b10;  /* small pages */
  for (int i = 0; i < 16; i++)
    l2[0x40 + i] = 0x40300000u | 0b01;          /* large page */
}

static void snapshot_card(void) {
  memcpy(card_before, card, (size_t)card_sectors * 512);
}

typedef struct {
  SectorRange r[4096];
  int n;
} RangeSet;

static int in_ranges(const RangeSet *set, uint32_t s) {
  for (int i = 0; i < set->n; i++)
    if (s >= set->r[i].start && s < set->r[i].start + set->r[i].count)
      return 1;
  return 0;
}

/* Number of changed sectors outside the allowed set (NULL = none allowed) */
static uint32_t changed_outside(const RangeSet *allowed, uint32_t *total) {
  uint32_t bad = 0, all = 0;
  for (uint32_t s = 0; s < card_sectors; s++) {
    if (memcmp(card + (size_t)s * 512, card_before + (size_t)s * 512, 512)) {
      all++;
      if (!allowed || !in_ranges(allowed, s))
        bad++;
    }
  }
  if (total)
    *total = all;
  return bad;
}

static void dump_partition(const char *name) {
  char path[256];
  snprintf(path, sizeof(path), "%s", name);
  FILE *f = fopen(path, "wb");
  fwrite(card + (size_t)PART_START * 512, 512, card_sectors - PART_START, f);
  fclose(f);
}

static void allow_everything(void) {
  guard_deny_all();
  guard_allow(0, card_sectors);
}

static FATFS fs;

static void mount(void) {
  FRESULT r = f_mount(&fs, "ux0:", 1);
  if (r != FR_OK) {
    printf("f_mount failed: %d\n", r);
    exit(2);
  }
}

static void unmount(void) {
  f_unmount("ux0:");
}

static void write_file(const char *path, const void *data, uint32_t len) {
  FIL f;
  UINT bw;
  CHECK(f_open(&f, path, FA_CREATE_ALWAYS | FA_WRITE) == FR_OK, "open %s", path);
  CHECK(f_write(&f, data, len, &bw) == FR_OK && bw == len, "write %s", path);
  CHECK(f_close(&f) == FR_OK, "close %s", path);
}

static int read_file(const char *path, void *data, uint32_t max, uint32_t *len) {
  FIL f;
  UINT br;
  if (f_open(&f, path, FA_READ) != FR_OK)
    return -1;
  f_read(&f, data, max, &br);
  *len = br;
  f_close(&f);
  return 0;
}

/* What the plugin does right before a save: write a fresh token at the
 * start of the slot file through the OS (here: FatFs, guard open). */
static void stamp_slot(const char *path) {
  static uint32_t ctr = 1;
  FIL f;
  UINT bw;
  uint8_t sector[512] = { 0 };
  for (int i = 0; i < 16; i++)
    g_nonce[i] = (uint8_t)(ctr * 37 + i * 11);
  ctr++;
  memcpy(sector, g_nonce, 16);
  allow_everything();
  mount();
  CHECK(f_open(&f, path, FA_WRITE) == FR_OK, "stamp open %s", path);
  f_write(&f, sector, sizeof(sector), &bw);
  f_close(&f);
  f_unmount("ux0:");
  guard_deny_all();
}

static void get_file_extents(const char *path, RangeSet *set) {
  static DWORD tbl[8192];
  FIL f;
  set->n = 0;
  CHECK(f_open(&f, path, FA_READ) == FR_OK, "open %s", path);
  tbl[0] = 8192;
  f.cltbl = tbl;
  CHECK(f_lseek(&f, CREATE_LINKMAP) == FR_OK, "linkmap");
  for (DWORD *t = &tbl[1]; *t; t += 2) {
    set->r[set->n].start = fs.database + (t[1] - 2) * fs.csize;
    set->r[set->n].count = t[0] * fs.csize;
    set->n++;
  }
  f.cltbl = NULL;
  f_close(&f);
}

/* Create the slot file the way the plugin does (outside the payload),
 * interleaved with filler files so that it ends up fragmented. */
static void create_slot(const char *path, uint32_t size, int fragment) {
  static uint8_t zero[65536];
  FIL f;
  UINT bw;
  char name[64];

  f_mkdir("ux0:savestate");

  CHECK(f_open(&f, path, FA_CREATE_ALWAYS | FA_WRITE) == FR_OK, "slot open");

  if (fragment) {
    /* grow the slot and a filler file alternately so their clusters
     * interleave, then delete the filler */
    FIL g;
    snprintf(name, sizeof(name), "ux0:filler.bin");
    CHECK(f_open(&g, name, FA_CREATE_ALWAYS | FA_WRITE) == FR_OK, "filler");
    for (uint32_t done = 0; done < size; done += sizeof(zero)) {
      uint32_t n = size - done < sizeof(zero) ? size - done : sizeof(zero);
      f_write(&f, zero, n, &bw);
      f_sync(&f);
      if (done < 16 * sizeof(zero)) {
        f_write(&g, zero, 4096, &bw);
        f_sync(&g);
      }
    }
    f_close(&g);
    f_unlink(name);
  } else {
    for (uint32_t done = 0; done < size; done += sizeof(zero)) {
      uint32_t n = size - done < sizeof(zero) ? size - done : sizeof(zero);
      f_write(&f, zero, n, &bw);
    }
  }
  f_close(&f);
}

static void make_card(const char *part_img) {
  FILE *f = fopen(part_img, "rb");
  if (!f) {
    perror(part_img);
    exit(2);
  }
  fseek(f, 0, SEEK_END);
  long part_size = ftell(f);
  fseek(f, 0, SEEK_SET);

  card_sectors = PART_START + part_size / 512;
  card = calloc(card_sectors, 512);
  card_before = calloc(card_sectors, 512);
  fread(card + (size_t)PART_START * 512, 1, part_size, f);
  fclose(f);

  /* Sony master block with a mediaid partition and the ux0 partition,
   * plus a standard MBR partition entry for FatFs. */
  master_block_t *mbr = (master_block_t *)card;
  memcpy(mbr->magic, "Sony Computer Entertainment Inc.", 32);
  mbr->version = 3;
  mbr->device_size = card_sectors;
  mbr->partitions[0].off = MEDIAID_SEC;
  mbr->partitions[0].sz = 0x10;
  mbr->partitions[0].code = 0xd;
  mbr->partitions[1].off = PART_START;
  mbr->partitions[1].sz = card_sectors - PART_START;
  mbr->partitions[1].code = 0x8;
  mbr->partitions[1].type = 0x7;
  uint8_t *pte = card + 446;
  pte[4] = 0x07;
  pte[8] = PART_START & 0xff;
  pte[9] = (PART_START >> 8) & 0xff;
  uint32_t n = card_sectors - PART_START;
  memcpy(pte + 12, &n, 4);
  mbr->sig = 0xaa55;

  for (int i = 0; i < 512; i++)
    card[MEDIAID_SEC * 512 + i] = (uint8_t)(i * 7 + 1);
}

static void reset_args(int mode, const char *path, uint32_t slot_size) {
  memset(&args, 0, sizeof(args));
  memcpy(args.slot_nonce, g_nonce, sizeof(g_nonce));
  args.mode = mode;
  args.process_num = 2;
  strcpy(args.titleid[0], "PCSE00123");
  strcpy(args.titleid[1], "NPXS10015"); /* system app: skipped */
  args.ttbs[0] = 0;
  args.ttbs[1] = 0;
  args.ttbs[2] = TTB_PA;               /* kernel TTB */
  strcpy(args.path, path);
  args.slot_size = slot_size;
  args.ignore_flag_paddr = FLAG_PA;
  args.backup_savedata = JV_SAVEDATA_SYSTEM | JV_SAVEDATA_RESAVEDATA;
}

static uint8_t *ram_copy(void) {
  size_t total = 0;
  for (size_t i = 0; i < N_REGIONS; i++)
    total += regions[i].size;
  uint8_t *c = malloc(total), *p = c;
  for (size_t i = 0; i < N_REGIONS; i++) {
    memcpy(p, regions[i].mem, regions[i].size);
    p += regions[i].size;
  }
  return c;
}

static int ram_equal(const uint8_t *c) {
  const uint8_t *p = c;
  for (size_t i = 0; i < N_REGIONS; i++) {
    if (memcmp(p, regions[i].mem, regions[i].size))
      return 0;
    p += regions[i].size;
  }
  return 1;
}

/* Load the state file's raw bytes (via FatFs, outside of the payload). */
static uint8_t *read_state(const char *path, uint32_t *len) {
  static uint8_t buf[16 * 1024 * 1024];
  allow_everything();
  mount();
  CHECK(read_file(path, buf, sizeof(buf), len) == 0, "read state");
  unmount();
  guard_deny_all();
  return buf;
}

static void write_state(const char *path, const uint8_t *data, uint32_t len) {
  allow_everything();
  mount();
  /* overwrite in place to keep the same clusters */
  FIL f;
  UINT bw;
  CHECK(f_open(&f, path, FA_WRITE) == FR_OK, "open state rw");
  f_write(&f, data, len, &bw);
  f_close(&f);
  unmount();
  guard_deny_all();
}

static void refresh_crcs(uint8_t *st) {
  SaveStateHeader *h = (SaveStateHeader *)st;
  h->body_crc = crc32_update(0, st + 0x200, h->data_offset - 0x200);
  h->header_crc = crc32_update(0, h, offsetof(SaveStateHeader, header_crc));
}

/* Runs load on a tampered copy of the state and expects a clean refusal:
 * no sector changed, no memory changed, applied == 0. */
static void expect_rejected(const char *what, const char *path,
                            const uint8_t *orig, uint32_t len,
                            void (*tamper)(uint8_t *), int expect) {
  static uint8_t tmp[16 * 1024 * 1024];
  memcpy(tmp, orig, len);
  tamper(tmp);
  write_state(path, tmp, len);

  uint8_t *ram = ram_copy();
  snapshot_card();
  reset_args(JAMAISVU_MODE_LOAD, path, 0);
  mount();
  int res = load_state(&fs, path);
  unmount();

  printf("  %-44s -> %d\n", what, res);
  CHECK(res == expect, "%s: expected %d got %d", what, expect, res);
  CHECK(args.applied == 0, "%s: applied must be 0", what);
  CHECK(changed_outside(NULL, NULL) == 0, "%s: card modified", what);
  CHECK(ram_equal(ram), "%s: memory modified", what);
  free(ram);

  write_state(path, orig, len); /* restore */
}

static SaveStateFile *first_file_record(uint8_t *st) {
  return (SaveStateFile *)(st + SAVESTATE_FILES_OFFSET);
}

static void t_flip_body(uint8_t *st) { st[0x500] ^= 0x01; }
static void t_flip_header(uint8_t *st) { st[0x10] ^= 0x01; }
static void t_zero_header(uint8_t *st) { memset(st, 0, 512); }
static void t_old_version(uint8_t *st) {
  ((SaveStateHeader *)st)->version = 1;
  refresh_crcs(st);
}
static void t_traversal(uint8_t *st) {
  SaveStateFile *f = first_file_record(st);
  char *p = (char *)(f + 1);
  /* "ux0:user/00/savedata/PCSE00123" -> same length, escapes the prefix */
  memcpy(p, "ux0:user/00/savedata/../../../x", 30);
  (void)f;
  refresh_crcs(st);
}
static void t_other_prefix(uint8_t *st) {
  SaveStateFile *f = first_file_record(st);
  char *p = (char *)(f + 1);
  memcpy(p, "ux0:app/", 8);
  refresh_crcs(st);
}
static void t_bad_segment(uint8_t *st) {
  SaveStateHeader *h = (SaveStateHeader *)st;
  Segment *seg = (Segment *)(st + h->segtab_offset);
  seg[0].addr = 0x1c000000; /* the payload itself */
  refresh_crcs(st);
}
static void t_seg_overflow(uint8_t *st) {
  SaveStateHeader *h = (SaveStateHeader *)st;
  Segment *seg = (Segment *)(st + h->segtab_offset);
  seg[0].addr = 0x5ff00000;
  seg[0].size = 0x00200000; /* runs past the end of DRAM */
  refresh_crcs(st);
}
static void t_flag_outside(uint8_t *st) {
  ((SaveStateHeader *)st)->ignore_flag_paddr = 0x40500000;
  refresh_crcs(st);
}
static void t_too_many_files(uint8_t *st) {
  ((SaveStateHeader *)st)->file_num += 1;
  refresh_crcs(st);
}
static void t_huge_segs(uint8_t *st) {
  ((SaveStateHeader *)st)->segs_num = 0x7fffffff;
  refresh_crcs(st);
}
static void t_path_len(uint8_t *st) {
  first_file_record(st)->path_len = 4096;
  refresh_crcs(st);
}

static void t_none(uint8_t *st) { (void)st; }
static SaveStateSum *sum_sector(uint8_t *st) {
  SaveStateHeader *h = (SaveStateHeader *)st;
  return (SaveStateSum *)(st + ALIGN(h->data_offset + h->data_size, 512));
}
static void t_sum_flip(uint8_t *st) { sum_sector(st)->s1 ^= 1; }
static void t_sum_zero(uint8_t *st) { memset(sum_sector(st), 0, 512); }

/* ------------------------------------------------------------------------ */

int main(int argc, char **argv) {
  const char *slot = "ux0:savestate/slot1.bin";
  const uint32_t slot_size = 8 * 1024 * 1024;
  uint8_t sav_a[3000], sav_b[2000], sub_a[700], buf[8192];
  uint8_t sav_x[3000]; /* same size as sav_a, other content */
  RangeSet same_ext, same_ext_after;
  uint32_t len, total;
  RangeSet ext;
  int res;

  setvbuf(stdout, NULL, _IONBF, 0);
  if (argc < 3) {
    printf("usage: %s part.img outdir\n", argv[0]);
    return 2;
  }
  const char *outdir = argv[2];
  char out[512];

  for (size_t i = 0; i < N_REGIONS; i++)
    regions[i].mem = calloc(1, regions[i].size);

  make_card(argv[1]);
  for (size_t i = 0; i < sizeof(sav_a); i++) sav_a[i] = i * 3;
  for (size_t i = 0; i < sizeof(sav_b); i++) sav_b[i] = i * 5 + 1;
  for (size_t i = 0; i < sizeof(sub_a); i++) sub_a[i] = i ^ 0x5a;
  for (size_t i = 0; i < sizeof(sav_x); i++) sav_x[i] = (uint8_t)~sav_a[i];

  /* --- Setup (what the OS / plugin would do) -------------------------- */
  printf("setup\n");
  allow_everything();
  mount();
  f_mkdir("ux0:user");
  f_mkdir("ux0:user/00");
  f_mkdir("ux0:user/00/savedata");
  f_mkdir("ux0:user/00/savedata/PCSE00123");
  f_mkdir("ux0:user/00/savedata/PCSE00123/sce_sys");
  f_mkdir("ux0:user/00/savedata/PCSE00999");
  write_file("ux0:user/00/savedata/PCSE00123/data.bin", sav_a, sizeof(sav_a));
  write_file("ux0:user/00/savedata/PCSE00123/same.bin", sav_a, sizeof(sav_a));
  write_file("ux0:user/00/savedata/PCSE00123/sce_sys/param.sfo", sub_a, sizeof(sub_a));
  write_file("ux0:user/00/savedata/PCSE00999/other.bin", sav_b, sizeof(sav_b));
  /* reSavedata's folder (the plugin redirects games there) */
  f_mkdir("ux0:data");
  f_mkdir("ux0:resavedata");
  f_mkdir("ux0:resavedata/PCSE00123");
  write_file("ux0:resavedata/PCSE00123/save.dat", sav_b, sizeof(sav_b));
  write_file("ux0:other.txt", "untouched", 9);
  create_slot(slot, slot_size, 1);
  get_file_extents(slot, &ext);
  printf("  slot file has %d fragment(s), cluster = %u sectors\n", ext.n, fs.csize);
  CHECK(ext.n > 1, "slot should be fragmented for this test");
  unmount();
  guard_deny_all();
  snprintf(out, sizeof(out), "%s/01_setup.img", outdir);
  dump_partition(out);

  setup_page_tables();
  fill_pattern(0xaaaa);
  uint8_t *ram_a = ram_copy();

  /* --- Save ----------------------------------------------------------- */
  printf("save\n");
  stamp_slot(slot);
  snapshot_card();
  reset_args(JAMAISVU_MODE_SAVE, slot, slot_size);
  mount();
  res = save_state(&fs, slot);
  unmount();
  printf("  save_state -> %d, guard violations %d\n", res, guard_violations());
  CHECK(res == JV_OK, "save failed: %d", res);
  CHECK(guard_violations() == 0, "guard violations during save");
  CHECK(changed_outside(&ext, &total) == 0,
        "save changed sectors outside the slot file");
  printf("  %u sectors changed, all inside the slot file\n", total);
  CHECK(ram_equal(ram_a), "save modified memory");
  snprintf(out, sizeof(out), "%s/02_after_save.img", outdir);
  dump_partition(out);

  uint8_t *state_copy = malloc(slot_size);
  uint8_t *st = read_state(slot, &len);
  memcpy(state_copy, st, len);
  CHECK(len == slot_size, "state length");
  CHECK(args.savedata_files == ((SaveStateHeader *)st)->file_num &&
        args.savedata_files > 0, "savedata count reported to the plugin");
  CHECK(args.savedata_folders == (JV_SAVEDATA_SYSTEM | JV_SAVEDATA_RESAVEDATA),
        "savedata folders reported to the plugin");
  CHECK(((SaveStateHeader *)st)->version == 4 &&
        sum_sector(st)->magic == JV_SUM_MAGIC &&
        args.data_sum_state == JV_SUM_OK,
        "save: checksum sector written and reported");

  /* --- Change everything ---------------------------------------------- */
  printf("modify card + memory\n");
  allow_everything();
  mount();
  write_file("ux0:user/00/savedata/PCSE00123/data.bin", sav_b, sizeof(sav_b));
  write_file("ux0:user/00/savedata/PCSE00123/new_since_save.bin", sav_a, 1000);
  f_unlink("ux0:user/00/savedata/PCSE00123/sce_sys/param.sfo");
  /* same size, other content: must come back in place (same clusters) */
  write_file("ux0:user/00/savedata/PCSE00123/same.bin", sav_x, sizeof(sav_x));
  get_file_extents("ux0:user/00/savedata/PCSE00123/same.bin", &same_ext);
  write_file("ux0:resavedata/PCSE00123/save.dat", sav_a, sizeof(sav_a));
  write_file("ux0:resavedata/PCSE00123/newer.dat", sav_a, 500);
  write_file("ux0:other.txt", "changed after save", 18);
  unmount();
  card[MEDIAID_SEC * 512 + 3] ^= 0xff;
  guard_deny_all();
  fill_pattern(0xbbbb);

  /* --- Load ----------------------------------------------------------- */
  printf("load\n");
  snapshot_card();
  reset_args(JAMAISVU_MODE_LOAD, slot, 0);
  mount();
  res = load_state(&fs, slot);
  unmount();
  printf("  load_state -> %d (applied=%u)\n", res, args.applied);
  CHECK(res == JV_OK, "load failed: %d", res);
  CHECK(args.data_sum_state == JV_SUM_OK,
        "load: the restored memory was not checked against the checksum");

  /* memory = snapshot, except the write-block flag which is now armed */
  CHECK(*(uint32_t *)jv_phys(FLAG_PA, 4) == 1, "write-block flag not armed");
  memcpy(ram_a + (FLAG_PA - 0x40000000u), "\x01\x00\x00\x00", 4);
  CHECK(ram_equal(ram_a), "memory not restored");

  CHECK(card[MEDIAID_SEC * 512 + 3] == (uint8_t)(3 * 7 + 1), "mediaid not restored");
  CHECK(memcmp(card, card_before, 512) == 0, "MBR modified");

  allow_everything();
  mount();
  CHECK(read_file("ux0:user/00/savedata/PCSE00123/data.bin", buf, sizeof(buf), &len) == 0 &&
        len == sizeof(sav_a) && memcmp(buf, sav_a, len) == 0, "savedata not restored");
  CHECK(read_file("ux0:user/00/savedata/PCSE00123/sce_sys/param.sfo", buf, sizeof(buf), &len) == 0 &&
        len == sizeof(sub_a) && memcmp(buf, sub_a, len) == 0, "deleted savedata not restored");
  CHECK(read_file("ux0:user/00/savedata/PCSE00123/new_since_save.bin", buf, sizeof(buf), &len) != 0,
        "file created after save should be gone");
  CHECK(read_file("ux0:user/00/savedata/PCSE00123/same.bin", buf, sizeof(buf), &len) == 0 &&
        len == sizeof(sav_a) && memcmp(buf, sav_a, len) == 0, "same-size file not restored");
  /* data.bin (other size), same.bin (other content), param.sfo (gone),
   * resavedata's save.dat (other content): rewritten; the rest untouched */
  CHECK(args.savedata_rewritten == 4, "rewritten count %u, expected 4",
        (unsigned)args.savedata_rewritten);
  get_file_extents("ux0:user/00/savedata/PCSE00123/same.bin", &same_ext_after);
  CHECK(same_ext_after.n == same_ext.n &&
        memcmp(same_ext_after.r, same_ext.r, same_ext.n * sizeof(same_ext.r[0])) == 0,
        "same-size file must be restored in place (same clusters)");
  CHECK(read_file("ux0:user/00/savedata/PCSE00999/other.bin", buf, sizeof(buf), &len) == 0 &&
        len == sizeof(sav_b) && memcmp(buf, sav_b, len) == 0, "other title's savedata touched");
  CHECK(read_file("ux0:resavedata/PCSE00123/save.dat", buf, sizeof(buf), &len) == 0 &&
        len == sizeof(sav_b) && memcmp(buf, sav_b, len) == 0, "reSavedata savedata not restored");
  CHECK(read_file("ux0:resavedata/PCSE00123/newer.dat", buf, sizeof(buf), &len) != 0,
        "reSavedata file created after save should be gone");
  CHECK(read_file("ux0:other.txt", buf, sizeof(buf), &len) == 0 && len == 18 &&
        memcmp(buf, "changed after save", 18) == 0, "unrelated file touched");
  unmount();
  guard_deny_all();
  snprintf(out, sizeof(out), "%s/03_after_load.img", outdir);
  dump_partition(out);

  /* sectors outside the exFAT area (MBR, mediaid partition except its
   * sector, boot region) must be untouched by load, apart from mediaid */
  {
    uint32_t bad = 0;
    for (uint32_t s = 0; s < PART_START + 24; s++) {
      if (s == MEDIAID_SEC)
        continue;
      if (memcmp(card + (size_t)s * 512, card_before + (size_t)s * 512, 512))
        bad++;
    }
    CHECK(bad == 0, "load wrote %u sectors outside the allowed area", bad);
  }

  /* --- Rejection tests -------------------------------------------------- */
  printf("rejection tests (nothing may be written)\n");
  fill_pattern(0xcccc);
  expect_rejected("flipped body byte", slot, state_copy, slot_size, t_flip_body, JV_ERR_BAD_STATE);
  expect_rejected("flipped header byte", slot, state_copy, slot_size, t_flip_header, JV_ERR_BAD_STATE);
  expect_rejected("interrupted save (no header)", slot, state_copy, slot_size, t_zero_header, JV_ERR_BAD_STATE);
  expect_rejected("old format version", slot, state_copy, slot_size, t_old_version, JV_ERR_BAD_STATE);
  expect_rejected("path traversal (valid CRC)", slot, state_copy, slot_size, t_traversal, JV_ERR_BAD_STATE);
  expect_rejected("path outside savedata (valid CRC)", slot, state_copy, slot_size, t_other_prefix, JV_ERR_BAD_STATE);
  expect_rejected("segment over payload (valid CRC)", slot, state_copy, slot_size, t_bad_segment, JV_ERR_BAD_STATE);
  expect_rejected("segment past DRAM end (valid CRC)", slot, state_copy, slot_size, t_seg_overflow, JV_ERR_BAD_STATE);
  expect_rejected("write-block flag not restored", slot, state_copy, slot_size, t_flag_outside, JV_ERR_BAD_STATE);
  expect_rejected("file count mismatch", slot, state_copy, slot_size, t_too_many_files, JV_ERR_BAD_STATE);
  expect_rejected("huge segment count", slot, state_copy, slot_size, t_huge_segs, JV_ERR_BAD_STATE);
  expect_rejected("oversized path length", slot, state_copy, slot_size, t_path_len, JV_ERR_BAD_STATE);

  /* different card: change a byte in the partition table area */
  card[0x60] ^= 0x01;
  expect_rejected("state from another card", slot, state_copy, slot_size, t_none, JV_ERR_OTHER_CARD);
  card[0x60] ^= 0x01;

  /* missing mediaid partition: must never fall back to sector 0 */
  ((master_block_t *)card)->partitions[0].code = 0;
  expect_rejected("no mediaid partition", slot, state_copy, slot_size, t_none, JV_ERR_MEDIAID);
  ((master_block_t *)card)->partitions[0].code = 0xd;

  /* --- Memory that no translation table maps ----------------------------- */
  printf("memory between the mapped pages\n");
  {
    uint8_t *gap = jv_phys(GAP_PA, GAP_END - GAP_PA);
    uint8_t *keep = malloc(slot_size);
    int same = 1;

    /* as saved so far: the mapped pages only - what lies between them
     * is not in the snapshot and stays as it is at the load */
    memset(gap, 0x5a, GAP_END - GAP_PA);
    reset_args(JAMAISVU_MODE_LOAD, slot, 0);
    mount();
    res = load_state(&fs, slot);
    unmount();
    for (uint32_t i = 0; i < GAP_END - GAP_PA; i += 4096)
      same = same && gap[i] == 0x5a;
    printf("  %-44s -> %d\n", "mapped pages only: the gap is left as it is", res);
    CHECK(res == JV_OK && same, "mapped-only load touched unmapped memory");

    /* save_unmapped: the gap is in the snapshot and comes back */
    memcpy(keep, state_copy, slot_size);
    for (uint32_t i = 0; i < GAP_END - GAP_PA; i++)
      gap[i] = (uint8_t)(i * 7 + (i >> 12));
    stamp_slot(slot);
    snapshot_card();
    reset_args(JAMAISVU_MODE_SAVE, slot, slot_size);
    args.save_unmapped = 1;
    mount();
    res = save_state(&fs, slot);
    unmount();
    CHECK(res == JV_OK && changed_outside(&ext, NULL) == 0,
          "save with the unmapped memory: %d", res);
    memset(gap, 0xa5, GAP_END - GAP_PA);
    fill_pattern(0x1234);
    reset_args(JAMAISVU_MODE_LOAD, slot, 0);
    mount();
    res = load_state(&fs, slot);
    unmount();
    same = 1;
    for (uint32_t i = 0; i < GAP_END - GAP_PA; i++)
      same = same && gap[i] == (uint8_t)(i * 7 + (i >> 12));
    printf("  %-44s -> %d\n", "with the gap: it comes back", res);
    CHECK(res == JV_OK && same && args.data_sum_state == JV_SUM_OK,
          "unmapped memory not restored (res %d)", res);

    /* back to the state the following tests expect */
    memset(gap, 0, GAP_END - GAP_PA);
    write_state(slot, keep, slot_size);
    fill_pattern(0xcccc);
    free(keep);
  }

  /* --- Memory image checksum (format 4) ---------------------------------- */
  printf("memory image checksum\n");
  expect_rejected("damaged checksum sector", slot, state_copy, slot_size, t_sum_flip, JV_ERR_BAD_STATE);
  expect_rejected("missing checksum sector", slot, state_copy, slot_size, t_sum_zero, JV_ERR_BAD_STATE);
  {
    uint8_t *tmp = malloc(slot_size);
    SaveStateHeader *h = (SaveStateHeader *)tmp;

    /* a sampled word of the memory contents differs on the card: found
     * once the memory is back, and the payload must then stop (applied) */
    memcpy(tmp, state_copy, slot_size);
    tmp[h->data_offset + 5 * JV_SUM_STRIDE] ^= 0x01;
    write_state(slot, tmp, slot_size);
    reset_args(JAMAISVU_MODE_LOAD, slot, 0);
    mount();
    res = load_state(&fs, slot);
    unmount();
    printf("  %-44s -> %d\n", "memory contents changed (sampled word)", res);
    CHECK(res == JV_ERR_CHECKSUM && args.applied == 1 &&
          args.data_sum_state == JV_SUM_MISMATCH,
          "changed memory contents: res %d applied %u state %u", res,
          (unsigned)args.applied, (unsigned)args.data_sum_state);

    /* a whole sector of the memory contents replaced by another one */
    memcpy(tmp, state_copy, slot_size);
    memcpy(tmp + h->data_offset + 512 * 9, tmp + h->data_offset + 512 * 3, 512);
    if (memcmp(tmp, state_copy, slot_size)) {
      write_state(slot, tmp, slot_size);
      reset_args(JAMAISVU_MODE_LOAD, slot, 0);
      mount();
      res = load_state(&fs, slot);
      unmount();
      printf("  %-44s -> %d\n", "memory contents: a misplaced sector", res);
      CHECK(res == JV_ERR_CHECKSUM, "misplaced sector: res %d", res);
    }

    /* an old-format slot (version 2: no checksum sector) still loads */
    memcpy(tmp, state_copy, slot_size);
    h->version = 2;
    refresh_crcs(tmp);
    memset(sum_sector(tmp), 0, 512);
    write_state(slot, tmp, slot_size);
    fill_pattern(0xdddd);
    reset_args(JAMAISVU_MODE_LOAD, slot, 0);
    mount();
    res = load_state(&fs, slot);
    unmount();
    printf("  %-44s -> %d\n", "old-format slot (version 2) still loads", res);
    CHECK(res == JV_OK && args.data_sum_state == JV_SUM_NONE,
          "old-format slot: res %d state %u", res,
          (unsigned)args.data_sum_state);
    CHECK(ram_equal(ram_a), "old-format slot: memory not restored");

    write_state(slot, state_copy, slot_size); /* restore */
    free(tmp);
  }
  /* --- Save failure modes ------------------------------------------------ */
  printf("save failure modes\n");
  {
    /* slot file whose token does not match (e.g. ux0 is an SD2Vita and a
     * same-named file exists on the Memory Card) */
    stamp_slot(slot);
    snapshot_card();
    reset_args(JAMAISVU_MODE_SAVE, slot, slot_size);
    args.slot_nonce[0] ^= 0xff;
    mount();
    res = save_state(&fs, slot);
    unmount();
    printf("  %-44s -> %d\n", "slot token mismatch (not this card)", res);
    CHECK(res == JV_ERR_NOT_MC, "token mismatch: %d", res);
    CHECK(changed_outside(NULL, NULL) == 0, "token mismatch wrote to the card");

    snapshot_card();
    reset_args(JAMAISVU_MODE_SAVE, slot, slot_size + 512);
    mount();
    res = save_state(&fs, slot);
    unmount();
    printf("  %-44s -> %d\n", "slot size mismatch", res);
    CHECK(res == JV_ERR_NO_SLOT, "size mismatch: %d", res);
    CHECK(changed_outside(NULL, NULL) == 0, "size mismatch wrote to the card");

    snapshot_card();
    reset_args(JAMAISVU_MODE_SAVE, "ux0:savestate/missing.bin", slot_size);
    mount();
    res = save_state(&fs, "ux0:savestate/missing.bin");
    unmount();
    printf("  %-44s -> %d\n", "missing slot file", res);
    CHECK(res == JV_ERR_NO_SLOT, "missing slot: %d", res);
    CHECK(changed_outside(NULL, NULL) == 0, "missing slot wrote to the card");

    /* slot too small for the memory image */
    const char *small = "ux0:savestate/small.bin";
    RangeSet sext;
    allow_everything();
    mount();
    create_slot(small, 1024 * 1024, 0);
    get_file_extents(small, &sext);
    unmount();
    guard_deny_all();
    stamp_slot(small);
    snapshot_card();
    reset_args(JAMAISVU_MODE_SAVE, small, 1024 * 1024);
    mount();
    res = save_state(&fs, small);
    unmount();
    printf("  %-44s -> %d\n", "slot too small", res);
    CHECK(res == JV_ERR_TOO_BIG, "too small: %d", res);
    CHECK(changed_outside(&sext, NULL) == 0, "too-small save wrote outside its slot");
    reset_args(JAMAISVU_MODE_LOAD, small, 0);
    mount();
    snapshot_card();
    res = load_state(&fs, small);
    unmount();
    printf("  %-44s -> %d\n", "load of the failed save", res);
    CHECK(res == JV_ERR_BAD_STATE && args.applied == 0 &&
          changed_outside(NULL, NULL) == 0, "failed save must not be loadable");

    /* card write error in the middle of a save */
    stamp_slot(slot);
    snapshot_card();
    reset_args(JAMAISVU_MODE_SAVE, slot, slot_size);
    writes_done = 0;
    fail_writes_after = 20;
    mount();
    res = save_state(&fs, slot);
    unmount();
    fail_writes_after = -1;
    printf("  %-44s -> %d\n", "write error during save", res);
    CHECK(res == JV_ERR_IO, "write error: %d", res);
    CHECK(changed_outside(&ext, NULL) == 0, "failed save wrote outside its slot");
    reset_args(JAMAISVU_MODE_LOAD, slot, 0);
    snapshot_card();
    mount();
    res = load_state(&fs, slot);
    unmount();
    printf("  %-44s -> %d\n", "load after interrupted save", res);
    CHECK(res == JV_ERR_BAD_STATE && args.applied == 0 &&
          changed_outside(NULL, NULL) == 0, "interrupted save must not be loadable");
  }

  /* --- Write test: a save, then the slot read back and compared --------- */
  printf("write test (save, read back, compare)\n");
  {
    stamp_slot(slot);
    snapshot_card();
    reset_args(JAMAISVU_MODE_SAVE, slot, slot_size);
    args.debug_mode = JV_TEST_VERIFY;
    args.write_sectors = 255;
    mount();
    res = save_state(&fs, slot);
    unmount();
    printf("  %-44s -> %d (%u words differ, %u ms)\n", "write test", res,
           args.verify_bad, args.verify_ms);
    CHECK(res == JV_TEST_DONE, "write test result: %d", res);
    CHECK(args.verify_bad == 0 && args.verify_screen == 0 &&
          args.verify_first == 0, "write test: read back differs");
    CHECK(changed_outside(&ext, NULL) == 0, "write test wrote outside its slot");
    /* the slot now holds a complete state: it must load like a save's */
    reset_args(JAMAISVU_MODE_LOAD, slot, 0);
    mount();
    res = load_state(&fs, slot);
    unmount();
    CHECK(res == JV_OK, "state written by the write test must load: %d", res);
  }

  /* --- Memory Card mounted as uma0 (ux0 = SD2Vita/PSVSD) ---------------- */
  printf("memory card as uma0 (no savedata backup)\n");
  {
    fill_pattern(0xdddd);
    uint8_t *ram_d = ram_copy();
    stamp_slot(slot);
    snapshot_card();
    reset_args(JAMAISVU_MODE_SAVE, slot, slot_size);
    args.backup_savedata = 0;
    mount();
    res = save_state(&fs, slot);
    unmount();
    printf("  %-44s -> %d\n", "save without savedata", res);
    CHECK(res == JV_OK, "save (uma0) failed: %d", res);
    CHECK(changed_outside(&ext, NULL) == 0, "save (uma0) wrote outside slot");
    st = read_state(slot, &len);
    CHECK(((SaveStateHeader *)st)->file_num == 0, "savedata was backed up");
    {
      /* TTBCR.N = 2: the kernel (TTBR0) table has 0x400 entries and a
       * process (TTBR1) table is used from entry 0x400 on. The same table
       * offered as both must give the same segments as one full walk. */
      const SaveStateHeader *h0 = (const SaveStateHeader *)state_copy;
      static uint8_t segtab[0x2000 * 8];
      memcpy(segtab, state_copy + h0->segtab_offset, h0->segtab_size);
      stamp_slot(slot);
      reset_args(JAMAISVU_MODE_SAVE, slot, slot_size);
      args.backup_savedata = 0;
      args.ttbcr_n = 2;
      args.ttbs[0] = TTB_PA; /* as a process table */
      mount();
      res = save_state(&fs, slot);
      unmount();
      st = read_state(slot, &len);
      printf("  %-44s -> %d\n", "save with TTBCR.N = 2", res);
      CHECK(res == JV_OK, "save (N=2) failed: %d", res);
      CHECK(((SaveStateHeader *)st)->segtab_size == h0->segtab_size &&
            memcmp(st + ((SaveStateHeader *)st)->segtab_offset, segtab,
                   h0->segtab_size) == 0,
            "split page table walk gives other segments");
    }

    /* savedata changes after the save must survive the load untouched */
    allow_everything();
    mount();
    write_file("ux0:user/00/savedata/PCSE00123/data.bin", sav_b, sizeof(sav_b));
    write_file("ux0:resavedata/PCSE00123/save.dat", sav_a, sizeof(sav_a));
    unmount();
    guard_deny_all();
    fill_pattern(0xeeee);

    reset_args(JAMAISVU_MODE_LOAD, slot, 0);
    args.backup_savedata = 0;
    mount();
    res = load_state(&fs, slot);
    unmount();
    printf("  %-44s -> %d\n", "load without savedata", res);
    CHECK(res == JV_OK, "load (uma0) failed: %d", res);
    memcpy(ram_d + (FLAG_PA - 0x40000000u), "\x01\x00\x00\x00", 4);
    CHECK(ram_equal(ram_d), "memory not restored (uma0)");
    allow_everything();
    mount();
    CHECK(read_file("ux0:user/00/savedata/PCSE00123/data.bin", buf, sizeof(buf), &len) == 0 &&
          len == sizeof(sav_b) && memcmp(buf, sav_b, len) == 0,
          "savedata must not be touched when the card is uma0");
    CHECK(read_file("ux0:resavedata/PCSE00123/save.dat", buf, sizeof(buf), &len) == 0 &&
          len == sizeof(sav_a) && memcmp(buf, sav_a, len) == 0,
          "reSavedata folder must not be touched when the card is uma0");
    unmount();
    guard_deny_all();
    free(ram_d);
  }

  /* --- Only one of the two savedata folders is the games' on this card:
   *     the first slot state (both folders backed up) loaded with the
   *     system folder selected alone puts that one back and reads past
   *     reSavedata's records without touching its folder ------------- */
  printf("load with the system savedata folder only\n");
  {
    fill_pattern(0xcccc);
    uint8_t *ram_e = ram_copy();
    allow_everything();
    mount();
    write_file("ux0:user/00/savedata/PCSE00123/data.bin", sav_b, sizeof(sav_b));
    write_file("ux0:resavedata/PCSE00123/save.dat", sav_a, sizeof(sav_a));
    unmount();
    guard_deny_all();
    /* the slot holds the uma0 save by now: put the first state back */
    allow_everything();
    mount();
    {
      FIL f;
      UINT bw;
      CHECK(f_open(&f, slot, FA_WRITE) == FR_OK, "slot reopen");
      CHECK(f_write(&f, state_copy, slot_size, &bw) == FR_OK && bw == slot_size,
            "slot rewrite");
      f_close(&f);
    }
    unmount();
    guard_deny_all();
    fill_pattern(0xeeee);
    reset_args(JAMAISVU_MODE_LOAD, slot, 0);
    args.backup_savedata = JV_SAVEDATA_SYSTEM;
    mount();
    res = load_state(&fs, slot);
    unmount();
    printf("  %-44s -> %d\n", "load, system folder only", res);
    CHECK(res == JV_OK, "load (system folder only) failed: %d", res);
    CHECK(args.savedata_folders == JV_SAVEDATA_SYSTEM && args.savedata_files > 0 &&
          args.savedata_files < ((SaveStateHeader *)state_copy)->file_num,
          "restored savedata count/folders reported to the plugin");
    allow_everything();
    mount();
    CHECK(read_file("ux0:user/00/savedata/PCSE00123/data.bin", buf, sizeof(buf), &len) == 0 &&
          len == sizeof(sav_a) && memcmp(buf, sav_a, len) == 0,
          "system savedata not restored (system folder only)");
    CHECK(read_file("ux0:resavedata/PCSE00123/save.dat", buf, sizeof(buf), &len) == 0 &&
          len == sizeof(sav_a) && memcmp(buf, sav_a, len) == 0,
          "reSavedata folder touched although not selected");
    unmount();
    guard_deny_all();
    free(ram_e);
  }

  /* --- Power cut during a load's savedata restore ------------------------
   * The restore is the one part of a run that changes the file system
   * (through FatFs). Here it is cut after every single card write in
   * turn - the card then holds exactly what a power cut at that moment
   * leaves - and each of those cards is checked: by fsck.exfat, and by
   * loading the slot once more, which must finish the restore.
   *
   * What a cut may leave: clusters that are allocated and unused, and a
   * file whose directory entry is marked unused for the moment (its other
   * entries, still in use, are what fsck calls an "unknown entry type").
   * What it must never leave: an entry that points at clusters that are
   * free, an entry block with a wrong checksum, or anything else fsck
   * objects to. And the next load of the slot must put all of it right.
   *
   * An entry block that lies across two sectors is the hard case, so the
   * whole run is repeated with the folder's entries shifted (pad files of
   * 5 entries each: 16 runs put every block at every position). All 16 on
   * one cluster size, two on the others; JV_POWERCUT_QUICK=1: one,
   * JV_POWERCUT_FULL=1: all 16 on every cluster size. ------------------ */
  printf("power cut during the savedata restore of a load\n");
  {
    size_t card_bytes = (size_t)card_sectors * 512;
    uint8_t *card_base = malloc(card_bytes), *card_cut = malloc(card_bytes);
    static uint8_t big[100 * 1024], rd[128 * 1024];
    char img[300], cmd[700], fsck_out[320];
    int cuts = 0, leaks = 0, hidden = 0, dangerous = 0, repair_bad = 0;
    int not_error = 0, repaired = 0, n_shifts;
    int explore = getenv("JV_POWERCUT_EXPLORE") != NULL;

    for (size_t i = 0; i < sizeof(big); i++)
      big[i] = (uint8_t)(i * 13 + (i >> 9));
    mount();
    n_shifts = getenv("JV_POWERCUT_QUICK") ? 1
               : getenv("JV_POWERCUT_FULL") || fs.csize == 64 ? 16 : 2;
    unmount();
    memcpy(card_base, card, card_bytes);
    /* the card images fsck reads: two per cut point, 64 MB each - in
     * memory where there is /dev/shm (a disk makes this take minutes) */
    snprintf(img, sizeof(img), "/dev/shm/jv_powercut_%ld.img", (long)getpid());
    {
      FILE *probe = fopen(img, "wb");
      if (probe)
        fclose(probe);
      else
        snprintf(img, sizeof(img), "%s/powercut.img", outdir);
    }
    snprintf(fsck_out, sizeof(fsck_out), "%s.fsck", img);
    snprintf(cmd, sizeof(cmd), "fsck.exfat -n '%s' >'%s' 2>&1", img, fsck_out);

    for (int shift = 0; shift < n_shifts; shift++) {
      int total;

      memcpy(card, card_base, card_bytes);

      /* the state to come back to: several clusters of savedata, behind
       * `shift` pad files that move every later entry by 5 places */
      allow_everything();
      mount();
      for (int k = 0; k < shift; k++) {
        char pad[96];
        snprintf(pad, sizeof(pad), "ux0:user/00/savedata/PCSE00123/"
                 "pad%02d_a_name_of_thirty_five_letters.bin", k);
        write_file(pad, sav_a, 10);
      }
      write_file("ux0:user/00/savedata/PCSE00123/data.bin", sav_a, sizeof(sav_a));
      write_file("ux0:user/00/savedata/PCSE00123/big.bin", big, sizeof(big));
      write_file("ux0:user/00/savedata/PCSE00123/shrunk.bin", big, 60000);
      write_file("ux0:user/00/savedata/PCSE00123/long.bin", big, 20000);
      write_file("ux0:user/00/savedata/PCSE00123/emptied.bin", big, 0);
      write_file("ux0:resavedata/PCSE00123/save.dat", sav_b, sizeof(sav_b));
      unmount();
      guard_deny_all();
      fill_pattern(0x7777);
      stamp_slot(slot);
      reset_args(JAMAISVU_MODE_SAVE, slot, slot_size);
      mount();
      res = save_state(&fs, slot);
      unmount();
      CHECK(res == JV_OK, "power cut: save failed: %d", res);

      /* what the card looks like when the load starts: a file gone, one
       * shorter, one longer, one with other content, new ones */
      allow_everything();
      mount();
      f_unlink("ux0:user/00/savedata/PCSE00123/big.bin");
      write_file("ux0:user/00/savedata/PCSE00123/data.bin", sav_b, sizeof(sav_b));
      write_file("ux0:user/00/savedata/PCSE00123/shrunk.bin", big, 9000);
      write_file("ux0:user/00/savedata/PCSE00123/grown.bin", big, 70000);
      /* longer on the card than in the slot: the restore cuts it back,
       * one to a part of itself, one to nothing */
      write_file("ux0:user/00/savedata/PCSE00123/long.bin", big, 90000);
      write_file("ux0:user/00/savedata/PCSE00123/emptied.bin", big, 30000);
      write_file("ux0:resavedata/PCSE00123/save.dat", sav_x, sizeof(sav_x));
      write_file("ux0:resavedata/PCSE00123/extra.dat", big, 50000);
      unmount();
      guard_deny_all();
      memcpy(card_cut, card, card_bytes);

      /* the whole load once, to count its card writes */
      writes_done = 0;
      reset_args(JAMAISVU_MODE_LOAD, slot, 0);
      mount();
      res = load_state(&fs, slot);
      unmount();
      total = writes_done;
      CHECK(res == JV_OK && total > 20, "power cut: full load %d, %d writes",
            res, total);
      if (explore && shift == atoi(getenv("JV_POWERCUT_EXPLORE"))) {
        printf("    volume: fat at %u, data at %u, cluster %u sectors\n",
               (unsigned)fs.fatbase, (unsigned)fs.database, (unsigned)fs.csize);
        for (int n = 0; n < total && n < WLOG_N; n++)
          printf("    write %3d: sector %u x%u (%s)\n", n,
                 (unsigned)wlog[n].sector, (unsigned)wlog[n].count,
                 wlog[n].sector < fs.fatbase ? "boot region"
                 : wlog[n].sector < fs.database ? "FAT"
                 : "cluster heap");
      }

      for (int n = 0; n < total; n++) {
        int ok;
        cuts++;
        memcpy(card, card_cut, card_bytes);
        writes_done = 0;
        fail_writes_after = n; /* the card takes n writes, then no more */
        reset_args(JAMAISVU_MODE_LOAD, slot, 0);
        mount();
        res = load_state(&fs, slot);
        unmount();
        fail_writes_after = -1;
        if (res == JV_OK)
          not_error++;

        /* the card as the cut left it */
        dump_partition(img);
        if (system(cmd) != 0) {
          char ln[400];
          int leak = 0, hid = 0;
          FILE *f = fopen(fsck_out, "r");
          while (f && fgets(ln, sizeof(ln), f)) {
            if (!strstr(ln, "ERROR"))
              continue;
            if (strstr(ln, "more clusters are allocated")) {
              leak = 1;
            } else if (strstr(ln, "unknown entry type 0xc0") ||
                       strstr(ln, "unknown entry type 0xc1")) {
              hid = 1;
            } else {
              dangerous++;
              printf("    shift %d, cut after %d writes: %s", shift, n, ln);
            }
          }
          if (f)
            fclose(f);
          leaks += leak;
          hidden += hid;
          if (explore)
            printf("    shift %d, cut after %d writes: %s%s\n", shift, n,
                   leak ? "unused clusters left allocated " : "",
                   hid ? "an entry hidden" : "");
        }

        /* loading the slot again must finish the job */
        reset_args(JAMAISVU_MODE_LOAD, slot, 0);
        mount();
        res = load_state(&fs, slot);
        unmount();
        repaired += (int)args.savedata_repaired;
        ok = res == JV_OK;
        allow_everything();
        mount();
        ok = ok &&
             read_file("ux0:user/00/savedata/PCSE00123/big.bin", rd, sizeof(rd), &len) == 0 &&
             len == sizeof(big) && memcmp(rd, big, len) == 0 &&
             read_file("ux0:user/00/savedata/PCSE00123/data.bin", rd, sizeof(rd), &len) == 0 &&
             len == sizeof(sav_a) && memcmp(rd, sav_a, len) == 0 &&
             read_file("ux0:user/00/savedata/PCSE00123/shrunk.bin", rd, sizeof(rd), &len) == 0 &&
             len == 60000 && memcmp(rd, big, len) == 0 &&
             read_file("ux0:user/00/savedata/PCSE00123/long.bin", rd, sizeof(rd), &len) == 0 &&
             len == 20000 && memcmp(rd, big, len) == 0 &&
             read_file("ux0:user/00/savedata/PCSE00123/emptied.bin", rd, sizeof(rd), &len) == 0 &&
             len == 0 &&
             read_file("ux0:user/00/savedata/PCSE00123/grown.bin", rd, sizeof(rd), &len) != 0 &&
             read_file("ux0:resavedata/PCSE00123/save.dat", rd, sizeof(rd), &len) == 0 &&
             len == sizeof(sav_b) && memcmp(rd, sav_b, len) == 0 &&
             read_file("ux0:resavedata/PCSE00123/extra.dat", rd, sizeof(rd), &len) != 0;
        for (int k = 0; ok && k < shift; k++) {
          char pad[96];
          snprintf(pad, sizeof(pad), "ux0:user/00/savedata/PCSE00123/"
                   "pad%02d_a_name_of_thirty_five_letters.bin", k);
          ok = read_file(pad, rd, sizeof(rd), &len) == 0 && len == 10;
        }
        unmount();
        guard_deny_all();
        dump_partition(img);
        if (!ok || system(cmd) != 0) {
          repair_bad++;
          printf("    shift %d, cut after %d writes: loading again did not "
                 "put it right (res %d)\n", shift, n, res);
          if (explore) { /* the card as the cut left it, to look at */
            char keep[340];
            memcpy(card, card_cut, card_bytes);
            writes_done = 0;
            fail_writes_after = n;
            reset_args(JAMAISVU_MODE_LOAD, slot, 0);
            mount();
            load_state(&fs, slot);
            unmount();
            fail_writes_after = -1;
            snprintf(keep, sizeof(keep), "%s/powercut_fail_%d_%d.img", outdir,
                     shift, n);
            dump_partition(keep);
          }
        }
      }
    }
    (void)hidden; /* (fsck does not report a hidden entry as an error) */
    printf("  %d cut points over %d layouts: nothing unsafe left on the "
           "card; %d left unused clusters allocated; loading the slot again "
           "finished every one (%d directory entries put right)\n", cuts,
           n_shifts, leaks, repaired);
    CHECK(dangerous == 0, "power cut: %d findings of a kind that must "
          "never be left on the card", dangerous);
    CHECK(repair_bad == 0, "power cut: %d cut points that loading the slot "
          "again did not put right", repair_bad);
    CHECK(not_error == 0, "power cut: %d cut loads reported success",
          not_error);
    CHECK(n_shifts < 16 || repaired > 0, "power cut: no entry block across "
          "two sectors was ever cut (the test no longer covers that)");
    remove(img);
    remove(fsck_out);
    free(card_cut);
    free(card_base);
  }

  snprintf(out, sizeof(out), "%s/04_final.img", outdir);
  dump_partition(out);

  free(ram_a);
  free(state_copy);
  for (size_t i = 0; i < N_REGIONS; i++)
    free(regions[i].mem);
  free(card);
  free(card_before);

  printf("\n%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
