#pragma once
// ============================================================================
//  osd_overlay.h - Lightweight Direct3D In-Game OSD & Telemetry Overlay
//  Zero-dependency, high-performance GDI-to-Compute rasterizer.
// ============================================================================
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d12.h>
#include <d3d11.h>
#include <dxgi.h>
#include <cstdint>
#include <string>
#include <chrono>

namespace sm86 {

struct OsdParams {
    uint32_t DstX;
    uint32_t DstY;
    uint32_t OverlayW;
    uint32_t OverlayH;
    float    MasterAlpha;
    uint32_t BackbufferW;
    uint32_t BackbufferH;
    uint32_t Pad;
};

class OsdOverlay {
public:
    static const uint32_t OSD_WIDTH  = 540;
    static const uint32_t OSD_HEIGHT = 150;

    OsdOverlay();
    ~OsdOverlay();

    // Direct3D 12 Backend
    bool Initialize(ID3D12Device* device, ID3D12CommandQueue* queue);
    void Shutdown();

    // Direct3D 11 Backend (e.g. MPC-HC, MPC-VR, D3D11 games)
    bool InitializeD3D11(ID3D11Device* device);
    void ShutdownD3D11();
    bool RenderD3D11(IDXGISwapChain* swap);

    // Updates hotkeys (F11/F10/F9) and telemetry
    void Update(float frameGenMs, const char* engineName, bool uiMaskEnabled, bool debugHeatmap);

    // Records the overlay blending onto the target backbuffer (D3D12)
    bool Record(ID3D12GraphicsCommandList* cl,
                ID3D12Resource* backbuffer,
                uint32_t backbufferW, uint32_t backbufferH);

    bool IsVisible() const { return m_visible; }
    void ToggleVisible()   { m_visible = !m_visible; }

    bool IsUiMaskEnabled() const  { return m_uiMaskEnabled; }
    bool IsDebugHeatmap()  const  { return m_debugHeatmap; }

    bool IsInitialized() const { return m_initialized || m_initialized11; }

private:
    void InitGdi();
    void RenderGdiSurface(float frameGenMs, const char* engineName, bool uiMaskEnabled, bool debugHeatmap);
    bool CreateGpuResources();

    ID3D12Device*           m_device        = nullptr;
    ID3D12CommandQueue*     m_queue         = nullptr;
    ID3D12RootSignature*    m_rootSig       = nullptr;
    ID3D12PipelineState*    m_pso           = nullptr;
    ID3D12DescriptorHeap*   m_srvUavHeap    = nullptr;
    UINT                    m_descriptorSize = 0;

    ID3D12Resource*         m_overlayTex    = nullptr;
    ID3D12Resource*         m_uploadBuffer  = nullptr;
    uint8_t*                m_mappedUpload  = nullptr;

    // Direct3D 11 Backend
    ID3D11Device*           m_device11      = nullptr;
    ID3D11DeviceContext*    m_context11     = nullptr;
    ID3D11Texture2D*        m_overlayTex11  = nullptr;
    ID3D11ShaderResourceView* m_overlaySRV11 = nullptr;
    ID3D11VertexShader*     m_vs11          = nullptr;
    ID3D11PixelShader*      m_ps11          = nullptr;
    ID3D11Buffer*           m_cb11          = nullptr;
    ID3D11BlendState*       m_blendState11  = nullptr;
    ID3D11DepthStencilState* m_dsState11    = nullptr;
    ID3D11RasterizerState*  m_rsState11     = nullptr;
    ID3D11SamplerState*     m_sampler11     = nullptr;
    bool                    m_initialized11 = false;

    // GDI Rasterizer Objects
    HDC                     m_memDC         = nullptr;
    HBITMAP                 m_hBitmap       = nullptr;
    HBITMAP                 m_hOldBitmap    = nullptr;
    HFONT                   m_hFontTitle    = nullptr;
    HFONT                   m_hFontText     = nullptr;
    uint32_t*               m_pBits         = nullptr;

    // State & Metrics
    bool                    m_initialized   = false;
    bool                    m_visible       = true;
    bool                    m_uiMaskEnabled = true;
    bool                    m_debugHeatmap  = false;

    LARGE_INTEGER           m_perfFreq      = {};
    LARGE_INTEGER           m_lastTime      = {};
    float                   m_currentFps    = 0.0f;
    float                   m_fpsHistory[30] = {};
    int                     m_fpsIndex      = 0;
    uint64_t                m_frameCount    = 0;
};

} // namespace sm86
