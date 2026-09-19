## 2026-09-19T12:30:50Z

You are Worker 1 (teamwork_preview_worker) assigned to Milestone 1: Proxy Runtime Hardening (Requirements R1, R2, R3, R4) in `sm86_smooth` proxy runtime (`version.dll`).

Your working directory is:
`C:\Users\lsp\Documents\antigravity\calm-carson\.agents\worker_m1`
Create your BRIEFING.md and progress.md in your working directory.

MANDATORY FIRST STEP:
Read `C:\Users\lsp\Documents\antigravity\calm-carson\.agents\ORIGINAL_REQUEST.md` verbatim before starting work.
Read `C:\Users\lsp\Documents\antigravity\calm-carson\PROJECT.md`.
Read the Phase 0 Survey Reports for in-depth technical details:
- `C:\Users\lsp\Documents\antigravity\calm-carson\.agents\explorer_survey_1\survey_report.md` (Safe SwapChain probing & SEH)
- `C:\Users\lsp\Documents\antigravity\calm-carson\.agents\explorer_survey_2\survey_report.md` (Early logger & VBlank pacing)
- `C:\Users\lsp\Documents\antigravity\calm-carson\.agents\explorer_survey_3\survey_report.md` (ResizeBuffers hooks & build system)

MANDATORY INTEGRITY WARNING:
DO NOT CHEAT. All implementations must be genuine. DO NOT hardcode test results, create dummy/facade implementations, or circumvent the intended task. A teamwork_preview_auditor will independently verify your work. Integrity violations WILL be detected and your work WILL be rejected.

Scope & File Ownership:
You own implementation changes to:
- `src/proxy/early_logger.h` (and `src/proxy/early_logger.cpp` if needed)
- `src/proxy/sm86_rehost.cpp`
- `src/proxy/d3d11_to_d3d12_bridge.cpp`
- `src/proxy/proxy.cpp`
- `CMakeLists.txt` (if adding new source files like early_logger.cpp)

Detailed Implementation Requirements:

1. Requirement R1: Safe SwapChain Wrapper Identification & Fault-Tolerant Inspection:
   - In `src/proxy/sm86_rehost.cpp` and `src/proxy/d3d11_to_d3d12_bridge.cpp`:
   - Eliminate blind dereferencing of `[swap + 0x18]`.
   - Implement `SafeReadPointer` helper function:
     - Pure C-style static function using `__try / __except (EXCEPTION_EXECUTE_HANDLER)` to avoid MSVC C2712 compilation error with `/EHsc`.
     - Validates pointer alignment (8-byte aligned on x64).
     - Validates pointer address range (canonical user-mode memory).
     - Calls `VirtualQuery` to verify memory page is `MEM_COMMIT` and readable.
   - Implement `InspectNvPresentSwapChain`:
     - Checks if `swap` vtable pointer equals `(uintptr_t)g_nvpresent + 0x1d3228`.
     - If matched, safely reads `[swap + 0x18]` to obtain `wrapper`.
     - Checks if `wrapper` vtable pointer equals `(uintptr_t)g_nvpresent + 0x1d39c0`.
     - Only if both match, returns `true` and yields `wrapper`.
     - If not matched (native DXGI swapchain, Streamline `sl.interposer.dll`, Reflex, Agility SDK), return `false` without dereferencing or crashing.
   - In `ActivateSmoothMotionIfWrapped`: probe safely. If not NvPresent wrapper, return gracefully without crashing.
   - In `d3d11_to_d3d12_bridge.cpp:506`: replace blind dereference with `InspectNvPresentSwapChain`.

2. Requirement R2: Early Persistent File Logging:
   - Create `src/proxy/early_logger.h` (and `.cpp` if needed):
     - Log path: `logs\sm86_proxy_<pid>.log` relative to the proxy DLL/executable. Automatically create `logs\` directory using `CreateDirectoryW`. Fallback to `%LOCALAPPDATA%\sm86_smooth\logs\` or current directory if creation fails.
     - Immediate flush to disk (`FlushFileBuffers` on critical milestones) using Win32 Kernel32 APIs.
     - Thread-safe via `SRWLOCK` or `CRITICAL_SECTION`.
     - Formatted timestamps: `[YYYY-MM-DD HH:MM:SS.mmm] [PID:TID] [LEVEL] message`.
     - Initialized immediately in `DllMain` at `DLL_PROCESS_ATTACH`.
     - Log all 14 lifecycle milestones:
       1. DllMain attach (PID, process name, proxy version)
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
     - Replace legacy hardcoded debug log path in `sm86_rehost.cpp` with `PROXY_LOG`.

3. Requirement R3: VBlank Pacing for Dual Presents:
   - In `src/proxy/proxy.cpp` (`doFrameGen` / dual present path) and/or `sm86_rehost.cpp`:
   - Query and cache `IDXGIOutput` via `IDXGISwapChain::GetContainingOutput`.
   - In dual-frame presentation mode, invoke `output->WaitForVBlank()` between Present 1 (synthesized frame) and Present 2 (real frame) so each frame displays for a full vertical refresh period.
   - Handle fallback gracefully: if output is unavailable (`DXGI_ERROR_NOT_CURRENTLY_AVAILABLE` or null) or in windowed/minimized mode, fallback gracefully without stalling.

4. Requirement R4: SwapChain ResizeBuffers Lifecycle Handling:
   - In `src/proxy/sm86_rehost.cpp`:
   - Intercept `IDXGISwapChain::ResizeBuffers` (vtable slot 13) and `ResizeBuffers1` (vtable slot 39).
   - In `InstallDxgiHooks`: retrieve slot 13 and slot 39 from dummy swapchain (`IDXGISwapChain3` query for slot 39).
   - Implement `HookedResizeBuffers` and `HookedResizeBuffers1`:
     - Cleanly invalidate cached `IDXGIOutput` pointers, clear `g_activatedWrappers`, and reset `g_bridge` shadow resources before calling original methods.
     - Forward to original `ResizeBuffers` / `ResizeBuffers1`.
     - Log milestone with dimensions, buffer count, format, and return status.
     - Smoothly re-establish wrapper detection, bridge, and output caching on the subsequent `Present()` call.

5. Verification:
   - Run `build.bat` using run_command to verify MSVC C++17 compiles cleanly with zero errors.
   - Run existing test executables (`nvp_live_test.exe`, etc.) to confirm no regressions.

Deliverables:
- Write `changes.md` and `handoff.md` in `C:\Users\lsp\Documents\antigravity\calm-carson\.agents\worker_m1`.
- Send completion message to parent orchestrator with build/test results and report path.
