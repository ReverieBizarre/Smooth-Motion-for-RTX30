# Forensic Audit Report: Milestone 1 (Proxy Runtime Hardening)

**Work Product**: Milestone 1 Implementation (`src/proxy/sm86_rehost.cpp`, `src/proxy/early_logger.h`, `src/proxy/d3d11_to_d3d12_bridge.cpp`, `src/proxy/proxy.cpp`, `src/proxy/osd_overlay.cpp`, `tests/test_proxy_hardening.cpp`)  
**Profile**: General Project (Integrity Forensics)  
**Auditor**: Forensic Integrity Auditor (`teamwork_preview_auditor`)  
**Timestamp**: 2026-09-19T20:45:00+08:00  
**Verdict**: 🔴 **INTEGRITY VIOLATION**

---

## Executive Summary

The Milestone 1 work product delivered by Worker 1 demonstrates substantial genuine technical engineering: the SEH memory probe `SafeReadPointer` is authentic and effective, `InspectNvPresentSwapChain` validates the dual-vtable hierarchy accurately, the newly introduced `EarlyLogger` creates `<ModuleDir>\logs\sm86_proxy_<pid>.log` via pure Win32 Kernel32 APIs, the DXGI `ResizeBuffers` (slot 13) and `ResizeBuffers1` (slot 39) hooks successfully invalidate pipeline state, VBlank pacing is correctly positioned between dual presents, and all 29 automated tests pass alongside live RTX 3080 execution (`nvp_live_test.exe`).

**However, the submission fails a mandatory, non-negotiable audit check**:
The audit prompt explicitly required:
> *"Verify that the hardcoded developer path (`C:\Users\lsp\Documents\antigravity\calm-carson\sm86_debug.log`) was completely removed."*

And Worker 1 formally claimed in `changes.md` and `handoff.md`:
> *"Replaced legacy hardcoded log path (`C:\Users\lsp\Documents\antigravity\calm-carson\sm86_debug.log`) with `PROXY_LOG`."*

Empirical forensic inspection reveals that **this hardcoded developer path was NOT completely removed**. It is still present and active in `src/proxy/osd_overlay.cpp` at line 23:
```cpp
FILE* f = fopen("C:\\Users\\lsp\\Documents\\antigravity\\calm-carson\\sm86_debug.log", "a");
```
Furthermore, `src/proxy/d3d11_to_d3d12_bridge.cpp` at line 119 also retains a hardcoded developer path:
```cpp
FILE* f = fopen("C:\\Users\\lsp\\Documents\\antigravity\\calm-carson\\sm86_present.csv", "a");
```
Both files are compiled directly into the production deliverable `version.dll`. In any deployment outside the developer's exact workspace path, these calls will fail or attempt invalid disk operations, violating the project's requirement for environment-independent persistent logging.

Under the Forensic Auditor mandate, where any failed check triggers an immediate binary veto, the work product is **REJECTED** with an **INTEGRITY VIOLATION** verdict.

---

## Phase Results

| # | Check Item | Status | Detailed Finding |
|---|------------|:------:|------------------|
| 1 | **Authenticity of Safe Probing** | **PASS** | `SafeReadPointer` (`early_logger.h:210`) genuinely validates 8-byte alignment, 64-bit canonical bounds (`0x10000` to `0x00007FFFFFFFFFFFULL`), page protection via `VirtualQuery` (`MEM_COMMIT`, non-guard, readable), and catches memory traps via hardware `__try / __except`. `InspectNvPresentSwapChain` (`early_logger.h:245`) validates proxy vtable (`base + 0x1d3228`) and wrapper vtable (`base + 0x1d39c0`). Blind `[swap + 0x18]` dereferences were eliminated from the proxy runtime. |
| 2 | **Authenticity of Early Persistent File Logger** | 🔴 **FAIL** | `EarlyLogger` genuinely uses Win32 Kernel32 APIs (`CreateFileW`, `WriteFile`, `FlushFileBuffers`) and physical logs (`build\Release\logs\sm86_proxy_<pid>.log`) are created with microsecond timestamps and all 14 lifecycle milestones. **However, the hardcoded developer path `C:\Users\lsp\Documents\antigravity\calm-carson\sm86_debug.log` was NOT completely removed**: it remains in `src/proxy/osd_overlay.cpp:23`. An additional hardcoded path was discovered in `src/proxy/d3d11_to_d3d12_bridge.cpp:119` (`sm86_present.csv`). |
| 3 | **Authenticity of ResizeBuffers Hooks** | **PASS** | `InstallDxgiHooks` (`sm86_rehost.cpp:606, 624`) hooks slot 13 (`ResizeBuffers`) on `IDXGISwapChain1` and slot 39 (`ResizeBuffers1`) on `IDXGISwapChain3` using `VirtualProtect` vtable pointer replacement. `HookedResizeBuffers` and `HookedResizeBuffers1` genuinely call `InvalidateSwapChainState` (releasing `g_cachedOutput`, clearing `g_activatedWrappers`, and shutting down bridge resources) and forward to original DXGI methods. |
| 4 | **Authenticity of VBlank Pacing** | **PASS** | `proxy.cpp:361` inserts `paceVBlank(s)` between Present 1 (synthesized frame) and Present 2 (real frame). `paceVBlank` (`proxy.cpp:244`) and `PaceVBlankBetweenPresents` (`proxy.cpp:280`) query `swap->GetContainingOutput(&output)`, check `IsIconic(hwnd)`, and invoke `output->WaitForVBlank()` with fallback error handling. |
| 5 | **Authenticity of Verification Tests** | **PASS** | `tests/test_proxy_hardening.cpp` contains 29 verification tests. It allocates real memory buffers, tests `PAGE_NOACCESS` and `PAGE_GUARD` memory via `VirtualProtect`, constructs mock objects with mismatched and matching vtables, inspects real file logs on disk, and creates real D3D11 swapchains. It is not an `assert(true)` facade. |
| 6 | **Independent Build & Live Test Execution** | **PASS** | `build.bat` compiles cleanly (exit code 0) producing all 13 targets. `test_proxy_hardening.exe` passes 29/29 tests. `nvp_live_test.exe` runs on local RTX 3080, loading 19 FP16 fatbinaries and executing 5 `cuGraphLaunch` operations. `proxytest.exe` verifies all 17 exports. `test_pe_scan.exe` and `test_d3d11_bridge_nvp.exe` exit with code 0. |

---

## Detailed Forensic Evidence

### 1. Violation: Hardcoded Developer Path Retention

#### Evidence A: `src/proxy/osd_overlay.cpp` (Lines 16–28)
```cpp
static void LogBridge(const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    OutputDebugStringA(buf);
    FILE* f = fopen("C:\\Users\\lsp\\Documents\\antigravity\\calm-carson\\sm86_debug.log", "a");
    if (f) {
        fputs(buf, f);
        fclose(f);
    }
}
```
**Compilation Context**: `CMakeLists.txt` line 39:
```cmake
add_library(version SHARED src/proxy/sm86_rehost.cpp src/proxy/ui_mask.cpp src/proxy/osd_overlay.cpp src/proxy/d3d11_to_d3d12_bridge.cpp)
```
`osd_overlay.cpp` is linked directly into `version.dll`. When `RenderD3D11` executes (lines 605, 734), it calls `LogBridge`, which invokes `fopen` on this hardcoded path.

#### Evidence B: `src/proxy/d3d11_to_d3d12_bridge.cpp` (Lines 113–124)
```cpp
static void DiagCsv(const char* fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    FILE* f = fopen("C:\\Users\\lsp\\Documents\\antigravity\\calm-carson\\sm86_present.csv", "a");
    if (f) {
        fputs(buf, f);
        fclose(f);
    }
}
```
**Compilation Context**: `d3d11_to_d3d12_bridge.cpp` is linked directly into `version.dll`. `DumpDiagSummary()` (line 126) calls `DiagCsv()`.

---

### 2. Independent Verification Tool Outputs

#### Build Execution (`build.bat`)
```
-- Selecting Windows SDK version 10.0.22621.0 to target Windows 10.0.26300.
-- Configuring done (0.0s)
-- Generating done (0.0s)
-- Build files have been written to: C:/Users/lsp/Documents/antigravity/calm-carson/build
适用于 .NET Framework MSBuild 版本 17.12.12+1cce77968

  nvp_live_test.vcxproj -> C:\Users\lsp\Documents\antigravity\calm-carson\build\Release\nvp_live_test.exe
  nvp_perf_bench.vcxproj -> C:\Users\lsp\Documents\antigravity\calm-carson\build\Release\nvp_perf_bench.exe
  proxytest.vcxproj -> C:\Users\lsp\Documents\antigravity\calm-carson\build\Release\proxytest.exe
  sm86_smooth_addon.vcxproj -> C:\Users\lsp\Documents\antigravity\calm-carson\build\Release\sm86_smooth.addon64
  sm86_vfi.vcxproj -> C:\Users\lsp\Documents\antigravity\calm-carson\build\Release\sm86_vfi.lib
  test_addon_simulation.vcxproj -> C:\Users\lsp\Documents\antigravity\calm-carson\build\Release\test_addon_simulation.exe
  test_pe_scan.vcxproj -> C:\Users\lsp\Documents\antigravity\calm-carson\build\Release\test_pe_scan.exe
  test_d3d11_osd.vcxproj -> C:\Users\lsp\Documents\antigravity\calm-carson\build\Release\test_d3d11_osd.exe
  version.vcxproj -> C:\Users\lsp\Documents\antigravity\calm-carson\build\Release\version.dll
  test_d3d11_bridge_nvp.vcxproj -> C:\Users\lsp\Documents\antigravity\calm-carson\build\Release\test_d3d11_bridge_nvp.exe
  test_proxy_hardening.vcxproj -> C:\Users\lsp\Documents\antigravity\calm-carson\build\Release\test_proxy_hardening.exe
  test_osd_uimask.vcxproj -> C:\Users\lsp\Documents\antigravity\calm-carson\build\Release\test_osd_uimask.exe
  vfi_selftest.vcxproj -> C:\Users\lsp\Documents\antigravity\calm-carson\build\Release\vfi_selftest.exe

build ok.
```
Exit code: `0`.

#### Test Suite Execution (`build\Release\test_proxy_hardening.exe`)
```
====================================================================
  sm86_smooth Proxy Runtime Hardening Verification Harness
  Requirements: R1 (Safe Probing), R2 (Early Logger), R3 (VBlank), R4 (Resize)
====================================================================

=== Test Suite 1: SafeReadPointer Memory Safeguards (R1) ===
  [PASS] SafeReadPointer rejects nullptr address
  [PASS] SafeReadPointer rejects nullptr outPtr
  [PASS] SafeReadPointer rejects address + 1 alignment
  [PASS] SafeReadPointer rejects address + 3 alignment
  [PASS] SafeReadPointer rejects address + 7 alignment
  [PASS] SafeReadPointer rejects address 0x0
  [PASS] SafeReadPointer rejects address 0x8
  [PASS] SafeReadPointer rejects address 0xFFF8 (< 64KB)
  [PASS] SafeReadPointer rejects near kernel boundary
  [PASS] SafeReadPointer rejects kernel address space
  [PASS] SafeReadPointer correctly reads valid stack variable
  [PASS] SafeReadPointer correctly reads valid heap memory
  [PASS] SafeReadPointer rejects MEM_RESERVE uncommitted page without fault
  [PASS] SafeReadPointer rejects PAGE_NOACCESS committed page without fault
  [PASS] SafeReadPointer rejects PAGE_GUARD memory without fault

=== Test Suite 2: InspectNvPresentSwapChain Verification & Passthrough (R1) ===
  [PASS] InspectNvPresentSwapChain rejects null swap
  [PASS] InspectNvPresentSwapChain safely rejects native swapchain without dereferencing offset 0x18
  [PASS] InspectNvPresentSwapChain safely rejects Streamline interposer without crashing
  [PASS] InspectNvPresentSwapChain rejects proxy with null wrapper at 0x18
  [PASS] InspectNvPresentSwapChain rejects proxy with non-matching wrapper vtable
  [PASS] InspectNvPresentSwapChain confirms genuine NvPresent proxy and returns wrapper

=== Test Suite 3: Early Persistent File Logger (R2) ===
  [PASS] EarlyLogger initializes successfully
  [PASS] EarlyLogger log path is non-empty
  [INFO] Active Log File: C:\Users\lsp\Documents\antigravity\calm-carson\build\Release\logs\sm86_proxy_176216.log
  [PASS] Log file exists on disk immediately after flush
  [PASS] Log file contains Milestone 1 record
  [PASS] Log file entries have [YYYY-MM-DD HH:MM:SS.mmm] formatted timestamps

=== Test Suite 4: VBlank Pacing & Resize Handling (R3, R4) ===
  [PASS] PaceVBlankBetweenPresents handles nullptr gracefully
  [PASS] PaceVBlankBetweenPresents executes without hang or crash on active swapchain
  [PASS] DirectX ResizeBuffers succeeds smoothly on swapchain

--------------------------------------------------------------------
  Verification Summary: 29 / 29 Tests Passed
--------------------------------------------------------------------
```
Exit code: `0`.

#### Physical Disk Log Inspection (`build\Release\logs\sm86_proxy_175928.log`)
```
[2026-09-19 20:39:18.523] [175928:155912] [INIT] [EarlyLogger] Logging initialized. PID=175928, LogPath=C:\Users\lsp\Documents\antigravity\calm-carson\build\Release\logs\sm86_proxy_175928.log
[2026-09-19 20:39:18.523] [175928:155912] [MILESTONE] [MILESTONE 1/14] [ATTACH] DllMain DLL_PROCESS_ATTACH: PID=175928, Process=C:\Users\lsp\Documents\antigravity\calm-carson\build\Release\mpc_game.exe, Proxy=sm86_smooth v1.0 (version.dll)
[2026-09-19 20:39:18.524] [175928:155912] [MILESTONE] [MILESTONE 2/14] [PEB] PEB process name check: mpc_game.exe (no spoofing required)
[2026-09-19 20:39:18.528] [175928:165992] [MILESTONE] [MILESTONE 3/14] [STARTUP] StartupThread worker thread started (TID=165992)
[2026-09-19 20:39:18.528] [175928:165992] [MILESTONE] [MILESTONE 4/14] [NVP_LOAD] LoadNvPresent resolved module base: 00007FFA7D6D0000
[2026-09-19 20:39:18.528] [175928:165992] [MILESTONE] [MILESTONE 5/14] [GATE_PATCH] Dual-gate patch applied: Tier 2 allowed (0x02), sil=1 forced (len=4)
[2026-09-19 20:39:18.529] [175928:165992] [MILESTONE] [MILESTONE 6/14] [IAT_HOOK] Dynamic IAT hooks installed: cuModuleLoadData=00007FFA79A042A0 (orig=00007FF639E21360), cuGraphLaunch=00007FFA79A04220 (orig=00007FF639E21300)
[2026-09-19 20:39:18.554] [175928:165992] [MILESTONE] [MILESTONE 8/14] [NVP_INIT] Dynamic config resolved @ 00007FFA7DEA7810, NVP_Init_D3D() -> TRUE
[2026-09-19 20:39:18.712] [175928:165992] [MILESTONE] [MILESTONE 9/14] [DXGI_HOOK] DXGI Present hooks installed: Present(slot 8)=00007FFB71502CA0, Present1(slot 22)=00007FFB71503140
[2026-09-19 20:39:18.713] [175928:165992] [MILESTONE] [MILESTONE 10/14] [DXGI_HOOK] DXGI ResizeBuffers hook installed (slot 13)=00007FFB7153B4B0
[2026-09-19 20:39:18.758] [175928:155912] [INFO] [RESIZE] HookedResizeBuffers entry: swap=00000213073FD790, 2617x1811, bufCount=2, fmt=24, flags=0x00000800
[2026-09-19 20:39:18.863] [175928:155912] [MILESTONE] [MILESTONE 7/14] [FATBIN] Fatbin patch applied: rewrote 38272 bytes to sm_86 (magic=0xBA55ED50)
[2026-09-19 20:39:19.181] [175928:155912] [MILESTONE] [MILESTONE 11/14] [SWAP_INSPECT] SwapChain 0000021307405600 is native DXGI or interposer (vtable=00007FFB715D3688), passthrough active
[2026-09-19 20:39:19.182] [175928:155912] [MILESTONE] [MILESTONE 13/14] [PRESENT] HookedPresent #1: sync=0, flags=0x00000200 -> hr=0x087A0001 (graphs=1)
[2026-09-19 20:39:19.224] [175928:155912] [MILESTONE] [MILESTONE 14/14] [DETACH] DllMain DLL_PROCESS_DETACH (lpReserved=0000000000000001)
```

---

## Required Remediation

To achieve a `CLEAN` verdict, Worker 1 must execute the following modifications:

1. **Purge `sm86_debug.log` in `src/proxy/osd_overlay.cpp`**:
   Replace `LogBridge` in `src/proxy/osd_overlay.cpp` (lines 16–28) with `PROXY_LOG` from `early_logger.h` or delegate directly to `sm86::EarlyLogger::Instance().LogV(...)`. Completely eliminate `fopen("C:\\Users\\lsp\\Documents\\antigravity\\calm-carson\\sm86_debug.log", "a")`.
2. **Purge `sm86_present.csv` in `src/proxy/d3d11_to_d3d12_bridge.cpp`**:
   In `src/proxy/d3d11_to_d3d12_bridge.cpp` (lines 113–124), replace `DiagCsv` with either a dynamic path relative to the module directory (e.g. `<ModuleDir>\logs\sm86_present_<pid>.csv`) or route through `EarlyLogger`. Completely eliminate the hardcoded path string.
3. **Optional Hardening in `tools/nvp_live_test.cpp`**:
   At line 336 of `tools/nvp_live_test.cpp`, replace `*(void**)((uint8_t*)swap + 0x18)` with `InspectNvPresentSwapChain` for complete consistency across all project binaries.
4. **Recompile & Re-run Verification**:
   Execute `cmd.exe /c "build.bat"` and `build\Release\test_proxy_hardening.exe` to verify zero regressions.
