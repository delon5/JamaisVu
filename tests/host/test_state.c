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

#include "main.h"
#include "state.h"
#include "guard.h"
#include "crc32.h"
#include "phys.h"
#include "ms.h"
#include "ff.h"

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
  writes_done++;
  memcpy(card + (size_t)sector * 512, buf, (size_t)count * 512);
  return 0;
}

uint64_t ksceKernelGetSystemTimeWide(void) { return 0; }
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

static Region regions[] = {
  { 0x40000000u, 0x204000, NULL }, /* two 1MB sections + 4 small pages
                                      (adjacent, so coalesced) */
  { 0x40300000u, 0x10000,  NULL }, /* one large page via L2          */
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

/* ------------------------------------------------------------------------ */

int main(int argc, char **argv) {
  const char *slot = "ux0:savestate/slot1.bin";
  const uint32_t slot_size = 8 * 1024 * 1024;
  uint8_t sav_a[3000], sav_b[2000], sub_a[700], buf[8192];
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
  write_file("ux0:user/00/savedata/PCSE00123/sce_sys/param.sfo", sub_a, sizeof(sub_a));
  write_file("ux0:user/00/savedata/PCSE00999/other.bin", sav_b, sizeof(sav_b));
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

  /* --- Change everything ---------------------------------------------- */
  printf("modify card + memory\n");
  allow_everything();
  mount();
  write_file("ux0:user/00/savedata/PCSE00123/data.bin", sav_b, sizeof(sav_b));
  write_file("ux0:user/00/savedata/PCSE00123/new_since_save.bin", sav_a, 1000);
  f_unlink("ux0:user/00/savedata/PCSE00123/sce_sys/param.sfo");
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
  CHECK(read_file("ux0:user/00/savedata/PCSE00999/other.bin", buf, sizeof(buf), &len) == 0 &&
        len == sizeof(sav_b) && memcmp(buf, sav_b, len) == 0, "other title's savedata touched");
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
