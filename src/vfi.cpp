// ============================================================================
//  sm86_smooth - VfiEngine implementation
// ============================================================================
#include "vfi.h"

#include <d3dcompiler.h>
#include <cstdio>
#include <cstring>
#include <vector>

#pragma comment(lib, "d3dcompiler.lib")

namespace sm86 {

// ---------------------------------------------------------------------------
// descriptor heap layout
//   per in-flight slot: kMaxDispatch dispatches, each owning
//     kSrvPerDisp SRV descriptors starting at t0, then kUavPerDisp at u0
//   Slots are addressed by *register number*, not sequentially: the shaders
//   declare distinct registers per resource (see the table in vfi.hlsl).
// ---------------------------------------------------------------------------
static const uint32_t kMaxDispatch = 44;
static const uint32_t kSrvPerDisp  = 16;   // t0..t14 used, t15 pad
static const uint32_t kUavPerDisp  = 8;    // u0..u6  used, u7  pad
static const uint32_t kDescPerDisp = kSrvPerDisp + kUavPerDisp;
static const uint32_t kMaxSlots    = 3;
static const uint32_t kTotalDesc   = kMaxSlots * kMaxDispatch * kDescPerDisp;

static const uint32_t kSrvCount = 15;   // root table: t0 .. t14
static const uint32_t kUavCount = 7;    // root table: u0 .. u6

static inline uint32_t ceildiv(uint32_t a, uint32_t b) { return (a + b - 1) / b; }

static void barrier(ID3D12GraphicsCommandList* cl, ID3D12Resource* r,
                    D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
{
    if (!r || a == b) return;
    D3D12_RESOURCE_BARRIER bb = {};
    bb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bb.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    bb.Transition.pResource   = r;
    bb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    bb.Transition.StateBefore = a;
    bb.Transition.StateAfter  = b;
    cl->ResourceBarrier(1, &bb);
}

// ---------------------------------------------------------------------------
// shader compilation
// ---------------------------------------------------------------------------
static ID3D12PipelineState* makePso(ID3D12Device* dev, ID3D12RootSignature* root,
                                    const wchar_t* path, const char* entry,
                                    std::string* err)
{
    UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_ENABLE_STRICTNESS;
    ID3DBlob* blob  = nullptr;
    ID3DBlob* eblob = nullptr;

    HRESULT hr = D3DCompileFromFile(path, nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                                    entry, "cs_5_0", flags, 0, &blob, &eblob);
    if (FAILED(hr))
    {
        if (err)
        {
            *err = "compile failed for entry '";
            *err += entry;
            *err += "': ";
            if (eblob) *err += (const char*)eblob->GetBufferPointer();
        }
        if (eblob) eblob->Release();
        if (blob)  blob->Release();
        return nullptr;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC cd = {};
    cd.pRootSignature = root;
    cd.CS.pShaderBytecode = blob->GetBufferPointer();
    cd.CS.BytecodeLength  = blob->GetBufferSize();

    ID3D12PipelineState* pso = nullptr;
    hr = dev->CreateComputePipelineState(&cd, IID_PPV_ARGS(&pso));
    if (FAILED(hr) && err)
    {
        *err = "CreateComputePipelineState failed for '";
        *err += entry;
        *err += "'";
    }

    blob->Release();
    if (eblob) eblob->Release();
    return pso;
}

// ---------------------------------------------------------------------------
// persistent resource-state bookkeeping
// ---------------------------------------------------------------------------
D3D12_RESOURCE_STATES VfiEngine::stateGet(ID3D12Resource* r) const
{
    for (const auto& p : states_)
        if (p.first == r) return p.second;
    return D3D12_RESOURCE_STATE_COMMON;
}

void VfiEngine::stateSet(ID3D12Resource* r, D3D12_RESOURCE_STATES s)
{
    for (auto& p : states_)
        if (p.first == r) { p.second = s; return; }
    states_.push_back(std::make_pair(r, s));
}

void VfiEngine::stateReset() { states_.clear(); }

// ---------------------------------------------------------------------------
VfiEngine::~VfiEngine()
{
    shutdown();
}

ID3D12Resource* VfiEngine::makeTex(uint32_t w, uint32_t h, DXGI_FORMAT fmt)
{
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Alignment        = 0;
    rd.Width            = w ? w : 1;
    rd.Height           = h ? h : 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels        = 1;
    rd.Format           = fmt;
    rd.SampleDesc.Count = 1;
    rd.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    ID3D12Resource* r = nullptr;
    if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                             D3D12_RESOURCE_STATE_COMMON, nullptr,
                                             IID_PPV_ARGS(&r))))
        return nullptr;
    return r;
}

bool VfiEngine::init(ID3D12Device* dev, DXGI_FORMAT fmt,
                     uint32_t width, uint32_t height,
                     const VfiOptions& opt, const wchar_t* shader_path,
                     std::string* err)
{
    shutdown();
    dev_ = dev;
    w_ = width; h_ = height; fmt_ = fmt; opt_ = opt;
    err_.clear();

    auto fail = [&](const char* what) -> bool {
        err_ = what ? what : "unknown init failure";
        if (err) *err = err_;
        shutdown();
        return false;
    };

    if (!dev_ || !shader_path || !w_ || !h_) return fail("bad init arguments");
    if (opt_.radius_l2 > 4 || opt_.radius_l1 > 4 || opt_.radius_l0 > 4)
        return fail("refine radius must be <= 4 (groupshared tile is sized for RMAX=4)");

    // Auto-tune the widest search to the frame size.  A fixed pixel radius is
    // wrong in both directions: too small and fast motion clips at the window
    // edge (which does not just miss the motion - it returns a confidently
    // wrong vector), too large and it burns time for nothing on small frames.
    // 0.8% of width per frame is comfortably above what a 60 fps camera pan
    // produces and still under what the 1/8-res grid can cover cheaply.
    if (opt_.radius_l3 <= 0)
    {
        int r = (int)((float)width * 0.008f + 0.5f);
        if (r < 2)  r = 2;
        if (r > 32) r = 32;
        opt_.radius_l3 = r;
    }
    if (opt_.radius_l3 > 32) return fail("radius_l3 out of range (0..32)");

    // ---- root signature ----
    D3D12_ROOT_PARAMETER params[3] = {};

    params[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;
    params[0].Constants.RegisterSpace  = 0;
    params[0].Constants.Num32BitValues = (UINT)(sizeof(VfiParams) / 4);
    params[0].ShaderVisibility         = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_DESCRIPTOR_RANGE srvRange = {};
    srvRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors                    = kSrvCount;
    srvRange.BaseShaderRegister                = 0;
    srvRange.RegisterSpace                     = 0;
    srvRange.OffsetInDescriptorsFromTableStart = 0;

    params[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges   = &srvRange;
    params[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_DESCRIPTOR_RANGE uavRange = {};
    uavRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors                    = kUavCount;
    uavRange.BaseShaderRegister                = 0;
    uavRange.RegisterSpace                     = 0;
    uavRange.OffsetInDescriptorsFromTableStart = 0;

    params[2].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable.NumDescriptorRanges = 1;
    params[2].DescriptorTable.pDescriptorRanges   = &uavRange;
    params[2].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_STATIC_SAMPLER_DESC samp = {};
    samp.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.ComparisonFunc   = D3D12_COMPARISON_FUNC_ALWAYS;
    samp.MaxLOD           = D3D12_FLOAT32_MAX;
    samp.ShaderRegister   = 0;
    samp.RegisterSpace    = 0;
    samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rsd = {};
    rsd.NumParameters     = 3;
    rsd.pParameters       = params;
    rsd.NumStaticSamplers = 1;
    rsd.pStaticSamplers   = &samp;
    rsd.Flags             = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ID3DBlob* sig  = nullptr;
    ID3DBlob* serr = nullptr;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1,
                                           &sig, &serr)))
    {
        std::string m = "D3D12SerializeRootSignature failed";
        if (serr) m += (const char*)serr->GetBufferPointer();
        if (serr) serr->Release();
        if (sig)  sig->Release();
        return fail(m.c_str());
    }
    HRESULT hr = dev_->CreateRootSignature(0, sig->GetBufferPointer(),
                                           sig->GetBufferSize(), IID_PPV_ARGS(&root_));
    sig->Release();
    if (serr) serr->Release();
    if (FAILED(hr)) return fail("CreateRootSignature failed");

    // ---- descriptor heap ----
    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = kTotalDesc;
    hd.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(dev_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap_))))
        return fail("CreateDescriptorHeap failed");
    inc_ = dev_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // ---- pipeline states ----
    struct Pass { ID3D12PipelineState** pso; const char* entry; };
    const Pass passes[] = {
        { &psoLuma_,   "cs_luma"       },
        { &psoDown2l_, "cs_down2l"     },
        { &psoDown2c_, "cs_down2c"     },
        { &psoGauss_,  "cs_gauss3c"    },
        { &psoSeedWarp_, "cs_seed_warp" },
        { &psoCoarse_, "cs_bm_coarse"  },
        { &psoRefine_, "cs_bm_refine"  },
        { &psoMedian_, "cs_median3f"   },
        { &psoFill_,   "cs_flow_fill"  },
        { &psoUp2_,    "cs_up_f2"      },
        { &psoUp1_,    "cs_up_f1"      },
        { &psoBlend_,  "cs_warp_blend" },
    };
    for (const Pass& p : passes)
    {
        *p.pso = makePso(dev_, root_, shader_path, p.entry, &err_);
        if (!*p.pso) { if (err) *err = err_; shutdown(); return false; }
    }

    // ---- resources ----
    // Four pyramid levels.  The large search radius lives at the coarsest
    // level so it costs (scale^2) fewer SAD comparisons: a +-12 px full search
    // at 1/8 res reaches +-96 px at full res on a 1/64-sized grid, which a
    // +-12 search at 1/4 res could never afford.
    const uint32_t W = w_, H = h_;
    const uint32_t wh = W / 2, hh = H / 2;
    const uint32_t wq = W / 4, hq = H / 4;
    const uint32_t we = W / 8, he = H / 8;
    gw3_ = ceildiv(W, 32); gh3_ = ceildiv(H, 32);
    gw2_ = ceildiv(W, 16); gh2_ = ceildiv(H, 16);
    gw1_ = ceildiv(W, 8);  gh1_ = ceildiv(H, 8);
    gw0_ = ceildiv(W, 4);  gh0_ = ceildiv(H, 4);

    const DXGI_FORMAT F1 = DXGI_FORMAT_R32_FLOAT;                 // confidence / scratch
    // luma may be FP16; flow stays FP32 because a flow vector's useful precision
    // is absolute (pixels), and FP16 loses it above ~1024 px of displacement.
    const DXGI_FORMAT FL = opt_.luma_fp16 ? DXGI_FORMAT_R16_FLOAT
                                          : DXGI_FORMAT_R32_FLOAT;
    const DXGI_FORMAT F2 = DXGI_FORMAT_R32G32_FLOAT;
    const DXGI_FORMAT F4 = DXGI_FORMAT_R16G16B16A16_FLOAT;

    for (int i = 0; i < 4; ++i)
    {
        uint32_t lw = (i == 0) ? W : (i == 1 ? wh : (i == 2 ? wq : we));
        uint32_t lh = (i == 0) ? H : (i == 1 ? hh : (i == 2 ? hq : he));
        lumaA_[i] = makeTex(lw, lh, FL);
        lumaB_[i] = makeTex(lw, lh, FL);
        if (!lumaA_[i] || !lumaB_[i]) return fail("luma alloc failed");
    }
    baseA_ = makeTex(wh, hh, F4);  baseB_ = makeTex(wh, hh, F4);
    blurA_ = makeTex(wh, hh, F4);  blurB_ = makeTex(wh, hh, F4);
    if (!baseA_ || !baseB_ || !blurA_ || !blurB_) return fail("colour pyramid alloc failed");

    for (int d = 0; d < 2; ++d)
    {
        flow_[d][0] = makeTex(gw3_, gh3_, F2);
        flow_[d][1] = makeTex(gw2_, gh2_, F2);
        flow_[d][2] = makeTex(gw1_, gh1_, F2);
        flow_[d][3] = makeTex(gw0_, gh0_, F2);
        flow_[d][4] = makeTex(W, H, F2);
        flowTmp_[d]  = makeTex(gw0_, gh0_, F2);
        costGrid_[d] = makeTex(gw0_, gh0_, F1);
        costFull_[d] = makeTex(W, H, F1);
        if (!flow_[d][0] || !flow_[d][1] || !flow_[d][2] || !flow_[d][3] ||
            !flow_[d][4] || !flowTmp_[d] || !costGrid_[d] || !costFull_[d])
            return fail("flow alloc failed");
    }
    seedWarp_ = makeTex(W, H, FL);
    costTmp2_ = makeTex(gw2_, gh2_, F1);
    costTmp1_ = makeTex(gw1_, gh1_, F1);
    fillFlow_ = makeTex(gw0_, gh0_, F2);
    fillCost_ = makeTex(gw0_, gh0_, F1);
    dummy_    = makeTex(1, 1, F4);
    if (!seedWarp_ || !costTmp2_ || !costTmp1_ || !fillFlow_ || !fillCost_ || !dummy_)
        return fail("scratch alloc failed");

    // register every internal resource as COMMON
    stateReset();
    for (int i = 0; i < 4; ++i) { stateSet(lumaA_[i], D3D12_RESOURCE_STATE_COMMON);
                                   stateSet(lumaB_[i], D3D12_RESOURCE_STATE_COMMON); }
    stateSet(baseA_, D3D12_RESOURCE_STATE_COMMON); stateSet(baseB_, D3D12_RESOURCE_STATE_COMMON);
    stateSet(blurA_, D3D12_RESOURCE_STATE_COMMON); stateSet(blurB_, D3D12_RESOURCE_STATE_COMMON);
    for (int d = 0; d < 2; ++d)
    {
        for (int i = 0; i < 5; ++i) stateSet(flow_[d][i], D3D12_RESOURCE_STATE_COMMON);
        stateSet(flowTmp_[d], D3D12_RESOURCE_STATE_COMMON);
        stateSet(costGrid_[d], D3D12_RESOURCE_STATE_COMMON);
        stateSet(costFull_[d], D3D12_RESOURCE_STATE_COMMON);
    }
    stateSet(seedWarp_, D3D12_RESOURCE_STATE_COMMON);
    stateSet(costTmp2_, D3D12_RESOURCE_STATE_COMMON);
    stateSet(costTmp1_, D3D12_RESOURCE_STATE_COMMON);
    stateSet(fillFlow_, D3D12_RESOURCE_STATE_COMMON);
    stateSet(fillCost_, D3D12_RESOURCE_STATE_COMMON);
    stateSet(dummy_, D3D12_RESOURCE_STATE_COMMON);

    ready_ = true;
    return true;
}

void VfiEngine::shutdown()
{
    auto rel  = [](ID3D12Resource*& r) { if (r) { r->Release(); r = nullptr; } };
    auto relp = [](ID3D12PipelineState*& p) { if (p) { p->Release(); p = nullptr; } };

    for (int i = 0; i < 4; ++i) { rel(lumaA_[i]); rel(lumaB_[i]); }
    rel(baseA_); rel(baseB_); rel(blurA_); rel(blurB_);
    for (int d = 0; d < 2; ++d)
    {
        for (int i = 0; i < 5; ++i) rel(flow_[d][i]);
        rel(flowTmp_[d]); rel(costGrid_[d]); rel(costFull_[d]);
    }
    rel(seedWarp_); rel(costTmp2_); rel(costTmp1_); rel(fillFlow_); rel(fillCost_); rel(dummy_);

    relp(psoLuma_); relp(psoDown2l_); relp(psoDown2c_); relp(psoGauss_);
    relp(psoSeedWarp_); relp(psoCoarse_); relp(psoRefine_); relp(psoMedian_); relp(psoFill_);
    relp(psoUp2_); relp(psoUp1_); relp(psoBlend_);

    if (heap_) { heap_->Release(); heap_ = nullptr; }
    if (root_) { root_->Release(); root_ = nullptr; }
    stateReset();
    dev_   = nullptr;
    ready_ = false;
}

bool VfiEngine::record(ID3D12GraphicsCommandList* cl, uint32_t slot,
                       ID3D12Resource* inA, D3D12_RESOURCE_STATES stA,
                       ID3D12Resource* inB, D3D12_RESOURCE_STATES stB,
                       ID3D12Resource* out, D3D12_RESOURCE_STATES stOut,
                       float t, bool out_is_bgra)
{
    if (!ready_ || !cl || !inA || !inB || !out) return false;
    if (slot >= kMaxSlots) return false;

    dispatch_count_ = 0;

    const uint32_t W = w_, H = h_;
    const uint32_t wh = W / 2, hh = H / 2;
    const uint32_t wq = W / 4, hq = H / 4;
    const uint32_t we = W / 8, he = H / 8;

    // the caller's declaration of external states is authoritative
    stateSet(inA, stA);
    stateSet(inB, stB);
    stateSet(out, stOut);

    cl->SetDescriptorHeaps(1, &heap_);
    cl->SetComputeRootSignature(root_);

    uint32_t d = 0;

    auto cpuAt = [&](uint32_t idx, uint32_t off) {
        D3D12_CPU_DESCRIPTOR_HANDLE h = heap_->GetCPUDescriptorHandleForHeapStart();
        h.ptr += (SIZE_T)inc_ * (slot * kMaxDispatch * kDescPerDisp + idx * kDescPerDisp + off);
        return h;
    };
    auto gpuAt = [&](uint32_t idx, uint32_t off) {
        D3D12_GPU_DESCRIPTOR_HANDLE h = heap_->GetGPUDescriptorHandleForHeapStart();
        h.ptr += (UINT64)inc_ * (slot * kMaxDispatch * kDescPerDisp + idx * kDescPerDisp + off);
        return h;
    };

    auto want = [&](ID3D12Resource* r, D3D12_RESOURCE_STATES s) {
        D3D12_RESOURCE_STATES b = stateGet(r);
        if (b != s) { barrier(cl, r, b, s); stateSet(r, s); }
    };

    VfiParams pc = {};
    pc.T        = t;
    pc.FbThresh = opt_.fb_thresh;
    pc.HfAmount = opt_.hf_amount;
    pc.WBias    = opt_.w_bias;
    pc.Flags    = opt_.temporal_aa ? 1u : 0u;
    pc.Block    = (int32_t)TG_BLOCK;
    pc.P0       = opt_.prewarp_seed ? 1.0f : 0.0f;
    pc.OutW     = W;
    pc.OutH     = H;

    typedef std::pair<uint32_t, ID3D12Resource*> Bind;

    auto go = [&](ID3D12PipelineState* pso, const VfiParams& p,
                  std::initializer_list<Bind> srvs,
                  std::initializer_list<Bind> uavs,
                  uint32_t gx, uint32_t gy)
    {
        const uint32_t idx = d++;
        const D3D12_CPU_DESCRIPTOR_HANDLE cpuS = cpuAt(idx, 0);
        const D3D12_CPU_DESCRIPTOR_HANDLE cpuU = cpuAt(idx, kSrvPerDisp);

        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels = 1;

        D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {};
        ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

        // first make every slot valid, then overwrite the ones this pass uses
        for (uint32_t i = 0; i < kSrvPerDisp; ++i)
        {
            sd.Format = dummy_->GetDesc().Format;
            D3D12_CPU_DESCRIPTOR_HANDLE h = cpuS; h.ptr += (SIZE_T)inc_ * i;
            dev_->CreateShaderResourceView(dummy_, &sd, h);
        }
        for (uint32_t i = 0; i < kUavPerDisp; ++i)
        {
            ud.Format = dummy_->GetDesc().Format;
            D3D12_CPU_DESCRIPTOR_HANDLE h = cpuU; h.ptr += (SIZE_T)inc_ * i;
            dev_->CreateUnorderedAccessView(dummy_, nullptr, &ud, h);
        }

        for (const Bind& b : srvs)
        {
            sd.Format = b.second->GetDesc().Format;
            D3D12_CPU_DESCRIPTOR_HANDLE h = cpuS; h.ptr += (SIZE_T)inc_ * b.first;
            dev_->CreateShaderResourceView(b.second, &sd, h);
            want(b.second, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        for (const Bind& b : uavs)
        {
            ud.Format = b.second->GetDesc().Format;
            D3D12_CPU_DESCRIPTOR_HANDLE h = cpuU; h.ptr += (SIZE_T)inc_ * b.first;
            dev_->CreateUnorderedAccessView(b.second, nullptr, &ud, h);
            want(b.second, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }

        cl->SetPipelineState(pso);
        cl->SetComputeRoot32BitConstants(0, (UINT)(sizeof(VfiParams) / 4), &p, 0);
        cl->SetComputeRootDescriptorTable(1, gpuAt(idx, 0));
        cl->SetComputeRootDescriptorTable(2, gpuAt(idx, kSrvPerDisp));
        cl->Dispatch(gx, gy, 1);
        ++dispatch_count_;
    };

    auto groups = [&](uint32_t n) { return ceildiv(n, 16u); };

    // -----------------------------------------------------------------
    // Oracle mode.  An externally supplied flow field replaces the entire
    // estimation half.  This exists to separate two questions that are
    // otherwise indistinguishable in the numbers: "my matcher is bad" and
    // "classical warp/blend has a ceiling of its own".  Only 5 dispatches
    // run in this mode (2 colour downsample + 2 gaussian + 1 blend), which
    // is also the floor for a runtime that gets its flow from the engine
    // instead of estimating it.
    // -----------------------------------------------------------------
    const bool oracle = has_flow_override();

    // ---- estimation half: luma pyramid + matcher (skipped in oracle mode) ----
    if (!oracle)
    {
        // ---------------- luma ----------------
        pc.SrcW = 0;  pc.SrcH = 0;  pc.DstW = W;  pc.DstH = H;
        go(psoLuma_, pc, { { 0, inA } }, { { 0, lumaA_[0] } }, groups(W), groups(H));
        go(psoLuma_, pc, { { 0, inB } }, { { 0, lumaB_[0] } }, groups(W), groups(H));

        // ---------------- luma pyramid ----------------
        pc.SrcW = W;  pc.SrcH = H;  pc.DstW = wh; pc.DstH = hh;
        go(psoDown2l_, pc, { { 1, lumaA_[0] } }, { { 1, lumaA_[1] } }, groups(wh), groups(hh));
        go(psoDown2l_, pc, { { 1, lumaB_[0] } }, { { 1, lumaB_[1] } }, groups(wh), groups(hh));
        pc.SrcW = wh; pc.SrcH = hh; pc.DstW = wq; pc.DstH = hq;
        go(psoDown2l_, pc, { { 1, lumaA_[1] } }, { { 1, lumaA_[2] } }, groups(wq), groups(hq));
        go(psoDown2l_, pc, { { 1, lumaB_[1] } }, { { 1, lumaB_[2] } }, groups(wq), groups(hq));
        pc.SrcW = wq; pc.SrcH = hq; pc.DstW = we; pc.DstH = he;
        go(psoDown2l_, pc, { { 1, lumaA_[2] } }, { { 1, lumaA_[3] } }, groups(we), groups(he));
        go(psoDown2l_, pc, { { 1, lumaB_[2] } }, { { 1, lumaB_[3] } }, groups(we), groups(he));

    }

    // ---------------- half-res colour base ----------------
    pc.SrcW = W;  pc.SrcH = H;  pc.DstW = wh; pc.DstH = hh;
    go(psoDown2c_, pc, { { 2, inA } }, { { 2, baseA_ } }, groups(wh), groups(hh));
    go(psoDown2c_, pc, { { 2, inB } }, { { 2, baseB_ } }, groups(wh), groups(hh));
    pc.SrcW = wh; pc.SrcH = hh; pc.DstW = wh; pc.DstH = hh;
    go(psoGauss_, pc, { { 2, baseA_ } }, { { 3, blurA_ } }, groups(wh), groups(hh));
    go(psoGauss_, pc, { { 2, baseB_ } }, { { 3, blurB_ } }, groups(wh), groups(hh));

    // ---------------- optical flow, both directions (skipped in oracle mode) ----
    if (!oracle)
    {
        // ---------------- optical flow, both directions ----------------
        for (int dir = 0; dir < 2; ++dir)
        {
            ID3D12Resource* cur   = (dir == 0) ? lumaA_[0] : lumaB_[0];
            ID3D12Resource* nxt   = (dir == 0) ? lumaB_[0] : lumaA_[0];
            ID3D12Resource* cur1  = (dir == 0) ? lumaA_[1] : lumaB_[1];
            ID3D12Resource* nxt1  = (dir == 0) ? lumaB_[1] : lumaA_[1];
            ID3D12Resource* cur2  = (dir == 0) ? lumaA_[2] : lumaB_[2];
            ID3D12Resource* nxt2  = (dir == 0) ? lumaB_[2] : lumaA_[2];
            ID3D12Resource* cur3  = (dir == 0) ? lumaA_[3] : lumaB_[3];
            ID3D12Resource* nxt3  = (dir == 0) ? lumaB_[3] : lumaA_[3];

            // L3: 1/8 res, wide-open full search.  Flow values are in 1/8-res px.
            pc.SrcW = we; pc.SrcH = he; pc.DstW = gw3_; pc.DstH = gh3_;
            pc.SearchR = opt_.radius_l3;
            go(psoCoarse_, pc, { { 3, cur3 }, { 4, nxt3 } }, { { 4, flow_[dir][0] } },
               groups(gw3_), groups(gh3_));

            // L2: 1/4 res.  The seed carries over from L3 (x2 units), so the
            // reference is shifted by its integer part first and the refinement
            // below only has to search the residual.  PrevInvW/H address the
            // *luma* of this level, which is the coordinate both passes share.
            pc.SrcW = wq; pc.SrcH = hq;
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
               { { 4, flow_[dir][1] }, { 5, costTmp2_ } }, groups(gw2_), groups(gh2_));

            // L1: 1/2 res
            pc.SrcW = wh; pc.SrcH = hh;
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
               { { 4, flow_[dir][2] }, { 5, costTmp1_ } }, groups(gw1_), groups(gh1_));

            // L0: full res, final sub-pixel polish.  PrevInvW/H are the luma
            // reciprocals here, not the grid ones: cs_seed_warp and cs_bm_refine
            // must round the same seed at the same position or the residual
            // they search is not the residual the warp introduced.
            pc.SrcW = W;  pc.SrcH = H;
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
               { { 4, flow_[dir][3] }, { 5, costGrid_[dir] } }, groups(gw0_), groups(gh0_));

            // median (separate target: a UAV cannot be read and written at once)
            pc.SrcW = gw0_; pc.SrcH = gh0_; pc.DstW = gw0_; pc.DstH = gh0_;
            go(psoMedian_, pc, { { 6, flow_[dir][3] } }, { { 4, flowTmp_[dir] } },
               groups(gw0_), groups(gh0_));

            // hole fill: two passes, stride 6 then 12 -> ~72 px reach at 1/4 res
            pc.SrcW = gw0_; pc.SrcH = gh0_; pc.DstW = gw0_; pc.DstH = gh0_;
            pc.SearchR = 6;
            go(psoFill_, pc, { { 6, flowTmp_[dir] }, { 7, costGrid_[dir] } },
               { { 4, fillFlow_ }, { 5, fillCost_ } }, groups(gw0_), groups(gh0_));
            pc.SearchR = 12;
            go(psoFill_, pc, { { 6, fillFlow_ }, { 7, fillCost_ } },
               { { 4, flowTmp_[dir] }, { 5, costGrid_[dir] } }, groups(gw0_), groups(gh0_));

            // upsample flow to full res (values already in full-res pixels)
            pc.SrcW = gw0_; pc.SrcH = gh0_; pc.DstW = W; pc.DstH = H;
            pc.UpScale = 1.0f;
            go(psoUp2_, pc, { { 6, flowTmp_[dir] } }, { { 4, flow_[dir][4] } },
               groups(W), groups(H));

            // upsample confidence
            pc.SrcW = gw0_; pc.SrcH = gh0_;
            go(psoUp1_, pc, { { 7, costGrid_[dir] } }, { { 5, costFull_[dir] } },
               groups(W), groups(H));
        }

    }

    // ---------------- synthesis ----------------
    pc.SrcW = W; pc.SrcH = H; pc.OutW = W; pc.OutH = H;
    pc.Flags = (opt_.temporal_aa ? 1u : 0u) | (out_is_bgra ? 2u : 0u);
    ID3D12Resource* fAB  = oracle ? ovrAB_   : flow_[0][4];
    ID3D12Resource* fBA  = oracle ? ovrBA_   : flow_[1][4];
    ID3D12Resource* fCst = oracle ? ovrCost_ : costFull_[0];

    go(psoBlend_, pc,
       { { 8, inA }, { 9, inB }, { 10, blurA_ }, { 11, blurB_ },
         { 12, fAB }, { 13, fBA }, { 14, fCst } },
       { { 6, out } }, groups(W), groups(H));

    // restore external states
    want(out, stOut);
    want(inB, stB);
    want(inA, stA);

    return true;
}

} // namespace sm86
