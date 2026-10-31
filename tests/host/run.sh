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

# --- plugin storage (Memory Card detection, uma0 archive) -----------------
# Two FatFs copies: the plugin's read-only one (raw Memory Card) and a
# read-write one playing the OS (symbols renamed to os_*).
SO="$WORK/storage_obj"
mkdir -p "$SO"
CF="-std=gnu11 -O1 -g -Wall -Wno-unused-parameter -fsanitize=address,undefined"
SZ="-DJV_SLOT_SIZE=0x800000u -DJV_FREE_SPACE_MARGIN=0x100000ull"
gcc $CF -c -I"$WORK/fatfs" -o "$SO/ff_os.o" "$WORK/fatfs/ff.c"
gcc $CF -c -I"$WORK/fatfs" -o "$SO/ffuni_os.o" "$WORK/fatfs/ffunicode.c"
{
  nm -g --defined-only "$SO/ff_os.o" "$SO/ffuni_os.o" | awk 'NF==3{print $3}'
  echo disk_read; echo disk_write; echo disk_status; echo disk_initialize
  echo disk_ioctl; echo get_fattime
} | sort -u | awk '{print $1" os_"$1}' > "$SO/rename.txt"
objcopy --redefine-syms="$SO/rename.txt" "$SO/ff_os.o"
objcopy --redefine-syms="$SO/rename.txt" "$SO/ffuni_os.o"
for src in "$WORK/fatfs/ff.c" "$WORK/fatfs/ffunicode.c"; do
  gcc $CF -DFF_FS_READONLY=1 -c -I"$WORK/fatfs" \
    -o "$SO/ro_$(basename "$src" .c).o" "$src"
done
for src in "$ROOT/plugin/storage.c" "$ROOT/plugin/slots.c" \
           "$ROOT/plugin/mc_raw.c" "$ROOT/payload/crc32.c"; do
  gcc $CF $SZ -DFF_FS_READONLY=1 -c -I"$ROOT/tests/host/fake_sdk" \
    -I"$ROOT/plugin" -I"$WORK/fatfs" -o "$SO/$(basename "$src" .c).o" "$src"
done
for src in "$ROOT/tests/host/fake_kernel.c" "$ROOT/tests/host/test_storage.c"; do
  gcc $CF $SZ -c -I"$ROOT/tests/host/fake_sdk" -I"$ROOT/tests/host" \
    -I"$ROOT/plugin" -I"$WORK/fatfs" -o "$SO/$(basename "$src" .c).o" "$src"
done
gcc $CF -o "$WORK/test_storage" "$SO"/*.o
echo "=== plugin storage (detection, uma0 first, archive copies) ==="
mkdir -p "$WORK/img/storage"
truncate -s 64M "$WORK/img/storage/card.img"
mkfs.exfat -q -c 32K "$WORK/img/storage/card.img" >/dev/null
"$WORK/test_storage" "$WORK/img/storage/card.img" "$WORK/img/storage" || rc=1
if fsck.exfat -n "$WORK/img/storage/card.img" >/dev/null 2>&1; then
  echo "fsck.exfat: card image clean"
else
  echo "fsck.exfat: card image ERRORS"; rc=1
fi
echo

gcc -Wall -Wextra -I"$ROOT/plugin" -o "$WORK/test_slot_policy" \
  "$ROOT/tests/host/test_slot_policy.c" "$ROOT/plugin/slot_policy.c"
"$WORK/test_slot_policy" || rc=1
echo

for flavour in release testing; do
  def=""
  [ "$flavour" = testing ] && def="-DJAMAISVU_TESTING"
  gcc -Wall -Wextra $def -I"$ROOT/plugin" -o "$WORK/test_offsets_$flavour" \
    "$ROOT/tests/host/test_offsets.c" "$ROOT/plugin/offsets.c"
  "$WORK/test_offsets_$flavour" || rc=1
done
echo

if [ $rc -eq 0 ]; then echo "ALL HOST TESTS PASSED"; else echo "HOST TESTS FAILED"; fi
exit $rc
