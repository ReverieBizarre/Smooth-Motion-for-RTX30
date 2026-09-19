#!/usr/bin/env python3
"""analyze_frames.py - temporal-median anomaly detection for frame dumps.

Recovers short-lived "black frame" / "flash" events that a manual screenshot
(or a 30 fps screen recording) normally misses, by comparing every frame
against the temporal median of the same tile position.

Pipeline:
  ffmpeg -i clip.mp4 -vf scale=1:1:flags=area   -pix_fmt gray -f rawvideo mean.raw
  ffmpeg -i clip.mp4 -vf scale=48:32:flags=area -pix_fmt gray -f rawvideo tiles.raw

Usage:
  python tools/analyze_frames.py <mean.raw> <tiles.raw> [fps] [tileW] [tileH]
"""
import sys


def read_raw(path):
    with open(path, "rb") as fh:
        return fh.read()


def median(xs):
    s = sorted(xs)
    n = len(s)
    if n == 0:
        return 0
    return s[n // 2] if n % 2 else (s[n // 2 - 1] + s[n // 2]) / 2


def main():
    mean_path = sys.argv[1] if len(sys.argv) > 1 else "capture_out/mean1x1.raw"
    tile_path = sys.argv[2] if len(sys.argv) > 2 else "capture_out/mean32.raw"
    fps = float(sys.argv[3]) if len(sys.argv) > 3 else 30.0
    tw = int(sys.argv[4]) if len(sys.argv) > 4 else 32
    th = int(sys.argv[5]) if len(sys.argv) > 5 else 32

    means = read_raw(mean_path)
    tiles = read_raw(tile_path)
    n_tiles = tw * th
    n = min(len(means), len(tiles) // n_tiles)
    if n == 0:
        print("no frames")
        return 1

    # temporal median per tile position
    med = []
    for k in range(n_tiles):
        col = [tiles[i * n_tiles + k] for i in range(n)]
        med.append(median(col))

    print(f"frames={n} fps={fps:g} duration={n / fps:.2f}s  tile grid={tw}x{th}")
    print(f"{'#':>4s} {'t(s)':>7s} {'mean':>5s} {'medMean':>8s} {'drop':>6s} {'tiles<50%':>10s} {'verdict'}")
    events = []
    for i in range(n):
        row = tiles[i * n_tiles:(i + 1) * n_tiles]
        rmean = sum(row) / n_tiles
        mmean = sum(med) / n_tiles
        drop = (1.0 - rmean / mmean) * 100.0 if mmean else 0.0
        dark = sum(1 for k in range(n_tiles) if med[k] > 12 and row[k] < med[k] * 0.5)
        verdict = ""
        if dark > n_tiles * 0.5 or drop > 40:
            verdict = "FULL-FRAME BLACK/FLASH"
            events.append((i, dark, drop))
        elif dark > n_tiles * 0.08:
            verdict = f"PARTIAL BLACK ({dark} tiles)"
            events.append((i, dark, drop))
        print(f"{i:4d} {i / fps:7.3f} {means[i]:5d} {mmean:8.1f} {drop:5.1f}% {dark:10d}  {verdict}")

    # dominant frame-to-frame cadence (a duplicated/stale frame shows as 0 delta)
    deltas = [abs(means[i] - means[i - 1]) for i in range(1, n)]
    frozen = [i + 1 for i, d in enumerate(deltas) if d == 0]
    print(f"\nevents: {[(e[0], e[2]) for e in events]}")
    print(f"frames identical to predecessor (stale/frozen): {frozen}")

    if events:
        i = min(events, key=lambda e: e[1] * -1)[0]
        print(f"\nworst event frame #{i} (t={i / fps:.3f}s), tile map vs median:")
        chars = " .:-=+*#%@"
        row = tiles[i * n_tiles:(i + 1) * n_tiles]
        for y in range(th):
            line = ""
            for x in range(tw):
                k = y * tw + x
                line += chars[min(9, row[k] * 10 // 256)]
            print("   " + line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
