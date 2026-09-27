#!/usr/bin/env python3
"""Renders a labelled contact sheet of WAD3 textures, or lists texture names, so textures
can be picked by looking at them instead of guessing from names.

Usage:
  texsheet.py list WAD [WAD ...] [--grep KEYWORD ...]          names with sizes
  texsheet.py sheet OUT.png WAD [WAD ...] --names TEX [TEX ...]  contact sheet
Example: texsheet.py sheet /tmp/s.png halflife.wad xeno.wad --names FIFTIES_FLR01 C4A1A_SWMPFLR
"""
import argparse
import struct

from PIL import Image, ImageDraw


def load_wad(path):
    f = open(path, "rb").read()
    n, off = struct.unpack_from("<ii", f, 4)
    entries = {}
    for i in range(n):
        e = off + i * 32
        pos = struct.unpack_from("<i", f, e)[0]
        name = f[e + 16:e + 32].split(b"\0")[0].decode("latin1").upper()
        entries[name] = (f, pos)
    return entries


def image(f, p):
    w, h = struct.unpack_from("<II", f, p + 16)
    o0 = struct.unpack_from("<I", f, p + 24)[0]
    pal = p + 40 + w * h + (w * h) // 4 + (w * h) // 16 + (w * h) // 64 + 2
    im = Image.frombytes("P", (w, h), f[p + o0:p + o0 + w * h])
    im.putpalette(f[pal:pal + 768])
    return im.convert("RGB")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    ls = sub.add_parser("list")
    ls.add_argument("wads", nargs="+")
    ls.add_argument("--grep", nargs="*", default=[])
    sh = sub.add_parser("sheet")
    sh.add_argument("out")
    sh.add_argument("wads", nargs="+")
    sh.add_argument("--names", nargs="+", required=True)
    sh.add_argument("--cell", type=int, default=128)
    args = ap.parse_args()

    entries = {}
    for w in args.wads:
        entries.update(load_wad(w))
    if args.cmd == "list":
        for name, (f, p) in sorted(entries.items()):
            if not args.grep or any(k.upper() in name for k in args.grep):
                w, h = struct.unpack_from("<II", f, p + 16)
                print(f"{name} {w}x{h}")
        return
    cols, cell = 6, args.cell
    rows = (len(args.names) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * cell, rows * (cell + 14)), (40, 40, 40))
    draw = ImageDraw.Draw(sheet)
    for i, name in enumerate(args.names):
        x, y = (i % cols) * cell, (i // cols) * (cell + 14)
        draw.text((x + 2, y + 1), name, fill=(255, 255, 0))
        if name.upper() in entries:
            im = image(*entries[name.upper()])
            im.thumbnail((cell, cell))
            sheet.paste(im, (x, y + 14))
    sheet.save(args.out)
    print(args.out)


if __name__ == "__main__":
    main()
