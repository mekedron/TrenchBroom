#!/usr/bin/env python3
"""Lists z-fighting in a .map file: visible faces of different brushes that lie in the same
plane, face the same way and overlap, and are not hidden by a touching face of another brush.

Usage: zfight.py MAP [--limit N]
Tool textures (CLIP, ORIGIN, AAATRIGGER, ...) and trigger entities are ignored.
"""
import argparse
import itertools
import os
import re
import sys
from collections import defaultdict

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mapio import P, parse  # noqa: E402

SKIP = {"CLIP", "ORIGIN", "AAATRIGGER", "SKIP", "NULL", "HINT", "BEVEL"}


def planes_of(brush):
    out = []
    for line in brush:
        m = re.match(r"\(([^)]*)\)\s*\(([^)]*)\)\s*\(([^)]*)\)\s*(\S+)", line)
        p = [np.array([float(v) for v in m.group(i).split()]) for i in (1, 2, 3)]
        n = np.cross(p[2] - p[0], p[1] - p[0])
        n /= np.linalg.norm(n)
        out.append((n, float(n @ p[0]), m.group(4)))
    return out


def clip(poly, n, d):
    res = []
    for i in range(len(poly)):
        a, b = poly[i], poly[(i + 1) % len(poly)]
        da, db = a @ n - d, b @ n - d
        if da <= 1e-6:
            res.append(a)
        if (da > 1e-6 and db < -1e-6) or (da < -1e-6 and db > 1e-6):
            res.append(a + da / (da - db) * (b - a))
    return res


def face_polys(brush):
    planes = planes_of(brush)
    out = []
    for i, (n, d, tex) in enumerate(planes):
        u = np.cross(n, [0, 0, 1] if abs(n[2]) < 0.9 else [1, 0, 0])
        u /= np.linalg.norm(u)
        v = np.cross(n, u)
        c, s = n * d, 1e5
        poly = [c + s * (u + v), c + s * (u - v), c + s * (-u - v), c + s * (-u + v)]
        for j, (n2, d2, _) in enumerate(planes):
            if j != i:
                poly = clip(poly, n2, d2)
            if not poly:
                break
        if len(poly) >= 3:
            out.append((n, d, tex, poly))
    return out


def area2d(poly):
    return 0.5 * abs(sum(poly[i][0] * poly[i - 1][1] - poly[i - 1][0] * poly[i][1] for i in range(len(poly))))


def clip2d(subject, clipper):
    def ccw(p):
        s = sum(p[i - 1][0] * p[i][1] - p[i][0] * p[i - 1][1] for i in range(len(p)))
        return p if s > 0 else p[::-1]

    def inside(p, a, b):
        return (b[0] - a[0]) * (p[1] - a[1]) - (b[1] - a[1]) * (p[0] - a[0]) >= -1e-6

    def inter(p, q, a, b):
        m = np.array([[q[0] - p[0], a[0] - b[0]], [q[1] - p[1], a[1] - b[1]]])
        t = np.linalg.solve(m, [a[0] - p[0], a[1] - p[1]])[0]
        return (p[0] + t * (q[0] - p[0]), p[1] + t * (q[1] - p[1]))

    out, cl = ccw(subject), ccw(clipper)
    for i in range(len(cl)):
        a, b = cl[i - 1], cl[i]
        inp, out = out, []
        for j in range(len(inp)):
            p, q = inp[j - 1], inp[j]
            if inside(q, a, b):
                if not inside(p, a, b):
                    out.append(inter(p, q, a, b))
                out.append(q)
            elif inside(p, a, b):
                out.append(inter(p, q, a, b))
        if not out:
            return []
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("map")
    ap.add_argument("--limit", type=int, default=60)
    args = ap.parse_args()

    ents = parse(open(args.map, encoding="latin-1").read())
    bucket = defaultdict(list)
    for ei, e in enumerate(ents):
        cls = P(e).get("classname", "")
        if cls.startswith("trigger_"):
            continue
        for bi, b in enumerate(e["brushes"]):
            for n, d, tex, poly in face_polys(b):
                if tex.upper() in SKIP:
                    continue
                key = (tuple(np.round(n, 4) + 0.0), round(d, 2) + 0.0)
                bucket[key].append((ei, bi, cls, tex, poly))

    def project(poly, keep):
        return [(p[keep[0]], p[keep[1]]) for p in poly]

    def covered(region, key, keep):
        opp = (tuple(-x + 0.0 for x in key[0]), round(-key[1], 2) + 0.0)
        total = 0.0
        for f in bucket.get(opp, []):
            ov = clip2d(region, project(f[4], keep))
            if len(ov) >= 3:
                total += area2d(ov)
        return total >= area2d(region) - 1

    hits = []
    for key, faces in bucket.items():
        if len(faces) < 2:
            continue
        axis = int(np.argmax(np.abs(key[0])))
        keep = [i for i in range(3) if i != axis]
        for a, b in itertools.combinations(faces, 2):
            if (a[0], a[1]) == (b[0], b[1]):
                continue
            ov = clip2d(project(a[4], keep), project(b[4], keep))
            if len(ov) >= 3 and area2d(ov) > 1 and not covered(ov, key, keep):
                center = np.mean(np.array(ov), axis=0)
                hits.append((round(area2d(ov)), a[2], a[3], b[2], b[3],
                             [round(float(x)) for x in center], [float(x) for x in key[0]], key[1]))
    hits.sort(key=lambda h: -h[0])
    print(f"{len(hits)} coplanar overlaps")
    for area, ca, ta, cb, tb, center, normal, dist in hits[: args.limit]:
        print(f"area {area:5}  {ca}:{ta}  vs  {cb}:{tb}  at {center} (2D)  plane n={normal} d={dist}")


if __name__ == "__main__":
    main()
