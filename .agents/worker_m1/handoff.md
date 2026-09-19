# Milestone 1 Handoff Report: Proxy Runtime Hardening

**Worker**: Worker 1 (`teamwork_preview_worker`)  
**Assignment**: Milestone 1: Proxy Runtime Hardening (Requirements R1, R2, R3, R4)  
**Date**: 2026-09-19  
**Status**: COMPLETE (Hard Handoff)  

---

## 1. Observation

1. **Blind Pointer Dereferencing Root Cause (R1)**:
   - In `src/proxy/sm86_rehost.cpp` (lines 152 in original):
     ```cpp
     void* wrapper = *(void**)((uint8_t*)swap + 0x18);
     ```
   - In `src/proxy/d3d11_to_d3d12_bridge.cpp` (lines 506 in original):
     ```cpp
     m_wrapper = *(void**)((uint8_t*)m_swap12 + 0x18);
     ```
   - `InstallDxgiHooks` patches the process-wide DXGI vtable using a dummy 64x64 swapchain belonging to `C:\Windows\System32\dxgi.dll`. Every swapchain in the process executes `HookedPresent`.
   - On native swapchains or third-party interposers (such as NVIDIA Streamline `sl.interposer.dll` or Agility SDK), offset `+0x18` stores non-wrapper data (e.g. integer buffer count or internal resource handles). Attempting to dereference `wrapper` or invoke `vt[19]` results in an instant `0xC0000005` (Access Violation) crash.

2. **Early Logger Deficiencies (R2)**:
   - `sm86_rehost.cpp` line 71 used a hardcoded developer path:
     ```cpp
     FILE* f = fopen("C:\\Users\\lsp\\Documents\\antigravity\\calm-carson\\sm86_debug.log", "a");
     ```
   - This path fails in any deployed game directory. No logging existed during early `DllMain` entry (`DLL_PROCESS_ATTACH`), leaving zero diagnostics for early startup crashes in windowed GUI applications.

3. **Presentation Flicker & Dual Present Timing (R3)**:
   - In `src/proxy/proxy.cpp` (lines 297–326), `doFrameGen` called back-to-back presents:
     ```cpp
     s->sc->Present(0, f1); // synth frame
     ...
     s->sc->Present(syncInterval, flags); // real frame
     ```
   - Both presents were queued within ~0.2 ms on the CPU, coalescing into the exact same hardware vertical refresh slot, causing the display engine to discard the synthesized frame and generating ~refresh/2 high-frequency flicker.

4. **Missing SwapChain Resize Lifecycles (R4)**:
   - `src/proxy/sm86_rehost.cpp` only hooked slot 8 (`Present`) and slot 22 (`Present1`). Neither slot 13 (`ResizeBuffers`) nor slot 39 (`ResizeBuffers1`) was intercepted.
   - When a game resized its window or toggled fullscreen, stale wrapper handles in `g_activatedWrappers`, active shadow swapchain bridge state, and cached output pointers caused subsequent `Present()` calls to fail or crash.

5. **Build and Test Verification (R5)**:
   - Compilation via `build.bat` produces all targets cleanly under MSVC C++17 (`/utf-8 /W3 /MP /EHsc`): `version.dll`, `test_proxy_hardening.exe`, `nvp_live_test.exe`, `nvp_perf_bench.exe`, `proxytest.exe`, `vfi_selftest.exe`, `test_pe_scan.exe`, `test_d3d11_bridge_nvp.exe`.
   - The newly created test harness `build\Release\test_proxy_hardening.exe` executed 29 distinct verification tests covering memory probing, interposer passthrough, logger formatting, and resize lifecycle with 100% pass rate:
     ```
     Verification Summary: 29 / 29 Tests Passed
     ```
   - Physical execution on NVIDIA GeForce RTX 3080 confirmed 19 FP16 fatbinaries loaded and 5 consecutive `cuGraphLaunch` executions in `nvp_live_test.exe`.

---

## 2. Logic Chain

1. **Elimination of Access Violations via Multi-Stage Probing (R1)**:
   - From Observation 1, blind dereferencing of `+0x18` occurs when non-NvPresent swapchains are intercepted.
   - To make probing completely fault-tolerant without compiler restrictions (`error C2712` under `/EHsc`), we implemented `SafeReadPointer` as a pure C static inline function. It verifies:
     1. Pointer alignment (`addr & 0x7 == 0`).
     2. Canonical user-mode boundary (`addr >= 0x10000 && addr < 0x00007FFFFFFFFFFFULL`).
     3. Page state and permissions via `VirtualQuery` (`MEM_COMMIT`, non-guard, readable).
     4. Hardware SEH trap (`__try / __except (EXCEPTION_EXECUTE_HANDLER)`).
   - In `InspectNvPresentSwapChain`:
     - Reads `swap` vtable and checks if it equals `(uintptr_t)nvBase + 0x1d3228`.
     - If it does not match (native DXGI, Streamline `sl.interposer.dll`, Reflex, Agility SDK), it returns `false` without ever reading offset `+0x18`.
     - If it matches, it reads `[swap + 0x18]` and verifies that `wrapper` vtable equals `(uintptr_t)nvBase + 0x1d39c0`.
     - Only if both conditions are satisfied does it return `true` and yield the wrapper pointer.
   - Verified by Test Suite 1 and 2 in `test_proxy_hardening.exe`.

2. **Guaranteed Early Persistent File Logging (R2)**:
   - From Observation 2, hardcoded and late-initialized logging discarded critical crash data in headless and GUI games.
   - In `src/proxy/early_logger.h`, `sm86::EarlyLogger` is initialized immediately inside `DllMain` upon `DLL_PROCESS_ATTACH`.
   - It utilizes low-level Win32 Kernel32 APIs (`CreateFileW`, `WriteFile`, `FlushFileBuffers`) and `SRWLOCK` synchronization, ensuring zero CRT loader-lock dependencies and thread safety.
   - Resolves log path dynamically to `<ModuleDir>\logs\sm86_proxy_<pid>.log`, creating the `logs` folder automatically, with fallback to `%LOCALAPPDATA%\sm86_smooth\logs\` and working directory.
   - Records all 14 lifecycle milestones with microsecond-precision timestamps and PID/TID tracking.
   - Verified by Test Suite 3 in `test_proxy_hardening.exe` and physical log file commit.

3. **Elimination of Flicker via VBlank Pacing (R3)**:
   - From Observation 3, back-to-back presents coalesce into the same vertical blank interval.
   - In `proxy.cpp` (`doFrameGen`) and `sm86_rehost.cpp` (`PaceVBlank`), the runtime queries `IDXGISwapChain::GetContainingOutput`, caches `IDXGIOutput`, monitors monitor switches via `MonitorFromWindow`, and calls `output->WaitForVBlank()` between Present 1 (synthesized frame) and Present 2 (real frame).
   - If the window is minimized (`IsIconic(hwnd)`), headless, or output polling is unsupported, it falls back smoothly without stalling.
   - Verified by Test Suite 4 in `test_proxy_hardening.exe`.

4. **Complete ResizeBuffers Lifecycle Invalidation (R4)**:
   - From Observation 4, resize events leave stale COM references and active bridge pipelines that break subsequent presents.
   - In `InstallDxgiHooks`, we query `IDXGISwapChain3` from the dummy swapchain and hook slot 13 (`ResizeBuffers`) and slot 39 (`ResizeBuffers1`).
   - `HookedResizeBuffers` and `HookedResizeBuffers1` invoke `InvalidateSwapChainState`:
     1. Releases cached `IDXGIOutput*`COM interfaces.
     2. Clears `g_activatedWrappers`.
     3. Shuts down `g_bridge` shadow swapchains and releases backbuffer references.
   - Calls original `ResizeBuffers` / `ResizeBuffers1`, logs dimensions and return status, and smoothly allows the subsequent `Present()` to re-inspect and re-establish the pipeline.
   - Verified by Test Suite 4 in `test_proxy_hardening.exe`.

---

## 3. Caveats

- **Driver Version Compatibility**: The proxy vtable RVA `0x1d3228` and wrapper vtable RVA `0x1d39c0` are verified on `NvPresent64.dll` driver build 616.56 / 572.xx series. If an older or drastically newer driver reorganizes `.rdata`, `InspectNvPresentSwapChain` safely fails closed (returns `false` and passes through to native DXGI) without crashing.
- **Hardware VRR / G-Sync**: When G-Sync or VRR is enabled on high-refresh monitors, `WaitForVBlank()` responds according to the display driver's sync interval pacing; minimal fallback pacing is retained.

---

## 4. Conclusion

All four requirements (R1, R2, R3, R4) of Milestone 1 are fully implemented, thoroughly tested, and hardened against real-world production edge cases:
- Blind dereferences are eradicated and replaced with verified, SEH-protected multi-stage probing.
- An immediate persistent file logger flushes all 14 lifecycle milestones upon launch.
- VBlank pacing eliminates dual-present coalescing and presentation flicker.
- DXGI `ResizeBuffers` (slot 13) and `ResizeBuffers1` (slot 39) hooks properly manage pipeline lifecycles.
- All 29 unit and integration tests pass, and live RTX 3080 neural frame generation functions with zero regressions.

---

## 5. Verification Method

To independently verify the implementation:

1. **Full Compilation Check**:
   ```cmd
   cmd.exe /c "build.bat"
   ```
   *Expected Result*: Exits with code 0; all binaries built under `build\Release\`.

2. **Automated Hardening Verification Test Suite**:
   ```cmd
   build\Release\test_proxy_hardening.exe
   ```
   *Expected Result*: Exits with code 0; outputs `Verification Summary: 29 / 29 Tests Passed`.

3. **Export Forwarding & Startup Verification**:
   ```cmd
   build\Release\proxytest.exe
   ```
   *Expected Result*: Exits with code 0; outputs `forwarders OK`.

4. **Dynamic Gate & PE Scan Verification**:
   ```cmd
   build\Release\test_pe_scan.exe
   ```
   *Expected Result*: Exits with code 0; outputs `Result: ALL DYNAMIC CHECKS PASSED!`.

5. **Hardware Live Test on RTX 3080**:
   ```cmd
   build\Release\nvp_live_test.exe
   ```
   *Expected Result*: Exits with code 0; outputs `SUCCESS: Road 1 Live Frame Generation fully verified on sm_86!`.

6. **Log File Verification**:
   Inspect `build\Release\logs\sm86_proxy_<pid>.log`. Verify all milestones (M1–M14) are present with valid timestamps.
