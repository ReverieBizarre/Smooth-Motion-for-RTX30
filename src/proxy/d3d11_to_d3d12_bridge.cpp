// ============================================================================
//  d3d11_to_d3d12_bridge.cpp - Shadow D3D12 SwapChain Bridge for Road 1
//  Enables NvPresent64 (Smooth Motion / DLSS 3 Frame Gen) in D3D11 apps (e.g. MPC-HC)
// ============================================================================
#include "d3d11_to_d3d12_bridge.h"
#include <cstdio>
#include <cstdarg>

static void LogBridge(const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    OutputDebugStringA(buf);
    FILE* f = fopen("C:\\Users\\lsp\\Documents\\antigravity\\calm-carson\\sm86_debug.log", "a");
    if (f) {
        fputs(buf, f);
        fclose(f);
    }
}

namespace sm86 {

D3D11ToD3D12Bridge::D3D11ToD3D12Bridge() = default;

D3D11ToD3D12Bridge::~D3D11ToD3D12Bridge() {
    Shutdown();
}

void D3D11ToD3D12Bridge::Shutdown() {
    if (m_sharedHandle) {
        CloseHandle(m_sharedHandle);
        m_sharedHandle = nullptr;
    }
    if (m_fenceEvent12) {
        CloseHandle(m_fenceEvent12);
        m_fenceEvent12 = nullptr;
    }
    if (m_fence12)        { m_fence12->Release(); m_fence12 = nullptr; }
    if (m_backbuffers12[0]) { m_backbuffers12[0]->Release(); m_backbuffers12[0] = nullptr; }
    if (m_backbuffers12[1]) { m_backbuffers12[1]->Release(); m_backbuffers12[1] = nullptr; }
    if (m_sharedTex12)    { m_sharedTex12->Release(); m_sharedTex12 = nullptr; }
    if (m_swap3_12)       { m_swap3_12->Release(); m_swap3_12 = nullptr; }
    if (m_swap12)         { m_swap12->Release(); m_swap12 = nullptr; }
    if (m_cl12)           { m_cl12->Release(); m_cl12 = nullptr; }
    if (m_alloc12)        { m_alloc12->Release(); m_alloc12 = nullptr; }
    if (m_cq12)           { m_cq12->Release(); m_cq12 = nullptr; }
    if (m_dev12)          { m_dev12->Release(); m_dev12 = nullptr; }
    if (m_sharedTex11)    { m_sharedTex11->Release(); m_sharedTex11 = nullptr; }
    if (m_ctx11)          { m_ctx11->Release(); m_ctx11 = nullptr; }
    m_dev11 = nullptr;
    m_wrapper = nullptr;
    m_hwnd = nullptr;
    m_width = 0;
    m_height = 0;
    m_active = false;
}

bool D3D11ToD3D12Bridge::CreateSharedTexture(uint32_t width, uint32_t height, DXGI_FORMAT format) {
    if (!m_dev11 || !m_dev12) return false;

    DXGI_FORMAT texFormat = (format == DXGI_FORMAT_B8G8R8A8_UNORM) 
                          ? DXGI_FORMAT_B8G8R8A8_UNORM 
                          : DXGI_FORMAT_R8G8B8A8_UNORM;

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = width;
    td.Height = height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = texFormat;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

    HRESULT hr = m_dev11->CreateTexture2D(&td, nullptr, &m_sharedTex11);
    if (FAILED(hr)) {
        // Fallback to legacy shared handle flag
        td.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
        hr = m_dev11->CreateTexture2D(&td, nullptr, &m_sharedTex11);
        if (FAILED(hr)) {
            printf("[D3D11ToD3D12Bridge] Failed to create shared D3D11 texture: 0x%08X\n", (uint32_t)hr);
            return false;
        }
        IDXGIResource* res = nullptr;
        m_sharedTex11->QueryInterface(IID_PPV_ARGS(&res));
        res->GetSharedHandle(&m_sharedHandle);
        res->Release();
    } else {
        IDXGIResource1* res1 = nullptr;
        m_sharedTex11->QueryInterface(IID_PPV_ARGS(&res1));
        res1->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &m_sharedHandle);
        res1->Release();
    }

    hr = m_dev12->OpenSharedHandle(m_sharedHandle, IID_PPV_ARGS(&m_sharedTex12));
    if (FAILED(hr)) {
        LogBridge("[D3D11ToD3D12Bridge] Failed to open shared handle in D3D12: 0x%08X\n", (uint32_t)hr);
        return false;
    }

    LogBridge("[D3D11ToD3D12Bridge] Shared texture successfully created & opened in D3D12 (tex11=%p, tex12=%p)\n",
              m_sharedTex11, m_sharedTex12);
    return true;
}

bool D3D11ToD3D12Bridge::CreateD3D12Resources(HWND hwnd, uint32_t width, uint32_t height, DXGI_FORMAT format) {
    HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_dev12));
    if (FAILED(hr)) {
        LogBridge("[D3D11ToD3D12Bridge] D3D12CreateDevice failed: 0x%08X\n", (uint32_t)hr);
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr = m_dev12->CreateCommandQueue(&qd, IID_PPV_ARGS(&m_cq12));
    if (FAILED(hr)) {
        LogBridge("[D3D11ToD3D12Bridge] CreateCommandQueue failed: 0x%08X\n", (uint32_t)hr);
        return false;
    }

    hr = m_dev12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_alloc12));
    if (FAILED(hr)) return false;

    hr = m_dev12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_alloc12, nullptr, IID_PPV_ARGS(&m_cl12));
    if (FAILED(hr)) return false;
    m_cl12->Close();

    // Create D3D12 SwapChain on hwnd
    IDXGIFactory4* f = nullptr;
    CreateDXGIFactory1(IID_PPV_ARGS(&f));
    if (!f) {
        LogBridge("[D3D11ToD3D12Bridge] CreateDXGIFactory1 failed\n");
        return false;
    }

    DXGI_FORMAT swapFormat = (format == DXGI_FORMAT_B8G8R8A8_UNORM) 
                           ? DXGI_FORMAT_B8G8R8A8_UNORM 
                           : DXGI_FORMAT_R8G8B8A8_UNORM;

    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width = width;
    sd.Height = height;
    sd.Format = swapFormat;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    hr = f->CreateSwapChainForHwnd(m_cq12, hwnd, &sd, nullptr, nullptr, &m_swap12);
    f->Release();
    if (FAILED(hr) || !m_swap12) {
        LogBridge("[D3D11ToD3D12Bridge] CreateSwapChainForHwnd failed on HWND %p: 0x%08X\n", hwnd, (uint32_t)hr);
        return false;
    }

    // Inspect if NvPresent64 proxy wrapper is present at +0x18
    m_wrapper = *(void**)((uint8_t*)m_swap12 + 0x18);
    if (m_wrapper) {
        void** vt = *(void***)m_wrapper;
        typedef void (*pfnSetByte)(void*, uint8_t);
        ((pfnSetByte)vt[19])(m_wrapper, 1);
        ((pfnSetByte)vt[20])(m_wrapper, 1);
        LogBridge("[D3D11ToD3D12Bridge] >>> SUCCESS: NvPresent64 wrapped D3D12 shadow swapchain @ %p (wrapper @ %p) <<<\n",
                  m_swap12, m_wrapper);
    } else {
        LogBridge("[D3D11ToD3D12Bridge] Warning: NvPresent64 wrapper not found on shadow swapchain\n");
    }

    m_swap12->QueryInterface(IID_PPV_ARGS(&m_swap3_12));
    m_swap12->GetBuffer(0, IID_PPV_ARGS(&m_backbuffers12[0]));
    m_swap12->GetBuffer(1, IID_PPV_ARGS(&m_backbuffers12[1]));

    hr = m_dev12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence12));
    if (FAILED(hr)) return false;
    m_fenceEvent12 = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    m_fenceVal12 = 0;

    return true;
}

bool D3D11ToD3D12Bridge::Initialize(ID3D11Device* dev11, HWND hwnd, uint32_t width, uint32_t height, DXGI_FORMAT format) {
    LogBridge("[D3D11ToD3D12Bridge::Initialize] dev11=%p, hwnd=%p, res=%ux%u, fmt=%d\n",
              dev11, hwnd, width, height, (int)format);

    if (!dev11 || !hwnd) {
        LogBridge("[D3D11ToD3D12Bridge::Initialize] FAILED: dev11 or hwnd is NULL\n");
        return false;
    }
    // NvPresent64 resolution guard: Width >= 480 and Height >= 480
    if (width < 480 || height < 480) {
        LogBridge("[D3D11ToD3D12Bridge] Resolution %ux%u below NvPresent 480p threshold, passthrough.\n", width, height);
        return false;
    }

    if (m_active && m_hwnd == hwnd && m_width == width && m_height == height) {
        return true;
    }

    Shutdown();

    m_dev11 = dev11;
    m_dev11->GetImmediateContext(&m_ctx11);
    m_hwnd = hwnd;
    m_width = width;
    m_height = height;
    m_format = format;

    if (!CreateD3D12Resources(hwnd, width, height, format)) {
        LogBridge("[D3D11ToD3D12Bridge::Initialize] CreateD3D12Resources FAILED\n");
        Shutdown();
        return false;
    }

    if (!CreateSharedTexture(width, height, format)) {
        LogBridge("[D3D11ToD3D12Bridge::Initialize] CreateSharedTexture FAILED\n");
        Shutdown();
        return false;
    }

    m_active = true;
    LogBridge("[D3D11ToD3D12Bridge] Shadow D3D12 SwapChain Bridge ACTIVE for %ux%u (HWND %p, Wrapper %p)\n",
              width, height, hwnd, m_wrapper);
    return true;
}

ID3D12Resource* D3D11ToD3D12Bridge::GetCurrentBackbuffer() const {
    if (!m_swap3_12) return m_backbuffers12[0];
    UINT idx = m_swap3_12->GetCurrentBackBufferIndex();
    return m_backbuffers12[idx];
}

bool D3D11ToD3D12Bridge::Present(IDXGISwapChain* swap11, UINT sync, UINT flags) {
    if (!m_active || !swap11 || !m_ctx11 || !m_sharedTex11 || !m_swap12)
        return false;

    // 1. Get D3D11 backbuffer and copy to shared texture
    ID3D11Texture2D* bb11 = nullptr;
    HRESULT hr = swap11->GetBuffer(0, IID_PPV_ARGS(&bb11));
    if (FAILED(hr) || !bb11) return false;

    m_ctx11->CopyResource(m_sharedTex11, bb11);
    bb11->Release();
    m_ctx11->Flush();

    // 2. In D3D12, copy shared texture to current D3D12 backbuffer
    UINT idx = m_swap3_12 ? m_swap3_12->GetCurrentBackBufferIndex() : 0;
    ID3D12Resource* bb12 = m_backbuffers12[idx];

    m_alloc12->Reset();
    m_cl12->Reset(m_alloc12, nullptr);

    D3D12_RESOURCE_BARRIER barriers[2] = {};
    barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[0].Transition.pResource = bb12;
    barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[1].Transition.pResource = m_sharedTex12;
    barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_cl12->ResourceBarrier(2, barriers);

    m_cl12->CopyResource(bb12, m_sharedTex12);

    barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;

    barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    m_cl12->ResourceBarrier(2, barriers);

    m_cl12->Close();

    ID3D12CommandList* lists[] = { m_cl12 };
    m_cq12->ExecuteCommandLists(1, lists);

    m_fenceVal12++;
    m_cq12->Signal(m_fence12, m_fenceVal12);
    if (m_fence12->GetCompletedValue() < m_fenceVal12) {
        m_fence12->SetEventOnCompletion(m_fenceVal12, m_fenceEvent12);
        WaitForSingleObject(m_fenceEvent12, INFINITE);
    }

    // 3. Present on D3D12 Shadow SwapChain (triggers NvPresent64 cuGraphLaunch & frame interpolation!)
    hr = m_swap12->Present(sync, flags);
    return SUCCEEDED(hr);
}

} // namespace sm86
