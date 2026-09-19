## 2026-09-19T13:01:54Z

You are Worker 2 (teamwork_preview_worker) assigned to Milestone 1 Remediation (Iteration 2).

Your working directory is:
`C:\Users\lsp\Documents\antigravity\calm-carson\.agents\worker_rem_1`
Create your BRIEFING.md and progress.md in your working directory.

MANDATORY FIRST STEP:
Read `C:\Users\lsp\Documents\antigravity\calm-carson\.agents\ORIGINAL_REQUEST.md` verbatim before starting work.
Read `C:\Users\lsp\Documents\antigravity\calm-carson\PROJECT.md`.
Read the Remediation Reports from Explorers 1, 2, and 3:
- `C:\Users\lsp\Documents\antigravity\calm-carson\.agents\explorer_rem_1\exploration_report.md` (Audit path purge)
- `C:\Users\lsp\Documents\antigravity\calm-carson\.agents\explorer_rem_2\exploration_report.md` (ResizeBuffers re-entrancy & flip vtable)
- `C:\Users\lsp\Documents\antigravity\calm-carson\.agents\explorer_rem_3\exploration_report.md` (Bridge GPU sync & test hardening)
Read the Forensic Auditor's report:
- `C:\Users\lsp\Documents\antigravity\calm-carson\.agents\auditor_1\audit_report.md`
Read Challenger 2's report:
- `C:\Users\lsp\Documents\antigravity\calm-carson\.agents\challenger_2\challenge_report.md`

MANDATORY INTEGRITY WARNING:
DO NOT CHEAT. All implementations must be genuine. DO NOT hardcode test results, create dummy/facade implementations, or circumvent the intended task. A teamwork_preview_auditor will independently verify your work. Integrity violations WILL be detected and your work WILL be rejected.

Scope & Required Changes:
1. Re-entrancy Protection in `src/proxy/sm86_rehost.cpp`:
   - In `HookedResizeBuffers` and `HookedResizeBuffers1`:
     Add `static thread_local bool t_inResize = false;`.
     If `t_inResize` is true, immediately return `g_origResizeBuffers(swap, ...)` or `g_origResizeBuffers1(swap, ...)`.
     Set `t_inResize = true;` before calling `InvalidateSwapChainState`, and ensure `t_inResize = false;` is restored upon exit (using RAII or a finally block).
   - In `InstallDxgiHooks`:
     Update the dummy swapchain creation when hooking `ResizeBuffers1` (slot 39) to use `DXGI_SWAP_EFFECT_FLIP_DISCARD` (with WARP fallback), ensuring `ResizeBuffers1` binds to a genuine flip-model vtable. Add idempotency check before patching slot 39.
2. GPU Queue Synchronization in `src/proxy/d3d11_to_d3d12_bridge.cpp`:
   - In `D3D11ToD3D12Bridge::Shutdown()` (before releasing `m_swap12`, `m_backbuffers12`, and destroying `m_childHwnd`):
     Flush D3D11 device context: `m_ctx11->ClearState(); m_ctx11->Flush();`.
     Signal D3D12 command queue fence: `m_cq12->Signal(m_fence12, ++m_fenceVal12); m_fence12->SetEventOnCompletion(m_fenceVal12, m_fenceEvent12); WaitForSingleObject(m_fenceEvent12, 2000);`.
     Wait on `m_frameLatencyWaitable` if valid.
3. Purge Hardcoded File Paths (Forensic Audit Remediation):
   - In `src/proxy/osd_overlay.cpp:23`:
     Replace `fopen("C:\\Users\\lsp\\Documents\\antigravity\\calm-carson\\sm86_debug.log", "a")` in `LogBridge` with `PROXY_LOG` or `sm86::EarlyLogger::Instance().LogV(false, "OSD", fmt, args)`.
   - In `src/proxy/d3d11_to_d3d12_bridge.cpp:119`:
     Replace `fopen("C:\\Users\\lsp\\Documents\\antigravity\\calm-carson\\sm86_present.csv", "a")` in `DiagCsv` with dynamic path resolution relative to module directory (`<ModuleDir>\logs\sm86_present_<pid>.csv`) with `CreateDirectoryW` and `%LOCALAPPDATA%` fallback.
   - In `src/addon/sm86_addon.cpp:41`:
     Replace `fopen("C:\\Users\\lsp\\Documents\\antigravity\\calm-carson\\sm86_addon.log", "a")` with dynamic path resolution relative to module directory.
4. Hardening Test Tools:
   - In `tools/nvp_live_test.cpp:336` and `tools/nvp_perf_bench.cpp:144`:
     Replace blind `*(void**)((uint8_t*)swap + 0x18)` dereference with `InspectNvPresentSwapChain` from `early_logger.h`.
5. Build & Test Verification:
   - Run `cmd /c build.bat` to verify clean MSVC C++17 compilation with zero errors.
   - Run `build\Release\test_proxy_hardening.exe` (all 29 tests must pass).
   - Run `build\Release\test_challenger_stress.exe` (verify that the `ResizeBuffers` crash `0xC0000005` is completely resolved and all stress tests pass).
   - Run `build\Release\test_challenger_r1.exe` (all 48 tests must pass).
   - Run `build\Release\nvp_live_test.exe` on local RTX 3080 (live CUDA graph launches must execute without regression).

Deliverables:
- Write `changes.md` and `handoff.md` in your working directory.
- Send completion message to parent orchestrator with build and test outcomes.
