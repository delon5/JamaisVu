/* JamaisVu savestate plugin
 *
 * PS TV progress screen: our HDMI picture comes out as four squashed
 * copies in the top half with every other line dark and the colours
 * shifted (user's photos, display test and a real save, 720p and 1080p).
 * The setup is xerpi's libbaremetal unchanged. To see what the system
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

/* appends "off=value" for each register offset (word index) */
static int words(char *out, int len, const char *title,
                 volatile const uint32_t *regs, const uint16_t *idx, int n,
                 int byte_offsets) {
  int k = snprintf(out, len, "    hdmi %s:", title);
  for (int i = 0; i < n && k > 0 && k < len - 16; i++)
    k += snprintf(out + k, len - k, " %X=%08X",
                  (unsigned)(byte_offsets ? idx[i] * 4u : idx[i]),
                  (unsigned)regs[idx[i]]);
  if (k > 0 && k < len - 3)
    k += snprintf(out + k, len - k, "\r\n");
  return k;
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

void hwdump_hdmi(void (*log)(const char *)) {
  static char line[900];
  SceUID uid;
  int err = 0;

  /* IFTU bus 1: planes A (0x0000), B (0x1000), control (0x2000) */
  volatile uint32_t *iftu = map_io(0xE5030000, 0x3000, &uid, &err);
  if (!iftu) {
    snprintf(line, sizeof(line), "    hdmi: display controller registers "
             "not readable (0x%08X)\r\n", (unsigned)err);
    log(line);
  } else {
    static const uint16_t cregs[] = { 0x800, 0x801, 0x804, 0x805, 0x806,
                                      0x807, 0x808 };
    static const uint16_t plane[] = { 0x23, 0x28, 0x40, 0x41, 0x42, 0x43,
                                      0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
                                      0x4A, 0x4B, 0x4C, 0x4D, 0x4E, 0x4F,
                                      0x50, 0x51, 0x52, 0x53, 0x54, 0x55,
                                      0x56, 0x57, 0x58, 0x59, 0x5A };
    /* plane config 0 and 1 (0x200 / 0x300): every word we write and the
     * unknown ones between them */
    static const uint16_t cfg[] = { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05,
                                    0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B,
                                    0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11,
                                    0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
                                    0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D,
                                    0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23,
                                    0x24, 0x25, 0x26, 0x27, 0x28, 0x29,
                                    0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F,
                                    0x30, 0x31, 0x32, 0x33, 0x34, 0x35 };
    words(line, sizeof(line), "IFTU control (byte offsets)",
          iftu, cregs, 7, 1);
    log(line);
    words(line, sizeof(line), "IFTU plane A (byte offsets)", iftu, plane,
          (int)(sizeof(plane) / sizeof(plane[0])), 1);
    log(line);
    for (int c = 0; c < 2; c++) {
      words(line, sizeof(line), c ? "IFTU plane A config 1 (byte offsets)"
                                  : "IFTU plane A config 0 (byte offsets)",
            iftu + (0x200 + c * 0x100) / 4, cfg,
            (int)(sizeof(cfg) / sizeof(cfg[0])), 1);
      log(line);
    }
    words(line, sizeof(line), "IFTU plane B config 0 (byte offsets)",
          iftu + (0x1000 + 0x200) / 4, cfg, 20, 1);
    log(line);
    ksceKernelFreeMemBlock(uid);
  }

  /* DSI bus 1: timing (0x00-0x20), control (0x140-0x146), lane setup
   * (0x200-0x20F, 0x240-0x253) - word indexes as in dsi.c */
  volatile uint32_t *dsi = map_io(0xE5060000, 0x1000, &uid, &err);
  if (!dsi) {
    snprintf(line, sizeof(line), "    hdmi: display link registers not "
             "readable (0x%08X)\r\n", (unsigned)err);
    log(line);
  } else {
    static const uint16_t t[] = { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
                                  0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D,
                                  0x0E, 0x0F, 0x10, 0x11, 0x13, 0x14, 0x15,
                                  0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C,
                                  0x1D, 0x1E, 0x1F, 0x20 };
    static const uint16_t l[] = { 0x140, 0x141, 0x142, 0x143, 0x144, 0x145,
                                  0x146, 0x200, 0x201, 0x202, 0x203, 0x204,
                                  0x205, 0x206, 0x207, 0x208, 0x209, 0x20A,
                                  0x20B, 0x20C, 0x20D, 0x20E, 0x20F, 0x240,
                                  0x241, 0x242, 0x243, 0x250, 0x251, 0x252,
                                  0x253 };
    words(line, sizeof(line), "DSI timing (word index)", dsi, t,
          (int)(sizeof(t) / sizeof(t[0])), 0);
    log(line);
    words(line, sizeof(line), "DSI setup (word index)", dsi, l,
          (int)(sizeof(l) / sizeof(l[0])), 0);
    log(line);
    ksceKernelFreeMemBlock(uid);
  }

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
