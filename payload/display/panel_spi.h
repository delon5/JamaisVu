/* JamaisVu savestate plugin
 *
 * The panel command channel shared by the PCH-1000 OLED and the
 * PCH-2000 LCD: SPI bus 2 with 9-bit frames (see panel_spi.c), and the
 * little scripts the two panel files are written as.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef JV_PANEL_SPI_H
#define JV_PANEL_SPI_H

#include <stdint.h>

/* step of a panel script: a command with n data bytes, or a pause of n
 * ticks (one tick = the gap the system leaves between commands) */
#define CMD(c, n) (c), (n)
#define PAUSE(n)  0xFE, (n)
#define END       0xFF, 0

#define PANEL_OLED 0
#define PANEL_LCD  1

void panel_spi_init(int kind);  /* the bus set up, clocked only while sending */
void panel_send(uint8_t cmd, const uint8_t *data, int n);
/* bytes the panel answered to the last command (header_bits skipped) */
void panel_read(uint8_t *buf, int n, int header_bits);
void panel_run(const uint8_t *script);

#endif
