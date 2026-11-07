#!/bin/sh
# Host tests for the payload's save/load logic. Needs gcc and exfatprogs
# (mkfs.exfat, fsck.exfat). Usage: tests/host/run.sh
set -eu

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
if [ -z "${WORK:-}" ]; then
  # own temporary directory: removed on exit (card images are large)
  WORK=$(mktemp -d)
  trap 'rm -rf -- "$WORK"' EXIT
fi
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

# --- plugin storage (Memory Card detection, writes only to the card) ------
# The fake OS views the Memory Card through a read-write FatFs whose
# symbols are renamed to os_*; SD2Vita/USB mounts are host folders.
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
for src in "$ROOT/plugin/storage.c" "$ROOT/plugin/slots.c" \
           "$ROOT/plugin/slot_policy.c" "$ROOT/payload/crc32.c" \
           "$ROOT/plugin/trace.c" "$ROOT/plugin/md5.c" \
           "$ROOT/tests/host/fake_kernel.c" "$ROOT/tests/host/test_storage.c"; do
  gcc $CF $SZ -c -I"$ROOT/tests/host/fake_sdk" -I"$ROOT/tests/host" \
    -I"$ROOT/plugin" -I"$WORK/fatfs" -o "$SO/$(basename "$src" .c).o" "$src"
done
gcc $CF -o "$WORK/test_storage" "$SO"/*.o
echo "=== plugin storage (Memory Card only, read-only detection, versions) ==="
mkdir -p "$WORK/img/storage"
truncate -s 64M "$WORK/img/storage/card.img"
mkfs.exfat -q -c 32K "$WORK/img/storage/card.img" >/dev/null
JV_CFG_MD5=$(printf '*KERNEL\nux0:tai/JamaisVu.skprx\n' | md5sum | cut -d' ' -f1) \
  "$WORK/test_storage" "$WORK/img/storage/card.img" "$WORK/img/storage" || rc=1
echo

gcc -Wall -Wextra -I"$ROOT/plugin" -o "$WORK/test_slot_policy" \
  "$ROOT/tests/host/test_slot_policy.c" "$ROOT/plugin/slot_policy.c"
"$WORK/test_slot_policy" || rc=1

gcc -Wall -Wextra -I"$ROOT/plugin" -o "$WORK/test_fs_policy" \
  "$ROOT/tests/host/test_fs_policy.c" "$ROOT/plugin/fs_policy.c"
"$WORK/test_fs_policy" || rc=1

gcc -Wall -Wextra -I"$ROOT/plugin" -o "$WORK/test_md5" \
  "$ROOT/tests/host/test_md5.c" "$ROOT/plugin/md5.c"
"$WORK/test_md5" || rc=1

gcc -O1 -g -Wall -Wextra -fsanitize=address,undefined \
  -fno-sanitize-recover=undefined -I"$ROOT/plugin/gui" -o "$WORK/test_rle16" \
  "$ROOT/tests/host/test_rle16.c" "$ROOT/plugin/gui/rle16.c"
"$WORK/test_rle16" || rc=1
echo

for enums in "" "-fshort-enums"; do
  gcc -Wall -Wextra $enums -I"$ROOT/plugin" -o "$WORK/test_remap" \
    "$ROOT/tests/host/test_remap.c" "$ROOT/plugin/remap.c"
  "$WORK/test_remap" || rc=1
done
echo

gcc -O2 -Wall -Wextra -fsanitize=address,undefined -I"$ROOT/plugin/gui" \
  -o "$WORK/test_render" "$ROOT/tests/host/test_render.c" \
  "$ROOT/plugin/gui/render.c" "$ROOT/plugin/gui/menu_view.c" \
  "$ROOT/plugin/gui/rle16.c" \
  "$ROOT/plugin/gui/logo.c" \
  "$ROOT/plugin/gui/font_data.c" "$ROOT/plugin/gui/jpfont.c"
"$WORK/test_render" "$ROOT/plugin/gui/jpfont.bin" || rc=1
echo

mkdir -p "$WORK/appdb"
python3 "$ROOT/tests/host/make_appdb.py" "$WORK/appdb"
gcc -O1 -g -Wall -Wextra -fsanitize=address,undefined \
  -fno-sanitize-recover=undefined -I"$ROOT/plugin" -o "$WORK/test_appdb" \
  "$ROOT/tests/host/test_appdb.c" "$ROOT/plugin/appdb.c"
"$WORK/test_appdb" "$WORK/appdb" ${JV_REAL_APPDB:-} || rc=1
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
