// ============================================================================
//  sm86_smooth - VFI compute shaders  (target: cs_5_0, runs on SM86 / RTX 3080)
// ----------------------------------------------------------------------------
//  Pipeline (per generated frame):
//    cs_luma        x2   RGBA8 backbuffer -> R32F luma
//    cs_down2l      x4   luma pyramid 1/1 -> 1/2 -> 1/4
//    cs_down2c      x2   RGBA -> 1/2 res colour (for the high-frequency split)
//    cs_gauss3c     x2   3x3 gaussian on 1/2 res colour -> smooth base
//    cs_bm_coarse   x2   1/4 res, full search, 4x4 blocks
//    cs_bm_refine   x4   1/2 and 1/1 res, groupshared local search -> flow+cost
//    cs_median3f    x2   3x3 median on the finest grid flow
//    cs_up_f2       x2   bilinear upsample of flow to full res
//    cs_up_f1       x2   bilinear upsample of confidence
//    cs_warp_blend  x1   bi-directional warp + occlusion mask + HF transfer
//
//  Flow convention: fAB(p) is the displacement of the pixel AT p such that
//    B(p + fAB(p)) ~= A(p)
//  Interpolation at time t (0 = A, 1 = B):
//    A is sampled at  p - t*fAB(p)
//    B is sampled at  p - (1-t)*fBA(p)
//  Extrapolation past B is the same expression with t > 1 (caller supplies t).
//
//  Register layout is fixed and unique per resource so that the runtime can
//  bind one flat descriptor table for every dispatch (see vfi.cpp).
//    t0  tColor   t1  tS1      t2  tS4      t3  tCur    t4  tNxt
//    t5  tPrev    t6  tF       t7  tF1      t8  tA      t9  tB
//    t10 tBAse    t11 tBBse    t12 tFAB     t13 tFBA    t14 tCst
//    u0  rLuma    u1  rD1      u2  rD4      u3  rG      u4  rFlow   u5 rCost  u6 uOut
// ============================================================================

#define TG   16
#define BLK   4
#define RMAX  4
#define TILE (TG*BLK + 2*RMAX + BLK)      // 76, compile-time constant

#define FLAG_TAA   1u
#define FLAG_BGRA  2u

cbuffer Params : register(b0)
{
    uint  SrcW, SrcH;      // source dims (luma / colour at the current level)
    uint  DstW, DstH;      // destination dims (grid dims / dst texture dims)
    uint  OutW, OutH;      // final output dims
    int   SearchR;         // local search radius, level px
    int   Block;           // block size, level px (must be BLK)
    float T;               // interpolation time
    float FbThresh;        // forward/backward consistency threshold, px
    float HfAmount;        // high-frequency transfer gain
    float WBias;           // 0 -> prefer A, 1 -> prefer B
    uint  Flags;           // bit0 temporal_aa, bit1 bgra
    float PrevScale;       // multiplier: prev-level flow -> this level px
    float PrevInvW, PrevInvH;   // 1 / prev grid dims
    float UpScale;         // multiplier applied while upsampling flow
    float P0, P1, P2;
};

SamplerState sLin : register(s0);

// ---------------------------------------------------------------------------
// 0. RGBA -> luma.  Rec.709 weights on the *encoded* values: block matching
//    wants to respond to the same edges the eye sees.
// ---------------------------------------------------------------------------
Texture2D<float4>   tColor : register(t0);
RWTexture2D<float>  rLuma  : register(u0);

[numthreads(TG, TG, 1)]
void cs_luma(uint3 dt : SV_DispatchThreadID)
{
    if (dt.x >= DstW || dt.y >= DstH) return;
    float3 c = tColor.Load(int3(dt.xy, 0)).rgb;
    rLuma[dt.xy] = dot(c, float3(0.2126, 0.7152, 0.0722));
}

// ---------------------------------------------------------------------------
// 1. 2x2 box downsample, single channel
// ---------------------------------------------------------------------------
Texture2D<float>  tS1 : register(t1);
RWTexture2D<float> rD1 : register(u1);

[numthreads(TG, TG, 1)]
void cs_down2l(uint3 dt : SV_DispatchThreadID)
{
    if (dt.x >= DstW || dt.y >= DstH) return;
    int2 lim = int2(SrcW - 1, SrcH - 1);
    int2 s   = min(int2(dt.xy) * 2, lim);
    float a = tS1.Load(int3(s, 0));
    float b = tS1.Load(int3(min(s + int2(1, 0), lim), 0));
    float c = tS1.Load(int3(min(s + int2(0, 1), lim), 0));
    float d = tS1.Load(int3(min(s + int2(1, 1), lim), 0));
    rD1[dt.xy] = (a + b + c + d) * 0.25;
}

// ---------------------------------------------------------------------------
// 2. 2x2 box downsample, RGBA -> half res colour
// ---------------------------------------------------------------------------
Texture2D<float4>    tS4 : register(t2);
RWTexture2D<float4>  rD4 : register(u2);

[numthreads(TG, TG, 1)]
void cs_down2c(uint3 dt : SV_DispatchThreadID)
{
    if (dt.x >= DstW || dt.y >= DstH) return;
    int2 lim = int2(SrcW - 1, SrcH - 1);
    int2 s   = min(int2(dt.xy) * 2, lim);
    float4 a = tS4.Load(int3(s, 0));
    float4 b = tS4.Load(int3(min(s + int2(1, 0), lim), 0));
    float4 c = tS4.Load(int3(min(s + int2(0, 1), lim), 0));
    float4 d = tS4.Load(int3(min(s + int2(1, 1), lim), 0));
    rD4[dt.xy] = (a + b + c + d) * 0.25;
}

// ---------------------------------------------------------------------------
// 3. separable 3x3 gaussian on the half-res colour -> the "base" signal.
//    colour - base = detail, which we re-attach after warping.
// ---------------------------------------------------------------------------
RWTexture2D<float4> rG : register(u3);

[numthreads(TG, TG, 1)]
void cs_gauss3c(uint3 dt : SV_DispatchThreadID)
{
    if (dt.x >= DstW || dt.y >= DstH) return;
    int2 lim = int2(SrcW - 1, SrcH - 1);
    const float k[3] = { 0.25, 0.5, 0.25 };
    float4 acc = 0;
    [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x)
        acc += tS4.Load(int3(clamp(int2(dt.xy) + int2(x, y), int2(0, 0), lim), 0))
               * (k[x + 1] * k[y + 1]);
    rG[dt.xy] = acc;
}

// ---------------------------------------------------------------------------
// 4. Block SAD.  cur == A, nxt == B.  sum |A(p) - B(p + d)|
// ---------------------------------------------------------------------------
Texture2D<float>  tCur : register(t3);
Texture2D<float>  tNxt : register(t4);
RWTexture2D<float2> rFlow : register(u4);

float blockSAD(int2 basePx, int2 d, int block, int2 lim)
{
    float s = 0;
    [loop] for (int y = 0; y < block; ++y)
    [loop] for (int x = 0; x < block; ++x)
    {
        int2 off = int2(x, y);
        float a = tCur.Load(int3(clamp(basePx + off,     int2(0, 0), lim), 0));
        float b = tNxt.Load(int3(clamp(basePx + off + d, int2(0, 0), lim), 0));
        s += abs(a - b);
    }
    return s;
}

// 1-D parabola fit on the SAD surface -> quarter-pixel offset
float2 subpixel(int2 basePx, int2 best, float sadC, int block, int2 lim)
{
    float sxm = blockSAD(basePx, best + int2(-1, 0), block, lim);
    float sxp = blockSAD(basePx, best + int2( 1, 0), block, lim);
    float sym = blockSAD(basePx, best + int2( 0,-1), block, lim);
    float syp = blockSAD(basePx, best + int2( 0, 1), block, lim);

    // Vertex of the parabola through (-1, sxm), (0, sadC), (+1, sxp):
    //   x* = (S(-1) - S(+1)) / (2 * (S(-1) + S(+1) - 2*S(0)))
    // S(x) = a*(x - d*)^2 + c gives S(-1) - S(+1) = 4*a*d*, so this reduces to
    // d*, with the 0.5*(sxm - sxp) / dxx form below being exactly that.
    //
    // The difference matters: writing (sxp - sxm) instead yields -d*, so every
    // level moves *away* from the sub-pixel optimum by twice the true offset.
    // At the 1/8 level one pixel is eight full-res pixels, so the resulting
    // error is several pixels wide and looks exactly like a bad matcher.
    float2 sub = float2(0, 0);
    float dx = 0.5f * (sxm - sxp);
    float dxx = (sxp + sxm) - 2.0f * sadC;
    if (abs(dxx) > 1e-4f) sub.x = clamp(dx / dxx, -0.5f, 0.5f);
    float dy = 0.5f * (sym - syp);
    float dyy = (syp + sym) - 2.0f * sadC;
    if (abs(dyy) > 1e-4f) sub.y = clamp(dy / dyy, -0.5f, 0.5f);
    return sub;
}

// ---------------------------------------------------------------------------
// 5. Coarse full search at 1/4 resolution.  No groupshared: the grid is only
//    W/16 x H/16 and the whole luma plane stays resident in L2.
// ---------------------------------------------------------------------------
[numthreads(TG, TG, 1)]
void cs_bm_coarse(uint3 dt : SV_DispatchThreadID)
{
    if (dt.x >= DstW || dt.y >= DstH) return;
    int2 lim    = int2(SrcW - 1, SrcH - 1);
    int2 basePx = int2(dt.xy) * Block;

    float best  = 1e30f;
    int2  bestD = int2(0, 0);
    [loop] for (int dy = -SearchR; dy <= SearchR; ++dy)
    [loop] for (int dx = -SearchR; dx <= SearchR; ++dx)
    {
        float s = blockSAD(basePx, int2(dx, dy), Block, lim);
        if (s < best) { best = s; bestD = int2(dx, dy); }
    }
    rFlow[dt.xy] = float2(bestD) + subpixel(basePx, bestD, best, Block, lim);
}

// Declared here because both cs_seed_warp (which reads the previous level's
// flow) and cs_bm_refine (which re-reads it to report the absolute vector) use
// the same register.
Texture2D<float2>  tPrev : register(t5);

// ---------------------------------------------------------------------------
// 6a. Seed pre-warp.
//
//     The refinement below searches a residual around the flow inherited from
//     the previous pyramid level, and the groupshared tile is only large enough
//     for a *zero-centred* window.  So the seed has to be taken out of the
//     image first: this pass translates the reference frame by the integer part
//     of the seed, which makes the residual search genuinely zero centred and
//     lets the tile be used for what it is good at.
//
//     The integer part is deliberate.  Warping bilinearly by the full seed
//     would preserve the fraction but blur exactly the high-frequency detail
//     the matcher needs in order to lock on - which is the failure mode this
//     whole file exists to avoid.  Whole-pixel shifts keep every sample exact,
//     and the sub-pixel part of the seed is recovered anyway by the parabola
//     fit on the residual, which runs against these same exact samples.
//
//     The shift is quantised to the *block* grid, not evaluated per pixel.
//     cs_bm_refine reports one vector per block, so it has to add back exactly
//     the shift that was applied to that block; if the warp varied inside the
//     4x4 block the SAD would be comparing content that is a pixel out of step
//     with itself, which shows up as a noisier surface and worse convergence
//     than not pre-warping at all.
// ---------------------------------------------------------------------------
[numthreads(TG, TG, 1)]
void cs_seed_warp(uint3 dt : SV_DispatchThreadID)
{
    if (dt.x >= DstW || dt.y >= DstH) return;
    int2   bp = (int2(dt.xy) / Block) * Block;      // same origin cs_bm_refine uses
    float2 uv = (float2(bp) + 0.5f) * float2(PrevInvW, PrevInvH);
    float2 sd = tPrev.SampleLevel(sLin, uv, 0) * PrevScale;
    int2   p  = clamp(int2(dt.xy) + int2(round(sd)), int2(0, 0), int2(SrcW - 1, SrcH - 1));
    rLuma[dt.xy] = tS1.Load(int3(p, 0));
}

// ---------------------------------------------------------------------------
// 6. Local refinement with a groupshared tile of the (pre-warped) reference.
//    A thread group covers TG*BLK = 64 level pixels per axis; the tile adds
//    RMAX of halo plus one block so every candidate read lands in shared
//    memory.  This is what makes the 1/1 level affordable on Ampere: global
//    traffic drops by ~20x versus re-reading the reference from VRAM per
//    candidate.
//
//    The tile is sized for |candidate| <= RMAX, which is now true by
//    construction: cs_seed_warp removed the seed, so the candidate offset here
//    is just dx/dy in [-SearchR, SearchR].  The seed is added back to the
//    output instead of being carried inside the tile index.
// ---------------------------------------------------------------------------
RWTexture2D<float> rCost : register(u5);

groupshared float shN[TILE * TILE];

[numthreads(TG, TG, 1)]
void cs_bm_refine(uint3 gt : SV_GroupThreadID, uint3 gid : SV_GroupID,
                  uint3 dt : SV_DispatchThreadID)
{
    int2 lim    = int2(SrcW - 1, SrcH - 1);
    int2 g0     = int2(gid.xy) * (TG * BLK);
    int2 origin = g0 - RMAX;

    for (uint i = gt.y * TG + gt.x; i < TILE * TILE; i += TG * TG)
    {
        int2 o = int2(int(i % TILE), int(i / TILE));
        int2 p = clamp(origin + o, int2(0, 0), lim);
        shN[o.y * TILE + o.x] = tNxt.Load(int3(p, 0));
    }
    GroupMemoryBarrierWithGroupSync();

    if (dt.x >= DstW || dt.y >= DstH) return;

    int2 basePx = int2(dt.xy) * Block;

    // Two strategies, selected by P0 (see VfiOptions::prewarp_seed):
    //
    //   P0 == 0  the reference is unwarped, so the seed has to travel inside the
    //            tile index.  |candidate| is then seed +- SearchR, which leaves
    //            the tile at the fine levels, and those reads fall back to
    //            global memory.
    //   P0 == 1  cs_seed_warp already removed round(seed) from the reference, so
    //            the candidate is just +- SearchR and the tile always suffices;
    //            the removed part is added back to the output instead.
    //
    // Sampled at the block's own position and in the same coordinate space
    // cs_seed_warp uses, so in mode 1 both passes agree on round(seed) exactly.
    float2 uv    = (float2(basePx) + 0.5f) * float2(PrevInvW, PrevInvH);
    float2 sdraw = tPrev.SampleLevel(sLin, uv, 0) * PrevScale;

    const bool prewarp = (P0 > 0.5f);
    int2 inside = prewarp ? int2(0, 0)              : int2(round(sdraw));
    int2 added  = prewarp ? int2(round(sdraw))      : int2(0, 0);

    float best  = 1e30f;
    int2  bestD = int2(0, 0);

    [loop] for (int dy = -SearchR; dy <= SearchR; ++dy)
    [loop] for (int dx = -SearchR; dx <= SearchR; ++dx)
    {
        int2 d = inside + int2(dx, dy);
        float s = 0;
        [loop] for (int y = 0; y < BLK; ++y)
        [loop] for (int x = 0; x < BLK; ++x)
        {
            int2 off = int2(x, y);
            float a = tCur.Load(int3(clamp(basePx + off, int2(0, 0), lim), 0));
            int2 o  = basePx + off + d - origin;
            float b = (all(o >= int2(0, 0)) && all(o < int2(TILE, TILE)))
                        ? shN[o.y * TILE + o.x]
                        : tNxt.Load(int3(clamp(basePx + off + d, int2(0, 0), lim), 0));
            s += abs(a - b);
        }
        if (s < best) { best = s; bestD = d; }
    }

    rFlow[dt.xy] = float2(added + bestD) + subpixel(basePx, bestD, best, BLK, lim);

    // Confidence has two independent failure modes and both must be reported:
    //   * the block matched badly          -> SAD is large
    //   * the block has no texture at all  -> SAD is *tiny* but meaningless
    // The second one is the killer: a flat-shaded object has a near-zero SAD
    // surface with no gradient, so the matcher confidently returns a ~zero
    // vector and the object smears instead of moving.  Guard it with the
    // block's luma range.
    float vmin = 1e9f, vmax = -1e9f;
    [loop] for (int y = 0; y < BLK; ++y)
    [loop] for (int x = 0; x < BLK; ++x)
    {
        float a = tCur.Load(int3(clamp(basePx + int2(x, y), int2(0, 0), lim), 0));
        vmin = min(vmin, a);
        vmax = max(vmax, a);
    }
    float flat  = 1.0f - saturate((vmax - vmin) / 0.08f);
    float sadc  = saturate(best / (float)(BLK * BLK * 255));
    rCost[dt.xy] = max(sadc, flat * 0.90f);
}

// ---------------------------------------------------------------------------
// 7b. Flow hole filling.
//     Push the surrounding confident flow into the unreliable cells: each pass
//     takes the lowest-cost of 8 taps at a given stride and relaxes the cost a
//     little so the fill can creep further out on the next pass.  Two passes at
//     stride 6 and 12 cover ~72 px at the 1/4-res grid, which is enough for a
//     character silhouette or a HUD panel.
//     SearchR carries the stride for this shader.
// ---------------------------------------------------------------------------
#define FILL_THRESH 0.35f

Texture2D<float2> tF : register(t6);   // flow (also used by the median pass)
Texture2D<float>  tC : register(t7);   // confidence, same grid as tF

[numthreads(TG, TG, 1)]
void cs_flow_fill(uint3 dt : SV_DispatchThreadID)
{
    if (dt.x >= DstW || dt.y >= DstH) return;
    int2 lim = int2(SrcW - 1, SrcH - 1);

    float  c0 = tC.Load(int3(dt.xy, 0));
    float2 f0 = tF.Load(int3(dt.xy, 0));
    if (c0 <= FILL_THRESH) { rFlow[dt.xy] = f0; rCost[dt.xy] = c0; return; }

    const int S = SearchR;
    const int2 T[8] = { int2( S, 0), int2(-S, 0), int2(0,  S), int2(0, -S),
                        int2( S, S), int2(-S,-S), int2( S,-S), int2(-S,  S) };
    float  bestC = c0;
    float2 bestF = f0;
    [unroll] for (int k = 0; k < 8; ++k)
    {
        int2 p = clamp(dt.xy + T[k], int2(0, 0), lim);
        float c = tC.Load(int3(p, 0));
        if (c < bestC) { bestC = c; bestF = tF.Load(int3(p, 0)); }
    }
    rFlow[dt.xy] = bestF;
    rCost[dt.xy] = min(bestC + 0.002f, 1.0f);
}

// ---------------------------------------------------------------------------
// 7. 3x3 median on the flow field - kills isolated bad vectors cheaply.
// ---------------------------------------------------------------------------
[numthreads(TG, TG, 1)]
void cs_median3f(uint3 dt : SV_DispatchThreadID)
{
    if (dt.x >= DstW || dt.y >= DstH) return;
    int2 lim = int2(SrcW - 1, SrcH - 1);

    float2 v[9];
    [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x)
        v[(y + 1) * 3 + (x + 1)] =
            tF.Load(int3(clamp(int2(dt.xy) + int2(x, y), int2(0, 0), lim), 0));

    float mx[9], my[9];
    [unroll] for (int i = 0; i < 9; ++i) { mx[i] = v[i].x; my[i] = v[i].y; }
    [unroll] for (int i = 1; i < 9; ++i)
    {
        float kx = mx[i], ky = my[i];
        int j = i - 1;
        [loop] while (j >= 0 && mx[j] > kx) { mx[j + 1] = mx[j]; my[j + 1] = my[j]; --j; }
        mx[j + 1] = kx; my[j + 1] = ky;
    }
    rFlow[dt.xy] = float2(mx[4], my[4]);
}

// ---------------------------------------------------------------------------
// 8. bilinear upsampling
// ---------------------------------------------------------------------------
[numthreads(TG, TG, 1)]
void cs_up_f2(uint3 dt : SV_DispatchThreadID)
{
    if (dt.x >= DstW || dt.y >= DstH) return;
    float2 uv = (float2(dt.xy) + 0.5f) / float2(DstW, DstH);
    rFlow[dt.xy] = tF.SampleLevel(sLin, uv, 0) * UpScale;
}

Texture2D<float> tF1 : register(t7);

[numthreads(TG, TG, 1)]
void cs_up_f1(uint3 dt : SV_DispatchThreadID)
{
    if (dt.x >= DstW || dt.y >= DstH) return;
    float2 uv = (float2(dt.xy) + 0.5f) / float2(DstW, DstH);
    rCost[dt.xy] = tF1.SampleLevel(sLin, uv, 0);
}

// ---------------------------------------------------------------------------
// 9. Frame synthesis.
//
//    base + detail recombination: A is sampled twice at the same warped
//    coordinate, once as full colour and once as the blurred base.  The base
//    is low-pass, so warping it with a block-accurate flow field is accurate;
//    the difference is detail, which we add back.  Net effect: moving edges
//    stay crisp instead of getting the usual bilinear-flow smear.
//
//    occlusion via forward/backward consistency measured at BOTH ends:
//      errA = |fAB(p) + fBA(p + fAB(p))|          -- is the A->B match trusted?
//      errB = |fBA(p) + fAB(p - (1-t)*fBA(p))|    -- is the B->A match trusted?
//    w -> 0 picks A, w -> 1 picks B.  Where both are distrusted we fall back
//    to the caller bias, which is what stops the "ghost hand" artefact on
//    disocclusion.
// ---------------------------------------------------------------------------
Texture2D<float4> tA    : register(t8);
Texture2D<float4> tB    : register(t9);
Texture2D<float4> tBAse : register(t10);
Texture2D<float4> tBBse : register(t11);
Texture2D<float2> tFAB  : register(t12);
Texture2D<float2> tFBA  : register(t13);
Texture2D<float>  tCst  : register(t14);
RWTexture2D<float4> uOut : register(u6);

float2 fetchFlow(Texture2D<float2> t, float2 p, int2 lim)
{
    return t.Load(int3(clamp(int2(p + 0.5f), int2(0, 0), lim), 0));
}

[numthreads(TG, TG, 1)]
void cs_warp_blend(uint3 dt : SV_DispatchThreadID)
{
    if (dt.x >= OutW || dt.y >= OutH) return;

    int2   lim  = int2(OutW - 1, OutH - 1);
    int2   p    = int2(dt.xy);
    float2 fab  = tFAB.Load(int3(p, 0));
    float2 fba  = tFBA.Load(int3(p, 0));
    float  conf = saturate(1.0f - tCst.Load(int3(p, 0)) * 3.0f);

    float t = T;

    // ---- forward/backward consistency ----
    float2 fba_q = fetchFlow(tFBA, float2(p) + fab, lim);
    float  errA  = length(fab + fba_q);

    float2 fab_r = fetchFlow(tFAB, float2(p) - (1.0f - t) * fba, lim);
    float  errB  = length(fba + fab_r);

    float aA = saturate(errA / max(FbThresh, 1e-3f));
    float aB = saturate(errB / max(FbThresh, 1e-3f));
    float wA = (1.0f - aA) * conf;
    float wB = (1.0f - aB) * conf;
    float w  = (wA + wB > 1e-4f) ? (wB / (wA + wB)) : WBias;
    w = lerp(WBias, w, conf);

    // ---- warped sampling ----
    float2 sz  = float2(OutW, OutH);
    float2 uvA = (float2(p) + 0.5f - t * fab) / sz;
    float2 uvB = (float2(p) + 0.5f - (1.0f - t) * fba) / sz;

    float4 ca = tA.SampleLevel(sLin, uvA, 0);
    float4 cb = tB.SampleLevel(sLin, uvB, 0);

    float4 baseA = tBAse.SampleLevel(sLin, uvA, 0);
    float4 baseB = tBBse.SampleLevel(sLin, uvB, 0);
    float4 hfA   = ca - baseA;
    float4 hfB   = cb - baseB;

    float4 base = lerp(baseA, baseB, w);
    float4 hf   = lerp(hfA,   hfB,   w);
    float4 col  = base + hf * HfAmount;

    // ---- static regions get a free 2-frame temporal average (noise down).
    //      Guarded by flow magnitude so it can never smear real motion. ----
    if (Flags & FLAG_TAA)
    {
        float m      = max(length(fab), length(fba));
        float freeze = 1.0f - saturate(m / 1.0f);
        float4 avg   = lerp(ca, cb, saturate(t));
        col = lerp(col, avg, freeze * 0.5f);
    }

    col.a = 1.0f;
    if (Flags & FLAG_BGRA)
        uOut[dt.xy] = float4(col.b, col.g, col.r, 1.0f);
    else
        uOut[dt.xy] = col;
}
