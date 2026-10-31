/* JamaisVu savestate plugin
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <psp2kern/kernel/sysmem.h>
#include <psp2kern/kernel/threadmgr.h>
#include <psp2kern/io/fcntl.h>
#include <psp2kern/io/stat.h>

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "../JamaisVu.h"
#include "../payload/crc32.h"
#include "ff.h"
#include "mc_raw.h"
#include "slots.h"
#include "storage.h"

#define COPY_CHUNK 0x40000
#define PROBE_MAGIC 0x5250564a /* "JVPR" */

/* Candidate mounts of the Memory Card, in order of preference */
static const char *const k_roots[] = { "uma0:", "ux0:", "xmc0:" };
#define N_ROOTS (sizeof(k_roots) / sizeof(k_roots[0]))

static StorageMode g_mode = STORAGE_NONE;
static const char *g_mc_root = NULL;
static const char *g_slot_root = NULL;
static const char *g_reason = "not detected yet";

static FATFS g_fs; /* read-only FatFs over the raw Memory Card */

StorageMode storage_mode(void) { return g_mode; }
const char *storage_mc_root(void) { return g_mc_root; }
const char *storage_slot_root(void) { return g_slot_root; }
const char *storage_reason(void) { return g_reason; }

static int root_present(const char *root) {
  SceIoStat st;
  memset(&st, 0, sizeof(st));
  return ksceIoGetstat(root, &st) >= 0;
}

static int raw_mount(void) {
  if (mc_raw_open() < 0)
    return -1;
  if (f_mount(&g_fs, "ux0:", 1) != FR_OK) {
    mc_raw_close();
    return -1;
  }
  return 0;
}

static void raw_unmount(void) {
  f_unmount("ux0:");
  mc_raw_close();
}

StorageMode storage_detect(void) {
  static char path[64];
  static uint32_t probe[4];
  static uint32_t counter;
  int present[N_ROOTS];
  int found = -1;

  g_mode = STORAGE_NONE;
  g_mc_root = NULL;
  g_slot_root = NULL;

  if (mc_raw_open() < 0) {
    g_reason = "no Memory Card inserted";
    return g_mode;
  }
  mc_raw_close();

  uint32_t token = (uint32_t)ksceKernelGetSystemTimeWide() ^
                   (++counter * 0x9e3779b9u);

  /* 1. write a probe through every mounted candidate */
  for (size_t i = 0; i < N_ROOTS; i++) {
    present[i] = root_present(k_roots[i]);
    if (!present[i])
      continue;
    snprintf(path, sizeof(path), "%sdata", k_roots[i]);
    ksceIoMkdir(path, 0777);
    snprintf(path, sizeof(path), "%sdata/JamaisVu", k_roots[i]);
    ksceIoMkdir(path, 0777);
    snprintf(path, sizeof(path), "%sdata/JamaisVu/probe.bin", k_roots[i]);
    probe[0] = PROBE_MAGIC;
    probe[1] = token;
    probe[2] = (uint32_t)i;
    probe[3] = token ^ (uint32_t)i ^ PROBE_MAGIC;
    SceUID fd = ksceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC,
                           0666);
    if (fd >= 0) {
      ksceIoWrite(fd, probe, sizeof(probe));
      ksceIoClose(fd);
    }
    ksceIoSync(k_roots[i], 0);
  }

  /* 2. read it back from the raw Memory Card */
  if (raw_mount() == 0) {
    FIL f;
    UINT br;
    if (f_open(&f, "ux0:data/JamaisVu/probe.bin", FA_READ) == FR_OK) {
      memset(probe, 0, sizeof(probe));
      if (f_read(&f, probe, sizeof(probe), &br) == FR_OK &&
          br == sizeof(probe) && probe[0] == PROBE_MAGIC &&
          probe[1] == token && probe[2] < N_ROOTS && present[probe[2]] &&
          probe[3] == (token ^ probe[2] ^ PROBE_MAGIC))
        found = (int)probe[2];
      f_close(&f);
    }
    raw_unmount();
  }

  /* 3. clean up */
  for (size_t i = 0; i < N_ROOTS; i++) {
    if (!present[i])
      continue;
    snprintf(path, sizeof(path), "%sdata/JamaisVu/probe.bin", k_roots[i]);
    ksceIoRemove(path);
  }

  if (found < 0) {
    g_reason = "the Memory Card is not mounted as uma0, ux0 or xmc0";
    return g_mode;
  }

  g_mc_root = k_roots[found];
  g_slot_root = present[0] ? k_roots[0] /* uma0 first */ : g_mc_root;
  g_mode = (g_slot_root == g_mc_root) ? STORAGE_DIRECT : STORAGE_ARCHIVE;
  g_reason = "ok";
  return g_mode;
}

/* --- copying ------------------------------------------------------------ */

typedef struct {
  SceUID blk;
  uint8_t *buf;
} CopyBuf;

static int copybuf_get(CopyBuf *cb) {
  void *base;
  cb->blk = ksceKernelAllocMemBlock("JamaisVuCopy",
                                    SCE_KERNEL_MEMBLOCK_TYPE_KERNEL_RW,
                                    COPY_CHUNK, NULL);
  if (cb->blk < 0)
    return -1;
  ksceKernelGetMemBlockBase(cb->blk, &base);
  cb->buf = base;
  return 0;
}

static void copybuf_put(CopyBuf *cb) {
  if (cb->blk >= 0)
    ksceKernelFreeMemBlock(cb->blk);
  cb->blk = -1;
}

static void report(void (*progress)(int), uint32_t done, uint32_t total,
                   int *last) {
  int pct = total ? (int)((uint64_t)done * 100 / total) : 100;
  if (progress && pct != *last) {
    progress(pct);
    *last = pct;
  }
}

/* Reads the work file's header (raw) and returns the length in use. */
static int work_used_length(FIL *f, uint8_t *buf, uint32_t *len) {
  SaveStateHeader h;
  UINT br;

  if (f_read(f, buf, 512, &br) != FR_OK || br != 512)
    return JV_ERR_SLOT_IO;
  memcpy(&h, buf, sizeof(h));

  if (h.magic != SAVESTATE_MAGIC || h.version != SAVESTATE_VERSION ||
      h.header_size != sizeof(SaveStateHeader) ||
      crc32_update(0, &h, offsetof(SaveStateHeader, header_crc)) !=
          h.header_crc)
    return JV_ERR_ARCHIVE_BAD;

  uint64_t used = (uint64_t)h.data_offset + h.data_size;
  used = (used + 511) & ~511ull;
  if (used < 1024 || used > f_size(f) || used > JV_SLOT_SIZE)
    return JV_ERR_ARCHIVE_BAD;

  *len = (uint32_t)used;
  return 0;
}

int storage_archive_from_work(int slot, uint32_t *out_len, uint32_t *out_crc,
                              void (*progress)(int pct)) {
  static char tmp[80], dst[80], work[80];
  CopyBuf cb = { -1, NULL };
  FIL f;
  UINT br;
  uint32_t len = 0, crc = 0, done = 0;
  int last = -1;
  int res;

  if (g_mode != STORAGE_ARCHIVE)
    return JV_ERR_NO_MC;

  if (copybuf_get(&cb) < 0)
    return JV_ERR_SLOT_IO;

  if (raw_mount() < 0) {
    copybuf_put(&cb);
    return JV_ERR_NO_MC;
  }

  work_payload_path(work, sizeof(work));
  if (f_open(&f, work, FA_READ) != FR_OK) {
    res = JV_ERR_SLOT_IO;
    goto out_raw;
  }

  res = work_used_length(&f, cb.buf, &len);
  if (res < 0)
    goto out_file;

  int64_t avail = slot_free_space();
  if (avail >= 0 && (uint64_t)avail < (uint64_t)len + JV_FREE_SPACE_MARGIN) {
    res = JV_ERR_SLOT_NO_SPACE;
    goto out_file;
  }

  slot_path(tmp, sizeof(tmp), slot, "tmp");
  SceUID fd = ksceIoOpen(tmp, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
  if (fd < 0) {
    res = JV_ERR_SLOT_IO;
    goto out_file;
  }

  if (f_lseek(&f, 0) != FR_OK)
    res = JV_ERR_SLOT_IO;

  while (res == 0 && done < len) {
    uint32_t n = len - done < COPY_CHUNK ? len - done : COPY_CHUNK;
    if (f_read(&f, cb.buf, n, &br) != FR_OK || br != n) {
      res = JV_ERR_SLOT_IO;
      break;
    }
    crc = crc32_update(crc, cb.buf, n);
    if (ksceIoWrite(fd, cb.buf, n) != (int)n) {
      res = JV_ERR_SLOT_IO;
      break;
    }
    done += n;
    report(progress, done, len, &last);
  }

  if (ksceIoClose(fd) < 0 && res == 0)
    res = JV_ERR_SLOT_IO;
  slots_sync();

  if (res == 0 && slot_file_size(tmp) != (int64_t)len)
    res = JV_ERR_SLOT_IO;

  if (res == 0) {
    /* commit: only now does the old slot go away */
    slot_info_remove(slot);
    slot_path(dst, sizeof(dst), slot, "bin");
    ksceIoRemove(dst);
    if (ksceIoRename(tmp, dst) < 0)
      res = JV_ERR_SLOT_IO;
    slots_sync();
  }

  if (res < 0)
    ksceIoRemove(tmp);

out_file:
  f_close(&f);
out_raw:
  raw_unmount();
  copybuf_put(&cb);

  if (res == 0) {
    *out_len = len;
    *out_crc = crc;
  }
  return res;
}

int storage_restore_to_work(int slot, uint32_t len, uint32_t crc,
                            void (*progress)(int pct)) {
  static char src[80], work_os[80], work[80];
  CopyBuf cb = { -1, NULL };
  uint32_t done = 0, c = 0;
  int last = -1;
  int res = 0;
  SceUID in, out;

  if (g_mode != STORAGE_ARCHIVE)
    return JV_ERR_NO_MC;
  if (len < 1024 || len > JV_SLOT_SIZE)
    return JV_ERR_ARCHIVE_BAD;

  slot_path(src, sizeof(src), slot, "bin");
  work_os_path(work_os, sizeof(work_os));
  if (slot_file_size(src) != (int64_t)len)
    return JV_ERR_ARCHIVE_BAD;
  if (slot_file_size(work_os) != (int64_t)JV_SLOT_SIZE)
    return JV_ERR_SLOT_IO;

  if (copybuf_get(&cb) < 0)
    return JV_ERR_SLOT_IO;

  in = ksceIoOpen(src, SCE_O_RDONLY, 0);
  out = ksceIoOpen(work_os, SCE_O_WRONLY, 0); /* fixed size: no truncate */
  if (in < 0 || out < 0)
    res = JV_ERR_SLOT_IO;

  /* 1. copy through the OS, checking the slot's crc on the way */
  while (res == 0 && done < len) {
    uint32_t n = len - done < COPY_CHUNK ? len - done : COPY_CHUNK;
    if (ksceIoRead(in, cb.buf, n) != (int)n) {
      res = JV_ERR_SLOT_IO;
      break;
    }
    c = crc32_update(c, cb.buf, n);
    if (ksceIoWrite(out, cb.buf, n) != (int)n) {
      res = JV_ERR_SLOT_IO;
      break;
    }
    done += n;
    report(progress, done / 2, len, &last);
  }
  if (in >= 0)
    ksceIoClose(in);
  if (out >= 0 && ksceIoClose(out) < 0 && res == 0)
    res = JV_ERR_SLOT_IO;
  slots_sync();

  if (res == 0 && c != crc)
    res = JV_ERR_ARCHIVE_BAD;

  /* 2. read it back from the card itself (no OS caches involved) */
  if (res == 0) {
    FIL f;
    UINT br;
    if (raw_mount() < 0) {
      res = JV_ERR_NO_MC;
    } else {
      work_payload_path(work, sizeof(work));
      if (f_open(&f, work, FA_READ) != FR_OK) {
        res = JV_ERR_SLOT_IO;
      } else {
        c = 0;
        done = 0;
        while (done < len) {
          uint32_t n = len - done < COPY_CHUNK ? len - done : COPY_CHUNK;
          if (f_read(&f, cb.buf, n, &br) != FR_OK || br != n) {
            res = JV_ERR_SLOT_IO;
            break;
          }
          c = crc32_update(c, cb.buf, n);
          done += n;
          report(progress, len / 2 + done / 2, len, &last);
        }
        f_close(&f);
        if (res == 0 && c != crc)
          res = JV_ERR_ARCHIVE_VERIFY;
      }
      raw_unmount();
    }
  }

  copybuf_put(&cb);
  return res;
}
