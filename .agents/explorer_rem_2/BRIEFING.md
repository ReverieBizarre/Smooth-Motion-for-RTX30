# BRIEFING — 2026-09-19T13:01:30Z

## Mission
Investigate D3D11 swapchain resize re-entrancy crash and flip-model dummy swapchain creation in sm86_rehost.cpp, formulating robust fixes for Worker 2.

## 🔒 My Identity
- Archetype: explorer
- Roles: teamwork_preview_explorer
- Working directory: C:\Users\lsp\Documents\antigravity\calm-carson\.agents\explorer_rem_2
- Original parent: 713e396d-1dc4-417d-9c1c-1bfd09bee6b7
- Milestone: Milestone 1 Remediation (Iteration 2)

## 🔒 Key Constraints
- Read-only investigation — do NOT implement
- Do not modify source code directly
- Output reports to .agents/explorer_rem_2/
- Follow 5-component handoff protocol

## Current Parent
- Conversation ID: 713e396d-1dc4-417d-9c1c-1bfd09bee6b7
- Updated: 2026-09-19T13:01:30Z

## Investigation State
- **Explored paths**:
  - `src/proxy/sm86_rehost.cpp` (lines 260-295: HookedResizeBuffers, HookedResizeBuffers1; lines 567-640: InstallDxgiHooks)
  - `src/proxy/d3d11_to_d3d12_bridge.cpp` (Shutdown GPU sync, DiagCsv path)
  - `src/proxy/osd_overlay.cpp` (LogBridge path)
  - `tools/nvp_live_test.cpp` (safe wrapper probing)
  - `tests/test_challenger_stress.cpp` (Step 2.3 ResizeBuffers1 contract & Step 4 Live Proxy crash)
  - Windows SDK `shared/dxgi1_4.h` & live `dxgi.dll` vtable verification
- **Key findings**:
  - Swapchain recursion in `HookedResizeBuffers` causes `InvalidateSwapChainState` double-free, `0x887A0001` error, and `0xC0000005` heap corruption.
  - Adding `thread_local bool t_inResize = false;` completely breaks recursion.
  - In `dxgi.dll`, all swapchains share a single vtable (`0x7ffb715d3688`).
  - Calling `ResizeBuffers1` on D3D11 returns `DXGI_ERROR_INVALID_CALL` by DXGI design; D3D12 with queues is required for `ResizeBuffers1`.
  - Dummy swapchain in `InstallDxgiHooks` must use `DXGI_SWAP_EFFECT_FLIP_DISCARD` with idempotent hook checks.
  - `Shutdown()` requires `m_cq12->Signal(m_fence12)` and `WaitForSingleObject` to prevent async use-after-free.
  - Hardcoded paths in `osd_overlay.cpp` and `d3d11_to_d3d12_bridge.cpp` must be purged.
- **Unexplored areas**: None; all questions answered and concrete code changes formulated.

## Key Decisions Made
- Formulated exact drop-in code patches for Worker 2 in `exploration_report.md`
- Prepared 5-component handoff report in `handoff.md`

## Artifact Index
- DISPATCH.md — Task dispatch record
- BRIEFING.md — Situational awareness and state
- progress.md — Liveness heartbeat and task progress
- exploration_report.md — Detailed investigation findings and proposed patch
- handoff.md — 5-component handoff report
