# AGENTS.md — AI Agent Onboarding Guide

> This file tells AI coding assistants (Antigravity, Claude, Copilot, etc.) everything they need to know to work effectively in this repository **without re-exploring from scratch**.

---

## Project Overview

**Smooth-Motion-for-RTX30** is a `version.dll` proxy that re-hosts NVIDIA's driver-level **Smooth Motion / DLSS Frame Generation** pipeline onto RTX 30-series (Ampere, `sm_86`) GPUs.

- Patches `NvPresent64.dll` in-memory at runtime — **zero system file modifications**.
- Works for any single-player DX12 game by dropping `version.dll` into the game folder.
- GitHub: `https://github.com/ReverieBizarre/Smooth-Motion-for-RTX30`

---

## Repository Layout

`
calm-carson/
├── src/
│   ├── proxy/
│   │   ├── sm86_rehost.cpp        # MAIN FILE: version.dll proxy, all hooks live here
│   │   ├── early_logger.h         # High-reliability Win32 file logger (SRWLock, WriteFile)
│   │   ├── d3d11_to_d3d12_bridge.cpp/.h  # D3D11 to D3D12 bridge for MPC-HC etc.
│   │   ├── osd_overlay.cpp/.h     # On-screen display overlay
│   │   ├── ui_mask.cpp/.h         # UI mask engine
│   │   └── pe_scan.h              # PE/IAT scanner utilities
│   ├── addon/
│   │   └── sm86_addon.cpp         # ReShade addon mode (requires imgui.h from ReShade SDK)
│   ├── nvof/
│   │   └── nvofapi.h              # NVOF API header
│   └── shaders/                   # HLSL shaders (osd, ui_mask, nvof_up)
├── tools/
│   ├── nvp_live_test.cpp          # End-to-end RTX hardware verification tool
│   ├── nvp_perf_bench.cpp         # Multi-resolution latency + VRAM benchmark
│   └── test_*.cpp                 # Various test harnesses
├── tests/
│   ├── test_proxy_hardening.cpp   # Safe pointer probing unit tests
│   ├── test_challenger_stress.cpp # Stress / concurrency tests
│   └── test_challenger_r1_probing.cpp
├── CMakeLists.txt                 # CMake build (MSVC, C++17, x64 Release)
├── build.bat                      # One-command build script
└── AGENTS.md                      # You are here
`

### Build Source vs Git Repo

The CMake build root is at:
`C:\Users\lsp\WorkBuddy\2026-09-10-18-04-16\sm86_smooth\`

This git repo is the working copy. After editing, sync and build:

`at
xcopy /E /Y /I src       C:\Users\lsp\WorkBuddy\2026-09-10-18-04-16\sm86_smooth\src
xcopy /E /Y /I tools     C:\Users\lsp\WorkBuddy\2026-09-10-18-04-16\sm86_smooth\tools
xcopy /E /Y /I tests     C:\Users\lsp\WorkBuddy\2026-09-10-18-04-16\sm86_smooth\tests
copy CMakeLists.txt      C:\Users\lsp\WorkBuddy\2026-09-10-18-04-16\sm86_smooth\
cd C:\Users\lsp\WorkBuddy\2026-09-10-18-04-16\sm86_smooth
build.bat
`

---

## Hardware Environment

| Item | Value |
|---|---|
| GPU | RTX 3080 12GB (Ampere, sm_86) |
| Driver | 616.56 |
| OS | Windows 11 |
| NvPresent64.dll | C:\Windows\System32\DriverStore\FileRepository\nv_dispi.inf_amd64_a3944b54ff18b284\NvPresent64.dll |
| GitHub account | ReverieBizarre |
| Token | Windows Credential Manager (git credential fill) |

---

## Key RVA Addresses (NvPresent64.dll, driver 616.56)

| Symbol | RVA | Description |
|---|---|---|
| Tier gate (cmp) | 0xc41f | cmp [rcx+0x14], 3 — patch to 2 to allow Tier 2 FP16 |
| VFI enable (setge sil) | 0xc437 | Patch: mov sil, 1; nop to force enable |
| IAT cuModuleLoadData | 0x1d2820 | Hook here to rewrite fatbin arch on the fly |
| Global config gate | base + 0x7d7810 | Set [+0x4c]=1, [+0xe9]=1 before NVP_Init_D3D() |
| Proxy swapchain vtable | base + 0x1d3228 | Used by InspectNvPresentSwapChain() |
| Internal wrapper vtable | base + 0x1d39c0 | Secondary verification of 0x1720-byte wrapper |
| Smooth Motion enable | vt[19](wrapper, 1) | Enable Smooth Motion on wrapper |
| Smooth Motion mode | vt[20](wrapper, 1) | Set mode on wrapper |
| ResizeBuffers vtable | slot 13 | Hooked by HookedResizeBuffers() |
| ResizeBuffers1 vtable | slot 39 | Hooked by HookedResizeBuffers1() |

---

## Core Architecture

### Startup Flow (sm86_rehost.cpp)

`
DllMain(DLL_PROCESS_ATTACH)
  └─ CreateThread → StartupThread()
       ├─ EarlyLogger::Initialize()          writes logs\sm86_proxy_<pid>.log
       ├─ LoadLibrary(NvPresent64.dll)
       ├─ patch_bytes(RVA 0xc41f)            tier gate: allow FP16 Tier 2
       ├─ patch_bytes(RVA 0xc437)            sil=1: force VFI enable
       ├─ Hook IAT cuModuleLoadData          fatbin arch rewriter
       ├─ config gate [0x7d7810]             unlock NVP_Init_D3D
       ├─ NVP_Init_D3D() → TRUE
       └─ Hook DXGI vtable slots 8,22,13,39  Present, Present1, ResizeBuffers x2
`

### Per-Frame Flow

`
HookedPresent(swap, SyncInterval, Flags)
  ├─ InspectNvPresentSwapChain(swap)
  │    ├─ SafeReadPointer(swap → vtable)     VirtualQuery + __try/__except
  │    ├─ Compare vtable == base+0x1d3228
  │    └─ SafeReadPointer(swap+0x18 → wrapper)
  ├─ if NvPresent: ActivateSmoothMotion()    vt[19]=1, vt[20]=1
  ├─ g_origPresent(swap, ...)                real Present, dispatches cuGraphLaunch
  └─ PaceVBlank(swap)                        WaitForVBlank between dual presents
`

---

## Safety Rules (NEVER violate)

1. **Never blindly dereference (void**)(swap + 0x18)** without first calling InspectNvPresentSwapChain().
   Causes 0xC0000005 in games using Streamline, Agility SDK, or native DXGI swapchains.

2. **Never hardcode absolute paths.** Use GetModuleFileNameW + GetEnvironmentVariableW(LOCALAPPDATA).

3. **Always wrap bare pointer reads** in SafeReadPointer() (VirtualQuery + SEH __try/__except).

4. **ResizeBuffers hooks must use TLS reentrancy guard** (thread_local bool t_inResize).

5. **Never modify any files under C:\Windows\** or in the DriverStore. All patches are in-memory.

6. **sm86_smooth_addon requires ReShade SDK** (imgui.h, reshade.hpp). Don't fix its build without the SDK.

7. **Always #include <intrin.h>** if using _ReturnAddress() in MSVC.

---

## Verification Sequence

`at
REM After every change:
build.bat
build\Release\nvp_live_test.exe
REM Expected: 5/5 frames PASS, 19 FP16 fatbins loaded, cuGraphLaunch x5 = CUDA_SUCCESS
`

---

## Release History

| Version | Changes |
|---|---|
| v1.0.0 | Initial release |
| v1.0.1 | Safe swapchain probing (R1), early file logger (R2), VBlank pacing (R3), ResizeBuffers lifecycle (R4) |

## Known Bug Patterns

| Symptom | Root Cause |
|---|---|
| 0xC0000005 in ActivateSmoothMotionIfWrapped | Missing InspectNvPresentSwapChain check |
| Present() fails after Alt+Enter / resize | InvalidateSwapChainState not called |
| Log file never created | EarlyLogger::Initialize() not called at DllMain |
| Flicker at ~refresh/2 | PaceVBlank() missing between dual presents |
| error C3861: _ReturnAddress | Missing #include <intrin.h> |
