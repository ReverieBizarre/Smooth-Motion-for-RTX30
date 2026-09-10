# ---------------------------------------------------------------------------
#  Add the oracle measurement to the self test.
#
#  The question "is my hand-written pipeline any good" has two answers tangled
#  together, and PSNR alone cannot tell them apart:
#    (a) the block matcher produces a bad flow field
#    (b) the warp/blend stage itself has a ceiling, whatever the flow
#  Feeding the analytic motion field in through set_flow_override() answers
#  (b) directly: same shaders, same blend, only the flow source changes.
# ---------------------------------------------------------------------------
import io

p = 'tools/selftest.cpp'
s = io.open(p, encoding='utf-8').read()


def sub(old, new, cnt=1):
    global s
    assert s.count(old) == cnt, ('anchor miss', old[:70], s.count(old))
    s = s.replace(old, new)


# ---- 1. uploadTex: generalise to any format / bytes per pixel ----
sub('''static ID3D12Resource* uploadTex(Gpu& g, int W, int H, const uint8_t* data)
{
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = W; rd.Height = H; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;''',
    '''static ID3D12Resource* uploadTexFmt(Gpu& g, int W, int H, const uint8_t* data,
                                    DXGI_FORMAT fmt, int bpp)
{
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = W; rd.Height = H; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = fmt; rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;''')

sub('''        memcpy((uint8_t*)p + fp.Offset + y * fp.Footprint.RowPitch,
               data + (size_t)y * W * 4, (size_t)W * 4);''',
    '''        memcpy((uint8_t*)p + fp.Offset + y * fp.Footprint.RowPitch,
               data + (size_t)y * W * bpp, (size_t)W * bpp);''')

sub('''    upBuf->Release();
    return tex;
}

// generic readback''',
    '''    upBuf->Release();
    return tex;
}

static ID3D12Resource* uploadTex(Gpu& g, int W, int H, const uint8_t* data)
{
    return uploadTexFmt(g, W, H, data, DXGI_FORMAT_R8G8B8A8_UNORM, 4);
}

// generic readback''')

# ---- 2. analytic motion field ----
sub('''// ---------------------------------------------------------------------------
struct Gpu
{''',
    '''// ---------------------------------------------------------------------------
//  The analytic motion field of the same scene, in the engine's convention:
//    fAB(p) is the displacement at p such that  B(p + fAB(p)) ~= A(p)
//  Frame A is t=0 and frame B is t=1.  renderScene draws background, then bar,
//  then disc, so the membership tests below have to be applied in reverse
//  order - the disc sits on top of the bar sits on top of the background.
//
//  Note this is NOT a global constant vector: the bar rotates, so its field is
//  a real per-pixel rotation, and the disc moves 6x faster than the
//  background.  A single-vector oracle would have measured nothing.
// ---------------------------------------------------------------------------
static void analyticFlowAB(int W, int H, std::vector<float>& f)
{
    const float D   = W * 0.04f;
    const float ang = 0.60f;                       // rotation at t = 1
    const float ca  = cosf(ang), sa = sinf(ang);
    const float bcx = W * 0.62f, bcy = H * 0.40f;
    const float bhx = W * 0.19f * 0.5f, bhy = H * 0.055f * 0.5f;
    const float dcx = W * 0.22f, dcy = H * 0.62f, dr = H * 0.14f;
    const float dxs = W * 0.24f;

    f.assign((size_t)W * H * 2, 0.0f);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
        {
            const float px = (float)x, py = (float)y;
            float rx, ry;

            const float ex = px - dcx, ey = py - dcy;
            if (ex * ex + ey * ey < dr * dr) { rx = dxs; ry = 0.0f; }
            else
            {
                const float bx = px - bcx, by = py - bcy;
                if (fabsf(bx) < bhx && fabsf(by) < bhy)
                {
                    // bar local coords at t=0 are just (bx,by); the bar body is
                    // at c + R(ang) * local at t=1
                    const float lx = ca * bx - sa * by;
                    const float ly = sa * bx + ca * by;
                    rx = (bcx + lx) - px; ry = (bcy + ly) - py;
                }
                else { rx = D; ry = 0.0f; }
            }

            const size_t i = ((size_t)y * W + x) * 2;
            f[i] = rx; f[i + 1] = ry;
        }
}

static void analyticFlowBA(int W, int H, std::vector<float>& f)
{
    const float D   = W * 0.04f;
    const float ang = 0.60f;
    const float ca  = cosf(ang), sa = sinf(ang);
    const float bcx = W * 0.62f, bcy = H * 0.40f;
    const float bhx = W * 0.19f * 0.5f, bhy = H * 0.055f * 0.5f;
    const float dcx = W * 0.46f, dcy = H * 0.62f, dr = H * 0.14f;
    const float dxs = -W * 0.24f;                  // disc is at t=1 here

    f.assign((size_t)W * H * 2, 0.0f);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
        {
            const float px = (float)x, py = (float)y;
            float rx, ry;

            const float ex = px - dcx, ey = py - dcy;
            if (ex * ex + ey * ey < dr * dr) { rx = dxs; ry = 0.0f; }
            else
            {
                const float bx = px - bcx, by = py - bcy;
                // membership at t=1: local = R(-ang) * (p - c)
                const float lx =  ca * bx + sa * by;
                const float ly = -sa * bx + ca * by;
                if (fabsf(lx) < bhx && fabsf(ly) < bhy)
                {
                    rx = (bcx + lx) - px; ry = (bcy + ly) - py;
                }
                else { rx = -D; ry = 0.0f; }
            }

            const size_t i = ((size_t)y * W + x) * 2;
            f[i] = rx; f[i + 1] = ry;
        }
}

// 50/50 average - the floor any interpolator has to beat.  With no motion
// information at all this is the only thing you can do.
static std::vector<uint8_t> mix5050(const std::vector<uint8_t>& a,
                                    const std::vector<uint8_t>& b)
{
    std::vector<uint8_t> o(a.size());
    for (size_t i = 0; i < a.size(); i += 4)
    {
        for (int c = 0; c < 3; ++c)
            o[i + c] = (uint8_t)(((int)a[i + c] + (int)b[i + c] + 1) >> 1);
        o[i + 3] = 255;
    }
    return o;
}

// ---------------------------------------------------------------------------
struct Gpu
{''')

# ---- 3. capture the estimator's dispatch count ----
sub('''    printf("dispatches per generated frame : %u\\n", eng.dispatch_count());''',
    '''    const uint32_t estDisp = eng.dispatch_count();
    printf("dispatches per generated frame : %u\\n", estDisp);''')

# ---- 4. estimate PSNR + oracle run ----
sub('''    printf("PSNR(truth vs A)               : %6.2f dB   <- difficulty reference\\n", psnr(cpuA, cpuM));
    printf("PSNR(gen   vs truth)           : %6.2f dB\\n", psnr(gen, cpuM));''',
    '''    std::vector<uint8_t> mix = mix5050(cpuA, cpuB);
    const double psnrMix = psnr(mix, cpuM);
    const double psnrEst = psnr(gen, cpuM);

    printf("PSNR(truth vs A)               : %6.2f dB   <- difficulty reference\\n", psnr(cpuA, cpuM));
    printf("PSNR(gen   vs truth)           : %6.2f dB\\n", psnrEst);''')

sub('''    printf("latency  (1 frame, idle queue) : %6.3f ms\\n", latMs);
    printf("throughput (sustained, n=%d)   : %6.3f ms/frame\\n", iters, thrMs);

    eng.shutdown();''',
    '''    printf("latency  (1 frame, idle queue) : %6.3f ms\\n", latMs);
    printf("throughput (sustained, n=%d)   : %6.3f ms/frame\\n", iters, thrMs);

    // =======================================================================
    //  Ceiling measurement.
    //  Same shaders, same blend, same HF transfer - the only change is that the
    //  flow field is the analytic one instead of an estimate.  So the gap
    //  between psnrEst and psnrOracle is what the matcher costs, and the gap
    //  between psnrOracle and a perfect 99 dB is what the classical
    //  warp/blend formulation costs and no amount of tuning can recover.
    // =======================================================================
    uint32_t oracleDisp = 0;
    double   oraclePsnr = 0.0, oracleLat = 0.0, oracleThr = 0.0;
    std::vector<uint8_t> genO;
    {
        std::vector<float> fab, fba;
        analyticFlowAB(W, H, fab);
        analyticFlowBA(W, H, fba);
        std::vector<uint8_t> cost((size_t)W * H * 4, 0);   // 0 = fully trusted

        ID3D12Resource* tfAB  = uploadTexFmt(g, W, H, (const uint8_t*)fab.data(),
                                             DXGI_FORMAT_R32G32_FLOAT, 8);
        ID3D12Resource* tfBA  = uploadTexFmt(g, W, H, (const uint8_t*)fba.data(),
                                             DXGI_FORMAT_R32G32_FLOAT, 8);
        ID3D12Resource* tCost = uploadTexFmt(g, W, H, cost.data(),
                                             DXGI_FORMAT_R32_FLOAT, 4);

        eng.set_flow_override(tfAB, tfBA, tCost);

        g.alloc[0]->Reset();
        g.list->Reset(g.alloc[0], nullptr);
        eng.record(g.list, 0, A, D3D12_RESOURCE_STATE_COPY_DEST,
                   B, D3D12_RESOURCE_STATE_COPY_DEST,
                   outTex, D3D12_RESOURCE_STATE_COMMON, 0.5f, false);
        oracleDisp = eng.dispatch_count();
        g.list->Close();
        g.queue->ExecuteCommandLists(1, (ID3D12CommandList**)&g.list);
        gpuFlush(g);

        genO = readBackRGBA(g, outTex, W, H, D3D12_RESOURCE_STATE_COMMON);
        oraclePsnr = psnr(genO, cpuM);

        for (int i = 0; i < 8; ++i)
        {
            g.alloc[i % 4]->Reset();
            g.list->Reset(g.alloc[i % 4], nullptr);
            eng.record(g.list, i % 3, A, D3D12_RESOURCE_STATE_COPY_DEST,
                       B, D3D12_RESOURCE_STATE_COPY_DEST,
                       outTex, D3D12_RESOURCE_STATE_COMMON, 0.5f, false);
            g.list->Close();
            g.queue->ExecuteCommandLists(1, (ID3D12CommandList**)&g.list);
        }
        gpuFlush(g);

        LARGE_INTEGER o0, o1; QueryPerformanceCounter(&o0);
        {
            g.alloc[0]->Reset();
            g.list->Reset(g.alloc[0], nullptr);
            eng.record(g.list, 0, A, D3D12_RESOURCE_STATE_COPY_DEST,
                       B, D3D12_RESOURCE_STATE_COPY_DEST,
                       outTex, D3D12_RESOURCE_STATE_COMMON, 0.5f, false);
            g.list->Close();
            g.queue->ExecuteCommandLists(1, (ID3D12CommandList**)&g.list);
            gpuFlush(g);
        }
        QueryPerformanceCounter(&o1);
        oracleLat = 1000.0 * (double)(o1.QuadPart - o0.QuadPart) / (double)g.tFreq;

        QueryPerformanceCounter(&o0);
        for (int i = 0; i < iters; ++i)
        {
            g.alloc[i % 4]->Reset();
            g.list->Reset(g.alloc[i % 4], nullptr);
            eng.record(g.list, i % 3, A, D3D12_RESOURCE_STATE_COPY_DEST,
                       B, D3D12_RESOURCE_STATE_COPY_DEST,
                       outTex, D3D12_RESOURCE_STATE_COMMON, 0.5f, false);
            g.list->Close();
            g.queue->ExecuteCommandLists(1, (ID3D12CommandList**)&g.list);
            if ((i & 3) == 3) { g.queue->Signal(g.fence, ++g.fenceVal); gpuWait(g, g.fenceVal); }
        }
        gpuFlush(g);
        QueryPerformanceCounter(&o1);
        oracleThr = 1000.0 * (double)(o1.QuadPart - o0.QuadPart) / (double)g.tFreq / iters;

        eng.set_flow_override(nullptr, nullptr, nullptr);
        tfAB->Release(); tfBA->Release(); tCost->Release();
    }

    printf("\\n---- warp/blend ceiling (analytic flow, matcher bypassed) ----\\n");
    printf("dispatches per generated frame : %u   (vs %u with the matcher)\\n",
           oracleDisp, estDisp);
    printf("PSNR(oracle vs truth)          : %6.2f dB\\n", oraclePsnr);
    printf("latency  (1 frame, idle queue) : %6.3f ms\\n", oracleLat);
    printf("throughput (sustained, n=%d)   : %6.3f ms/frame\\n", iters, oracleThr);

    printf("\\n---- %dx%d PSNR(gen vs truth) ----\\n", W, H);
    printf("  no motion, 50/50 average     : %6.2f dB   <- floor\\n", psnrMix);
    printf("  matcher + blend (this build) : %6.2f dB\\n", psnrEst);
    printf("  analytic flow + blend        : %6.2f dB   <- ceiling of this blend\\n", oraclePsnr);
    printf("  cost of the matcher          : %6.2f dB\\n", oraclePsnr - psnrEst);

    if (visual && outDir)
    {
        char p[512];
        sprintf_s(p, "%s/%dx%d_oracle.png", outDir, W, H); writeRGBPNG(p, genO, W, H);
        sprintf_s(p, "%s/%dx%d_oracle_diff.png", outDir, W, H); writeDiff(p, genO, cpuM, W, H);
        sprintf_s(p, "%s/%dx%d_mix5050.png", outDir, W, H); writeRGBPNG(p, mix, W, H);
    }

    eng.shutdown();''')

io.open(p, 'w', encoding='utf-8').write(s)
print('selftest.cpp patched')
