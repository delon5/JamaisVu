/* Host tests for plugin/storage.c + plugin/slots.c: Memory Card detection,
 * where slots go (uma0 first), and the ARCHIVE copy to/from uma0.
 *
 * usage: test_storage card.img workdir
 */

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ff.h"
#include "fake_kernel.h"
#include "fake_ctl.h"
#include "../../JamaisVu.h"
#include "../../payload/crc32.h"
#include "storage.h"
#include "slots.h"

/* the os_* (read-write) FatFs, for inspecting the card like the OS would */
FRESULT os_f_open(FIL *fp, const TCHAR *path, BYTE mode);
FRESULT os_f_close(FIL *fp);
FRESULT os_f_lseek(FIL *fp, FSIZE_t ofs);
FRESULT os_f_stat(const TCHAR *path, FILINFO *fno);

static int failures, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; \
  printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
  printf("\n"); } } while (0)

static uint8_t *pristine;
static char workdir[256], d_ux0[300], d_uma0[300];

static void rm_rf(const char *dir) {
  char cmd[600];
  snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
  if (system(cmd)) {}
}

static void scenario(const char *name, const char *mc_mount, int mc_present,
                     int sd_ux0, int usb_uma0) {
  printf("%s\n", name);
  fk_reset();
  memcpy(fk_mc_image, pristine, (size_t)fk_mc_sectors * 512);
  strcpy(fk_mc_mount, mc_mount);
  fk_mc_present = mc_present;
  rm_rf(d_ux0);
  rm_rf(d_uma0);
  mkdir(d_ux0, 0777);
  mkdir(d_uma0, 0777);
  if (sd_ux0) fk_add_host_mount("ux0:", d_ux0);
  if (usb_uma0) fk_add_host_mount("uma0:", d_uma0);
  fk_host_free = 4ll << 30;
}

static int host_file_exists(const char *dir, const char *rel) {
  char p[600];
  snprintf(p, sizeof(p), "%s/%s", dir, rel);
  return access(p, F_OK) == 0;
}

static int64_t host_file_size(const char *dir, const char *rel) {
  char p[600];
  struct stat s;
  snprintf(p, sizeof(p), "%s/%s", dir, rel);
  return stat(p, &s) == 0 ? s.st_size : -1;
}

static int card_has(const char *path) {
  FILINFO fno;
  return os_f_stat(path, &fno) == FR_OK;
}

static void no_probes_left(void) {
  CHECK(!card_has("ux0:data/JamaisVu/probe.bin"), "probe left on card");
  CHECK(!host_file_exists(d_ux0, "data/JamaisVu/probe.bin"), "probe left on ux0 dir");
  CHECK(!host_file_exists(d_uma0, "data/JamaisVu/probe.bin"), "probe left on uma0 dir");
}

/* What the payload does: write the state straight into the work file's
 * clusters on the card, bypassing the OS. */
static void raw_write_work(const uint8_t *data, uint32_t len) {
  static DWORD tbl[256];
  FIL f;
  CHECK(os_f_open(&f, "ux0:data/JamaisVu/work.bin", FA_READ) == FR_OK,
        "open work.bin");
  tbl[0] = 256;
  f.cltbl = tbl;
  CHECK(os_f_lseek(&f, CREATE_LINKMAP) == FR_OK, "linkmap");
  FATFS *fs = f.obj.fs;
  uint32_t off = 0;
  for (DWORD *t = &tbl[1]; *t && off < len; t += 2) {
    uint64_t sec = fs->database + (uint64_t)(t[1] - 2) * fs->csize;
    uint64_t bytes = (uint64_t)t[0] * fs->csize * 512;
    uint32_t n = (uint32_t)(len - off < bytes ? len - off : bytes);
    memcpy(fk_mc_image + sec * 512, data + off, n);
    off += n;
  }
  f.cltbl = NULL;
  os_f_close(&f);
  CHECK(off == len, "work.bin too small for test state");
}

static uint8_t *make_state(uint32_t data_size, uint32_t seed, uint32_t *len) {
  uint32_t used = (1024 + data_size + 511) & ~511u;
  uint8_t *buf = calloc(1, used);
  SaveStateHeader *h = (SaveStateHeader *)buf;
  for (uint32_t i = 512; i < used; i++)
    buf[i] = (uint8_t)((i * 2654435761u) >> 24 ^ seed);
  h->magic = SAVESTATE_MAGIC;
  h->version = SAVESTATE_VERSION;
  h->header_size = sizeof(SaveStateHeader);
  h->data_offset = 1024;
  h->data_size = data_size;
  h->header_crc = crc32_update(0, h, offsetof(SaveStateHeader, header_crc));
  *len = used;
  return buf;
}

int main(int argc, char **argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  if (argc < 3) return 2;

  FILE *f = fopen(argv[1], "rb");
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  fk_mc_sectors = size / 512;
  fk_mc_image = malloc(size);
  pristine = malloc(size);
  if (fread(pristine, 1, size, f) != (size_t)size) return 2;
  fclose(f);

  snprintf(workdir, sizeof(workdir), "%s", argv[2]);
  snprintf(d_ux0, sizeof(d_ux0), "%s/sd_ux0", workdir);
  snprintf(d_uma0, sizeof(d_uma0), "%s/usb_uma0", workdir);

  static char path[640];
  StorageMode m;

  /* ------------------------------------------------------------------ */
  scenario("Memory Card as ux0, no uma0", "ux0:", 1, 0, 0);
  m = storage_detect();
  CHECK(m == STORAGE_DIRECT, "mode %d", m);
  CHECK(storage_mc_root() && !strcmp(storage_mc_root(), "ux0:"), "mc root");
  CHECK(storage_slot_root() && !strcmp(storage_slot_root(), "ux0:"), "slot root");
  slot_payload_path(path, sizeof(path), 2);
  CHECK(!strcmp(path, "ux0:data/JamaisVu/slot3.bin"), "payload path %s", path);
  CHECK(slots_mkdirs() == 0, "mkdirs");
  CHECK(slot_prepare(2, NULL) == 0, "prepare");
  CHECK(card_has("ux0:data/JamaisVu/slot3.bin"), "slot file on card");
  slots_sync(); /* must not recurse */
  no_probes_left();

  /* ------------------------------------------------------------------ */
  scenario("Memory Card as uma0, SD2Vita as ux0", "uma0:", 1, 1, 0);
  m = storage_detect();
  CHECK(m == STORAGE_DIRECT, "mode %d", m);
  CHECK(storage_mc_root() && !strcmp(storage_mc_root(), "uma0:"), "mc root %s", storage_mc_root());
  CHECK(storage_slot_root() && !strcmp(storage_slot_root(), "uma0:"), "slot root");
  CHECK(slots_mkdirs() == 0, "mkdirs");
  CHECK(slot_prepare(0, NULL) == 0, "prepare");
  CHECK(card_has("ux0:data/JamaisVu/slot1.bin"), "slot file on the card (uma0)");
  CHECK(!host_file_exists(d_ux0, "data/JamaisVu/slot1.bin"), "slot file on the SD card");
  slots_sync();
  no_probes_left();

  /* ------------------------------------------------------------------ */
  scenario("Memory Card as ux0, USB/SD2Vita as uma0 (archive)", "ux0:", 1, 0, 1);
  m = storage_detect();
  CHECK(m == STORAGE_ARCHIVE, "mode %d", m);
  CHECK(storage_mc_root() && !strcmp(storage_mc_root(), "ux0:"), "mc root");
  CHECK(storage_slot_root() && !strcmp(storage_slot_root(), "uma0:"), "slot root");
  no_probes_left();
  slot_payload_path(path, sizeof(path), 1);
  CHECK(!strcmp(path, "ux0:data/JamaisVu/work.bin"), "payload path %s", path);
  CHECK(slots_mkdirs() == 0, "mkdirs");
  CHECK(work_available(), "work available");
  CHECK(slot_prepare(1, NULL) == 0, "prepare work");
  CHECK(card_has("ux0:data/JamaisVu/work.bin"), "work.bin on card");
  CHECK(!host_file_exists(d_uma0, "data/JamaisVu/slot2.bin"), "no slot yet");
  {
    uint8_t nonce[16];
    CHECK(slot_stamp(1, nonce) == 0, "stamp work");
  }

  uint32_t len1, len2, alen = 0, acrc = 0;
  uint8_t *st1 = make_state(3 * 1024 * 1024 + 77, 0x11, &len1);
  raw_write_work(st1, len1);
  int r = storage_archive_from_work(1, &alen, &acrc, NULL);
  CHECK(r == 0, "archive: %d", r);
  CHECK(alen == len1, "archived length %u vs %u", alen, len1);
  CHECK(acrc == crc32_update(0, st1, len1), "archived crc");
  CHECK(host_file_size(d_uma0, "data/JamaisVu/slot2.bin") == len1, "slot2.bin size");
  CHECK(!host_file_exists(d_uma0, "data/JamaisVu/slot2.tmp"), "tmp left");
  {
    FILE *g;
    snprintf(path, sizeof(path), "%s/data/JamaisVu/slot2.bin", d_uma0);
    g = fopen(path, "rb");
    uint8_t *got = malloc(len1);
    CHECK(g && fread(got, 1, len1, g) == len1 && !memcmp(got, st1, len1),
          "slot2.bin content");
    if (g) fclose(g);
    free(got);
  }
  SlotInfo info;
  static uint32_t thumb[JV_THUMB_W * JV_THUMB_H];
  memset(&info, 0, sizeof(info));
  strcpy(info.titleid, "PCSE00123");
  info.state_len = alen;
  info.state_crc = acrc;
  CHECK(slot_info_write(1, &info, thumb) == 0, "info write");
  CHECK(slot_is_loadable(1), "slot loadable");
  CHECK(slot_file_exists(1), "slot exists");

  /* a later save overwrote the work file */
  uint8_t *st2 = make_state(1024 * 1024, 0x22, &len2);
  raw_write_work(st2, len2);

  /* load: slot 2 back into the work file, verified raw */
  r = storage_restore_to_work(1, info.state_len, info.state_crc, NULL);
  CHECK(r == 0, "restore: %d", r);
  {
    /* read the card the way the payload does */
    FIL wf;
    UINT br;
    uint8_t *got = malloc(len1);
    CHECK(os_f_open(&wf, "ux0:data/JamaisVu/work.bin", FA_READ) == FR_OK, "open");
    os_f_read(&wf, got, len1, &br);
    os_f_close(&wf);
    CHECK(br == len1 && !memcmp(got, st1, len1), "work.bin holds slot 2 again");
    free(got);
  }

  /* damaged slot copy on uma0: refused */
  {
    snprintf(path, sizeof(path), "%s/data/JamaisVu/slot2.bin", d_uma0);
    FILE *g = fopen(path, "r+b");
    fseek(g, 5000, SEEK_SET);
    fputc(0x5a ^ st1[5000], g);
    fclose(g);
    r = storage_restore_to_work(1, info.state_len, info.state_crc, NULL);
    printf("  %-44s -> %d\n", "restore from damaged slot", r);
    CHECK(r == JV_ERR_ARCHIVE_BAD, "damaged slot: %d", r);
    g = fopen(path, "r+b");
    fseek(g, 5000, SEEK_SET);
    fputc(st1[5000], g);
    fclose(g);
  }

  /* failed new save (no valid header): old slot untouched */
  {
    uint8_t junk[2048];
    memset(junk, 0xee, sizeof(junk));
    raw_write_work(junk, sizeof(junk));
    uint32_t l, c;
    r = storage_archive_from_work(1, &l, &c, NULL);
    printf("  %-44s -> %d\n", "archive of an invalid work file", r);
    CHECK(r == JV_ERR_ARCHIVE_BAD, "invalid work: %d", r);
    CHECK(host_file_size(d_uma0, "data/JamaisVu/slot2.bin") == len1 &&
          slot_is_loadable(1), "old slot must survive a failed save");
  }

  /* not enough space on uma0: old slot untouched */
  {
    raw_write_work(st2, len2);
    fk_host_free = len2; /* less than len + margin */
    uint32_t l, c;
    r = storage_archive_from_work(1, &l, &c, NULL);
    printf("  %-44s -> %d\n", "archive without space on uma0", r);
    CHECK(r == JV_ERR_SLOT_NO_SPACE, "no space: %d", r);
    CHECK(slot_is_loadable(1), "old slot must survive");
    fk_host_free = 4ll << 30;
  }

  /* overwrite slot 2 with the second state */
  r = storage_archive_from_work(1, &alen, &acrc, NULL);
  CHECK(r == 0 && alen == len2, "re-archive: %d", r);
  CHECK(host_file_size(d_uma0, "data/JamaisVu/slot2.bin") == len2, "slot replaced");
  slots_sync();
  CHECK(fk_blocks_in_use() == 0, "memory blocks leaked: %d", fk_blocks_in_use());
  CHECK(fk_fds_in_use() == 0, "file descriptors leaked: %d", fk_fds_in_use());
  free(st1);
  free(st2);

  /* ------------------------------------------------------------------ */
  scenario("no Memory Card inserted", "", 0, 1, 1);
  m = storage_detect();
  printf("  -> mode %d, \"%s\"\n", m, storage_reason());
  CHECK(m == STORAGE_NONE && strstr(storage_reason(), "no Memory Card"), "no card");
  CHECK(!slot_is_loadable(0), "nothing loadable");
  no_probes_left();

  scenario("Memory Card inserted but not mounted", "", 1, 1, 1);
  m = storage_detect();
  printf("  -> mode %d, \"%s\"\n", m, storage_reason());
  CHECK(m == STORAGE_NONE && strstr(storage_reason(), "not mounted"), "unmounted card");
  no_probes_left();

  scenario("Memory Card as xmc0, SD2Vita as ux0, no uma0", "xmc0:", 1, 1, 0);
  m = storage_detect();
  CHECK(m == STORAGE_DIRECT && !strcmp(storage_mc_root(), "xmc0:"), "xmc0");

  CHECK(fk_blocks_in_use() == 0, "memory blocks leaked");
  CHECK(fk_fds_in_use() == 0, "file descriptors leaked");

  printf("\n%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
