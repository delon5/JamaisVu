/* JamaisVu host test: LiveArea names from app.db (plugin/appdb.c), on
 * databases made by real SQLite (make_appdb.py), plus damaged copies.
 *
 * usage: test_appdb <dir with make_appdb.py output> [a real app.db]
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "appdb.h"

static int rc;
#define CHECK(cond, ...)                         \
  do {                                           \
    printf("  %s ", (cond) ? "ok    " : "FAILED"); \
    printf(__VA_ARGS__);                         \
    printf("\n");                                \
    if (!(cond))                                 \
      rc = 1;                                    \
  } while (0)

typedef struct {
  uint8_t *data;
  long size;
} Mem;

static int mem_read(void *ctx, uint32_t off, void *buf, uint32_t len) {
  Mem *m = ctx;
  if (off >= (uint32_t)m->size)
    return 0;
  if (len > m->size - off)
    len = m->size - off;
  memcpy(buf, m->data + off, len);
  return (int)len;
}

static Mem load(const char *path) {
  Mem m = { NULL, 0 };
  FILE *f = fopen(path, "rb");
  if (!f)
    return m;
  fseek(f, 0, SEEK_END);
  m.size = ftell(f);
  fseek(f, 0, SEEK_SET);
  m.data = malloc(m.size);
  if (fread(m.data, 1, m.size, f) != (size_t)m.size)
    m.size = 0;
  fclose(f);
  return m;
}

static const char *ids[] = { "PCSG01179", "PCSE00349", "PCSG00490",
                             "PCSE99999", "PCSB00001", "PCSB00002",
                             "PCSB00003" };
#define NIDS 7
static char names[NIDS][APPDB_NAME_LEN];
static const char catherine[] =
  "\xe3\x82\xad\xe3\x83\xa3\xe3\x82\xb5\xe3\x83\xaa\xe3\x83\xb3"
  "\xe3\x83\xbb\xef\xbc\xa6\xef\xbc\xa2";

static int lookup(const char *dir, const char *file) {
  char path[512];
  snprintf(path, sizeof(path), "%s/%s", dir, file);
  Mem m = load(path);
  int n = m.size ? appdb_names(mem_read, &m, ids, NIDS, names) : -99;
  free(m.data);
  return n;
}

static void common(const char *dir, const char *file) {
  int n = lookup(dir, file);
  CHECK(n >= 3, "%s: names found (%d)", file, n);
  CHECK(!strcmp(names[0], catherine), "%s: Japanese name", file);
  CHECK(!strcmp(names[1], "1001 Spikes"), "%s: English name", file);
  CHECK(!strcmp(names[2], "Trails in the Sky the 3rd"),
        "%s: renamed game", file);
  CHECK(names[3][0] == '\0', "%s: unknown ID -> no name", file);
}

int main(int argc, char **argv) {
  const char *dir = argv[1];
  printf("=== LiveArea names from app.db ===\n");
  common(dir, "small.db");
  common(dir, "big.db");
  common(dir, "p1024.db");
  CHECK(lookup(dir, "p65536.db") < 0, "64 KB pages: refused (Vita uses 4 KB)");
  common(dir, "long.db");
  CHECK(strlen(names[4]) == 126 && strlen(names[4]) % 3 == 0,
        "long name cut at a character (%d bytes)", (int)strlen(names[4]));
  CHECK(names[5][0] == '\0', "record on overflow pages: skipped");
  CHECK(!strcmp(names[6], "Real name"), "folder entry is not the game");
  CHECK(lookup(dir, "schema.db") < 0 && names[0][0] == '\0',
        "other column layout: refused");

  /* damaged copies: any byte may be wrong - never a crash or bad read
   * (run under ASan), and when it does find a name it is a real one */
  char path[512];
  snprintf(path, sizeof(path), "%s/big.db", dir);
  Mem good = load(path);
  Mem bad = { malloc(good.size), good.size };
  unsigned seed = 12345, bogus = 0, runs = 3000;
  for (unsigned r = 0; r < runs; r++) {
    memcpy(bad.data, good.data, good.size);
    int flips = 1 + r % 64;
    for (int k = 0; k < flips; k++) {
      seed = seed * 1103515245u + 12345u;
      uint32_t at = (seed >> 3) % (uint32_t)good.size;
      if (r & 1)
        at = 100 + at % 4096; /* page 1: the schema and the root */
      bad.data[at] = (uint8_t)(seed >> 20);
    }
    if (r % 7 == 0)
      bad.size = (seed >> 5) % good.size; /* cut short */
    else
      bad.size = good.size;
    appdb_names(mem_read, &bad, ids, NIDS, names);
    for (int i = 0; i < NIDS; i++)
      if (memchr(names[i], 0, APPDB_NAME_LEN) == NULL)
        bogus++;
  }
  CHECK(bogus == 0, "%u damaged databases: always a terminated result", runs);
  free(good.data);
  free(bad.data);

  if (argc > 2) { /* the user's own app.db, if given */
    Mem m = load(argv[2]);
    int n = appdb_names(mem_read, &m, ids, NIDS, names);
    CHECK(n >= 1 && !strcmp(names[0], catherine),
          "real app.db: Catherine is %s", names[0]);
    free(m.data);
  }
  return rc;
}
