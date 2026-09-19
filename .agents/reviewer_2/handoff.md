# Milestone 1 Handoff Report: Reviewer 2 (Hard Handoff)

**Agent**: Reviewer 2 (`teamwork_preview_reviewer`)  
**Roles**: Reviewer, Adversarial Critic  
**Date**: 2026-09-19  
**Milestone**: Milestone 1: Proxy Runtime Hardening  
**Verdict**: **APPROVE**  

---

## 1. Observation

1. **Compilation and Binary Generation**:
   - Executed `cmd.exe /c "build.bat"`. Exited with code 0 in 5.2 seconds.
   - Built targets confirmed present in `build\Release\`: `version.dll`, `test_proxy_hardening.exe`, `nvp_live_test.exe`, `nvp_perf_bench.exe`, `proxytest.exe`, `vfi_selftest.exe`, `test_pe_scan.exe`, `test_d3d11_bridge_nvp.exe`.

2. **Automated Test Suite Execution**:
   - Executed `build\Release\test_proxy_hardening.exe`. Exited with code 0.
   - 29 / 29 tests passed across all 4 suites:
     - Suite 1 (R1 SafeReadPointer): 15 tests passed (null rejection, alignment, low-page traps, canonical range, heap/stack, uncommitted/guard pages).
     - Suite 2 (R1 InspectNvPresentSwapChain): 6 tests passed (null swap, native DXGI passthrough, Streamline interposer passthrough, null wrapper, mismatched vtable, genuine NvPresent proxy confirmation).
     - Suite 3 (R2 Early Persistent File Logger): 5 tests passed (initialization, path check, physical file creation, milestone records, formatted timestamps).
     - Suite 4 (R3 & R4 VBlank Pacing & Resize): 3 tests passed (null safety, active swapchain pacing, hardware `ResizeBuffers` execution).

3. **Hardware Regression Verification**:
   - Executed `build\Release\nvp_live_test.exe` on local NVIDIA GeForce RTX 3080 GPU. Exited with code 0.
   - Confirmed 19 FP16 fatbinaries loaded, gate patched (`cmp [rcx+0x14], 2` and `mov sil, 1`), 5 consecutive `cuGraphLaunch` executions observed, and readback frame generation verified.

4. **Code Inspection**:
   - `src/proxy/early_logger.h`: `EarlyLog_Init` uses Win32 Kernel32 APIs (`CreateFileW`, `WriteFile`, `FlushFileBuffers`) with `SRWLOCK` thread-safety and fallback from `<ModuleDir>\logs\` to `%LOCALAPPDATA%\sm86_smooth\logs\`. Pure C `SafeReadPointer` handles memory probing with `VirtualQuery` and `__try / __except`.
   - `src/proxy/sm86_rehost.cpp`: Initialized immediately on `DLL_PROCESS_ATTACH` (line 850). Logs all 14 lifecycle milestones. Hooks DXGI slot 13 (`ResizeBuffers`) and slot 39 (`ResizeBuffers1`). `InvalidateSwapChainState` cleanly resets `g_cachedOutput`, `g_activatedWrappers`, and shuts down `g_bridge` shadow resources.
   - `src/proxy/proxy.cpp`: `paceVBlank` called between Present 1 (synthesized frame) and Present 2 (real frame) on line 361. Handles `IsIconic`, `GetContainingOutput`, and monitor switches.

---

## 2. Logic Chain

1. **R1 Safe Probing Elimination of 0xC0000005**:
   - Original crash occurred because non-NvPresent swapchains had arbitrary values at offset `+0x18`.
   - `InspectNvPresentSwapChain` now validates that `swap->vtable` matches `nvBase + 0x1d3228` before ever reading offset `+0x18`. Non-NvPresent and interposer swapchains return `false` immediately and pass through safely.
   - `SafeReadPointer` performs alignment, canonical range, and page protection checks, sealed with hardware SEH to prevent TOCTOU access violations.

2. **R2 Early Persistent Logging Reliability**:
   - Moving logger initialization to `DLL_PROCESS_ATTACH` before any other subsystem ensures crashes in early startup are recorded.
   - Eliminating CRT file streams (`FILE*`) avoids loader-lock and CRT initialization issues.
   - Milestone events trigger immediate unbuffered disk flushes via `FlushFileBuffers`.

3. **R3 VBlank Pacing Elimination of Flicker**:
   - Presenting twice in rapid succession (~0.2ms) causes display drivers to drop the intermediate frame or coalesce presentations into one VBlank interval.
   - In `proxy.cpp:361`, `paceVBlank` blocks on `output->WaitForVBlank()` between the presents, ensuring each frame occupies an individual refresh slot. Minimized or headless states fallback gracefully without stalling.

4. **R4 ResizeBuffers Lifecycle Integrity**:
   - Intercepting both slot 13 and slot 39 ensures window resize and display mode changes are captured.
   - `InvalidateSwapChainState` releases cached COM interfaces (`IDXGIOutput`) and shuts down the shadow swapchain before calling the original DXGI methods, preventing `DXGI_ERROR_INVALID_CALL` and stale texture pointer dereferences.

---

## 3. Caveats

1. **Unreferenced `PaceVBlank` in `sm86_rehost.cpp`**:
   - In `sm86_rehost.cpp:201`, `static void PaceVBlank(IDXGISwapChain* swap)` is implemented but not called inside `HookedPresent`. In Road 1, presentation timing is handled by NvPresent64 or shadow swapchain vsync (`shadowSync = (sync > 0) ? sync : 1`). Pacing between dual presents is active in Road 2 (`src/proxy/proxy.cpp:361`).
2. **NvPresent64 VTable RVAs**:
   - RVAs `0x1d3228` (proxy swapchain) and `0x1d39c0` (wrapper) are validated against NVIDIA driver 572.xx / 616.56. On mismatch, the code fails closed safely, falling back to native DXGI.

---

## 4. Conclusion

The code changes in Milestone 1 fully satisfy all requirements (R1, R2, R3, R4) with high engineering quality, robust defensive programming, and zero integrity violations.

**Verdict**: **APPROVE**

---

## 5. Verification Method

To reproduce and independently verify:

1. **Compile the full project**:
   ```cmd
   cmd.exe /c "build.bat"
   ```
   *Expected*: Code 0, all binaries produced in `build\Release\`.

2. **Run proxy hardening test suite**:
   ```cmd
   build\Release\test_proxy_hardening.exe
   ```
   *Expected*: Code 0, `Verification Summary: 29 / 29 Tests Passed`.

3. **Run hardware regression live test**:
   ```cmd
   build\Release\nvp_live_test.exe
   ```
   *Expected*: Code 0, 19 FP16 fatbinaries loaded, 5 cuGraphLaunch executions.

4. **Inspect log file**:
   Check `build\Release\logs\sm86_proxy_<pid>.log` for all 14 lifecycle milestones with formatted timestamps.
