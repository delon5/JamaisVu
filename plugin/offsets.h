/* JamaisVu savestate plugin
 *
 * Firmware-specific offsets.
 *
 * Everything the plugin needs that is NOT reachable through a stable NID
 * export lives here: function offsets inside SceExfatfs / SceIofilemgr,
 * global variables in their data segments and the layout of the exFAT
 * driver's internal structures. These differ between firmware versions.
 * Using 3.65 offsets on another firmware hooks random functions inside the
 * filesystem driver, which is exactly the kind of thing that destroys a
 * Memory Card, so the plugin refuses to arm itself unless the running
 * firmware has a complete, verified entry. See docs/porting-3.60.md.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __OFFSETS_H__
#define __OFFSETS_H__

#include <stdint.h>

typedef struct {
  const char *name;
  uint32_t iofilemgr_nids[5]; /* SceIofilemgr module NIDs, 0 terminated */
  int verified;               /* save AND load tested on real hardware */

  /* SceIofilemgr, text segment (Thumb, so odd) */
  uint32_t iofilemgr_clear_ncache;
  uint32_t iofilemgr_invalidate_buff_cache;
  /* SceIofilemgr, data segment */
  uint32_t iofilemgr_num_vnodes;
  uint32_t iofilemgr_vnodes;
  uint32_t iofilemgr_vnode_size;
  uint32_t iofilemgr_mnt_head;           /* informational, unused */

  /* SceExfatfs, text segment */
  uint32_t exfat_mount;                 /* UVFAT_Mount */
  uint32_t exfat_read_management_area;  /* informational, unused */
  uint32_t exfat_search_file_common;    /* UVFAT_SearchFile_common */
  uint32_t exfat_get_parent_fnode;      /* UVFAT_GetParentFnode */
  uint32_t exfat_release_fnode;         /* UVFAT_ReleaseFnode */
  uint32_t exfat_vop_umount;            /* informational, unused */
  uint32_t exfat_cleanup_cluster_cache; /* cleanup_cluster_cache_area */
  uint32_t exfat_invalidate_cluster_cache; /* invalidate_cluster_cache_data */
  uint32_t exfat_free_fp_cache;         /* FreeFpCache */
  uint32_t exfat_sfree;                 /* sfree */
  uint32_t exfat_utf16_to_utf8;         /* informational, unused */
  uint32_t exfat_write_device;          /* UVFAT_WriteDevice (hooked) */
  uint32_t exfat_read_mgmt_patch;       /* 2 Thumb NOPs: allow re-reading
                                           the management area on remount */
  /* SceExfatfs, data segment */
  uint32_t exfat_shared_resource;

  /* SceRtc, data segment */
  uint32_t rtc_ctx;

  /* exFAT driver structure layout */
  uint32_t sr_num_files;    /* shared resource: number of FileCache */
  uint32_t sr_files;        /* shared resource: FileCache array ptr */
  uint32_t sr_num_fnodes;   /* shared resource: number of fnodes */
  uint32_t sr_fnodes;       /* shared resource: fnode array */
  uint32_t fnode_size;
  uint32_t fs_stride;       /* size of one fileSystem in shared resource */
  uint32_t fs_count;        /* number of fileSystems */
} FwOffsets;

/* Returns the matching entry, or NULL if this firmware is unknown. */
const FwOffsets *offsets_find(uint32_t iofilemgr_nid);

/* An entry is usable when every field is filled in and it has been
 * verified (or JAMAISVU_ALLOW_UNVERIFIED_FW was set at build time). */
int offsets_usable(const FwOffsets *o, const char **why);

#endif
