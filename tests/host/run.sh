#!/bin/sh
# Host tests for the payload's save/load logic. Needs gcc and exfatprogs
# (mkfs.exfat, fsck.exfat). Usage: tests/host/run.sh
set -eu

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
WORK=${WORK:-$(mktemp -d)}
mkdir -p "$WORK/fatfs" "$WORK/img"

# FatFs with the payload's configuration (copied so the test build can never
# pick up a different ffconf.h).
cp "$ROOT"/payload/FatFs/*.c "$ROOT"/payload/FatFs/*.h "$WORK/fatfs/"

gcc -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare \
  -Wno-implicit-fallthrough -Wno-unused-result \
  -fsanitize=address,undefined -fno-sanitize-recover=undefined \
  -DJV_HOST_TEST \
  -I"$ROOT/payload" -I"$ROOT/payload/msif" -I"$WORK/fatfs" \
  -o "$WORK/test_state" \
  "$ROOT/tests/host/test_state.c" \
  "$ROOT/payload/state.c" "$ROOT/payload/guard.c" "$ROOT/payload/crc32.c" \
  "$WORK/fatfs/diskio.c" "$WORK/fatfs/ff.c" "$WORK/fatfs/ffunicode.c" \
  "$WORK/fatfs/ffsystem.c"

rc=0
# Small clusters fragment easily; 32K/128K match real Memory Cards.
for cluster in 4K 32K 128K; do
  echo "=== exFAT, $cluster clusters ==="
  dir="$WORK/img/$cluster"
  mkdir -p "$dir"
  rm -f "$dir"/*.img "$dir"/*.fsck
  truncate -s 64M "$dir/part.img"
  mkfs.exfat -q -c "$cluster" "$dir/part.img" >/dev/null

  if ! "$WORK/test_state" "$dir/part.img" "$dir"; then
    rc=1
  fi

  echo "fsck.exfat on every checkpoint image:"
  for img in "$dir"/0*.img; do
    if fsck.exfat -n "$img" >"$img.fsck" 2>&1; then
      echo "  $(basename "$img"): clean"
    else
      echo "  $(basename "$img"): ERRORS"; cat "$img.fsck"; rc=1
    fi
  done
  echo
done

if [ $rc -eq 0 ]; then echo "ALL HOST TESTS PASSED"; else echo "HOST TESTS FAILED"; fi
exit $rc
