# ---------------------------------------------------------------------------
#  Masked PSNR.
#
#  A single global PSNR conflates two very different situations:
#    - pixels whose motion is well posed (flat background, hashed detail):
#      a warp should reproduce these almost exactly
#    - pixels on an occlusion boundary (disc leading/trailing edge, the swept
#      area of a rotating bar): no warp of *either* source frame contains the
#      answer, because the surface at that pixel changed identity
#  Without splitting them, "the blend is broken" and "the blend is fine but
#  occlusion is unsolvable from two frames" produce the same number.
# ---------------------------------------------------------------------------
import io

p = 'tools/selftest.cpp'
s = io.open(p, encoding='utf-8').read()


def sub(old, new, cnt=1):
    global s
    assert s.count(old) == cnt, ('anchor miss', old[:70], s.count(old))
    s = s.replace(old, new)


# ---- masked PSNR helper ----
sub('''static void writePPM(const char* path''',
    '''// PSNR restricted to a mask (1 = include).  Anything outside the mask is an
// occlusion boundary and is deliberately excluded.
static double psnrMasked(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b,
                         const std::vector<uint8_t>& mask, size_t* nOut)
{
    double se = 0; size_t n = 0;
    for (size_t i = 0, k = 0; i < a.size(); i += 4, ++k)
    {
        if (!mask[k]) continue;
        for (int c = 0; c < 3; ++c)
        {
            double d = (double)a[i + c] - (double)b[i + c];
            se += d * d; ++n;
        }
    }
    if (nOut) *nOut = n;
    if (n == 0) return 0.0;
    double mse = se / (double)n;
    if (mse <= 1e-12) return 99.0;
    return 10.0 * log10(255.0 * 255.0 / mse);
}

// 1 = "well posed": nowhere near the disc at any of t=0/0.5/1, nor the bar at
// any of those orientations, with a margin so that a wrong answer just outside
// a boundary does not leak in.
static void wellPosedMask(int W, int H, std::vector<uint8_t>& m, float margin)
{
    m.assign((size_t)W * H, 0);
    const float bhx = W * 0.19f * 0.5f + margin;
    const float bhy = H * 0.055f * 0.5f + margin;
    const float bcx = W * 0.62f, bcy = H * 0.40f;
    const float dr  = H * 0.14f + margin;
    const float dcy = H * 0.62f;
    const float dcx[3] = { W * 0.22f, W * 0.34f, W * 0.46f };

    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
        {
            const float px = (float)x, py = (float)y;
            bool obj = false;
            for (int k = 0; k < 3 && !obj; ++k)
            {
                const float ex = px - dcx[k], ey = py - dcy;
                if (ex * ex + ey * ey < dr * dr) obj = true;
            }
            for (int k = 0; k < 3 && !obj; ++k)
            {
                const float a  = 0.30f * (float)k;      // t = k/2
                const float ca = cosf(a), sa = sinf(a);
                const float bx = px - bcx, by = py - bcy;
                const float lx =  ca * bx + sa * by;
                const float ly = -sa * bx + ca * by;
                if (fabsf(lx) < bhx && fabsf(ly) < bhy) obj = true;
            }
            m[(size_t)y * W + x] = obj ? 0 : 1;
        }
}

static void writePPM(const char* path''')

# ---- use it in the summary ----
sub('''    printf("\\n---- %dx%d PSNR(gen vs truth) ----\\n", W, H);
    printf("  no motion, 50/50 average     : %6.2f dB   <- floor\\n", psnrMix);
    printf("  matcher + blend (this build) : %6.2f dB\\n", psnrEst);
    printf("  analytic flow + blend        : %6.2f dB   <- ceiling of this blend\\n", oraclePsnr);
    printf("  cost of the matcher          : %6.2f dB\\n", oraclePsnr - psnrEst);''',
    '''    std::vector<uint8_t> wm;
    wellPosedMask(W, H, wm, (float)W * 0.01f);
    size_t wmN = 0;
    for (uint8_t v : wm) if (v) ++wmN;
    const double wellPct = 100.0 * (double)wmN / ((double)W * (double)H);
    const double mixWell = psnrMasked(mix,  cpuM, wm, nullptr);
    const double estWell = psnrMasked(gen,  cpuM, wm, nullptr);
    const double orcWell = psnrMasked(genO, cpuM, wm, nullptr);

    printf("\\n---- %dx%d PSNR(gen vs truth), every pixel ----\\n", W, H);
    printf("  no motion, 50/50 average     : %6.2f dB   <- floor\\n", psnrMix);
    printf("  matcher + blend (this build) : %6.2f dB\\n", psnrEst);
    printf("  analytic flow + blend        : %6.2f dB   <- ceiling of this blend\\n", oraclePsnr);
    printf("  cost of the matcher          : %6.2f dB\\n", oraclePsnr - psnrEst);

    printf("\\n---- %dx%d PSNR over well-posed pixels only (%.1f%% of frame) ----\\n",
           W, H, wellPct);
    printf("  no motion, 50/50 average     : %6.2f dB\\n", mixWell);
    printf("  matcher + blend (this build) : %6.2f dB\\n", estWell);
    printf("  analytic flow + blend        : %6.2f dB   <- blend ceiling, no occlusion\\n", orcWell);
    printf("  everything outside this mask is an occlusion boundary, where neither\\n");
    printf("  source warp contains the answer - that is the learned model's job.\\n");''')

io.open(p, 'w', encoding='utf-8').write(s)
print('selftest.cpp patched (masked psnr)')
