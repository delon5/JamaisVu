/* MD5 test vectors from RFC 1321, plus chunked updates. */
#include <stdio.h>
#include <string.h>
#include "md5.h"

static int fails;

static void hex(const uint8_t d[16], char out[33]) {
  for (int i = 0; i < 16; i++)
    sprintf(out + i * 2, "%02x", d[i]);
}

static void check(const char *msg, const char *want, int chunk) {
  Md5 m;
  uint8_t d[16];
  char h[33];
  size_t len = strlen(msg);
  md5_init(&m);
  for (size_t i = 0; i < len; i += chunk)
    md5_update(&m, msg + i, (uint32_t)(len - i < (size_t)chunk ? len - i : (size_t)chunk));
  md5_final(&m, d);
  hex(d, h);
  if (strcmp(h, want)) {
    printf("  FAIL md5(\"%.20s...\") chunk %d = %s, want %s\n", msg, chunk, h, want);
    fails++;
  }
}

int main(void) {
  static const struct { const char *m, *h; } v[] = {
    {"", "d41d8cd98f00b204e9800998ecf8427e"},
    {"a", "0cc175b9c0f1b6a831c399e269772661"},
    {"abc", "900150983cd24fb0d6963f7d28e17f72"},
    {"message digest", "f96b697d7cb7938d525a2f31aaf161d0"},
    {"abcdefghijklmnopqrstuvwxyz", "c3fcd3d76192e4007dfb496cca67e13b"},
    {"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789",
     "d174ab98d277d9f5a5611c2c9f419d9f"},
    {"12345678901234567890123456789012345678901234567890123456789012345678901234567890",
     "57edf4a22be3c955ac49da2e2107b67a"},
  };
  for (size_t i = 0; i < sizeof(v) / sizeof(v[0]); i++)
    for (int c = 1; c <= 80; c += 13)
      check(v[i].m, v[i].h, c);
  printf("md5 tests %s\n", fails ? "FAILED" : "passed");
  return fails != 0;
}
