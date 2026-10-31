/* Fake Vita kernel I/O for host tests of the plugin's storage code.
 *
 * - The Memory Card is an exFAT image in memory. The "OS" sees it through a
 *   read-write FatFs instance (symbols renamed to os_* by run.sh), mounted
 *   under whatever name the test chooses (ux0:, uma0:, ...). The plugin's
 *   own read-only FatFs reaches it through the raw device
 *   "sdstor0:xmc-lp-ign-userext", like on the console.
 * - Other mounts (an SD2Vita as ux0, a USB stick as uma0, ...) are host
 *   directories.
 *
 * Built WITHOUT FF_FS_READONLY so the FatFs structures match the os_* copy.
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ff.h"
#include "diskio.h"
#include "fake_kernel.h"
#include "fake_ctl.h"
#include "storage.h"

/* os_* = the renamed read-write FatFs */
FRESULT os_f_mount(FATFS *fs, const TCHAR *path, BYTE opt);
FRESULT os_f_open(FIL *fp, const TCHAR *path, BYTE mode);
FRESULT os_f_close(FIL *fp);
FRESULT os_f_read(FIL *fp, void *buff, UINT btr, UINT *br);
FRESULT os_f_write(FIL *fp, const void *buff, UINT btw, UINT *bw);
FRESULT os_f_lseek(FIL *fp, FSIZE_t ofs);
FRESULT os_f_stat(const TCHAR *path, FILINFO *fno);
FRESULT os_f_unlink(const TCHAR *path);
FRESULT os_f_rename(const TCHAR *path_old, const TCHAR *path_new);
FRESULT os_f_mkdir(const TCHAR *path);
FRESULT os_f_getfree(const TCHAR *path, DWORD *nclst, FATFS **fatfs);

#define ERR_NOENT  ((int)0x80010002)
#define ERR_EXIST  ((int)0x80010011)
#define ERR_IO     ((int)0x80010005)
#define ERR_BADF   ((int)0x80010009)

/* --- configuration (fake_ctl.h) ------------------------------------------ */

uint8_t *fk_mc_image;      /* exFAT partition image */
uint32_t fk_mc_sectors;
int fk_mc_present = 1;     /* raw device opens */
char fk_mc_mount[8] = "ux0:"; /* OS mount name of the card, "" = none */
int64_t fk_host_free = 4ll << 30;

typedef struct {
  char root[8];
  char dir[256];
} HostMount;
static HostMount g_hosts[4];
static int g_nhosts;

static FATFS g_osfs;
static int g_os_mounted;

void fk_reset(void) {
  g_nhosts = 0;
  if (g_os_mounted)
    os_f_mount(NULL, "ux0:", 0);
  g_os_mounted = 0;
}

void fk_add_host_mount(const char *root, const char *dir) {
  strcpy(g_hosts[g_nhosts].root, root);
  strcpy(g_hosts[g_nhosts].dir, dir);
  g_nhosts++;
}

static int os_ready(void) {
  if (!g_os_mounted) {
    if (os_f_mount(&g_osfs, "ux0:", 1) != FR_OK)
      return 0;
    g_os_mounted = 1;
  }
  return 1;
}

/* os FatFs disk: the card image */
DSTATUS os_disk_status(BYTE pdrv) { (void)pdrv; return 0; }
DSTATUS os_disk_initialize(BYTE pdrv) { (void)pdrv; return 0; }
DRESULT os_disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count) {
  (void)pdrv;
  if (sector + count > fk_mc_sectors) return RES_PARERR;
  memcpy(buff, fk_mc_image + (size_t)sector * 512, (size_t)count * 512);
  return RES_OK;
}
DRESULT os_disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count) {
  (void)pdrv;
  if (sector + count > fk_mc_sectors) return RES_PARERR;
  memcpy(fk_mc_image + (size_t)sector * 512, buff, (size_t)count * 512);
  return RES_OK;
}
DRESULT os_disk_ioctl(BYTE pdrv, BYTE cmd, void *buff) {
  (void)pdrv; (void)buff;
  return cmd == CTRL_SYNC ? RES_OK : RES_PARERR;
}
DWORD os_get_fattime(void) { return (46u << 25) | (10u << 21) | (31u << 16); }

/* --- path resolution ----------------------------------------------------- */

typedef enum { P_NONE, P_CARD, P_HOST } PathKind;

static PathKind resolve(const char *path, char *out, size_t len) {
  if (fk_mc_mount[0] && strncmp(path, fk_mc_mount, strlen(fk_mc_mount)) == 0) {
    snprintf(out, len, "ux0:%s", path + strlen(fk_mc_mount));
    return P_CARD;
  }
  for (int i = 0; i < g_nhosts; i++) {
    size_t n = strlen(g_hosts[i].root);
    if (strncmp(path, g_hosts[i].root, n) == 0) {
      snprintf(out, len, "%s/%s", g_hosts[i].dir, path + n);
      return P_HOST;
    }
  }
  return P_NONE;
}

static int is_root(const char *path) {
  size_t n = strlen(path);
  return n > 0 && path[n - 1] == ':';
}

/* --- descriptors --------------------------------------------------------- */

typedef enum { FD_FREE, FD_CARD, FD_HOST, FD_RAW } FdKind;
typedef struct {
  FdKind kind;
  FIL fil;
  FILE *fp;
  int64_t pos;
} Fd;
static Fd g_fds[32];

static SceUID alloc_fd(FdKind kind) {
  for (int i = 1; i < 32; i++)
    if (g_fds[i].kind == FD_FREE) {
      memset(&g_fds[i], 0, sizeof(g_fds[i]));
      g_fds[i].kind = kind;
      return i;
    }
  return ERR_IO;
}

static Fd *get_fd(SceUID fd) {
  if (fd <= 0 || fd >= 32 || g_fds[fd].kind == FD_FREE)
    return NULL;
  return &g_fds[fd];
}

SceUID ksceIoOpen(const char *file, int flags, SceMode mode) {
  char p[512];
  (void)mode;

  if (strcmp(file, "sdstor0:xmc-lp-ign-userext") == 0) {
    if (!fk_mc_present || !(flags == SCE_O_RDONLY))
      return ERR_NOENT;
    return alloc_fd(FD_RAW);
  }

  switch (resolve(file, p, sizeof(p))) {
    case P_CARD: {
      if (!os_ready()) return ERR_IO;
      BYTE m = 0;
      if ((flags & 3) == SCE_O_RDONLY) m = FA_READ;
      if ((flags & 3) == SCE_O_WRONLY) m = FA_WRITE;
      if ((flags & 3) == SCE_O_RDWR) m = FA_READ | FA_WRITE;
      if ((flags & SCE_O_CREAT) && (flags & SCE_O_TRUNC)) m |= FA_CREATE_ALWAYS;
      else if (flags & SCE_O_CREAT) m |= FA_OPEN_ALWAYS;
      SceUID fd = alloc_fd(FD_CARD);
      if (fd < 0) return fd;
      if (os_f_open(&g_fds[fd].fil, p, m) != FR_OK) {
        g_fds[fd].kind = FD_FREE;
        return ERR_NOENT;
      }
      return fd;
    }
    case P_HOST: {
      const char *m = "rb";
      if ((flags & 3) == SCE_O_WRONLY || (flags & 3) == SCE_O_RDWR) {
        if (flags & SCE_O_TRUNC) m = "w+b";
        else if (access(p, F_OK) == 0) m = "r+b";
        else if (flags & SCE_O_CREAT) m = "w+b";
        else return ERR_NOENT;
      }
      FILE *fp = fopen(p, m);
      if (!fp) return ERR_NOENT;
      SceUID fd = alloc_fd(FD_HOST);
      g_fds[fd].fp = fp;
      return fd;
    }
    default:
      return ERR_NOENT;
  }
}

int ksceIoClose(SceUID fd) {
  Fd *f = get_fd(fd);
  if (!f) return ERR_BADF;
  int res = 0;
  if (f->kind == FD_CARD) res = os_f_close(&f->fil) == FR_OK ? 0 : ERR_IO;
  if (f->kind == FD_HOST) res = fclose(f->fp) == 0 ? 0 : ERR_IO;
  f->kind = FD_FREE;
  return res;
}

int ksceIoRead(SceUID fd, void *data, SceSize size) {
  Fd *f = get_fd(fd);
  UINT br;
  if (!f) return ERR_BADF;
  switch (f->kind) {
    case FD_CARD:
      return os_f_read(&f->fil, data, size, &br) == FR_OK ? (int)br : ERR_IO;
    case FD_HOST:
      return (int)fread(data, 1, size, f->fp);
    case FD_RAW: {
      if (f->pos % 512 || size % 512) return ERR_IO; /* sector access only */
      if (f->pos + size > (int64_t)fk_mc_sectors * 512) return ERR_IO;
      memcpy(data, fk_mc_image + f->pos, size);
      f->pos += size;
      return (int)size;
    }
    default:
      return ERR_BADF;
  }
}

int ksceIoWrite(SceUID fd, const void *data, SceSize size) {
  Fd *f = get_fd(fd);
  UINT bw;
  if (!f) return ERR_BADF;
  if (f->kind == FD_CARD)
    return os_f_write(&f->fil, data, size, &bw) == FR_OK && bw == size
               ? (int)bw : ERR_IO;
  if (f->kind == FD_HOST)
    return fwrite(data, 1, size, f->fp) == size ? (int)size : ERR_IO;
  return ERR_IO; /* raw device is read-only */
}

SceOff ksceIoLseek(SceUID fd, SceOff offset, int whence) {
  Fd *f = get_fd(fd);
  if (!f || whence != SCE_SEEK_SET) return ERR_BADF;
  if (f->kind == FD_CARD)
    return os_f_lseek(&f->fil, offset) == FR_OK ? offset : ERR_IO;
  if (f->kind == FD_HOST)
    return fseeko(f->fp, offset, SEEK_SET) == 0 ? offset : ERR_IO;
  f->pos = offset;
  return offset;
}

int ksceIoRemove(const char *file) {
  char p[512];
  switch (resolve(file, p, sizeof(p))) {
    case P_CARD: return os_ready() && os_f_unlink(p) == FR_OK ? 0 : ERR_NOENT;
    case P_HOST: return unlink(p) == 0 ? 0 : ERR_NOENT;
    default: return ERR_NOENT;
  }
}

int ksceIoRename(const char *oldname, const char *newname) {
  char a[512], b[512];
  PathKind ka = resolve(oldname, a, sizeof(a));
  PathKind kb = resolve(newname, b, sizeof(b));
  if (ka != kb) return ERR_IO;
  if (ka == P_CARD)
    return os_ready() && os_f_rename(a, b + 4 /* drop "ux0:" */) == FR_OK
               ? 0 : ERR_IO;
  if (ka == P_HOST) return rename(a, b) == 0 ? 0 : ERR_IO;
  return ERR_NOENT;
}

int ksceIoMkdir(const char *dir, SceMode mode) {
  char p[512];
  (void)mode;
  switch (resolve(dir, p, sizeof(p))) {
    case P_CARD: {
      if (!os_ready()) return ERR_IO;
      FRESULT r = os_f_mkdir(p);
      return r == FR_OK ? 0 : r == FR_EXIST ? ERR_EXIST : ERR_IO;
    }
    case P_HOST:
      if (mkdir(p, 0777) == 0) return 0;
      return errno == EEXIST ? ERR_EXIST : ERR_IO;
    default:
      return ERR_NOENT;
  }
}

int ksceIoGetstat(const char *file, SceIoStat *st) {
  char p[512];
  memset(st, 0, sizeof(*st));
  PathKind k = resolve(file, p, sizeof(p));
  if (k == P_NONE) return ERR_NOENT;
  if (is_root(file)) return 0;
  if (k == P_CARD) {
    FILINFO fno;
    if (!os_ready() || os_f_stat(p, &fno) != FR_OK) return ERR_NOENT;
    st->st_size = fno.fsize;
    return 0;
  }
  struct stat s;
  if (stat(p, &s) != 0) return ERR_NOENT;
  st->st_size = s.st_size;
  return 0;
}

int ksceIoSync(const char *device, unsigned int unk) {
  (void)device; (void)unk;
  return 0; /* FatFs syncs on every close */
}

int ksceIoDevctl(const char *dev, unsigned int cmd, void *indata, int inlen,
                 void *outdata, int outlen) {
  char p[512];
  (void)indata; (void)inlen;
  struct { uint64_t max, free; uint32_t cluster, unk; } info = { 0, 0, 0, 0 };
  if (cmd != 0x3001 || outlen < (int)sizeof(info)) return ERR_IO;
  switch (resolve(dev, p, sizeof(p))) {
    case P_CARD: {
      DWORD nclst;
      FATFS *fs;
      if (!os_ready() || os_f_getfree("ux0:", &nclst, &fs) != FR_OK)
        return ERR_IO;
      info.cluster = fs->csize * 512;
      info.free = (uint64_t)nclst * info.cluster;
      info.max = (uint64_t)(fs->n_fatent - 2) * info.cluster;
      break;
    }
    case P_HOST:
      info.free = fk_host_free;
      info.max = fk_host_free * 2;
      info.cluster = 32768;
      break;
    default:
      return ERR_NOENT;
  }
  memcpy(outdata, &info, sizeof(info));
  return 0;
}

/* --- mount table (sceIoFindMountPoint) ------------------------------------ */

static int host_has_root(const char *root) {
  for (int i = 0; i < g_nhosts; i++)
    if (!strcmp(g_hosts[i].root, root)) return 1;
  return 0;
}

JvIoMountPoint *fk_find_mount_point(int id) {
  static JvIoDevice devs[3];
  static JvIoMountPoint mps[3];
  static const char *const MC = "sdstor0:xmc-lp-ign-userext";
  int k;
  const char *root, *blk;

  switch (id) {
    case 0x800: /* ux0 */
      k = 0; root = "ux0:";
      blk = !strcmp(fk_mc_mount, root) ? MC
          : host_has_root(root) ? "sdstor0:gcd-lp-ign-entire" : MC;
      break;
    case 0xF00: /* uma0 */
      k = 1; root = "uma0:";
      blk = !strcmp(fk_mc_mount, root) ? MC : "sdstor0:uma-pp-act-a";
      break;
    case 0xE00: /* xmc0: always the Memory Card in the system table */
      k = 2; root = "xmc0:";
      blk = MC;
      break;
    default:
      return NULL;
  }
  memset(&devs[k], 0, sizeof(devs[k]));
  memset(&mps[k], 0, sizeof(mps[k]));
  devs[k].dev = root;
  devs[k].blkdev = blk;
  devs[k].id = id;
  mps[k].id = id;
  mps[k].dev = &devs[k];
  mps[k].dev2 = &devs[k];
  return &mps[k];
}

/* --- memory / time ------------------------------------------------------- */

static void *g_blocks[64];

SceUID ksceKernelAllocMemBlock(const char *name, SceKernelMemBlockType type,
                               SceSize size,
                               SceKernelAllocMemBlockKernelOpt *opt) {
  (void)name; (void)type; (void)opt;
  for (int i = 1; i < 64; i++)
    if (!g_blocks[i]) {
      g_blocks[i] = aligned_alloc(4096, (size + 4095) & ~4095u);
      return g_blocks[i] ? 0x10000 + i : -1;
    }
  return -1;
}

int ksceKernelFreeMemBlock(SceUID uid) {
  int i = uid - 0x10000;
  if (i <= 0 || i >= 64 || !g_blocks[i]) return -1;
  free(g_blocks[i]);
  g_blocks[i] = NULL;
  return 0;
}

int ksceKernelGetMemBlockBase(SceUID uid, void **base) {
  int i = uid - 0x10000;
  if (i <= 0 || i >= 64 || !g_blocks[i]) return -1;
  *base = g_blocks[i];
  return 0;
}

int fk_blocks_in_use(void) {
  int n = 0;
  for (int i = 1; i < 64; i++) n += g_blocks[i] != NULL;
  return n;
}

int fk_card_has(const char *fatfs_path) {
  FILINFO fno;
  return os_ready() && os_f_stat(fatfs_path, &fno) == FR_OK;
}

int fk_fds_in_use(void) {
  int n = 0;
  for (int i = 1; i < 32; i++) n += g_fds[i].kind != FD_FREE;
  return n;
}

int64_t ksceKernelGetSystemTimeWide(void) {
  static int64_t t = 123456789;
  return t += 1000;
}

int ksceKernelDelayThread(SceUInt delay) {
  (void)delay;
  return 0;
}
