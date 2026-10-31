/* JamaisVu savestate plugin
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stddef.h>
#include "offsets.h"

/* The exFAT driver offsets below are the ones the original plugin used
 * unconditionally; they were only ever tested on 3.65. */
#define EXFAT_365                                   \
  .exfat_mount = 0x7169,                            \
  .exfat_read_management_area = 0x13f01,            \
  .exfat_search_file_common = 0x10e39,              \
  .exfat_get_parent_fnode = 0x7a79,                 \
  .exfat_release_fnode = 0x7ae9,                    \
  .exfat_vop_umount = 0x5f95,                       \
  .exfat_cleanup_cluster_cache = 0x2695,            \
  .exfat_invalidate_cluster_cache = 0x2191,         \
  .exfat_free_fp_cache = 0x7ebd,                    \
  .exfat_sfree = 0x68a5,                            \
  .exfat_utf16_to_utf8 = 0x19e1,                    \
  .exfat_write_device = 0x1315c,                    \
  .exfat_read_mgmt_patch = 0x140fe,                 \
  .exfat_shared_resource = 0x80,                    \
  .rtc_ctx = 0,                                     \
  .sr_num_files = 0x295d5c,                         \
  .sr_files = 0x5cd0,                               \
  .sr_num_fnodes = 0x295d54,                        \
  .sr_fnodes = 0x5cd8,                              \
  .fnode_size = 0x290,                              \
  .fs_stride = 0x430,                               \
  .fs_count = 14

#define IOFILEMGR_DATA_365            \
  .iofilemgr_num_vnodes = 0x24,       \
  .iofilemgr_vnodes = 0x19dc,         \
  .iofilemgr_vnode_size = 0x19e0,     \
  .iofilemgr_mnt_head = 0x19fc

static const FwOffsets g_table[] = {
  {
    /* 3.60: SceIofilemgr function offsets were already known to the
     * original plugin. Everything else must be located in the 3.60
     * modules before this entry can be enabled (docs/porting-3.60.md).
     * Zero = unknown. */
    .name = "3.60",
    .iofilemgr_nids = { 0x9642948C, 0 },
    .verified = 0,
    .iofilemgr_clear_ncache = 0x5a39,
    .iofilemgr_invalidate_buff_cache = 0x126f9,
  },
  {
    .name = "3.65",
    .iofilemgr_nids = { 0xA96ACE9D, 0 },
    .verified = 1, /* the only firmware the original author tested */
    .iofilemgr_clear_ncache = 0x3a11,
    .iofilemgr_invalidate_buff_cache = 0x106d1,
    IOFILEMGR_DATA_365,
    EXFAT_365,
  },
  {
    .name = "3.67-3.68",
    .iofilemgr_nids = { 0x3347A95F, 0x90DA33DE, 0 },
    .verified = 0,
    .iofilemgr_clear_ncache = 0x3a11,
    .iofilemgr_invalidate_buff_cache = 0x106d1,
    IOFILEMGR_DATA_365,
    EXFAT_365,
  },
  {
    .name = "3.69-3.73",
    .iofilemgr_nids = { 0xF16E72C7, 0x81A49C2B, 0xF2D59083, 0x9C16D40A,
                        0xF7794A6C },
    .verified = 0,
    .iofilemgr_clear_ncache = 0x3e69,
    .iofilemgr_invalidate_buff_cache = 0x10b29,
    IOFILEMGR_DATA_365,
    EXFAT_365,
  },
};

const FwOffsets *offsets_find(uint32_t iofilemgr_nid) {
  for (size_t i = 0; i < sizeof(g_table) / sizeof(g_table[0]); i++) {
    for (int j = 0; j < 5 && g_table[i].iofilemgr_nids[j]; j++) {
      if (g_table[i].iofilemgr_nids[j] == iofilemgr_nid)
        return &g_table[i];
    }
  }
  return NULL;
}

int offsets_usable(const FwOffsets *o, const char **why) {
  if (!o) {
    *why = "unknown firmware";
    return 0;
  }

  /* rtc_ctx may legitimately be 0 (start of the data segment). */
  if (!o->iofilemgr_clear_ncache || !o->iofilemgr_invalidate_buff_cache ||
      !o->iofilemgr_num_vnodes || !o->iofilemgr_vnodes ||
      !o->iofilemgr_vnode_size ||
      !o->exfat_mount ||
      !o->exfat_search_file_common || !o->exfat_get_parent_fnode ||
      !o->exfat_release_fnode ||
      !o->exfat_cleanup_cluster_cache || !o->exfat_invalidate_cluster_cache ||
      !o->exfat_free_fp_cache || !o->exfat_sfree ||
      !o->exfat_write_device ||
      !o->exfat_read_mgmt_patch || !o->exfat_shared_resource ||
      !o->sr_num_files || !o->sr_files || !o->sr_num_fnodes ||
      !o->sr_fnodes || !o->fnode_size || !o->fs_stride || !o->fs_count) {
    *why = "offsets for this firmware are incomplete";
    return 0;
  }

#ifndef JAMAISVU_ALLOW_UNVERIFIED_FW
  if (!o->verified) {
    *why = "offsets for this firmware are not verified on hardware";
    return 0;
  }
#endif

  *why = "ok";
  return 1;
}
