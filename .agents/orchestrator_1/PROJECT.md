# Project: sm86_smooth Proxy Runtime Hardening

## Architecture
The `sm86_smooth` runtime (`version.dll`) is an advanced DirectX/DXGI presentation proxy and Frame Generation engine designed for Ampere RTX 30-series GPUs. It intercepts DXGI swapchain operations, hooks into `NvPresent64.dll`'s presentation pipeline, and injects hardware-accelerated Optical Flow / VFI frame synthesis using patched CUDA graphs.

### Module Boundaries & Data Flow
1. **Early Logger (`src/proxy/early_logger.h`, `early_logger.cpp`)**:
   - Initialized at `DllMain` entry (`DLL_PROCESS_ATTACH`) and startup thread.
   - Flushes milestone events immediately to `logs\sm86_proxy_<pid>.log`.
   - Thread-safe via `SRWLOCK` or `CRITICAL_SECTION`. Pure Win32 Kernel32 APIs (no C runtime buffering issues).
2. **Safe SwapChain Probing & Interposer Passthrough (`src/proxy/sm86_rehost.cpp`, `d3d11_to_d3d12_bridge.cpp`)**:
   - Pure C-style `SafeReadPointer` memory probing (`VirtualQuery` + hardware `__try / __except` SEH).
   - Validates that `*(void**)swap` matches NvPresent64 proxy swapchain vtable RVA `base + 0x1d3228`.
   - Validates that `*(void**)((uint8_t*)swap + 0x18)` matches wrapper object vtable RVA `base + 0x1d39c0`.
   - Unrecognized swapchains (native DXGI, Streamline `sl.interposer.dll`, Reflex, Agility SDK) bypass activation safely.
3. **ResizeBuffers Lifecycle Manager (`src/proxy/sm86_rehost.cpp`)**:
   - Intercepts `IDXGISwapChain::ResizeBuffers` (vtable slot 13) and `ResizeBuffers1` (vtable slot 39).
   - Pre-resize: Invalidates cached `IDXGIOutput` interfaces, clears `g_activatedWrappers`, resets `g_bridge` resources, and releases backbuffer references.
   - Forwards to original DXGI `ResizeBuffers` / `ResizeBuffers1`.
   - Post-resize: Smoothly re-establishes wrapper activation and bridge state on the next `Present()`.
4. **VBlank Pacing Engine (`src/proxy/proxy.cpp`)**:
   - Queries `IDXGISwapChain::GetContainingOutput` and caches `IDXGIOutput`.
   - In dual-present mode (`doFrameGen`: synthesized frame followed by real frame), inserts `output->WaitForVBlank()` between Present 1 and Present 2.
   - Eliminates frame coalescing into the same vertical refresh interval and resolves ~refresh/2 flicker.
   - Falls back gracefully to timer/QPC pacing when minimized or when output is unavailable.
5. **Hardware & E2E Verification (`test/`, `build.bat`, `nvp_live_test.exe`)**:
   - Regression test suite validating safe wrapper inspection, logger output, ResizeBuffers cycles, and VBlank fallback.
   - Clean MSVC C++17 build of all targets (`version.dll`, `nvp_live_test.exe`, `nvp_perf_bench.exe`, `proxytest.exe`, `vfi_selftest.exe`).
   - Live RTX 3080 execution verifying CUDA graph launches and FP16 fatbinary rewriting.

## Feature Inventory
| # | Feature | Description | Milestone | Source |
|---|---------|-------------|-----------|--------|
| F1 | Early Persistent File Logger | Creates `logs\sm86_proxy_<pid>.log` at DllMain attach with immediate flush | M1 | ORIGINAL_REQUEST §R2 |
| F2 | Lifecycle Milestone Logging | 14 lifecycle milestones logged with timestamp and PID | M1 | ORIGINAL_REQUEST §R2 |
| F3 | Safe Memory Probing Primitive | `SafeReadPointer` combining alignment, `VirtualQuery`, and pure C SEH | M1 | ORIGINAL_REQUEST §R1 |
| F4 | Proxy SwapChain VTable Verification | Checks `base + 0x1d3228` and wrapper `base + 0x1d39c0` before dereferencing | M1 | ORIGINAL_REQUEST §R1 |
| F5 | Interposer Passthrough | Zero crash pass-through for native DXGI, Streamline, Reflex, Agility SDK | M1 | ORIGINAL_REQUEST §R1 |
| F6 | D3D11-to-D3D12 Bridge Hardening | Safe wrapper inspection in `d3d11_to_d3d12_bridge.cpp:506` | M1 | ORIGINAL_REQUEST §R1 |
| F7 | ResizeBuffers Interception | Hooks `IDXGISwapChain::ResizeBuffers` (slot 13), invalidates cached state | M2 | ORIGINAL_REQUEST §R4 |
| F8 | ResizeBuffers1 Interception | Hooks `IDXGISwapChain::ResizeBuffers1` (slot 39), invalidates cached state | M2 | ORIGINAL_REQUEST §R4 |
| F9 | State Re-establishment | Re-establishes wrapper handles and bridge state smoothly on subsequent Present | M2 | ORIGINAL_REQUEST §R4 |
| F10 | Output Interface Caching | Caches `IDXGIOutput` via `IDXGISwapChain::GetContainingOutput` | M2 | ORIGINAL_REQUEST §R3 |
| F11 | VBlank Pacing for Dual Presents | Invokes `output->WaitForVBlank()` between Present 1 and Present 2 | M2 | ORIGINAL_REQUEST §R3 |
| F12 | VBlank Fallback Handling | Handles windowed, minimized, and `DXGI_ERROR_NOT_CURRENTLY_AVAILABLE` | M2 | ORIGINAL_REQUEST §R3 |
| F13 | Complete Clean MSVC C++17 Build | Zero errors via `build.bat` producing all targets | M3 | ORIGINAL_REQUEST §R5 |
| F14 | Local RTX 3080 Hardware Verification | Live test execution via `nvp_live_test.exe` verifying CUDA graphs & frame gen | M3 | ORIGINAL_REQUEST §R5 |

## Milestones
| # | Name | Scope | Dependencies | Status |
|---|------|-------|-------------|--------|
| M1 | Early File Logger & Safe Wrapper Probing | F1, F2, F3, F4, F5, F6 | none | IN_PROGRESS |
| M2 | ResizeBuffers Lifecycle & VBlank Pacing | F7, F8, F9, F10, F11, F12 | M1 | PLANNED |
| M3 | E2E Testing Suite & Hardware Verification | F13, F14 | M1, M2 | PLANNED |

## Interface Contracts
### Early Logger Contract (`src/proxy/early_logger.h`)
```cpp
void EarlyLog_Init();
void EarlyLog_Shutdown();
void EarlyLog_Write(const char* fmt, ...);
#define PROXY_LOG(fmt, ...) EarlyLog_Write(fmt, ##__VA_ARGS__)
```

### Safe Wrapper Probe Contract (`src/proxy/safe_probe.h` or `sm86_rehost.cpp`)
```cpp
bool SafeReadPointer(const void* address, void** outValue);
bool InspectNvPresentSwapChain(void* swap, uintptr_t nvpresentBase, void** outWrapper);
```

### ResizeBuffers Lifecycle Contract
```cpp
HRESULT STDMETHODCALLTYPE HookedResizeBuffers(IDXGISwapChain* swap, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags);
HRESULT STDMETHODCALLTYPE HookedResizeBuffers1(IDXGISwapChain* swap, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags, const UINT* pCreationNodeMask, IUnknown* const* ppPresentQueue);
void InvalidateSwapChainState(IDXGISwapChain* swap);
```

### VBlank Pacing Contract (`src/proxy/proxy.cpp`)
```cpp
void PaceVBlankBetweenPresents(IDXGISwapChain* swap);
```

## Code Layout
- `src/proxy/early_logger.h`, `src/proxy/early_logger.cpp`: Persistent file logging.
- `src/proxy/sm86_rehost.cpp`: Proxy initialization, DllMain, DXGI hooking, wrapper inspection, ResizeBuffers hooks.
- `src/proxy/d3d11_to_d3d12_bridge.cpp`: D3D11 interop bridge with safe wrapper inspection.
- `src/proxy/proxy.cpp`: Dual present orchestration and VBlank pacing.
- `tests/`: Automated unit and integration test harnesses for proxy hardening.
