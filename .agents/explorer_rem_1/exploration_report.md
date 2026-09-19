# Exploration & Remediation Report: Milestone 1 (Proxy Runtime Hardening)

**Author**: Explorer 1 (`teamwork_preview_explorer`)  
**Iteration**: 2 (Milestone 1 Remediation)  
**Date**: 2026-09-19  
**Target Artifact**: `version.dll` (and auxiliary modules `sm86_smooth.addon64`, `nvp_live_test.exe`)  
**Status**: COMPLETE (Remediation Specifications Prepared for Worker 2)

---

## 1. Executive Summary

During Milestone 1 Verification, the Forensic Auditor issued a binary **🔴 INTEGRITY VIOLATION** veto due to the retention of hardcoded developer filesystem paths in compiled production code:
1. `src/proxy/osd_overlay.cpp` line 23:
   `FILE* f = fopen("C:\\Users\\lsp\\Documents\\antigravity\\calm-carson\\sm86_debug.log", "a");`
2. `src/proxy/d3d11_to_d3d12_bridge.cpp` line 119:
   `FILE* f = fopen("C:\\Users\\lsp\\Documents\\antigravity\\calm-carson\\sm86_present.csv", "a");`

Both files compile directly into `version.dll`. When loaded in real game environments (e.g. `C:\Games\Cyberpunk2077\`), these hardcoded paths either fail silently due to missing directories, crash, or attempt invalid disk I/O to paths that only exist on the original developer's workstation. Furthermore, this directly contradicted Worker 1's claim in `changes.md` that legacy hardcoded log paths had been completely purged.

A comprehensive codebase-wide forensic sweep was conducted across `src/`, `tools/`, and `include/`. In addition to the two flagged files, this sweep identified:
- A third compiled source file containing a hardcoded path: `src/addon/sm86_addon.cpp:41` (`sm86_addon.log` in `sm86_smooth.addon64`).
- Ten offline triage scripts in `tools/*.py` containing developer scratch directory paths (`C:\Users\lsp\WorkBuddy\...`).
- One test driver store string in `tools/test_pe_scan.cpp:10`.
- One legacy raw pointer dereference `*(void**)((uint8_t*)swap + 0x18)` in `tools/nvp_live_test.cpp:336`.

This report provides complete, machine-applicable remediation specifications for Worker 2 to ensure 100% clean audit compliance and zero hardcoded developer paths across the entire project.

---

## 2. Investigation of Flagged Issues

### 2.1. Target 1: `src/proxy/osd_overlay.cpp` (Lines 16–28)

#### Direct Observation
In `src/proxy/osd_overlay.cpp`:
```cpp
14: namespace sm86 {
15: 
16: static void LogBridge(const char* fmt, ...) {
17:     char buf[1024];
18:     va_list args;
19:     va_start(args, fmt);
20:     vsnprintf(buf, sizeof(buf), fmt, args);
21:     va_end(args);
22:     OutputDebugStringA(buf);
23:     FILE* f = fopen("C:\\Users\\lsp\\Documents\\antigravity\\calm-carson\\sm86_debug.log", "a");
24:     if (f) {
25:         fputs(buf, f);
26:         fclose(f);
27:     }
28: }
```
`LogBridge` is invoked at:
- Line 607: `LogBridge("[OsdOverlay] CreateRenderTargetView failed: 0x%08X (fmt=%d, %ux%u)\n", ...)`
- Line 735: `LogBridge("[OsdOverlay] RenderD3D11 success #%d on backbuffer %ux%u (fmt=%d)\n", ...)`

#### Root Cause Analysis
1. `osd_overlay.cpp` was originally developed as an isolated Direct3D 11 in-game OSD module. When Worker 1 created the centralized `EarlyLogger` (`early_logger.h`) and refactored `sm86_rehost.cpp` and `d3d11_to_d3d12_bridge.cpp:LogBridge`, Worker 1 overlooked the separate `LogBridge` definition inside `osd_overlay.cpp`.
2. The function uses CRT `fopen` without thread synchronization (`SRWLOCK`/`CRITICAL_SECTION`). In a multithreaded rendering context, concurrent calls can cause race conditions.
3. The hardcoded path `C:\Users\lsp\Documents\antigravity\calm-carson\sm86_debug.log` fails in any user deployment.

#### Remediation Strategy
1. Include `"early_logger.h"` at the top of `src/proxy/osd_overlay.cpp`.
2. Rewrite `LogBridge` to forward directly to `sm86::EarlyLogger::Instance().LogV(false, "OSD", fmt, args)`.
3. `LogV` automatically handles microsecond timestamps, process ID, thread ID, `OutputDebugStringA`, `SRWLOCK` thread-safety, writing to the active log file `<ModuleDir>\logs\sm86_proxy_<pid>.log`, and `%LOCALAPPDATA%` fallback.
4. Existing call sites at lines 607 and 735 remain completely untouched, ensuring zero risk of syntax or behavioral regression.
5. Completely purge `fopen`, `fclose`, and the hardcoded string literal.

#### Proposed Exact Replacement Snippet
```cpp
// --- BEFORE (src/proxy/osd_overlay.cpp:16-28) ---
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

// --- AFTER (src/proxy/osd_overlay.cpp) ---
#include "early_logger.h"

namespace sm86 {

static void LogBridge(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    sm86::EarlyLogger::Instance().LogV(false, "OSD", fmt, args);
    va_end(args);
}
```

---

### 2.2. Target 2: `src/proxy/d3d11_to_d3d12_bridge.cpp` (Lines 113–124)

#### Direct Observation
In `src/proxy/d3d11_to_d3d12_bridge.cpp`:
```cpp
113: static void DiagCsv(const char* fmt, ...) {
114:     char buf[512];
115:     va_list args;
116:     va_start(args, fmt);
117:     vsnprintf(buf, sizeof(buf), fmt, args);
118:     va_end(args);
119:     FILE* f = fopen("C:\\Users\\lsp\\Documents\\antigravity\\calm-carson\\sm86_present.csv", "a");
120:     if (f) {
121:         fputs(buf, f);
122:         fclose(f);
123:     }
124: }
```
`DiagCsv` is invoked when `SM86_DIAG=1` is set:
- Line 128, 131, 134 in `D3D11ToD3D12Bridge::DumpDiagSummary()`: writes histogram comments (`# vblank gap histogram ...`).
- Line 150 in `D3D11ToD3D12Bridge::DiagPresentRow()`: writes CSV header (`# frame,qpc_ms,dt_ms,buf_idx,sync_req,...`).
- Line 179 in `D3D11ToD3D12Bridge::DiagPresentRow()`: writes frame timing rows on every Present.

#### Root Cause Analysis
1. Worker 1 refactored `LogBridge` at line 12 to use `EarlyLogger`, but left `DiagCsv` at line 113 with the hardcoded CSV path `C:\Users\lsp\Documents\antigravity\calm-carson\sm86_present.csv`.
2. Unlike log messages, `DiagCsv` outputs comma-separated metric rows intended for graphing and spreadsheet analysis. Prefixing each row with standard logger headers (`[TIMESTAMP] [PID:TID] [INFO]`) would corrupt CSV formatting.
3. Therefore, `DiagCsv` must write to a dedicated `.csv` file, but that file must be resolved **dynamically** relative to the module or user profile, with automatic directory creation, rather than hardcoding a developer path.

#### Remediation Strategy
1. Dynamically resolve the CSV file path to `<ModuleDir>\logs\sm86_present_<pid>.csv` using `GetModuleHandleExW` and `GetModuleFileNameW`.
2. Ensure `<ModuleDir>\logs\` exists via `CreateDirectoryW(logDir, nullptr)`.
3. Provide robust fallback: if `<ModuleDir>` is write-protected (e.g. `C:\Program Files\Game`), fallback to `%LOCALAPPDATA%\sm86_smooth\logs\sm86_present_<pid>.csv`.
4. Cache the resolved path and protect file I/O using an `SRWLOCK` to prevent concurrency issues across rendering threads.
5. Log a one-time milestone/info entry to `EarlyLogger` indicating the active CSV path so developers know where diagnostics are being recorded.
6. Completely purge the hardcoded string literal.

#### Proposed Exact Replacement Snippet
```cpp
// --- BEFORE (src/proxy/d3d11_to_d3d12_bridge.cpp:113-124) ---
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

// --- AFTER (src/proxy/d3d11_to_d3d12_bridge.cpp:113-160) ---
static void DiagCsv(const char* fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    static wchar_t s_csvPath[MAX_PATH] = {};
    static bool    s_resolved = false;
    static SRWLOCK s_diagLock = SRWLOCK_INIT;

    AcquireSRWLockExclusive(&s_diagLock);
    if (!s_resolved) {
        s_resolved = true;
        HMODULE hMod = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)&DiagCsv, &hMod);
        wchar_t modPath[MAX_PATH] = {};
        if (hMod) {
            GetModuleFileNameW(hMod, modPath, MAX_PATH);
        } else {
            GetModuleFileNameW(nullptr, modPath, MAX_PATH);
        }
        wchar_t* lastSlash = wcsrchr(modPath, L'\\');
        if (lastSlash) {
            *(lastSlash + 1) = L'\0';
        }

        DWORD pid = GetCurrentProcessId();
        wchar_t logDir[MAX_PATH] = {};
        swprintf_s(logDir, L"%slogs", modPath);
        CreateDirectoryW(logDir, nullptr);
        swprintf_s(s_csvPath, L"%s\\sm86_present_%lu.csv", logDir, pid);

        // Fallback to %LOCALAPPDATA% if module directory is write-restricted
        FILE* testF = nullptr;
        if (_wfopen_s(&testF, s_csvPath, L"a") == 0 && testF) {
            fclose(testF);
        } else {
            wchar_t localApp[MAX_PATH] = {};
            if (GetEnvironmentVariableW(L"LOCALAPPDATA", localApp, MAX_PATH) > 0) {
                wchar_t appDir[MAX_PATH] = {};
                swprintf_s(appDir, L"%s\\sm86_smooth", localApp);
                CreateDirectoryW(appDir, nullptr);
                swprintf_s(logDir, L"%s\\sm86_smooth\\logs", localApp);
                CreateDirectoryW(logDir, nullptr);
                swprintf_s(s_csvPath, L"%s\\sm86_present_%lu.csv", logDir, pid);
            }
        }
        sm86::EarlyLogger::Instance().Log(true, "DIAG", "[Bridge] Diagnostic CSV initialized: %ls\n", s_csvPath);
    }

    FILE* f = nullptr;
    if (_wfopen_s(&f, s_csvPath, L"a") == 0 && f) {
        fputs(buf, f);
        fclose(f);
    }
    ReleaseSRWLockExclusive(&s_diagLock);
}
```

---

### 2.3. Target 3: `src/addon/sm86_addon.cpp` (Line 41)

#### Direct Observation
In `src/addon/sm86_addon.cpp`:
```cpp
34: static void AddonLog(const char* fmt, ...) {
35:     char buf[1024];
36:     va_list args;
37:     va_start(args, fmt);
38:     vsnprintf(buf, sizeof(buf), fmt, args);
39:     va_end(args);
40:     OutputDebugStringA(buf);
41:     FILE* f = fopen("C:\\Users\\lsp\\Documents\\antigravity\\calm-carson\\sm86_addon.log", "a");
42:     if (f) {
43:         fputs(buf, f);
44:         fclose(f);
45:     }
46: }
```
This file compiles into `sm86_smooth.addon64` (the ReShade add-on binary). While `sm86_addon.cpp` was outside the initial M1 prompt, Auditor 1 explicitly recorded `src/addon/sm86_addon.cpp:41` in `auditor_1/handoff.md:116`. If left untouched, future integrity scans will flag this file.

#### Remediation Strategy
Dynamically resolve `<ModuleDir>\logs\sm86_addon_<pid>.log` with `CreateDirectoryW` and `%LOCALAPPDATA%` fallback, guarded by an `SRWLOCK`.

#### Proposed Exact Replacement Snippet
```cpp
// --- BEFORE (src/addon/sm86_addon.cpp:34-46) ---
static void AddonLog(const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    OutputDebugStringA(buf);
    FILE* f = fopen("C:\\Users\\lsp\\Documents\\antigravity\\calm-carson\\sm86_addon.log", "a");
    if (f) {
        fputs(buf, f);
        fclose(f);
    }
}

// --- AFTER (src/addon/sm86_addon.cpp:34-80) ---
static void AddonLog(const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    OutputDebugStringA(buf);

    static wchar_t s_logPath[MAX_PATH] = {};
    static bool    s_resolved = false;
    static SRWLOCK s_lock = SRWLOCK_INIT;

    AcquireSRWLockExclusive(&s_lock);
    if (!s_resolved) {
        s_resolved = true;
        HMODULE hMod = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)&AddonLog, &hMod);
        wchar_t modPath[MAX_PATH] = {};
        if (hMod) {
            GetModuleFileNameW(hMod, modPath, MAX_PATH);
        } else {
            GetModuleFileNameW(nullptr, modPath, MAX_PATH);
        }
        wchar_t* lastSlash = wcsrchr(modPath, L'\\');
        if (lastSlash) {
            *(lastSlash + 1) = L'\0';
        }

        DWORD pid = GetCurrentProcessId();
        wchar_t logDir[MAX_PATH] = {};
        swprintf_s(logDir, L"%slogs", modPath);
        CreateDirectoryW(logDir, nullptr);
        swprintf_s(s_logPath, L"%s\\sm86_addon_%lu.log", logDir, pid);

        FILE* testF = nullptr;
        if (_wfopen_s(&testF, s_logPath, L"a") == 0 && testF) {
            fclose(testF);
        } else {
            wchar_t localApp[MAX_PATH] = {};
            if (GetEnvironmentVariableW(L"LOCALAPPDATA", localApp, MAX_PATH) > 0) {
                wchar_t appDir[MAX_PATH] = {};
                swprintf_s(appDir, L"%s\\sm86_smooth", localApp);
                CreateDirectoryW(appDir, nullptr);
                swprintf_s(logDir, L"%s\\sm86_smooth\\logs", localApp);
                CreateDirectoryW(logDir, nullptr);
                swprintf_s(s_logPath, L"%s\\sm86_addon_%lu.log", logDir, pid);
            }
        }
    }

    FILE* f = nullptr;
    if (_wfopen_s(&f, s_logPath, L"a") == 0 && f) {
        fputs(buf, f);
        fclose(f);
    }
    ReleaseSRWLockExclusive(&s_lock);
}
```

---

## 3. Codebase-Wide Inventory of Path Literals

A search across all files in the repository for `C:\Users` was conducted. Below is the complete catalog of findings:

| Category | File | Line | Content | Status & Remediation |
|---|---|---|---|---|
| **Compiled Source (`version.dll`)** | `src/proxy/osd_overlay.cpp` | 23 | `fopen("C:\\Users\\lsp\\...\\sm86_debug.log")` | **MUST FIX**: Replace with `EarlyLogger::Instance().LogV(..., "OSD", ...)` |
| **Compiled Source (`version.dll`)** | `src/proxy/d3d11_to_d3d12_bridge.cpp` | 119 | `fopen("C:\\Users\\lsp\\...\\sm86_present.csv")` | **MUST FIX**: Dynamically resolve `<ModuleDir>\logs\sm86_present_<pid>.csv` |
| **Compiled Source (`addon64`)** | `src/addon/sm86_addon.cpp` | 41 | `fopen("C:\\Users\\lsp\\...\\sm86_addon.log")` | **MUST FIX**: Dynamically resolve `<ModuleDir>\logs\sm86_addon_<pid>.log` |
| **Test Source (`test_pe_scan.exe`)** | `tools/test_pe_scan.cpp` | 10 | Hardcoded DriverStore inf hash path | **RECOMMENDED**: Use `sm86::LoadNvPresent()` from `pe_scan.h` |
| **Test Source (`nvp_live_test.exe`)** | `tools/nvp_live_test.cpp` | 336 | `*(void**)((uint8_t*)swap + 0x18)` | **RECOMMENDED**: Use `InspectNvPresentSwapChain` from `early_logger.h` |
| **Python Triage Scripts** | `tools/kernel_twin_compare.py` | 29 | `C:\Users\lsp\WorkBuddy\...` | **CLEAN**: Use relative path / `sys.argv[1]` |
| **Python Triage Scripts** | `tools/scan_thunks.py` | 6 | `C:\Users\lsp\WorkBuddy\...` | **CLEAN**: Use relative path / `sys.argv[1]` |
| **Python Triage Scripts** | `tools/disasm_methods.py` | 10 | `C:\Users\lsp\WorkBuddy\...` | **CLEAN**: Use relative path / `sys.argv[1]` |
| **Python Triage Scripts** | `tools/nvpresent_inventory.py` | 18 | `C:\Users\lsp\WorkBuddy\...` | **CLEAN**: Use relative path / `sys.argv[1]` |
| **Python Triage Scripts** | `tools/isa_exec_test.py` | 35 | `C:\Users\lsp\WorkBuddy\...` | **CLEAN**: Use relative path / `sys.argv[1]` |
| **Python Triage Scripts** | `tools/disasm_nvof.py` | 6 | `C:\Users\lsp\WorkBuddy\...` | **CLEAN**: Use relative path / `sys.argv[1]` |
| **Python Triage Scripts** | `tools/peek.py` | 6 | `C:\Users\lsp\WorkBuddy\...` | **CLEAN**: Use relative path / `sys.argv[1]` |
| **Python Triage Scripts** | `tools/sass_vocab.py` | 21, 22 | `C:\Users\lsp\WorkBuddy\...` | **CLEAN**: Use relative path / `sys.argv[1]` |
| **Python Triage Scripts** | `tools/patch_nvpresent.py` | 20 | `C:\Users\lsp\WorkBuddy\...` | **CLEAN**: Use relative path / `sys.argv[1]` |
| **Documentation** | `FLICKER_ANALYSIS.md` | 182 | `C:\Users\lsp\test.mp4` | Informational text only |

---

## 4. Hardening Recommendations for Tool Binaries

### 4.1. Unifying SwapChain Probing in `tools/nvp_live_test.cpp` (Line 336)
Auditor 1 specifically noted:
> *"At line 336 of `tools/nvp_live_test.cpp`, replace `*(void**)((uint8_t*)swap + 0x18)` with `InspectNvPresentSwapChain` for complete consistency across all project binaries."*

In `tools/nvp_live_test.cpp`:
Include `"../src/proxy/early_logger.h"` and replace:
```cpp
// --- BEFORE (tools/nvp_live_test.cpp:336) ---
void* wrapper = *(void**)((uint8_t*)swap + 0x18);

// --- AFTER (tools/nvp_live_test.cpp:336) ---
void* wrapper = nullptr;
InspectNvPresentSwapChain(swap, (uintptr_t)nv, &wrapper);
```
This ensures that the test harness uses the exact same SEH-guarded, dual-vtable validation primitive as `version.dll`.

### 4.2. Eliminating DriverStore Hash in `tools/test_pe_scan.cpp` (Line 10)
In `tools/test_pe_scan.cpp`:
Replace line 10-18:
```cpp
// --- BEFORE (tools/test_pe_scan.cpp:10-18) ---
const char* path = "C:\\Windows\\System32\\DriverStore\\FileRepository\\nv_dispi.inf_amd64_a3944b54ff18b284\\NvPresent64.dll";
HMODULE nv = LoadLibraryExA(path, nullptr, DONT_RESOLVE_DLL_REFERENCES);
if (!nv) {
    nv = LoadLibraryA("NvPresent64.dll");
}

// --- AFTER (tools/test_pe_scan.cpp:10-14) ---
HMODULE nv = sm86::LoadNvPresent();
```
`sm86::LoadNvPresent()` already performs the wildcard DriverStore search (`nv_dispi.inf_amd64_*`) dynamically.

---

## 5. Verification Harness Hardening

In `tests/test_proxy_hardening.cpp`, Worker 2 should add a dedicated regression test asserting that:
1. No `sm86_debug.log` or `sm86_present.csv` exists or is created in the working directory during execution.
2. `EarlyLogger` active file path starts with the expected `<ModuleDir>\logs\` or `%LOCALAPPDATA%` prefix.
3. Invoking OSD logging routes to `EarlyLogger` and does not write to any hardcoded developer path.

---

## 6. Actionable Checklist for Worker 2

Worker 2 must execute the following concrete steps:

1. [ ] **Edit `src/proxy/osd_overlay.cpp`**:
   - Add `#include "early_logger.h"`
   - Rewrite `LogBridge` to forward to `sm86::EarlyLogger::Instance().LogV(false, "OSD", fmt, args)`
   - Delete `fopen("C:\\Users\\lsp\\...\\sm86_debug.log", "a")`
2. [ ] **Edit `src/proxy/d3d11_to_d3d12_bridge.cpp`**:
   - Rewrite `DiagCsv` with dynamic `<ModuleDir>\logs\sm86_present_<pid>.csv` resolution, `CreateDirectoryW`, `%LOCALAPPDATA%` fallback, and `SRWLOCK`
   - Delete `fopen("C:\\Users\\lsp\\...\\sm86_present.csv", "a")`
3. [ ] **Edit `src/addon/sm86_addon.cpp`**:
   - Rewrite `AddonLog` with dynamic `<ModuleDir>\logs\sm86_addon_<pid>.log` resolution, `CreateDirectoryW`, `%LOCALAPPDATA%` fallback, and `SRWLOCK`
   - Delete `fopen("C:\\Users\\lsp\\...\\sm86_addon.log", "a")`
4. [ ] **Harden `tools/nvp_live_test.cpp`**:
   - Add `#include "../src/proxy/early_logger.h"`
   - Replace raw `[swap + 0x18]` dereference with `InspectNvPresentSwapChain`
5. [ ] **Harden `tools/test_pe_scan.cpp`**:
   - Replace hardcoded driver store path with `sm86::LoadNvPresent()`
6. [ ] **Clean Python Scripts in `tools/`**:
   - Replace `C:\Users\lsp\WorkBuddy\...` defaults with relative paths / `sys.argv[1]`
7. [ ] **Build & Test Verification**:
   - Execute `cmd.exe /c "build.bat"` — verify clean build of all 13 targets
   - Execute `build\Release\test_proxy_hardening.exe` — verify all tests pass
   - Execute `build\Release\nvp_live_test.exe` — verify RTX 3080 live execution
   - Execute `git grep -n "C:\\\\Users" src/ tools/ include/` — verify **ZERO** remaining matches
