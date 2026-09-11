#pragma once
// ============================================================================
//  d3d11_to_d3d12_bridge.h - Shadow D3D12 SwapChain Bridge for Road 1
//  Enables NvPresent64 (Smooth Motion / DLSS 3 Frame Gen) in D3D11 apps (e.g. MPC-HC)
// ============================================================================
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <cstdint>

namespace sm86 {

class D3D11ToD3D12Bridge {
public:
    D3D11ToD3D12Bridge();
    ~D3D11ToD3D12Bridge();

    // Initializes or reconfigures the shadow D3D12 swapchain on the given HWND
    bool Initialize(ID3D11Device* dev11, HWND hwnd, uint32_t width, uint32_t height, DXGI_FORMAT format);
    void Shutdown();

    // Copies D3D11 backbuffer to D3D12 shadow swapchain and presents via NvPresent64
    bool Present(IDXGISwapChain* swap11, UINT sync, UINT flags);

    bool IsActive() const { return m_active && (m_wrapper != nullptr); }
    void* GetWrapper() const { return m_wrapper; }
    ID3D12Device* GetD3D12Device() const { return m_dev12; }
    ID3D12CommandQueue* GetCommandQueue() const { return m_cq12; }
    ID3D12GraphicsCommandList* GetCommandList() const { return m_cl12; }
    ID3D12Resource* GetCurrentBackbuffer() const;
    uint32_t GetWidth() const { return m_width; }
    uint32_t GetHeight() const { return m_height; }

private:
    bool CreateSharedTexture(uint32_t width, uint32_t height, DXGI_FORMAT format);
    bool CreateD3D12Resources(HWND hwnd, uint32_t width, uint32_t height, DXGI_FORMAT format);

    ID3D11Device*              m_dev11          = nullptr;
    ID3D11DeviceContext*       m_ctx11          = nullptr;
    ID3D11Texture2D*           m_sharedTex11    = nullptr;
    HANDLE                     m_sharedHandle   = nullptr;

    ID3D12Device*              m_dev12          = nullptr;
    ID3D12CommandQueue*        m_cq12           = nullptr;
    ID3D12CommandAllocator*    m_alloc12        = nullptr;
    ID3D12GraphicsCommandList* m_cl12           = nullptr;
    IDXGISwapChain1*           m_swap12         = nullptr;
    IDXGISwapChain3*           m_swap3_12       = nullptr;
    ID3D12Resource*            m_sharedTex12    = nullptr;
    ID3D12Resource*            m_backbuffers12[2] = { nullptr, nullptr };
    ID3D12Fence*               m_fence12        = nullptr;
    HANDLE                     m_fenceEvent12   = nullptr;
    UINT64                     m_fenceVal12     = 0;

    void*                      m_wrapper        = nullptr;
    HWND                       m_hwnd           = nullptr;
    uint32_t                   m_width          = 0;
    uint32_t                   m_height         = 0;
    DXGI_FORMAT                m_format         = DXGI_FORMAT_UNKNOWN;
    bool                       m_active         = false;
};

} // namespace sm86
