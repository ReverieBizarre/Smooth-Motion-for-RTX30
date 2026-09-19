#!/usr/bin/env python3
"""analyze_presentmon.py - find scanout-level tearing in a PresentMon v1 CSV.

Why this can see what a screen recorder cannot: PresentMon's
`msBetweenDisplayChange` is the time between two *display updates* recorded by
the OS. One flip cannot land faster than one vblank (1000/144 = 6.94 ms on this
panel). An interval clearly below that means the scanout switched content
mid-frame - i.e. a tear - which never appears in the composited image a
recorder captures.

Usage: python tools/analyze_presentmon.py <pm.csv> [vblank_ms]
"""
import csv
import collections
import sys


def pct(vals, q):
    if not vals:
        return 0.0
    s = sorted(vals)
    return s[min(len(s) - 1, int(q * len(s)))]


def num(v):
    try:
        return float(v)
    except (TypeError, ValueError):
        return None


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "pm.csv"
    vblank = float(sys.argv[2]) if len(sys.argv) > 2 else 6.9444

    with open(path, "r", encoding="utf-8", errors="replace", newline="") as fh:
        rows = list(csv.DictReader(fh))
    print(f"rows: {len(rows)}   assumed vblank: {vblank:.3f} ms\n")

    by_sc = collections.defaultdict(list)
    for r in rows:
        by_sc[r["SwapChainAddress"]].append(r)

    for sc, rs in sorted(by_sc.items(), key=lambda kv: -len(kv[1])):
        sync = collections.Counter(r["SyncInterval"] for r in rs)
        flags = collections.Counter(r["PresentFlags"] for r in rs)
        mode = collections.Counter(r["PresentMode"] for r in rs)
        tear = collections.Counter(r["AllowsTearing"] for r in rs)
        dropped = collections.Counter(r["Dropped"] for r in rs)
        runtime = collections.Counter(r["Runtime"] for r in rs)
        bp = [num(r["msBetweenPresents"]) for r in rs if num(r["msBetweenPresents"]) is not None]
        bd = [num(r["msBetweenDisplayChange"]) for r in rs if num(r["msBetweenDisplayChange"]) is not None]
        fd = [num(r["msFlipDelay"]) for r in rs if num(r["msFlipDelay"]) is not None]
        ud = [num(r["msUntilDisplayed"]) for r in rs if num(r["msUntilDisplayed"]) is not None]

        print(f"--- swapchain {sc}  ({len(rs)} presents) ---")
        print(f"    Runtime={dict(runtime)}  SyncInterval={dict(sync)}  AllowsTearing={dict(tear)}")
        print(f"    PresentFlags={dict(flags)}")
        print(f"    PresentMode={dict(mode)}")
        print(f"    Dropped={dict(dropped)}  (1 = never displayed)")
        print(f"    msBetweenPresents      p50={pct(bp,0.5):7.3f}  p95={pct(bp,0.95):7.3f}  max={max(bp) if bp else 0:8.3f}")
        print(f"    msBetweenDisplayChange p50={pct(bd,0.5):7.3f}  p95={pct(bd,0.95):7.3f}  min={min(bd) if bd else 0:8.3f}")
        print(f"    msUntilDisplayed       p50={pct(ud,0.5):7.3f}  p95={pct(ud,0.95):7.3f}")
        print(f"    msFlipDelay            p50={pct(fd,0.5):7.3f}  p95={pct(fd,0.95):7.3f}  max={max(fd) if fd else 0:8.3f}")

        # ---- the tearing detector -------------------------------------------
        # Count display intervals shorter than a vblank; those cannot come from
        # two separate vblank-aligned flips.
        fast = [(i, d) for i, d in enumerate(bd) if d < vblank * 0.85]
        print(f"    display changes faster than one vblank: {len(fast)} / {len(bd)}"
              f"  ({100.0*len(fast)/max(1,len(bd)):.1f}%)")
        if fast:
            hist = collections.Counter(round(d / 1.0) for _, d in fast)
            print(f"      interval histogram (ms -> count): {dict(sorted(hist.items()))}")

        # vblank multiples: a clean pipeline shows ~k*vblank between updates
        mult = collections.Counter()
        for d in bd:
            mult[round(d / vblank)] += 1
        print(f"    display interval in vblank units: {dict(sorted(mult.items()))}")

        # pacing drift: is msBetweenPresents itself stable?
        if bp:
            dev = [abs(b - pct(bp, 0.5)) for b in bp]
            print(f"    |msBetweenPresents - p50| p95 = {pct(dev,0.95):.3f} ms")
        print()

    # ---- displayed-frame timeline over time --------------------------------
    allrows = sorted(rows, key=lambda r: num(r["TimeInSeconds"]) or 0)
    disp = [num(r["msBetweenDisplayChange"]) for r in allrows
            if num(r["msBetweenDisplayChange"]) is not None]
    if disp:
        anomalies = [(i, d) for i, d in enumerate(disp) if d < vblank * 0.85]
        print(f"ALL swapchains: {len(anomalies)} sub-vblank display intervals out of {len(disp)}")
        if anomalies:
            print("first 20 (index, ms):", [(i, round(d, 2)) for i, d in anomalies[:20]])
        over = [d for d in disp if d > 3 * vblank]
        print(f"display intervals longer than 3 vblanks ({3*vblank:.1f} ms): {len(over)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
