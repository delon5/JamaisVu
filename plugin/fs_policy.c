/* JamaisVu savestate plugin
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include "fs_policy.h"

JvVnodeAction fs_vnode_action(int lookup_res, int looked_up, uint32_t refs,
                              uint32_t state, uint32_t type) {
  if (!looked_up)
    return JV_VN_LEAVE; /* not an answer about the file */
  if (lookup_res >= 0)
    return JV_VN_REFRESH;
  if (refs != 0)
    return JV_VN_HOLD_RO;
  /* gone and unreferenced: only an ordinary, active file or folder entry
   * is given back - the same filter as the kernel's reclaim; anything in
   * another state (new, being reclaimed, unmounted, deleted) or a device /
   * root / mount-point vnode is left to the kernel */
  if (!(state & JV_VNODE_STATE_ACTIVE) ||
      (state & (JV_VNODE_STATE_NEW | JV_VNODE_STATE_RECLAIMED |
                JV_VNODE_STATE_UNMOUNTED | JV_VNODE_STATE_DELETED |
                JV_VNODE_STATE_INACTIVE)))
    return JV_VN_LEAVE;
  if (type & (JV_VNODE_TYPE_DEV | JV_VNODE_TYPE_MOUNTED | JV_VNODE_TYPE_ROOT))
    return JV_VN_LEAVE;
  if (type > 0x1001u)
    return JV_VN_LEAVE;
  return JV_VN_FREE;
}
