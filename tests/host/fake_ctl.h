/* Test controls for tests/host/fake_kernel.c */
#ifndef FAKE_CTL_H
#define FAKE_CTL_H
#include <stdint.h>
extern uint8_t *fk_mc_image;
extern uint32_t fk_mc_sectors;
extern int fk_mc_present;
extern char fk_mc_mount[8];
extern char fk_mc_table[8];
extern int64_t fk_host_free;
void fk_reset(void);
void fk_add_host_mount(const char *root, const char *dir);
int fk_blocks_in_use(void);
int fk_fds_in_use(void);
/* path as FatFs sees the card, e.g. "ux0:data/JamaisVu/slot1.bin" */
int fk_card_has(const char *fatfs_path);

#endif
