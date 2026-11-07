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
#define FS_WRITES_READ_ONLY 2 /* after a load with stale open files: only
                                * the drives they are on */

/* Resolves all offsets and installs the exFAT hooks. < 0 on failure, in
 * which case nothing is left hooked. */
int fs_init(const FwOffsets *o);

void fs_set_write_mode(uint32_t mode);
/* the drives FS_WRITES_READ_ONLY applies to, e.g. "ux0:" (empty if none) */
int fs_read_only_names(char *out, int len);
int fs_read_only_count(void);
/* how update_filesystem() tells whether a drive's device is back
 * (storage_dev_state) */
void fs_set_dev_check(int (*fn)(const char *root));
/* After a load: re-reads the drives whose device was not back during the
 * wake-up, waiting up to timeout_ms for them; lifts read-only where that
 * worked. Returns how many were re-read. */
int fs_refresh_late(uint32_t timeout_ms);
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

/* Diagnostics of the last update_filesystem(): open files that were not
 * found again (gone between the save and the load) - their entry freed if
 * nothing held it, else the drive made read-only - and open files whose
 * size changed. The first FS_NOTE_MAX are kept with their path. */
#define FS_NOTE_MAX 10
typedef struct {
  char drive[8];
  char path[100];          /* ASCII (other characters as '?') */
  uint32_t refs;           /* references to the entry */
  int res;                 /* lookup result (< 0: not found) */
  uint8_t freed;           /* entry freed by JamaisVu */
  uint64_t size_was, size_now;
} FsNote;
typedef struct {
  uint32_t gone, freed, held, resized;
  uint32_t busy; /* entries whose lock could not be taken in 200 ms: left */
  uint32_t left; /* gone but not freed (state/type, or the drive lock
                    failed): left to the kernel */
  uint32_t n;
  FsNote note[FS_NOTE_MAX];
} FsLoadNotes;
extern FsLoadNotes g_fs_notes;

/* the open-file (vnode) table now: entries, entries with a file, entries
 * held open */
void fs_vnode_usage(uint32_t *total, uint32_t *with_file, uint32_t *held);

#endif
