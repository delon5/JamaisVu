/* Which firmware the plugin arms on (plugin/offsets.c), for the release
 * and the testing build. Built twice by tests/host/run.sh. */
#include <stdio.h>
#include <string.h>
#include "offsets.h"

static int failures = 0;

static void expect(const char *name, uint32_t io_nid, uint32_t exfat_nid,
                   int want) {
  const char *why = "";
  int got = offsets_usable(offsets_find(io_nid), exfat_nid, &why);
  printf("  %-48s %s (%s)\n", name, got ? "arms" : "refuses", why);
  if (got != want) {
    printf("  FAIL: expected %s\n", want ? "arms" : "refuses");
    failures++;
  }
}

int main(void) {
#ifdef JAMAISVU_TESTING
  const int testing = 1;
  printf("offset table, testing build\n");
#else
  const int testing = 0;
  printf("offset table, release build\n");
#endif
  expect("3.60 with the dumped SceExfatfs", 0x9642948C, 0x23AA294A, testing);
  expect("3.60 with a different SceExfatfs", 0x9642948C, 0x12345678, 0);
  expect("3.65", 0xA96ACE9D, 0, 1);
  expect("3.68 (never checked)", 0x90DA33DE, 0, 0);
  expect("unknown firmware", 0xDEADBEEF, 0, 0);
  printf("%s\n", failures ? "OFFSET TABLE TESTS FAILED" : "offset table tests passed");
  return failures != 0;
}
