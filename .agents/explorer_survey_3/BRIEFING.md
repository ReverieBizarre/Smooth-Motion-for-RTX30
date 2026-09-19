# BRIEFING — 2026-09-19T12:29:30Z

## Mission
Explore and map the sm86_smooth proxy codebase focusing on Requirements R4 (SwapChain ResizeBuffers Lifecycle Handling) and R5 (Complete Build & Hardware Regression Verification).

## 🔒 My Identity
- Archetype: teamwork_preview_explorer
- Roles: Explorer 3, Codebase Investigation & Synthesis
- Working directory: C:\Users\lsp\Documents\antigravity\calm-carson\.agents\explorer_survey_3
- Original parent: 713e396d-1dc4-417d-9c1c-1bfd09bee6b7
- Milestone: Phase 0 Survey

## 🔒 Key Constraints
- Read-only investigation — do NOT implement
- Write only to your working folder: C:\Users\lsp\Documents\antigravity\calm-carson\.agents\explorer_survey_3
- Never place source code, tests, or data files in .agents/
- Deliverables: survey_report.md, handoff.md, message parent via send_message

## Current Parent
- Conversation ID: 713e396d-1dc4-417d-9c1c-1bfd09bee6b7
- Updated: not yet

## Investigation State
- **Explored paths**:
  - `ORIGINAL_REQUEST.md`
  - `build.bat`, `dev_build.bat`, `CMakeLists.txt`
  - `src/proxy/sm86_rehost.cpp`, `src/proxy/pe_scan.h`
  - `src/proxy/d3d11_to_d3d12_bridge.h`, `src/proxy/d3d11_to_d3d12_bridge.cpp`
  - `src/proxy/osd_overlay.h`, `src/proxy/osd_overlay.cpp`
  - `src/proxy/ui_mask.h`, `src/proxy/ui_mask.cpp`
  - `tools/proxytest.cpp`, `tools/test_pe_scan.cpp`, `tools/nvp_live_test.cpp`, `tools/nvp_perf_bench.cpp`, `tools/selftest.cpp`
  - `tools/test_d3d11_bridge_nvp.cpp`, `tools/test_addon_simulation.cpp`, `tools/test_osd_uimask.cpp`, `tools/test_d3d11_osd.cpp`
  - `README.md`, `FLICKER_ANALYSIS.md`, `RESHADE_ADDON_INSTALL.md`
- **Key findings**:
  - R4: Slot 13 (`ResizeBuffers`) and Slot 39 (`ResizeBuffers1`) are completely unhooked. Stale pointers in `g_activatedWrappers`, active bridge swapchains/textures, and unreleased `IDXGIOutput` pointers cause `DXGI_ERROR_INVALID_CALL` or `0xC0000005` on resize.
  - R4: Hooking `IDXGISwapChain3` allows hooking both slot 13 and slot 39, invalidating state before calling original, and re-establishing on subsequent `Present()`.
  - R5: Build system via `build.bat` works cleanly under MSVC C++17. All targets (`version.dll`, `nvp_live_test.exe`, `nvp_perf_bench.exe`, `proxytest.exe`, `vfi_selftest.exe`) build and execute with 100% pass rate on local RTX 3080.
- **Unexplored areas**: None within the assigned R4 & R5 scope.

## Key Decisions Made
- Prioritized verbatim reading of ORIGINAL_REQUEST.md.
- Built and ran all binaries on physical RTX 3080 hardware to verify regression status empirically.
- Outlined exact C++ prototypes, vtable indices, and invalidation pipeline in survey_report.md.

## Artifact Index
- C:\Users\lsp\Documents\antigravity\calm-carson\.agents\explorer_survey_3\survey_report.md — Comprehensive Survey Report for R4 & R5
- C:\Users\lsp\Documents\antigravity\calm-carson\.agents\explorer_survey_3\handoff.md — 5-Component Handoff Report
- C:\Users\lsp\Documents\antigravity\calm-carson\.agents\explorer_survey_3\DISPATCH.md — Initial dispatch log
- C:\Users\lsp\Documents\antigravity\calm-carson\.agents\explorer_survey_3\progress.md — Task tracker & heartbeat
