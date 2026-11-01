/* JamaisVu: stands in for libbaremetal's sysroot.h. The payload has no
 * sysroot buffer of its own; the plugin passes the two values lcd.c needs
 * (from ksceSysrootGetHardwareInfo / ksceSysrootGetHardwareFlags). */
#ifndef SYSROOT_H
#define SYSROOT_H

#include <stdint.h>

extern uint32_t g_disp_hw_info;
extern uint32_t g_disp_hw_flags0;

static inline uint32_t sysroot_get_hw_info(void)
{
	return g_disp_hw_info;
}

static inline int sysroot_is_au_codec_ic_conexant(void)
{
	return (g_disp_hw_flags0 & 3) == 1;
}

#endif
