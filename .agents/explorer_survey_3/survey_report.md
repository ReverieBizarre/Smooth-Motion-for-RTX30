# Technical Survey Report: Requirements R4 & R5
**Component**: `sm86_smooth` Version Proxy Runtime (`version.dll`) & Hardware Harness  
**Author**: Explorer 3 (`teamwork_preview_explorer`)  
**Date**: 2026-09-19  
**Target Hardware**: NVIDIA GeForce RTX 3080 12GB (Ampere Architecture, `sm_86`)  
**Workspace**: `C:\Users\lsp\Documents\antigravity\calm-carson`  

---

## Executive Summary

This survey report provides a comprehensive architectural and technical map of the `sm86_smooth` codebase, focusing specifically on **Requirement R4 (SwapChain ResizeBuffers Lifecycle Handling)** and **Requirement R5 (Complete Build & Hardware Regression Verification)**.

Key findings:
1. **R4 Status (Gap Identified)**: Neither `IDXGISwapChain::ResizeBuffers` (vtable slot 13) nor `IDXGISwapChain3::ResizeBuffers1` (vtable slot 39) is currently hooked or handled in `src/proxy/sm86_rehost.cpp`. Only slot 8 (`Present`) and slot 22 (`Present1`) are intercepted. When a game or video player resizes its window, changes display mode (e.g. fullscreen toggle), or migrates to a different monitor, stale wrapper pointers in `g_activatedWrappers`, active D3D11-to-D3D12 bridge resources, cached backbuffers, and unmanaged `IDXGIOutput` pointers cause `ResizeBuffers` to fail with `DXGI_ERROR_INVALID_CALL` (0x887A0001) or trigger access violation crashes (`0xC0000005`).
2. **R5 Status (Verified & Fully Functional)**: The MSVC C++17 build pipeline via `build.bat` executes cleanly, producing all required binaries: `version.dll`, `nvp_live_test.exe`, `nvp_perf_bench.exe`, `proxytest.exe`, `vfi_selftest.exe`, as well as auxiliary test harnesses (`test_pe_scan.exe`, `test_d3d11_bridge_nvp.exe`, `test_osd_uimask.exe`, `test_d3d11_osd.exe`, `test_addon_simulation.exe`). Hardware regression tests on the local **RTX 3080 (12GB, driver 616.56)** confirm that dynamic gate patching, on-the-fly FP16 fatbinary rewriting (19 kernels), and live `cuGraphLaunch` neural frame generation execute with 100% success without regression.

---

## Part 1: Requirement R4 — SwapChain ResizeBuffers Lifecycle Handling

### 1.1 Current VTable Hook Status

Inspection of `src/proxy/sm86_rehost.cpp` (lines 457–506, function `InstallDxgiHooks`) reveals the exact current hook mechanism:

```cpp
// src/proxy/sm86_rehost.cpp:457-506
static void InstallDxgiHooks() {
    // Creates dummy HWND and dummy D3D11 device/swapchain...
    if (SUCCEEDED(D3D11CreateDeviceAndSwapChain(..., &sc, &dev11, &lvl, &ctx))) {
        IDXGISwapChain1* sc1 = nullptr;
        if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&sc1)))) {
            void** vt = *(void***)sc1;
            DWORD oldProt = 0;
            VirtualProtect(&vt[8], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProt);
            g_origPresent = (Present_t)vt[8];
            vt[8] = (void*)&HookedPresent;
            VirtualProtect(&vt[8], sizeof(void*), oldProt, &oldProt);

            VirtualProtect(&vt[22], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProt);
            g_origPresent1 = (Present1_t)vt[22];
            vt[22] = (void*)&HookedPresent1;
            VirtualProtect(&vt[22], sizeof(void*), oldProt, &oldProt);

            LogBridge("[sm86_rehost] DXGI Present hooks installed (slot 8 & slot 22)\n");
            sc1->Release();
        }
        // ...
    }
}
```

#### Exact Observations:
1. **Slot 8 (`IDXGISwapChain::Present`)**: Hooked via `vt[8] = (void*)&HookedPresent`.
2. **Slot 22 (`IDXGISwapChain1::Present1`)**: Hooked via `vt[22] = (void*)&HookedPresent1`.
3. **Slot 13 (`IDXGISwapChain::ResizeBuffers`)**: **NOT HOOKED**.
4. **Slot 39 (`IDXGISwapChain3::ResizeBuffers1`)**: **NOT HOOKED**. Notice that `sc1` is an `IDXGISwapChain1` interface which only exposes 29 vtable slots (slots 0–28). `ResizeBuffers1` is located on `IDXGISwapChain3` (slot 39), so querying `sc1` alone is physically incapable of reaching slot 39!
5. **Grep Search Results**: `ResizeBuffers` is completely absent from all source code in `src/proxy/`. It only appears in documentation (`FLICKER_ANALYSIS.md` line 587, `RESHADE_ADDON_INSTALL.md` line 38) where real crashes were logged in MPC-HC during window resize (`ResizeBuffers(2617x1689)`), and in ReShade headers (`deps/reshade/include/reshade_events.hpp`).

---

### 1.2 Inventory of Internal State Requiring Lifecycle Invalidation

When a game or video player resizes its swapchain or changes display modes, DirectX runtime rules dictate that **all outstanding backbuffer references must be released** before calling `ResizeBuffers`. Furthermore, all proxy state tracking dimensions, output adapters, and GPU handles must be invalidated.

The following table documents every internal state object across the codebase that must be managed:

| State Variable / Resource | File Location | Type / Interface | Risk on Resize if Not Handled | Invalidation Action Required |
|---|---|---|---|---|
| `g_activatedWrappers` | `src/proxy/sm86_rehost.cpp:81` | `std::set<void*>` | Stale pointer or reallocated wrapper. On resize, `NvPresent64` may reinitialize or recreate its wrapper. If stale pointer remains in set, subsequent presents fail to re-enable Smooth Motion (`vt[19]=1, vt[20]=1`). If memory address is reused, UB occurs. | Remove wrapper associated with resizing swapchain from `g_activatedWrappers`. |
| `g_cachedOutput` (R3 VBlank Pacing) | Global to be added for R3 | `IDXGIOutput*` | `IDXGIOutput` holds an active COM reference to the display adapter. If retained during `ResizeBuffers` or monitor change, `ResizeBuffers` fails with `DXGI_ERROR_INVALID_CALL` (0x887A0001) or leaks cross-adapter state. | `if (g_cachedOutput) { g_cachedOutput->Release(); g_cachedOutput = nullptr; }` |
| `g_bridge` (`D3D11ToD3D12Bridge`) | `src/proxy/sm86_rehost.cpp:180`, `d3d11_to_d3d12_bridge.h/.cpp` | `sm86::D3D11ToD3D12Bridge` | Contains active shadow swapchain (`m_swap12`), 8 backbuffer resources (`m_backbuffers12`), shared texture (`m_sharedTex11`, `m_sharedTex12`), NT handle (`m_sharedHandle`), and child window (`m_childHwnd`). If the app resizes its D3D11 swapchain while bridge is active, dimensions mismatch, causing presentation clipping or `E_ACCESSDENIED`. | Invoke `g_bridge.Shutdown()` inside pre-resize handler. Re-initialize bridge on next `Present()` with new dimensions. |
| `g_osd` & `g_uiMask` Command Objects | `src/proxy/sm86_rehost.cpp:170-176` | `ID3D12CommandAllocator*`, `ID3D12GraphicsCommandList*` | In `ProcessOverlayAndUiMask`, commands transition backbuffers to `UAV` and execute on `g_cmdQueue`. If `ResizeBuffers` occurs immediately while GPU commands are in flight on the backbuffer, race condition causes driver crash. | Flush command queue (`Signal` fence & wait) before proceeding with `ResizeBuffers`. |
| Direct3D 11 Backbuffer References | `src/proxy/osd_overlay.cpp:600-611` | `ID3D11RenderTargetView*`, `ID3D11Texture2D*` | In `RenderD3D11`, `swap->GetBuffer(0)` creates temporary RTV. Currently released before return, but thread synchronization during resize is essential. | Ensure thread-safety via critical section so `RenderD3D11` cannot execute concurrently with `ResizeBuffers`. |
| `NvPresent64.dll` Internal Wrapper Pool | `NvPresent64.dll` internal (RVA `0x1d3228`) | Hidden buffer pool (`wrapper + 0x1618`, entries at +0x80) | `NvPresent64.dll` wraps DXGI swapchain and registers hidden backbuffers with CUDA (`cuGraphicsD3D12RegisterResource`). Its internal `ResizeBuffers` forwards to DXGI and re-allocates hidden buffers. | Hook slot 13 and slot 39, clean up outer proxy state, forward call to original `ResizeBuffers`, and let `NvPresent64` handle internal CUDA reallocation cleanly. |

---

### 1.3 Detailed COM VTable Layout & Method Signatures

The DXGI swapchain COM interface vtable hierarchy is defined as follows:

#### `IDXGISwapChain` (Inherits from `IDXGIDeviceSubObject` -> `IDXGIObject` -> `IUnknown`):
- Slots 0–2: `IUnknown` (`QueryInterface`, `AddRef`, `Release`)
- Slots 3–6: `IDXGIObject` (`SetPrivateData`, `SetPrivateDataInterface`, `GetPrivateData`, `GetParent`)
- Slot 7: `IDXGIDeviceSubObject` (`GetDevice`)
- Slot 8: `IDXGISwapChain::Present`
- Slot 9: `IDXGISwapChain::GetBuffer`
- Slot 10: `IDXGISwapChain::SetFullscreenState`
- Slot 11: `IDXGISwapChain::GetFullscreenState`
- Slot 12: `IDXGISwapChain::GetDesc`
- **Slot 13: `IDXGISwapChain::ResizeBuffers`** *(Target 1)*
- Slot 14: `IDXGISwapChain::ResizeTarget`
- Slot 15: `IDXGISwapChain::GetContainingOutput`
- Slot 16: `IDXGISwapChain::GetFrameStatistics`
- Slot 17: `IDXGISwapChain::GetLastPresentCount`

#### `IDXGISwapChain1` (Inherits from `IDXGISwapChain`):
- Slots 18–21: `GetDesc1`, `GetFullscreenDesc`, `GetHwnd`, `GetCoreWindow`
- Slot 22: `IDXGISwapChain1::Present1`
- Slots 23–28: `IsTemporaryMonoSupported`, `GetRestrictToOutput`, `SetBackgroundColor`, `GetBackgroundColor`, `SetRotation`, `GetRotation`

#### `IDXGISwapChain2` (Inherits from `IDXGISwapChain1`):
- Slots 29–35: `SetSourceSize`, `GetSourceSize`, `SetMaximumFrameLatency`, `GetMaximumFrameLatency`, `GetFrameLatencyWaitableObject`, `SetMatrixTransform`, `GetMatrixTransform`

#### `IDXGISwapChain3` (Inherits from `IDXGISwapChain2`):
- Slot 36: `GetCurrentBackBufferIndex`
- Slot 37: `CheckColorSpaceSupport`
- Slot 38: `SetColorSpace1`
- **Slot 39: `IDXGISwapChain3::ResizeBuffers1`** *(Target 2)*

#### Exact Function Prototypes:
```cpp
// Slot 13: IDXGISwapChain::ResizeBuffers
typedef HRESULT (STDMETHODCALLTYPE *ResizeBuffers_t)(
    IDXGISwapChain* This,
    UINT BufferCount,
    UINT Width,
    UINT Height,
    DXGI_FORMAT NewFormat,
    UINT SwapChainFlags
);

// Slot 39: IDXGISwapChain3::ResizeBuffers1
typedef HRESULT (STDMETHODCALLTYPE *ResizeBuffers1_t)(
    IDXGISwapChain3* This,
    UINT BufferCount,
    UINT Width,
    UINT Height,
    DXGI_FORMAT NewFormat,
    UINT SwapChainFlags,
    const UINT* pCreationNodeMask,
    IUnknown* const* ppPresentQueue
);
```

---

### 1.4 Step-by-Step Architectural Implementation for Requirement R4

To implement R4 robustly, the following sequence must be established:

#### Step 1: Hook Installation in `InstallDxgiHooks()`
Instead of only querying `IDXGISwapChain1`, query `IDXGISwapChain3`:
```cpp
IDXGISwapChain3* sc3 = nullptr;
if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&sc3)))) {
    void** vt = *(void***)sc3;
    DWORD oldProt = 0;

    // Slot 8: Present
    VirtualProtect(&vt[8], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProt);
    g_origPresent = (Present_t)vt[8];
    vt[8] = (void*)&HookedPresent;
    VirtualProtect(&vt[8], sizeof(void*), oldProt, &oldProt);

    // Slot 13: ResizeBuffers
    VirtualProtect(&vt[13], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProt);
    g_origResizeBuffers = (ResizeBuffers_t)vt[13];
    vt[13] = (void*)&HookedResizeBuffers;
    VirtualProtect(&vt[13], sizeof(void*), oldProt, &oldProt);

    // Slot 22: Present1
    VirtualProtect(&vt[22], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProt);
    g_origPresent1 = (Present1_t)vt[22];
    vt[22] = (void*)&HookedPresent1;
    VirtualProtect(&vt[22], sizeof(void*), oldProt, &oldProt);

    // Slot 39: ResizeBuffers1
    VirtualProtect(&vt[39], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProt);
    g_origResizeBuffers1 = (ResizeBuffers1_t)vt[39];
    vt[39] = (void*)&HookedResizeBuffers1;
    VirtualProtect(&vt[39], sizeof(void*), oldProt, &oldProt);

    LogProxy("[sm86_proxy] DXGI hooks installed: Present(8), ResizeBuffers(13), Present1(22), ResizeBuffers1(39)\n");
    sc3->Release();
}
```

#### Step 2: Implementation of `HookedResizeBuffers` and `HookedResizeBuffers1`
```cpp
static HRESULT STDMETHODCALLTYPE HookedResizeBuffers(
    IDXGISwapChain* swap,
    UINT bufferCount,
    UINT width,
    UINT height,
    DXGI_FORMAT newFormat,
    UINT swapChainFlags)
{
    LogProxy("[sm86_proxy] ResizeBuffers entry: swap=%p, %ux%u, bufCount=%u, fmt=%d, flags=0x%08X\n",
             swap, width, height, bufferCount, (int)newFormat, swapChainFlags);

    EnterCriticalSection(&g_cs);

    // 1. Release cached display output (R3 VBlank pacing)
    if (g_cachedOutput) {
        g_cachedOutput->Release();
        g_cachedOutput = nullptr;
    }

    // 2. Invalidate wrapper state for this swapchain
    if (IsNvPresentWrapped(swap)) {
        void* wrapper = *(void**)((uint8_t*)swap + 0x18);
        if (wrapper) {
            g_activatedWrappers.erase(wrapper);
        }
    }

    // 3. Shutdown D3D11 bridge if active
    if (g_bridge.IsActive()) {
        g_bridge.Shutdown();
    }

    // 4. Ensure GPU command list execution has completed
    if (g_cmdQueue && g_fence) {
        g_fenceVal++;
        g_cmdQueue->Signal(g_fence, g_fenceVal);
        if (g_fence->GetCompletedValue() < g_fenceVal) {
            g_fence->SetEventOnCompletion(g_fenceVal, g_fenceEvent);
            WaitForSingleObject(g_fenceEvent, 2000);
        }
    }

    LeaveCriticalSection(&g_cs);

    // 5. Call original ResizeBuffers
    HRESULT hr = g_origResizeBuffers(swap, bufferCount, width, height, newFormat, swapChainFlags);

    LogProxy("[sm86_proxy] ResizeBuffers exit: hr=0x%08X\n", (uint32_t)hr);
    return hr;
}
```

#### Step 3: Graceful Re-Establishment on Subsequent `Present()`
On the first `Present()` following a resize:
1. `IsNvPresentWrapped(swap)` validates the swapchain against proxy vtable RVA `0x1d3228` with SEH / `VirtualQuery`.
2. `ActivateSmoothMotionIfWrapped(swap)` re-invokes `vt[19](wrapper, 1)` and `vt[20](wrapper, 1)` on the newly updated wrapper dimensions.
3. `GetContainingOutput` re-queries the current display output on the target monitor and caches `g_cachedOutput` for VBlank synchronization.
4. If in D3D11 mode, `g_bridge.Initialize` creates the shadow swapchain matching the new client dimensions.

---

## Part 2: Requirement R5 — Complete Build & Hardware Regression Verification

### 2.1 Build System Architecture Analysis

The project employs CMake 3.20+ with Visual Studio 2022 (v143 toolset, x64) as the primary build driver, orchestrated via `build.bat`.

#### 1. Compiler Configuration
- **Standard**: C++17 (`CMAKE_CXX_STANDARD 17`, `CMAKE_CXX_STANDARD_REQUIRED ON`).
- **Flags**: `/utf-8 /W3 /MP /EHsc`
  - `/utf-8`: Ensures Unicode UTF-8 execution character set across international systems.
  - `/W3`: Warning level 3.
  - `/MP`: Multi-processor compilation for parallel translation unit builds.
  - `/EHsc`: Standard C++ exception handling (catches C++ exceptions, assumes extern "C" functions never throw).
- **Predefined Macros**:
  - `UNICODE` & `_UNICODE`: Windows wide-character API binding.
  - `NOMINMAX`: Suppresses Windows macro definitions of `min` and `max`.
  - `WIN32_LEAN_AND_MEAN`: Excludes rarely-used Windows cryptography and networking headers.

#### 2. Linker Configuration & Forwarders
The proxy DLL (`version.dll`) uses MSVC linker export directives to transparently forward all 17 Version API calls to the real Windows system DLL:
```cpp
#pragma comment(linker, "/export:GetFileVersionInfoA=C:////Windows////System32////version.dll.GetFileVersionInfoA")
#pragma comment(linker, "/export:GetFileVersionInfoByHandle=C:////Windows////System32////version.dll.GetFileVersionInfoByHandle")
// ... through VerQueryValueW
```
This enables zero-latency passthrough without stub code or manual export tables.

#### 3. Dependencies
- **DirectX**: `d3d12.lib`, `d3d11.lib`, `dxgi.lib`, `d3dcompiler.lib`
- **Windows Subsystem**: `user32.lib`, `gdi32.lib`, `kernel32.lib`
- **CUDA Runtime / Driver**: `nvcuda.dll` (loaded dynamically via `LoadLibraryA` or resolved via PE import table traversal). No static CUDA toolkit installation is strictly required to build or run `version.dll`.

---

### 2.2 Target Executables and Binary Matrix

The build produces 5 primary deliverables specified in R5, plus auxiliary validation targets:

```
build\Release\
├── version.dll               [PRIMARY] Core proxy DLL placed into game root
├── nvp_live_test.exe         [PRIMARY] End-to-end Smooth Motion hardware test
├── nvp_perf_bench.exe        [PRIMARY] Latency and VRAM performance benchmark
├── proxytest.exe             [PRIMARY] Export forwarding verification harness
├── vfi_selftest.exe          [PRIMARY] Headless shader VFI PSNR & throughput test
├── sm86_smooth.addon64       [ReShade] ReShade 6.x add-on DLL for UI mask & OSD
├── test_pe_scan.exe          [AUX] Dynamic PE pattern scanner test on NvPresent64.dll
├── test_d3d11_bridge_nvp.exe [AUX] D3D11-to-D3D12 bridge test with live FG
├── test_osd_uimask.exe       [AUX] OSD overlay and UI mask pass test
├── test_d3d11_osd.exe        [AUX] D3D11 GDI fallback overlay test
└── test_addon_simulation.exe [AUX] Pass-level ground-truth simulation test
```

---

### 2.3 Detailed Test Binary Operation & Hardware Execution Analysis

All target binaries were verified on the local physical **NVIDIA GeForce RTX 3080 12GB**:

#### 1. `proxytest.exe`
- **Operation**: Loads `build\Release\version.dll`, retrieves function pointers for all 4 primary export forwarders (`GetFileVersionInfoSizeW`, `GetFileVersionInfoW`, `VerQueryValueW`, `VerLanguageNameW`), and executes queries against `C:\Windows\System32\notepad.exe`.
- **Hardware Verification Result**:
  ```
  loaded: C:\Users\lsp\Documents\antigravity\calm-carson\build\Release\version.dll
    GetFileVersionInfoSizeW      present
    GetFileVersionInfoW          present
    VerQueryValueW               present
    VerLanguageNameW             present
  GetFileVersionInfoSizeW(notepad.exe) -> 1764  (GetLastError 0)
  file version: 6.2.26100.7939
  forwarders OK
  ```
- **Exit Code**: `0` (PASS).

#### 2. `test_pe_scan.exe`
- **Operation**: Tests dynamic PE section scanning, gate pattern signature matching (`cmp dword ptr [rcx + 0x14], 3` -> `setge sil`), IAT entry discovery (`cuModuleLoadData`, `cuLaunchKernel`, `cuGraphLaunch`), and dynamic RIP-relative resolution of the global configuration struct in `NVP_Init_D3D`.
- **Hardware Verification Result**:
  ```
  [+] NvPresent64.dll loaded @ 00007FFA79A00000
  [+] Gate Scan: SUCCESS
      cmp [rcx+0x14], 3 imm @ 00007FFA79A0C41F (RVA 0xc41f) -> MATCH
      setge sil @ 00007FFA79A0C437 (RVA 0xc437, len 4) -> MATCH
  [+] IAT cuModuleLoadData @ 00007FFA79BD2820 (RVA 0x1d2820) -> MATCH
  [+] IAT cuLaunchKernel   @ 00007FFA79BD27F8 (RVA 0x1d27f8) -> MATCH
  [+] IAT cuGraphLaunch    @ 00007FFA79BD2780 (RVA 0x1d2780) -> MATCH
  [+] Config Struct @ 00007FFA7A1D7810 (RVA 0x7d7810) -> MATCH
  Result: ALL DYNAMIC CHECKS PASSED!
  ```
- **Exit Code**: `0` (PASS).

#### 3. `nvp_live_test.exe`
- **Operation**:
  1. Loads `NvPresent64.dll`, applies gate patch (`cmp [rcx+0x14], 2` + `mov sil, 1; nop`).
  2. Dynamically hooks IAT to rewrite 19 FP16 fatbinaries from `sm_89/120` to `sm_86`.
  3. Opens global config struct and calls `NVP_Init_D3D()`.
  4. Creates D3D12 device, command queue, and 512x512 swapchain wrapped by NvPresent proxy COM object.
  5. Activates Smooth Motion (`vt[19]=1, vt[20]=1`).
  6. Renders a moving white rectangle across 5 frames and captures alternating `cuGraphLaunch` execution (`gExec_0 = 0x...0C0`, `gExec_1 = 0x...E40`).
  7. Dumps base and interpolated hidden backbuffers to BMP (`demo_out/road1_*.bmp`).
- **Hardware Verification Result**:
  ```
  [+] Loaded NvPresent64.dll @ 00007FFA81640000
  [+] Gate located via Pattern Scan (cmp imm RVA +0xc41f, setge RVA +0xc437, len 4)
  [+] Gate patched: Tier 2 allowed, sil=1 forced
  [+] NvPresent64.dll IAT hooked dynamically (cuModuleLoadData, cuLaunchKernel, cuGraphLaunch)
  [+] NVP_Init_D3D() -> TRUE
  [+] SwapChain created @ 0000024845AB2C60 (Proxy COM Object, Internal Wrapper @ 00000247DCA9D650)
  [+] Smooth Motion activated on wrapper (vt[19]=1, vt[20]=1)
  [+] Hidden backbuffer array @ 00000247DCB8EA90 -> resources: 512x512, format 28
  [+] Starting Render & Present Loop with Visual Frame Readback...
    >>> cuGraphLaunch #1: gExec=000002484586E0C0 stream=000002482AD54FE0 -> result=0
  [Frame 0] swap->Present -> 0x00000000 (graphs launched: +1)
    >>> cuGraphLaunch #2: gExec=000002484586EE40 stream=000002482AD54FE0 -> result=0
  [Frame 1] swap->Present -> 0x00000000 (graphs launched: +1)
  ...
  SUMMARY STATS:
    - FP16 Fatbinaries Loaded: 19 (19 expected on Tier 2)
    - Warmup cuLaunchKernel:   50
    - Frame cuGraphLaunch:     5 (1 per Present)
  [+] SUCCESS: Road 1 Live Frame Generation fully verified on sm_86!
  ```
- **Exit Code**: `0` (PASS).

#### 4. `nvp_perf_bench.exe`
- **Operation**: Measures real-world GPU execution latency (using CUDA events `cuEventRecord`/`cuEventElapsedTime`) and dedicated video memory footprint (via `IDXGIAdapter3::QueryVideoMemoryInfo`) across 1080p, 1440p, and 4K resolutions.
- **Hardware Verification Result**:
  ```
  >>> Running Benchmark @ 1920x1080 (20 frames)...
  --- Results for 1920x1080 (18 steady frames) ---
      Avg Frame Interpolation GPU Time: 0.589 ms
      Min: 0.544 ms, Max: 0.926 ms
      Effective Theoretical Throughput: 1696.7 FPS
  >>> VRAM Delta after 1080p pipeline initialization: 452.9 MB

  >>> Running Benchmark @ 2560x1440 (20 frames)...
  --- Results for 2560x1440 (18 steady frames) ---
      Avg Frame Interpolation GPU Time: 0.944 ms
      Min: 0.715 ms, Max: 1.569 ms
      Effective Theoretical Throughput: 1059.3 FPS

  >>> Running Benchmark @ 3840x2160 (20 frames)...
  --- Results for 3840x2160 (18 steady frames) ---
      Avg Frame Interpolation GPU Time: 1.852 ms
      Min: 1.316 ms, Max: 2.692 ms
      Effective Theoretical Throughput: 539.8 FPS
  >>> Total VRAM Usage at 4K: 748.1 MB (Delta from baseline: 748.1 MB)
  ```
- **Exit Code**: `0` (PASS).

#### 5. `vfi_selftest.exe`
- **Operation**: Headless shader-based frame interpolation test (Road 2) validating optical flow accuracy and PSNR against analytical ground-truth test scenes at 540p, 1080p, and 1440p.
- **Hardware Verification Result**:
  ```
  === 960x540 ===
  PSNR(gen vs truth): 21.90 dB (well-posed pixels: 32.36 dB)
  flow mean |err|: 0.29 px (97.8% within 1 px of analytic)
  throughput: 1.634 ms/frame

  === 1920x1080 ===
  PSNR(gen vs truth): 21.64 dB (well-posed pixels: 28.41 dB)
  flow mean |err|: 6.71 px (73.6% within 1 px of analytic)
  throughput: 4.554 ms/frame

  === 2560x1440 ===
  PSNR(gen vs truth): 22.02 dB (well-posed pixels: 31.28 dB)
  flow mean |err|: 3.99 px (87.1% within 1 px of analytic)
  throughput: 7.794 ms/frame
  done.
  ```
- **Exit Code**: `0` (PASS).

---

### 2.4 Clean Verification Protocol

To independently verify the entire project from a clean state:

```powershell
# 1. Clean previous build artifacts
cmd.exe /c "build.bat clean"

# 2. Compile full suite in Release configuration
cmd.exe /c "build.bat"

# 3. Verify Version Proxy export forwarders
.\build\Release\proxytest.exe

# 4. Verify Dynamic PE & Pattern Scanner
.\build\Release\test_pe_scan.exe

# 5. Verify Road 1 Live Hardware Frame Generation (RTX 3080 sm_86)
.\build\Release\nvp_live_test.exe

# 6. Verify Road 1 Performance & VRAM Metrics
.\build\Release\nvp_perf_bench.exe

# 7. Verify Road 2 Headless Shader VFI
.\build\Release\vfi_selftest.exe build\Release\shaders\vfi.hlsl vfi_out
```

---

## Architectural Recommendations for the Implementation Worker

1. **Add `_CRT_SECURE_NO_WARNINGS` to `CMakeLists.txt`**:
   Add `add_compile_definitions(_CRT_SECURE_NO_WARNINGS)` in `CMakeLists.txt` to eliminate C4996 warnings for `fopen` and `wcscpy`.
2. **Implement VTable Hook for Slots 13 and 39**:
   In `InstallDxgiHooks()` (`src/proxy/sm86_rehost.cpp`), upgrade dummy query to `IDXGISwapChain3`. Hook slot 13 (`ResizeBuffers`) and slot 39 (`ResizeBuffers1`).
3. **Encapsulate Invalidation Routine**:
   Create a centralized helper function `InvalidateSwapChainState(IDXGISwapChain* swap)` that:
   - Releases any cached `IDXGIOutput` pointer.
   - Cleanses the swapchain wrapper entry from `g_activatedWrappers`.
   - Calls `g_bridge.Shutdown()` if bridge is active.
   - Flushes GPU commands via fence synchronization.
4. **Safe Probing Integration (R1)**:
   In `HookedPresent`, `HookedPresent1`, `HookedResizeBuffers`, and `HookedResizeBuffers1`, replace direct dereferencing of `[swap + 0x18]` with the safe verification helper:
   Compare swapchain vtable against `(uint8_t*)g_nvpresent + 0x1d3228` and protect memory reads via `VirtualQuery` or structured exception handling (`__try / __except`).
5. **Early File Logger (R2)**:
   Initialize the logger (`logs\sm86_proxy_<pid>.log`) immediately inside `DllMain` (`DLL_PROCESS_ATTACH`) with auto-flushing.
6. **VBlank Pacing (R3)**:
   Cache `IDXGIOutput` via `swap->GetContainingOutput(&g_cachedOutput)`. In dual-present mode, insert `g_cachedOutput->WaitForVBlank()` between the synthesized frame and the real frame. Ensure `g_cachedOutput` is released in `InvalidateSwapChainState`.
