/* Minimal stand-ins for the VitaSDK kernel headers, so plugin code that
 * deals with files and memory (slots.c, storage.c, mc_raw.c) can be built
 * and tested on a PC. Implemented by tests/host/fake_kernel.c. */
#ifndef FAKE_KERNEL_H
#define FAKE_KERNEL_H

#include <stdint.h>
#include <stddef.h>

typedef int SceUID;
typedef int64_t SceOff;
typedef int SceMode;
typedef unsigned int SceSize;
typedef unsigned int SceUInt;
typedef int SceKernelMemBlockType;
typedef struct { int unused; } SceKernelAllocMemBlockKernelOpt;

typedef struct {
  unsigned short year, month, day, hour, minute, second;
  unsigned int microsecond;
} SceDateTime;

typedef struct {
  SceMode st_mode;
  unsigned int st_attr;
  SceOff st_size;
} SceIoStat;

#define SCE_O_RDONLY 0x0001
#define SCE_O_WRONLY 0x0002
#define SCE_O_RDWR   0x0003
#define SCE_O_APPEND 0x0100
#define SCE_O_CREAT  0x0200
#define SCE_O_TRUNC  0x0400
#define SCE_SEEK_SET 0
#define SCE_SEEK_CUR 1
#define SCE_SEEK_END 2
#define SCE_KERNEL_MEMBLOCK_TYPE_KERNEL_RW 0x1020D006

SceUID ksceIoOpen(const char *file, int flags, SceMode mode);
int ksceIoClose(SceUID fd);
int ksceIoRead(SceUID fd, void *data, SceSize size);
int ksceIoWrite(SceUID fd, const void *data, SceSize size);
SceOff ksceIoLseek(SceUID fd, SceOff offset, int whence);
int ksceIoRemove(const char *file);
int ksceIoRename(const char *oldname, const char *newname);
int ksceIoMkdir(const char *dir, SceMode mode);
int ksceIoGetstat(const char *file, SceIoStat *stat);
#define SCE_S_IFMT  0xF000
#define SCE_S_IFDIR 0x1000
#define SCE_S_IFREG 0x2000
typedef struct {
  SceIoStat d_stat;
  char d_name[256];
  void *d_private;
  int dummy;
} SceIoDirent;
SceUID ksceIoDopen(const char *dirname);
int ksceIoDread(SceUID fd, SceIoDirent *dir);
int ksceIoDclose(SceUID fd);
int ksceIoSync(const char *device, unsigned int unk);
int ksceIoDevctl(const char *dev, unsigned int cmd, void *indata, int inlen,
                 void *outdata, int outlen);

SceUID ksceKernelAllocMemBlock(const char *name, SceKernelMemBlockType type,
                               SceSize size,
                               SceKernelAllocMemBlockKernelOpt *opt);
int ksceKernelFreeMemBlock(SceUID uid);
int ksceKernelGetMemBlockBase(SceUID uid, void **base);
int64_t ksceKernelGetSystemTimeWide(void);
int ksceKernelDelayThread(SceUInt delay);

#endif
