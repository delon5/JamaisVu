/* JamaisVu savestate plugin
 *
 * Persistent trace: data/JamaisVu/trace.bin on the Memory Card.
 *
 * log.txt is written by the plugin after the system resumes, so a run that
 * hangs or crashes before resuming left no record. Before every run the
 * plugin now writes trace.bin (through the OS) with a fresh random nonce
 * and finds the card sector it occupies, by reading the card's exFAT
 * structures from the raw device (read-only). The payload re-checks that
 * sector's magic + nonce on the card and then rewrites it after every
 * step, so the progress survives even a forced power-off; on the next
 * boot it is copied into log.txt.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <psp2kern/kernel/sysmem.h>
#include <psp2kern/kernel/threadmgr.h>
#include <psp2kern/io/fcntl.h>
#include <psp2kern/io/stat.h>

#include <stdio.h>
#include <string.h>

#include "slots.h"
#include "storage.h"
#include "trace.h"

#define TRACE_NAME  "trace.bin"
#define TRACE_BYTES 4096

/* ------------------------------------------------------------------------ */
/* Names for the log                                                        */
/* ------------------------------------------------------------------------ */

const char *jv_stage_name(uint32_t stage) {
  switch (stage) {
    case JV_STAGE_ENTRY:      return "started";
    case JV_STAGE_L2_FLUSH:   return "cache-flush";
    case JV_STAGE_SYSCON:     return "syscon";
    case JV_STAGE_CDRAM:      return "cdram";
    case JV_STAGE_MC_POWER:   return "card-power";
    case JV_STAGE_MC_START:   return "card-start";
    case JV_STAGE_MC_MOUNT:   return "card-mount";
    case JV_STAGE_MC_AUTH:    return "card-auth";
    case JV_STAGE_FS_MOUNT:   return "filesystem";
    case JV_STAGE_SLOT_FOUND: return "slot-found";
    case JV_STAGE_SLOT_TOKEN: return "slot-token-ok";
    case JV_STAGE_WRITING:    return "writing";
    case JV_STAGE_DONE:       return "returning-to-system";
    case JV_STAGE_DISP_I2C:   return "display-i2c";
    case JV_STAGE_DISP_DSI:   return "display-link";
    case JV_STAGE_DISP_DRAWN: return "display-drawn";
    case JV_STAGE_DISP_LCD:   return "display-lcd-on";
    case JV_STAGE_DISP_PLANE: return "display-showing";
    case JV_STAGE_DISP_SHOWN: return "display-6s-done";
    case JV_STAGE_DISP_LCD_OFF:   return "display-lcd-off";
    case JV_STAGE_DISP_PLANE_OFF: return "display-plane-off";
    case JV_STAGE_DISP_OFF:   return "display-link-off";
    case JV_STAGE_LOAD_SAVEDATA: return "restoring-savedata";
    case JV_STAGE_LOAD_MEMORY:   return "restoring-memory";
    default:                  return "none";
  }
}

static const char *jv_wake_name(uint8_t step) {
  static char fs[12];
  if (step >= JV_WAKE_FS_MOUNT) {
    snprintf(fs, sizeof(fs), "remount-%u", (unsigned)(step - JV_WAKE_FS_MOUNT));
    return fs;
  }
  switch (step) {
    case JV_WAKE_RESUME:    return "resume";
    case JV_WAKE_RESUMED:   return "resumed";
    case JV_WAKE_FS_CACHES: return "fs-caches";
    case JV_WAKE_FS_PATCH:  return "fs-patch";
    case JV_WAKE_FS_VNODES: return "fs-open-files";
    case JV_WAKE_FS_FNODES: return "fs-cached-files";
    case JV_WAKE_MENU:      return "menu";
    case JV_WAKE_WORKER:    return "worker";
    case JV_WAKE_CLOCKS:    return "clocks";
    case JV_WAKE_SLOT_INFO: return "slot-info";
    case JV_WAKE_SLOT_INFO_DONE: return "slot-info-done";
    case JV_WAKE_REPORT:    return "report";
    case JV_WAKE_CLOCKS_SET: return "clocks-set";
    case JV_WAKE_CARD_READY: return "card-ready";
    case JV_WAKE_CARD_REMOUNT:   return "card-remount";
    case JV_WAKE_CARD_REMOUNTED: return "card-remounted";
    case JV_WAKE_W_LOCK:    return "w-lock";
    case JV_WAKE_W_LOCK_TIMEOUT: return "w-lock-TIMEOUT";
    case JV_WAKE_W_TIME:    return "w-time";
    case JV_WAKE_W_MKDIR:   return "w-mkdir";
    case JV_WAKE_W_STAT:    return "w-stat";
    case JV_WAKE_W_OPEN:    return "w-open";
    case JV_WAKE_W_WRITE:   return "w-write";
    case JV_WAKE_W_CLOSE:   return "w-close";
    case JV_WAKE_W_SYNC:    return "w-sync";
    default:                return "?";
  }
}

const char *jv_result_text(int32_t res) {
  switch (res) {
    case JV_TEST_DONE:      return "test passed";
    case JV_OK:             return "ok";
    case JV_ERR_CARD:       return "Memory Card could not be read";
    case JV_ERR_NO_SLOT:    return "slot file is missing";
    case JV_ERR_FRAGMENTED: return "slot file is too fragmented";
    case JV_ERR_TOO_BIG:    return "the game uses too much memory";
    case JV_ERR_IO:         return "Memory Card read/write error";
    case JV_ERR_BAD_STATE:  return "this snapshot is damaged";
    case JV_ERR_OTHER_CARD: return "snapshot is from another Memory Card";
    case JV_ERR_GUARD:      return "an unsafe write was blocked";
    case JV_ERR_MEDIAID:    return "Memory Card layout not recognised";
    case JV_ERR_SAVEDATA:   return "could not back up the savedata";
    case JV_ERR_NOT_MC:     return "slot file is not on the official Memory Card";
    case JV_ERR_TIMER:      return "CPU cycle counter not running";
    case JV_ERR_HW:         return "hardware did not respond";
    case JV_ERR_CRASH:      return "crashed (details in log.txt)";
    case JV_ERR_ARGS:       return "invalid request";

    case JV_ERR_SLOT_NO_SPACE: return "not enough free space (704 MB needed)";
    case JV_ERR_SLOT_IO:    return "could not create the slot file";
    case JV_ERR_SLOT_CANCELLED: return "cancelled";
    default:                return "internal error";
  }
}

void trace_from_args(JvTraceSector *t, const PayloadArguments *pa) {
  memset(t, 0, sizeof(*t));
  memcpy(t->magic, JV_TRACE_MAGIC, 8);
  t->written = 1;
  t->mode = pa->mode;
  t->debug_mode = pa->debug_mode;
  t->slot = pa->slot;
  t->result = pa->result;
  t->detail = pa->detail;
  t->stage = pa->stage;
  t->soc_timer_10ms = pa->soc_timer_10ms;
  t->trace_n = pa->trace_n;
  memcpy(t->trace_stage, pa->trace_stage, sizeof(t->trace_stage));
  memcpy(t->trace_ms, pa->trace_ms, sizeof(t->trace_ms));
  t->fault_type = pa->fault_type;
  t->fault_addr = pa->fault_addr;
  t->fault_lr = pa->fault_lr;
}

/* appends to out (snprintf only: no vsnprintf needed in the kernel) */
#define APP(...)                                                  \
  do {                                                            \
    if (n >= 0 && n < len - 1) {                                  \
      int r_ = snprintf(out + n, len - n, __VA_ARGS__);           \
      if (r_ > 0)                                                 \
        n = (n + r_ >= len) ? len - 1 : n + r_;                   \
    }                                                             \
  } while (0)

int trace_format(char *out, int len, const JvTraceSector *t) {
  static const char *const modes[] = {"none", "load", "save"};
  int n = 0;

  out[0] = '\0';
  APP("%s%s slot=%u result=%d (%s) last-step=%u(%s) "
          "detail=%d/0x%08X timer=%u\r\n",
          t->mode <= 2 ? modes[t->mode] : "?",
          t->debug_mode == JV_TEST_RESET ? " (reset test)" :
          t->debug_mode == JV_TEST_DRYRUN ? " (dry run)" : "",
          (unsigned)t->slot + 1, (int)t->result, jv_result_text(t->result),
          (unsigned)t->stage, jv_stage_name(t->stage),
          (int)t->detail, (unsigned)t->detail,
          (unsigned)t->soc_timer_10ms);

  uint32_t cnt = t->trace_n;
  if (cnt > sizeof(t->trace_stage))
    cnt = sizeof(t->trace_stage);
  APP("    steps:");
  for (uint32_t i = 0; i < cnt; i++)
    APP(" %s@%ums", jv_stage_name(t->trace_stage[i]),
            (unsigned)t->trace_ms[i]);
  APP("\r\n");

  uint32_t wn = t->wake_n;
  if (wn > sizeof(t->wake_step))
    wn = sizeof(t->wake_step);
  if (wn) {
    APP("    after the wake-up (ms since the first resume event):");
    for (uint32_t i = 0; i < wn; i++)
      APP(" %s@%u", jv_wake_name(t->wake_step[i]), (unsigned)t->wake_ms[i]);
    APP("\r\n");
  }

  if (t->fault_type)
    APP("    crash: %s, fault address 0x%08X, return address 0x%08X\r\n",
            t->fault_type == 1 ? "undefined instruction" :
            t->fault_type == 2 ? "svc" :
            t->fault_type == 3 ? "prefetch abort" :
            t->fault_type == 4 ? "data abort" : "exception",
            (unsigned)t->fault_addr, (unsigned)t->fault_lr);
  return n;
}

/* ------------------------------------------------------------------------ */
/* Reading the card's exFAT structures from the raw device (read-only)      */
/* ------------------------------------------------------------------------ */

typedef struct {
  SceUID fd;
  uint32_t fat_off;   /* sectors */
  uint32_t heap_off;  /* sectors */
  uint32_t spc_shift; /* sectors per cluster = 1 << spc_shift */
  uint32_t root;      /* first cluster of the root directory */
  uint32_t clusters;  /* cluster count */
} ExFat;

static uint8_t g_sec[512] __attribute__((aligned(64)));

static uint32_t rd32(const uint8_t *p) {
  return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd64(const uint8_t *p) {
  return rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

static int raw_read(ExFat *f, uint32_t lba) {
  if (ksceIoLseek(f->fd, (SceOff)lba * 512, SCE_SEEK_SET) < 0)
    return -1;
  return ksceIoRead(f->fd, g_sec, 512) == 512 ? 0 : -1;
}

static int fat_next(ExFat *f, uint32_t cl, uint32_t *next) {
  if (raw_read(f, f->fat_off + cl / 128) < 0)
    return -1;
  *next = rd32(g_sec + (cl % 128) * 4);
  return 0;
}

static int name_eq(const char *a, const char *b, int n) {
  for (int i = 0; i < n; i++) {
    char x = a[i], y = b[i];
    if (x >= 'A' && x <= 'Z') x += 32;
    if (y >= 'A' && y <= 'Z') y += 32;
    if (x != y)
      return 0;
  }
  return 1;
}

/* Finds `name` in a directory. contiguous/len describe the directory's own
 * data (len 0 = follow the FAT, as for the root). */
static int dir_find(ExFat *f, uint32_t cl, int contiguous, uint64_t len,
                    const char *name, uint32_t *o_cl, int *o_contig,
                    uint64_t *o_len) {
  int want_len = strlen(name);
  uint32_t csize = 512u << f->spc_shift;
  uint32_t max_cl = contiguous && len ? (uint32_t)((len + csize - 1) / csize)
                                      : 4096;
  int remaining = 0, have_stream = 0, name_len = 0, name_pos = 0;
  uint8_t flags = 0;
  uint32_t s_cl = 0;
  uint64_t s_len = 0;
  char nbuf[256];

  for (uint32_t k = 0; k < max_cl; k++) {
    if (cl < 2 || cl >= f->clusters + 2)
      return -1;
    for (uint32_t s = 0; s < (1u << f->spc_shift); s++) {
      if (raw_read(f, f->heap_off + ((cl - 2) << f->spc_shift) + s) < 0)
        return -1;
      for (int e = 0; e < 16; e++) {
        const uint8_t *d = g_sec + e * 32;
        uint8_t type = d[0];
        if (type == 0x00)
          return -1; /* end of directory */
        if (!(type & 0x80)) { /* unused / deleted */
          remaining = 0;
          continue;
        }
        if (type == 0x85) {
          remaining = d[1];
          have_stream = 0;
          name_pos = 0;
          continue;
        }
        if (remaining <= 0)
          continue;
        if (type == 0xC0) {
          flags = d[1];
          name_len = d[3];
          s_cl = rd32(d + 20);
          s_len = rd64(d + 24);
          have_stream = 1;
        } else if (type == 0xC1 && have_stream) {
          for (int i = 0; i < 15 && name_pos < (int)sizeof(nbuf); i++) {
            uint8_t lo = d[2 + i * 2], hi = d[3 + i * 2];
            nbuf[name_pos++] = hi ? '?' : (char)lo;
          }
        }
        if (--remaining == 0 && have_stream && name_len == want_len &&
            name_pos >= name_len && name_eq(nbuf, name, name_len)) {
          *o_cl = s_cl;
          *o_contig = (flags & 2) != 0;
          *o_len = s_len;
          return 0;
        }
      }
    }
    if (contiguous) {
      cl++;
    } else {
      uint32_t next;
      if (fat_next(f, cl, &next) < 0 || next < 2 || next >= 0xFFFFFFF7u)
        return -1;
      cl = next;
    }
  }
  return -1;
}

/* Sector of trace.bin's first byte, relative to the start of the device. */
static int locate_trace(const char *blkdev, uint32_t *rel, const char **why) {
  static const char *const path[] = {"data", "JamaisVu", TRACE_NAME};
  ExFat f;
  int res = -1;

  memset(&f, 0, sizeof(f));
  f.fd = ksceIoOpen(blkdev, SCE_O_RDONLY, 0);
  if (f.fd < 0) {
    *why = "cannot open the card's raw device";
    return -1;
  }

  if (raw_read(&f, 0) < 0 || memcmp(g_sec + 3, "EXFAT   ", 8) != 0 ||
      g_sec[108] != 9 || g_sec[109] > 16) {
    *why = "the card's exFAT boot sector was not recognised";
    goto out;
  }
  f.fat_off = rd32(g_sec + 80);
  f.heap_off = rd32(g_sec + 88);
  f.clusters = rd32(g_sec + 92);
  f.root = rd32(g_sec + 96);
  f.spc_shift = g_sec[109];

  uint32_t cl = f.root;
  int contig = 0;
  uint64_t len = 0;
  for (int i = 0; i < 3; i++) {
    if (dir_find(&f, cl, contig, len, path[i], &cl, &contig, &len) < 0) {
      *why = "trace.bin not found in the card's directories";
      goto out;
    }
  }
  if (cl < 2 || cl >= f.clusters + 2) {
    *why = "trace.bin has an invalid cluster";
    goto out;
  }
  *rel = f.heap_off + ((cl - 2) << f.spc_shift);

  /* it must hold exactly what we just wrote through the OS */
  if (raw_read(&f, *rel) < 0) {
    *why = "cannot read trace.bin's sector";
    goto out;
  }
  res = 0;

out:
  ksceIoClose(f.fd);
  return res;
}

/* ------------------------------------------------------------------------ */

static int write_header(const uint8_t nonce[16]) {
  static char path[80];
  static uint8_t buf[TRACE_BYTES] __attribute__((aligned(64)));
  JvTraceSector *t = (JvTraceSector *)buf;

  memset(buf, 0, sizeof(buf));
  memcpy(t->magic, JV_TRACE_MAGIC, 8);
  memcpy(t->nonce, nonce, 16);

  jv_file_path(path, sizeof(path), TRACE_NAME);
  /* no O_TRUNC: keep the file's cluster(s) from run to run */
  SceUID fd = ksceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT, 0666);
  if (fd < 0)
    return -1;
  int w = ksceIoWrite(fd, buf, sizeof(buf));
  ksceIoClose(fd);
  slots_sync();
  return w == (int)sizeof(buf) ? 0 : -1;
}

int trace_prepare(uint32_t *rel_lba, uint8_t nonce[16], const char **why) {
  static uint32_t counter;
  const char *blkdev;

  *rel_lba = 0;
  if (!storage_mc_root() || slots_mkdirs() < 0) {
    *why = "no Memory Card";
    return -1;
  }
  blkdev = storage_mc_blkdev();
  if (!blkdev) {
    *why = "the card's raw device name is unknown";
    return -1;
  }

  uint32_t x = (uint32_t)ksceKernelGetSystemTimeWide() ^
               (++counter * 0x9e3779b9u) ^ 0x5bd1e995u;
  for (int i = 0; i < 16; i++) {
    x = x * 1664525u + 1013904223u;
    nonce[i] = x >> 24;
  }

  if (write_header(nonce) < 0) {
    *why = "cannot write trace.bin";
    return -1;
  }

  uint32_t rel;
  if (locate_trace(blkdev, &rel, why) < 0)
    return -1;
  const JvTraceSector *t = (const JvTraceSector *)g_sec;
  if (memcmp(t->magic, JV_TRACE_MAGIC, 8) != 0 ||
      memcmp(t->nonce, nonce, 16) != 0) {
    *why = "trace.bin's sector on the card does not match";
    return -1;
  }
  if (rel == 0) {
    *why = "trace.bin at sector 0";
    return -1;
  }
  *rel_lba = rel;
  *why = "ready";
  return 0;
}

int trace_collect(char *out, int len) {
  static char path[80];
  static JvTraceSector t;
  static const uint8_t zero[16];

  if (!storage_mc_root())
    return 0;
  jv_file_path(path, sizeof(path), TRACE_NAME);
  SceUID fd = ksceIoOpen(path, SCE_O_RDONLY, 0);
  if (fd < 0)
    return 0;
  int r = ksceIoRead(fd, &t, sizeof(t));
  ksceIoClose(fd);
  if (r != (int)sizeof(t) || memcmp(t.magic, JV_TRACE_MAGIC, 8) != 0 ||
      t.written != 1)
    return 0;

  trace_format(out, len, &t);
  write_header(zero); /* consumed */
  return 1;
}

void trace_consumed(void) {
  static const uint8_t zero[16];
  if (storage_mc_root())
    write_header(zero);
}

/* ------------------------------------------------------------------------ */
/* Steps after the wake-up                                                  */
/* ------------------------------------------------------------------------ */

static JvTraceSector g_wake __attribute__((aligned(64)));
static uint32_t g_wake_rel;
static const char *g_wake_dev;
static int g_wake_on, g_wake_io, g_wake_writes, g_wake_ret;
static int64_t g_wake_t0;

void trace_wake_begin(const PayloadArguments *pa) {
  g_wake_on = 0;
  g_wake_writes = 0;
  g_wake_ret = 0;
  g_wake_dev = storage_mc_blkdev();
  g_wake_rel = pa->trace_rel_lba;
  if (!g_wake_dev || !g_wake_rel)
    return;
  trace_from_args(&g_wake, pa);
  memcpy(g_wake.nonce, pa->trace_nonce, 16);
  g_wake.wake_n = 0;
  g_wake_t0 = ksceKernelGetSystemTimeWide();
  g_wake_io = 0;
  g_wake_on = 1;
}

void trace_wake_io(int on) {
  g_wake_io = on;
}

void trace_wake(uint8_t step) {
  if (!g_wake_on || g_wake.wake_n >= sizeof(g_wake.wake_step))
    return;
  uint32_t ms = (uint32_t)((ksceKernelGetSystemTimeWide() - g_wake_t0) / 1000);
  g_wake.wake_step[g_wake.wake_n] = step;
  g_wake.wake_ms[g_wake.wake_n] = ms > 0xFFFF ? 0xFFFF : (uint16_t)ms;
  g_wake.wake_n++;
  /* steps from before the card may be touched are kept in memory and go
   * out with the first write */
  if (!g_wake_io)
    return;
  /* the payload checked magic + nonce in this very sector before the run;
   * only it is written, through the card's raw device */
  SceUID fd = ksceIoOpen(g_wake_dev, SCE_O_WRONLY, 0);
  if (fd < 0) {
    g_wake_ret = fd;
    return;
  }
  int r = (int)ksceIoLseek(fd, (SceOff)g_wake_rel * 512, SCE_SEEK_SET);
  if (r >= 0)
    r = ksceIoWrite(fd, &g_wake, 512);
  ksceIoClose(fd);
  g_wake_ret = r;
  if (r == 512)
    g_wake_writes++;
}

int trace_wake_end(int *last_ret) {
  int n = g_wake_on ? g_wake_writes : -1;
  if (last_ret)
    *last_ret = g_wake_ret;
  g_wake_on = 0;
  return n;
}
