/* JamaisVu savestate plugin
 *
 * Copyright (C) 2020 TheFloW
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

/*
 * Savestate save/load, designed so that it cannot damage the Memory Card:
 *
 * SAVE
 *  - The plugin creates the slot file with the OS's own exFAT driver and
 *    gives it a fixed size *before* the payload runs. The payload never
 *    allocates clusters or touches any directory entry, FAT or bitmap.
 *  - The payload looks up the sectors that belong to the slot file and
 *    allows writes to exactly those sectors (guard.c). Every other write is
 *    refused before it reaches the card driver.
 *  - The header sector is cleared first and written last, with CRCs, so an
 *    interrupted save leaves a file that will never be loaded.
 *
 * LOAD
 *  - The whole file is validated (header CRC, body CRC, every path, every
 *    segment, card identity) before a single byte is written anywhere.
 *  - Savedata restore is limited to paths under ux0:user/00/savedata/<TITLE>
 *    and ux0:resavedata/<TITLE> (the folders the save was told to
 *    back up), and to the exFAT data area of ux0 (no partition table, no
 *    boot sector).
 *  - The mediaid sector is written only to the sector it was read from, and
 *    only if the card's partition table is identical to the one at save time.
 *  - The plugin's write-blocking flag is armed inside the restored memory
 *    image, so the restored OS cannot write with its stale filesystem caches
 *    before the plugin has remounted ux0.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "main.h"
#include "state.h"
#include "crc32.h"
#include "guard.h"
#include "phys.h"
#include "msif/ms.h"
#include "ff.h"

#define N_SEGMENTS      0x2000
#define MAX_FILE_NUM    65536
#define MAX_PATH_LEN    512
#define MAX_DIR_DEPTH   16
#define MAX_SAVEDATA_FILE_SIZE (512u * 1024 * 1024)
#define MAX_EXTENTS     4096
#define CLMT_SIZE       (2 * MAX_EXTENTS + 2)
#define SECTOR_SIZE     512u

/* the savedata folders, indexed by the bit of pargs->backup_savedata */
static const char *const k_savedata_roots[2] = { SAVEDATA_PREFIX,
                                                 RESAVEDATA_PREFIX };

static Segment segments[N_SEGMENTS];
static int n_segments = 0;
static int segments_overflow = 0;

static uint8_t __attribute__((aligned(64))) mbr_buf[SECTOR_SIZE];
static uint8_t __attribute__((aligned(64))) mediaid_buf[SECTOR_SIZE];
static uint8_t __attribute__((aligned(64))) zero_sector[SECTOR_SIZE];
static uint8_t __attribute__((aligned(64))) io_buf[0x8000];
static DWORD clmt[CLMT_SIZE];

/* ------------------------------------------------------------------------ */
/* Card identity                                                            */
/* ------------------------------------------------------------------------ */

static int verify_mbr(const master_block_t *mbr) {
  if (memcmp(mbr->magic, "Sony Computer Entertainment Inc.", 32) != 0)
    return -1;
  if (mbr->sig != 0xaa55)
    return -1;
  return 0;
}

/* Returns the sector of the mediaid partition (> 0), or < 0 on error.
 * Never returns 0, so callers can never be pointed at the MBR. */
int get_mediaid_sector(uint32_t *mbr_crc) {
  const master_block_t *mbr = (const master_block_t *)mbr_buf;
  uint32_t sector = 0;
  int found = 0;

  if (ms_read_sector(mbr_buf, 0, 1) != 0)
    return JV_ERR_IO;

  if (verify_mbr(mbr) < 0)
    return JV_ERR_MEDIAID;

  for (int i = 0; i < 0x10; i++) {
    if (mbr->partitions[i].code == 0xd) {
      if (found)
        return JV_ERR_MEDIAID; /* ambiguous */
      sector = mbr->partitions[i].off;
      found = 1;
    }
  }

  if (!found || sector == 0 || sector >= 0x80000000u)
    return JV_ERR_MEDIAID;

  if (mbr_crc)
    *mbr_crc = crc32_update(0, mbr_buf, SECTOR_SIZE);

  return (int)sector;
}

/* ------------------------------------------------------------------------ */
/* Memory segments                                                          */
/* ------------------------------------------------------------------------ */

static int compare_segment(const void *a, const void *b) {
  const Segment *_a = a;
  const Segment *_b = b;
  if (_a->addr < _b->addr) return -1;
  if (_a->addr > _b->addr) return 1;
  return 0;
}

static int is_segment_in_vram_dram(uint32_t addr, uint32_t size) {
  uint64_t end = (uint64_t)addr + size;
  return size != 0 && addr >= DRAM_VRAM_START && end <= DRAM_VRAM_END;
}

static void add_segment(uint32_t addr, uint32_t size) {
  /* page tables are walked in address order and physical pages are mostly
   * handed out in runs: extend the last run instead of adding an entry */
  if (n_segments > 0 &&
      segments[n_segments - 1].addr + segments[n_segments - 1].size == addr &&
      addr >= DRAM_VRAM_START) {
    segments[n_segments - 1].size += size;
    return;
  }
  if (n_segments < N_SEGMENTS) {
    segments[n_segments].addr = addr;
    segments[n_segments].size = size;
    n_segments++;
  } else {
    segments_overflow = 1;
  }
}

static void l2_page_table(uint32_t tbl_paddr) {
  const uint32_t *tbl = jv_phys(tbl_paddr, 0x100 * 4);
  if (!tbl)
    return;

  for (int i = 0; i < 0x100; i++) {
    uint32_t entry = tbl[i];
    uint32_t addr, size;

    if ((entry & 0b11) == 0b00) {
      continue; /* Fault */
    } else if ((entry & 0b11) == 0b01) {
      addr = entry & 0xffff0000; /* Large page */
      size = 0x10000;
      i += 0xf;
    } else {
      addr = entry & 0xfffff000; /* Small page */
      size = 0x1000;
    }

    if (is_segment_in_vram_dram(addr, size))
      add_segment(addr, size);
  }
}

/* Walks the L1 entries [first, end) of one translation table. */
static void l1_page_table(uint32_t tbl_paddr, int first, int end) {
  const uint32_t *tbl = jv_phys(tbl_paddr, 0x1000 * 4);
  if (!tbl)
    return;

  for (int i = first; i < end; i++) {
    uint32_t entry = tbl[i];
    uint32_t addr, size;

    if ((entry & 0b11) == 0b00) {
      continue; /* Fault */
    } else if ((entry & 0b11) == 0b01) {
      l2_page_table(entry & 0xfffffc00); /* Page table */
      continue;
    } else if ((entry & 0b11) == 0b10) {
      if (entry & 0x40000) {
        addr = entry & 0xff000000; /* Supersection */
        size = 0x1000000;
        i += 0xf;
      } else {
        addr = entry & 0xfff00000; /* Section */
        size = 0x100000;
      }

      if (is_segment_in_vram_dram(addr, size))
        add_segment(addr, size);
    }
  }
}

static void coalesce_segments(void) {
  uint32_t s, e;
  int j = 0;

  if (n_segments == 0)
    return;

  qsort(segments, n_segments, sizeof(Segment), compare_segment);

  s = segments[0].addr;
  e = s + segments[0].size;

  for (int i = 1; i < n_segments; i++) {
    uint32_t start = segments[i].addr;
    uint32_t end = start + segments[i].size;

    if (start >= s && start <= e) {
      e = end > e ? end : e; /* overlap or adjacent */
    } else {
      segments[j].addr = s;
      segments[j].size = e - s;
      j++;
      s = start;
      e = end;
    }
  }

  segments[j].addr = s;
  segments[j].size = e - s;
  j++;

  n_segments = j;
}

static void get_dram_vram_segments(void) {
  uint32_t num = pargs->process_num;
  /* TTBCR.N splits the address space: the TTBR0 table (ttbs[num], the one
   * shared by everything) has only 0x1000 >> N entries, and of a process's
   * TTBR1 table the hardware uses only the entries from 0x1000 >> N on.
   * Reading past the short table, or the unused half of a long one, only
   * turns whatever lies there into bogus segments. N = 0: full tables. */
  int split = 0x1000 >> (pargs->ttbcr_n & 7);
  if (num > JV_MAX_PROCESSES)
    num = JV_MAX_PROCESSES;

  for (uint32_t i = 0; i < num; i++) {
    if (pargs->ttbs[i])
      l1_page_table(pargs->ttbs[i], split == 0x1000 ? 0 : split, 0x1000);
  }
  if (pargs->ttbs[num])
    l1_page_table(pargs->ttbs[num], 0, split);

  coalesce_segments();
}

/* A segment is only acceptable if save_state() could have produced it. */
static int is_valid_segment(const Segment *seg) {
  if (seg->addr == CONTEXT_PADDR)
    return seg->size == sizeof(SceSysconResumeContext);
  if (seg->addr == SPRAM_PADDR)
    return seg->size == SPRAM_SIZE;
  return is_segment_in_vram_dram(seg->addr, seg->size) &&
         (seg->addr & 0xfff) == 0 && (seg->size & 0xfff) == 0;
}

/* ------------------------------------------------------------------------ */
/* Path validation                                                          */
/* ------------------------------------------------------------------------ */

static int is_valid_titleid(const char *s, uint32_t len) {
  if (len < 4 || len > 16)
    return 0;
  for (uint32_t i = 0; i < len; i++) {
    char c = s[i];
    if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')))
      return 0;
  }
  return 1;
}

/* which savedata folder a path is under: 0 or 1 (k_savedata_roots), -1 */
static int savedata_root_of(const char *path) {
  for (int i = 0; i < 2; i++)
    if (strncmp(path, k_savedata_roots[i], strlen(k_savedata_roots[i])) == 0)
      return i;
  return -1;
}

/* Accepts only <savedata folder>/<TITLEID>[/<name>...] with no empty, "."
 * or ".." components and no control characters, ':' or '\'. */
static int is_valid_savedata_path(const char *path, int is_root) {
  const char *p;
  int comps = 0;
  int root = savedata_root_of(path);

  if (root < 0)
    return 0;

  p = path + strlen(k_savedata_roots[root]);
  while (*p) {
    const char *s = p;
    uint32_t len;

    while (*p && *p != '/') {
      unsigned char c = (unsigned char)*p;
      if (c < 0x20 || c == 0x7f || c == '\\' || c == ':')
        return 0;
      p++;
    }

    len = p - s;
    if (len == 0)
      return 0;
    if (len == 1 && s[0] == '.')
      return 0;
    if (len == 2 && s[0] == '.' && s[1] == '.')
      return 0;
    if (comps == 0 && !is_valid_titleid(s, len))
      return 0;

    comps++;
    if (comps > MAX_DIR_DEPTH)
      return 0;

    if (*p == '/') {
      p++;
      if (*p == '\0')
        return 0; /* trailing slash */
    }
  }

  if (is_root)
    return comps == 1;
  return comps >= 2;
}

/* ------------------------------------------------------------------------ */
/* Slot writer: writes the pre-allocated slot file sector by sector,        */
/* bypassing all filesystem metadata.                                       */
/* ------------------------------------------------------------------------ */

typedef struct {
  SectorRange ext[MAX_EXTENTS];
  int n_ext;
  uint32_t size;       /* slot file size in bytes */
  uint32_t pos;        /* current byte position */
  uint32_t max_chunk;  /* sectors per card write */
  int crc_on;
  uint32_t crc;
  uint8_t __attribute__((aligned(64))) stage[SECTOR_SIZE];
} SlotWriter;

static SlotWriter g_slot;

static int slot_map(uint32_t fsec, uint32_t *card_sec, uint32_t *contig) {
  for (int i = 0; i < g_slot.n_ext; i++) {
    if (fsec < g_slot.ext[i].count) {
      *card_sec = g_slot.ext[i].start + fsec;
      *contig = g_slot.ext[i].count - fsec;
      return 0;
    }
    fsec -= g_slot.ext[i].count;
  }
  return -1;
}

static int slot_write_sectors(uint32_t fsec, const void *buf, uint32_t n) {
  const uint8_t *p = buf;

  if ((uint64_t)fsec + n > g_slot.size / SECTOR_SIZE)
    return JV_ERR_TOO_BIG;

  while (n > 0) {
    uint32_t card_sec, contig, c;

    if (slot_map(fsec, &card_sec, &contig) < 0)
      return JV_ERR_TOO_BIG;

    c = MIN(n, contig);
    c = MIN(c, g_slot.max_chunk);

    if (guarded_write_sectors(p, card_sec, c) != 0)
      return guard_violations() ? JV_ERR_GUARD : JV_ERR_IO;

    p += c * SECTOR_SIZE;
    fsec += c;
    n -= c;
    progress_at(fsec * SECTOR_SIZE); /* progress screen: draws only */
  }

  return 0;
}

static int slot_write(const void *buf, uint32_t len) {
  const uint8_t *p = buf;
  int res;

  if ((uint64_t)g_slot.pos + len > g_slot.size)
    return JV_ERR_TOO_BIG;

  if (g_slot.crc_on)
    g_slot.crc = crc32_update(g_slot.crc, buf, len);

  while (len > 0) {
    uint32_t off = g_slot.pos & (SECTOR_SIZE - 1);

    if (off != 0 || len < SECTOR_SIZE || ((uintptr_t)p & 0x3)) {
      uint32_t n = MIN(SECTOR_SIZE - off, len);
      memcpy(g_slot.stage + off, p, n);
      g_slot.pos += n;
      p += n;
      len -= n;

      if ((g_slot.pos & (SECTOR_SIZE - 1)) == 0) {
        res = slot_write_sectors(g_slot.pos / SECTOR_SIZE - 1, g_slot.stage, 1);
        if (res < 0)
          return res;
        memset(g_slot.stage, 0, SECTOR_SIZE);
      }
    } else {
      uint32_t nsec = len / SECTOR_SIZE;
      res = slot_write_sectors(g_slot.pos / SECTOR_SIZE, p, nsec);
      if (res < 0)
        return res;
      g_slot.pos += nsec * SECTOR_SIZE;
      p += nsec * SECTOR_SIZE;
      len -= nsec * SECTOR_SIZE;
    }
  }

  return 0;
}

static int slot_pad(uint32_t align) {
  static const uint8_t zeros[64];
  while (g_slot.pos & (align - 1)) {
    uint32_t n = MIN(align - (g_slot.pos & (align - 1)), sizeof(zeros));
    int res = slot_write(zeros, n);
    if (res < 0)
      return res;
  }
  return 0;
}

/* Opens the slot file read-only, resolves its clusters to card sectors and
 * restricts the write guard to exactly those sectors. */
static int slot_open(FATFS *fs, const char *path) {
  FIL fp;
  FRESULT fres;
  uint32_t remaining;
  int res = 0;

  memset(&g_slot, 0, sizeof(g_slot));
  guard_deny_all();

  if (pargs->slot_size < 0x100000 || (pargs->slot_size & (SECTOR_SIZE - 1)))
    return JV_ERR_NO_SLOT;

  fres = f_open(&fp, path, FA_READ);
  if (fres != FR_OK) {
    pargs->detail = 100 + fres;
    return JV_ERR_NO_SLOT;
  }

  if (f_size(&fp) != pargs->slot_size) {
    pargs->detail = 200;
    res = JV_ERR_NO_SLOT;
    goto out;
  }

  clmt[0] = CLMT_SIZE;
  fp.cltbl = clmt;
  fres = f_lseek(&fp, CREATE_LINKMAP);
  if (fres == FR_NOT_ENOUGH_CORE) {
    pargs->detail = (int32_t)clmt[0];
    res = JV_ERR_FRAGMENTED;
    goto out;
  } else if (fres != FR_OK) {
    pargs->detail = 300 + fres;
    res = JV_ERR_IO;
    goto out;
  }

  g_slot.size = pargs->slot_size;
  /* one card command per write (ms.c splits above 128 sectors anyway);
   * was the cluster size, i.e. twice the commands for the same bytes */
  g_slot.max_chunk = 128;
  remaining = g_slot.size / SECTOR_SIZE;

  for (DWORD *t = &clmt[1]; *t != 0 && remaining > 0; t += 2) {
    uint32_t ncl = t[0];
    uint32_t scl = t[1];
    uint64_t start, count;

    if (scl < 2 || (uint64_t)scl + ncl > fs->n_fatent) {
      res = JV_ERR_IO;
      goto out;
    }

    start = (uint64_t)fs->database + (uint64_t)(scl - 2) * fs->csize;
    count = (uint64_t)ncl * fs->csize;
    if (count > remaining)
      count = remaining;
    if (start + count > 0xffffffffu || g_slot.n_ext >= MAX_EXTENTS) {
      res = JV_ERR_FRAGMENTED;
      goto out;
    }

    g_slot.ext[g_slot.n_ext].start = (uint32_t)start;
    g_slot.ext[g_slot.n_ext].count = (uint32_t)count;
    g_slot.n_ext++;
    remaining -= (uint32_t)count;
  }

  if (remaining != 0) {
    res = JV_ERR_IO;
    goto out;
  }

  for (int i = 0; i < g_slot.n_ext; i++) {
    if (guard_allow(g_slot.ext[i].start, g_slot.ext[i].count) < 0) {
      guard_deny_all();
      res = JV_ERR_FRAGMENTED;
      goto out;
    }
  }

out:
  fp.cltbl = NULL;
  f_close(&fp); /* read-only: never writes */
  return res;
}

/* ------------------------------------------------------------------------ */
/* Savedata backup (save)                                                   */
/* ------------------------------------------------------------------------ */

static int write_file_info(const FILINFO *fno, const char *path, int is_root,
                           uint32_t data_len) {
  SaveStateFile file;
  uint32_t path_len = strlen(path) + 1;
  int res;

  if (path_len > MAX_PATH_LEN)
    return JV_ERR_SAVEDATA;

  memset(&file, 0, sizeof(file));
  file.is_root  = is_root;
  file.fattrib  = fno->fattrib;
  file.ftime    = fno->ftime;
  file.fdate    = fno->fdate;
  file.path_len = ALIGN(path_len, 4);
  file.data_len = data_len;

  res = slot_write(&file, sizeof(file));
  if (res < 0)
    return res;

  res = slot_write(path, path_len);
  if (res < 0)
    return res;

  return slot_pad(4);
}

static int backup_file(const char *src, uint32_t size) {
  FIL fpsrc;
  UINT br;
  int res = 0;

  if (f_open(&fpsrc, src, FA_READ) != FR_OK)
    return JV_ERR_SAVEDATA;

  if (f_size(&fpsrc) != size) {
    res = JV_ERR_SAVEDATA;
    goto out;
  }

  while (size > 0) {
    if (f_read(&fpsrc, io_buf, MIN(size, sizeof(io_buf)), &br) != FR_OK ||
        br == 0) {
      res = JV_ERR_SAVEDATA;
      goto out;
    }

    res = slot_write(io_buf, br);
    if (res < 0)
      goto out;

    size -= br;
  }

  res = slot_pad(4);

out:
  f_close(&fpsrc);
  return res;
}

static int backup_dir(const char *path, int level, uint32_t *count) {
  static FILINFO fno;
  char new_path[MAX_PATH_LEN];
  uint32_t len = strlen(path);
  DIR dp;
  int res;

  if (level > MAX_DIR_DEPTH || len + 2 > MAX_PATH_LEN)
    return JV_ERR_SAVEDATA;

  if (f_stat(path, &fno) != FR_OK || !(fno.fattrib & AM_DIR))
    return JV_ERR_SAVEDATA;

  if (f_opendir(&dp, path) != FR_OK)
    return JV_ERR_SAVEDATA;

  res = write_file_info(&fno, path, level == 0, 0);
  if (res < 0)
    goto out;
  (*count)++;

  memcpy(new_path, path, len);
  new_path[len] = '/';

  while (1) {
    uint32_t name_len;

    if (f_readdir(&dp, &fno) != FR_OK) {
      res = JV_ERR_SAVEDATA;
      break;
    }
    if (fno.fname[0] == '\0')
      break;

    name_len = strlen(fno.fname);
    if (len + 1 + name_len + 1 > MAX_PATH_LEN) {
      res = JV_ERR_SAVEDATA;
      break;
    }
    memcpy(new_path + len + 1, fno.fname, name_len + 1);

    if (fno.fattrib & AM_DIR) {
      res = backup_dir(new_path, level + 1, count);
    } else {
      if (fno.fsize > MAX_SAVEDATA_FILE_SIZE) {
        res = JV_ERR_SAVEDATA;
        break;
      }
      res = write_file_info(&fno, new_path, 0, (uint32_t)fno.fsize);
      if (res == 0)
        res = backup_file(new_path, (uint32_t)fno.fsize);
      if (res == 0)
        (*count)++;
    }

    if (res < 0)
      break;
  }

out:
  f_closedir(&dp);
  return res;
}

/* ------------------------------------------------------------------------ */
/* Save                                                                     */
/* ------------------------------------------------------------------------ */

/* Write test (JV_TEST_VERIFY): reads the slot back and compares it with
 * memory, a word at a time (the CPU runs uncached here: a byte loop over
 * 556 MB takes minutes, words take seconds). The screen's frame is drawn
 * during the save and differs by design; its words are counted apart. */
static int verify_slot(const char *path, const SaveStateHeader *h) {
  FIL fp;
  UINT br;
  uint32_t t0 = jv_time_us();
  uint32_t bad = 0, screen = 0, first = 0, done = 0;
  uint32_t fb0 = pargs->disp_fb_paddr;
  uint32_t fb1 = fb0 + pargs->disp_fb_pitch * pargs->disp_fb_height * 4;

  stage_mark(JV_STAGE_VERIFY);
  if (f_open(&fp, path, FA_READ) != FR_OK)
    return JV_ERR_IO;
  if (f_lseek(&fp, h->data_offset) != FR_OK) {
    f_close(&fp);
    return JV_ERR_IO;
  }
  for (int i = 0; i < n_segments; i++) {
    const uint8_t *src = jv_phys(segments[i].addr, segments[i].size);
    if (!src) {
      f_close(&fp);
      return JV_ERR_IO;
    }
    for (uint32_t off = 0; off < segments[i].size;) {
      uint32_t n = MIN(segments[i].size - off, sizeof(io_buf));
      if (f_read(&fp, io_buf, n, &br) != FR_OK || br != n) {
        f_close(&fp);
        return JV_ERR_IO;
      }
      const uint32_t *a = (const uint32_t *)io_buf;
      const uint32_t *b = (const uint32_t *)(src + off);
      for (uint32_t w = 0; w < n / 4; w++) {
        if (a[w] == b[w])
          continue;
        uint32_t pa = segments[i].addr + off + w * 4;
        if (fb0 && pa >= fb0 && pa < fb1) {
          screen++;
        } else {
          bad++;
          if (!first)
            first = pa;
        }
      }
      off += n;
      done += n;
      progress_at(h->data_offset + done); /* the bar runs a second time */
    }
  }
  f_close(&fp);
  pargs->verify_bad = bad;
  pargs->verify_screen = screen;
  pargs->verify_first = first;
  pargs->verify_ms = (jv_time_us() - t0) / 1000;
  return JV_TEST_DONE;
}

int save_state(FATFS *fs, const char *path) {
  static SaveStateHeader header;
  uint32_t mbr_crc;
  uint64_t data_size;
  int sector;
  int res;

  memset(&header, 0, sizeof(header));
  n_segments = 0;
  segments_overflow = 0;

  if (pargs->ignore_flag_paddr == 0 || (pargs->ignore_flag_paddr & 3))
    return JV_ERR_ARGS;

  /* 1. Locate the pre-allocated slot and lock writes to it. */
  res = slot_open(fs, path);
  if (res < 0)
    return res;
  stage_mark(JV_STAGE_SLOT_FOUND);

  /* 1b. Make sure this is the file the OS created as ux0 (and not a file
   * with the same name on a Memory Card while ux0 is SD2Vita/USB). */
  {
    uint32_t card_sec, contig;
    if (slot_map(0, &card_sec, &contig) < 0) {
      pargs->detail = 400;
      res = JV_ERR_IO;
      goto out;
    }
    if (ms_read_sector(io_buf, card_sec, 1) != 0) {
      pargs->detail = 500;
      res = JV_ERR_IO;
      goto out;
    }
    if (memcmp(io_buf, pargs->slot_nonce, sizeof(pargs->slot_nonce)) != 0) {
      /* first bytes found, for comparison with the token */
      pargs->detail = (int32_t)(((uint32_t)io_buf[0] << 24) |
                                ((uint32_t)io_buf[1] << 16) |
                                ((uint32_t)io_buf[2] << 8) | io_buf[3]);
      res = JV_ERR_NOT_MC;
      goto out;
    }
  }

  stage_mark(JV_STAGE_SLOT_TOKEN);

  /* Diagnostics: stop here, before the first write. */
  if (pargs->debug_mode == JV_TEST_DRYRUN) {
    res = JV_TEST_DONE;
    goto out;
  }
  stage_mark(JV_STAGE_WRITING);

  /* 2. Invalidate the header first, so a partial save is never loadable. */
  res = slot_write_sectors(0, zero_sector, 1);
  if (res < 0)
    goto out;

  /* 3. mediaid sector copy. */
  sector = get_mediaid_sector(&mbr_crc);
  if (sector < 0) {
    res = sector;
    goto out;
  }
  if (ms_read_sector(mediaid_buf, sector, 1) != 0) {
    res = JV_ERR_IO;
    goto out;
  }

  g_slot.pos = SAVESTATE_MEDIAID_OFFSET;
  g_slot.crc_on = 1;
  g_slot.crc = 0;

  res = slot_write(mediaid_buf, SECTOR_SIZE);
  if (res < 0)
    goto out;

  /* 4. Savedata of the running titles, from each folder the plugin
   *    found on this card (JV_SAVEDATA_* bits). */
  header.files_offset = g_slot.pos;
  pargs->savedata_files = 0;
  pargs->savedata_folders = 0;

  for (uint32_t i = 0;
       pargs->backup_savedata && i < pargs->process_num && i < JV_MAX_PROCESSES;
       i++) {
    static char savedata_path[64];
    const char *titleid = pargs->titleid[i];
    uint32_t tlen = strnlen(titleid, 32);

    if (tlen == 0 || tlen == 32 ||
        strncmp(titleid, "main", 4) == 0 ||
        strncmp(titleid, "NPXS", 4) == 0)
      continue;

    if (!is_valid_titleid(titleid, tlen))
      continue;

    for (int r = 0; r < 2; r++) {
      uint32_t plen = strlen(k_savedata_roots[r]);

      if (!(pargs->backup_savedata & (1u << r)))
        continue;
      memcpy(savedata_path, k_savedata_roots[r], plen);
      memcpy(savedata_path + plen, titleid, tlen + 1);

      if (f_stat(savedata_path, NULL) != FR_OK)
        continue; /* this title has no savedata here */

      res = backup_dir(savedata_path, 0, &header.file_num);
      if (res < 0)
        goto out;
      pargs->savedata_folders |= 1u << r;
      pargs->savedata_files = header.file_num;

      if (header.file_num > MAX_FILE_NUM) {
        res = JV_ERR_SAVEDATA;
        goto out;
      }
    }
  }

  header.files_size = g_slot.pos - header.files_offset;

  /* 5. Segment table. */
  get_dram_vram_segments();
  add_segment(CONTEXT_PADDR, sizeof(SceSysconResumeContext));
  add_segment(SPRAM_PADDR, SPRAM_SIZE);

  if (segments_overflow) {
    res = JV_ERR_TOO_BIG;
    goto out;
  }

  data_size = 0;
  for (int i = 0; i < n_segments; i++)
    data_size += segments[i].size;

  header.segs_num = n_segments;
  header.segtab_offset = g_slot.pos;
  header.segtab_size = n_segments * sizeof(Segment);
  header.data_offset = ALIGN(header.segtab_offset + header.segtab_size,
                             SECTOR_SIZE);
  header.data_size = (uint32_t)data_size;

  if (data_size > 0xffffffffu ||
      (uint64_t)header.data_offset + data_size > g_slot.size) {
    res = JV_ERR_TOO_BIG;
    goto out;
  }
  progress_total(header.data_offset + (uint32_t)data_size);

  res = slot_write(segments, header.segtab_size);
  if (res < 0)
    goto out;

  res = slot_pad(SECTOR_SIZE);
  if (res < 0)
    goto out;

  header.body_crc = g_slot.crc;
  g_slot.crc_on = 0;

  /* 6. Memory contents. No checksum over the data: the payload runs with
   * the MMU and data cache off, and a CPU pass over ~400 MB of uncached
   * RAM takes minutes (build 3ab11e1: saves sat at 0%). The card's own
   * ECC covers the medium; the header and metadata CRCs cover the file. */
  for (int i = 0; i < n_segments; i++) {
    void *src = jv_phys(segments[i].addr, segments[i].size);
    if (!src) {
      res = JV_ERR_IO;
      goto out;
    }
    res = slot_write(src, segments[i].size);
    if (res < 0)
      goto out;
  }

  res = slot_pad(SECTOR_SIZE);
  if (res < 0)
    goto out;

  /* 7. Commit: write the header last. */
  header.magic = SAVESTATE_MAGIC;
  header.version = SAVESTATE_VERSION;
  header.header_size = sizeof(SaveStateHeader);
  header.mediaid_sector = (uint32_t)sector;
  header.mbr_crc = mbr_crc;
  header.ignore_flag_paddr = pargs->ignore_flag_paddr;
  header.header_crc = crc32_update(0, &header,
                                   offsetof(SaveStateHeader, header_crc));

  memset(g_slot.stage, 0, SECTOR_SIZE);
  memcpy(g_slot.stage, &header, sizeof(header));
  res = slot_write_sectors(0, g_slot.stage, 1);

  /* Write test: the slot is complete; now read it back. The plugin never
   * marks the slot as used (JV_TEST_DONE, not JV_OK). */
  if (res == 0 && pargs->debug_mode == JV_TEST_VERIFY)
    res = verify_slot(path, &header);

out:
  guard_deny_all();
  return res;
}

/* ------------------------------------------------------------------------ */
/* Load                                                                     */
/* ------------------------------------------------------------------------ */

/* Reads from the state file, optionally updating a running CRC. */
static uint32_t g_read_crc;
static int g_read_crc_on;

static int state_read(FIL *fp, void *buf, uint32_t len) {
  UINT br;
  if (f_read(fp, buf, len, &br) != FR_OK || br != len)
    return JV_ERR_IO;
  if (g_read_crc_on)
    g_read_crc = crc32_update(g_read_crc, buf, len);
  return 0;
}

static int state_skip(FIL *fp, uint32_t len) {
  while (len > 0) {
    uint32_t n = MIN(len, sizeof(io_buf));
    int res = state_read(fp, io_buf, n);
    if (res < 0)
      return res;
    len -= n;
  }
  return 0;
}

/* Reads one SaveStateFile record and its path, and validates both.
 * root_path holds the current root and is updated for root records. */
static int read_file_record(FIL *fp, SaveStateFile *file, char *path,
                            char *root_path, uint32_t *consumed) {
  uint32_t root_len;
  int res;

  res = state_read(fp, file, sizeof(*file));
  if (res < 0)
    return res;

  if (file->path_len < 8 || file->path_len > MAX_PATH_LEN ||
      (file->path_len & 3) || file->reserved != 0)
    return JV_ERR_BAD_STATE;

  res = state_read(fp, path, file->path_len);
  if (res < 0)
    return res;

  if (strnlen(path, file->path_len) == file->path_len)
    return JV_ERR_BAD_STATE; /* not NUL-terminated */
  if (ALIGN(strlen(path) + 1, 4) != file->path_len)
    return JV_ERR_BAD_STATE;

  if (file->is_root > 1)
    return JV_ERR_BAD_STATE;

  if (!is_valid_savedata_path(path, file->is_root))
    return JV_ERR_BAD_STATE;

  if (file->fattrib & AM_DIR) {
    if (file->data_len != 0)
      return JV_ERR_BAD_STATE;
  } else {
    if (file->is_root || file->data_len > MAX_SAVEDATA_FILE_SIZE)
      return JV_ERR_BAD_STATE;
  }

  if (file->is_root) {
    strcpy(root_path, path);
  } else {
    /* every entry must live inside the most recent root */
    root_len = strlen(root_path);
    if (root_len == 0 || strncmp(path, root_path, root_len) != 0 ||
        path[root_len] != '/')
      return JV_ERR_BAD_STATE;
  }

  *consumed += sizeof(*file) + file->path_len;
  return 0;
}

static int remove_dir(const char *path, int level) {
  static FILINFO fno;
  char new_path[MAX_PATH_LEN];
  uint32_t len = strlen(path);
  FRESULT fres;
  DIR dp;

  if (level > MAX_DIR_DEPTH || len + 2 > MAX_PATH_LEN)
    return FR_INVALID_NAME;

  fres = f_opendir(&dp, path);
  if (fres != FR_OK)
    return fres;

  memcpy(new_path, path, len);
  new_path[len] = '/';

  while (1) {
    uint32_t name_len;

    fres = f_readdir(&dp, &fno);
    if (fres != FR_OK || fno.fname[0] == '\0')
      break;

    name_len = strlen(fno.fname);
    if (len + 1 + name_len + 1 > MAX_PATH_LEN) {
      fres = FR_INVALID_NAME;
      break;
    }
    memcpy(new_path + len + 1, fno.fname, name_len + 1);

    if (fno.fattrib & AM_DIR)
      fres = remove_dir(new_path, level + 1);
    else
      fres = f_unlink(new_path);

    if (fres != FR_OK)
      break;
  }

  f_closedir(&dp);

  if (fres == FR_OK)
    fres = f_unlink(path);

  return fres;
}

/* Puts the savedata records back, those under the folders selected by
 * the JV_SAVEDATA_* bits in folders; the others are read past and left
 * alone (a state saved with reSavedata's folder on the card, loaded on a
 * console where it is not). */
static int restore_files(FIL *fpsrc, uint32_t file_num, uint32_t folders,
                         uint32_t *written, uint32_t *used) {
  static SaveStateFile file;
  static char path[MAX_PATH_LEN];
  static char root_path[MAX_PATH_LEN];
  static FILINFO fno;
  uint32_t consumed = 0;
  int skipping = 0;
  int root = -1;
  FRESULT fres;
  FIL fpdst;
  UINT bw;
  int res;

  root_path[0] = '\0';

  for (uint32_t i = 0; i < file_num; i++) {
    res = read_file_record(fpsrc, &file, path, root_path, &consumed);
    if (res < 0)
      return res;

    if (file.is_root) {
      root = savedata_root_of(path); /* valid: read_file_record checked */
      skipping = root < 0 || !(folders & (1u << root));
    }
    if (skipping) {
      if (!(file.fattrib & AM_DIR)) {
        res = state_skip(fpsrc, ALIGN(file.data_len, 4));
        if (res < 0)
          return res;
      }
      continue;
    }
    (*written)++;
    *used |= 1u << root;

    if (file.is_root) {
      fres = remove_dir(path, 0);
      if (fres != FR_OK && fres != FR_NO_PATH && fres != FR_NO_FILE)
        return JV_ERR_IO;
    }

    if (file.fattrib & AM_DIR) {
      fres = f_mkdir(path);
      if (fres != FR_OK && fres != FR_EXIST)
        return JV_ERR_IO;
    } else {
      uint32_t size = file.data_len;

      if (f_open(&fpdst, path, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK)
        return JV_ERR_IO;

      res = 0;
      while (size > 0) {
        uint32_t n = MIN(size, sizeof(io_buf));
        res = state_read(fpsrc, io_buf, n);
        if (res < 0)
          break;
        if (f_write(&fpdst, io_buf, n, &bw) != FR_OK || bw != n) {
          res = JV_ERR_IO;
          break;
        }
        size -= n;
      }

      if (f_close(&fpdst) != FR_OK && res == 0)
        res = JV_ERR_IO;
      if (res < 0)
        return res;

      res = state_skip(fpsrc, ALIGN(file.data_len, 4) - file.data_len);
      if (res < 0)
        return res;
    }

    memset(&fno, 0, sizeof(fno));
    fno.ftime = file.ftime;
    fno.fdate = file.fdate;
    if (f_utime(path, &fno) != FR_OK)
      return JV_ERR_IO;
    if (f_chmod(path, file.fattrib, AM_RDO | AM_HID | AM_SYS | AM_ARC) != FR_OK)
      return JV_ERR_IO;
  }

  return 0;
}

/* Pass 1: validate everything without writing anything. */
static int validate_state(FATFS *fs, FIL *fp, SaveStateHeader *header) {
  static SaveStateFile file;
  static char path[MAX_PATH_LEN];
  static char root_path[MAX_PATH_LEN];
  uint32_t consumed = 0;
  uint32_t mbr_crc;
  uint64_t data_size = 0;
  uint64_t fsize = f_size(fp);
  int ignore_flag_ok = 0;
  int sector;
  int res;

  (void)fs;

  /* Header */
  g_read_crc_on = 0;
  if (f_lseek(fp, 0) != FR_OK)
    return JV_ERR_IO;
  res = state_read(fp, io_buf, SECTOR_SIZE);
  if (res < 0)
    return res;
  memcpy(header, io_buf, sizeof(*header));

  /* version 3 (build 3ab11e1 only) is version 2 plus a trailing table of
   * per-segment CRCs that is no longer read: those slots still load */
  if (header->magic != SAVESTATE_MAGIC ||
      (header->version != SAVESTATE_VERSION && header->version != 3) ||
      header->header_size != sizeof(SaveStateHeader))
    return JV_ERR_BAD_STATE;

  if (crc32_update(0, header, offsetof(SaveStateHeader, header_crc)) !=
      header->header_crc)
    return JV_ERR_BAD_STATE;

  /* Layout */
  if (header->files_offset != SAVESTATE_FILES_OFFSET ||
      header->file_num > MAX_FILE_NUM ||
      header->segs_num == 0 || header->segs_num > N_SEGMENTS ||
      header->segtab_size != header->segs_num * sizeof(Segment) ||
      (uint64_t)header->files_offset + header->files_size !=
          header->segtab_offset ||
      (uint64_t)header->segtab_offset + header->segtab_size > fsize ||
      header->data_offset !=
          ALIGN((uint64_t)header->segtab_offset + header->segtab_size,
                SECTOR_SIZE) ||
      (uint64_t)header->data_offset + header->data_size > fsize ||
      header->ignore_flag_paddr == 0 || (header->ignore_flag_paddr & 3))
    return JV_ERR_BAD_STATE;

  /* Same card, same partition table */
  sector = get_mediaid_sector(&mbr_crc);
  if (sector < 0)
    return sector;
  if ((uint32_t)sector != header->mediaid_sector || mbr_crc != header->mbr_crc)
    return JV_ERR_OTHER_CARD;

  /* Body: mediaid copy, savedata records, segment table, padding */
  g_read_crc = 0;
  g_read_crc_on = 1;

  res = state_read(fp, mediaid_buf, SECTOR_SIZE);
  if (res < 0)
    return res;

  root_path[0] = '\0';
  for (uint32_t i = 0; i < header->file_num; i++) {
    res = read_file_record(fp, &file, path, root_path, &consumed);
    if (res < 0)
      return res;

    if (i == 0 && !file.is_root)
      return JV_ERR_BAD_STATE;

    if (!(file.fattrib & AM_DIR)) {
      uint32_t padded = ALIGN(file.data_len, 4);
      if ((uint64_t)consumed + padded > header->files_size)
        return JV_ERR_BAD_STATE;
      res = state_skip(fp, padded);
      if (res < 0)
        return res;
      consumed += padded;
    }

    if (consumed > header->files_size)
      return JV_ERR_BAD_STATE;
  }

  if (consumed != header->files_size)
    return JV_ERR_BAD_STATE;

  res = state_read(fp, segments, header->segtab_size);
  if (res < 0)
    return res;

  for (uint32_t i = 0; i < header->segs_num; i++) {
    if (!is_valid_segment(&segments[i]))
      return JV_ERR_BAD_STATE;
    data_size += segments[i].size;

    if (segments[i].addr >= DRAM_START &&
        header->ignore_flag_paddr >= segments[i].addr &&
        (uint64_t)header->ignore_flag_paddr + 4 <=
            (uint64_t)segments[i].addr + segments[i].size)
      ignore_flag_ok = 1;
  }

  if (data_size != header->data_size || !ignore_flag_ok)
    return JV_ERR_BAD_STATE;

  res = state_skip(fp, header->data_offset -
                       (header->segtab_offset + header->segtab_size));
  if (res < 0)
    return res;

  g_read_crc_on = 0;
  if (g_read_crc != header->body_crc)
    return JV_ERR_BAD_STATE;

  return 0;
}

int load_state(FATFS *fs, const char *path) {
  static SaveStateHeader header;
  FIL fp;
  uint32_t vol_start, vol_end;
  uint64_t end;
  int res;

  guard_deny_all();
  pargs->applied = 0;

  if (f_open(&fp, path, FA_READ) != FR_OK)
    return JV_ERR_NO_SLOT;

  /* Pass 1: validate. Nothing has been written yet; on failure the device
   * simply resumes. */
  res = validate_state(fs, &fp, &header);
  if (res < 0)
    goto out;

  /* exFAT area of ux0 (FAT, bitmap, up-case table, directories and data).
   * Boot sectors and everything outside the partition stay read-only. */
  end = (uint64_t)fs->database + (uint64_t)(fs->n_fatent - 2) * fs->csize;
  if (fs->fatbase == 0 || fs->fatbase >= end || end > 0xffffffffu) {
    res = JV_ERR_CARD;
    goto out;
  }
  vol_start = fs->fatbase;
  vol_end = (uint32_t)end;

  /* Pass 2: apply. From here on, any failure must not resume the OS. */
  pargs->applied = 1;

  /* 2a. Savedata */
  if (f_lseek(&fp, header.files_offset) != FR_OK) {
    res = JV_ERR_IO;
    goto out;
  }
  stage_mark(JV_STAGE_LOAD_SAVEDATA);
  /* only into the savedata folders the games use on this card now (the
   * plugin's JV_SAVEDATA_* bits), never into a folder that is not theirs */
  pargs->savedata_files = 0;
  pargs->savedata_folders = 0;
  if (pargs->backup_savedata) {
    guard_deny_all();
    guard_allow(vol_start, vol_end - vol_start);
    res = restore_files(&fp, header.file_num, pargs->backup_savedata,
                        &pargs->savedata_files, &pargs->savedata_folders);
    guard_deny_all();
    if (res < 0)
      goto out;
  }

  /* 2b. mediaid sector: exactly one sector, verified in pass 1 */
  guard_allow(header.mediaid_sector, 1);
  if (guarded_write_sectors(mediaid_buf, header.mediaid_sector, 1) != 0)
    res = JV_ERR_IO;
  guard_deny_all();
  if (res < 0)
    goto out;

  /* 2c. Memory */
  if (f_lseek(&fp, header.data_offset) != FR_OK) {
    res = JV_ERR_IO;
    goto out;
  }

  /* Read in 8 MiB pieces (the same bytes into the same places as one
   * read per segment) so the progress screen can follow; the hooks only
   * draw. (1 MiB pieces cost seconds on hardware.) */
  stage_mark(JV_STAGE_LOAD_MEMORY);
  progress_load_begin(header.data_size);
  uint32_t done = 0;
  for (uint32_t i = 0; i < header.segs_num; i++) {
    void *dst = jv_phys(segments[i].addr, segments[i].size);
    if (!dst) {
      res = JV_ERR_IO;
      goto out;
    }
    for (uint32_t off = 0; off < segments[i].size;) {
      uint32_t n = MIN(segments[i].size - off, 0x800000u);
      res = state_read(&fp, (uint8_t *)dst + off, n);
      if (res < 0)
        goto out;
      off += n;
      done += n;
      progress_load_at(segments[i].addr + off, done);
    }
  }

  /* 2d. Block writes in the restored OS until the plugin has remounted. */
  *(volatile uint32_t *)jv_phys(header.ignore_flag_paddr, 4) = 1;

out:
  guard_deny_all();
  f_close(&fp);
  return res;
}
