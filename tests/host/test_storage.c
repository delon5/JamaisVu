/* Host tests for plugin/storage.c + plugin/slots.c:
 *  - the Memory Card is found from the mount table, in every layout
 *  - detection never writes anything
 *  - every file JamaisVu writes lands on the official Memory Card and
 *    nowhere else (SD2Vita / PSVSD / USB storage stays untouched)
 *  - game version (param.sfo APP_VER) lookup and matching
 *
 * usage: test_storage card.img workdir
 */

#include <dirent.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fake_kernel.h"
#include "fake_ctl.h"
#include "storage.h"
#include "slots.h"
#include "trace.h"

JvIoMountPoint *fk_find_mount_point(int id);

static int failures, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; \
  printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
  printf("\n"); } } while (0)

static uint8_t *pristine;
static char d_ux0[300], d_uma0[300];

static void rm_rf(const char *dir) {
  DIR *d = opendir(dir);
  struct dirent *e;
  char p[700];
  if (!d) return;
  while ((e = readdir(d))) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
    struct stat st;
    if (lstat(p, &st) == 0 && S_ISDIR(st.st_mode)) rm_rf(p);
    else unlink(p);
  }
  closedir(d);
  rmdir(dir);
}

static int count_entries(const char *dir) {
  DIR *d = opendir(dir);
  struct dirent *e;
  char p[700];
  int n = 0;
  if (!d) return 0;
  while ((e = readdir(d))) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    n++;
    snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
    struct stat st;
    if (lstat(p, &st) == 0 && S_ISDIR(st.st_mode)) n += count_entries(p);
  }
  closedir(d);
  return n;
}

static void scenario(const char *name, const char *mc_mount, int sd_ux0,
                     int usb_uma0) {
  printf("%s\n", name);
  fk_reset();
  memcpy(fk_mc_image, pristine, (size_t)fk_mc_sectors * 512);
  strcpy(fk_mc_mount, mc_mount);
  fk_mc_present = mc_mount[0] != 0;
  rm_rf(d_ux0);
  rm_rf(d_uma0);
  mkdir(d_ux0, 0777);
  mkdir(d_uma0, 0777);
  if (sd_ux0) fk_add_host_mount("ux0:", d_ux0);
  if (usb_uma0) fk_add_host_mount("uma0:", d_uma0);
}

static int card_has(const char *path) {
  return fk_card_has(path);
}

/* detection must not write anything anywhere */
static const char *detect_readonly(void) {
  uint8_t *before = malloc((size_t)fk_mc_sectors * 512);
  memcpy(before, fk_mc_image, (size_t)fk_mc_sectors * 512);
  int a = count_entries(d_ux0), b = count_entries(d_uma0);
  const char *root = storage_detect();
  CHECK(!memcmp(before, fk_mc_image, (size_t)fk_mc_sectors * 512),
        "detection wrote to the Memory Card");
  CHECK(count_entries(d_ux0) == a && count_entries(d_uma0) == b,
        "detection wrote to other storage");
  free(before);
  return root;
}

/* Every write path JamaisVu has: folders, slot file, token, slot info.
 * Afterwards the other storage must still be completely empty. */
static void exercise_writes(int slot) {
  static uint32_t thumb[JV_THUMB_W * JV_THUMB_H];
  static char want[64];
  SlotInfo info;
  uint8_t nonce[16];

  CHECK(slots_mkdirs() == 0, "mkdirs");
  CHECK(slot_prepare(slot, NULL) == 0, "prepare");
  CHECK(slot_stamp(slot, nonce) == 0, "stamp");
  memset(&info, 0, sizeof(info));
  strcpy(info.titleid, "PCSE00123");
  strcpy(info.title, "Test Game");
  strcpy(info.app_ver, "01.02");
  CHECK(slot_info_write(slot, &info, thumb) == 0, "info write");
  CHECK(slot_is_loadable(slot), "loadable");
  CHECK(slot_file_exists(slot), "exists");
  {
    SlotInfo back;
    CHECK(slot_info_read(slot, &back, thumb) == 0 &&
          !strcmp(back.app_ver, "01.02"), "version stored with the slot");
  }
  slots_sync(); /* must not recurse */

  snprintf(want, sizeof(want), "ux0:data/JamaisVu/slot%d.bin", slot + 1);
  CHECK(card_has(want), "slot file is on the Memory Card");
  snprintf(want, sizeof(want), "ux0:data/JamaisVu/slot%d.jvi", slot + 1);
  CHECK(card_has(want), "slot info is on the Memory Card");
  CHECK(count_entries(d_ux0) == 0, "something was written to ux0 storage (SD card)");
  CHECK(count_entries(d_uma0) == 0, "something was written to uma0 storage (USB)");

  CHECK(slot_info_remove(slot) == 0, "info remove");
  CHECK(!slot_is_loadable(slot), "removed info -> not loadable");
}

/* --- param.sfo ----------------------------------------------------------- */

static void mkdirs_os(const char *path) {
  char p[256];
  for (size_t i = 0; path[i]; i++) {
    if (path[i] == '/') {
      memcpy(p, path, i);
      p[i] = 0;
      ksceIoMkdir(p, 0777);
    }
  }
}

/* writes a minimal param.sfo with TITLE and APP_VER through the "OS" */
static void write_sfo(const char *path, const char *title, const char *ver) {
  uint8_t buf[512];
  memset(buf, 0, sizeof(buf));
  const char *keys[2] = { "APP_VER", "TITLE" };
  const char *vals[2] = { ver, title };
  uint32_t key_table = 20 + 2 * 16, data_table, koff = 0, doff = 0;
  data_table = key_table + 16;
  memcpy(buf, "\0PSF", 4);
  buf[4] = 1; buf[5] = 1;
  memcpy(buf + 8, &key_table, 4);
  memcpy(buf + 12, &data_table, 4);
  uint32_t count = 2;
  memcpy(buf + 16, &count, 4);
  for (int i = 0; i < 2; i++) {
    uint8_t *e = buf + 20 + i * 16;
    uint16_t ko = koff, fmt = 0x0204;
    uint32_t len = strlen(vals[i]) + 1, max = 64;
    memcpy(e, &ko, 2);
    memcpy(e + 2, &fmt, 2);
    memcpy(e + 4, &len, 4);
    memcpy(e + 8, &max, 4);
    memcpy(e + 12, &doff, 4);
    strcpy((char *)buf + key_table + koff, keys[i]);
    koff += strlen(keys[i]) + 1;
    strcpy((char *)buf + data_table + doff, vals[i]);
    doff += max;
  }
  mkdirs_os(path);
  SceUID fd = ksceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
  CHECK(fd >= 0, "write sfo %s", path);
  ksceIoWrite(fd, buf, data_table + doff);
  ksceIoClose(fd);
}

/* Persistent trace: trace_prepare must find trace.bin's sector by walking
 * the card's exFAT structures through the raw device, and the payload's
 * record there must come back through trace_collect. */
static void test_trace(const char *mc_mount, int sd_ux0, int usb_uma0,
                       int filler) {
  static char path[128], out[1400];
  uint32_t rel = 0;
  uint8_t nonce[16];
  const char *why = "";

  printf("trace.bin with the card as %s, %d other files first\n", mc_mount,
         filler);
  scenario("", mc_mount, sd_ux0, usb_uma0);
  CHECK(detect_readonly() != NULL, "card");
  CHECK(slots_mkdirs() == 0, "mkdirs");
  /* push trace.bin's directory entries across many sectors and clusters */
  for (int i = 0; i < filler; i++) {
    snprintf(path, sizeof(path), "%sdata/JamaisVu/filler_with_a_long_name_%04d"
             ".txt", mc_mount, i);
    SceUID fd = ksceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT, 0666);
    CHECK(fd >= 0, "filler %d", i);
    ksceIoWrite(fd, "x", 1);
    ksceIoClose(fd);
  }

  CHECK(trace_prepare(&rel, nonce, &why) == 0, "trace_prepare: %s", why);
  CHECK(rel != 0 && rel < fk_mc_sectors, "rel %u", rel);
  if (rel == 0 || rel >= fk_mc_sectors)
    return;
  JvTraceSector *t = (JvTraceSector *)(fk_mc_image + (size_t)rel * 512);
  CHECK(!memcmp(t->magic, JV_TRACE_MAGIC, 8) && !memcmp(t->nonce, nonce, 16),
        "the located sector holds this run's magic + nonce");

  /* a second run keeps working (new nonce, same file) */
  uint32_t rel2 = 0;
  uint8_t nonce2[16];
  CHECK(trace_prepare(&rel2, nonce2, &why) == 0 && rel2 == rel,
        "second prepare: %s (%u vs %u)", why, rel2, rel);
  CHECK(!memcmp(t->nonce, nonce2, 16), "nonce updated");

  CHECK(trace_collect(out, sizeof(out)) == 0, "nothing to collect yet");

  /* what the payload writes behind the OS's back */
  t->written = 1;
  t->mode = JAMAISVU_MODE_SAVE;
  t->debug_mode = JV_TEST_DRYRUN;
  t->slot = 2;
  t->result = JV_ERR_CRASH;
  t->detail = 4;
  t->stage = JV_STAGE_MC_AUTH;
  t->trace_n = 3;
  t->trace_stage[0] = JV_STAGE_ENTRY;      t->trace_ms[0] = 0;
  t->trace_stage[1] = JV_STAGE_MC_MOUNT;   t->trace_ms[1] = 1700;
  t->trace_stage[2] = JV_STAGE_MC_AUTH;    t->trace_ms[2] = 1750;
  t->fault_type = 4;
  t->fault_addr = 0x1c0abcde;
  t->fault_lr = 0x1c012345;

  CHECK(trace_collect(out, sizeof(out)) == 1, "collect");
  printf("  collected: %s", out);
  CHECK(strstr(out, "save (dry run) slot=3") && strstr(out, "card-auth@1750ms")
        && strstr(out, "data abort") && strstr(out, "0x1C0ABCDE"),
        "formatted record");
  CHECK(trace_collect(out, sizeof(out)) == 0, "consumed after collecting");
  CHECK(count_entries(d_ux0) == 0 && count_entries(d_uma0) == 0,
        "trace.bin only on the Memory Card");
}

static void test_versions(void) {
  char ver[8], title[64];

  scenario("game version checks (param.sfo APP_VER)", "ux0:", 0, 0);
  CHECK(lookup_app_ver("PCSE00123", ver, sizeof(ver)) < 0,
        "no game installed -> no version");

  write_sfo("ux0:app/PCSE00123/sce_sys/param.sfo", "Test Game", "01.01");
  CHECK(lookup_app_ver("PCSE00123", ver, sizeof(ver)) == 0 &&
        !strcmp(ver, "01.01"), "base game version: %s", ver);
  lookup_title("PCSE00123", title, sizeof(title));
  CHECK(!strcmp(title, "Test Game"), "title %s", title);

  write_sfo("ux0:patch/PCSE00123/sce_sys/param.sfo", "Test Game", "01.02");
  CHECK(lookup_app_ver("PCSE00123", ver, sizeof(ver)) == 0 &&
        !strcmp(ver, "01.02"), "installed update wins: %s", ver);

  write_sfo("ux0:app/PCSE00999/sce_sys/param.sfo", "Odd", "1.0a");
  CHECK(lookup_app_ver("PCSE00999", ver, sizeof(ver)) < 0,
        "malformed version rejected");

  CHECK(app_versions_match("01.02", "01.02"), "same version loads");
  CHECK(!app_versions_match("01.01", "01.02"), "update blocks load");
  CHECK(!app_versions_match("01.02", "01.01"), "downgrade blocks load");
  CHECK(!app_versions_match("01.02", "01.020"), "prefix is not a match");
  CHECK(!app_versions_match("01.02", ""), "unknown installed version blocks load");
  CHECK(!app_versions_match("", ""), "unknown saved version blocks load");
  CHECK(!app_versions_match(NULL, "01.02"), "missing version blocks load");
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

  snprintf(d_ux0, sizeof(d_ux0), "%s/sd_ux0", argv[2]);
  snprintf(d_uma0, sizeof(d_uma0), "%s/usb_uma0", argv[2]);

  static char path[128];
  const char *root;

  storage_set_mount_lookup(fk_find_mount_point);

  scenario("Memory Card as ux0, no uma0", "ux0:", 0, 0);
  root = detect_readonly();
  CHECK(root && !strcmp(root, "ux0:"), "root %s", root ? root : "(null)");
  slot_payload_path(path, sizeof(path), 2);
  CHECK(!strcmp(path, "ux0:data/JamaisVu/slot3.bin"), "payload path %s", path);
  exercise_writes(2);

  scenario("Memory Card as ux0, USB stick/SD2Vita as uma0", "ux0:", 0, 1);
  root = detect_readonly();
  CHECK(root && !strcmp(root, "ux0:"), "root %s", root ? root : "(null)");
  exercise_writes(0);

  scenario("Memory Card as uma0, SD2Vita/PSVSD as ux0", "uma0:", 1, 0);
  root = detect_readonly();
  CHECK(root && !strcmp(root, "uma0:"), "root %s", root ? root : "(null)");
  slot_payload_path(path, sizeof(path), 0);
  CHECK(!strcmp(path, "ux0:data/JamaisVu/slot1.bin"), "payload path %s", path);
  exercise_writes(1);

  scenario("Memory Card as xmc0, SD2Vita as ux0, USB as uma0", "xmc0:", 1, 1);
  root = detect_readonly();
  CHECK(root && !strcmp(root, "xmc0:"), "root %s", root ? root : "(null)");
  exercise_writes(4);

  scenario("no Memory Card (SD2Vita as ux0, USB as uma0)", "", 1, 1);
  root = detect_readonly();
  printf("  -> \"%s\"\n", storage_reason());
  CHECK(root == NULL, "must not find a Memory Card");
  {
    static uint32_t thumb[JV_THUMB_W * JV_THUMB_H];
    SlotInfo info;
    uint8_t nonce[16];
    memset(&info, 0, sizeof(info));
    CHECK(slots_mkdirs() < 0, "mkdirs must refuse");
    CHECK(slot_prepare(0, NULL) < 0, "prepare must refuse");
    CHECK(slot_stamp(0, nonce) < 0, "stamp must refuse");
    CHECK(slot_info_write(0, &info, thumb) < 0, "info write must refuse");
    CHECK(!slot_is_loadable(0) && !slot_file_exists(0), "nothing to load");
    slots_sync();
    CHECK(count_entries(d_ux0) == 0 && count_entries(d_uma0) == 0,
          "nothing may be written without a Memory Card");
  }

  scenario("mount table unavailable", "ux0:", 0, 0);
  storage_set_mount_lookup(NULL);
  root = detect_readonly();
  CHECK(root == NULL, "no lookup -> no card");
  storage_set_mount_lookup(fk_find_mount_point);

  test_versions();

  test_trace("ux0:", 0, 0, 0);
  test_trace("ux0:", 0, 0, 1500); /* directory spans clusters */
  test_trace("uma0:", 1, 0, 40);
  test_trace("xmc0:", 1, 1, 0);

  CHECK(fk_blocks_in_use() == 0, "memory blocks leaked: %d", fk_blocks_in_use());
  CHECK(fk_fds_in_use() == 0, "file descriptors leaked: %d", fk_fds_in_use());

  printf("\n%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
