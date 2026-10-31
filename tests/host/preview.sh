#!/bin/sh
# Renders the in-game menu to PNGs using the plugin's drawing code.
# usage: tests/host/preview.sh <background image> <outdir>
# Needs gcc and python3 with Pillow.
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BG=$1
OUT=$2
mkdir -p "$OUT"
WORK=$(mktemp -d)

python3 - "$BG" "$WORK" <<'PY'
import sys
from PIL import Image, ImageOps
bg = ImageOps.fit(Image.open(sys.argv[1]).convert("RGB"), (960, 544))
bg.convert("RGBA").save(sys.argv[2] + "/bg.png")
open(sys.argv[2] + "/bg.rgba", "wb").write(bg.convert("RGBA").tobytes())
# "snapshots": the whole screen scaled down, like the plugin captures them,
# with a few colour variations so the slots look different
from PIL import ImageEnhance
full = bg.resize((160, 90), Image.LANCZOS)
variants = [full, ImageOps.mirror(full),
            ImageEnhance.Color(full).enhance(0.2),
            Image.merge("RGB", full.split()[::-1]),
            ImageEnhance.Brightness(full).enhance(0.6)]
data = b""
for v in variants:
    data += v.convert("RGBA").tobytes()
open(sys.argv[2] + "/thumbs.rgba", "wb").write(data)
PY

gcc -O2 -Wall -I"$ROOT/plugin/gui" -o "$WORK/preview" \
  "$ROOT/tests/host/preview.c" "$ROOT/plugin/gui/render.c" \
  "$ROOT/plugin/gui/menu_view.c" "$ROOT/plugin/gui/font_data.c"
"$WORK/preview" "$WORK/bg.rgba" "$WORK/thumbs.rgba" "$WORK"

python3 - "$WORK" "$OUT" <<'PY'
import sys, glob, os
from PIL import Image
for f in sorted(glob.glob(sys.argv[1] + "/[0-9]_*.rgba")):
    img = Image.frombytes("RGBA", (960, 544), open(f, "rb").read()).convert("RGB")
    out = os.path.join(sys.argv[2], os.path.basename(f)[:-5] + ".png")
    img.save(out)
    print(out)
PY
