#!/usr/bin/env python3
"""display_timeline.py - reconstruct the actual display-update cadence.

`msBetweenDisplayChange` is per-present and reads 0 for presents that were never
displayed, so the raw column overstates anomalies. This rebuilds the timeline
from TimeInSeconds + msUntilDisplayed, which is where each frame really landed
on the panel, and reports the real update cadence and any sub-vblank gap.

Usage: python capture_out/display_timeline.py <pm.csv> [vblank_ms]
"""
import csv
import collections
import sys


def num(v):
    try:
        return float(v)
    except (TypeError, ValueError):
        return None


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "capture_out/pm_24fps_fgon.csv"
    vblank = float(sys.argv[2]) if len(sys.argv) > 2 else 6.9444

    with open(path, "r", encoding="utf-8", errors="replace", newline="") as fh:
        rows = list(csv.DictReader(fh))

    pts = []
    for r in rows:
        t = num(r["TimeInSeconds"])
        ud = num(r["msUntilDisplayed"])
        if t is None or ud is None:
            continue
        pts.append((r["Dropped"], t + ud / 1000.0, num(r["msBetweenPresents"]) or 0.0))
    pts.sort(key=lambda x: x[1])

    print(f"rows={len(rows)}  with display time={len(pts)}  vblank={vblank:.3f} ms\n")

    shown = [p for p in pts if p[0] == "0"]
    print(f"presented (Dropped=0): {len(shown)}   never displayed (Dropped=1): {len(pts)-len(shown)}")
    if len(shown) < 2:
        return 0

    span = shown[-1][1] - shown[0][1]
    print(f"display window: {span:.3f} s   -> {len(shown)/span:.2f} display updates/s\n")

    # collapse to unique display instants (a tear = two different contents land
    # at the same instant, a flip can never be faster than a vblank)
    uniq = []
    for _, t, _ in shown:
        if not uniq or t - uniq[-1] > 1e-6:
            uniq.append(t)
    d = [uniq[i] - uniq[i - 1] for i in range(1, len(uniq))]
    print(f"unique display instants: {len(uniq)}  -> {len(uniq)/span:.2f} updates/s")

    if d:
        s = sorted(d)
        print(f"interval p05={s[int(0.05*len(s))]*1000:7.3f}  p50={s[len(s)//2]*1000:7.3f}  "
              f"p95={s[int(0.95*len(s))]*1000:7.3f}  min={min(d)*1000:7.3f}  max={max(d)*1000:8.3f} ms")

        mult = collections.Counter(round(x * 1000.0 / vblank) for x in d)
        print(f"intervals in vblank units: {dict(sorted(mult.items()))}")

        sub = [x for x in d if x * 1000.0 < vblank * 0.8]
        print(f"SUB-VBLANK intervals (<{vblank*0.8:.2f} ms) = {len(sub)} / {len(d)}"
              f"  ({100.0*len(sub)/len(d):.1f}%)   <-- each one is a mid-scan buffer switch")

        # how often does the panel hold a frame for exactly k vblanks
        hold = collections.Counter(round(x * 1000.0 / vblank) for x in d)
        total = sum(hold.values())
        for k in sorted(hold):
            print(f"    hold {k:3d} vblank(s): {hold[k]:5d}  ({100.0*hold[k]/total:5.1f}%)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
