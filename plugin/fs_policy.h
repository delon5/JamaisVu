/* JamaisVu savestate plugin
 *
 * What to do with one open-file (vnode) table entry after a load, once it
 * has been locked and looked up again on its drive. Pure logic, host-tested
 * (tests/host/test_fs_policy.c); fs.c applies it.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __FS_POLICY_H__
#define __FS_POLICY_H__

#include <stdint.h>

/* vnode state / type bits (psp2kern/vfs.h; the kernel's own vnode reclaim
 * on 3.60 frees only entries with ref_count 0, the ACTIVE bit set and a
 * type of at most 0x1001, i.e. never DEV / MOUNTED vnodes) */
#define JV_VNODE_STATE_ACTIVE    0x00000001u
#define JV_VNODE_STATE_INACTIVE  0x00000100u
#define JV_VNODE_STATE_UNMOUNTED 0x00000200u
#define JV_VNODE_STATE_DELETED   0x00000400u
#define JV_VNODE_STATE_RECLAIMED 0x00002000u
#define JV_VNODE_STATE_NEW       0x00008000u
#define JV_VNODE_TYPE_REG        0x00000001u
#define JV_VNODE_TYPE_DIR        0x00000002u
#define JV_VNODE_TYPE_ROOT       0x00001000u
#define JV_VNODE_TYPE_DEV        0x00002000u
#define JV_VNODE_TYPE_MOUNTED    0x00004000u

typedef enum {
  JV_VN_LEAVE = 0, /* nothing to do, or not safe to touch: left as it is */
  JV_VN_REFRESH,   /* the file is still there: take its fresh size */
  JV_VN_FREE,      /* gone, nothing holds it: the entry can be given back */
  JV_VN_HOLD_RO,   /* gone but held open: its drive goes read-only */
} JvVnodeAction;

/* lookup_res: the drive's answer for the file (< 0: not found),
 * looked_up: 0 if the lookup could not even be made (drive lock failed) -
 * then the entry is left alone, never counted as gone.
 * refs / state / type: the entry's fields, read under its lock. */
JvVnodeAction fs_vnode_action(int lookup_res, int looked_up, uint32_t refs,
                              uint32_t state, uint32_t type);

#endif
