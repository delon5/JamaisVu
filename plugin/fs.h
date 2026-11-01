/* JamaisVu savestate plugin
 *
 * Copyright (C) 2020 TheFloW
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __FS_H__
#define __FS_H__

#include <stdint.h>
#include "offsets.h"

typedef struct {
  void *fnode;
  int type;
  uint64_t Position;
  void *alignedBuf;
  void *buf;
  int Cluster;
  int ClusterIndex; // index
  int unk_20;
  int unk_24;
  uint64_t CacheIndex;
  int CachedCluster;
  int unk_34;
} FileCache;

typedef struct {
  int unk_00;
  int unk_04;
  char path[0x208];
  char path2[0x208];
  int unk_418;
  int isRoot;
} SearchFile;

/* exFAT write modes */
#define FS_WRITES_ALLOWED   0
#define FS_WRITES_DROPPED   1 /* set by the payload in a restored image */
#define FS_WRITES_READ_ONLY 2 /* after a load with stale open files */

/* Resolves all offsets and installs the exFAT hooks. < 0 on failure, in
 * which case nothing is left hooked. */
int fs_init(const FwOffsets *o);

void fs_set_write_mode(uint32_t mode);
uint32_t fs_get_write_mode(void);
volatile uint32_t *fs_write_mode_addr(void);

/* Re-reads ux0 after a load. Returns the number of files that are still
 * open but no longer exist (see update_vnodes). */
int update_filesystem(void);

/* Diagnostics: ms each part of the last update_filesystem() took
 * (buffer caches, file caches, remount, vnodes, fnodes) and how many
 * vnodes/fnodes it went through. */
typedef struct {
  uint32_t ms[5];
  uint32_t vnodes, fnodes, fnodes_skipped;
} FsUpdateTiming;
extern FsUpdateTiming g_fs_update_timing;

#endif
