# sm86_smooth: Unlocking NVIDIA Smooth Motion on Ampere (SM86)

[简体中文](README.md) | [English](README_en.md)

[![GitHub Release](https://img.shields.io/github/v/release/ReverieBizarre/Smooth-Motion-for-RTX30?style=flat-square&color=blue)](https://github.com/ReverieBizarre/Smooth-Motion-for-RTX30/releases/latest)
[![Build Status](https://img.shields.io/badge/build-passing-brightgreen.svg?style=flat-square)](#building--compiling)
[![Platform](https://img.shields.io/badge/platform-Windows%20x64-blue.svg?style=flat-square)](#prerequisites)
[![Architecture](https://img.shields.io/badge/target-NVIDIA%20Ampere%20(SM86)-76b900.svg?style=flat-square)](#key-reverse-engineering-breakthroughs-road-1)
[![Graphics API](https://img.shields.io/badge/API-Direct3D%2011%20%7C%20Direct3D%2012%20%7C%20DXGI-orange.svg?style=flat-square)](#architecture--injection-flow)
[![License](https://img.shields.io/badge/license-MIT%20%2F%20Academic%20Research-lightgrey.svg?style=flat-square)](#legal--provenance-disclaimer)

**`sm86_smooth`** unlocks NVIDIA's driver-level **Smooth Motion (DLSS Frame Generation / Video Frame Interpolation)** on **Ampere (GeForce RTX 30 Series / `sm_86`)** GPUs via clean, in-memory binary re-hosting of `NvPresent64.dll`. 

Previously gated by NVIDIA exclusively to Ada Lovelace (RTX 40 / `sm_89`) and Blackwell (RTX 50 / `sm_120`), this project proves that NVIDIA's proprietary neural frame synthesis pipeline can run natively on Ampere tensor cores **under 1 ms per frame** without modifying system driver files on disk.

## Table of Contents

- [Executive Summary & Verification Status](#executive-summary--verification-status)
- [Smooth Motion vs. DLSS-G](#smooth-motion-vs-dlss-g)
- [Key Reverse Engineering Breakthroughs](#key-reverse-engineering-breakthroughs)
  - [1. The Dual-Gate Architecture Classifier Bypass](#1-the-dual-gate-architecture-classifier-bypass)
  - [2. Fatbinary Dynamic IAT Hooking & Dual Header Patching](#2-fatbinary-dynamic-iat-hooking--dual-header-patching)
  - [3. FP16 vs. FP8 Kernel Isolation (Dodging QMMA 715 Traps)](#3-fp16-vs-fp8-kernel-isolation-dodging-qmma-715-traps)
  - [4. The 5-Object COM Swapchain Hierarchy & Activation](#4-the-5-object-com-swapchain-hierarchy--activation)
  - [5. CUDA Graph Ping-Pong Execution & Resolution Guard](#5-cuda-graph-ping-pong-execution--resolution-guard)
- [Empirical Benchmarks on RTX 3080 Hardware](#empirical-benchmarks-on-rtx-3080-hardware)
- [Architecture & Injection Flow](#architecture--injection-flow)
- [Repository Structure](#repository-structure)
- [Building & Compiling](#building--compiling)
- [Deployment & Usage Guide](#deployment--usage-guide)
- [Live Verification & Diagnostics](#live-verification--diagnostics)
- [Anti-Cheat & Safety Warning](#anti-cheat--safety-warning)
- [Legal & Provenance Disclaimer](#legal--provenance-disclaimer)

---

## Executive Summary & Verification Status

| Dimension | Metric / Hardware Result |
|---|---|
| **Core Mechanism** | In-memory IAT hook & runtime binary re-hosting of `NvPresent64.dll` (Zero system file modification) |
| **GPU Hardware** | Ampere Tensor Cores (`sm_86`, GeForce RTX 30 Series) |
| **1080p GPU Latency** | **0.622 ms** (~1607 FPS throughput) |
| **1440p GPU Latency** | **0.759 ms** (~1317 FPS throughput) |
| **4K GPU Latency** | **1.900 ms** (~526 FPS throughput) |
| **VRAM Footprint** | ~419 MB (1080p) to ~526 MB (4K) bounded footprint |
| **CUDA Dependency** | Leverages host driver's `nvcuda.dll`; no CUDA Toolkit installation needed |
| **Motion Vectors** | Self-contained optical flow CNN; **no game-provided MVs or depth buffer required** |
| **Verification State** | **100% Verified on RTX 3080 12GB**: 19/19 FP16 fatbinaries loaded, 50/50 warmup kernels passed, live `cuGraphLaunch` execution, verified BMP frame readback |

---

## Smooth Motion vs. DLSS-G

A common misconception is that "DLSS Frame Generation" and "Smooth Motion" are the same technology. They are fundamentally different architectural approaches:

```
+-----------------------------------------------------------------------------------+
| DLSS-G (DLSS 3 Frame Generation via Streamline / nvngx_dlssg.dll)                 |
| Requires: Game Engine Motion Vectors (DLSSG.MVecs), Depth Buffer, Camera Matrices |
| Re-hosting project: dlssg_for_sm86 (NVIDIA already ships Ampere cubins for this!)  |
+-----------------------------------------------------------------------------------+
                                        vs.
+-----------------------------------------------------------------------------------+
| Smooth Motion (Driver-Level Frame Interpolation via NvPresent64.dll)              |
| Operates on: Raw Backbuffers at DXGI Present. ZERO Game Engine MVs required!      |
| NVIDIA Driver Gate: Strictly sm_89 (Ada) and sm_120 (Blackwell). sm_86 blocked!    |
| Re-hosting project: sm86_smooth (THIS PROJECT - Cracked & Hardware Verified)      |
+-----------------------------------------------------------------------------------+
```

1. **DLSS-G (`nvngx_dlssg.dll`)**: Requires deep game-engine integration. The game must supply motion vectors, depth, and projection matrices. NVIDIA already compiles the core CNN blocks (`custom_block*`, `upsample_hf`) for `sm_86` inside their NGX packages.
2. **Smooth Motion (`NvPresent64.dll`)**: Operates universally at the DXGI presentation layer on arbitrary swapchains. It computes bidirectional optical flow and synthesizes intermediate frames purely from pixel backbuffers. NVIDIA omitted `sm_86` images entirely and enforced hard driver-level architecture gates.

`sm86_smooth` brings **Smooth Motion** to Ampere, making driver-level frame generation accessible to single-player games without game-engine modding or motion vector feeds.

---

## Key Reverse Engineering Breakthroughs (Road 1)

`NvPresent64.dll` (located in the NVIDIA Driver Store) was reverse-engineered to identify why Ampere was locked out and how to overcome the restriction.

```
                  +------------------------------------------------+
                  |              Game Process                      |
                  +------------------------------------------------+
                                          |
                                          | (Loads version.dll proxy)
                                          v
+-----------------------------------------------------------------------------------+
| sm86_smooth (version.dll)                                                         |
|                                                                                   |
|  [1] Gate Patch:                                                                  |
|      0xc41f: cmp [rcx+0x14], 3 -> 2   (Tier 2 Ampere Allowed)                    |
|      0xc437: setge sil -> mov sil, 1; nop  (Master Capability Flag Forced)       |
|                                                                                   |
|  [2] IAT Hook on NvPresent64.dll:                                                 |
|      cuModuleLoadData (RVA 0x1d2820) -> hook_cuModuleLoadData                     |
|      -> Scans 0xba55ed50 fatbinaries in-memory                                    |
|      -> Rewrites arch 0x78/0x59 -> 0x56                                           |
|      -> Rewrites inner ELF e_flags -> 0x560556                                    |
|      -> Result: 19 FP16 Fatbinaries Load with 0 CUDA_SUCCESS                      |
|                                                                                   |
|  [3] Global Config Gate:                                                          |
|      Config @ base+0x7d7810: S[0x4c]=1, S[0xe8]=1, S[0xe9]=1, S[0x12a5]=1         |
|      Calls NVP_Init_D3D() -> Returns TRUE, installs DXGI Detours                  |
|                                                                                   |
|  [4] SwapChain Wrapper Detour:                                                    |
|      Intercepts Present() / Present1()                                            |
|      Detects 0x1720-byte wrapper at [swap + 0x18]                                 |
|      vt[19](wrapper, 1) & vt[20](wrapper, 1) -> Activates Smooth Motion          |
|      Dispatches Optical Flow CNN & cuGraphLaunch on Ping-Pong Execution Streams   |
+-----------------------------------------------------------------------------------+
```

### 1. The Dual-Gate Architecture Classifier Bypass

During device creation, `NvPresent64.dll` queries `cuDeviceGetAttribute` for `CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR` (75) and `MINOR` (76), computing an internal tier:
- `sm_75` (Turing) $\rightarrow$ Tier 1
- `sm_86` (Ampere) $\rightarrow$ Tier 2
- `sm_89` (Ada Lovelace) $\rightarrow$ Tier 3
- `sm_120` (Blackwell) $\rightarrow$ Tier 4

At RVA `0xc41c`, the gate function executes:
```x86asm
83 79 14 03       cmp dword ptr [rcx+0x14], 3   ; Is Tier >= 3 (Ada/Blackwell)?
0f 4d eb          cmovge ebp, ebx               ; ebp=0 (Success) if tier >= 3, else ebp=4 (Unsupported)
0f 9d c6          setge sil                     ; sil=1 if tier >= 3, else sil=0
```

#### The Trap & The Dual Patch:
1. **Immediate `cmp` patch**: Patching offset `0xc41f` from `0x03` to `0x02` permits Tier 2 (Ampere) into `cmovge ebp, ebx`, clearing the error code to `ebp = 0`.
2. **Master Capability Flag (`sil`)**: Early reverse engineering attempts set `sil = 0` to disable FP8. However, `sil` is the master boolean return value stored in `[wrapper + 0x3a]`. If `sil == 0`, `NvPresent64.dll` silently aborts all CUDA graph allocation and swapchain wrapping!
3. **The Solution**: Patch RVA `0xc437` to `40 B6 01 90` (`mov sil, 1; nop`). Both conditions are satisfied: error code cleared and capability authorized.

---

### 2. Fatbinary Dynamic IAT Hooking & Dual Header Patching

`NvPresent64.dll` embeds 37 fatbinaries (`0xba55ed50`). Each contains both `sm_89` and `sm_120` ELF images.

When loading an unpatched fatbinary on Ampere, `cuModuleLoadData` fails with error `209 CUDA_ERROR_NO_BINARY_FOR_GPU`. 

**Why patching only the inner ELF `e_flags` fails**:
The CUDA driver's fatbinary container parser inspects the **16-byte fatbinary entry headers** first. Offset `+0x2c` contains `0x78` (`sm_120`) and `+0x22fc` contains `0x59` (`sm_89`). If these container architecture tags do not match the GPU, the driver rejects the entire container before reading the ELF header!

**The In-Memory IAT Hook Solution**:
`sm86_smooth` intercepts `cuModuleLoadData` via `NvPresent64.dll`'s Import Address Table (IAT) at RVA `0x1d2820`:
```cpp
static void patch_fatbin(uint8_t* b, size_t size) {
    // 1. Patch container entry architecture headers
    for (size_t off = 0; off + 3 < size; off += 4)
        if ((b[off] == 0x78 || b[off] == 0x59) && b[off+1] == 0 && b[off+2] == 0 && b[off+3] == 0)
            b[off] = 0x56; // sm_86

    // 2. Patch embedded ELF e_flags
    for (size_t off = 0; off + 0x34 < size; ++off)
        if (b[off] == 0x7f && b[off+1] == 'E' && b[off+2] == 'L' && b[off+3] == 'F') {
            uint32_t fl = 0x560556; // sm_86 e_flags
            memcpy(b + off + 0x30, &fl, 4);
        }
}
```
At runtime, whenever `NvPresent64.dll` invokes `cuModuleLoadData`, the proxy intercepts the memory pointer, patches the architecture tags in a private buffer, and passes it to `nvcuda.dll`. **All 19 FP16 fatbinaries load with `0 CUDA_SUCCESS` without touching driver files on disk.**

---

### 3. FP16 vs. FP8 Kernel Isolation (Dodging QMMA 715 Traps)

`NvPresent64.dll` maintains a master table of 36 fatbinary pointers at VA `0x18022ead0`:
- Entries `[0..18]`: **19 FP16 Fatbinaries** (`conv1..8`, `conv_fused`, `conv_proj1/2`, `conv_out1/2/3`, `attn1/2`, `depth_to_space`, `downscale_kernel`, `warp_coarse_kernel`, `main_kernel`).
- Entries `[19..35]`: **17 FP8 Fatbinaries** (`*_fp8`).

#### Hardware SASS Verification on RTX 3080:
| Kernel Type | Opcode | `sm_89` Unpatched | `sm_89` Patched to `sm_86` | Hardware Execution Result on Ampere |
|---|---|---|---|---|
| **FP16 MMA** | `HMMA.16816.F32` | 209 `NO_BINARY` | 0 `CUDA_SUCCESS` | **0 `CUDA_SUCCESS`** (128/128 elements identical to native sm_86) |
| **FP8 MMA** | `QMMA.16832.F32` | 209 `NO_BINARY` | 0 `CUDA_SUCCESS` | **715 `CUDA_ERROR_ILLEGAL_INSTRUCTION`** |

Ampere's 3rd-generation Tensor Cores lack FP8 matrix multiply logic (`QMMA`). Forcing the device to report `sm_89` would cause `NvPresent64.dll` to launch `QMMA` instructions, crashing the driver.

**The Discovery**: Because our patch preserves the device's true identity as **Tier 2 (Ampere)**, `NvPresent64.dll`'s internal dispatch table **automatically selects the FP16 kernel table exclusively**! Exactly 19 FP16 kernels are loaded, and zero FP8 kernels are ever invoked. The SASS instruction stream runs purely on Ampere's native `HMMA` units.

---

### 4. The 5-Object COM Swapchain Hierarchy & Activation

`NvPresent64.dll` does not directly hook the application's DXGI swapchain pointer. Instead, it constructs a multi-tiered COM object wrapping the swapchain:

```
[Game Engine] -> IDXGISwapChain* (0x20-byte Proxy Object, vtable 0x1801d3228)
                       |
                       +--> Offset +0x18: Internal Wrapper (0x1720 bytes, vtable 0x1801d39c0)
                                 |
                                 +--> vt[19](wrapper, 1): Set Smooth Motion Enabled
                                 +--> vt[20](wrapper, 1): Set Presentation Mode Active
                                 +--> Offset +0x80: Hidden Backbuffer Pool (Array of 2-4 textures)
                                 +--> Offset +0xa28: Optical Flow & CNN Dispatch Engine
```

When the game calls `IDXGISwapChain::Present` (slot 8) or `Present1` (slot 22):
1. The proxy hook inspects `[swap + 0x18]`.
2. Upon first presentation, it invokes `vt[19](wrapper, 1)` and `vt[20](wrapper, 1)` to engage Smooth Motion.
3. The underlying wrapper captures the backbuffer into an internal DXGI/CUDA shared resource pool (`Hidden buffer [0..1]`, format `R8G8B8A8_UNORM`, 512x512 to 4K).

---

### 5. CUDA Graph Ping-Pong Execution & Resolution Guard

Once activated, `NvPresent64.dll` instantiates two execution graphs:
- `gExec_0 = 0x...120`
- `gExec_1 = 0x...C60`

Each subsequent `Present()` call dispatches `cuGraphLaunch` on the CUDA stream alternating between the two execution handles, completely overlapping optical flow estimation with neural warping.

```
[Present #0] -> Base Frame 0 captured
[Present #1] -> cuGraphLaunch #1 (gExec_0) -> Interpolates Frame 0.5 -> Presents Synth + Real Frame 1
[Present #2] -> cuGraphLaunch #2 (gExec_1) -> Interpolates Frame 1.5 -> Presents Synth + Real Frame 2
[Present #3] -> cuGraphLaunch #3 (gExec_0) -> Interpolates Frame 2.5 -> Presents Synth + Real Frame 3
```

> [!NOTE]
> **Resolution Guard**: Reverse engineering of `CreateSwapChainForHwnd` detour uncovered an internal resolution threshold:
> `Width >= 480 && Height >= 480`. Swapchains smaller than 480x480 (such as thumbnail views or auxiliary tools) bypass neural interpolation and pass through unwarped.

---

## Empirical Benchmarks on RTX 3080 Hardware

The following metrics were collected directly on physical hardware using the benchmark suite (`tools/nvp_perf_bench.cpp`) on an **NVIDIA GeForce RTX 3080 12GB** (Driver Store 616.56, Ampere `sm_86`):

```
================================================================
  NvPresent64 Smooth Motion (Road 1) Performance Benchmark
  GPU: NVIDIA GeForce RTX 3080 (sm_86, Ampere)
================================================================
```

### Performance & Memory Overhead Table:

| Target Resolution | Aspect Ratio | Avg GPU Latency | Min Latency | Max Latency | Theoretical Max FPS | VRAM Delta (Allocated) |
|---|---|---|---|---|---|---|
| **1920 x 1080 (1080p)** | 16:9 | **0.622 ms** | 0.552 ms | 1.011 ms | **1607.1 FPS** | **+419.2 MB** |
| **2560 x 1440 (1440p)** | 16:9 | **0.759 ms** | 0.722 ms | 1.055 ms | **1317.1 FPS** | **+465.0 MB** |
| **3840 x 2160 (4K UHD)** | 16:9 | **1.900 ms** | 1.322 ms | 4.181 ms | **526.2 FPS** | **+526.8 MB** |

### Key Takeaways:
- **Negligible Frame-Time Impact**: At 1080p and 1440p, frame interpolation consumes **under 0.8 ms** of GPU time. At 144Hz (6.94 ms frame budget) or 60Hz (16.66 ms frame budget), the interpolation pass occupies less than **5% to 11%** of the frame slice.
- **Constant VRAM Overhead**: Regardless of game complexity, the VRAM consumption is bounded between ~419 MB and ~527 MB, corresponding to the internal optical flow feature pyramid, ping-pong ping buffers, and CUDA graph weights.


---

## Architecture & Injection Flow

```
[Game Application] (e.g. Game.exe)
        |
        | [Windows DLL Search Order loads local version.dll]
        v
+--------------------------------------------------------------------+
| version.dll (sm86_smooth)                                          |
|                                                                    |
|  1. Forward all 17 Version APIs -> C:\Windows\System32\version.dll |
|  2. DllMain -> Creates StartupThread (Clears Loader Lock)          |
|  3. StartupThread:                                                 |
|     a. Loads NvPresent64.dll from DriverStore / System             |
|     b. In-Memory Patch: RVA 0xc41f (cmp 3->2), RVA 0xc437 (mov sil)|
|     c. In-Memory IAT Hook: cuModuleLoadData -> Dynamic arch patch  |
|     d. Config Gate: S[0x4c]=1, S[0xe8]=1, S[0xe9]=1, S[0x12a5]=1   |
|     e. Calls NVP_Init_D3D() -> Installs Detours on DXGI            |
|     f. Installs HookedPresent on DXGI vtable slot 8 & slot 22      |
+--------------------------------------------------------------------+
        |
        v
[DXGI CreateSwapChainForHwnd]
        |
        +--> NvPresent64 wraps SwapChain -> Internal Wrapper (+0x18)
        |
[DXGI Present / Present1]
        |
        +--> HookedPresent invokes vt[19] & vt[20] -> Activates Smooth Motion
        +--> cuGraphLaunch generates intermediate frame
        +--> Backbuffer flipped to display
```

---

## Repository Structure

```
sm86_smooth/
├── CMakeLists.txt              # Unified CMake configuration (MSVC C++17)
├── build.bat                   # One-click build script (Visual Studio 2022)
├── README.md                   # This comprehensive technical guide (English)
├── README_zh.md                # Chinese technical guide
├── AGENTS.md                   # AI agent onboarding and architectural guide
│
├── src/
│   ├── proxy/
│   │   ├── sm86_rehost.cpp     # Production version.dll proxy & NvPresent64 rehost
│   │   ├── early_logger.h      # High-reliability Win32 file logger (SRWLock, WriteFile)
│   │   ├── d3d11_to_d3d12_bridge.cpp/.h # D3D11 to D3D12 bridge layer
│   │   ├── osd_overlay.cpp/.h  # Runtime OSD overlay
│   │   ├── ui_mask.cpp/.h      # UI protection mask engine
│   │   ├── pe_scan.h           # PE/IAT pattern scanner utilities
│   │   └── version.def         # Version.dll export forwarder definition
│   └── shaders/
│       ├── osd.hlsl            # OSD rendering shader
│       ├── ui_mask.hlsl        # UI mask shader
│       └── nvof_up.hlsl        # Optical flow upsampling shader
│
├── tools/
│   ├── nvp_live_test.cpp       # Live end-to-end verification harness & BMP dumper
│   ├── nvp_perf_bench.cpp      # High-precision GPU latency & VRAM benchmark suite
│   ├── proxytest.cpp           # Verification tool for version.dll export forwarding
│   ├── isa_exec_test.py        # SASS HMMA vs QMMA hardware execution validator
│   ├── kernel_twin_compare.py  # Binary ELF comparator for FP16 vs FP8 kernel twins
│   └── patch_nvpresent.py      # Standalone static analyzer & patch explorer
│
├── tests/
│   ├── test_proxy_hardening.cpp   # Safe pointer probing unit tests
│   ├── test_challenger_stress.cpp # Stress / concurrency tests
│   └── test_challenger_r1_probing.cpp # Swapchain identification edge cases
│
└── demo_out/                   # Verification output (BMP dumps of synthesized frames)
```

---

## Building & Compiling

### Prerequisites
- **Operating System**: Windows 10 / Windows 11 (64-bit)
- **Compiler**: Visual Studio 2022 (Community, Professional, or Build Tools) with Desktop C++
- **SDK**: Windows 10 / 11 SDK (DirectX 11 & DirectX 12 headers)
- **Build System**: CMake $\ge$ 3.20
- *Note: You do NOT need the CUDA Toolkit or Optical Flow SDK installed to build the project!*

### One-Click Build
Run the provided `build.bat` in the repository root:

```cmd
build.bat
```

To perform a clean rebuild:
```cmd
build.bat clean
```

### Build Artifacts
Upon completion, the binaries are generated in `build\Release\`:
- `version.dll`: The drop-in injection proxy for games (completely self-contained).
- `nvp_live_test.exe`: The live hardware validation tool for `NvPresent64.dll`.
- `nvp_perf_bench.exe`: The latency and VRAM performance benchmark suite.
- `proxytest.exe`: Quick sanity checker confirming export forwarders resolve.

---

## Deployment & Usage Guide

### Step 1: Copy Binaries
Place the compiled `version.dll` directly into the target game's executable directory (the folder containing the main `.exe`):

```
GameFolder/
├── Game.exe
└── version.dll             <-- Copied from build\Release\version.dll
```

> **Note**: The proxy DLL is fully self-contained. **No external .ini files or .hlsl shaders are required**.

### Step 2: Run the Game
Launch the game normally. Because Windows searches the application directory before `System32`, `version.dll` is loaded automatically. It spawns a background initialization thread, hooks `NvPresent64.dll`, detours the swapchain, and activates Smooth Motion. Milestones and initialization telemetry are automatically logged to `logs\sm86_proxy_<pid>.log`.

---

## Live Verification & Diagnostics

To verify on your own machine that your Ampere GPU executes `NvPresent64.dll`'s neural kernels:

### 1. Run the Live Test Harness
```cmd
build\Release\nvp_live_test.exe
```
**Expected Output**:
```
[+] Loaded NvPresent64.dll @ 00007FFA5E9C0000
[+] Gate patched: Tier 2 allowed, sil=1 forced
[+] NvPresent64.dll IAT hooked (cuModuleLoadData, cuLaunchKernel, cuGraphLaunch)
[+] NVP_Init_D3D() -> TRUE
[+] SwapChain created (Proxy COM Object, Internal Wrapper @ 0000018A688E5160)
[+] Smooth Motion activated on wrapper (vt[19]=1, vt[20]=1)
  >>> cuGraphLaunch #1: gExec=0000018ACF6A0120 stream=...
  <<< cuGraphLaunch result=0
[Frame 0] swap->Present -> 0x00000000 (graphs launched: +1)
================================================================
  SUMMARY STATS:
    - FP16 Fatbinaries Loaded: 19 (19 expected on Tier 2)
    - Warmup cuLaunchKernel:   50
    - Frame cuGraphLaunch:     5 (1 per Present)
================================================================
[+] SUCCESS: Road 1 Live Frame Generation fully verified on sm_86!
```

### 2. Run the Benchmark Suite
```cmd
build\Release\nvp_perf_bench.exe
```
Measures real-time GPU frame times across 1080p, 1440p, and 4K, calculating effective throughput and reporting VRAM allocations.

---

## Anti-Cheat & Safety Warning

> [!CAUTION]
> **STRICTLY FOR OFFLINE & SINGLE-PLAYER USE ONLY!**
> 
> `sm86_smooth` uses DLL proxying (`version.dll`), in-memory code patching, and DXGI vtable detouring. 
> 
> Modern online multiplayer anti-cheat engines (such as Easy Anti-Cheat, BattlEye, Ricochet, Vanguard, VAC) inspect loaded modules, verify memory integrity of driver components, and detect modified DXGI `Present` vtables. 
> 
> **DO NOT use this mod in online or competitive multiplayer games. You risk an immediate, irreversible account ban.**

---

## Legal & Provenance Disclaimer

1. **Clean-Room Implementation**: This project does not distribute or redistribute any proprietary NVIDIA code, CUDA binary blobs, model weights, or pre-compiled fatbinaries.
2. **In-Memory Transformation**: All patches are applied dynamically at runtime in memory within the caller's private address space. No files in `C:\Windows` or `DriverStore` are modified on disk.
3. **Academic & Research Purpose**: This repository is created for interoperability, performance analysis, and academic research into GPU instruction set architectures and driver presentation mechanics under fair use.
4. **Trademarks**: NVIDIA, GeForce, RTX, DLSS, and Ada Lovelace are registered trademarks of NVIDIA Corporation. This project is not affiliated with, endorsed by, or sponsored by NVIDIA Corporation.
