#!/usr/bin/env python3
"""Learns how the original maps use each texture: reads every BSP (version 30) in a folder
and writes per-texture statistics (median scale, typical repeats, face height) as JSON.

Usage: texstats.py MAPS_DIR OUT.json [--skip name ...] [--show TEX ...]
Example: texstats.py ~/.local/share/Steam/steamapps/common/Half-Life/valve/maps usage.json --show METAL_WALL04
BSP faces are split by the compiler, so repeats are indicative; the scale is reliable.
"""
import argparse
import glob
import json
import math
import os
import statistics as st
import struct
from collections import defaultdict


def read_bsp(path, data):
    d = open(path, "rb").read()
    if struct.unpack_from("<i", d, 0)[0] != 30:
        return
    lumps = [struct.unpack_from("<ii", d, 4 + i * 8) for i in range(15)]

    def lump(i):
        o, n = lumps[i]
        return d[o:o + n]

    tex = lump(2)
    count = struct.unpack_from("<i", tex, 0)[0]
    mips = []
    for o in struct.unpack_from(f"<{count}i", tex, 4):
        if o < 0:
            mips.append(("", 0, 0))
            continue
        name = tex[o:o + 16].split(b"\0")[0].decode("latin1").upper()
        w, h = struct.unpack_from("<II", tex, o + 16)
        mips.append((name, w, h))
    ti = lump(6)
    texinfo = [struct.unpack_from("<8f2i", ti, i * 40) for i in range(len(ti) // 40)]
    vb = lump(3)
    verts = [struct.unpack_from("<3f", vb, i * 12) for i in range(len(vb) // 12)]
    eb = lump(12)
    edges = [struct.unpack_from("<2H", eb, i * 4) for i in range(len(eb) // 4)]
    sb = lump(13)
    surfedges = struct.unpack_from(f"<{len(sb) // 4}i", sb, 0)
    fb = lump(7)
    base = os.path.basename(path)[:-4]
    for i in range(len(fb) // 20):
        _, _, first, num, tinfo = struct.unpack_from("<HHiHH", fb, i * 20)
        t = texinfo[tinfo]
        name, w, h = mips[t[8]]
        su = math.sqrt(t[0] ** 2 + t[1] ** 2 + t[2] ** 2)
        sv = math.sqrt(t[4] ** 2 + t[5] ** 2 + t[6] ** 2)
        if not name or not w or not su or not sv:
            continue
        us, vs = [], []
        for k in range(num):
            e = surfedges[first + k]
            p = verts[edges[e][0] if e >= 0 else edges[-e][1]]
            us.append(p[0] * t[0] + p[1] * t[1] + p[2] * t[2])
            vs.append(p[0] * t[4] + p[1] * t[5] + p[2] * t[6])
        data[name].append((1 / su, 1 / sv, max(us) - min(us), max(vs) - min(vs), w, h, base))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("maps_dir")
    ap.add_argument("out")
    ap.add_argument("--skip", nargs="*", default=[], help="map names to ignore (your own maps)")
    ap.add_argument("--show", nargs="*", default=[], help="textures to print")
    args = ap.parse_args()

    data = defaultdict(list)
    for path in sorted(glob.glob(os.path.join(args.maps_dir, "*.bsp"))):
        if os.path.basename(path)[:-4] not in args.skip:
            read_bsp(path, data)
    out = {}
    for name, rows in data.items():
        out[name] = {
            "size": [rows[0][4], rows[0][5]],
            "faces": len(rows),
            "maps": len({r[6] for r in rows}),
            "scaleU": round(st.median(r[0] for r in rows), 3),
            "scaleV": round(st.median(r[1] for r in rows), 3),
            "repeatsU_median": round(st.median(r[2] / r[4] for r in rows), 2),
            "repeatsV_median": round(st.median(r[3] / r[5] for r in rows), 2),
            "faceHeightWorld_median": round(st.median(r[3] * r[1] for r in rows), 1),
        }
    json.dump(out, open(args.out, "w"), indent=1, sort_keys=True)
    print(f"{len(out)} textures -> {args.out}")
    for name in args.show:
        print(name, out.get(name.upper()))


if __name__ == "__main__":
    main()
