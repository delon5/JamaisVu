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
#include "../payload/crc32.h"

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

static int g_nmount, g_numount, g_mount_id;
static int fake_mount(int id, const char *path, int perm, int a4, int a5,
                      int a6) {
  (void)path; (void)perm; (void)a4; (void)a5; (void)a6;
  g_nmount++;
  g_mount_id = id;
  if (id == 0xF00 && !strcmp(fk_mc_table, "uma0:"))
    strcpy(fk_mc_mount, "uma0:");
  return 0;
}
static int fake_umount(int id, int a2, int a3, int a4) {
  (void)id; (void)a2; (void)a3; (void)a4;
  g_numount++;
  return 0;
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
  static uint16_t thumb[JV_THUMB_W * JV_THUMB_H];
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
/* the plugin's steps after the wake-up, as they appear in log.txt */
static void test_wake_format(void) {
  static JvTraceSector t;
  static char out[1200];
  printf("trace: steps after the wake-up\n");
  memset(&t, 0, sizeof(t));
  memcpy(t.magic, JV_TRACE_MAGIC, 8);
  t.written = 1;
  t.mode = JAMAISVU_MODE_LOAD;
  t.stage = JV_STAGE_DONE;
  t.wake_n = 4;
  t.wake_step[0] = JV_WAKE_RESUME;   t.wake_ms[0] = 0;
  t.wake_step[1] = JV_WAKE_RESUMED;  t.wake_ms[1] = 840;
  t.wake_step[2] = JV_WAKE_FS_PATCH; t.wake_ms[2] = 851;
  t.wake_step[3] = JV_WAKE_FS_MOUNT + 1; t.wake_ms[3] = 852;
  trace_format(out, sizeof(out), &t);
  printf("%s", out);
  CHECK(strstr(out, "after the wake-up (ms since the first resume event): "
                    "resume@0 resumed@840 fs-patch@851 remount-1@852\r\n")
        != NULL, "wake steps not formatted");
  t.wake_n = 0;
  trace_format(out, sizeof(out), &t);
  CHECK(strstr(out, "wake-up") == NULL, "no wake line without steps");
}

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

static void put_file(const char *path, const char *text) {
  SceUID fd = ksceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
  CHECK(fd >= 0, "write %s", path);
  if (fd >= 0) {
    ksceIoWrite(fd, text, strlen(text));
    ksceIoClose(fd);
  }
}

/* tai/config.txt fingerprint: stored with a snapshot, a change blocks the
 * load. The md5 is checked against md5sum (JV_CFG_MD5, set by run.sh). */
static void test_config(void) {
  static SlotInfo info;
  int which = -1;

  printf("plugin configuration fingerprint (tai/config.txt)\n");
  scenario("", "ux0:", 0, 0);
  CHECK(detect_readonly() != NULL, "card");
  memset(&info, 0, sizeof(info));

  config_fingerprint(info.cfg_state, info.cfg_md5);
  CHECK(info.cfg_state[0] == JV_CFG_ABSENT && info.cfg_state[1] == JV_CFG_ABSENT,
        "no config files -> both absent");
  CHECK(config_matches(&info, NULL), "unchanged (both absent)");

  ksceIoMkdir("ux0:tai", 0777);
  put_file("ux0:tai/config.txt", "*KERNEL\nux0:tai/JamaisVu.skprx\n");
  CHECK(!config_matches(&info, &which) && which == 0,
        "config.txt appeared -> mismatch on ux0 (%d)", which);

  config_fingerprint(info.cfg_state, info.cfg_md5);
  CHECK(info.cfg_state[0] == JV_CFG_PRESENT, "present");
  char hex[33];
  for (int i = 0; i < 16; i++)
    sprintf(hex + i * 2, "%02x", info.cfg_md5[0][i]);
  printf("  md5(ux0:tai/config.txt) = %s\n", hex);
  CHECK(!strcmp(hex, getenv("JV_CFG_MD5") ? getenv("JV_CFG_MD5") : hex),
        "md5 %s vs %s", hex, getenv("JV_CFG_MD5"));
  CHECK(config_matches(&info, NULL), "same file -> match");

  put_file("ux0:tai/config.txt", "*KERNEL\nux0:tai/JamaisVu.skprx\n"
           "ux0:tai/other.skprx\n");
  which = -1;
  CHECK(!config_matches(&info, &which) && which == 0,
        "plugin added -> mismatch (%d)", which);

  put_file("ux0:tai/config.txt", "*KERNEL\nux0:tai/JamaisVu.skprx\n");
  CHECK(config_matches(&info, NULL), "restored -> match again");

  ksceIoRemove("ux0:tai/config.txt");
  CHECK(!config_matches(&info, NULL), "removed -> mismatch");
  CHECK(count_entries(d_ux0) == 0 && count_entries(d_uma0) == 0,
        "fingerprint only reads");
}

/* RGB565 of an RGBA8888 pixel (R in the low byte), computed here on its
 * own: thumbnails are kept as RGB565 in memory and in v4 files */
static uint16_t to565(uint32_t c) {
  uint32_t r = c & 0xff, g = (c >> 8) & 0xff, b = (c >> 16) & 0xff;
  return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static int same565(const uint32_t *rgba, const uint16_t *px) {
  for (int i = 0; i < JV_THUMB_W * JV_THUMB_H; i++)
    if (px[i] != to565(rgba[i]))
      return 0;
  return 1;
}

/* A slot written by the previous version (slot info v2, no config
 * fingerprint) must still read as a normal, loadable snapshot. */
static void test_slot_info_v2(void) {
  static uint32_t thumb[JV_THUMB_W * JV_THUMB_H];
  static uint16_t back_thumb[JV_THUMB_W * JV_THUMB_H];
  static SlotInfoV2 v2;
  static SlotInfo info;
  static char path[128];

  printf("slot info written by the previous version (v2)\n");
  scenario("", "ux0:", 0, 0);
  CHECK(detect_readonly() != NULL, "card");
  CHECK(slots_mkdirs() == 0 && slot_prepare(1, NULL) == 0, "prepare slot 2");

  memset(&v2, 0, sizeof(v2));
  v2.magic = SLOT_INFO_MAGIC;
  v2.version = 2;
  strcpy(v2.titleid, "PCSB00001");
  strcpy(v2.title, "Old Save");
  v2.year = 2026; v2.month = 11; v2.day = 1;
  v2.thumb_w = JV_THUMB_W;
  v2.thumb_h = JV_THUMB_H;
  v2.has_thumb = 1;
  strcpy(v2.app_ver, "01.00");
  for (int i = 0; i < JV_THUMB_W * JV_THUMB_H; i++)
    thumb[i] = 0xff000000u | (uint32_t)i;
  v2.crc = crc32_update(crc32_update(0, &v2, offsetof(SlotInfoV2, crc)),
                        thumb, sizeof(thumb));
  slot_path(path, sizeof(path), 1, "jvi");
  SceUID fd = ksceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
  CHECK(fd >= 0, "write v2 info");
  ksceIoWrite(fd, &v2, sizeof(v2));
  ksceIoWrite(fd, thumb, sizeof(thumb));
  ksceIoClose(fd);

  CHECK(slot_info_read(1, &info, back_thumb) == 0, "v2 info reads");
  CHECK(!strcmp(info.titleid, "PCSB00001") && !strcmp(info.title, "Old Save")
        && !strcmp(info.app_ver, "01.00") && info.has_thumb &&
        same565(thumb, back_thumb), "v2 fields and thumbnail (as RGB565)");
  CHECK(slot_is_loadable(1), "v2 slot is loadable");
  CHECK(config_matches(&info, NULL), "no fingerprint -> config check skipped");

  /* a corrupted v2 file is still rejected */
  thumb[5] ^= 1;
  fd = ksceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
  ksceIoWrite(fd, &v2, sizeof(v2));
  ksceIoWrite(fd, thumb, sizeof(thumb));
  ksceIoClose(fd);
  CHECK(slot_info_read(1, &info, back_thumb) < 0, "damaged v2 info rejected");
  CHECK(!slot_is_loadable(1), "damaged thumbnail caught by the chunked CRC");
}


/* v4 stores the thumbnail as RGB565 (half the size); v3 files with an
 * RGBA8888 thumbnail must still read. */
static void test_thumb_formats(void) {
  static uint32_t thumb[JV_THUMB_W * JV_THUMB_H];
  static uint16_t thumb16[JV_THUMB_W * JV_THUMB_H], back[JV_THUMB_W * JV_THUMB_H];
  static SlotInfo info, got;
  static char path[128];
  SceIoStat st;

  printf("thumbnail storage (v4 RGB565, v3 RGBA8888)\n");
  scenario("", "ux0:", 0, 0);
  CHECK(detect_readonly() != NULL, "card");
  CHECK(slots_mkdirs() == 0 && slot_prepare(3, NULL) == 0, "prepare slot 4");
  for (int i = 0; i < JV_THUMB_W * JV_THUMB_H; i++)
    thumb[i] = 0xff000000u | ((uint32_t)(i * 7) & 0xff) |
               (((uint32_t)(i * 13) & 0xff) << 8) |
               (((uint32_t)(i * 29) & 0xff) << 16);

  memset(&info, 0, sizeof(info));
  strcpy(info.titleid, "PCSE00999");
  strcpy(info.title, "Thumb Test");
  strcpy(info.app_ver, "01.00");
  info.has_thumb = 1;
  for (int i = 0; i < JV_THUMB_W * JV_THUMB_H; i++)
    thumb16[i] = to565(thumb[i]);
  CHECK(slot_info_write(3, &info, thumb16) == 0, "v4 write");
  slot_path(path, sizeof(path), 3, "jvi");
  CHECK(ksceIoGetstat(path, &st) >= 0 &&
        st.st_size == (SceOff)(sizeof(SlotInfo) + JV_THUMB_W * JV_THUMB_H * 2),
        "v4 file is %u + 28800 bytes (got %lld)", (unsigned)sizeof(SlotInfo),
        (long long)st.st_size);
  CHECK(slot_info_read(3, &got, back) == 0 && got.version == 4, "v4 read");
  CHECK(!memcmp(back, thumb16, sizeof(back)), "v4 thumbnail read back exactly");
  /* the v4 file layout is unchanged: the thumbnail bytes as written */
  {
    static uint16_t raw[JV_THUMB_W * JV_THUMB_H];
    SceUID rfd = ksceIoOpen(path, SCE_O_RDONLY, 0);
    ksceIoLseek(rfd, sizeof(SlotInfo), SCE_SEEK_SET);
    CHECK(ksceIoRead(rfd, raw, sizeof(raw)) == (int)sizeof(raw) &&
          !memcmp(raw, thumb16, sizeof(raw)), "v4 file holds the RGB565 pixels");
    ksceIoClose(rfd);
  }
  CHECK(slot_is_loadable(3), "v4 slot loadable (chunked CRC)");

  /* a v3 file: same header, RGBA8888 thumbnail */
  memset(&info, 0, sizeof(info));
  info.magic = SLOT_INFO_MAGIC;
  info.version = 3;
  strcpy(info.titleid, "PCSE00998");
  strcpy(info.title, "Old Thumb");
  strcpy(info.app_ver, "01.00");
  info.thumb_w = JV_THUMB_W;
  info.thumb_h = JV_THUMB_H;
  info.has_thumb = 1;
  info.crc = crc32_update(crc32_update(0, &info, offsetof(SlotInfo, crc)),
                          thumb, sizeof(thumb));
  SceUID fd = ksceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
  ksceIoWrite(fd, &info, sizeof(info));
  ksceIoWrite(fd, thumb, sizeof(thumb));
  ksceIoClose(fd);
  CHECK(slot_info_read(3, &got, back) == 0 && got.version == 3 &&
        same565(thumb, back), "v3 (RGBA8888) still reads (as RGB565)");
  CHECK(slot_is_loadable(3), "v3 slot loadable");
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

  /* Japanese names: kept as UTF-8 for the menu (ASCII copy: title ID) */
  write_sfo("ux0:app/PCSG01179/sce_sys/param.sfo",
            "\xe3\x82\xad\xe3\x83\xa3\xe3\x82\xb5\xe3\x83\xaa\xe3\x83\xb3"
            "\xe3\x83\xbb\xef\xbc\xa6\xef\xbc\xa2", "01.03");
  lookup_title("PCSG01179", title, sizeof(title));
  CHECK(!strcmp(title, "PCSG01179"), "Latin-only name falls back to ID: %s",
        title);
  CHECK(lookup_title_utf8("PCSG01179", title, sizeof(title)) == 0 &&
        !strcmp(title, "\xe3\x82\xad\xe3\x83\xa3\xe3\x82\xb5\xe3\x83\xaa"
                "\xe3\x83\xb3\xe3\x83\xbb\xef\xbc\xa6\xef\xbc\xa2"),
        "UTF-8 name kept");
  {
    char small[8]; /* 7 bytes: 2 whole characters fit, the 3rd is cut */
    CHECK(lookup_title_utf8("PCSG01179", small, sizeof(small)) == 0 &&
          strlen(small) == 6, "UTF-8 name cut at a character (%d bytes)",
          (int)strlen(small));
  }
  write_sfo("ux0:app/PCSG00490/sce_sys/param.sfo",
            "\xe8\x8b\xb1\xe9\x9b\x84\nthe 3rd  Evolution ", "01.01");
  CHECK(lookup_title_utf8("PCSG00490", title, sizeof(title)) == 0 &&
        !strcmp(title, "\xe8\x8b\xb1\xe9\x9b\x84 the 3rd Evolution"),
        "line break -> space, spaces collapsed, trimmed: %s", title);
  CHECK(lookup_title_utf8("PCSE00000", title, sizeof(title)) < 0,
        "not installed -> no name");

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

/* usbmc puts the card on uma0 once at boot; after a sleep the system can
 * leave uma0 unmounted (PS TV, PCH-1000). Mounting it again must happen
 * only then: never while the card is mounted somewhere (1b41e43 tried
 * xmc0 every 4 s, which the system table always lists as the card), and
 * never for a card that belongs on ux0 (the system's job). */
static void test_remount(void) {
  const char *root = NULL;
  storage_set_mount_ops(fake_mount, fake_umount);

  scenario("remount: card on uma0 and mounted (xmc0 down)", "uma0:", 1, 0);
  strcpy(fk_mc_table, "uma0:");
  g_nmount = g_numount = 0;
  CHECK(!storage_card_unmounted(&root), "nothing is missing");
  CHECK(storage_remount_card(&root) == 1 && g_nmount == 0 && g_numount == 0,
        "must not mount anything (mounts %d)", g_nmount);

  scenario("remount: uma0 left unmounted after a sleep", "", 1, 0);
  strcpy(fk_mc_table, "uma0:");
  g_nmount = g_numount = 0;
  CHECK(storage_card_unmounted(&root) && root && !strcmp(root, "uma0:"),
        "uma0 must be reported missing (%s)", root ? root : "(null)");
  CHECK(storage_remount_card(&root) == 0 && g_nmount == 1 &&
        g_mount_id == 0xF00 && g_numount == 2,
        "must mount uma0 once (mounts %d id 0x%X umounts %d)", g_nmount,
        g_mount_id, g_numount);
  root = detect_readonly();
  CHECK(root && !strcmp(root, "uma0:"), "card back on %s",
        root ? root : "(null)");

  scenario("remount: card belongs on ux0, unmounted", "", 0, 0);
  fk_mc_table[0] = '\0';
  g_nmount = 0;
  CHECK(!storage_card_unmounted(&root), "ux0 is the system's job");
  CHECK(storage_remount_card(&root) == 1 && g_nmount == 0,
        "must not mount anything (mounts %d)", g_nmount);

  scenario("remount: no mount functions", "", 1, 0);
  strcpy(fk_mc_table, "uma0:");
  storage_set_mount_ops(NULL, NULL);
  CHECK(storage_remount_card(&root) < 0, "must refuse without functions");

  fk_mc_table[0] = '\0';
  storage_set_mount_ops(NULL, NULL);
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
    static uint16_t thumb[JV_THUMB_W * JV_THUMB_H];
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

  test_remount();
  test_wake_format();

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
  test_config();
  test_slot_info_v2();
  test_thumb_formats();

  CHECK(fk_blocks_in_use() == 0, "memory blocks leaked: %d", fk_blocks_in_use());
  CHECK(fk_fds_in_use() == 0, "file descriptors leaked: %d", fk_fds_in_use());

  printf("\n%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
