#!/usr/bin/env python3
"""Lists the animation sequences of a GoldSrc .mdl with their looping flag and bounding box
relative to the model origin. The bounding box tells where a pose puts the body, e.g. a
sitting pose that reaches 35 units below the origin needs the origin 35 above the floor.

Usage: mdlseq.py MODEL.mdl [--grep NAME ...]
"""
import argparse
import struct


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mdl")
    ap.add_argument("--grep", nargs="*", default=[])
    args = ap.parse_args()
    d = open(args.mdl, "rb").read()
    n, idx = struct.unpack_from("<ii", d, 164)
    for i in range(n):
        o = idx + i * 176
        name = d[o:o + 32].split(b"\0")[0].decode()
        if args.grep and not any(g.lower() in name.lower() for g in args.grep):
            continue
        flags = struct.unpack_from("<i", d, o + 36)[0]
        bmin = [round(v) for v in struct.unpack_from("<3f", d, o + 96)]
        bmax = [round(v) for v in struct.unpack_from("<3f", d, o + 108)]
        print(f"{i:3} {name:28} {'loop' if flags & 1 else '    '} min {bmin} max {bmax}")


if __name__ == "__main__":
    main()
