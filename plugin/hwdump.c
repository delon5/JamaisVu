/* JamaisVu savestate plugin
 *
 * PS TV progress screen: our HDMI picture comes out as four squashed
 * copies in the top half with every other line dark and the colours
 * shifted (user's photos, display test and a real save, 720p and 1080p).
 * The setup was then xerpi's libbaremetal unchanged. To see what the system
 * itself sets that we do not, this reads - never writes - the system's
 * live HDMI configuration while its picture is on screen: the display
 * controller (IFTU bus 1), the display link (DSI bus 1) and the HDMI
 * bridge (ADV7533 on I2C bus 1, through the system's own I2C driver).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <psp2kern/kernel/modulemgr.h>
#include <psp2kern/kernel/sysmem.h>
#include <taihen.h>

#include <stdio.h>
#include <string.h>

#include "main.h"
#include "hwdump.h"

/* SceKernelAllocMemBlockKernelOpt (3.60), only the fields used */
typedef struct {
  uint32_t size, field_4, attr, field_C, paddr, alignment;
  uint32_t rest[16];
} JvMemOpt;
_Static_assert(sizeof(JvMemOpt) == 0x58, "3.60 option size");

#define MEMBLOCK_KERNEL_IO_SO_R 0x20100204 /* device memory, read-only */
#define MEMBLOCK_ATTR_HAS_PADDR 0x2

static volatile uint32_t *map_io(uint32_t paddr, uint32_t size, SceUID *uid,
                                 int *err) {
  JvMemOpt opt;
  void *base = NULL;
  memset(&opt, 0, sizeof(opt));
  opt.size = sizeof(opt);
  opt.attr = MEMBLOCK_ATTR_HAS_PADDR;
  opt.paddr = paddr;
  *uid = ksceKernelAllocMemBlock("JamaisVuHwDump", MEMBLOCK_KERNEL_IO_SO_R,
                                 size, (void *)&opt);
  if (*uid < 0) {
    *err = *uid;
    return NULL;
  }
  if (ksceKernelGetMemBlockBase(*uid, &base) < 0 || !base) {
    ksceKernelFreeMemBlock(*uid);
    *uid = -1;
    *err = -1;
    return NULL;
  }
  return base;
}

static int (*g_i2c_wr)(int bus, unsigned int waddr, unsigned char *wbuf,
                       int wsize, unsigned int raddr, unsigned char *rbuf,
                       int rsize);

static int adv_regs(char *out, int len, const char *title, unsigned addr,
                    const uint8_t *regs, int n) {
  int k = snprintf(out, len, "    hdmi %s (I2C 0x%02X):", title, addr);
  for (int i = 0; i < n && k > 0 && k < len - 12; i++) {
    unsigned char r = regs[i], v = 0;
    int ret = g_i2c_wr(1, addr, &r, 1, addr, &v, 1);
    if (ret < 0)
      k += snprintf(out + k, len - k, " %02X=err%08X", regs[i], (unsigned)ret);
    else
      k += snprintf(out + k, len - k, " %02X=%02X", regs[i], v);
  }
  if (k > 0 && k < len - 3)
    k += snprintf(out + k, len - k, "\r\n");
  return k;
}

/* the non-zero words of a register snapshot, by range */
void hwdump_regs(void (*log)(const char *), const char *who,
                 const uint32_t *words) {
  static char line[700];
  static const JvRegRange r[] = JV_REGSNAP_RANGES;
  uint32_t k = 0;
  for (uint32_t n = 0; n < sizeof(r) / sizeof(r[0]); n++) {
    int len = snprintf(line, sizeof(line), "    hdmi %s %08X:", who,
                       (unsigned)r[n].paddr);
    int any = 0;
    for (uint32_t w = 0; w < r[n].words; w++, k++) {
      if (!words[k])
        continue;
      if (len > (int)sizeof(line) - 20) {
        snprintf(line + len, sizeof(line) - len, "\r\n");
        log(line);
        len = snprintf(line, sizeof(line), "    hdmi %s %08X (cont.):", who,
                       (unsigned)r[n].paddr);
      }
      len += snprintf(line + len, sizeof(line) - len, " %X=%X",
                      (unsigned)(w * 4), (unsigned)words[k]);
      any = 1;
    }
    snprintf(line + len, sizeof(line) - len, any ? "\r\n" : " all 0\r\n");
    log(line);
  }
}

void hwdump_hdmi(void (*log)(const char *)) {
  static char line[900];
  SceUID uid;
  int err = 0;

  /* every IFTU bus 1 / DSI bus 1 register in the snapshot ranges (only
   * the non-zero ones are logged), the same ranges the payload copies */
  static uint32_t sys[JV_REGSNAP_WORDS];
  static const JvRegRange r[] = JV_REGSNAP_RANGES;
  SceUID uid2 = -1;
  int err2 = 0;
  volatile uint32_t *iftu = map_io(0xE5030000, 0x3000, &uid, &err);
  volatile uint32_t *dsi = map_io(0xE5060000, 0x1000, &uid2, &err2);
  if (!iftu || !dsi) {
    snprintf(line, sizeof(line), "    hdmi: registers not readable (0x%08X "
             "0x%08X)\r\n", (unsigned)err, (unsigned)err2);
    log(line);
  } else {
    uint32_t k = 0;
    for (uint32_t n = 0; n < sizeof(r) / sizeof(r[0]); n++) {
      volatile uint32_t *b = r[n].paddr >= 0xE5060000
                                 ? dsi + (r[n].paddr - 0xE5060000) / 4
                                 : iftu + (r[n].paddr - 0xE5030000) / 4;
      for (uint32_t w = 0; w < r[n].words; w++)
        sys[k++] = b[w];
    }
    hwdump_regs(log, "system", sys);
  }
  if (iftu)
    ksceKernelFreeMemBlock(uid);
  if (dsi)
    ksceKernelFreeMemBlock(uid2);

  /* ADV7533 through the system's I2C driver (it serialises the bus) */
  if (!g_i2c_wr &&
      module_get_export_func(KERNEL_PID, "SceLowio", 0xE14BEF6E, 0x0A40B7BF,
                             (uintptr_t *)&g_i2c_wr) < 0)
    g_i2c_wr = NULL;
  if (!g_i2c_wr) {
    log("    hdmi: I2C driver function not found\r\n");
    return;
  }
  static const uint8_t main_regs[] = { 0x15, 0x16, 0x17, 0x18, 0x3B, 0x3C,
                                       0x40, 0x41, 0x42, 0x48, 0x49, 0x4C,
                                       0x55, 0x56, 0x57, 0x58, 0x59, 0x9A,
                                       0xAF, 0xBA, 0xD0, 0xD5, 0xD6, 0xDE,
                                       0xE1, 0xE4, 0xE5, 0xF9, 0xFA };
  adv_regs(line, sizeof(line), "bridge main", 0x7A, main_regs,
           (int)sizeof(main_regs));
  log(line);
  static const uint8_t cec_regs[] = { 0x03, 0x05, 0x15, 0x16, 0x17, 0x1C,
                                      0x24, 0x27, 0x28, 0x29, 0x2A, 0x2B,
                                      0x2C, 0x2D, 0x2E, 0x2F, 0x30, 0x31,
                                      0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
                                      0x55, 0x57 };
  adv_regs(line, sizeof(line), "bridge CEC/DSI", 0x7C, cec_regs,
           (int)sizeof(cec_regs));
  log(line);
}
