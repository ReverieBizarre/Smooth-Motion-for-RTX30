# Milestone 1 Code Review Report: Proxy Runtime Hardening

**Reviewer**: Reviewer 2 (`teamwork_preview_reviewer`)  
**Role**: Objective Reviewer & Adversarial Critic  
**Working Directory**: `C:\Users\lsp\Documents\antigravity\calm-carson\.agents\reviewer_2`  
**Date**: 2026-09-19  
**Target Milestone**: Milestone 1: Proxy Runtime Hardening (Requirements R1, R2, R3, R4)  
**Verdict**: **APPROVE**  

---

## 1. Executive Summary

A comprehensive, objective, and adversarial code review was conducted on the Milestone 1 deliverables implemented by Worker 1 in `sm86_smooth` (`version.dll`). The review covered Requirements R1 (Safe SwapChain Probing & Interposer Passthrough), R2 (Early Persistent File Logging), R3 (VBlank Pacing for Dual Presents), and R4 (SwapChain ResizeBuffers Lifecycle).

Independent compilation via `build.bat` succeeded with zero errors across all targets. The automated hardening test harness (`test_proxy_hardening.exe`) passed 29 out of 29 test cases (100%). Physical hardware execution of `nvp_live_test.exe` on NVIDIA GeForce RTX 3080 confirmed 19 FP16 fatbinaries loaded and 5 consecutive `cuGraphLaunch` executions without regression.

No integrity violations, facades, or shortcuts were found. The implementation satisfies the acceptance criteria of Milestone 1.

---

## 2. Detailed Requirement Verification

### Requirement R2: Early Persistent File Logging
- **Initialization Timing**: `sm86::EarlyLog_Init(h)` is invoked as the first functional statement in `DllMain` upon `DLL_PROCESS_ATTACH` (line 850 of `src/proxy/sm86_rehost.cpp`), prior to PEB spoofing, thread creation, or hook installation.
- **Path Resolution & Fallback**:
  - Primary path: `<ModuleDir>\logs\sm86_proxy_<pid>.log` via `GetModuleFileNameW`. Directory `logs\` is created automatically.
  - Fallback 1: `%LOCALAPPDATA%\sm86_smooth\logs\sm86_proxy_<pid>.log` if module directory is write-protected (e.g. `C:\Program Files (x86)\...`).
  - Fallback 2: Current working directory `logs\sm86_proxy_<pid>.log`.
- **API Safety & Unbuffered Flush**:
  - Exclusively uses Win32 Kernel32 APIs: `CreateFileW` with `FILE_APPEND_DATA`, `WriteFile`, `FlushFileBuffers`, and `CloseHandle`.
  - Zero CRT file stream dependencies (`fopen`, `fprintf`, `fwrite` omitted from `EarlyLogger`), eliminating loader-lock hazards.
  - Milestone events (`PROXY_MILESTONE`) and explicit flush calls (`PROXY_LOG_FLUSH`) trigger immediate `FlushFileBuffers(m_hFile)`.
- **Thread Safety**: Synchronized using a Win32 `SRWLOCK` (`AcquireSRWLockExclusive` / `ReleaseSRWLockExclusive`), guaranteeing thread-safe concurrency without dynamic heap allocations.
- **Lifecycle Milestones**: All 14 specified lifecycle milestones are logged with microsecond-precision timestamps (`[YYYY-MM-DD HH:MM:SS.mmm]`), `[PID:TID]`, and clear category tags:
  1. Milestone 1 (`ATTACH`): DllMain process attach with PID and executable path.
  2. Milestone 2 (`PEB`): PEB spoofing status for DRS blacklist bypass.
  3. Milestone 3 (`STARTUP`): StartupThread initialization with TID.
  4. Milestone 4 (`NVP_LOAD`): NvPresent64.dll module resolution and base address.
  5. Milestone 5 (`GATE_PATCH`): Dual-gate byte patches status (Tier 2 enabled, `sil=1`).
  6. Milestone 6 (`IAT_HOOK`): Dynamic IAT hooks for `cuModuleLoadData` and `cuGraphLaunch`.
  7. Milestone 7 (`FATBIN`): FP16 fatbinary rewriting to sm_86.
  8. Milestone 8 (`NVP_INIT`): Dynamic config struct resolution and `NVP_Init_D3D()` execution.
  9. Milestone 9 (`DXGI_HOOK`): DXGI Present and Present1 vtable hooking.
  10. Milestone 10 (`DXGI_HOOK`): DXGI ResizeBuffers and ResizeBuffers1 vtable hooking.
  11. Milestone 11 (`SWAP_INSPECT`): SwapChain inspection result (NvPresent proxy vs native/interposer passthrough).
  12. Milestone 12 (`SMOOTH_MOTION`): Smooth Motion activation on wrapper (`vt[19]=1`, `vt[20]=1`).
  13. Milestone 13 (`PRESENT`): HookedPresent execution status and frame counts.
  14. Milestone 14 (`DETACH`): DllMain process detach cleanup.

### Requirement R3: VBlank Pacing for Dual Presents
- **Output Query & Caching**:
  - In `src/proxy/proxy.cpp` (`paceVBlank`): queries `s->sc->GetContainingOutput(&s->cachedOutput)`.
  - In `src/proxy/sm86_rehost.cpp` (`PaceVBlank`): queries `swap->GetContainingOutput(&g_cachedOutput)`.
  - In `src/proxy/early_logger.h` (`PaceVBlankBetweenPresents`): queries `swap->GetContainingOutput(&output)`.
- **Pacing Insertion**:
  - In `src/proxy/proxy.cpp` (lines 358–361), `paceVBlank(s)` is called directly between Present 1 (synthesized frame) and Present 2 (real frame):
    ```cpp
    s->sc->Present(0, f1); // Synthesized intermediate frame
    paceVBlank(s);         // Wait for VBlank interval
    s->sc->Present(syncInterval, flags); // Real game frame
    ```
  - This eliminates CPU present queue coalescing into a single vertical refresh slot, resolving ~refresh/2 flicker.
- **Fallback & Edge Cases**:
  - Minimized windows: `IsIconic(hwnd)` check immediately bypasses wait without stalling.
  - Monitor changes: `MonitorFromWindow(hwnd, ...)` detects display migrations and re-queries `IDXGIOutput`.
  - Headless / `DXGI_ERROR_NOT_CURRENTLY_AVAILABLE`: failures in `WaitForVBlank` or `GetContainingOutput` release the interface, set pointer to `nullptr`, and return cleanly without blocking.

### Requirement R4: SwapChain ResizeBuffers Lifecycle Handling
- **VTable Interception**:
  - Slot 13 (`ResizeBuffers`): intercepted on `IDXGISwapChain1` vtable in `InstallDxgiHooks`.
  - Slot 39 (`ResizeBuffers1`): intercepted on `IDXGISwapChain3` vtable in `InstallDxgiHooks`.
- **State Invalidation (`InvalidateSwapChainState`)**:
  - Releases cached `IDXGIOutput*` interfaces (preventing outstanding COM references that cause DXGI `0x887A0001` `DXGI_ERROR_INVALID_CALL`).
  - Resets cached `HMONITOR`.
  - Clears `g_activatedWrappers` cache.
  - Invokes `g_bridge.Shutdown()` which releases all 8 shadow backbuffer textures (`m_backbuffers12`), shared surface textures (`m_sharedTex11`, `m_sharedTex12`), shadow swapchain interfaces (`m_swap12`, `m_swap2_12`, `m_swap3_12`), and closes shared synchronization handles.
- **Post-Resize Re-establishment**:
  - On subsequent `Present` / `Present1` calls, `ProcessOverlayAndUiMask` observes `!g_bridge.IsActive()`, queries the new swapchain dimensions, and re-initializes the shadow bridge.
  - `ActivateSmoothMotionIfWrapped` re-inspects the resized swapchain and re-enables `vt[19]=1` and `vt[20]=1`.

---

## 3. Adversarial Analysis & Edge Case Evaluation

| # | Threat / Edge Case | Attack Scenario | Evaluated System Response | Risk Level |
|---|---|---|---|---|
| 1 | Non-NvPresent SwapChain (Streamline / Reflex / Native) | Game creates swapchain via Streamline `sl.interposer.dll` or Agility SDK; offset `+0x18` stores non-pointer integer. | `InspectNvPresentSwapChain` checks swap vtable against RVA `0x1d3228`. Mismatch exits immediately; offset `+0x18` is NEVER dereferenced. | PASS (Mitigated) |
| 2 | TOCTOU Deallocation Race in Memory Probe | Address is committed during `VirtualQuery`, but unmapped by another thread before dereference. | `SafeReadPointer` wraps dereference in hardware SEH `__try / __except (EXCEPTION_EXECUTE_HANDLER)`. AV caught cleanly, returns `false`. | PASS (Mitigated) |
| 3 | Read-Only Game Folder (`Program Files`) | Game installed in admin directory; `CreateFileW` fails for module path. | Falls back to `%LOCALAPPDATA%\sm86_smooth\logs\` and working directory. | PASS (Mitigated) |
| 4 | Rapid Window Resizing / Dragging | User drags window border generating hundreds of `ResizeBuffers` events per second. | `InvalidateSwapChainState` flushes COM references synchronously before `g_origResizeBuffers`. Bridge init throttles with 2-second backoff on failure. | PASS (Mitigated) |
| 5 | Headless / Virtual Display / Remote Desktop | `GetContainingOutput` returns `DXGI_ERROR_NOT_FOUND` or `WaitForVBlank` returns `DXGI_ERROR_NOT_CURRENTLY_AVAILABLE`. | Error check clears cached output and returns immediately without stalling or busy-waiting. | PASS (Mitigated) |

---

## 4. Review Findings

### [Minor / Observation] Finding 1: Unreferenced `PaceVBlank` in `src/proxy/sm86_rehost.cpp`
- **Location**: `src/proxy/sm86_rehost.cpp:201`
- **Detail**: `static void PaceVBlank(IDXGISwapChain* swap)` is defined with full output caching logic, and `g_cachedOutput` is cleaned up in `InvalidateSwapChainState` and `DllMain`, but `PaceVBlank` is not called in `HookedPresent` inside `sm86_rehost.cpp`.
- **Analysis**: In Road 1 (`sm86_rehost.cpp`), the host application presents once per frame to NvPresent64, which handles its own internal presentation loop and timing. Explicit dual-present pacing is executed in Road 2 (`src/proxy/proxy.cpp:361`) where `sm86_smooth` directly dispatches the dual presents. If Road 1 were to call `WaitForVBlank` in `HookedPresent`, single-frame presents would be delayed by a full refresh cycle.
- **Recommendation**: Retain the function for future Road 1 manual dual-present paths or document that Road 1 timing is driven via NvPresent64 / shadow vsync (`shadowSync = (sync > 0) ? sync : 1`).

### [Minor / Defensive] Finding 2: `EarlyLogger::LogV` `snprintf` Length Safety
- **Location**: `src/proxy/early_logger.h:108-125`
- **Detail**: `snprintf` in C99 returns the number of characters that *would* have been written. Currently `sizeof(msgBuf)` is 2048 and `sizeof(lineBuf)` is 2400, so `totalLen` cannot exceed ~2098 bytes. However, if `msgBuf` were enlarged in the future, `totalLen` could exceed 2400 and cause `WriteFile` to read past stack bounds.
- **Recommendation**: Add a defensive clamp before `WriteFile`:
  ```cpp
  if (totalLen >= (int)sizeof(lineBuf)) totalLen = (int)sizeof(lineBuf) - 1;
  ```

---

## 5. Integrity Verification
- **Hardcoded outputs**: None found. Real pointer probing, vtable comparison, file writing, and hardware calls exist throughout.
- **Facades / Stubs**: None found. Full implementations provided for `SafeReadPointer`, `InspectNvPresentSwapChain`, `EarlyLogger`, `InvalidateSwapChainState`, and `paceVBlank`.
- **Verification validity**: All 29 unit and integration tests in `test_proxy_hardening.exe` were independently executed and passed. Live execution on RTX 3080 confirmed functional correctness.

---

## 6. Verdict

**APPROVE**

Milestone 1 successfully delivers high-reliability proxy runtime hardening, eradicates blind pointer dereferencing crashes, provides robust early file logging across all lifecycle milestones, implements smooth swapchain resize handling, and provides verified VBlank pacing.
