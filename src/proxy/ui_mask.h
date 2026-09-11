#pragma once
// ============================================================================
//  ui_mask.h - Pass-Level UI Mask Protection Engine
//  Eliminates HUD ghosting, text blurring, and jelly crosshairs in frame gen.
// ============================================================================
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d12.h>
#include <cstdint>
#include <string>

namespace sm86 {

struct UiMaskConfig {
    bool  enabled         = true;    // bit 0: enable UI mask protection
    bool  debugHeatmap    = false;   // bit 1: show red mask heatmap for tuning
    bool  crosshairBoost  = true;    // bit 2: aggressively protect center reticle
    bool  groundTruthPass = false;   // bit 3: ground-truth clean pass mode (RenoDX/DLSS5)
    float sensitivity     = 0.08f;   // temporal or pass delta threshold
    float blendGain       = 1.0f;    // mask contrast / gain
    float crosshairRadius = 0.04f;   // screen-space UV radius for crosshairs
    float cornerMargin    = 0.22f;   // screen-space UV margin for corner HUDs
};

struct UiMaskConstants {
    uint32_t Width;
    uint32_t Height;
    float    Sensitivity;
    float    BlendGain;
    uint32_t Flags;
    float    CrosshairRadius;
    float    CornerRadius;
    float    Pad0;
};

class UiMaskEngine {
public:
    UiMaskEngine();
    ~UiMaskEngine();

    bool Initialize(ID3D12Device* device);
    void Shutdown();

    // Records the UI mask detection and reconstruction compute dispatch
    bool Record(ID3D12GraphicsCommandList* cl,
                ID3D12Resource* currFrame,
                ID3D12Resource* prevFrame,
                ID3D12Resource* genFrame,
                ID3D12Resource* outFrame,
                uint32_t width, uint32_t height,
                const UiMaskConfig& config);

    bool IsInitialized() const { return m_initialized; }

private:
    bool CreateRootSignature();
    bool CreatePipelineState();

    ID3D12Device*               m_device        = nullptr;
    ID3D12RootSignature*        m_rootSig       = nullptr;
    ID3D12PipelineState*        m_pso           = nullptr;
    ID3D12DescriptorHeap*       m_srvUavHeap    = nullptr;
    UINT                        m_descriptorSize = 0;
    bool                        m_initialized   = false;
};

} // namespace sm86
