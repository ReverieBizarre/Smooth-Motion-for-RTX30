# Investigation Report: Milestone 1 Remediation (Iteration 2)
**Explorer**: Explorer 3 (`teamwork_preview_explorer`)  
**Mission**: Investigate `src/proxy/d3d11_to_d3d12_bridge.cpp:Shutdown()` GPU synchronization and `tools/nvp_live_test.cpp:336` blind dereference; formulate concrete remediation proposals for Worker 2.  
**Date**: 2026-09-19  

---

## 1. Executive Summary

During Milestone 1 evaluation, **Challenger 2** and **Auditor 1** identified critical gaps preventing release approval:
1. **Asynchronous GPU Presentation Race in `D3D11ToD3D12Bridge::Shutdown()`** (`src/proxy/d3d11_to_d3d12_bridge.cpp:194–244`):
   When `ResizeBuffers` occurs in D3D11 applications (e.g. MPC-HC, MPC-VR, or games), `InvalidateSwapChainState` immediately invokes `g_bridge.Shutdown()`. In `Shutdown()`, `m_swap12`, backbuffers, and `m_childHwnd` are released and destroyed on the CPU thread without waiting for in-flight asynchronous GPU presentation and NvPresent64 worker threads to drain. This causes use-after-free, device loss, or scanout tearing.
2. **Blind SwapChain Dereference in Test Binaries** (`tools/nvp_live_test.cpp:336` and `tools/nvp_perf_bench.cpp:144`):
   Test harnesses still dereference `*(void**)((uint8_t*)swap + 0x18)` directly, bypassing the safe dual-vtable validation primitive `InspectNvPresentSwapChain` established in `early_logger.h`.
3. **Audit & Challenger Defect Synthesis**:
   A complete remediation must also incorporate:
   - `static thread_local bool t_inResize = false;` re-entrancy guard in `HookedResizeBuffers` / `HookedResizeBuffers1` (`sm86_rehost.cpp:262–294`).
   - Dummy swapchain creation with `DXGI_SWAP_EFFECT_FLIP_DISCARD` in `InstallDxgiHooks` (`sm86_rehost.cpp:583`).
   - Purging hardcoded developer paths in `osd_overlay.cpp:23` (`sm86_debug.log`) and `d3d11_to_d3d12_bridge.cpp:119` (`sm86_present.csv`).

This report provides complete root-cause analyses, exact code replacements, and verification procedures for Worker 2.

---

## 2. Investigation Item 1: GPU Synchronization in `D3D11ToD3D12Bridge::Shutdown()`

### 2.1 Code Location & Flawed Behavior
File: `src/proxy/d3d11_to_d3d12_bridge.cpp` lines 194–244:

```cpp
void D3D11ToD3D12Bridge::Shutdown() {
    DumpDiagSummary();
    if (m_hwnd && g_subclassedParentHwnd == m_hwnd && g_subclassedParentOrigProc && IsWindow(m_hwnd)) {
        SetWindowLongPtrA(m_hwnd, GWLP_WNDPROC, (LONG_PTR)g_subclassedParentOrigProc);
        g_subclassedParentOrigProc = nullptr;
        g_subclassedParentHwnd = nullptr;
    }
    m_origParentWndProc = nullptr;

    if (m_sharedHandle) {
        CloseHandle(m_sharedHandle);
        m_sharedHandle = nullptr;
    }
    if (m_fenceEvent12) {
        CloseHandle(m_fenceEvent12);
        m_fenceEvent12 = nullptr;
    }
    if (m_fence12)        { m_fence12->Release(); m_fence12 = nullptr; }
    for (int i = 0; i < 8; i++) {
        if (m_backbuffers12[i]) { m_backbuffers12[i]->Release(); m_backbuffers12[i] = nullptr; }
    }
    if (m_query11)        { m_query11->Release(); m_query11 = nullptr; }
    // The waitable handle is owned by the swapchain - do NOT CloseHandle it.
    m_frameLatencyWaitable = nullptr;
    if (m_swap2_12)       { m_swap2_12->Release(); m_swap2_12 = nullptr; }
    if (m_sharedTex12)    { m_sharedTex12->Release(); m_sharedTex12 = nullptr; }
    if (m_swap3_12)       { m_swap3_12->Release(); m_swap3_12 = nullptr; }
    if (m_swap12)         { m_swap12->Release(); m_swap12 = nullptr; }
    if (m_cl12)           { m_cl12->Release(); m_cl12 = nullptr; }
    if (m_alloc12)        { m_alloc12->Release(); m_alloc12 = nullptr; }
    if (m_cq12)           { m_cq12->Release(); m_cq12 = nullptr; }
    if (m_dev12)          { m_dev12->Release(); m_dev12 = nullptr; }
    if (m_sharedTex11)    { m_sharedTex11->Release(); m_sharedTex11 = nullptr; }
    if (m_ctx11)          { m_ctx11->Release(); m_ctx11 = nullptr; }
    if (m_childHwnd && IsWindow(m_childHwnd)) {
        ShowWindow(m_childHwnd, SW_HIDE);
        DestroyWindow(m_childHwnd);
        m_childHwnd = nullptr;
    }
    m_dev11 = nullptr;
    m_wrapper = nullptr;
    m_hwnd = nullptr;
    m_childHwnd = nullptr;
    m_lastX = -1;
    m_lastY = -1;
    m_lastW = -1;
    m_lastH = -1;
    m_width = 0;
    m_height = 0;
    m_active = false;
}
```

### 2.2 Root-Cause Analysis
1. In `D3D11ToD3D12Bridge::Present` (`d3d11_to_d3d12_bridge.cpp:736–752`):
   - The GPU copy command list is executed on `m_cq12` and fenced.
   - `m_swap12->Present(shadowSync, shadowFlags)` is invoked immediately after.
   - DXGI presentation on flip-model swapchains is asynchronous: the driver queues the flip to the display compositor (DWM / hardware plane).
   - In NvPresent64, Smooth Motion interpolation launches CUDA graphs (`cuGraphLaunch`), reads the current frame, and produces intermediate synthesized frames asynchronously across background driver worker threads.
2. When the host application calls `ResizeBuffers` (e.g. user resizes video window or toggles fullscreen):
   - `InvalidateSwapChainState` calls `g_bridge.Shutdown()`.
   - `Shutdown()` runs on the main thread immediately.
   - It closes `m_fenceEvent12` and releases `m_fence12` without inserting a completion signal or wait on `m_cq12`.
   - It immediately releases all `m_backbuffers12[i]` and `m_swap12`, and destroys `m_childHwnd` (`DestroyWindow`).
3. Consequence:
   - The GPU and NVIDIA display driver worker threads attempt to access deallocated swapchain backbuffer memory and window surfaces that no longer exist on the CPU.
   - This results in `DXGI_ERROR_DEVICE_REMOVED`, use-after-free crashes in driver worker threads, black screen flashes, or leaked D3D12 resources preventing re-initialization.

### 2.3 Proposed GPU Synchronization Mechanism
To guarantee GPU idle before teardown, `Shutdown()` must:
1. **Flush and wait for all commands on `m_cq12`**:
   Signal `m_fence12` with an incremented fence value, and wait up to 2000 ms on `m_fenceEvent12`.
2. **Retire pending flips on `m_frameLatencyWaitable`**:
   If the shadow swapchain was created with `DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT`, wait on `m_frameLatencyWaitable` for up to 1000 ms to ensure the presentation engine has released the backbuffers.
3. **Flush D3D11 immediate context**:
   Ensure all D3D11 texture copy and query operations are retired.
4. **Sequence teardown strictly**:
   Only after the GPU is verified idle should backbuffers, swapchain, shared textures, command allocator, command queue, and the child HWND be released/destroyed.

#### Proposed Code for `D3D11ToD3D12Bridge::Shutdown()`:
```cpp
void D3D11ToD3D12Bridge::Shutdown() {
    DumpDiagSummary();

    // 1. Synchronize D3D11 execution
    if (m_ctx11) {
        if (m_query11) {
            m_ctx11->End(m_query11);
            m_ctx11->Flush();
            while (m_ctx11->GetData(m_query11, nullptr, 0, 0) == S_FALSE) {
                YieldProcessor();
            }
        } else {
            m_ctx11->Flush();
        }
    }

    // 2. Synchronize D3D12 GPU queue before releasing any resources
    if (m_cq12 && m_fence12 && m_fenceEvent12) {
        m_fenceVal12++;
        HRESULT hrSig = m_cq12->Signal(m_fence12, m_fenceVal12);
        if (SUCCEEDED(hrSig)) {
            if (m_fence12->GetCompletedValue() < m_fenceVal12) {
                m_fence12->SetEventOnCompletion(m_fenceVal12, m_fenceEvent12);
                WaitForSingleObject(m_fenceEvent12, 2000);
            }
        }
    }

    // 3. Wait for shadow swapchain latency queue to retire pending flip
    if (m_frameLatencyWaitable) {
        WaitForSingleObject(m_frameLatencyWaitable, 1000);
        m_frameLatencyWaitable = nullptr; // Owned by swapchain; do not CloseHandle
    }

    // 4. Restore parent window subclassing
    if (m_hwnd && g_subclassedParentHwnd == m_hwnd && g_subclassedParentOrigProc && IsWindow(m_hwnd)) {
        SetWindowLongPtrA(m_hwnd, GWLP_WNDPROC, (LONG_PTR)g_subclassedParentOrigProc);
        g_subclassedParentOrigProc = nullptr;
        g_subclassedParentHwnd = nullptr;
    }
    m_origParentWndProc = nullptr;

    // 5. Release shared handles and sync primitives
    if (m_sharedHandle) {
        CloseHandle(m_sharedHandle);
        m_sharedHandle = nullptr;
    }
    if (m_fenceEvent12) {
        CloseHandle(m_fenceEvent12);
        m_fenceEvent12 = nullptr;
    }
    if (m_fence12) {
        m_fence12->Release();
        m_fence12 = nullptr;
    }

    // 6. Release D3D12 backbuffers and swapchains
    for (int i = 0; i < 8; i++) {
        if (m_backbuffers12[i]) {
            m_backbuffers12[i]->Release();
            m_backbuffers12[i] = nullptr;
        }
    }
    if (m_query11)        { m_query11->Release(); m_query11 = nullptr; }
    if (m_swap2_12)       { m_swap2_12->Release(); m_swap2_12 = nullptr; }
    if (m_swap3_12)       { m_swap3_12->Release(); m_swap3_12 = nullptr; }
    if (m_swap12)         { m_swap12->Release(); m_swap12 = nullptr; }
    if (m_sharedTex12)    { m_sharedTex12->Release(); m_sharedTex12 = nullptr; }
    if (m_cl12)           { m_cl12->Release(); m_cl12 = nullptr; }
    if (m_alloc12)        { m_alloc12->Release(); m_alloc12 = nullptr; }
    if (m_cq12)           { m_cq12->Release(); m_cq12 = nullptr; }
    if (m_dev12)          { m_dev12->Release(); m_dev12 = nullptr; }
    if (m_sharedTex11)    { m_sharedTex11->Release(); m_sharedTex11 = nullptr; }
    if (m_ctx11)          { m_ctx11->Release(); m_ctx11 = nullptr; }

    // 7. Teardown overlay window
    if (m_childHwnd && IsWindow(m_childHwnd)) {
        ShowWindow(m_childHwnd, SW_HIDE);
        DestroyWindow(m_childHwnd);
        m_childHwnd = nullptr;
    }

    m_dev11 = nullptr;
    m_wrapper = nullptr;
    m_hwnd = nullptr;
    m_childHwnd = nullptr;
    m_lastX = -1;
    m_lastY = -1;
    m_lastW = -1;
    m_lastH = -1;
    m_width = 0;
    m_height = 0;
    m_active = false;
}
```

---

## 3. Investigation Item 2: Elimination of Blind SwapChain Dereference in Test Binaries

### 3.1 Code Location & Flawed Behavior
1. `tools/nvp_live_test.cpp:336`:
   ```cpp
   IDXGISwapChain1* swap = nullptr;
   f->CreateSwapChainForHwnd(cq, wnd, &sd, nullptr, nullptr, &swap);
   if (!swap) { printf("[!] CreateSwapChainForHwnd failed\n"); return 1; }

   void* wrapper = *(void**)((uint8_t*)swap + 0x18);
   printf("[+] SwapChain created @ %p (Proxy COM Object, Internal Wrapper @ %p)\n", swap, wrapper);

   if (wrapper) {
       void** vt = *(void***)wrapper;
       typedef void (*pfnSetByte)(void*, uint8_t);
       ((pfnSetByte)vt[19])(wrapper, 1);
       ((pfnSetByte)vt[20])(wrapper, 1);
       ...
   ```
2. `tools/nvp_perf_bench.cpp:144`:
   ```cpp
   IDXGISwapChain1* swap = nullptr;
   f->CreateSwapChainForHwnd(cq, wnd, &sd, nullptr, nullptr, &swap);
   if (!swap) { printf("[!] CreateSwapChainForHwnd failed\n"); return; }

   void* wrapper = *(void**)((uint8_t*)swap + 0x18);
   if (wrapper) {
       void** vt = *(void***)wrapper;
       typedef void (*pfnSetByte)(void*, uint8_t);
       ((pfnSetByte)vt[19])(wrapper, 1);
       ((pfnSetByte)vt[20])(wrapper, 1);
   }
   ```

### 3.2 Analysis & Risk
- If `swap` is a native DXGI swapchain (e.g. if NvPresent hook installation failed) or wrapped by third-party interposers (Streamline `sl.interposer.dll`, Reflex, ReShade, Agility SDK), offset `+0x18` contains either an integer (e.g. buffer count), an unrelated internal structure pointer, or uncommitted memory.
- Dereferencing `*(void**)((uint8_t*)swap + 0x18)` without checking vtables or memory protection violates Requirement R1.
- In `early_logger.h`, `InspectNvPresentSwapChain` provides a safe 5-stage validation using `SafeReadPointer`:
  1. Validates alignment and canonical address.
  2. Confirms outer vtable == `base + 0x1d3228`.
  3. Safely reads `+0x18` pointer within memory protection boundaries.
  4. Reads wrapper vtable.
  5. Confirms wrapper vtable == `base + 0x1d39c0`.
- Both `nvp_live_test.cpp` and `nvp_perf_bench.cpp` should use `InspectNvPresentSwapChain`.

### 3.3 Proposed Code for `tools/nvp_live_test.cpp`
1. Include `../src/proxy/early_logger.h`:
   ```cpp
   #include "../src/proxy/pe_scan.h"
   #include "../src/proxy/early_logger.h"
   ```
2. Replace lines 336–360 with:
   ```cpp
   void* wrapper = nullptr;
   if (!InspectNvPresentSwapChain(swap, (uintptr_t)nv, &wrapper) || !wrapper) {
       printf("[!] InspectNvPresentSwapChain failed: SwapChain %p is not a genuine NvPresent64 proxy object!\n", swap);
       swap->Release();
       f->Release();
       cq->Release();
       dev->Release();
       DestroyWindow(wnd);
       return 1;
   }
   printf("[+] SwapChain created @ %p (Verified NvPresent64 Proxy, Internal Wrapper @ %p)\n", swap, wrapper);

   void** vt = *(void***)wrapper;
   typedef void (*pfnSetByte)(void*, uint8_t);
   ((pfnSetByte)vt[19])(wrapper, 1);
   ((pfnSetByte)vt[20])(wrapper, 1);
   printf("[+] Smooth Motion activated on wrapper (vt[19]=1, vt[20]=1)\n");

   uint8_t* pBufArray = *(uint8_t**)((uint8_t*)wrapper + 0x1618);
   printf("[+] Hidden backbuffer array @ %p\n", pBufArray);
   if (pBufArray) {
       for (int i = 0; i < 2; i++) {
           uint8_t* pEntry = pBufArray + 288 * i;
           ID3D12Resource* pHidden = *(ID3D12Resource**)(pEntry + 8);
           printf("    Hidden buffer [%d] entry @ %p -> resource @ %p\n", i, pEntry, pHidden);
           if (pHidden) {
               D3D12_RESOURCE_DESC desc = pHidden->GetDesc();
               printf("      Resource Desc: %llux%u, format %d\n", desc.Width, desc.Height, (int)desc.Format);
           }
       }
   }
   ```

### 3.4 Proposed Code for `tools/nvp_perf_bench.cpp`
1. Include `../src/proxy/early_logger.h`:
   ```cpp
   #include "../src/proxy/pe_scan.h"
   #include "../src/proxy/early_logger.h"
   ```
2. Replace lines 144–150 with:
   ```cpp
   void* wrapper = nullptr;
   if (InspectNvPresentSwapChain(swap, 0, &wrapper) && wrapper) {
       void** vt = *(void***)wrapper;
       typedef void (*pfnSetByte)(void*, uint8_t);
       ((pfnSetByte)vt[19])(wrapper, 1);
       ((pfnSetByte)vt[20])(wrapper, 1);
   }
   ```

---

## 4. Synthesis of Other Pending Fixes for Worker 2

To ensure full approval in Milestone 1 Remediation, the following defects must also be resolved simultaneously:

### 4.1 Re-entrancy Protection in `HookedResizeBuffers` & `HookedResizeBuffers1`
**File**: `src/proxy/sm86_rehost.cpp:262–294`  
**Problem**: Windows DXGI invokes `ResizeBuffers` on an internal subordinate swapchain object during the outer resize. Without re-entrancy protection, `InvalidateSwapChainState` runs twice, inner resize fails with `0x887A0001`, and `RtlFreeHeap` triggers a `0xC0000005` crash.  
**Fix**:
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
        return g_origResizeBuffers(swap, BufferCount, Width, Height, NewFormat, SwapChainFlags);
    }
    t_inResize = true;
    PROXY_LOG("[RESIZE] HookedResizeBuffers entry: swap=%p, %ux%u, bufCount=%u, fmt=%d, flags=0x%08X\n",
              swap, Width, Height, BufferCount, (int)NewFormat, SwapChainFlags);
    InvalidateSwapChainState(swap);
    HRESULT hr = g_origResizeBuffers(swap, BufferCount, Width, Height, NewFormat, SwapChainFlags);
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
        return g_origResizeBuffers1(swap, BufferCount, Width, Height, NewFormat, SwapChainFlags, pCreationNodeMask, ppPresentQueue);
    }
    t_inResize = true;
    PROXY_LOG("[RESIZE] HookedResizeBuffers1 entry: swap=%p, %ux%u, bufCount=%u, fmt=%d, flags=0x%08X\n",
              swap, Width, Height, BufferCount, (int)NewFormat, SwapChainFlags);
    InvalidateSwapChainState(swap);
    HRESULT hr = g_origResizeBuffers1(swap, BufferCount, Width, Height, NewFormat, SwapChainFlags, pCreationNodeMask, ppPresentQueue);
    PROXY_LOG("[RESIZE] HookedResizeBuffers1 exit: hr=0x%08X\n", (uint32_t)hr);
    t_inResize = false;
    return hr;
}
```

### 4.2 Dummy SwapChain SwapEffect for `ResizeBuffers1` Hooking
**File**: `src/proxy/sm86_rehost.cpp:583`  
**Problem**: `InstallDxgiHooks` creates dummy swapchain with `sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD`. Under DXGI specification, `ResizeBuffers1` is strictly supported only on flip-model swapchains.  
**Fix**:
Change line 583:
```cpp
sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
```

### 4.3 Purge Hardcoded Developer Path `sm86_debug.log` in `osd_overlay.cpp`
**File**: `src/proxy/osd_overlay.cpp:16–28`  
**Problem**: Contains hardcoded `FILE* f = fopen("C:\\Users\\lsp\\Documents\\antigravity\\calm-carson\\sm86_debug.log", "a");`.  
**Fix**:
Include `early_logger.h` and rewrite `LogBridge` to route to `EarlyLogger` (matching `d3d11_to_d3d12_bridge.cpp:12–17`):
```cpp
#include "early_logger.h"

static void LogBridge(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    sm86::EarlyLogger::Instance().LogV(false, "INFO", fmt, args);
    va_end(args);
}
```

### 4.4 Purge Hardcoded Developer Path `sm86_present.csv` in `d3d11_to_d3d12_bridge.cpp`
**File**: `src/proxy/d3d11_to_d3d12_bridge.cpp:113–124`  
**Problem**: Contains hardcoded `FILE* f = fopen("C:\\Users\\lsp\\Documents\\antigravity\\calm-carson\\sm86_present.csv", "a");`.  
**Fix**:
Compute dynamic path relative to the module directory:
```cpp
static void DiagCsv(const char* fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    static wchar_t s_csvPath[MAX_PATH] = {};
    static bool s_inited = false;
    if (!s_inited) {
        HMODULE hMod = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)&DiagCsv, &hMod);
        wchar_t modPath[MAX_PATH] = {};
        if (hMod) GetModuleFileNameW(hMod, modPath, MAX_PATH);
        else GetModuleFileNameW(nullptr, modPath, MAX_PATH);
        wchar_t* lastSlash = wcsrchr(modPath, L'\\');
        if (lastSlash) *(lastSlash + 1) = L'\0';
        wchar_t logDir[MAX_PATH] = {};
        swprintf_s(logDir, L"%slogs", modPath);
        CreateDirectoryW(logDir, nullptr);
        swprintf_s(s_csvPath, L"%s\\sm86_present_%lu.csv", logDir, GetCurrentProcessId());
        s_inited = true;
    }
    FILE* f = _wfopen(s_csvPath, L"a");
    if (f) {
        fputs(buf, f);
        fclose(f);
    }
}
```

---

## 5. Implementation Roadmap for Worker 2

| Step | Target File | Action | Impact |
|---|---|---|---|
| 1 | `src/proxy/d3d11_to_d3d12_bridge.cpp` | Update `Shutdown()` to flush D3D11 and wait on `m_cq12->Signal(m_fence12, ...)` and `m_frameLatencyWaitable` before teardown | Eliminates GPU/driver race condition during swapchain recreation |
| 2 | `src/proxy/d3d11_to_d3d12_bridge.cpp` | Update `DiagCsv` to write to `<ModuleDir>\logs\sm86_present_<pid>.csv` | Resolves Auditor 1 path violation |
| 3 | `src/proxy/sm86_rehost.cpp` | Add `thread_local bool t_inResize = false;` to `HookedResizeBuffers` & `HookedResizeBuffers1` | Fixes Challenger 2 `0xC0000005` recursion crash |
| 4 | `src/proxy/sm86_rehost.cpp` | Change dummy swapchain to `DXGI_SWAP_EFFECT_FLIP_DISCARD` in `InstallDxgiHooks` | Fixes `ResizeBuffers1` `0x887A0001` error |
| 5 | `src/proxy/osd_overlay.cpp` | Include `early_logger.h` and delegate `LogBridge` to `EarlyLogger::LogV` | Resolves Auditor 1 `sm86_debug.log` path violation |
| 6 | `tools/nvp_live_test.cpp` | Include `early_logger.h` and use `InspectNvPresentSwapChain` at line 336 | Eliminates blind dereference in live test tool |
| 7 | `tools/nvp_perf_bench.cpp` | Include `early_logger.h` and use `InspectNvPresentSwapChain` at line 144 | Eliminates blind dereference in perf bench tool |

---

## 6. Verification Method

Once Worker 2 applies the proposed changes:
1. **Full Build**:
   ```cmd
   cmd.exe /c "build.bat"
   ```
   *Expected*: Zero warnings/errors; all 13 targets compile cleanly.
2. **Challenger Stress Suite**:
   ```cmd
   build\Release\test_challenger_stress.exe
   ```
   *Expected*: Exit code 0; all 19/19 stress tests pass (including 20 live `Present -> ResizeBuffers` cycles without crash).
3. **Hardening Verification Suite**:
   ```cmd
   build\Release\test_proxy_hardening.exe
   ```
   *Expected*: Exit code 0; all 29/29 tests pass.
4. **Hardware Verification on RTX 3080**:
   ```cmd
   build\Release\nvp_live_test.exe
   build\Release\nvp_perf_bench.exe
   build\Release\vfi_selftest.exe
   build\Release\test_d3d11_bridge_nvp.exe
   ```
   *Expected*: All hardware targets run and exit with code 0.
5. **Static Forensic Inspection**:
   Search for hardcoded developer paths:
   ```cmd
   git grep "sm86_debug.log"
   git grep "sm86_present.csv"
   ```
   *Expected*: Zero hits in `src/`.
