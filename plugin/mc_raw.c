/* JamaisVu savestate plugin
 *
 * Read-only access to the Memory Card's user partition as a raw block
 * device, used as the disk for the plugin's read-only FatFs. Reading the
 * card directly (instead of through ux0:/uma0:) bypasses every OS cache,
 * so the plugin sees exactly the bytes the payload wrote.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <psp2kern/io/fcntl.h>
#include <string.h>

#include "ff.h"
#include "diskio.h"
#include "mc_raw.h"

#define SECTOR 512

static SceUID g_fd = -1;
static uint8_t __attribute__((aligned(64))) g_bounce[SECTOR * 32];

int mc_raw_open(void) {
  if (g_fd >= 0)
    return 0;
  g_fd = ksceIoOpen(MC_RAW_DEVICE, SCE_O_RDONLY, 0);
  return g_fd >= 0 ? 0 : g_fd;
}

void mc_raw_close(void) {
  if (g_fd >= 0)
    ksceIoClose(g_fd);
  g_fd = -1;
}

int mc_raw_read(void *buf, uint32_t sector, uint32_t count) {
  uint8_t *p = buf;

  if (g_fd < 0)
    return -1;

  while (count > 0) {
    uint32_t n = count > 32 ? 32 : count;
    if (ksceIoLseek(g_fd, (SceOff)sector * SECTOR, SCE_SEEK_SET) < 0)
      return -1;
    /* always through an aligned bounce buffer: FatFs may pass any
     * address, the storage driver wants DMA-friendly memory */
    if (ksceIoRead(g_fd, g_bounce, n * SECTOR) != (int)(n * SECTOR))
      return -1;
    memcpy(p, g_bounce, n * SECTOR);
    p += n * SECTOR;
    sector += n;
    count -= n;
  }
  return 0;
}

/* --- FatFs disk interface (drive 0 = Memory Card, read-only) ------------ */

DSTATUS disk_status(BYTE pdrv) {
  return (pdrv == 0 && g_fd >= 0) ? 0 : STA_NOINIT;
}

DSTATUS disk_initialize(BYTE pdrv) {
  return disk_status(pdrv);
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count) {
  if (pdrv != 0)
    return RES_PARERR;
  return mc_raw_read(buff, sector, count) == 0 ? RES_OK : RES_ERROR;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff) {
  (void)buff;
  if (pdrv != 0)
    return RES_PARERR;
  return cmd == CTRL_SYNC ? RES_OK : RES_PARERR;
}
