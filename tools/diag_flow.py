# ---------------------------------------------------------------------------
#  Compare the estimated flow against the analytic field, by region.
#
#  A single "49% of pixels are within 1 px" number does not say *where* the
#  matcher is wrong, and that is the only thing that tells you what to fix:
#  a uniform error means the criterion is biased, an error that grows with
#  distance from the frame centre means the pyramid seeds are drifting, an
#  error concentrated on the smooth blobs means the matcher cannot lock on
#  low-texture content, and so on.
# ---------------------------------------------------------------------------
import struct
import sys
import os
import math

W = int(sys.argv[2].split('x')[0])
H = int(sys.argv[2].split('x')[1])
D = os.path.join(sys.argv[1], '%dx%d_flowAB.f32' % (W, H))
C = os.path.join(sys.argv[1], '%dx%d_cost.f32' % (W, H))

raw = open(D, 'rb').read()
n = W * H
fx = [0.0] * n
fy = [0.0] * n
for i in range(n):
    fx[i], fy[i] = struct.unpack_from('<ff', raw, i * 8)

cost = None
if os.path.exists(C):
    cr = open(C, 'rb').read()
    cost = list(struct.unpack_from('<%df' % n, cr, 0))

# ---- analytic field (must mirror analyticFlowAB in selftest.cpp) ----
Dan = W * 0.04
ang = 0.60
ca, sa = math.cos(ang), math.sin(ang)
bcx, bcy = W * 0.62, H * 0.40
bhx, bhy = W * 0.19 * 0.5, H * 0.055 * 0.5
bdcx, bdcy, bdr = W * 0.22, H * 0.62, H * 0.14
dxs = W * 0.24


def kind(x, y):
    """0 = pure background, 1 = inside disc at t=0, 2 = inside bar at t=0,
       3 = the union of all object positions across t=0/0.5/1 (contaminated)."""
    for k, cx in enumerate((W * 0.22, W * 0.34, W * 0.46)):
        ex, ey = x - cx, y - bdcy
        if ex * ex + ey * ey < (bdr + 4) ** 2:
            return 3
    for k in range(3):
        a = 0.30 * k
        c2, s2 = math.cos(a), math.sin(a)
        bx, by = x - bcx, y - bcy
        lx = c2 * bx + s2 * by
        ly = -s2 * bx + c2 * by
        if abs(lx) < bhx + 4 and abs(ly) < bhy + 4:
            return 3
    ex, ey = x - bdcx, y - bdcy
    if ex * ex + ey * ey < bdr * bdr:
        return 1
    bx, by = x - bcx, y - bcy
    if abs(bx) < bhx and abs(by) < bhy:
        return 2
    return 0


groups = {0: [], 1: [], 2: [], 3: []}          # -> list of (|ex|, |ey|)
costbg = []
for y in range(H):
    for x in range(W):
        i = y * W + x
        ax = Dan
        ay = 0.0
        k = kind(x + 0.5, y + 0.5)
        if k == 1:
            ax, ay = dxs, 0.0
        elif k == 2:
            bx, by = x + 0.5 - bcx, y + 0.5 - bcy
            lx = ca * bx - sa * by
            ly = sa * bx + ca * by
            ax, ay = (bcx + lx) - (x + 0.5), (bcy + ly) - (y + 0.5)
        groups[k].append((abs(fx[i] - ax), abs(fy[i] - ay)))
        if k == 0 and cost:
            costbg.append(cost[i])


def pct(v, q):
    if not v:
        return float('nan')
    v = sorted(v)
    return v[min(len(v) - 1, int(q * (len(v) - 1) + 0.5))]


names = {0: 'pure background', 1: 'inside disc (t=0)',
         2: 'inside bar (t=0)', 3: 'near object / contaminated'}
print('frame %dx%d   analytic background shift = %.2f px' % (W, H, Dan))
for k in (0, 1, 2, 3):
    g = groups[k]
    if not g:
        continue
    ex = [a for a, b in g]
    ey = [b for a, b in g]
    good = sum(1 for a, b in g if a <= 1.0 and b <= 1.0) / len(g) * 100.0
    print('  %-26s n=%7d  within1px=%5.1f%%  |ex| p50=%7.2f p90=%7.2f  |ey| p50=%7.2f p90=%7.2f'
          % (names[k], len(g), good, pct(ex, .5), pct(ex, .9), pct(ey, .5), pct(ey, .9)))

if costbg:
    print('  background confidence: p50=%.4f p90=%.4f p99=%.4f  (0 = trusted)'
          % (pct(costbg, .5), pct(costbg, .9), pct(costbg, .99)))
    frac_fill = sum(1 for c in costbg if c > 0.35) / len(costbg) * 100.0
    print('  background cells sent to hole fill (>0.35): %.1f%%' % frac_fill)

# ---- error as a function of distance from the frame edge ----
# If the error grows toward the borders, blocks are sampling clamped edge
# pixels; if it is flat, the criterion itself is biased.
print('\n  |ex| p50 and within-1px by distance from the nearest vertical edge')
bg = groups[0]
if bg:
    W2 = W
    buckets = [(0, 32), (32, 64), (64, 128), (128, 256), (256, 512), (512, 10 ** 9)]
    counts = {b: [0, 0, []] for b in buckets}
    for y in range(H):
        for x in range(W):
            if kind(x + 0.5, y + 0.5) != 0:
                continue
            d = min(x, W - 1 - x)
            for b in buckets:
                if b[0] <= d < b[1]:
                    e = abs(fx[y * W + x] - Dan)
                    counts[b][0] += 1
                    counts[b][1] += 1 if e <= 1.0 else 0
                    counts[b][2].append(e)
                    break
    for b in buckets:
        c, g, e = counts[b]
        if not c:
            continue
        print('    d=%4d..%-8d n=%7d  within1px=%5.1f%%  p50=%7.2f'
              % (b[0], b[1] if b[1] < 10 ** 9 else -1, c, 100.0 * g / c, pct(e, .5)))

# ---- visual maps ----
try:
    from PIL import Image
    have = True
except Exception:
    have = False
if have:
    def mk(fn, fnc):
        im = Image.new('RGB', (W, H))
        px = im.load()
        for y in range(H):
            for x in range(W):
                px[x, y] = fnc(x, y)
        im.save(fn)
    def clamp8(v):
        return max(0, min(255, int(v)))
    mk(os.path.join(sys.argv[1], '%dx%d_flowerr.png' % (W, H)),
       lambda x, y: (clamp8(abs(fx[y * W + x] - Dan) * 8),) * 3)
    mx = max(abs(v) for v in fx) or 1.0
    mk(os.path.join(sys.argv[1], '%dx%d_flowx.png' % (W, H)),
       lambda x, y: (clamp8(128 + 127 * fx[y * W + x] / mx),) * 3)
    print('\n  wrote flowerr.png (|error| x8) and flowx.png (normalised estimate)')
else:
    print('\n  (no PIL; skipped PNG maps)')
