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
#include "../payload/crc32.h"

#define THUMB_BYTES (JV_THUMB_W * JV_THUMB_H * 4)

#define SLOT_SUBDIR "data/JamaisVu"

static const char *g_root = "ux0:";
static int g_uma0_bad = 0;

void slots_select_root(void) {
  SceIoStat st;
  if (!g_uma0_bad && ksceIoGetstat("uma0:", &st) >= 0)
    g_root = "uma0:";
  else
    g_root = "ux0:";
}

const char *slots_root(void) {
  return g_root;
}

int slots_root_is_ux0(void) {
  return strcmp(g_root, "ux0:") == 0;
}

void slots_mark_root_bad(void) {
  if (!slots_root_is_ux0())
    g_uma0_bad = 1;
  slots_select_root();
}

void slots_sync(void) {
  ksceIoSync(g_root, 0);
  if (!slots_root_is_ux0())
    slots_sync();
}

static void path_for(char *out, int len, int slot, const char *ext) {
  snprintf(out, len, "%s" SLOT_SUBDIR "/slot%d.%s", g_root, slot + 1, ext);
}

void slot_payload_path(char *out, int len, int slot) {
  snprintf(out, len, "ux0:" SLOT_SUBDIR "/slot%d.bin", slot + 1);
}

static int mkdir_ok(const char *path) {
  int res = ksceIoMkdir(path, 0777);
  return (res >= 0 || res == (int)0x80010011 /* EEXIST */) ? 0 : res;
}

int jv_mkdirs(void) {
  mkdir_ok("ux0:data");
  return mkdir_ok(JV_DIR);
}

int slots_mkdirs(void) {
  static char path[32];
  snprintf(path, sizeof(path), "%sdata", g_root);
  mkdir_ok(path);
  snprintf(path, sizeof(path), "%s" SLOT_SUBDIR, g_root);
  return mkdir_ok(path);
}

static int64_t file_size(const char *path) {
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
  memset(&info, 0, sizeof(info));
  if (ksceIoDevctl(g_root, 0x3001, NULL, 0, &info, sizeof(info)) < 0)
    return -1;
  return (int64_t)info.free_size;
}

int slot_prepare(int slot, void (*progress)(int pct)) {
  static char path[64];
  SceUID blk, fd;
  void *zero;
  int res = 0;

  path_for(path, sizeof(path), slot, "bin");

  if (file_size(path) == JV_SLOT_SIZE)
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
      progress(pct);
      last = pct;
    }
  }

  if (ksceIoClose(fd) < 0 && res == 0)
    res = JV_ERR_SLOT_IO;
  ksceKernelFreeMemBlock(blk);

  if (res == 0 && file_size(path) != JV_SLOT_SIZE)
    res = JV_ERR_SLOT_IO;

  if (res < 0)
    ksceIoRemove(path); /* never leave a half-sized slot behind */

  /* flush everything the OS wrote before the payload touches the card */
  slots_sync();

  return res;
}

int slot_stamp(int slot, uint8_t nonce[16]) {
  static char path[64];
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

  path_for(path, sizeof(path), slot, "bin");
  SceUID fd = ksceIoOpen(path, SCE_O_WRONLY, 0);
  if (fd < 0)
    return JV_ERR_SLOT_IO;
  int res = ksceIoWrite(fd, sector, sizeof(sector)) == sizeof(sector)
                ? 0 : JV_ERR_SLOT_IO;
  if (ksceIoClose(fd) < 0)
    res = JV_ERR_SLOT_IO;
  slots_sync();

  if (res == 0 && file_size(path) != JV_SLOT_SIZE)
    res = JV_ERR_SLOT_IO;
  return res;
}

static uint32_t info_crc(const SlotInfo *info, const uint32_t *thumb) {
  uint32_t crc = crc32_update(0, info, offsetof(SlotInfo, crc));
  if (info->has_thumb)
    crc = crc32_update(crc, thumb, THUMB_BYTES);
  return crc;
}

int slot_info_write(int slot, SlotInfo *info, const uint32_t *thumb) {
  static char path[64], tmp[64];
  int res = 0;

  path_for(path, sizeof(path), slot, "jvi");
  path_for(tmp, sizeof(tmp), slot, "jvt");

  info->magic = SLOT_INFO_MAGIC;
  info->version = SLOT_INFO_VERSION;
  info->thumb_w = JV_THUMB_W;
  info->thumb_h = JV_THUMB_H;
  info->crc = info_crc(info, thumb);

  SceUID fd = ksceIoOpen(tmp, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
  if (fd < 0)
    return JV_ERR_SLOT_IO;
  if (ksceIoWrite(fd, info, sizeof(*info)) != sizeof(*info))
    res = JV_ERR_SLOT_IO;
  if (res == 0 && info->has_thumb &&
      ksceIoWrite(fd, thumb, THUMB_BYTES) != THUMB_BYTES)
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

int slot_info_read(int slot, SlotInfo *info, uint32_t *thumb) {
  static char path[64];
  int res = 0;

  path_for(path, sizeof(path), slot, "jvi");

  SceUID fd = ksceIoOpen(path, SCE_O_RDONLY, 0);
  if (fd < 0)
    return JV_ERR_SLOT_IO;

  if (ksceIoRead(fd, info, sizeof(*info)) != sizeof(*info) ||
      info->magic != SLOT_INFO_MAGIC || info->version != SLOT_INFO_VERSION ||
      info->thumb_w != JV_THUMB_W || info->thumb_h != JV_THUMB_H)
    res = JV_ERR_SLOT_IO;

  if (res == 0 && info->has_thumb &&
      ksceIoRead(fd, thumb, THUMB_BYTES) != THUMB_BYTES)
    res = JV_ERR_SLOT_IO;

  ksceIoClose(fd);

  if (res == 0 && info_crc(info, thumb) != info->crc)
    res = JV_ERR_SLOT_IO;

  if (res == 0) {
    info->titleid[sizeof(info->titleid) - 1] = '\0';
    info->title[sizeof(info->title) - 1] = '\0';
  }

  return res;
}

int slot_info_remove(int slot) {
  static char path[64];
  path_for(path, sizeof(path), slot, "jvi");
  int res = ksceIoRemove(path);
  slots_sync();
  return res;
}

int slot_file_exists(int slot) {
  static char path[64];
  path_for(path, sizeof(path), slot, "bin");
  return file_size(path) == JV_SLOT_SIZE;
}

int slot_is_loadable(int slot) {
  static char path[64];
  static SlotInfo info;
  static uint32_t thumb[JV_THUMB_W * JV_THUMB_H];

  path_for(path, sizeof(path), slot, "bin");
  if (file_size(path) != JV_SLOT_SIZE)
    return 0;
  return slot_info_read(slot, &info, thumb) == 0;
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

static int sfo_get_title(const char *path, char *out, int len) {
  static uint8_t buf[0x4000];
  SceUID fd = ksceIoOpen(path, SCE_O_RDONLY, 0);
  if (fd < 0)
    return -1;
  int n = ksceIoRead(fd, buf, sizeof(buf));
  ksceIoClose(fd);

  if (n < (int)sizeof(SfoHeader))
    return -1;
  SfoHeader *h = (SfoHeader *)buf;
  if (h->magic != 0x46535000 || h->key_table >= (uint32_t)n ||
      h->data_table >= (uint32_t)n || h->count > 256 ||
      sizeof(SfoHeader) + h->count * sizeof(SfoEntry) > (uint32_t)n)
    return -1;

  SfoEntry *e = (SfoEntry *)(buf + sizeof(SfoHeader));
  for (uint32_t i = 0; i < h->count; i++) {
    uint32_t k = h->key_table + e[i].key_off;
    uint32_t d = h->data_table + e[i].data_off;
    if (k + 6 > (uint32_t)n || d + e[i].len > (uint32_t)n)
      continue;
    if (memcmp(buf + k, "TITLE", 6) != 0)
      continue;

    /* UTF-8 -> printable ASCII. A few common symbols are spelled out,
     * anything else non-ASCII is skipped. */
    const uint8_t *p = buf + d, *end = buf + d + e[i].len;
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
  return -1;
}

void lookup_title(const char *titleid, char *out, int len) {
  static const char *const roots[] = { "ux0:patch", "ux0:app", "gro0:app",
                                       "ur0:app" };
  static char path[96];

  for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); i++) {
    snprintf(path, sizeof(path), "%s/%s/sce_sys/param.sfo", roots[i],
             titleid);
    if (sfo_get_title(path, out, len) == 0)
      return;
  }

  /* No readable name (or a non-Latin one): show the title ID */
  strncpy(out, titleid, len - 1);
  out[len - 1] = '\0';
}
