// ============================================================================
//  sm86_smooth - VfiEngine
//  Records the whole frame-interpolation chain onto a caller-supplied command
//  list.  Deliberately device-agnostic: the same engine is used by the injected
//  runtime (on the game's D3D12 queue) and by the headless self-test tool.
// ============================================================================
#pragma once

#include <cstdint>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>
#include <d3d12.h>

namespace sm86 {

// must match BLK in vfi.hlsl
static const uint32_t TG_BLOCK = 4;

struct VfiOptions
{
    // Search radii, in *level* pixels.  A large radius belongs at a coarse
    // level: the same radius there costs (scale^2) fewer SAD comparisons.
    // Full-res reach = radius * scale.
    //   l3 sits at 1/8 res, so radius_l3 = 12 reaches +-96 px.
    //   radius_l3 = 0 means "auto": derived from the frame width, because a
    //   fixed pixel budget stops being enough as resolution goes up.
    int   radius_l3 = 0;       // 1/8 res full search, 0 = auto
    int   radius_l2 = 4;       // 1/4 res local search -> +-16 px
    int   radius_l1 = 4;       // 1/2 res local search -> +-8  px
    int   radius_l0 = 2;       // 1/1 res local search -> +-2  px
    float fb_thresh  = 1.5f;   // forward/backward consistency, px
    float hf_amount  = 1.0f;   // high-frequency transfer gain
    // How the refinement handles the seed inherited from the previous level.
    //   false: seed inside the tile index, global read when it leaves the tile
    //   true : cs_seed_warp removes the seed first, residual search centred on 0
    // Measured both ways - see the two tables in README section 3.  The default
    // is the one with the better end-to-end number, not the faster one.
    bool  prewarp_seed = false;
    // Store the luma pyramid as FP16 instead of FP32.  Measured: this is a
    // bandwidth experiment, not a math one - the SAD reads dominate, so halving
    // the bytes per luma sample is the only version of "use FP16" that can pay
    // here.  Ampere's FP32 pipeline does not run FP32 arithmetic slower than
    // FP16 unless the work is packed as half2, which this kernel is not.
    bool  luma_fp16 = true;    // measured 2.67x faster, bit-identical output
    float w_bias     = 0.5f;   // weight bias: 0 -> A, 1 -> B
    bool  temporal_aa = true;  // 2-frame temporal average in static regions
};

// The root-constant block.  Field order MUST match `cbuffer Params` in vfi.hlsl.
struct VfiParams
{
    uint32_t SrcW, SrcH, DstW, DstH, OutW, OutH;
    int32_t  SearchR, Block;
    float    T, FbThresh, HfAmount, WBias;
    uint32_t Flags;
    float    PrevScale, PrevInvW, PrevInvH, UpScale;
    float    P0, P1, P2;
};
static_assert(sizeof(VfiParams) == 80, "root constants layout drifted");

class VfiEngine
{
public:
    VfiEngine() = default;
    ~VfiEngine();

    VfiEngine(const VfiEngine&) = delete;
    VfiEngine& operator=(const VfiEngine&) = delete;

    // Compiles the shaders and allocates every intermediate.  Expensive
    // (D3DCompile + PSO creation, ~100-400 ms) - call it off the present path.
    bool init(ID3D12Device* dev, DXGI_FORMAT backbuffer_format,
              uint32_t width, uint32_t height,
              const VfiOptions& opt, const wchar_t* shader_path,
              std::string* err);

    bool ready() const { return ready_; }
    void shutdown();

    // Record one generated frame.  Source/destination states are supplied by
    // the caller and restored before returning, so this is safe to splice into
    // someone else's command list.
    //
    //   t = 0.5  -> midpoint between A and B
    //   t > 1    -> extrapolation past B (t = 1 + dt)
    bool record(ID3D12GraphicsCommandList* cl, uint32_t slot,
                ID3D12Resource* inA, D3D12_RESOURCE_STATES stA,
                ID3D12Resource* inB, D3D12_RESOURCE_STATES stB,
                ID3D12Resource* out, D3D12_RESOURCE_STATES stOut,
                float t, bool out_is_bgra);

    // Feed a pre-computed flow field instead of estimating one.  Used by the
    // oracle experiment: it isolates the warp/blend stage from the flow
    // estimator, so "my blend is bad" and "classical flow is bad" stop being
    // the same question.  Pass nullptr for all three to go back to estimating.
    // Textures must be R32G32_FLOAT / R32_FLOAT at full resolution and stay
    // alive for as long as the override is installed.
    void set_flow_override(ID3D12Resource* fab, ID3D12Resource* fba, ID3D12Resource* cost)
    {
        ovrAB_ = fab; ovrBA_ = fba; ovrCost_ = cost;
    }
    bool has_flow_override() const { return ovrAB_ && ovrBA_ && ovrCost_; }

    uint32_t    width()  const { return w_; }
    uint32_t    height() const { return h_; }
    DXGI_FORMAT format() const { return fmt_; }

    // diagnostics
    uint32_t dispatch_count() const { return dispatch_count_; }
    int      effective_radius_l3() const { return opt_.radius_l3; }
    const std::string& last_error() const { return err_; }
    ID3D12Resource*       debug_flow_ab() const { return flow_[0][4]; }
    ID3D12Resource*       debug_flow_ba() const { return flow_[1][4]; }
    ID3D12Resource*       debug_cost()    const { return costFull_[0]; }
    D3D12_RESOURCE_STATES debug_state(ID3D12Resource* r) const { return stateGet(r); }

private:
    ID3D12Resource* makeTex(uint32_t w, uint32_t h, DXGI_FORMAT fmt);

    // resource-state bookkeeping, persists across frames so that a resource
    // written as UAV this frame and read as SRV the next stays consistent
    D3D12_RESOURCE_STATES stateGet(ID3D12Resource* r) const;
    void                stateSet(ID3D12Resource* r, D3D12_RESOURCE_STATES s);
    void                stateReset();
    std::vector<std::pair<ID3D12Resource*, D3D12_RESOURCE_STATES>> states_;

    ID3D12Device*                dev_   = nullptr;
    ID3D12RootSignature*         root_  = nullptr;
    ID3D12DescriptorHeap*        heap_  = nullptr;
    UINT                         inc_   = 0;
    bool                         ready_ = false;
    std::string                  err_;

    uint32_t    w_ = 0, h_ = 0;
    DXGI_FORMAT fmt_ = DXGI_FORMAT_UNKNOWN;
    VfiOptions  opt_{};
    uint32_t    dispatch_count_ = 0;

    // grids (1/32, 1/16, 1/8, 1/4 of full res)
    uint32_t gw3_ = 0, gh3_ = 0, gw2_ = 0, gh2_ = 0;
    uint32_t gw1_ = 0, gh1_ = 0, gw0_ = 0, gh0_ = 0;

    ID3D12PipelineState* psoLuma_   = nullptr;
    ID3D12PipelineState* psoDown2l_ = nullptr;
    ID3D12PipelineState* psoDown2c_ = nullptr;
    ID3D12PipelineState* psoGauss_  = nullptr;
    ID3D12PipelineState* psoSeedWarp_ = nullptr;
    ID3D12PipelineState* psoCoarse_ = nullptr;
    ID3D12PipelineState* psoRefine_ = nullptr;
    ID3D12PipelineState* psoMedian_ = nullptr;
    ID3D12PipelineState* psoFill_   = nullptr;
    ID3D12PipelineState* psoUp2_    = nullptr;
    ID3D12PipelineState* psoUp1_    = nullptr;
    ID3D12PipelineState* psoBlend_  = nullptr;

    // luma pyramid: [0]=1/1 [1]=1/2 [2]=1/4 [3]=1/8
    // flow_[dir]:   [0]=1/8 grid [1]=1/4 grid [2]=1/2-ish [3]=1/4 grid(full-res px)
    //               naming below is by grid resolution, see vfi.cpp
    ID3D12Resource* lumaA_[4] = {};
    ID3D12Resource* lumaB_[4] = {};
    ID3D12Resource* baseA_ = nullptr, * baseB_ = nullptr;
    ID3D12Resource* blurA_ = nullptr, * blurB_ = nullptr;
    ID3D12Resource* flow_[2][5] = {};       // L3, L2, L1, L0 grids + full res
    ID3D12Resource* flowTmp_[2] = {};       // median output (UAV cannot alias)
    ID3D12Resource* costGrid_[2] = {};      // L0 grid
    ID3D12Resource* costFull_[2] = {};      // full res
    ID3D12Resource* costTmp2_ = nullptr;    // L2 grid scratch
    ID3D12Resource* costTmp1_ = nullptr;    // L1 grid scratch
    ID3D12Resource* fillFlow_ = nullptr;    // L0 grid, hole-fill ping-pong
    ID3D12Resource* fillCost_ = nullptr;    // L0 grid, hole-fill ping-pong
    ID3D12Resource* seedWarp_ = nullptr;    // reference shifted by the seed; full-res alloc, per-level extents
    ID3D12Resource* dummy_ = nullptr;

    // optional externally supplied flow (oracle / A-B testing)
    ID3D12Resource* ovrAB_ = nullptr, * ovrBA_ = nullptr, * ovrCost_ = nullptr;
};

} // namespace sm86
