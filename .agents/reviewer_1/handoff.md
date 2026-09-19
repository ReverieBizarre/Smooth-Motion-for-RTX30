# Milestone 1 Handoff Report: Reviewer 1 (Runtime Hardening Review)

**Agent**: Reviewer 1 (`teamwork_preview_reviewer`)  
**Roles**: Reviewer, Adversarial Critic  
**Milestone**: Milestone 1: Proxy Runtime Hardening in `sm86_smooth` (`version.dll`)  
**Verdict**: **APPROVE**  
**Date**: 2026-09-19  

---

## 1. Observation

1. **Memory Probing & SEH Isolation (`src/proxy/early_logger.h:209-242`)**:
   - `SafeReadPointer` checks 8-byte alignment:
     ```cpp
     const uintptr_t addr = (uintptr_t)address;
     if ((addr & 0x7) != 0) return false;
     ```
   - Canonical 64-bit user address space bounds:
     ```cpp
     if (addr < 0x10000 || addr >= 0x00007FFFFFFFFFFFULL) return false;
     ```
   - Page commit and permission validation via `VirtualQuery`:
     ```cpp
     MEMORY_BASIC_INFORMATION mbi = {};
     if (VirtualQuery(address, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
     if (mbi.State != MEM_COMMIT) return false;
     const DWORD prot = (mbi.Protect & 0xFF);
     if (prot != PAGE_READONLY && prot != PAGE_READWRITE && prot != PAGE_WRITECOPY &&
         prot != PAGE_EXECUTE_READ && prot != PAGE_EXECUTE_READWRITE && prot != PAGE_EXECUTE_WRITECOPY) {
         return false;
     }
     if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
     ```
   - Isolated hardware SEH trap without C++ objects/destructors in scope (preventing MSVC C2712 under `/EHsc`):
     ```cpp
     __try {
         *outPtr = *(void* const*)address;
         return true;
     } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION 
                 ? EXCEPTION_EXECUTE_HANDLER 
                 : EXCEPTION_CONTINUE_SEARCH) {
         *outPtr = nullptr;
         return false;
     }
     ```

2. **Dual-VTable Probing & Interposer Passthrough (`src/proxy/early_logger.h:244-280`)**:
   - `InspectNvPresentSwapChain` checks swapchain vtable against `nvpresentBase + 0x1d3228` before dereferencing offset `+0x18`:
     ```cpp
     void* swapVtbl = nullptr;
     if (!SafeReadPointer(swap, &swapVtbl) || !swapVtbl) return false;
     if ((uintptr_t)swapVtbl != expectedProxyVtbl) return false;
     ```
   - If matched, reads `[swap + 0x18]` and validates `wrapper->vtable == nvpresentBase + 0x1d39c0`.
   - Native DXGI, Streamline (`sl.interposer.dll`), Reflex, and Agility SDK swapchains fail Step 2 and return `false` without dereferencing offset `+0x18`.

3. **Bridge Blind Dereference Elimination (`src/proxy/d3d11_to_d3d12_bridge.cpp:501-516`)**:
   - Replaced `m_wrapper = *(void**)((uint8_t*)m_swap12 + 0x18);` with `InspectNvPresentSwapChain(m_swap12, (uintptr_t)hNv, &wrapper)`.
   - Safely aborts bridge initialization returning `false` if the shadow swapchain is not wrapped by NvPresent.

4. **Early Persistent File Logger (`src/proxy/early_logger.h:18-169`)**:
   - Thread-safe class utilizing pure Win32 Kernel32 APIs (`CreateFileW`, `WriteFile`, `FlushFileBuffers`, `CloseHandle`) and `SRWLOCK`.
   - Multi-path resolution: `<ModuleDir>\logs\sm86_proxy_<pid>.log`, `%LOCALAPPDATA%\sm86_smooth\logs\`, and working directory.
   - Flushes all 14 lifecycle milestones with microsecond-precision timestamps.

5. **VBlank Pacing & ResizeBuffers Lifecycles (`src/proxy/proxy.cpp`, `src/proxy/sm86_rehost.cpp`)**:
   - `paceVBlank` invokes `output->WaitForVBlank()` between Present 1 and Present 2 in `doFrameGen`. Skips when minimized (`IsIconic`).
   - `HookedResizeBuffers` (slot 13) and `HookedResizeBuffers1` (slot 39) call `InvalidateSwapChainState`, releasing cached `IDXGIOutput`, clearing `g_activatedWrappers`, and shutting down bridge resources before invoking original DXGI resize.

6. **Build and Automated Test Verification**:
   - `cmd.exe /c "build.bat"` exited with code 0; all 13 targets compiled cleanly under MSVC C++17.
   - `build\Release\test_proxy_hardening.exe` exited with code 0: `Verification Summary: 29 / 29 Tests Passed`.
   - `build\Release\proxytest.exe` exited with code 0: `forwarders OK`.
   - `build\Release\test_pe_scan.exe` exited with code 0: `Result: ALL DYNAMIC CHECKS PASSED!`.
   - `build\Release\nvp_live_test.exe` exited with code 0 on RTX 3080: 19 FP16 fatbinaries loaded, 5 `cuGraphLaunch` executions verified.
   - Active log file inspected at `build\Release\logs\sm86_proxy_171656.log`: All milestone records present with timestamps.

---

## 2. Logic Chain

1. From Observation 1, memory probing validates 8-byte alignment, 64-bit canonical user-space boundaries, page commit status, and readable permissions prior to access, while isolating SEH within a pure C static function. This guarantees zero `0xC0000005` access violations even in the presence of multithreaded TOCTOU deallocation races, and prevents MSVC compiler error C2712 under `/EHsc`.
2. From Observation 2, `InspectNvPresentSwapChain` explicitly compares the swapchain's vtable against `(uintptr_t)nvBase + 0x1d3228` before attempting to access offset `+0x18`. Any non-NvPresent swapchain (native DXGI, Streamline, Reflex, Agility SDK) fails this initial equality check and returns `false` immediately, ensuring zero memory access to offset `+0x18` on foreign swapchains.
3. From Observation 3, the unsafe blind dereference at line 506 of `d3d11_to_d3d12_bridge.cpp` has been completely replaced with `InspectNvPresentSwapChain`, preventing crash hazards when shadow swapchains are unwrapped.
4. From Observation 4, `sm86::EarlyLogger` avoids CRT loader-lock deadlocks by using pure Win32 Kernel32 APIs, providing reliable milestone logging and disk flushes starting from `DLL_PROCESS_ATTACH`.
5. From Observation 5, dual-present coalescing and presentation flicker are eliminated by pacing consecutive presents across vertical refresh intervals using `output->WaitForVBlank()`, while `ResizeBuffers` and `ResizeBuffers1` interception guarantees clean pipeline teardown and re-establishment across resolution changes.
6. From Observation 6, independent compilation and hardware test execution verify that the hardened runtime functions with 100% test pass rate and zero regressions on the target RTX 3080 GPU.
7. No integrity violations (hardcoded test results, facade implementations, task bypasses, fabricated logs) were detected in any examined component.

---

## 3. Caveats

- **Driver VTable Offset Invariance**: Probing assumes `NvPresent64.dll` vtable RVAs `0x1d3228` and `0x1d39c0` (driver series 565-572 / 616.56). On untargeted driver versions where these RVAs differ, `InspectNvPresentSwapChain` safely fails closed (returns `false` and passes through to native DXGI) without crashing, but will not activate Smooth Motion.
- **VRR Display Behavior**: On G-Sync/FreeSync displays with variable refresh rate active, `WaitForVBlank()` timing is governed by the driver's adaptive sync frame presentation interval; fallback pacing operates cleanly if VBlank waits return `DXGI_ERROR_NOT_CURRENTLY_AVAILABLE`.

---

## 4. Conclusion

**Verdict**: **APPROVE**

Milestone 1 (Proxy Runtime Hardening) strictly meets all requirements (R1, R2, R3, R4) specified in `ORIGINAL_REQUEST.md` and `PROJECT.md`. The implementation is robust, defensive, fully verified by automated tests, and regression-free on physical Ampere RTX 3080 hardware.

---

## 5. Verification Method

To independently reproduce verification:
1. **Compile Full Solution**:
   ```cmd
   cmd.exe /c "build.bat"
   ```
   *Expected*: Code 0, builds `version.dll`, `test_proxy_hardening.exe`, `nvp_live_test.exe`, etc.
2. **Execute Hardening Verification Suite**:
   ```cmd
   build\Release\test_proxy_hardening.exe
   ```
   *Expected*: `Verification Summary: 29 / 29 Tests Passed`.
3. **Execute Dynamic Scan & Forwarding Tests**:
   ```cmd
   build\Release\proxytest.exe
   build\Release\test_pe_scan.exe
   ```
   *Expected*: `forwarders OK`, `Result: ALL DYNAMIC CHECKS PASSED!`.
4. **Execute Hardware Live Test (RTX 3080)**:
   ```cmd
   build\Release\nvp_live_test.exe
   ```
   *Expected*: `SUCCESS: Road 1 Live Frame Generation fully verified on sm_86!`.
5. **Inspect Log Output**:
   Check `build\Release\logs\sm86_proxy_<pid>.log` for all 14 lifecycle milestones.
