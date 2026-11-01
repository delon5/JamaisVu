#!/usr/bin/env python3
"""Builds jpfont.bin: Japanese (kana, kanji of JIS X 0208 levels 1 and 2,
symbols, full-width forms) and Latin-1 glyphs for game names in the
in-game menu. The plugin does not keep this in memory: when the menu
opens it reads only the glyphs the shown names need (plugin/gui/jpfont.c).

usage: tools/gen_jpfont.py <NotoSansJP font> <out.bin> [pixel size]
Font: Noto Sans JP (SIL Open Font License 1.1), see
plugin/gui/FONT_LICENSE_JP.txt.

File format (little endian), see plugin/gui/jpfont.h:
  header   32 bytes: "JVJF", u16 version 1, u16 reserved,
           u32 count, u32 nblocks, u32 index_off, u32 entries_off,
           u32 data_off, u32 data_size
  index    nblocks x u32: first code point of each block of 64 entries
  entries  count x 16 bytes, sorted by code point:
           u32 code point, u32 data offset, u8 w, u8 h, s8 x_off,
           s8 y_off (from the top of a font_large line), u8 advance, 3 pad
  data     per glyph w*h 4-bit coverage values, two per byte (high
           nibble first), each glyph starting on a byte
"""
import struct
import sys
from PIL import Image, ImageDraw, ImageFont

BLOCK = 64
LINE_ASCENT = 24  # font_large's ascent: shares its baseline


def charset():
    cps = set()
    # Shift-JIS (cp932): JIS X 0208 rows 1-8 (symbols, kana, Greek,
    # Cyrillic), NEC row 13 (circled numbers, Roman numerals), kanji
    # levels 1 and 2
    for lead in list(range(0x81, 0x85)) + [0x87] + list(range(0x88, 0xA0)) + \
            list(range(0xE0, 0xEB)):
        for trail in list(range(0x40, 0x7F)) + list(range(0x80, 0xFD)):
            try:
                ch = bytes([lead, trail]).decode("cp932")
            except UnicodeDecodeError:
                continue
            if len(ch) == 1:
                cps.add(ord(ch))
    cps.update(range(0xA0, 0x100))      # Latin-1 (accented letters)
    cps.update(range(0x2010, 0x2028))   # dashes, quotes, ellipsis
    cps.update([0x2122, 0x2605, 0x2606, 0x2661, 0x2665, 0x266A, 0x266B,
                0x301C, 0x30A0, 0x30FB])
    cps.update(range(0x2160, 0x2180))   # Roman numerals
    cps.update(range(0x2460, 0x2474))   # circled numbers
    cps.update(range(0x3041, 0x3097))   # hiragana
    cps.update(range(0x309B, 0x3100))   # katakana
    cps.update(range(0xFF01, 0xFF5F))   # full-width ASCII
    cps.update(range(0xFF61, 0xFFA0))   # half-width katakana
    return sorted(c for c in cps if c >= 0x80)


def main():
    path, out = sys.argv[1], sys.argv[2]
    size = int(sys.argv[3]) if len(sys.argv) > 3 else 22
    font = ImageFont.truetype(path, size)
    try:
        font.set_variation_by_name("SemiBold")
    except Exception:
        pass
    asc, _ = font.getmetrics()

    def render(ch):
        bbox = font.getbbox(ch)
        w, h = max(0, bbox[2] - bbox[0]), max(0, bbox[3] - bbox[1])
        img = Image.new("L", (max(1, w), max(1, h)), 0)
        ImageDraw.Draw(img).text((-bbox[0], -bbox[1]), ch, font=font, fill=255)
        return bbox, w, h, img.tobytes()

    tofu = render(chr(0x10FFFD))  # what a missing glyph looks like

    entries, data = [], bytearray()
    for cp in charset():
        ch = chr(cp)
        r = render(ch)
        if r[1:] == tofu[1:]:
            continue  # not in the font
        bbox, w, h, pixels = r
        adv = int(round(font.getlength(ch)))
        if w > 255 or h > 255 or adv > 255:
            continue
        off = len(data)
        if w and h:
            px = [(v * 15 + 127) // 255 for v in pixels]
            if len(px) & 1:
                px.append(0)
            data += bytes((px[i] << 4) | px[i + 1] for i in range(0, len(px), 2))
        yoff = bbox[1] - asc + LINE_ASCENT
        entries.append(struct.pack("<IIBBbbB3x", cp, off, w, h, bbox[0], yoff,
                                   adv))

    count = len(entries)
    index = [struct.unpack_from("<I", entries[i])[0]
             for i in range(0, count, BLOCK)]
    index_off = 32
    entries_off = index_off + 4 * len(index)
    data_off = entries_off + 16 * count
    hdr = struct.pack("<4sHHIIIIII", b"JVJF", 1, 0, count, len(index),
                      index_off, entries_off, data_off, len(data))
    with open(out, "wb") as f:
        f.write(hdr)
        f.write(b"".join(struct.pack("<I", c) for c in index))
        f.write(b"".join(entries))
        f.write(data)
    print(f"{count} glyphs, {size}px, {data_off + len(data)} bytes",
          file=sys.stderr)


if __name__ == "__main__":
    main()
