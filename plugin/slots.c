/* JamaisVu savestate plugin
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <psp2kern/kernel/sysmem.h>
#include <psp2kern/kernel/threadmgr.h>
#include <psp2kern/io/fcntl.h>
#include <psp2kern/io/stat.h>
#include <psp2kern/io/devctl.h>

#include <stdio.h>
#include <string.h>

#include "slots.h"
#include "md5.h"
#include "storage.h"
#include "../payload/crc32.h"

#define THUMB_PIXELS  (JV_THUMB_W * JV_THUMB_H)
#define THUMB_BYTES   (THUMB_PIXELS * 4) /* in memory; v2/v3 files */
#define THUMB_BYTES16 (THUMB_PIXELS * 2) /* v4 files: RGB565      */

/* In memory thumbnails are RGBA8888 (R in the low byte, see render.h). */
static uint16_t to_565(uint32_t c) {
  uint32_t r = c & 0xff, g = (c >> 8) & 0xff, b = (c >> 16) & 0xff;
  return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static uint32_t from_565(uint16_t v) {
  uint32_t r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
  r = (r << 3) | (r >> 2);
  g = (g << 2) | (g >> 4);
  b = (b << 3) | (b >> 2);
  return r | (g << 8) | (b << 16) | 0xff000000u;
}

#define CHUNK_PX 2048
static uint16_t g_px16[CHUNK_PX]; /* conversion chunk (worker thread) */

#define SLOT_SUBDIR "data/JamaisVu"

/* Everything JamaisVu writes lives on the official Memory Card. Callers
 * must check storage_mc_root() != NULL before writing. */
static const char *mc_root(void) {
  return storage_mc_root() ? storage_mc_root() : "";
}

void slots_sync(void) {
  if (storage_mc_root())
    ksceIoSync(storage_mc_root(), 0);
}

void slot_path(char *out, int len, int slot, const char *ext) {
  snprintf(out, len, "%s" SLOT_SUBDIR "/slot%d.%s", mc_root(), slot + 1,
           ext);
}

void jv_file_path(char *out, int len, const char *name) {
  snprintf(out, len, "%s" SLOT_SUBDIR "/%s", mc_root(), name);
}

void slot_payload_path(char *out, int len, int slot) {
  /* "ux0:" is just the payload FatFs' name for the Memory Card */
  snprintf(out, len, "ux0:" SLOT_SUBDIR "/slot%d.bin", slot + 1);
}

#define path_for slot_path

static int mkdir_ok(const char *path) {
  int res = ksceIoMkdir(path, 0777);
  return (res >= 0 || res == (int)0x80010011 /* EEXIST */) ? 0 : res;
}

int slots_mkdirs(void) {
  static char path[32];
  if (!storage_mc_root())
    return -1;
  snprintf(path, sizeof(path), "%sdata", mc_root());
  mkdir_ok(path);
  snprintf(path, sizeof(path), "%s" SLOT_SUBDIR, mc_root());
  return mkdir_ok(path);
}

int64_t slot_file_size(const char *path) {
  SceIoStat st;
  memset(&st, 0, sizeof(st));
  if (ksceIoGetstat(path, &st) < 0)
    return -1;
  return st.st_size;
}

typedef struct {
  uint64_t max_size;
  uint64_t free_size;
  uint32_t cluster_size;
  uint32_t unk;
} DevInfo;

int64_t slot_free_space(void) {
  DevInfo info;
  if (!storage_mc_root())
    return -1;
  memset(&info, 0, sizeof(info));
  if (ksceIoDevctl(mc_root(), 0x3001, NULL, 0, &info, sizeof(info)) < 0)
    return -1;
  return (int64_t)info.free_size;
}

int slot_prepare(int slot, int (*progress)(int pct)) {
  static char path[80];
  SceUID blk, fd;
  void *zero;
  int res = 0;

  if (!storage_mc_root())
    return JV_ERR_SLOT_IO;

  slot_path(path, sizeof(path), slot, "bin");

  if (slot_file_size(path) == JV_SLOT_SIZE)
    return 0;

  /* wrong size (or old format): recreate */
  ksceIoRemove(path);

  int64_t avail = slot_free_space();
  if (avail >= 0 &&
      (uint64_t)avail < (uint64_t)JV_SLOT_SIZE + JV_FREE_SPACE_MARGIN)
    return JV_ERR_SLOT_NO_SPACE;

  blk = ksceKernelAllocMemBlock("JamaisVuZero",
                                SCE_KERNEL_MEMBLOCK_TYPE_KERNEL_RW,
                                0x40000, NULL);
  if (blk < 0)
    return JV_ERR_SLOT_IO;
  ksceKernelGetMemBlockBase(blk, &zero);
  memset(zero, 0, 0x40000);

  fd = ksceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
  if (fd < 0) {
    ksceKernelFreeMemBlock(blk);
    return JV_ERR_SLOT_IO;
  }

  int last = -1;
  for (uint32_t done = 0; done < JV_SLOT_SIZE; done += 0x40000) {
    if (ksceIoWrite(fd, zero, 0x40000) != 0x40000) {
      res = JV_ERR_SLOT_IO;
      break;
    }
    int pct = (int)((uint64_t)done * 100 / JV_SLOT_SIZE);
    if (pct != last && progress) {
      last = pct;
      if (progress(pct)) {
        res = JV_ERR_SLOT_CANCELLED; /* the partial file is removed below */
        break;
      }
    }
    /* let the rest of the system get at the Memory Card in between */
    ksceKernelDelayThread(2000);
  }

  if (ksceIoClose(fd) < 0 && res == 0)
    res = JV_ERR_SLOT_IO;
  ksceKernelFreeMemBlock(blk);

  if (res == 0 && slot_file_size(path) != JV_SLOT_SIZE)
    res = JV_ERR_SLOT_IO;

  if (res < 0)
    ksceIoRemove(path); /* never leave a half-sized slot behind */

  /* flush everything the OS wrote before the payload touches the card */
  slots_sync();

  return res;
}

int slot_stamp(int slot, uint8_t nonce[16]) {
  static char path[80];
  static uint8_t sector[512];
  static uint32_t counter;

  /* Not cryptographic: it only has to differ from whatever happens to be
   * in a same-named file on another device. */
  uint32_t x = (uint32_t)ksceKernelGetSystemTimeWide() ^ (++counter * 0x9e3779b9u);
  for (int i = 0; i < 16; i++) {
    x = x * 1664525u + 1013904223u;
    nonce[i] = x >> 24;
  }

  memset(sector, 0, sizeof(sector));
  memcpy(sector, nonce, 16);

  if (!storage_mc_root())
    return JV_ERR_SLOT_IO;

  slot_path(path, sizeof(path), slot, "bin");
  SceUID fd = ksceIoOpen(path, SCE_O_WRONLY, 0);
  if (fd < 0)
    return JV_ERR_SLOT_IO;
  int res = ksceIoWrite(fd, sector, sizeof(sector)) == sizeof(sector)
                ? 0 : JV_ERR_SLOT_IO;
  if (ksceIoClose(fd) < 0)
    res = JV_ERR_SLOT_IO;
  slots_sync();

  if (res == 0 && slot_file_size(path) != JV_SLOT_SIZE)
    res = JV_ERR_SLOT_IO;
  return res;
}

/* CRC of the slot info (up to crc) and, as stored in v4, the RGB565
 * thumbnail; with fd >= 0 the thumbnail is also written. */
static int info_crc_write565(const SlotInfo *info, const uint32_t *thumb,
                             SceUID fd, uint32_t *crc_out) {
  uint32_t crc = crc32_update(0, info, offsetof(SlotInfo, crc));
  if (info->has_thumb) {
    for (int done = 0; done < THUMB_PIXELS; done += CHUNK_PX) {
      int n = THUMB_PIXELS - done < CHUNK_PX ? THUMB_PIXELS - done : CHUNK_PX;
      for (int i = 0; i < n; i++)
        g_px16[i] = to_565(thumb[done + i]);
      crc = crc32_update(crc, g_px16, n * 2);
      if (fd >= 0 && ksceIoWrite(fd, g_px16, n * 2) != n * 2)
        return -1;
    }
  }
  *crc_out = crc;
  return 0;
}

int slot_info_write(int slot, SlotInfo *info, const uint32_t *thumb) {
  static char path[80], tmp[80];
  int res = 0;

  path_for(path, sizeof(path), slot, "jvi");
  path_for(tmp, sizeof(tmp), slot, "jvt");

  if (!storage_mc_root())
    return JV_ERR_SLOT_IO;

  info->magic = SLOT_INFO_MAGIC;
  info->version = SLOT_INFO_VERSION;
  info->thumb_w = JV_THUMB_W;
  info->thumb_h = JV_THUMB_H;
  uint32_t crc;
  info_crc_write565(info, thumb, -1, &crc);
  info->crc = crc;

  SceUID fd = ksceIoOpen(tmp, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
  if (fd < 0)
    return JV_ERR_SLOT_IO;
  if (ksceIoWrite(fd, info, sizeof(*info)) != sizeof(*info))
    res = JV_ERR_SLOT_IO;
  if (res == 0 && info_crc_write565(info, thumb, fd, &crc) < 0)
    res = JV_ERR_SLOT_IO;
  if (ksceIoClose(fd) < 0)
    res = JV_ERR_SLOT_IO;

  if (res == 0) {
    ksceIoRemove(path);
    if (ksceIoRename(tmp, path) < 0)
      res = JV_ERR_SLOT_IO;
  }
  if (res < 0)
    ksceIoRemove(tmp);

  slots_sync();
  return res;
}

/* v4: reads the RGB565 thumbnail through the CRC and, if thumb is not
 * NULL, expands it to RGBA8888. */
static int read_thumb565(SceUID fd, uint32_t *thumb, uint32_t *crc) {
  for (int done = 0; done < THUMB_PIXELS; done += CHUNK_PX) {
    int n = THUMB_PIXELS - done < CHUNK_PX ? THUMB_PIXELS - done : CHUNK_PX;
    if (ksceIoRead(fd, g_px16, n * 2) != n * 2)
      return -1;
    *crc = crc32_update(*crc, g_px16, n * 2);
    if (thumb)
      for (int i = 0; i < n; i++)
        thumb[done + i] = from_565(g_px16[i]);
  }
  return 0;
}

/* v2/v3: reads the RGBA8888 thumbnail that follows the slot info into
 * thumb, or, if thumb is NULL, only through the CRC in small chunks. */
static int read_thumb(SceUID fd, uint32_t *thumb, uint32_t *crc) {
  static uint8_t chunk[4096];
  if (thumb) {
    if (ksceIoRead(fd, thumb, THUMB_BYTES) != THUMB_BYTES)
      return -1;
    *crc = crc32_update(*crc, thumb, THUMB_BYTES);
    return 0;
  }
  for (uint32_t left = THUMB_BYTES; left > 0;) {
    uint32_t n = left < sizeof(chunk) ? left : sizeof(chunk);
    if (ksceIoRead(fd, chunk, n) != (int)n)
      return -1;
    *crc = crc32_update(*crc, chunk, n);
    left -= n;
  }
  return 0;
}

/* Version 2 (before the config.txt check): converted to the current
 * layout; its plugin configuration is unknown (JV_CFG_UNKNOWN). */
static int slot_info_read_v2(SceUID fd, SlotInfo *info, uint32_t *thumb) {
  static SlotInfoV2 v2;
  uint32_t crc;

  if (ksceIoLseek(fd, 0, SCE_SEEK_SET) != 0 ||
      ksceIoRead(fd, &v2, sizeof(v2)) != sizeof(v2) ||
      v2.thumb_w != JV_THUMB_W || v2.thumb_h != JV_THUMB_H)
    return JV_ERR_SLOT_IO;
  crc = crc32_update(0, &v2, offsetof(SlotInfoV2, crc));
  if (v2.has_thumb && read_thumb(fd, thumb, &crc) < 0)
    return JV_ERR_SLOT_IO;
  if (crc != v2.crc)
    return JV_ERR_SLOT_IO;

  memset(info, 0, sizeof(*info));
  info->magic = v2.magic;
  info->version = v2.version;
  memcpy(info->titleid, v2.titleid, sizeof(info->titleid));
  memcpy(info->title, v2.title, sizeof(info->title));
  info->year = v2.year;
  info->month = v2.month;
  info->day = v2.day;
  info->hour = v2.hour;
  info->minute = v2.minute;
  info->second = v2.second;
  info->thumb_w = v2.thumb_w;
  info->thumb_h = v2.thumb_h;
  info->has_thumb = v2.has_thumb;
  memcpy(info->app_ver, v2.app_ver, sizeof(info->app_ver));
  for (int i = 0; i < JV_CFG_FILES; i++)
    info->cfg_state[i] = JV_CFG_UNKNOWN;
  return 0;
}

int slot_info_read(int slot, SlotInfo *info, uint32_t *thumb) {
  static char path[80];
  int res = 0;

  path_for(path, sizeof(path), slot, "jvi");

  SceUID fd = ksceIoOpen(path, SCE_O_RDONLY, 0);
  if (fd < 0)
    return JV_ERR_SLOT_IO;

  /* the header (magic, version) is the same in every version */
  if (ksceIoRead(fd, info, 8) != 8 || info->magic != SLOT_INFO_MAGIC) {
    res = JV_ERR_SLOT_IO;
  } else if (info->version == 2) {
    res = slot_info_read_v2(fd, info, thumb);
  } else if ((info->version != 3 && info->version != 4) ||
             ksceIoRead(fd, (uint8_t *)info + 8, sizeof(*info) - 8) !=
                 (int)(sizeof(*info) - 8) ||
             info->thumb_w != JV_THUMB_W || info->thumb_h != JV_THUMB_H) {
    res = JV_ERR_SLOT_IO;
  } else {
    /* 3: RGBA8888 thumbnail, 4: RGB565 (same header layout) */
    uint32_t crc = crc32_update(0, info, offsetof(SlotInfo, crc));
    if (info->has_thumb &&
        (info->version == 4 ? read_thumb565(fd, thumb, &crc)
                            : read_thumb(fd, thumb, &crc)) < 0)
      res = JV_ERR_SLOT_IO;
    if (res == 0 && crc != info->crc)
      res = JV_ERR_SLOT_IO;
  }

  ksceIoClose(fd);

  if (res >= 0) {
    info->titleid[sizeof(info->titleid) - 1] = '\0';
    info->title[sizeof(info->title) - 1] = '\0';
  }

  return res;
}

int slot_info_remove(int slot) {
  static char path[80];
  if (!storage_mc_root())
    return JV_ERR_SLOT_IO;
  path_for(path, sizeof(path), slot, "jvi");
  int res = ksceIoRemove(path);
  slots_sync();
  return res;
}

int slot_file_exists(int slot) {
  static char path[80];
  if (!storage_mc_root())
    return 0;
  path_for(path, sizeof(path), slot, "bin");
  return slot_file_size(path) == JV_SLOT_SIZE;
}

int slot_is_loadable(int slot) {
  static SlotInfo info;
  return slot_file_exists(slot) && slot_info_read(slot, &info, NULL) == 0;
}



/* --- param.sfo ---------------------------------------------------------- */

typedef struct {
  uint32_t magic;
  uint32_t version;
  uint32_t key_table;
  uint32_t data_table;
  uint32_t count;
} SfoHeader;

typedef struct {
  uint16_t key_off;
  uint16_t fmt;
  uint32_t len;
  uint32_t max_len;
  uint32_t data_off;
} SfoEntry;

/* Finds key in a param.sfo; returns a pointer into buf and its length */
static const uint8_t *sfo_find(const uint8_t *buf, int n, const char *key,
                               uint32_t *len) {
  uint32_t klen = strlen(key) + 1;

  if (n < (int)sizeof(SfoHeader))
    return NULL;
  const SfoHeader *h = (const SfoHeader *)buf;
  if (h->magic != 0x46535000 || h->key_table >= (uint32_t)n ||
      h->data_table >= (uint32_t)n || h->count > 256 ||
      sizeof(SfoHeader) + h->count * sizeof(SfoEntry) > (uint32_t)n)
    return NULL;

  const SfoEntry *e = (const SfoEntry *)(buf + sizeof(SfoHeader));
  for (uint32_t i = 0; i < h->count; i++) {
    uint32_t k = h->key_table + e[i].key_off;
    uint32_t d = h->data_table + e[i].data_off;
    if (k + klen > (uint32_t)n || d + e[i].len > (uint32_t)n)
      continue;
    if (memcmp(buf + k, key, klen) != 0)
      continue;
    *len = e[i].len;
    return buf + d;
  }
  return NULL;
}

static int sfo_read(const char *path, uint8_t *buf, int size) {
  SceUID fd = ksceIoOpen(path, SCE_O_RDONLY, 0);
  if (fd < 0)
    return -1;
  int n = ksceIoRead(fd, buf, size);
  ksceIoClose(fd);
  return n;
}

/* one buffer for all param.sfo reads (all on the worker thread) */
static uint8_t g_sfo_buf[0x4000];

static int sfo_get_title(const char *path, char *out, int len) {
  uint8_t *buf = g_sfo_buf;
  uint32_t vlen;
  int n = sfo_read(path, buf, sizeof(g_sfo_buf));
  const uint8_t *p = sfo_find(buf, n, "TITLE", &vlen);
  if (!p)
    return -1;

  /* UTF-8 -> printable ASCII. A few common symbols are spelled out,
   * anything else non-ASCII is skipped. */
  const uint8_t *end = p + vlen;
  int o = 0, letters = 0;
  while (p < end && *p && o < len - 5) {
    if (*p < 0x80) {
      char ch = (*p == '\n' || *p == '\r' || *p == '\t') ? ' ' : *p;
      if (ch >= 0x20 && !(ch == ' ' && (o == 0 || out[o - 1] == ' ')))
        out[o++] = ch;
      if ((ch | 0x20) >= 'a' && (ch | 0x20) <= 'z')
        letters++;
      p++;
    } else if (p + 2 < end && p[0] == 0xe2 && p[1] == 0x84 && p[2] == 0xa2) {
      memcpy(out + o, "(TM)", 4); o += 4; p += 3;
    } else if (p + 1 < end && p[0] == 0xc2 && p[1] == 0xae) {
      memcpy(out + o, "(R)", 3); o += 3; p += 2;
    } else {
      /* skip one UTF-8 sequence */
      p++;
      while (p < end && (*p & 0xc0) == 0x80)
        p++;
    }
  }
  while (o > 0 && out[o - 1] == ' ')
    o--;
  out[o] = '\0';
  return letters >= 2 ? 0 : -1;
}

/* An installed update (ux0:patch) is what actually runs, so it is checked
 * first. These are reads only. */
static const char *const k_sfo_roots[] = { "ux0:patch", "ux0:app", "gro0:app",
                                           "ur0:app" };

void lookup_title(const char *titleid, char *out, int len) {
  static char path[96];

  for (size_t i = 0; i < sizeof(k_sfo_roots) / sizeof(k_sfo_roots[0]); i++) {
    snprintf(path, sizeof(path), "%s/%s/sce_sys/param.sfo", k_sfo_roots[i],
             titleid);
    if (sfo_get_title(path, out, len) == 0)
      return;
  }

  /* No readable name (or a non-Latin one): show the title ID */
  strncpy(out, titleid, len - 1);
  out[len - 1] = '\0';
}

int lookup_app_ver(const char *titleid, char *out, int len) {
  uint8_t *buf = g_sfo_buf;
  static char path[96];

  for (size_t i = 0; i < sizeof(k_sfo_roots) / sizeof(k_sfo_roots[0]); i++) {
    uint32_t vlen;
    snprintf(path, sizeof(path), "%s/%s/sce_sys/param.sfo", k_sfo_roots[i],
             titleid);
    int n = sfo_read(path, buf, sizeof(g_sfo_buf));
    const uint8_t *v = n > 0 ? sfo_find(buf, n, "APP_VER", &vlen) : NULL;
    if (!v)
      continue;

    /* "01.02": digits and dots only, NUL-terminated within the entry */
    int o = 0;
    for (uint32_t k = 0; k < vlen && v[k]; k++) {
      if (!((v[k] >= '0' && v[k] <= '9') || v[k] == '.') || o >= len - 1)
        return -1;
      out[o++] = (char)v[k];
    }
    out[o] = '\0';
    return o > 0 ? 0 : -1;
  }
  return -1;
}

/* ------------------------------------------------------------------------ */
/* Plugin configuration fingerprint                                         */
/* ------------------------------------------------------------------------ */

const char *const jv_cfg_paths[JV_CFG_FILES] = {
  "ux0:tai/config.txt",
  "ur0:tai/config.txt",
};

#define CFG_MAX_BYTES (1024 * 1024)

static int md5_file(const char *path, uint8_t out[16]) {
  static uint8_t buf[4096];
  Md5 m;
  uint32_t total = 0;

  SceUID fd = ksceIoOpen(path, SCE_O_RDONLY, 0);
  if (fd < 0)
    return JV_CFG_ABSENT;
  md5_init(&m);
  while (1) {
    int n = ksceIoRead(fd, buf, sizeof(buf));
    if (n < 0 || total + (uint32_t)n > CFG_MAX_BYTES) {
      ksceIoClose(fd);
      return JV_CFG_UNREADABLE;
    }
    if (n == 0)
      break;
    md5_update(&m, buf, (uint32_t)n);
    total += (uint32_t)n;
  }
  ksceIoClose(fd);
  md5_final(&m, out);
  return JV_CFG_PRESENT;
}

void config_fingerprint(uint8_t state[JV_CFG_FILES],
                        uint8_t md5[JV_CFG_FILES][16]) {
  for (int i = 0; i < JV_CFG_FILES; i++) {
    memset(md5[i], 0, 16);
    state[i] = (uint8_t)md5_file(jv_cfg_paths[i], md5[i]);
  }
}

int config_matches(const SlotInfo *info, int *which) {
  uint8_t state[JV_CFG_FILES];
  uint8_t md5[JV_CFG_FILES][16];

  config_fingerprint(state, md5);
  return config_matches_fp(info, state, md5, which);
}

int config_matches_fp(const SlotInfo *info,
                      const uint8_t state[JV_CFG_FILES],
                      const uint8_t md5[JV_CFG_FILES][16], int *which) {
  for (int i = 0; i < JV_CFG_FILES; i++) {
    /* snapshot older than the check: nothing to compare */
    if (info->cfg_state[i] == JV_CFG_UNKNOWN)
      continue;
    /* an unreadable file can never be shown to be unchanged */
    if (state[i] != info->cfg_state[i] || state[i] == JV_CFG_UNREADABLE ||
        (state[i] == JV_CFG_PRESENT && memcmp(md5[i], info->cfg_md5[i], 16))) {
      if (which)
        *which = i;
      return 0;
    }
  }
  return 1;
}
