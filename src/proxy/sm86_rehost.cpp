// ============================================================================
//  sm86_rehost.cpp - Production Version.dll Proxy for Road 1 (NvPresent64)
//  Re-hosting NVIDIA Smooth Motion (DLSS Frame Generation) on RTX 30-series (sm_86).
//
//  How it works when placed in a game folder as version.dll:
//    1. Forwards all 17 Version.dll API calls to C:\Windows\System32\version.dll.
//    2. On attach (via private startup thread to avoid loader lock):
//       - Finds and loads NvPresent64.dll from process or DriverStore.
//       - Patches gate: cmp [rcx+0x14], 2 (allow Tier 2) + mov sil, 1; nop (enable VFI).
//       - Hooks NvPresent64.dll IAT: cuModuleLoadData (RVA 0x1d2820) dynamically
//         rewrites 19 FP16 fatbinaries from sm_89/sm_120 to sm_86 on the fly.
//       - Opens global config gate [0x7d7810] and calls NVP_Init_D3D().
//       - NvPresent64.dll detours DXGI CreateSwapChainForHwnd.
//    3. Intercepts DXGI Present:
//       - Checks if swapchain is wrapped by NvPresent64 (proxy COM object).
//       - Automatically enables Smooth Motion on internal wrapper (vt[19]=1, vt[20]=1).
//       - Calls original Present, which dispatches optical flow & cuGraphLaunch!
// ============================================================================
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winternl.h>
#include <d3d12.h>
#include <d3d11.h>
#include <dxgi1_4.h>

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <set>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "d3d11.lib")

// ---- forward the 17 version.dll exports to the real System32 DLL ----
#pragma comment(linker, "/export:GetFileVersionInfoA=C:////Windows////System32////version.dll.GetFileVersionInfoA")
#pragma comment(linker, "/export:GetFileVersionInfoByHandle=C:////Windows////System32////version.dll.GetFileVersionInfoByHandle")
#pragma comment(linker, "/export:GetFileVersionInfoExA=C:////Windows////System32////version.dll.GetFileVersionInfoExA")
#pragma comment(linker, "/export:GetFileVersionInfoExW=C:////Windows////System32////version.dll.GetFileVersionInfoExW")
#pragma comment(linker, "/export:GetFileVersionInfoSizeA=C:////Windows////System32////version.dll.GetFileVersionInfoSizeA")
#pragma comment(linker, "/export:GetFileVersionInfoSizeExA=C:////Windows////System32////version.dll.GetFileVersionInfoSizeExA")
#pragma comment(linker, "/export:GetFileVersionInfoSizeExW=C:////Windows////System32////version.dll.GetFileVersionInfoSizeExW")
#pragma comment(linker, "/export:GetFileVersionInfoSizeW=C:////Windows////System32////version.dll.GetFileVersionInfoSizeW")
#pragma comment(linker, "/export:GetFileVersionInfoW=C:////Windows////System32////version.dll.GetFileVersionInfoW")
#pragma comment(linker, "/export:VerFindFileA=C:////Windows////System32////version.dll.VerFindFileA")
#pragma comment(linker, "/export:VerFindFileW=C:////Windows////System32////version.dll.VerFindFileW")
#pragma comment(linker, "/export:VerInstallFileA=C:////Windows////System32////version.dll.VerInstallFileA")
#pragma comment(linker, "/export:VerInstallFileW=C:////Windows////System32////version.dll.VerInstallFileW")
#pragma comment(linker, "/export:VerLanguageNameA=C:////Windows////System32////version.dll.VerLanguageNameA")
#pragma comment(linker, "/export:VerLanguageNameW=C:////Windows////System32////version.dll.VerLanguageNameW")
#pragma comment(linker, "/export:VerQueryValueA=C:////Windows////System32////version.dll.VerQueryValueA")
#pragma comment(linker, "/export:VerQueryValueW=C:////Windows////System32////version.dll.VerQueryValueW")

#include "pe_scan.h"
#include "ui_mask.h"
#include "osd_overlay.h"
#include "d3d11_to_d3d12_bridge.h"
#include "early_logger.h"

static const uint32_t FATBIN_MAGIC      = 0xba55ed50;

static void LogBridge(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    sm86::EarlyLogger::Instance().LogV(false, "INFO", fmt, args);
    va_end(args);
}

static HMODULE g_nvpresent = nullptr;
static HANDLE g_startupEvent = nullptr;
static CRITICAL_SECTION g_cs;
static std::set<void*> g_activatedWrappers;
static IDXGIOutput* g_cachedOutput = nullptr;
static HMONITOR g_cachedMonitor = nullptr;

// ---------------------------------------------------------------------------
// In-Memory Patch Helpers
// ---------------------------------------------------------------------------
static bool patch_bytes(uint8_t* p, const uint8_t* pat, size_t n) {
    DWORD old = 0;
    if (!VirtualProtect(p, n, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(p, pat, n);
    VirtualProtect(p, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, n);
    return true;
}

static void patch_fatbin(uint8_t* b, size_t size) {
    for (size_t off = 0; off + 3 < size; off += 4)
        if ((b[off] == 0x78 || b[off] == 0x59) && b[off+1] == 0 && b[off+2] == 0 && b[off+3] == 0)
            b[off] = 0x56;
    for (size_t off = 0; off + 0x34 < size; ++off)
        if (b[off] == 0x7f && b[off+1] == 'E' && b[off+2] == 'L' && b[off+3] == 'F') {
            uint32_t fl = 0x560556;
            memcpy(b + off + 0x30, &fl, 4);
        }
}

// ---------------------------------------------------------------------------
// cuModuleLoadData IAT Hook
// ---------------------------------------------------------------------------
typedef int (__stdcall *cuModuleLoadData_t)(void** module, const void* image);
static cuModuleLoadData_t g_realModuleLoadData = nullptr;

static int __stdcall hook_cuModuleLoadData(void** module, const void* image) {
    const uint8_t* p = (const uint8_t*)image;
    if (p && *(const uint32_t*)p == FATBIN_MAGIC) {
        uint64_t size = *(const uint64_t*)(p + 8);
        PROXY_MILESTONE(7, "FATBIN", "Fatbin patch applied: rewrote %llu bytes to sm_86 (magic=0x%08X)\n", size, FATBIN_MAGIC);
        static uint8_t buf[8 * 1024 * 1024];
        if (size < sizeof(buf)) {
            memcpy(buf, p, (size_t)size);
            patch_fatbin(buf, (size_t)size);
            p = buf;
        }
    }
    return g_realModuleLoadData(module, p);
}

typedef int (__stdcall *cuGraphLaunch_t)(void*, void*);
static cuGraphLaunch_t g_realGraphLaunch = nullptr;
static volatile long g_graphLaunchCount = 0;

static int __stdcall hook_cuGraphLaunch(void* gExec, void* stream) {
    long count = InterlockedIncrement(&g_graphLaunchCount);
    if (count <= 10 || count % 60 == 0) {
        PROXY_LOG("[sm86_rehost] cuGraphLaunch #%ld (Ampere FP16 HMMA Tensor Cores Active!)\n", count);
    }
    return g_realGraphLaunch(gExec, stream);
}

typedef bool (*pfnInitD3D)(void);


// ---------------------------------------------------------------------------
// DXGI SwapChain Hooking & Lifecycle Management
// ---------------------------------------------------------------------------
typedef HRESULT (STDMETHODCALLTYPE *Present_t)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE *Present1_t)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
typedef HRESULT (STDMETHODCALLTYPE *ResizeBuffers_t)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
typedef HRESULT (STDMETHODCALLTYPE *ResizeBuffers1_t)(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT, const UINT*, IUnknown* const*);

static Present_t        g_origPresent        = nullptr;
static Present1_t       g_origPresent1       = nullptr;
static ResizeBuffers_t  g_origResizeBuffers  = nullptr;
static ResizeBuffers1_t g_origResizeBuffers1 = nullptr;

static void InvalidateSwapChainState(IDXGISwapChain* swap);

// Safe SwapChain wrapper activation (R1)
static void ActivateSmoothMotionIfWrapped(IDXGISwapChain* swap) {
    if (!swap) return;

    void* wrapper = nullptr;
    uintptr_t nvBase = (uintptr_t)g_nvpresent;
    bool isNvPresent = InspectNvPresentSwapChain(swap, nvBase, &wrapper);

    static std::set<void*> s_loggedSwaps;
    bool needLogInspect = false;
    EnterCriticalSection(&g_cs);
    if (s_loggedSwaps.find(swap) == s_loggedSwaps.end()) {
        s_loggedSwaps.insert(swap);
        needLogInspect = true;
    }
    LeaveCriticalSection(&g_cs);

    if (needLogInspect) {
        if (isNvPresent && wrapper) {
            PROXY_MILESTONE(11, "SWAP_INSPECT", "SwapChain %p verified as NvPresent proxy (wrapper @ %p)\n", swap, wrapper);
        } else {
            void* swapVtbl = nullptr;
            SafeReadPointer(swap, &swapVtbl);
            PROXY_MILESTONE(11, "SWAP_INSPECT", "SwapChain %p is native DXGI or interposer (vtable=%p), passthrough active\n", swap, swapVtbl);
        }
    }

    if (!isNvPresent || !wrapper) {
        // Safe passthrough: native DXGI or Streamline/Reflex interposer
        return;
    }

    EnterCriticalSection(&g_cs);
    if (g_activatedWrappers.find(wrapper) == g_activatedWrappers.end()) {
        g_activatedWrappers.insert(wrapper);
        LeaveCriticalSection(&g_cs);

        void** vt = *(void***)wrapper;
        typedef void (*pfnSetByte)(void*, uint8_t);
        ((pfnSetByte)vt[19])(wrapper, 1);
        ((pfnSetByte)vt[20])(wrapper, 1);
        PROXY_MILESTONE(12, "SMOOTH_MOTION", "Activated Smooth Motion on wrapper @ %p: vt[19]=1 (Enable), vt[20]=1 (Mode)\n", wrapper);
    } else {
        LeaveCriticalSection(&g_cs);
    }
}

// VBlank Pacing between dual presents (R3)
static void PaceVBlank(IDXGISwapChain* swap) {
    if (!swap) return;
    HWND hwnd = nullptr;
    DXGI_SWAP_CHAIN_DESC sd = {};
    if (SUCCEEDED(swap->GetDesc(&sd))) {
        hwnd = sd.OutputWindow;
    }
    if (hwnd && IsIconic(hwnd)) return;

    HMONITOR curMon = hwnd ? MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL) : nullptr;
    if (curMon && curMon != g_cachedMonitor) {
        if (g_cachedOutput) {
            g_cachedOutput->Release();
            g_cachedOutput = nullptr;
        }
        g_cachedMonitor = curMon;
    }

    if (!g_cachedOutput) {
        HRESULT hr = swap->GetContainingOutput(&g_cachedOutput);
        if (FAILED(hr)) g_cachedOutput = nullptr;
    }

    if (g_cachedOutput) {
        HRESULT hrVb = g_cachedOutput->WaitForVBlank();
        if (FAILED(hrVb)) {
            g_cachedOutput->Release();
            g_cachedOutput = nullptr;
            g_cachedMonitor = nullptr;
        }
    }
}

static sm86::OsdOverlay        g_osd;
static sm86::UiMaskEngine      g_uiMask;
static sm86::UiMaskConfig      g_uiMaskCfg;
static ID3D12Device*           g_d3d12Dev    = nullptr;
static ID3D12CommandQueue*     g_cmdQueue    = nullptr;
static ID3D12CommandAllocator* g_osdAlloc    = nullptr;
static ID3D12GraphicsCommandList* g_osdCl    = nullptr;
static bool                    g_d3d12Inited = false;
static ID3D11Device*           g_d3d11Dev    = nullptr;
static bool                    g_d3d11Inited = false;
static sm86::D3D11ToD3D12Bridge g_bridge;

// SwapChain ResizeBuffers Lifecycle Invalidation (R4)
static void InvalidateSwapChainState(IDXGISwapChain* swap) {
    EnterCriticalSection(&g_cs);
    if (g_cachedOutput) {
        g_cachedOutput->Release();
        g_cachedOutput = nullptr;
    }
    g_cachedMonitor = nullptr;
    g_activatedWrappers.clear();
    LeaveCriticalSection(&g_cs);

    if (g_bridge.IsActive()) {
        g_bridge.Shutdown();
    }
}

static thread_local bool t_inResize = false;

struct ResizeReentrancyGuard {
    bool& flag;
    ResizeReentrancyGuard(bool& f) : flag(f) { flag = true; }
    ~ResizeReentrancyGuard() { flag = false; }
};

static HRESULT STDMETHODCALLTYPE HookedResizeBuffers(
    IDXGISwapChain* swap,
    UINT BufferCount,
    UINT Width,
    UINT Height,
    DXGI_FORMAT NewFormat,
    UINT SwapChainFlags)
{
    if (t_inResize) {
        return g_origResizeBuffers ? g_origResizeBuffers(swap, BufferCount, Width, Height, NewFormat, SwapChainFlags) : E_FAIL;
    }
    ResizeReentrancyGuard guard(t_inResize);

    PROXY_LOG("[RESIZE] HookedResizeBuffers entry: swap=%p (caller=%p), %ux%u, bufCount=%u, fmt=%d, flags=0x%08X\n",
              swap, _ReturnAddress(), Width, Height, BufferCount, (int)NewFormat, SwapChainFlags);
    InvalidateSwapChainState(swap);
    HRESULT hr = g_origResizeBuffers ? g_origResizeBuffers(swap, BufferCount, Width, Height, NewFormat, SwapChainFlags) : E_FAIL;
    PROXY_LOG("[RESIZE] HookedResizeBuffers exit: swap=%p (caller=%p), hr=0x%08X\n", swap, _ReturnAddress(), (uint32_t)hr);
    return hr;
}

static HRESULT STDMETHODCALLTYPE HookedResizeBuffers1(
    IDXGISwapChain3* swap,
    UINT BufferCount,
    UINT Width,
    UINT Height,
    DXGI_FORMAT NewFormat,
    UINT SwapChainFlags,
    const UINT* pCreationNodeMask,
    IUnknown* const* ppPresentQueue)
{
    if (t_inResize) {
        return g_origResizeBuffers1 ? g_origResizeBuffers1(swap, BufferCount, Width, Height, NewFormat, SwapChainFlags, pCreationNodeMask, ppPresentQueue) : E_FAIL;
    }
    ResizeReentrancyGuard guard(t_inResize);

    PROXY_LOG("[RESIZE] HookedResizeBuffers1 entry: swap=%p, %ux%u, bufCount=%u, fmt=%d, flags=0x%08X\n",
              swap, Width, Height, BufferCount, (int)NewFormat, SwapChainFlags);
    InvalidateSwapChainState(swap);
    HRESULT hr = g_origResizeBuffers1 ? g_origResizeBuffers1(swap, BufferCount, Width, Height, NewFormat, SwapChainFlags, pCreationNodeMask, ppPresentQueue) : E_FAIL;
    PROXY_LOG("[RESIZE] HookedResizeBuffers1 exit: hr=0x%08X\n", (uint32_t)hr);
    return hr;
}

// Defined further down; needed here so a bypassed run never even creates the
// shadow swapchain (otherwise it still claims the frame-generation slot and the
// "direct D3D11 swapchain" test is meaningless).
static bool BridgeBypassed();

static void EnsureOverlay(IDXGISwapChain* swap) {
    if (!swap) return;

    IUnknown* devObj = nullptr;
    if (FAILED(swap->GetDevice(IID_PPV_ARGS(&devObj)))) return;

    ID3D12CommandQueue* q = nullptr;
    ID3D12Device* dev12 = nullptr;

    if (SUCCEEDED(devObj->QueryInterface(IID_PPV_ARGS(&q)))) {
        g_cmdQueue = q;
        q->GetDevice(IID_PPV_ARGS(&dev12));
        g_d3d12Dev = dev12;
        if (!g_d3d12Inited && g_d3d12Dev && g_cmdQueue) {
            if (SUCCEEDED(g_d3d12Dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_osdAlloc)))) {
                if (SUCCEEDED(g_d3d12Dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_osdAlloc, nullptr, IID_PPV_ARGS(&g_osdCl)))) {
                    g_osdCl->Close();
                    g_osd.Initialize(g_d3d12Dev, g_cmdQueue);
                    g_uiMask.Initialize(g_d3d12Dev);
                    g_d3d12Inited = true;
                    LogBridge("[sm86_rehost] D3D12 OSD & UI Mask Protection initialized on swapchain\n");
                }
            }
        }
    } else {
        ID3D11Device* dev11 = nullptr;
        if (SUCCEEDED(devObj->QueryInterface(IID_PPV_ARGS(&dev11)))) {
            if (dev11 != g_d3d11Dev) {
                LogBridge("[sm86_rehost] D3D11 Device changed (old=%p, new=%p), reinitializing overlay & bridge\n",
                          g_d3d11Dev, dev11);
                if (g_d3d11Dev) {
                    g_bridge.Shutdown();
                    g_osd.ShutdownD3D11();
                    g_d3d11Dev->Release();
                    g_d3d11Dev = nullptr;
                    g_d3d11Inited = false;
                }
                g_d3d11Dev = dev11;
                if (g_osd.InitializeD3D11(g_d3d11Dev)) {
                    g_d3d11Inited = true;
                    LogBridge("[sm86_rehost] D3D11 OSD Overlay initialized on device %p\n", g_d3d11Dev);
                }
            } else {
                dev11->Release();
            }
        }
    }
    devObj->Release();
}

static void ProcessOverlayAndUiMask(IDXGISwapChain* swap) {
    if (!swap) return;
    if (g_startupEvent) {
        WaitForSingleObject(g_startupEvent, 2000);
    }
    EnsureOverlay(swap);

    DXGI_SWAP_CHAIN_DESC sd = {};
    swap->GetDesc(&sd);

    HWND hwnd = sd.OutputWindow;
    if (!hwnd) {
        IDXGISwapChain1* sc1 = nullptr;
        if (SUCCEEDED(swap->QueryInterface(IID_PPV_ARGS(&sc1)))) {
            sc1->GetHwnd(&hwnd);
            sc1->Release();
        }
    }
    if (!hwnd) {
        hwnd = GetActiveWindow();
        if (!hwnd) hwnd = GetForegroundWindow();
    }

    static bool s_sdLogged = false;
    if (!s_sdLogged && hwnd) {
        s_sdLogged = true;
        LogBridge("[sm86_rehost] swap11 Desc: BufferCount=%u, SwapEffect=%u, Flags=0x%08X, Windowed=%d, HWND=%p\n",
                  sd.BufferCount, sd.SwapEffect, sd.Flags, sd.Windowed, hwnd);
        LONG_PTR pStyle = GetWindowLongPtrA(hwnd, GWL_STYLE);
        LONG_PTR pExStyle = GetWindowLongPtrA(hwnd, GWL_EXSTYLE);
        LogBridge("[sm86_rehost] OutputWindow Styles: Style=0x%08llX, ExStyle=0x%08llX\n",
                  (uint64_t)pStyle, (uint64_t)pExStyle);
        IDXGISwapChain3* sc3 = nullptr;
        if (SUCCEEDED(swap->QueryInterface(IID_PPV_ARGS(&sc3)))) {
            LogBridge("[sm86_rehost] swap11 supports IDXGISwapChain3! CurBackBufferIndex=%u\n",
                      sc3->GetCurrentBackBufferIndex());
            sc3->Release();
        } else {
            LogBridge("[sm86_rehost] swap11 does NOT support IDXGISwapChain3\n");
        }
    }

    uint32_t width = sd.BufferDesc.Width;
    uint32_t height = sd.BufferDesc.Height;
    DXGI_FORMAT format = sd.BufferDesc.Format;

    if (g_d3d11Dev) {
        ID3D11Texture2D* bb = nullptr;
        if (SUCCEEDED(swap->GetBuffer(0, IID_PPV_ARGS(&bb)))) {
            D3D11_TEXTURE2D_DESC bbDesc = {};
            bb->GetDesc(&bbDesc);
            width = bbDesc.Width;
            height = bbDesc.Height;
            format = bbDesc.Format;
            bb->Release();
        }

        static ULONGLONG s_lastFailTick = 0;
        if (width >= 480 && height >= 480 && hwnd && !BridgeBypassed()) {
            bool needInit = !g_bridge.IsActive() || g_bridge.GetWidth() != width || g_bridge.GetHeight() != height || g_bridge.GetHwnd() != hwnd || g_bridge.GetFormat() != format;
            ULONGLONG now = GetTickCount64();
            if (needInit && (g_bridge.IsActive() || now - s_lastFailTick >= 2000)) {
                LogBridge("[sm86_rehost] Triggering Bridge Init: hwnd=%p, res=%ux%u, fmt=%d\n",
                          hwnd, width, height, (int)format);
                if (!g_bridge.Initialize(g_d3d11Dev, hwnd, width, height, format)) {
                    s_lastFailTick = now;
                }
            }
        } else {
            static int s_skipCount = 0;
            if (s_skipCount++ % 60 == 0) {
                LogBridge("[sm86_rehost] Skipping Bridge Init: hwnd=%p, res=%ux%u (need >=480), dev11=%p\n",
                          hwnd, width, height, g_d3d11Dev);
            }
        }
    }

    bool bridgeActive = g_bridge.IsActive();
    bool fgActive = (g_d3d12Inited && (!g_activatedWrappers.empty())) || bridgeActive;

    const char* engineTitle = bridgeActive
        ? "Road 1 (NvPresent64 D3D11 Bridge) - FP16 HMMA"
        : (g_d3d11Inited ? "MPC-HC Video (D3D11 Passthrough)" : "Road 1 (NvPresent64 Rehost) - FP16 HMMA");

    // Update telemetry and check hotkeys (F11=OSD, F10=UI Mask, F9=Heatmap)
    g_osd.Update(0.62f, engineTitle, g_uiMaskCfg.enabled, g_uiMaskCfg.debugHeatmap, fgActive);
    g_uiMaskCfg.enabled = g_osd.IsUiMaskEnabled();
    g_uiMaskCfg.debugHeatmap = g_osd.IsDebugHeatmap();

    if (!g_osd.IsVisible()) return;

    if (g_d3d11Inited) {
        g_osd.RenderD3D11(swap);
        return;
    }


    if (!g_d3d12Inited || !g_cmdQueue) return;

    IDXGISwapChain3* sc3 = nullptr;
    if (SUCCEEDED(swap->QueryInterface(IID_PPV_ARGS(&sc3)))) {
        UINT idx = sc3->GetCurrentBackBufferIndex();
        ID3D12Resource* bb = nullptr;
        if (SUCCEEDED(swap->GetBuffer(idx, IID_PPV_ARGS(&bb)))) {
            DXGI_SWAP_CHAIN_DESC desc = {};
            swap->GetDesc(&desc);

            g_osdAlloc->Reset();
            g_osdCl->Reset(g_osdAlloc, nullptr);

            D3D12_RESOURCE_BARRIER b = {};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = bb;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            g_osdCl->ResourceBarrier(1, &b);

            g_osd.Record(g_osdCl, bb, desc.BufferDesc.Width, desc.BufferDesc.Height);

            b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
            g_osdCl->ResourceBarrier(1, &b);

            g_osdCl->Close();
            ID3D12CommandList* lists[] = { g_osdCl };
            g_cmdQueue->ExecuteCommandLists(1, lists);

            bb->Release();
        }
        sc3->Release();
    }
}

static thread_local bool t_inBridgePresent = false;

// Structural A/B switch. SM86_NO_BRIDGE=1 bypasses the whole shadow swapchain
// bridge so the application presents its own (NvPresent64-wrapped) swapchain
// directly. This is the decisive test for the slow-motion finding: if the
// mid-scan tearing disappears, it comes from having a second window / second
// flip swapchain, not from frame generation itself. Watch sm86_debug.log for
// whether cuGraphLaunch still fires on the direct path.
static bool BridgeBypassed() {
    static int cached = -1;
    if (cached < 0) {
        char buf[16] = {};
        DWORD n = GetEnvironmentVariableA("SM86_NO_BRIDGE", buf, sizeof(buf));
        cached = (n > 0 && buf[0] == '1') ? 1 : 0;
    }
    return cached == 1;
}

static void LogBypassOnce() {
    static bool logged = false;
    if (!logged) {
        logged = true;
        LogBridge("[sm86_rehost] SM86_NO_BRIDGE=1: shadow bridge bypassed, app swapchain presents directly\n");
    }
}

static HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain* swap, UINT sync, UINT flags) {
    if (t_inBridgePresent || (flags & DXGI_PRESENT_TEST)) {
        return g_origPresent(swap, sync, flags);
    }

    ActivateSmoothMotionIfWrapped(swap);
    ProcessOverlayAndUiMask(swap);

    if (g_bridge.IsActive() && !BridgeBypassed()) {
        t_inBridgePresent = true;
        g_bridge.Present(swap, sync, flags);
        t_inBridgePresent = false;
        static int s_presCount = 0;
        s_presCount++;
        if (s_presCount <= 10 || s_presCount % 60 == 0) {
            PROXY_MILESTONE(13, "PRESENT", "HookedPresent #%d: sync=%u, flags=0x%08X (Bridge Active)\n", s_presCount, sync, flags);
        }
        return S_OK;
    }
    if (g_bridge.IsActive()) LogBypassOnce();
    HRESULT hr = g_origPresent(swap, sync, flags);
    static int s_presCount = 0;
    s_presCount++;
    if (s_presCount <= 10 || s_presCount % 60 == 0) {
        PROXY_MILESTONE(13, "PRESENT", "HookedPresent #%d: sync=%u, flags=0x%08X -> hr=0x%08X (graphs=%ld)\n", s_presCount, sync, flags, (uint32_t)hr, g_graphLaunchCount);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE HookedPresent1(IDXGISwapChain1* swap, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* p) {
    if (t_inBridgePresent || (flags & DXGI_PRESENT_TEST)) {
        return g_origPresent1(swap, sync, flags, p);
    }

    ActivateSmoothMotionIfWrapped(swap);
    ProcessOverlayAndUiMask(swap);

    if (g_bridge.IsActive() && !BridgeBypassed()) {
        t_inBridgePresent = true;
        g_bridge.Present(swap, sync, flags);
        t_inBridgePresent = false;
        static int s_pres1Count = 0;
        if (++s_pres1Count % 120 == 1) {
            PROXY_MILESTONE(13, "PRESENT", "HookedPresent1 #%d: sync=%u, flags=0x%08X (Bridge Active)\n", s_pres1Count, sync, flags);
        }
        return S_OK;
    }
    if (g_bridge.IsActive()) LogBypassOnce();
    HRESULT hr = g_origPresent1(swap, sync, flags, p);
    static int s_pres1Count = 0;
    if (++s_pres1Count % 120 == 1) {
        PROXY_MILESTONE(13, "PRESENT", "HookedPresent1 #%d: sync=%u, flags=0x%08X -> hr=0x%08X\n", s_pres1Count, sync, flags, (uint32_t)hr);
    }
    return hr;
}

static void InstallDxgiHooks() {
    WNDCLASSA wc = {};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "sm86_dummy_cls";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowA("sm86_dummy_cls", "", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);

    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 64;
    sd.BufferDesc.Height = 64;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD; // Flip-model swapchain for modern DXGI hooks
    sd.Windowed = TRUE;

    IDXGISwapChain* sc = nullptr;
    ID3D11Device* dev11 = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL lvl = D3D_FEATURE_LEVEL_11_0;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                               nullptr, 0, D3D11_SDK_VERSION, &sd, &sc,
                                               &dev11, &lvl, &ctx);
    if (FAILED(hr) || !sc) {
        // Fallback to WARP if hardware adapter unavailable
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
                                           nullptr, 0, D3D11_SDK_VERSION, &sd, &sc,
                                           &dev11, &lvl, &ctx);
    }
    if (FAILED(hr) || !sc) {
        // Fallback to DISCARD if FLIP_DISCARD is not supported
        sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                           nullptr, 0, D3D11_SDK_VERSION, &sd, &sc,
                                           &dev11, &lvl, &ctx);
        if (FAILED(hr) || !sc) {
            hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
                                               nullptr, 0, D3D11_SDK_VERSION, &sd, &sc,
                                               &dev11, &lvl, &ctx);
        }
    }

    if (SUCCEEDED(hr) && sc) {
        IDXGISwapChain1* sc1 = nullptr;
        if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&sc1)))) {
            void** vt = *(void***)sc1;
            DWORD oldProt = 0;

            // Slot 8: Present
            if (vt[8] != (void*)&HookedPresent) {
                VirtualProtect(&vt[8], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProt);
                g_origPresent = (Present_t)vt[8];
                vt[8] = (void*)&HookedPresent;
                VirtualProtect(&vt[8], sizeof(void*), oldProt, &oldProt);
            }

            // Slot 13: ResizeBuffers (R4)
            if (vt[13] != (void*)&HookedResizeBuffers) {
                VirtualProtect(&vt[13], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProt);
                g_origResizeBuffers = (ResizeBuffers_t)vt[13];
                vt[13] = (void*)&HookedResizeBuffers;
                VirtualProtect(&vt[13], sizeof(void*), oldProt, &oldProt);
            }

            // Slot 22: Present1
            if (vt[22] != (void*)&HookedPresent1) {
                VirtualProtect(&vt[22], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProt);
                g_origPresent1 = (Present1_t)vt[22];
                vt[22] = (void*)&HookedPresent1;
                VirtualProtect(&vt[22], sizeof(void*), oldProt, &oldProt);
            }

            PROXY_MILESTONE(9, "DXGI_HOOK", "DXGI Present hooks installed: Present(slot 8)=%p, Present1(slot 22)=%p\n", (void*)g_origPresent, (void*)g_origPresent1);
            PROXY_MILESTONE(10, "DXGI_HOOK", "DXGI ResizeBuffers hook installed (slot 13)=%p\n", (void*)g_origResizeBuffers);

            // Slot 39: ResizeBuffers1 on IDXGISwapChain3 (R4)
            IDXGISwapChain3* sc3 = nullptr;
            if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&sc3)))) {
                void** vt3 = *(void***)sc3;
                if (vt3[39] != (void*)&HookedResizeBuffers1) {
                    VirtualProtect(&vt3[39], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProt);
                    g_origResizeBuffers1 = (ResizeBuffers1_t)vt3[39];
                    vt3[39] = (void*)&HookedResizeBuffers1;
                    VirtualProtect(&vt3[39], sizeof(void*), oldProt, &oldProt);
                    PROXY_LOG("[sm86_rehost] DXGI ResizeBuffers1 hook installed (slot 39)=%p\n", (void*)g_origResizeBuffers1);
                }
                sc3->Release();
            }

            sc1->Release();
        }
        sc->Release();
        if (ctx) ctx->Release();
        if (dev11) dev11->Release();
    }
    DestroyWindow(hwnd);
}

// ---------------------------------------------------------------------------
// Main Patch Engine
// ---------------------------------------------------------------------------
static bool ApplyRehost(HMODULE nv) {
    PROXY_LOG("[sm86_rehost] Initializing NvPresent64 rehost @ %p\n", nv);

    // 1. Dynamic Dual-Gate Patch (Pattern Scanner)
    uint8_t* cmpImm = nullptr;
    uint8_t* setgePtr = nullptr;
    uint32_t setgeLen = 0;
    if (sm86::LocateGateAddresses(nv, &cmpImm, &setgePtr, &setgeLen)) {
        PROXY_LOG("[sm86_rehost] Gate located via Pattern Scan (cmp imm RVA +0x%lx, setge RVA +0x%lx, len %u)\n",
               (uint32_t)(cmpImm - (uint8_t*)nv), (uint32_t)(setgePtr - (uint8_t*)nv), setgeLen);
    } else {
        PROXY_LOG("[sm86_rehost] WARNING: Gate pattern scan failed! Trying fallback RVAs (0xc41f, 0xc437)...\n");
        cmpImm = (uint8_t*)nv + 0xc41f;
        setgePtr = (uint8_t*)nv + 0xc437;
        setgeLen = 4;
    }

    uint8_t two = 0x02;
    if (!patch_bytes(cmpImm, &two, 1)) {
        PROXY_LOG("[sm86_rehost] FAILED to patch cmpImm\n");
        return false;
    }

    if (setgeLen == 4) {
        static const uint8_t movSil1[] = { 0x40, 0xB6, 0x01, 0x90 }; // mov sil, 1; nop
        if (!patch_bytes(setgePtr, movSil1, 4)) {
            PROXY_LOG("[sm86_rehost] FAILED to patch setgePtr\n");
            return false;
        }
    } else {
        static const uint8_t movSil1_3[] = { 0xB6, 0x01, 0x90 }; // mov sil, 1; nop
        if (!patch_bytes(setgePtr, movSil1_3, 3)) {
            PROXY_LOG("[sm86_rehost] FAILED to patch setgePtr\n");
            return false;
        }
    }
    PROXY_MILESTONE(5, "GATE_PATCH", "Dual-gate patch applied: Tier 2 allowed (0x02), sil=1 forced (len=%u)\n", setgeLen);

    // 2. Dynamic IAT Hook for cuModuleLoadData (PE Import Directory Walker)
    void** iat_load = sm86::FindIATEntry(nv, "nvcuda.dll", "cuModuleLoadData");
    if (iat_load) {
        PROXY_LOG("[sm86_rehost] IAT cuModuleLoadData found dynamically @ %p (RVA +0x%lx)\n",
               iat_load, (uint32_t)((uint8_t*)iat_load - (uint8_t*)nv));
    } else {
        PROXY_LOG("[sm86_rehost] WARNING: Dynamic IAT walk failed! Trying fallback RVA 0x1d2820...\n");
        iat_load = (void**)((uint8_t*)nv + 0x1d2820);
    }

    DWORD oldProt = 0;
    if (VirtualProtect(iat_load, sizeof(void*), PAGE_READWRITE, &oldProt)) {
        g_realModuleLoadData = (cuModuleLoadData_t)*iat_load;
        *iat_load = (void*)&hook_cuModuleLoadData;
        VirtualProtect(iat_load, sizeof(void*), oldProt, &oldProt);
        PROXY_LOG("[sm86_rehost] IAT cuModuleLoadData hooked (original @ %p)\n", g_realModuleLoadData);
    } else {
        PROXY_LOG("[sm86_rehost] FAILED to hook IAT cuModuleLoadData\n");
        return false;
    }

    void** iat_graph = sm86::FindIATEntry(nv, "nvcuda.dll", "cuGraphLaunch");
    if (!iat_graph) iat_graph = (void**)((uint8_t*)nv + 0x1d2780);
    if (iat_graph && VirtualProtect(iat_graph, sizeof(void*), PAGE_READWRITE, &oldProt)) {
        g_realGraphLaunch = (cuGraphLaunch_t)*iat_graph;
        *iat_graph = (void*)&hook_cuGraphLaunch;
        VirtualProtect(iat_graph, sizeof(void*), oldProt, &oldProt);
        PROXY_LOG("[sm86_rehost] IAT cuGraphLaunch hooked (original @ %p)\n", g_realGraphLaunch);
    }
    PROXY_MILESTONE(6, "IAT_HOOK", "Dynamic IAT hooks installed: cuModuleLoadData=%p (orig=%p), cuGraphLaunch=%p (orig=%p)\n",
                    (void*)hook_cuModuleLoadData, (void*)g_realModuleLoadData, (void*)hook_cuGraphLaunch, (void*)g_realGraphLaunch);

    // 3. Dynamic Global Config Struct Resolution (RIP-Relative Dissection)
    uint8_t* S = sm86::ResolveConfigStructFromInit(nv);
    if (S) {
        PROXY_LOG("[sm86_rehost] Config struct resolved dynamically from NVP_Init_D3D @ %p (RVA +0x%lx)\n",
               S, (uint32_t)(S - (uint8_t*)nv));
    } else {
        PROXY_LOG("[sm86_rehost] WARNING: Dynamic config resolution failed! Trying fallback RVA 0x7d7810...\n");
        S = (uint8_t*)nv + 0x7d7810;
    }

    S[0x4c] = 1; S[0xe8] = 1; S[0xe9] = 1; S[0x12a5] = 1;
    pfnInitD3D initD3D = (pfnInitD3D)GetProcAddress(nv, "NVP_Init_D3D");
    if (initD3D) {
        bool ok = initD3D();
        S[0x4c] = 1; S[0xe8] = 1; S[0xe9] = 1; S[0x12a5] = 1;
        PROXY_MILESTONE(8, "NVP_INIT", "Dynamic config resolved @ %p, NVP_Init_D3D() -> %s\n", S, ok ? "TRUE" : "FALSE");
    } else {
        PROXY_LOG("[sm86_rehost] WARNING: NVP_Init_D3D not found in NvPresent64.dll\n");
    }

    return true;
}

static DWORD WINAPI StartupThread(LPVOID) {
    HMODULE hSelf = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCSTR)StartupThread, &hSelf);

    PROXY_MILESTONE(3, "STARTUP", "StartupThread worker thread started (TID=%lu)\n", GetCurrentThreadId());
    g_nvpresent = sm86::LoadNvPresent();
    PROXY_MILESTONE(4, "NVP_LOAD", "LoadNvPresent resolved module base: %p\n", g_nvpresent);

    if (g_nvpresent) {
        bool ok = ApplyRehost(g_nvpresent);
        PROXY_LOG("[sm86_rehost] ApplyRehost() -> %s\n", ok ? "TRUE" : "FALSE");
        InstallDxgiHooks();
        PROXY_LOG("[sm86_rehost] Setup completed successfully!\n");
    } else {
        PROXY_LOG("[sm86_rehost] ERROR: Could not find NvPresent64.dll!\n");
    }
    if (g_startupEvent) SetEvent(g_startupEvent);

    if (hSelf) {
        FreeLibraryAndExitThread(hSelf, 0);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// PEB Process Name Spoofing & GetModuleFileName Hooking
// NVIDIA driver DRS (Driver Resource Settings) blacklists video players
// like mpc-hc64.exe / mpc-hc.exe from using NvPresent64 / DLSS Frame Gen.
// By spoofing ImagePathName in the PEB to "mpc_game.exe", NVIDIA driver
// treats the process as a 3D gaming app and allows NvPresent64 swapchain wrapping!
// Meanwhile, we preserve "mpc-hc64.exe" for the host application so all
// settings, configurations, and INI files remain 100% intact.
// ---------------------------------------------------------------------------
static wchar_t g_origExeName[MAX_PATH] = {};
static bool g_pebSpoofed = false;

static void SpoofPebProcessName() {
    uint8_t* peb = (uint8_t*)__readgsqword(0x60);
    if (!peb) return;
    uint8_t* params = *(uint8_t**)(peb + 0x20);
    if (!params) return;

    UNICODE_STRING* imgPath = (UNICODE_STRING*)(params + 0x60);
    if (!imgPath || !imgPath->Buffer) return;

    wchar_t* lastSlash = wcsrchr(imgPath->Buffer, L'\\');
    wchar_t* exeName = lastSlash ? (lastSlash + 1) : imgPath->Buffer;

    if (_wcsicmp(exeName, L"mpc-hc64.exe") == 0 || _wcsicmp(exeName, L"mpc-hc.exe") == 0) {
        wcsncpy_s(g_origExeName, exeName, _TRUNCATE);
        wcscpy_s(exeName, 16, L"mpc_game.exe");
        imgPath->Length = (USHORT)(wcslen(imgPath->Buffer) * sizeof(wchar_t));
        g_pebSpoofed = true;
        PROXY_MILESTONE(2, "PEB", "PEB ImagePath spoofed from %ls to %ls (bypassing NVIDIA driver video player blacklist)\n",
                        g_origExeName, exeName);
    } else {
        PROXY_MILESTONE(2, "PEB", "PEB process name check: %ls (no spoofing required)\n", exeName);
    }
}

typedef DWORD (WINAPI *GetModuleFileNameW_t)(HMODULE, LPWSTR, DWORD);
static GetModuleFileNameW_t g_origGetModuleFileNameW = nullptr;

static DWORD WINAPI HookedGetModuleFileNameW(HMODULE hModule, LPWSTR lpFilename, DWORD nSize) {
    DWORD res = g_origGetModuleFileNameW ? g_origGetModuleFileNameW(hModule, lpFilename, nSize)
                                         : GetModuleFileNameW(hModule, lpFilename, nSize);
    if (res > 0 && (hModule == nullptr || hModule == GetModuleHandleW(nullptr)) && g_pebSpoofed) {
        wchar_t* p = wcsrchr(lpFilename, L'\\');
        if (p && _wcsicmp(p + 1, L"mpc_game.exe") == 0 && g_origExeName[0] != L'\0') {
            wcscpy_s(p + 1, nSize - (p + 1 - lpFilename), g_origExeName);
            res = (DWORD)wcslen(lpFilename);
        }
    }
    return res;
}

static void InstallGetModuleFileNameHook() {
    HMODULE hExe = GetModuleHandleW(nullptr);
    if (!hExe) return;
    void** iat = sm86::FindIATEntry(hExe, "KERNEL32.dll", "GetModuleFileNameW");
    if (iat) {
        DWORD oldProt = 0;
        if (VirtualProtect(iat, sizeof(void*), PAGE_READWRITE, &oldProt)) {
            g_origGetModuleFileNameW = (GetModuleFileNameW_t)*iat;
            *iat = (void*)&HookedGetModuleFileNameW;
            VirtualProtect(iat, sizeof(void*), oldProt, &oldProt);
            PROXY_LOG("[sm86_rehost] Hooked main executable IAT GetModuleFileNameW -> %ls preserved for app\n",
                      g_origExeName);
        }
    }
}

static LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep) {
    HMODULE mod = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)ep->ExceptionRecord->ExceptionAddress, &mod);
    char modName[MAX_PATH] = {};
    GetModuleFileNameA(mod, modName, sizeof(modName));
    PROXY_LOG("[CRASH] Code=0x%08X at %p (Module: %s, offset 0x%tx)\n",
              ep->ExceptionRecord->ExceptionCode,
              ep->ExceptionRecord->ExceptionAddress,
              modName,
              (uint8_t*)ep->ExceptionRecord->ExceptionAddress - (uint8_t*)mod);
    return EXCEPTION_CONTINUE_SEARCH;
}

static void LogExit() {
    PROXY_LOG("[sm86_rehost] Process is exiting (atexit called)\n");
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID lpReserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        EarlyLog_Init(h);
        wchar_t exePath[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        PROXY_MILESTONE(1, "ATTACH", "DllMain DLL_PROCESS_ATTACH: PID=%lu, Process=%ls, Proxy=sm86_smooth v1.0 (version.dll)\n",
                        GetCurrentProcessId(), exePath);
        SetUnhandledExceptionFilter(CrashFilter);
        atexit(LogExit);
        SpoofPebProcessName();
        InstallGetModuleFileNameHook();
        InitializeCriticalSection(&g_cs);
        g_startupEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
        HANDLE thread = CreateThread(nullptr, 0, StartupThread, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
    } else if (reason == DLL_PROCESS_DETACH) {
        PROXY_MILESTONE(14, "DETACH", "DllMain DLL_PROCESS_DETACH (lpReserved=%p)\n", lpReserved);
        if (g_startupEvent) {
            CloseHandle(g_startupEvent);
            g_startupEvent = nullptr;
        }
        if (g_cachedOutput) {
            g_cachedOutput->Release();
            g_cachedOutput = nullptr;
        }
        g_cachedMonitor = nullptr;
        DeleteCriticalSection(&g_cs);
        EarlyLog_Shutdown();
    }
    return TRUE;
}

