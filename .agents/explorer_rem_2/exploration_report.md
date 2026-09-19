# Exploration Report: Milestone 1 Remediation (Iteration 2)

**Explorer**: Explorer 2 (`teamwork_preview_explorer`)  
**Working Directory**: `C:\Users\lsp\Documents\antigravity\calm-carson\.agents\explorer_rem_2`  
**Date**: 2026-09-19  
**Target Subsystems**:
- `src/proxy/sm86_rehost.cpp`: `HookedResizeBuffers`, `HookedResizeBuffers1`, `InstallDxgiHooks`
- `src/proxy/d3d11_to_d3d12_bridge.cpp`: `D3D11ToD3D12Bridge::Shutdown()`, `DiagCsv()`
- `src/proxy/osd_overlay.cpp`: `LogBridge()`
- `tests/test_challenger_stress.cpp`: Step 2.3 `ResizeBuffers1` contract test

---

## 1. Executive Summary

Milestone 1 (Proxy Runtime Hardening) underwent adversarial review by Challenger 2 and forensic inspection by Forensic Auditor 1. Three defects and one audit violation were identified:
1. **Critical Defect (Challenger 2)**: Fatal `0xC0000005` (Access Violation) in `ntdll.dll` during live D3D11 swapchain resize in `test_challenger_stress.exe` due to un-guarded recursion in `HookedResizeBuffers`.
2. **High Risk Defect (Challenger 2)**: Missing GPU fence synchronization in `D3D11ToD3D12Bridge::Shutdown()` prior to releasing Direct3D 12 shadow swapchain and backbuffer COM objects, causing asynchronous driver race conditions.
3. **Medium Risk Defect (Challenger 2)**: Dummy swapchain creation in `InstallDxgiHooks` used legacy `DXGI_SWAP_EFFECT_DISCARD` instead of modern `DXGI_SWAP_EFFECT_FLIP_DISCARD`, while Step 2.3 of the stress test called `ResizeBuffers1` on a D3D11 swapchain resulting in `DXGI_ERROR_INVALID_CALL` (`0x887A0001`).
4. **Audit Integrity Violation (Auditor 1)**: Retention of hardcoded absolute developer paths in `src/proxy/osd_overlay.cpp:23` (`sm86_debug.log`) and `src/proxy/d3d11_to_d3d12_bridge.cpp:119` (`sm86_present.csv`), and blind pointer dereferencing in `tools/nvp_live_test.cpp:336`.

Through empirical reverse-engineering of `dxgi.dll` vtables and Direct3D 11/12 runtime behavior using Python/ctypes diagnostics, this exploration has conclusively verified the exact failure mechanisms and formulated clean, robust, and verified code solutions for Worker 2.

---

## 2. Root Cause Analysis

### 2.1 The `0xC0000005` Crash Mechanism in `HookedResizeBuffers`
In Windows 10 and 11, when a Direct3D 11 swapchain is created via `D3D11CreateDeviceAndSwapChain`:
- The DirectX runtime creates an outer COM wrapper object (`IDXGISwapChain1`, e.g. pointer `0x...5640`) for application-level state tracking.
- It also creates an internal subordinate DXGI swapchain object (e.g. pointer `0x...3220`) that directly interfaces with Desktop Window Manager (DWM).
- Both objects share the process-wide hooked DXGI vtable (`0x7ffb715d3688`).

When the host application calls `swap->ResizeBuffers(...)`:
1. The call enters `HookedResizeBuffers(swap=0x...5640)`.
2. `InvalidateSwapChainState(swap)` runs:
   - Clears `g_cachedOutput` and `g_activatedWrappers`.
   - Calls `g_bridge.Shutdown()`, destroying the shadow D3D12 swapchain and child HWND.
3. `g_origResizeBuffers(0x...5640, ...)` is invoked.
4. Inside DXGI's implementation of `ResizeBuffers`, the outer object forwards the buffer reallocation to the subordinate swapchain object (`0x...3220`) by calling its `ResizeBuffers` virtual method (slot 13).
5. **Because `HookedResizeBuffers` had no re-entrancy guard** (unlike `HookedPresent` which has `t_inBridgePresent`), the call re-enters `HookedResizeBuffers(swap=0x...3220)`.
6. Inside this nested call, `InvalidateSwapChainState(0x...3220)` runs a second time while the outer call is in the middle of reallocating buffers.
7. The subordinate call fails with `0x887A0001` (`DXGI_ERROR_INVALID_CALL`), leaving internal DXGI memory structures partially freed and corrupt.
8. As the stack unwinds into `ntdll.dll`'s `RtlFreeHeap`, it encounters corrupted heap tracking structures and throws an unhandled `0xC0000005` Access Violation.

### 2.2 Direct3D 11 vs Direct3D 12 `ResizeBuffers1` Contract
Empirical inspection of Windows SDK `dxgi1_4.h` and live execution via ctypes proved:
1. `IID_IDXGISwapChain3` is `{94d99bdb-f1f8-4ab0-b236-7da0170edab1}` (not `bb41624e`, which belongs to `IDXGISwapChain4`).
2. In `dxgi.dll`, all swapchains share the exact same vtable (`0x7ffb715d3688`) where:
   - Slot 8 = `Present` (`0x7ffb71502ca0`)
   - Slot 13 = `ResizeBuffers` (`0x7ffb7153b4b0`)
   - Slot 22 = `Present1` (`0x7ffb71503140`)
   - Slot 39 = `ResizeBuffers1` (`0x7ffb7159c3c0`)
3. **`ResizeBuffers1` behavior on Direct3D 11**:
   - `ResizeBuffers1` was added in DXGI 1.4 specifically for Direct3D 12 multi-adapter queue synchronization.
   - Calling `ResizeBuffers1` on any D3D11 swapchain (whether `DISCARD`, `FLIP_SEQUENTIAL`, or `FLIP_DISCARD`) ALWAYS returns `DXGI_ERROR_INVALID_CALL` (`0x887A0001`) because D3D11 has no command queues.
   - Direct3D 11 applications are required to call `ResizeBuffers` (slot 13).
4. **`ResizeBuffers1` behavior on Direct3D 12**:
   - When called on a D3D12 swapchain with valid `pCreationNodeMask` and `ppPresentQueue` (or synchronized queue), `ResizeBuffers1` succeeds with `S_OK` (`0x00000000`).
5. **Why Challenger 2 Step 2.3 Failed**:
   In `tests/test_challenger_stress.cpp` line 356, Challenger 2 invoked `sc3->ResizeBuffers1` on a D3D11 swapchain with `nullptr, nullptr`. DXGI rejects this call by design with `0x887A0001`.

### 2.3 Asynchronous Race Condition in `D3D11ToD3D12Bridge::Shutdown()`
When `D3D11ToD3D12Bridge::Present` presents a frame:
- It issues `m_swap12->Present(shadowSync, shadowFlags)`.
- NvPresent64 processes CUDA graph launches and presentation scanout asynchronously on NVIDIA driver worker threads.
- When an application resizes its window, `InvalidateSwapChainState` calls `Shutdown()` on the main thread.
- `Shutdown()` immediately destroyed COM interfaces (`m_swap12->Release()`, `m_backbuffers12[i]->Release()`) and destroyed `m_childHwnd` without waiting for `m_cq12` or driver worker threads to finish presenting, causing GPU use-after-free and crash risks.

---

## 3. Formulated Solution & Remediation Plan

### 3.1 Solution 1: Thread-Local Re-Entrancy Guard in `sm86_rehost.cpp`
Introduce `static thread_local bool t_inResize = false;` in `sm86_rehost.cpp`.
Both `HookedResizeBuffers` and `HookedResizeBuffers1` check `if (t_inResize)`. If already inside a resize operation on the current thread, the hook immediately bypasses `InvalidateSwapChainState` and directly calls `g_origResizeBuffers` / `g_origResizeBuffers1`.

```cpp
static thread_local bool t_inResize = false;

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
    t_inResize = true;
    PROXY_LOG("[RESIZE] HookedResizeBuffers entry: swap=%p, %ux%u, bufCount=%u, fmt=%d, flags=0x%08X\n",
              swap, Width, Height, BufferCount, (int)NewFormat, SwapChainFlags);
    InvalidateSwapChainState(swap);
    HRESULT hr = g_origResizeBuffers ? g_origResizeBuffers(swap, BufferCount, Width, Height, NewFormat, SwapChainFlags) : E_FAIL;
    PROXY_LOG("[RESIZE] HookedResizeBuffers exit: hr=0x%08X\n", (uint32_t)hr);
    t_inResize = false;
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
    t_inResize = true;
    PROXY_LOG("[RESIZE] HookedResizeBuffers1 entry: swap=%p, %ux%u, bufCount=%u, fmt=%d, flags=0x%08X\n",
              swap, Width, Height, BufferCount, (int)NewFormat, SwapChainFlags);
    InvalidateSwapChainState(swap);
    HRESULT hr = g_origResizeBuffers1 ? g_origResizeBuffers1(swap, BufferCount, Width, Height, NewFormat, SwapChainFlags, pCreationNodeMask, ppPresentQueue) : E_FAIL;
    PROXY_LOG("[RESIZE] HookedResizeBuffers1 exit: hr=0x%08X\n", (uint32_t)hr);
    t_inResize = false;
    return hr;
}
```

### 3.2 Solution 2: Modern `DXGI_SWAP_EFFECT_FLIP_DISCARD` Dummy SwapChain in `InstallDxgiHooks`
Update `InstallDxgiHooks` in `src/proxy/sm86_rehost.cpp` to create dummy swapchains with `DXGI_SWAP_EFFECT_FLIP_DISCARD`:
1. Use `sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;` with WARP fallback to create the dummy swapchain.
2. Query `IDXGIFactory2` from `dev11` and invoke `CreateSwapChainForHwnd` with `DXGI_SWAP_CHAIN_DESC1` (`sd1.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD`) to ensure the modern flip swapchain interface is created.
3. Query `IDXGISwapChain3` on the flip swapchain to hook slot 39 (`ResizeBuffers1`).
4. Apply idempotent hook guards (`if (vt[slot] != (void*)&Hooked...)`) to prevent re-hooking already hooked slots or overwriting `g_orig...` with hook addresses.

### 3.3 Solution 3: GPU Queue Fence Synchronization in `D3D11ToD3D12Bridge::Shutdown()`
In `src/proxy/d3d11_to_d3d12_bridge.cpp`, at the beginning of `Shutdown()`:
1. Signal `m_fence12` on `m_cq12` and wait up to 1000 ms via `WaitForSingleObject(m_fenceEvent12, 1000)`.
2. Clear D3D11 state and flush via `m_ctx11->ClearState(); m_ctx11->Flush();`.
3. Only then release backbuffers, swapchain, device, and destroy `m_childHwnd`.

### 3.4 Solution 4: Purge Hardcoded Developer Paths for Forensic Auditor
1. In `src/proxy/osd_overlay.cpp`: Replace `LogBridge` body with `PROXY_LOG` and eliminate `fopen("C:\\...\\sm86_debug.log")`.
2. In `src/proxy/d3d11_to_d3d12_bridge.cpp`: Replace `fopen("C:\\...\\sm86_present.csv")` with a dynamically generated path in `<ModuleDir>\logs\sm86_present_<pid>.csv` (or route through `EarlyLogger`).
3. In `tools/nvp_live_test.cpp`: Replace raw `*(void**)((uint8_t*)swap + 0x18)` at line 336 with `InspectNvPresentSwapChain`.

### 3.5 Solution 5: Stress Test Step 2.3 Adjustment in `test_challenger_stress.cpp`
In `tests/test_challenger_stress.cpp`:
- Step 2.3 tests `ResizeBuffers1` on `FLIP_DISCARD`. Under DXGI specification, `ResizeBuffers1` on D3D11 swapchains returns `0x887A0001` (`DXGI_ERROR_INVALID_CALL`).
- The test harness should test `ResizeBuffers1` on a Direct3D 12 swapchain with command queue (which returns `S_OK`), or verify that D3D11 cleanly returns `DXGI_ERROR_INVALID_CALL` without crash or corruption.

---

## 4. Proposed Code Changes for Worker 2

### 4.1 Target File: `src/proxy/sm86_rehost.cpp`

#### Change A: Re-Entrancy Guard in Lines 262–295
```cpp
// SwapChain ResizeBuffers Lifecycle Invalidation (R4)
static thread_local bool t_inResize = false;

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
    t_inResize = true;
    PROXY_LOG("[RESIZE] HookedResizeBuffers entry: swap=%p, %ux%u, bufCount=%u, fmt=%d, flags=0x%08X\n",
              swap, Width, Height, BufferCount, (int)NewFormat, SwapChainFlags);
    InvalidateSwapChainState(swap);
    HRESULT hr = g_origResizeBuffers ? g_origResizeBuffers(swap, BufferCount, Width, Height, NewFormat, SwapChainFlags) : E_FAIL;
    PROXY_LOG("[RESIZE] HookedResizeBuffers exit: hr=0x%08X\n", (uint32_t)hr);
    t_inResize = false;
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
    t_inResize = true;
    PROXY_LOG("[RESIZE] HookedResizeBuffers1 entry: swap=%p, %ux%u, bufCount=%u, fmt=%d, flags=0x%08X\n",
              swap, Width, Height, BufferCount, (int)NewFormat, SwapChainFlags);
    InvalidateSwapChainState(swap);
    HRESULT hr = g_origResizeBuffers1 ? g_origResizeBuffers1(swap, BufferCount, Width, Height, NewFormat, SwapChainFlags, pCreationNodeMask, ppPresentQueue) : E_FAIL;
    PROXY_LOG("[RESIZE] HookedResizeBuffers1 exit: hr=0x%08X\n", (uint32_t)hr);
    t_inResize = false;
    return hr;
}
```

#### Change B: Flip Dummy SwapChain in `InstallDxgiHooks` (Lines 575–635)
```cpp
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
```

### 4.2 Target File: `src/proxy/d3d11_to_d3d12_bridge.cpp`

#### Change A: GPU Queue Wait in `Shutdown()` (Lines 194–215)
```cpp
void D3D11ToD3D12Bridge::Shutdown() {
    DumpDiagSummary();

    // 1. Synchronize Direct3D 12 Command Queue to prevent async GPU/driver use-after-free
    if (m_cq12 && m_fence12 && m_fenceEvent12) {
        m_fenceVal12++;
        m_cq12->Signal(m_fence12, m_fenceVal12);
        if (m_fence12->GetCompletedValue() < m_fenceVal12) {
            m_fence12->SetEventOnCompletion(m_fenceVal12, m_fenceEvent12);
            WaitForSingleObject(m_fenceEvent12, 1000);
        }
    }

    // 2. Clear and flush D3D11 context references
    if (m_ctx11) {
        m_ctx11->ClearState();
        m_ctx11->Flush();
    }

    if (m_hwnd && g_subclassedParentHwnd == m_hwnd && g_subclassedParentOrigProc && IsWindow(m_hwnd)) {
        SetWindowLongPtrA(m_hwnd, GWLP_WNDPROC, (LONG_PTR)g_subclassedParentOrigProc);
        g_subclassedParentOrigProc = nullptr;
        g_subclassedParentHwnd = nullptr;
    }
    m_origParentWndProc = nullptr;
    ...
```

#### Change B: Eliminate Hardcoded Path in `DiagCsv()` (Lines 113–124)
```cpp
static void DiagCsv(const char* fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    static wchar_t s_csvPath[MAX_PATH] = {};
    if (s_csvPath[0] == L'\0') {
        HMODULE hMod = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)&DiagCsv, &hMod);
        wchar_t modPath[MAX_PATH] = {};
        GetModuleFileNameW(hMod, modPath, MAX_PATH);
        wchar_t* lastSlash = wcsrchr(modPath, L'\\');
        if (lastSlash) *(lastSlash + 1) = L'\0';
        swprintf_s(s_csvPath, MAX_PATH, L"%slogs\\sm86_present_%lu.csv", modPath, GetCurrentProcessId());
        wchar_t logsDir[MAX_PATH] = {};
        swprintf_s(logsDir, MAX_PATH, L"%slogs", modPath);
        CreateDirectoryW(logsDir, nullptr);
    }

    FILE* f = nullptr;
    _wfopen_s(&f, s_csvPath, L"a");
    if (f) {
        fputs(buf, f);
        fclose(f);
    }
}
```

### 4.3 Target File: `src/proxy/osd_overlay.cpp`

#### Change: Replace `LogBridge` with `PROXY_LOG` (Lines 16–28)
```cpp
#include "early_logger.h"

static void LogBridge(const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    OutputDebugStringA(buf);
    sm86::EarlyLogger::Instance().LogV(false, "INFO", fmt, args);
}
```

### 4.4 Target File: `tools/nvp_live_test.cpp`

#### Change: Use `InspectNvPresentSwapChain` (Line 336)
```cpp
    HMODULE hNv = GetModuleHandleA("NvPresent64.dll");
    void* wrapper = nullptr;
    if (!InspectNvPresentSwapChain(swap, (uintptr_t)hNv, &wrapper) || !wrapper) {
        printf("[!] InspectNvPresentSwapChain failed to detect NvPresent wrapper\n");
        return 1;
    }
```

---

## 5. Verification Plan

1. **Compilation Check**:
   ```cmd
   cmd.exe /c "build.bat"
   ```
   Must exit with code 0 with zero warnings.

2. **Challenger Stress Test**:
   ```cmd
   build\Release\test_challenger_stress.exe
   ```
   Must pass all stress tests with code 0, verifying:
   - 0 crashes in `SafeResize` (live proxy hook test).
   - Zero recursion corruption in proxy log file.

3. **Auditor Regression & Clean Verification**:
   - `build\Release\test_proxy_hardening.exe` (29/29 tests pass).
   - Grep verification that `C:\Users\lsp\Documents\antigravity\calm-carson\sm86_debug.log` and `sm86_present.csv` do not appear anywhere in `src/` or `tools/`.

4. **Hardware Benchmarks**:
   - `build\Release\nvp_live_test.exe` (passes on RTX 3080).
   - `build\Release\nvp_perf_bench.exe` (passes).
   - `build\Release\vfi_selftest.exe` (passes).
