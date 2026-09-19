# Milestone 1 Code Changes Report

## Summary of Changes
Milestone 1 implements complete runtime hardening for the `sm86_smooth` proxy runtime (`version.dll`), resolving real-world game crashes (GitHub Issue #1: unsafe swapchain wrapper pointer dereference, missing early file logger, ResizeBuffers lifecycle failures) and eliminating presentation flicker (GitHub Issue #2: vblank pacing between dual presents).

---

## 1. `src/proxy/early_logger.h` (NEW)
- **Persistent Early File Logger (R2)**:
  - Created thread-safe `sm86::EarlyLogger` class utilizing pure Win32 Kernel32 APIs (`CreateFileW`, `WriteFile`, `FlushFileBuffers`, `CloseHandle`) to guarantee zero CRT startup dependencies under loader lock.
  - Dynamically resolves log destination to `<ModuleDir>\logs\sm86_proxy_<pid>.log` via `GetModuleFileNameW`. Automatically ensures directory creation via `CreateDirectoryW`.
  - Fallback logic cascades to `%LOCALAPPDATA%\sm86_smooth\logs\` and working directory.
  - Provides thread synchronization via lightweight `SRWLOCK`.
  - Timestamp formatting adheres to `[YYYY-MM-DD HH:MM:SS.mmm] [PID:TID] [LEVEL] message`.
  - Exposes C interface contracts: `EarlyLog_Init`, `EarlyLog_Shutdown`, `EarlyLog_Write`, `PROXY_LOG`, `PROXY_MILESTONE`.
- **Safe Memory Probing & Vtable Verification (R1)**:
  - Implemented `SafeReadPointer`: pure C-style static inline function safe under MSVC `/EHsc`. Validates 8-byte alignment, 64-bit user-mode canonical range (`0x10000` to `0x00007FFFFFFFFFFFULL`), page commit and readability via `VirtualQuery`, and hardware Structured Exception Handling (`__try / __except (EXCEPTION_EXECUTE_HANDLER)`).
  - Implemented `InspectNvPresentSwapChain`: checks if `swap` vtable matches `base + 0x1d3228` (proxy COM vtable). Only if matched, safely reads offset `+0x18` to obtain `wrapper`, and verifies `wrapper` vtable equals `base + 0x1d39c0`. Gracefully returns `false` on native DXGI, Streamline (`sl.interposer.dll`), Reflex, or Agility SDK without dereferencing or crashing.
- **VBlank Pacing Primitive (R3)**:
  - Implemented `PaceVBlankBetweenPresents`: queries `IDXGISwapChain::GetContainingOutput`, checks for windowed/minimized state, and executes `output->WaitForVBlank()` with graceful fallback.

---

## 2. `src/proxy/sm86_rehost.cpp` (MODIFIED)
- **Early Logger Integration & Milestone Telemetry (R2)**:
  - Included `early_logger.h`.
  - Replaced legacy hardcoded log path (`C:\Users\lsp\Documents\antigravity\calm-carson\sm86_debug.log`) with `PROXY_LOG`.
  - Initialized `EarlyLogger` immediately at `DllMain` entry (`DLL_PROCESS_ATTACH`) and logged all 14 lifecycle milestones:
    1. Attach (PID, process name, proxy version)
    2. PEB spoofing result
    3. Startup thread launch
    4. LoadNvPresent resolution & base address
    5. Dual-gate byte patches status
    6. Dynamic IAT hooks status
    7. Fatbin patch status
    8. NVP_Init_D3D config resolution
    9. DXGI Present / Present1 hook installation
    10. DXGI ResizeBuffers / ResizeBuffers1 hook installation
    11. SwapChain inspection result on each Present (Native vs NvPresent proxy)
    12. Smooth Motion activation (`vt[19]`, `vt[20]`)
    13. Presentation status and return code
    14. DllMain process detach
- **Safe Wrapper Inspection (R1)**:
  - In `ActivateSmoothMotionIfWrapped(swap)`: eliminated unchecked blind pointer dereference `*(void**)((uint8_t*)swap + 0x18)`. Replaced with `InspectNvPresentSwapChain(swap, nvBase, &wrapper)`.
  - If swapchain is native DXGI or third-party interposer, bypasses activation and passes through cleanly without crashing.
- **ResizeBuffers Lifecycle Interception (R4)**:
  - Added function typedefs `ResizeBuffers_t` (slot 13) and `ResizeBuffers1_t` (slot 39).
  - Implemented `InvalidateSwapChainState`: invalidates cached `IDXGIOutput`, resets `g_cachedMonitor`, clears `g_activatedWrappers`, and shuts down `g_bridge` shadow resources.
  - Implemented `HookedResizeBuffers` and `HookedResizeBuffers1`: logs resize parameters, invalidates state, forwards to original DXGI methods, and logs return status.
  - In `InstallDxgiHooks`: upgraded dummy swapchain query to `IDXGISwapChain3`. Hooked slot 8 (`Present`), slot 13 (`ResizeBuffers`), slot 22 (`Present1`), and slot 39 (`ResizeBuffers1`).
- **Thread Lifecycle Protection**:
  - In `StartupThread`: protected background worker thread with `GetModuleHandleExA` and `FreeLibraryAndExitThread` to prevent race conditions during dynamic `FreeLibrary` unloads.

---

## 3. `src/proxy/d3d11_to_d3d12_bridge.cpp` (MODIFIED)
- Included `early_logger.h` and redirected `LogBridge` to `EarlyLogger`.
- At line 501: replaced blind pointer dereference `m_wrapper = *(void**)((uint8_t*)m_swap12 + 0x18)` with `InspectNvPresentSwapChain(m_swap12, (uintptr_t)hNv, &wrapper)`. If shadow swapchain is not wrapped, logs a clean warning and aborts gracefully instead of crashing.

---

## 4. `src/proxy/proxy.cpp` (MODIFIED)
- **VBlank Pacing for Dual Presents (R3)**:
  - Added `cachedOutput` (`IDXGIOutput*`) and `cachedMonitor` (`HMONITOR`) to `struct SwapState`.
  - Cleaned up output interfaces in `releaseResources`.
  - Implemented `paceVBlank`: queries `GetContainingOutput`, monitors window and monitor migrations, and calls `cachedOutput->WaitForVBlank()` between Present 1 and Present 2. Minimized windows (`IsIconic`) and unavailable outputs fallback gracefully.
  - In `doFrameGen`: inserted `paceVBlank(s)` between Present 1 (synthesized frame) and Present 2 (real frame), eliminating back-to-back refresh slot coalescing and resolving high-frequency presentation flicker.
  - Implemented public contract `PaceVBlankBetweenPresents(IDXGISwapChain* swap)`.

---

## 5. `CMakeLists.txt` (MODIFIED)
- Added `test_proxy_hardening` target linking against `d3d11`, `dxgi`, and `user32`.

---

## 6. `tests/test_proxy_hardening.cpp` (NEW)
- Implemented comprehensive automated test suite covering 29 distinct test scenarios:
  - Suite 1 (R1 SafeReadPointer): null pointers, misalignments, low null-page memory traps, canonical boundaries, stack/heap reads, uncommitted pages, `PAGE_NOACCESS`, `PAGE_GUARD`.
  - Suite 2 (R1 InspectNvPresentSwapChain): null swap, native DXGI swapchain passthrough, Streamline interposer passthrough, proxy with null wrapper, proxy with corrupted wrapper vtable, genuine NvPresent proxy confirmation.
  - Suite 3 (R2 Early Persistent File Logger): initialization, milestone logging, physical disk existence, formatted timestamp validation `[YYYY-MM-DD HH:MM:SS.mmm] [PID:TID]`.
  - Suite 4 (R3 & R4 VBlank & Resize): null safety, active swapchain pacing, hardware `ResizeBuffers` execution.
- 100% test pass rate achieved (29 / 29).
