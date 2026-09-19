# E2E Test Infra: sm86_smooth Proxy Hardening

## Test Philosophy
- Opaque-box & regression verification derived directly from ORIGINAL_REQUEST.md.
- Verification covers:
  1. Safe SwapChain Wrapper identification & non-NvPresent/interposer pass-through (zero 0xC0000005 crashes).
  2. Early persistent file logger lifecycle milestone generation in `logs\sm86_proxy_<pid>.log`.
  3. VBlank pacing with display output synchronization between dual presents.
  4. SwapChain ResizeBuffers / ResizeBuffers1 lifecycle state invalidation and recovery.
  5. Clean MSVC C++17 build under `build.bat` and live RTX 3080 hardware validation (`nvp_live_test.exe`).

## Feature Inventory & Test Mapping
| # | Feature | Source | Tier 1 (Functional) | Tier 2 (Boundary/Fault) | Tier 3 (Interactions) | Tier 4 (Real-World) |
|---|---------|--------|:-------------------:|:-----------------------:|:---------------------:|:-------------------:|
| F1 | Early File Logger | R2 | Log created at attach | Read-only/missing dir | Concurrent threads | GUI game launch without console |
| F2 | Milestone Logging | R2 | All 14 milestones logged | High-frequency frames | Log flush during crash | Full proxy lifecycle |
| F3 | Memory Probe Primitive | R1 | Valid memory read | Page boundary, PAGE_NOACCESS | Null, unaligned, wild | Memory race during probe |
| F4 | Proxy VTable Check | R1 | Match base + 0x1d3228 | Shifted/invalid vtable | Dummy proxy mock | NvPresent64.dll runtime |
| F5 | Interposer Passthrough | R1 | Native swapchain passes | Streamline mock passes | Reflex/Agility wrapper | Combined interposer stack |
| F6 | D3D11 Bridge Safe Probe | R1 | Bridge probes safely | Bridge null wrapper | Multiple bridge instances | D3D11 game swapchain |
| F7 | ResizeBuffers Hook (slot 13) | R4 | Intercepts ResizeBuffers | Zero dimensions / flags | Resize -> Present | Resolution change in game |
| F8 | ResizeBuffers1 Hook (slot 39)| R4 | Intercepts ResizeBuffers1 | Queue mask variations | Resize1 -> Present | DXGI 1.4+ window resize |
| F9 | State Re-establishment | R4 | Cache cleared cleanly | Rapid resize sequence | Resize -> Rebind -> Gen | Stress window drag resize |
| F10| Output Caching | R3 | Output queried | Detached monitor | Multi-monitor move | Fullscreen to windowed |
| F11| VBlank Dual Pacing | R3 | WaitForVBlank invoked | Refresh rate timing | Back-to-back dual presents | 60Hz/120Hz/144Hz monitor |
| F12| VBlank Fallback | R3 | Fallback on error | DXGI_ERROR_NOT_CURRENTLY_AVAIL | Minimized window (IsIconic) | Background rendering |
| F13| Clean MSVC Build | R5 | Zero errors on build.bat | All 5 targets produced | Clean rebuild | MSVC C++17 environment |
| F14| RTX 3080 Hardware Test | R5 | nvp_live_test executes | 19 fatbins loaded | 5 cuGraphLaunches | Live frame generation |

## Test Architecture
- Test Runner: `cmd /c build.bat` followed by unit/hardening test execution and `.\build\Release\nvp_live_test.exe`.
- Pass / Fail Semantics: Exit code 0, all assertions pass, zero 0xC0000005 access violations, clean log output.
