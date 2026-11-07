/* Host test: what fs.c does with an open-file table entry after a load. */

#include <stdio.h>
#include "fs_policy.h"

static int fails;
#define CHECK(c, name) do { \
    printf("  %-58s %s\n", name, (c) ? "ok" : "FAILED"); \
    if (!(c)) fails++; } while (0)

int main(void) {
  const uint32_t A = JV_VNODE_STATE_ACTIVE;
  printf("open-file table entries after a load\n");

  CHECK(fs_vnode_action(-4, 1, 0, A, JV_VNODE_TYPE_REG) == JV_VN_FREE,
        "gone, unreferenced, active file: freed");
  CHECK(fs_vnode_action(-4, 1, 0, A, JV_VNODE_TYPE_DIR) == JV_VN_FREE,
        "gone, unreferenced, active folder: freed");
  CHECK(fs_vnode_action(-4, 1, 2, A, JV_VNODE_TYPE_REG) == JV_VN_HOLD_RO,
        "gone but held open: drive read-only, entry kept");
  CHECK(fs_vnode_action(0, 1, 0, A, JV_VNODE_TYPE_REG) == JV_VN_REFRESH,
        "still there: size refreshed");
  CHECK(fs_vnode_action(0, 1, 3, A, JV_VNODE_TYPE_REG) == JV_VN_REFRESH,
        "still there and held open: size refreshed");
  CHECK(fs_vnode_action(-4, 0, 0, A, JV_VNODE_TYPE_REG) == JV_VN_LEAVE,
        "drive lock failed: not counted as gone, left alone");
  CHECK(fs_vnode_action(-4, 1, 0, JV_VNODE_STATE_NEW, JV_VNODE_TYPE_REG) ==
            JV_VN_LEAVE,
        "gone but the entry is being set up by the kernel (NEW): left");
  CHECK(fs_vnode_action(-4, 1, 0, A | JV_VNODE_STATE_RECLAIMED,
                        JV_VNODE_TYPE_REG) == JV_VN_LEAVE,
        "being reclaimed by the kernel: left");
  CHECK(fs_vnode_action(-4, 1, 0, A | JV_VNODE_STATE_UNMOUNTED,
                        JV_VNODE_TYPE_REG) == JV_VN_LEAVE,
        "unmounted: left");
  CHECK(fs_vnode_action(-4, 1, 0, 0, JV_VNODE_TYPE_REG) == JV_VN_LEAVE,
        "not active: left");
  CHECK(fs_vnode_action(-4, 1, 0, A, JV_VNODE_TYPE_DEV | 0x10) ==
            JV_VN_LEAVE,
        "device vnode: left");
  CHECK(fs_vnode_action(-4, 1, 0, A, JV_VNODE_TYPE_MOUNTED | JV_VNODE_TYPE_DIR)
            == JV_VN_LEAVE,
        "mount point: left");
  CHECK(fs_vnode_action(-4, 1, 0, A, JV_VNODE_TYPE_ROOT | JV_VNODE_TYPE_DIR)
            == JV_VN_LEAVE,
        "root folder: left");

  printf("%s\n", fails ? "fs policy tests FAILED" : "fs policy tests passed");
  return fails ? 1 : 0;
}
