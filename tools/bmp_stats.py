#!/usr/bin/env python3
"""bmp_stats.py - per-frame luminance statistics for 32bpp BGRA BMP dumps.

Usage:  python tools/bmp_stats.py demo_out/*.bmp
Reports mean/max luma, non-black ratio and near-black ratio so that
"black frame" regressions (e.g. a black interpolated frame) can be detected
without relying on manual screenshots.
"""
import glob
import os
import struct
import sys


def read_bmp(path):
    with open(path, "rb") as fh:
        d = fh.read()
    off = struct.unpack_from("<I", d, 10)[0]
    w, h = struct.unpack_from("<ii", d, 18)
    bpp = struct.unpack_from("<H", d, 28)[0]
    return d, w, h, bpp, off


def stats(path):
    d, w, h, bpp, off = read_bmp(path)
    n = w * abs(h)
    body = d[off:off + n * (bpp // 8)]
    step = bpp // 8
    total = 0
    mx = 0
    nonzero = 0
    near_black = 0
    for i in range(0, n * step, step):
        b, g, r = body[i], body[i + 1], body[i + 2]
        y = (b * 114 + g * 587 + r * 299) // 1000
        total += y
        if y > mx:
            mx = y
        if y > 0:
            nonzero += 1
        if y < 8:
            near_black += 1
    return w, h, total / n, mx, nonzero * 100.0 / n, near_black * 100.0 / n


def main():
    patterns = sys.argv[1:] or ["demo_out/*.bmp"]
    files = []
    for p in patterns:
        files.extend(sorted(glob.glob(p)))
    if not files:
        print("no BMP matched")
        return 1
    print(f"{'file':44s} {'size':>11s} {'meanY':>8s} {'maxY':>5s} {'nonblack%':>10s} {'dark<8%':>8s}")
    flagged = 0
    for f in files:
        try:
            w, h, mean, mx, nz, dark = stats(f)
        except Exception as exc:  # noqa: BLE001
            print(f"{os.path.basename(f):44s}  <unreadable: {exc}>")
            continue
        flag = ""
        if mean < 2.0 or dark > 95.0:
            flag = "   <== BLACK FRAME"
            flagged += 1
        print(f"{os.path.basename(f):44s} {w:5d}x{h:<5d} {mean:8.2f} {mx:5d} {nz:10.2f} {dark:8.2f}{flag}")
    print(f"\n{flagged} frame(s) flagged as black/empty out of {len(files)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
