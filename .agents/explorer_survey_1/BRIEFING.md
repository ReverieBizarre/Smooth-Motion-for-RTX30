# BRIEFING — 2026-09-19T12:29:30Z

## Mission
Phase 0 Survey of sm86_smooth proxy codebase focusing on Requirement R1: Safe SwapChain Wrapper Identification & Fault-Tolerant Inspection.

## 🔒 My Identity
- Archetype: explorer
- Roles: explorer, investigator, synthesizer
- Working directory: C:\Users\lsp\Documents\antigravity\calm-carson\.agents\explorer_survey_1
- Original parent: 713e396d-1dc4-417d-9c1c-1bfd09bee6b7
- Milestone: Phase 0 - Discovery & Technical Architecture Survey (COMPLETED)

## 🔒 Key Constraints
- Read-only investigation — do NOT implement
- Do not modify source code in the main project tree
- Produce structured survey report at .agents/explorer_survey_1/survey_report.md
- Produce handoff report at .agents/explorer_survey_1/handoff.md
- Send message back to parent orchestrator (id: 713e396d-1dc4-417d-9c1c-1bfd09bee6b7, name: parent)

## Current Parent
- Conversation ID: 713e396d-1dc4-417d-9c1c-1bfd09bee6b7
- Updated: 2026-09-19T12:29:30Z

## Investigation State
- **Explored paths**:
  - `src/proxy/sm86_rehost.cpp` (DXGI hook installation, `ActivateSmoothMotionIfWrapped`, `HookedPresent`, `HookedPresent1`)
  - `src/proxy/d3d11_to_d3d12_bridge.cpp` (shadow swapchain wrapper inspection)
  - `src/proxy/pe_scan.h` (dynamic pattern scanning, gate patching, IAT hooking, DriverStore loader)
  - `tools/nvp_live_test.cpp` and `tools/nvp_perf_bench.cpp` (hardware test harnesses)
  - `NvPresent64.dll` PE disassembly and binary sections (.text, .rdata, vtable layout)
- **Key findings**:
  - Unsafe blind dereference at `src/proxy/sm86_rehost.cpp:152` (`[swap + 0x18]`) causes `0xC0000005` in games with native swapchains, sub-480p viewports, or third-party interposers (Streamline `sl.interposer.dll`, Reflex, Agility SDK).
  - VTable RVA `base + 0x1d3228` matches `NvPresent64.dll` proxy COM object uniquely (verified via PE disasm: `vt[8]` at RVA `0x35d80` begins with `mov rcx, [rcx + 0x18]`).
  - Internal wrapper vtable is at `base + 0x1d39c0` with `vt[19]` (enable Smooth Motion) and `vt[20]` (set present mode).
  - Safe memory probing combining MSVC x64 SEH (`__try / __except`) and `VirtualQuery` in pure C-style static helper functions resolves compiler error `C2712` under `/EHsc` and provides fault-tolerant probing with graceful passthrough.
- **Unexplored areas**: None for R1; downstream implementation ready.

## Key Decisions Made
- Concluded that a two-stage vtable check (Stage 1 on proxy vtable `base + 0x1d3228`, Stage 2 on wrapper vtable `base + 0x1d39c0`) combined with `SafeReadPointer` provides complete protection against `0xC0000005`.
- Verified that Streamline `sl.interposer.dll` and other interposers will cleanly pass through outer swapchains and activate Smooth Motion on the wrapped inner swapchain.

## Artifact Index
- `C:\Users\lsp\Documents\antigravity\calm-carson\.agents\explorer_survey_1\survey_report.md` — Comprehensive Technical Survey Report for R1
- `C:\Users\lsp\Documents\antigravity\calm-carson\.agents\explorer_survey_1\handoff.md` — 5-component handoff report
