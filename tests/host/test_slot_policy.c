/* Unit test for plugin/slot_policy.c (free space -> usable slots) */
#include <stdio.h>
#include "slot_policy.h"

#define MB (1024ll * 1024)
#define GB (1024ll * MB)

static int failures = 0;

static void expect(const char *name, long long free_bytes, const int *exists,
                   int want_usable, const char *want_locked) {
  int locked[5], ok;
  int usable = slot_policy(free_bytes, exists, locked, 5);
  char got[6];
  for (int i = 0; i < 5; i++)
    got[i] = locked[i] ? 'L' : '.';
  got[5] = 0;
  ok = usable == want_usable;
  for (int i = 0; i < 5; i++)
    ok &= got[i] == want_locked[i];
  printf("  %-44s usable=%d slots=%s %s\n", name, usable, got,
         ok ? "ok" : "FAIL");
  failures += !ok;
}

int main(void) {
  const int none[5] = { 0, 0, 0, 0, 0 };
  const int s2[5] = { 0, 1, 0, 0, 0 };
  const int s14[5] = { 1, 0, 0, 1, 0 };
  const int all[5] = { 1, 1, 1, 1, 1 };

  printf("slot policy (L = locked)\n");
  expect("500 MB free, no slot files", 500 * MB, none, 0, "LLLLL");
  expect("767 MB free (704 + 64 - 1)", 767 * MB, none, 0, "LLLLL");
  expect("768 MB free (704 + 64)", 768 * MB, none, 1, ".LLLL");
  expect("1 GB free", 1 * GB, none, 1, ".LLLL");
  expect("1.5 GB free", 1536 * MB, none, 2, "..LLL");
  expect("2.2 GB free", 2253 * MB, none, 3, "...LL");
  expect("3 GB free", 3 * GB, none, 4, "....L");
  expect("4 GB free", 4 * GB, none, 5, ".....");
  expect("100 GB free", 100 * GB, none, 5, ".....");
  expect("0 free, slot 2 exists", 0, s2, 1, "L.LLL");
  expect("1 GB free, slot 2 exists", 1 * GB, s2, 2, "..LLL");
  expect("1 GB free, slots 1+4 exist", 1 * GB, s14, 3, "..L.L");
  expect("free space unknown, slots 1+4 exist", -1, s14, 2, ".LL.L");
  expect("0 free, all exist", 0, all, 5, ".....");

  printf("%s\n", failures ? "SLOT POLICY TESTS FAILED" : "slot policy tests passed");
  return failures != 0;
}
