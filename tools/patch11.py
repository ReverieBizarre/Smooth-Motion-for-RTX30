# ---------------------------------------------------------------------------
#  Make the seed handling a measured choice rather than a replacement.
#
#  Two strategies, both now implemented in one shader (flagged by P0):
#    fallback (default): the seed is carried inside the tile index; candidates
#      that leave the tile fall back to a global read.  More accurate overall.
#    prewarp: cs_seed_warp removes the seed from the reference, so the residual
#      search is zero-centred and the shared tile is always usable.  Faster and
#      a much better median, but a wider error tail (the integer pre-shift
#      duplicates edge pixels over a band as wide as the shift, and that content
#      is unrecoverable).
#  The harness can run either; the default is whichever measured better.
# ---------------------------------------------------------------------------
import io


def patch(path, edits):
    s = io.open(path, encoding='utf-8').read()
    for old, new in edits:
        assert s.count(old) == 1, ('anchor miss in %s' % path, old[:80], s.count(old))
        s = s.replace(old, new)
    io.open(path, 'w', encoding='utf-8').write(s)
    print('patched', path)


# ---------------- shader: one refine, two modes ----------------
patch('src/shaders/vfi.hlsl', [
    ('''    // The seed was removed from the image, so it is only needed to report the
    // absolute displacement.  Sampled at the block's own position, in the same
    // coordinates cs_seed_warp used, so both passes agree on round(sd).
    float2 uv    = (float2(basePx) + 0.5f) * float2(PrevInvW, PrevInvH);
    float2 sdraw = tPrev.SampleLevel(sLin, uv, 0) * PrevScale;
    int2   seedI = int2(round(sdraw));

    float best  = 1e30f;
    int2  bestD = int2(0, 0);

    [loop] for (int dy = -SearchR; dy <= SearchR; ++dy)
    [loop] for (int dx = -SearchR; dx <= SearchR; ++dx)
    {
        int2 d = int2(dx, dy);''',
     '''    // Two strategies, selected by P0 (see VfiOptions::prewarp_seed):
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
        int2 d = inside + int2(dx, dy);'''),

    ('''    rFlow[dt.xy] = float2(seedI + bestD) + subpixel(basePx, bestD, best, BLK, lim);''',
     '''    rFlow[dt.xy] = float2(added + bestD) + subpixel(basePx, bestD, best, BLK, lim);'''),
])


# ---------------- vfi.h: the option ----------------
patch('src/vfi.h', [
    ('''    float w_bias     = 0.5f;   // weight bias: 0 -> A, 1 -> B''',
     '''    // How the refinement handles the seed inherited from the previous level.
    //   false: seed inside the tile index, global read when it leaves the tile
    //   true : cs_seed_warp removes the seed first, residual search centred on 0
    // Measured both ways - see the two tables in README section 3.  The default
    // is the one with the better end-to-end number, not the faster one.
    bool  prewarp_seed = false;
    float w_bias     = 0.5f;   // weight bias: 0 -> A, 1 -> B'''),
])


# ---------------- vfi.cpp: plumb it ----------------
patch('src/vfi.cpp', [
    ('''    pc.Block    = (int32_t)TG_BLOCK;''',
     '''    pc.Block    = (int32_t)TG_BLOCK;
    pc.P0       = opt_.prewarp_seed ? 1.0f : 0.0f;'''),

    # L2
    ('''            pc.SrcW = wq; pc.SrcH = hq; pc.DstW = wq; pc.DstH = hq;
            pc.PrevScale = 2.0f;
            pc.PrevInvW  = 1.0f / (float)wq;
            pc.PrevInvH  = 1.0f / (float)hq;
            go(psoSeedWarp_, pc, { { 1, nxt2 }, { 5, flow_[dir][0] } }, { { 0, seedWarp_ } },
               groups(wq), groups(hq));

            pc.SrcW = wq; pc.SrcH = hq; pc.DstW = gw2_; pc.DstH = gh2_;
            pc.SearchR = opt_.radius_l2;
            go(psoRefine_, pc, { { 3, cur2 }, { 4, seedWarp_ }, { 5, flow_[dir][0] } },
               { { 4, flow_[dir][1] }, { 5, costTmp2_ } }, groups(gw2_), groups(gh2_));''',
     '''            pc.SrcW = wq; pc.SrcH = hq;
            pc.PrevScale = 2.0f;
            pc.PrevInvW  = 1.0f / (float)wq;
            pc.PrevInvH  = 1.0f / (float)hq;
            if (opt_.prewarp_seed)
            {
                pc.DstW = wq; pc.DstH = hq;
                go(psoSeedWarp_, pc, { { 1, nxt2 }, { 5, flow_[dir][0] } }, { { 0, seedWarp_ } },
                   groups(wq), groups(hq));
            }

            pc.DstW = gw2_; pc.DstH = gh2_;
            pc.SearchR = opt_.radius_l2;
            go(psoRefine_, pc, { { 3, cur2 }, { 4, opt_.prewarp_seed ? seedWarp_ : nxt2 },
                                 { 5, flow_[dir][0] } },
               { { 4, flow_[dir][1] }, { 5, costTmp2_ } }, groups(gw2_), groups(gh2_));'''),

    # L1
    ('''            pc.SrcW = wh; pc.SrcH = hh; pc.DstW = wh; pc.DstH = hh;
            pc.PrevScale = 2.0f;
            pc.PrevInvW  = 1.0f / (float)wh;
            pc.PrevInvH  = 1.0f / (float)hh;
            go(psoSeedWarp_, pc, { { 1, nxt1 }, { 5, flow_[dir][1] } }, { { 0, seedWarp_ } },
               groups(wh), groups(hh));

            pc.SrcW = wh; pc.SrcH = hh; pc.DstW = gw1_; pc.DstH = gh1_;
            pc.SearchR = opt_.radius_l1;
            go(psoRefine_, pc, { { 3, cur1 }, { 4, seedWarp_ }, { 5, flow_[dir][1] } },
               { { 4, flow_[dir][2] }, { 5, costTmp1_ } }, groups(gw1_), groups(gh1_));''',
     '''            pc.SrcW = wh; pc.SrcH = hh;
            pc.PrevScale = 2.0f;
            pc.PrevInvW  = 1.0f / (float)wh;
            pc.PrevInvH  = 1.0f / (float)hh;
            if (opt_.prewarp_seed)
            {
                pc.DstW = wh; pc.DstH = hh;
                go(psoSeedWarp_, pc, { { 1, nxt1 }, { 5, flow_[dir][1] } }, { { 0, seedWarp_ } },
                   groups(wh), groups(hh));
            }

            pc.DstW = gw1_; pc.DstH = gh1_;
            pc.SearchR = opt_.radius_l1;
            go(psoRefine_, pc, { { 3, cur1 }, { 4, opt_.prewarp_seed ? seedWarp_ : nxt1 },
                                 { 5, flow_[dir][1] } },
               { { 4, flow_[dir][2] }, { 5, costTmp1_ } }, groups(gw1_), groups(gh1_));'''),

    # L0
    ('''            pc.SrcW = W;  pc.SrcH = H;  pc.DstW = W; pc.DstH = H;
            pc.PrevScale = 2.0f;
            pc.PrevInvW  = 1.0f / (float)W;
            pc.PrevInvH  = 1.0f / (float)H;
            go(psoSeedWarp_, pc, { { 1, nxt }, { 5, flow_[dir][2] } }, { { 0, seedWarp_ } },
               groups(W), groups(H));

            pc.SrcW = W;  pc.SrcH = H;  pc.DstW = gw0_; pc.DstH = gh0_;
            pc.SearchR = opt_.radius_l0;
            go(psoRefine_, pc, { { 3, cur }, { 4, seedWarp_ }, { 5, flow_[dir][2] } },
               { { 4, flow_[dir][3] }, { 5, costGrid_[dir] } }, groups(gw0_), groups(gh0_));''',
     '''            pc.SrcW = W;  pc.SrcH = H;
            pc.PrevScale = 2.0f;
            pc.PrevInvW  = 1.0f / (float)W;
            pc.PrevInvH  = 1.0f / (float)H;
            if (opt_.prewarp_seed)
            {
                pc.DstW = W; pc.DstH = H;
                go(psoSeedWarp_, pc, { { 1, nxt }, { 5, flow_[dir][2] } }, { { 0, seedWarp_ } },
                   groups(W), groups(H));
            }

            pc.DstW = gw0_; pc.DstH = gh0_;
            pc.SearchR = opt_.radius_l0;
            go(psoRefine_, pc, { { 3, cur }, { 4, opt_.prewarp_seed ? seedWarp_ : nxt },
                                 { 5, flow_[dir][2] } },
               { { 4, flow_[dir][3] }, { 5, costGrid_[dir] } }, groups(gw0_), groups(gh0_));'''),
])


# ---------------- selftest: --prewarp switch ----------------
patch('tools/selftest.cpp', [
    ('''    VfiOptions opt;

    if (runSize(g, opt, shader, 960, 540, true, outDir, 120)) return 1;''',
     '''    VfiOptions opt;
    for (int i = 1; i < argc; ++i)
        if (strcmp(argv[i], "--prewarp") == 0) opt.prewarp_seed = true;
    printf("seed handling : %s\\n", opt.prewarp_seed
           ? "cs_seed_warp pre-shift (residual centred on 0)"
           : "seed inside tile index, global read on miss");

    if (runSize(g, opt, shader, 960, 540, true, outDir, 120)) return 1;'''),
])
