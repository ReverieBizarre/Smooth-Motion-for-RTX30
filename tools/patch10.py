# ---------------------------------------------------------------------------
#  Wire the seed pre-warp pass into the engine.
#
#  Each refinement level now runs as:
#     cs_seed_warp(level reference, previous-level flow)  -> wB
#     cs_bm_refine(A, wB, residual only)
#  Six extra dispatches (2 directions x 3 levels, the coarse pass needs none)
#  buy a genuinely zero-centred residual search: shared-memory hits instead of
#  the global fallback, and the seed is added back to the output rather than
#  being carried inside the tile index.
# ---------------------------------------------------------------------------
import io


def patch(path, edits):
    s = io.open(path, encoding='utf-8').read()
    for old, new in edits:
        assert s.count(old) == 1, ('anchor miss in %s' % path, old[:80], s.count(old))
        s = s.replace(old, new)
    io.open(path, 'w', encoding='utf-8').write(s)
    print('patched', path)


# ---------------- vfi.h ----------------
patch('src/vfi.h', [
    ('''    ID3D12PipelineState* psoCoarse_ = nullptr;''',
     '''    ID3D12PipelineState* psoSeedWarp_ = nullptr;
    ID3D12PipelineState* psoCoarse_ = nullptr;'''),

    ('''    ID3D12Resource* dummy_ = nullptr;''',
     '''    ID3D12Resource* seedWarp_ = nullptr;    // reference shifted by the seed; full-res alloc, per-level extents
    ID3D12Resource* dummy_ = nullptr;'''),
])

# ---------------- vfi.cpp ----------------
patch('src/vfi.cpp', [
    ('static const uint32_t kMaxDispatch = 34;',
     'static const uint32_t kMaxDispatch = 44;'),

    ('''        { &psoCoarse_, "cs_bm_coarse"  },''',
     '''        { &psoSeedWarp_, "cs_seed_warp" },
        { &psoCoarse_, "cs_bm_coarse"  },'''),

    ('''    costTmp2_ = makeTex(gw2_, gh2_, F1);''',
     '''    seedWarp_ = makeTex(W, H, F1);
    costTmp2_ = makeTex(gw2_, gh2_, F1);'''),

    ('''    if (!costTmp2_ || !costTmp1_ || !fillFlow_ || !fillCost_ || !dummy_)
        return fail("scratch alloc failed");''',
     '''    if (!seedWarp_ || !costTmp2_ || !costTmp1_ || !fillFlow_ || !fillCost_ || !dummy_)
        return fail("scratch alloc failed");'''),

    ('''    stateSet(costTmp2_, D3D12_RESOURCE_STATE_COMMON);''',
     '''    stateSet(seedWarp_, D3D12_RESOURCE_STATE_COMMON);
    stateSet(costTmp2_, D3D12_RESOURCE_STATE_COMMON);'''),

    ('''    rel(costTmp2_); rel(costTmp1_);''',
     '''    rel(seedWarp_); rel(costTmp2_); rel(costTmp1_);'''),

    ('''    relp(psoCoarse_); relp(psoRefine_);''',
     '''    relp(psoSeedWarp_); relp(psoCoarse_); relp(psoRefine_);'''),

    # ---- L2 ----
    ('''            // L2: 1/4 res, local search around the L3 field (seed unit = x2)
            pc.SrcW = wq; pc.SrcH = hq; pc.DstW = gw2_; pc.DstH = gh2_;
            pc.SearchR   = opt_.radius_l2;
            pc.PrevScale = 2.0f;
            pc.PrevInvW  = 1.0f / (float)gw2_;
            pc.PrevInvH  = 1.0f / (float)gh2_;
            go(psoRefine_, pc, { { 3, cur2 }, { 4, nxt2 }, { 5, flow_[dir][0] } },
               { { 4, flow_[dir][1] }, { 5, costTmp2_ } }, groups(gw2_), groups(gh2_));''',
     '''            // L2: 1/4 res.  The seed carries over from L3 (x2 units), so the
            // reference is shifted by its integer part first and the refinement
            // below only has to search the residual.  PrevInvW/H address the
            // *luma* of this level, which is the coordinate both passes share.
            pc.SrcW = wq; pc.SrcH = hq; pc.DstW = wq; pc.DstH = hq;
            pc.PrevScale = 2.0f;
            pc.PrevInvW  = 1.0f / (float)wq;
            pc.PrevInvH  = 1.0f / (float)hq;
            go(psoSeedWarp_, pc, { { 1, nxt2 }, { 5, flow_[dir][0] } }, { { 0, seedWarp_ } },
               groups(wq), groups(hq));

            pc.SrcW = wq; pc.SrcH = hq; pc.DstW = gw2_; pc.DstH = gh2_;
            pc.SearchR = opt_.radius_l2;
            go(psoRefine_, pc, { { 3, cur2 }, { 4, seedWarp_ }, { 5, flow_[dir][0] } },
               { { 4, flow_[dir][1] }, { 5, costTmp2_ } }, groups(gw2_), groups(gh2_));'''),

    # ---- L1 ----
    ('''            // L1: 1/2 res
            pc.SrcW = wh; pc.SrcH = hh; pc.DstW = gw1_; pc.DstH = gh1_;
            pc.SearchR   = opt_.radius_l1;
            pc.PrevScale = 2.0f;
            pc.PrevInvW  = 1.0f / (float)gw1_;
            pc.PrevInvH  = 1.0f / (float)gh1_;
            go(psoRefine_, pc, { { 3, cur1 }, { 4, nxt1 }, { 5, flow_[dir][1] } },
               { { 4, flow_[dir][2] }, { 5, costTmp1_ } }, groups(gw1_), groups(gh1_));''',
     '''            // L1: 1/2 res
            pc.SrcW = wh; pc.SrcH = hh; pc.DstW = wh; pc.DstH = hh;
            pc.PrevScale = 2.0f;
            pc.PrevInvW  = 1.0f / (float)wh;
            pc.PrevInvH  = 1.0f / (float)hh;
            go(psoSeedWarp_, pc, { { 1, nxt1 }, { 5, flow_[dir][1] } }, { { 0, seedWarp_ } },
               groups(wh), groups(hh));

            pc.SrcW = wh; pc.SrcH = hh; pc.DstW = gw1_; pc.DstH = gh1_;
            pc.SearchR = opt_.radius_l1;
            go(psoRefine_, pc, { { 3, cur1 }, { 4, seedWarp_ }, { 5, flow_[dir][1] } },
               { { 4, flow_[dir][2] }, { 5, costTmp1_ } }, groups(gw1_), groups(gh1_));'''),

    # ---- L0 ----
    ('''            // L0: full res, final sub-pixel polish.  The previous-level flow is
            // sampled with *this* level's grid UV, hence 1/gw0_ and not 1/gw1_.
            pc.SrcW = W;  pc.SrcH = H;  pc.DstW = gw0_; pc.DstH = gh0_;
            pc.SearchR   = opt_.radius_l0;
            pc.PrevScale = 2.0f;
            pc.PrevInvW  = 1.0f / (float)gw0_;
            pc.PrevInvH  = 1.0f / (float)gh0_;
            go(psoRefine_, pc, { { 3, cur }, { 4, nxt }, { 5, flow_[dir][2] } },
               { { 4, flow_[dir][3] }, { 5, costGrid_[dir] } }, groups(gw0_), groups(gh0_));''',
     '''            // L0: full res, final sub-pixel polish.  PrevInvW/H are the luma
            // reciprocals here, not the grid ones: cs_seed_warp and cs_bm_refine
            // must round the same seed at the same position or the residual
            // they search is not the residual the warp introduced.
            pc.SrcW = W;  pc.SrcH = H;  pc.DstW = W; pc.DstH = H;
            pc.PrevScale = 2.0f;
            pc.PrevInvW  = 1.0f / (float)W;
            pc.PrevInvH  = 1.0f / (float)H;
            go(psoSeedWarp_, pc, { { 1, nxt }, { 5, flow_[dir][2] } }, { { 0, seedWarp_ } },
               groups(W), groups(H));

            pc.SrcW = W;  pc.SrcH = H;  pc.DstW = gw0_; pc.DstH = gh0_;
            pc.SearchR = opt_.radius_l0;
            go(psoRefine_, pc, { { 3, cur }, { 4, seedWarp_ }, { 5, flow_[dir][2] } },
               { { 4, flow_[dir][3] }, { 5, costGrid_[dir] } }, groups(gw0_), groups(gh0_));'''),
])
