import io

p = 'src/vfi.cpp'
s = io.open(p, encoding='utf-8').read()
subs = []

subs.append(('static const uint32_t kMaxDispatch = 30;',
             'static const uint32_t kMaxDispatch = 34;'))

subs.append(('''    costTmp2_ = makeTex(gw2_, gh2_, F1);
    costTmp1_ = makeTex(gw1_, gh1_, F1);
    dummy_    = makeTex(1, 1, F4);
    if (!costTmp2_ || !costTmp1_ || !dummy_) return fail("scratch alloc failed");''',
             '''    costTmp2_ = makeTex(gw2_, gh2_, F1);
    costTmp1_ = makeTex(gw1_, gh1_, F1);
    fillFlow_ = makeTex(gw0_, gh0_, F2);
    fillCost_ = makeTex(gw0_, gh0_, F1);
    dummy_    = makeTex(1, 1, F4);
    if (!costTmp2_ || !costTmp1_ || !fillFlow_ || !fillCost_ || !dummy_)
        return fail("scratch alloc failed");'''))

subs.append(('''    stateSet(costTmp2_, D3D12_RESOURCE_STATE_COMMON);
    stateSet(costTmp1_, D3D12_RESOURCE_STATE_COMMON);
    stateSet(dummy_, D3D12_RESOURCE_STATE_COMMON);''',
             '''    stateSet(costTmp2_, D3D12_RESOURCE_STATE_COMMON);
    stateSet(costTmp1_, D3D12_RESOURCE_STATE_COMMON);
    stateSet(fillFlow_, D3D12_RESOURCE_STATE_COMMON);
    stateSet(fillCost_, D3D12_RESOURCE_STATE_COMMON);
    stateSet(dummy_, D3D12_RESOURCE_STATE_COMMON);'''))

subs.append(('    rel(costTmp2_); rel(costTmp1_); rel(dummy_);',
             '    rel(costTmp2_); rel(costTmp1_); rel(fillFlow_); rel(fillCost_); rel(dummy_);'))

subs.append(('        { &psoMedian_, "cs_median3f"   },',
             '''        { &psoMedian_, "cs_median3f"   },
        { &psoFill_,   "cs_flow_fill"  },'''))

subs.append(('    relp(psoCoarse_); relp(psoRefine_); relp(psoMedian_);',
             '    relp(psoCoarse_); relp(psoRefine_); relp(psoMedian_); relp(psoFill_);'))

subs.append(('''        // upsample flow to full res (values already in full-res pixels)
        pc.SrcW = gw0_; pc.SrcH = gh0_; pc.DstW = W; pc.DstH = H;
        pc.UpScale = 1.0f;''',
             '''        // hole fill: two passes, stride 6 then 12 -> ~72 px reach at 1/4 res
        pc.SrcW = gw0_; pc.SrcH = gh0_; pc.DstW = gw0_; pc.DstH = gh0_;
        pc.SearchR = 6;
        go(psoFill_, pc, { { 6, flowTmp_[dir] }, { 7, costGrid_[dir] } },
           { { 4, fillFlow_ }, { 5, fillCost_ } }, groups(gw0_), groups(gh0_));
        pc.SearchR = 12;
        go(psoFill_, pc, { { 6, fillFlow_ }, { 7, fillCost_ } },
           { { 4, flowTmp_[dir] }, { 5, costGrid_[dir] } }, groups(gw0_), groups(gh0_));

        // upsample flow to full res (values already in full-res pixels)
        pc.SrcW = gw0_; pc.SrcH = gh0_; pc.DstW = W; pc.DstH = H;
        pc.UpScale = 1.0f;'''))

for old, new in subs:
    n = s.count(old)
    if n != 1:
        raise SystemExit('pattern count %d for: %r' % (n, old[:60]))
    s = s.replace(old, new)

io.open(p, 'w', encoding='utf-8').write(s)
print('patched ok')
