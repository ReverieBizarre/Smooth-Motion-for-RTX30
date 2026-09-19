# Original User Request

## 2026-09-19T12:22:10Z

完整多智能体团队（启动多路线并行探索与全面重构，适合大范围功能扩展）

Refactor and harden the `sm86_smooth` proxy runtime (`version.dll`) to resolve real-world game crashes (GitHub Issue #1: unsafe swapchain wrapper pointer dereference, missing early file logger, ResizeBuffers lifecycle failures) and eliminate presentation flicker (GitHub Issue #2: vblank pacing between dual presents).

Working directory: `C:\Users\lsp\Documents\antigravity\calm-carson`
Integrity mode: development

## Requirements

### R1. Safe SwapChain Wrapper Identification & Fault-Tolerant Inspection
- Eliminate blind pointer dereferencing of `[swap + 0x18]`.
- Accurately determine whether an `IDXGISwapChain` is legitimately wrapped by `NvPresent64.dll` by comparing its vtable pointer against the known proxy vtable RVA (`base + 0x1d3228`), and wrap probing in structured memory/exception safeguards (`VirtualQuery` or `__try / __except`).
- If a swapchain is native (e.g. game directly managing DXGI) or wrapped by third-party interposers (such as NVIDIA Streamline `sl.interposer.dll`, Reflex, or Agility SDK), the hook must gracefully pass through without triggering `0xC0000005` access violations.

### R2. Early Persistent File Logging
- Implement an immediate, high-reliability file logger (`logs\sm86_proxy_<pid>.log`) initialized at `DllMain` entry and startup thread launch.
- Ensure all lifecycle milestones (module resolution, gate patching, IAT module hook, `NVP_Init_D3D` execution, DXGI Present/Present1 hooking, swapchain detection, and presentation status) are written and flushed immediately to disk, resolving the issue where crashes in GUI games without consoles left no trace.

### R3. VBlank Pacing for Dual Presents
- In dual-frame presentation mode (synthesized intermediate frame followed by real frame), eliminate back-to-back presents coalescing into the same vertical refresh slot.
- Query and cache the display output interface (`IDXGIOutput`) via `IDXGISwapChain::GetContainingOutput`, and invoke `output->WaitForVBlank()` between the two presents so that each frame lands on an adjacent VBlank interval, eliminating ~refresh/2 high-frequency flicker.

### R4. SwapChain ResizeBuffers Lifecycle Handling
- Intercept `IDXGISwapChain::ResizeBuffers` (vtable slot 13) and `ResizeBuffers1` (vtable slot 39).
- When a game resizes its swapchain or changes display modes, cleanly invalidate cached output pointers, wrapper handles, and internal texture associations, re-establishing them smoothly on the subsequent `Present()` call to prevent `swap_chain->Present() failed` errors.

### R5. Complete Build & Hardware Regression Verification
- Ensure the full codebase cleanly compiles under MSVC C++17 via `build.bat` (producing `version.dll`, `nvp_live_test.exe`, `nvp_perf_bench.exe`, `proxytest.exe`, `vfi_selftest.exe`) with zero errors.
- Run `nvp_live_test.exe` on the local RTX 3080 to verify that live CUDA graph launches and Smooth Motion frame generation execute without regression.

## Acceptance Criteria

### Stability & Safety
- [ ] Safe swapchain probing: zero `0xC0000005` crashes when encountering non-NvPresent swapchains, Streamline-interposed swapchains, or Agility SDK wrappers.
- [ ] Log file is created immediately in `logs\sm86_proxy_<pid>.log` upon application launch, recording all init and hook milestones with timestamps.
- [ ] VBlank pacing is executed via `WaitForVBlank()` between consecutive presents when an output device is available.
- [ ] `ResizeBuffers` and `ResizeBuffers1` hooks properly handle window resize and resolution changes without breaking subsequent `Present()` calls.
- [ ] Full project builds cleanly with `build.bat` and passes local hardware verification (`nvp_live_test.exe`).
