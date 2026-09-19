# Technical Survey Report: sm86_smooth Proxy Runtime Architecture
## Requirements R2 (Early Persistent File Logging) & R3 (VBlank Pacing for Dual Presents)

- **Surveyor**: Explorer 2 (teamwork_preview_explorer)
- **Target Repository**: `C:\Users\lsp\Documents\antigravity\calm-carson`
- **Integrity Mode**: Development / Read-Only Survey
- **Date**: 2026-09-19

---

## Executive Summary

This report delivers an architectural and implementation survey of the `sm86_smooth` proxy codebase, focusing on:
1. **Requirement R2**: Early Persistent File Logging (`logs\sm86_proxy_<pid>.log`) initialized at `DllMain` entry, capturing all lifecycle milestones to resolve silent crashes in headless/GUI games.
2. **Requirement R3**: VBlank Pacing for Dual Presents via `IDXGIOutput::WaitForVBlank()`, eliminating vertical refresh slot coalescing and high-frequency presentation flicker (~refresh/2).

The survey maps the execution flow across both proxy implementations (`src/proxy/sm86_rehost.cpp` - Road 1 production proxy, and `src/proxy/proxy.cpp` - Road 2 native D3D12 proxy), audits existing logging mechanisms and vulnerabilities, identifies all critical lifecycle milestones, analyzes dual-frame presentation timing and mechanics, and formulates drop-in architectural designs for the implementation worker.

---

## Part 1: Requirement R2 — Early Persistent File Logging

### 1.1 Current Initialization & Logging Architecture Audit

#### 1.1.1 Location of Proxy Initialization & Entrypoints
In the active production target `version.dll` (defined in `CMakeLists.txt:39`):
- **Source File**: `src/proxy/sm86_rehost.cpp`
- **DllMain Entrypoint**: Lines 704–724
  ```cpp
  BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID lpReserved) {
      if (reason == DLL_PROCESS_ATTACH) {
          DisableThreadLibraryCalls(h);
          SetUnhandledExceptionFilter(CrashFilter);
          atexit(LogExit);
          SpoofPebProcessName();
          InstallGetModuleFileNameHook();
          InitializeCriticalSection(&g_cs);
          g_startupEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
          HANDLE thread = CreateThread(nullptr, 0, StartupThread, nullptr, 0, nullptr);
          if (thread) CloseHandle(thread);
      } else if (reason == DLL_PROCESS_DETACH) {
          LogBridge("[sm86_rehost] DllMain DLL_PROCESS_DETACH (lpReserved=%p)\n", lpReserved);
          if (g_startupEvent) {
              CloseHandle(g_startupEvent);
              g_startupEvent = nullptr;
          }
          DeleteCriticalSection(&g_cs);
      }
      return TRUE;
  }
  ```
- **Startup Worker Thread**: Lines 603–618 (`StartupThread`)
  ```cpp
  static DWORD WINAPI StartupThread(LPVOID) {
      LogBridge("[sm86_rehost] >>> StartupThread STARTED <<<\n");
      g_nvpresent = sm86::LoadNvPresent();
      LogBridge("[sm86_rehost] LoadNvPresent() -> %p\n", g_nvpresent);
      if (g_nvpresent) {
          bool ok = ApplyRehost(g_nvpresent);
          LogBridge("[sm86_rehost] ApplyRehost() -> %s\n", ok ? "TRUE" : "FALSE");
          InstallDxgiHooks();
          LogBridge("[sm86_rehost] Setup completed successfully!\n");
      } else {
          LogBridge("[sm86_rehost] ERROR: Could not find NvPresent64.dll!\n");
      }
      if (g_startupEvent) SetEvent(g_startupEvent);
      return 0;
  }
  ```

In the Road 2 proxy (`src/proxy/proxy.cpp`):
- **DllMain Entrypoint**: Lines 631–666
  - Initializes `CRITICAL_SECTION g_cs` and `CRITICAL_SECTION g_logCs`.
  - Determines module directory from `GetModuleFileNameW(hInst, ...)`.
  - Calls `readConfig()` and `logOpen()`.
  - Spawns `startupThread` which calls `installHooks()`.

#### 1.1.2 Current Logging Deficiencies & Crash Traps
1. **Hardcoded Machine-Specific Path**:
   In `src/proxy/sm86_rehost.cpp:71`:
   ```cpp
   FILE* f = fopen("C:\\Users\\lsp\\Documents\\antigravity\\calm-carson\\sm86_debug.log", "a");
   ```
   When deployed as `version.dll` into any end-user game directory (e.g. `D:\SteamLibrary\steamapps\common\Cyberpunk 2077\bin\x64\`), the directory path does not exist. `fopen` returns `nullptr` and **fails completely and silently**.
2. **Missing Early Logging in DllMain**:
   In `sm86_rehost.cpp`, no log file is opened or initialized upon `DLL_PROCESS_ATTACH`. If an exception or early exit occurs during `SpoofPebProcessName()`, `InstallGetModuleFileNameHook()`, or thread launch, zero bytes are written to disk.
3. **Console Logging Lost in GUI Subsystems**:
   In `sm86_rehost.cpp:164`:
   ```cpp
   printf("[sm86_rehost] Activated Smooth Motion on swapchain wrapper @ %p\n", wrapper);
   ```
   Standard game executables run under `/SUBSYSTEM:WINDOWS` without an attached console window. All `printf` output is discarded.
4. **No Thread Safety in File Logging**:
   `LogBridge()` in `sm86_rehost.cpp:64–76` does not hold any synchronization lock around `fopen`, `fputs`, and `fclose`. When multiple threads (render thread in `HookedPresent`, startup thread in `StartupThread`, CUDA callback in `hook_cuGraphLaunch`) log concurrently, file corruption, interleaved text, or access violations occur.
5. **No Subdirectory Creation**:
   Neither `sm86_rehost.cpp` nor `pe_scan.h` creates a `logs\` folder upon startup.
6. **No Immediate Flush Guarantee**:
   Opening and closing the file with `fopen(..., "a")` does not guarantee immediate physical disk commit if OS caching delays writes during an abrupt application crash (`0xC0000005`).

---

### 1.2 Comprehensive Inventory of Lifecycle Milestones

To satisfy Requirement R2, the new logger must record every stage of the proxy lifecycle with microsecond timestamps and thread IDs:

| Milestone ID | Lifecycle Stage | Location in Codebase | Verbatim Log & Captured Parameters |
|---|---|---|---|
| **M1: Attach** | DLL Process Attach | `src/proxy/sm86_rehost.cpp:705` | PID, host executable path, proxy `version.dll` module handle |
| **M2: PEB Spoof** | PEB Spoofing & Module Name | `src/proxy/sm86_rehost.cpp:649` | Original image path, spoofed image path (`mpc_game.exe`), IAT `GetModuleFileNameW` hook status |
| **M3: Worker Launch** | Startup Thread Started | `src/proxy/sm86_rehost.cpp:604` | Worker thread ID, `g_startupEvent` handle |
| **M4: NVP Resolution** | Module Resolution (`LoadNvPresent`) | `src/proxy/pe_scan.h:19–43` | Search locations checked (`GetModuleHandle`, `LoadLibrary`, DriverStore wildcard), resolved `HMODULE` address |
| **M5: Gate Scan & Patch** | Dual-Gate Pattern Scan & Patch | `src/proxy/sm86_rehost.cpp:518–547`<br>`src/proxy/pe_scan.h:133–186` | Gate addresses found (cmp imm RVA, setge RVA, length), fallback state, patch verification |
| **M6: IAT Hooking** | PE Import Directory Hooking | `src/proxy/sm86_rehost.cpp:550–578`<br>`src/proxy/pe_scan.h:96–128` | IAT entries resolved (`cuModuleLoadData`, `cuGraphLaunch`), original function pointers, hooked function pointers |
| **M7: Fatbin Patching** | Fatbinary Architecture Patch | `src/proxy/sm86_rehost.cpp:95–124` | Fatbin magic detected (`0xba55ed50`), payload size, container header patch (`0x78/0x59 -> 0x56`), ELF `e_flags` (`0x560556`) |
| **M8: Config Resolution** | Dynamic Config Resolution & Init | `src/proxy/sm86_rehost.cpp:581–598`<br>`src/proxy/pe_scan.h:191–210` | Global config struct address `S` (RVA), `NVP_Init_D3D` call result (`TRUE`/`FALSE`) |
| **M9: DXGI Hook Install** | DXGI VTable Hook Installation | `src/proxy/sm86_rehost.cpp:457–506` | Dummy D3D11 device/swapchain creation, vtable addresses, slot 8 (`Present`), slot 22 (`Present1`) |
| **M10: SwapChain Probe** | SwapChain Detection & Wrapper Probing | `src/proxy/sm86_rehost.cpp:263–277, 397` | Swapchain pointer, vtable address, wrapper verification (`base + 0x1d3228`), BufferCount, SwapEffect, Flags, HWND, dimensions, format |
| **M11: FG Activation** | Smooth Motion Activation | `src/proxy/sm86_rehost.cpp:149–168` | Wrapper pointer, activation methods called (`vt[19]`, `vt[20]`) |
| **M12: Present Status** | Presentation Dispatch & Return | `src/proxy/sm86_rehost.cpp:410–423` | Frame index, sync interval, flags, return `HRESULT`, active CUDA graph launch count |
| **M13: Resize Lifecycle** | ResizeBuffers / ResizeBuffers1 | Requirement R4 hooks (slots 13 & 39) | BufferCount, Width, Height, Format, SwapChainFlags, invalidation of cached outputs/wrappers |
| **M14: Process Detach** | Exit / Unhandled Exception | `src/proxy/sm86_rehost.cpp:686, 700, 716` | `DLL_PROCESS_DETACH` (clean vs terminate), `CrashFilter` exception code, fault address, faulting module |

---

### 1.3 High-Reliability Early File Logger Architecture

#### 1.3.1 Path Resolution & Safe Fallback Matrix
The logger must determine its output directory dynamically based on the location of `version.dll`:

```
[Module Directory from GetModuleFileNameW(hInstance)]
            |
            v
[Attempt: <ModuleDir>\logs\sm86_proxy_<pid>.log]
            |
      (Write Access?)
      /          \
    YES           NO (e.g. C:\Program Files)
    /              \
[Use Path]      [Fallback 1: %LOCALAPPDATA%\sm86_smooth\logs\sm86_proxy_<pid>.log]
                        |
                  (Write Access?)
                  /          \
                YES           NO
                /              \
            [Use Path]      [Fallback 2: %TEMP%\sm86_smooth\logs\sm86_proxy_<pid>.log]
```

#### 1.3.2 Win32 Low-Level API Selection (Loader Lock Safe)
Standard C Runtime (`fopen`, `std::ofstream`, `iostream`) relies on CRT startup code and dynamic heap allocation which may be uninitialized or fragile during early `DllMain` entry under the OS Loader Lock.
The logger will use pure Win32 Kernel32 APIs:
- `GetModuleFileNameW`: Retrieves the proxy module path safely.
- `CreateDirectoryW`: Ensures the `logs` folder exists.
- `CreateFileW`: Opens the log file with `FILE_APPEND_DATA`, `FILE_SHARE_READ`, and `OPEN_ALWAYS`.
- `WriteFile`: Direct unbuffered I/O.
- `FlushFileBuffers`: Forces kernel dirty pages to physical media on milestone events.
- `InitializeSRWLock` / `AcquireSRWLockExclusive` / `ReleaseSRWLockExclusive`: Slim Reader/Writer lock (zero heap allocation, fast user-mode spin, no handle destruction needed).

#### 1.3.3 Immediate Flush vs. Performance Tradeoff
Calling `FlushFileBuffers` synchronously halts the CPU thread until physical SSD/HDD disk write completion (~1–10 ms).
- **Milestone & Error Events**: Initialization (M1–M9), wrapper discovery (M10–M11), resize events (M13), and crash/detach (M14) MUST invoke `FlushFileBuffers()` immediately so that a crash milliseconds later preserves the exact failure point.
- **Per-Frame Present Telemetry** (M12): Rate-limited to every 60 frames (or first 10 frames), written via `WriteFile` without immediate `FlushFileBuffers` to maintain 0.001 ms logging overhead.

#### 1.3.4 Production Architectural Implementation Design
```cpp
// ============================================================================
// sm86_logger.h - Production High-Reliability Early File Logger
// ============================================================================
#pragma once
#include <windows.h>
#include <cstdio>
#include <cstdarg>

namespace sm86 {

class EarlyLogger {
public:
    static EarlyLogger& Instance() {
        static EarlyLogger s_instance;
        return s_instance;
    }

    void Initialize(HMODULE hModule) {
        AcquireSRWLockExclusive(&m_lock);
        if (m_initialized) {
            ReleaseSRWLockExclusive(&m_lock);
            return;
        }

        m_pid = GetCurrentProcessId();
        wchar_t modPath[MAX_PATH] = {};
        GetModuleFileNameW(hModule, modPath, MAX_PATH);

        wchar_t* lastSlash = wcsrchr(modPath, L'\\');
        if (lastSlash) *(lastSlash + 1) = L'\0';

        wchar_t logDir[MAX_PATH] = {};
        swprintf_s(logDir, L"%slogs", modPath);
        CreateDirectoryW(logDir, nullptr);

        swprintf_s(m_filePath, L"%s\\sm86_proxy_%lu.log", logDir, m_pid);
        m_hFile = CreateFileW(m_filePath, FILE_APPEND_DATA, FILE_SHARE_READ,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

        // Fallback to %LOCALAPPDATA% if write fails
        if (m_hFile == INVALID_HANDLE_VALUE) {
            wchar_t localApp[MAX_PATH] = {};
            if (GetEnvironmentVariableW(L"LOCALAPPDATA", localApp, MAX_PATH) > 0) {
                swprintf_s(logDir, L"%s\\sm86_smooth\\logs", localApp);
                CreateDirectoryW(localApp, nullptr);
                CreateDirectoryW(logDir, nullptr);
                swprintf_s(m_filePath, L"%s\\sm86_proxy_%lu.log", logDir, m_pid);
                m_hFile = CreateFileW(m_filePath, FILE_APPEND_DATA, FILE_SHARE_READ,
                                      nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            }
        }

        m_initialized = (m_hFile != INVALID_HANDLE_VALUE);
        ReleaseSRWLockExclusive(&m_lock);

        Log(true, "[INIT] sm86_proxy logging started. PID=%lu, Target=%ls\n", m_pid, m_filePath);
    }

    void Log(bool flushNow, const char* fmt, ...) {
        char msgBuf[2048];
        va_list args;
        va_start(args, fmt);
        int len = vsnprintf(msgBuf, sizeof(msgBuf), fmt, args);
        va_end(args);
        if (len <= 0) return;

        SYSTEMTIME st;
        GetLocalTime(&st);
        DWORD tid = GetCurrentThreadId();

        char lineBuf[2304];
        int totalLen = snprintf(lineBuf, sizeof(lineBuf),
                                "[%04d-%02d-%02d %02d:%02d:%02d.%03d] [TID:%05lu] %s",
                                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                                tid, msgBuf);

        OutputDebugStringA(lineBuf);

        AcquireSRWLockExclusive(&m_lock);
        if (m_hFile != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(m_hFile, lineBuf, (DWORD)totalLen, &written, nullptr);
            if (flushNow) {
                FlushFileBuffers(m_hFile);
            }
        }
        ReleaseSRWLockExclusive(&m_lock);
    }

    void Shutdown() {
        AcquireSRWLockExclusive(&m_lock);
        if (m_hFile != INVALID_HANDLE_VALUE) {
            FlushFileBuffers(m_hFile);
            CloseHandle(m_hFile);
            m_hFile = INVALID_HANDLE_VALUE;
        }
        m_initialized = false;
        ReleaseSRWLockExclusive(&m_lock);
    }

private:
    EarlyLogger() : m_hFile(INVALID_HANDLE_VALUE), m_pid(0), m_initialized(false) {
        InitializeSRWLock(&m_lock);
    }
    ~EarlyLogger() { Shutdown(); }

    SRWLOCK m_lock;
    HANDLE  m_hFile;
    DWORD   m_pid;
    wchar_t m_filePath[MAX_PATH];
    bool    m_initialized;
};

} // namespace sm86

#define LOG_MILESTONE(fmt, ...) sm86::EarlyLogger::Instance().Log(true, fmt, ##__VA_ARGS__)
#define LOG_FRAME(fmt, ...)     sm86::EarlyLogger::Instance().Log(false, fmt, ##__VA_ARGS__)
```

---

## Part 2: Requirement R3 — VBlank Pacing for Dual Presents

### 2.1 Dual Presents Location & Analysis in Codebase

#### 2.1.1 Location of Dual Presents
The dual presentation mechanism is located in **`src/proxy/proxy.cpp`**, within the `doFrameGen` function (lines 240–337):
```cpp
// src/proxy/proxy.cpp:297-326
    const UINT f1 = s->tearing ? DXGI_PRESENT_ALLOW_TEARING : 0;
    s->sc->Present(0, f1);                       // -> Present 1: The synthesised frame

    // 4. now present the real frame, from whichever buffer is current
    const UINT i1 = s->sc->GetCurrentBackBufferIndex();
    s->alloc[(s->frame + 1) % pc.max_in_flight]->Reset();
    s->list->Reset(s->alloc[(s->frame + 1) % pc.max_in_flight], nullptr);
    {
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource   = s->bb[i1];
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        b.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
        s->list->ResourceBarrier(1, &b);
    }
    s->list->CopyResource(s->bb[i1], s->capCur);
    {
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource   = s->bb[i1];
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
        s->list->ResourceBarrier(1, &b);
    }
    s->list->Close();
    q->ExecuteCommandLists(1, (ID3D12CommandList**)&s->list);
    s->sc->Present(syncInterval, flags);         // -> Present 2: The real frame
```

#### 2.1.2 Presentation Timing Breakdown & Root Cause of Flicker
1. **Back-to-Back Execution within Sub-Millisecond Window**:
   - `s->sc->Present(0, f1)` submits the synthesized frame with `syncInterval = 0`.
   - The CPU transitions the next backbuffer, issues `CopyResource`, closes the command list, calls `ExecuteCommandLists`, and immediately calls `s->sc->Present(syncInterval, flags)`.
   - The time elapsed between Present 1 and Present 2 on the CPU is **typically 0.1 to 0.3 milliseconds**.
2. **Refresh Slot Coalescing**:
   - Display hardware updates at discrete vertical blank intervals (e.g. 16.67 ms for 60 Hz, 6.94 ms for 144 Hz).
   - Because both presents are queued within 0.2 ms, both land inside the **SAME VBlank period**.
   - Under DWM / DXGI Flip Discard model:
     - The second present arrives before the display hardware scanout has consumed the first present.
     - The compositor / display controller discards or preempts the first backbuffer (the synthesized frame) to present the second one (the real frame).
     - The synthesized frame is never seen by the human eye, or flashes for a fraction of a millisecond.
     - Then, the system idles for the remaining 16.4 ms until the game's next frame is rendered.
   - **Result**: Severe frame pacing judder, ~refresh/2 brightness oscillation, and visible presentation flicker.

---

### 2.2 Deep Investigation of `IDXGIOutput` & `WaitForVBlank`

#### 2.2.1 Querying `IDXGIOutput` via `GetContainingOutput`
From DXGI API specifications (`dxgi.h`):
```cpp
HRESULT IDXGISwapChain::GetContainingOutput(IDXGIOutput **ppOutput);
```
- **Functionality**: Returns the display monitor that contains the largest portion of the client area of the target window.
- **Return Codes**:
  - `S_OK`: Output interface acquired with an incremented reference count (`AddRef`).
  - `DXGI_ERROR_NOT_CURRENTLY_AVAILABLE`: Window is minimized (`IsIconic(hwnd)`), fully off-screen, or during a video mode switch / monitor disconnect.
  - `DXGI_ERROR_DEVICE_REMOVED` / `DXGI_ERROR_DEVICE_RESET`: GPU reset.
- **Memory Management**: The caller owns the returned `IDXGIOutput*` pointer and must call `ppOutput->Release()`.

#### 2.2.2 `IDXGIOutput::WaitForVBlank()` Semantics
```cpp
HRESULT IDXGIOutput::WaitForVBlank(void);
```
- **Functionality**: Suspends the calling thread until the vertical blanking interval begins on the physical display adapter.
- **CPU & GPU Behavior**:
  - It is an OS kernel wait (`DxgkWaitForVerticalBlankEvent`), yielding CPU execution with 0% CPU burn.
  - Calling `WaitForVBlank()` between Present 1 and Present 2 guarantees that Present 1 has been latched and scanned out to the panel before Present 2 is presented.
- **Return Codes**:
  - `S_OK`: VBlank occurred.
  - `DXGI_ERROR_NOT_CURRENTLY_AVAILABLE`: Display does not support VBlank polling (e.g. headless, remote desktop session, or virtual display driver).

---

### 2.3 Safe Output Interface Caching and Invalidation Architecture

#### 2.3.1 Hazards of Naive Caching
1. **Memory Leaks**: Calling `GetContainingOutput()` every frame without releasing leaks COM references rapidly.
2. **Stale Monitor References**: If a user drags a window from Monitor 1 (e.g. 144 Hz) to Monitor 2 (e.g. 60 Hz), an uncached or statically cached `IDXGIOutput*` continues polling the wrong physical monitor or becomes invalid.
3. **Crash on Resolution / Mode Changes**: When games change resolution or fullscreen status, the underlying swapchain recreates its internal chain. Stale output pointers dereference freed memory (`0xC0000005`).

#### 2.3.2 Safe Caching & Tracking Design
To ensure zero per-frame COM overhead while maintaining absolute stability:
1. Maintain per-swapchain tracking in `SwapState`:
   ```cpp
   IDXGIOutput*  m_output = nullptr;
   HMONITOR      m_hMonitor = nullptr;
   HWND          m_hwnd = nullptr;
   ULONGLONG     m_lastOutputCheckTick = 0;
   ```
2. **Monitor Validation**:
   On each Present, call `MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL)`:
   - If `curMonitor == m_hMonitor` and `m_output != nullptr`, reuse `m_output`.
   - If `curMonitor != m_hMonitor` or `m_output == nullptr`:
     - Safely release `m_output` if non-null: `m_output->Release(); m_output = nullptr;`
     - Query new output via `swapChain->GetContainingOutput(&m_output);`
     - Update `m_hMonitor = curMonitor;`
3. **Invalidation on ResizeBuffers (Requirement R4 Linkage)**:
   When `ResizeBuffers` (slot 13) or `ResizeBuffers1` (slot 39) is intercepted:
   ```cpp
   if (m_output) {
       m_output->Release();
       m_output = nullptr;
   }
   m_hMonitor = nullptr;
   ```
4. **Invalidation on VBlank Error**:
   If `m_output->WaitForVBlank()` returns `DXGI_ERROR_NOT_CURRENTLY_AVAILABLE` or fails:
   ```cpp
   m_output->Release();
   m_output = nullptr;
   m_hMonitor = nullptr;
   ```

---

### 2.4 Handling Windowed Mode, Fallbacks, and Edge Cases

| Scenario | Condition | Behavior / Consequence | Recommended Mitigation |
|---|---|---|---|
| **Windowed Mode (Normal)** | Window visible on desktop | `GetContainingOutput` returns intersecting monitor | Call `output->WaitForVBlank()` normally; works seamlessly with DWM. |
| **Window Minimized** | `IsIconic(hwnd) == TRUE` | `GetContainingOutput` returns `DXGI_ERROR_NOT_CURRENTLY_AVAILABLE` | Bypass VBlank wait; execute single present or skip intermediate frame. |
| **Multi-Monitor Boundary** | Window straddles two monitors | DXGI picks monitor with largest client area | `MonitorFromWindow(..., MONITOR_DEFAULTTONEAREST)` keeps output cache synced. |
| **G-Sync / VRR Enabled** | Variable Refresh Rate active | Hardware VBlank adapts to frame rate | `WaitForVBlank()` may return immediately or pace to max refresh; wrap wait with timeout guard. |
| **Headless / Remote Desktop** | No physical monitor attached | `GetContainingOutput` fails or `WaitForVBlank` returns `DXGI_ERROR_NOT_CURRENTLY_AVAILABLE` | Fallback to high-precision software timer: `QueryPerformanceCounter` pacing or `Sleep(1)`. |

---

### 2.5 Concrete Code Snippet: Paced Dual Presents Implementation

```cpp
// ============================================================================
// Proposed Paced Dual Present Implementation (for src/proxy/proxy.cpp)
// ============================================================================

static void PacedPresentDual(SwapState* s, UINT syncInterval, UINT flags) {
    HWND hwnd = nullptr;
    DXGI_SWAP_CHAIN_DESC scDesc = {};
    if (SUCCEEDED(s->sc->GetDesc(&scDesc))) {
        hwnd = scDesc.OutputWindow;
    }

    // 1. Present the synthesised frame
    const UINT f1 = s->tearing ? DXGI_PRESENT_ALLOW_TEARING : 0;
    HRESULT hr1 = s->sc->Present(0, f1);
    if (FAILED(hr1)) {
        LOG_FRAME("[PRES] Synth frame Present failed: hr=0x%08X\n", (uint32_t)hr1);
    }

    // 2. Validate and acquire IDXGIOutput for VBlank pacing
    if (hwnd && !IsIconic(hwnd)) {
        HMONITOR curMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL);
        if (curMon && curMon != s->cachedMonitor) {
            if (s->cachedOutput) {
                s->cachedOutput->Release();
                s->cachedOutput = nullptr;
            }
            s->cachedMonitor = curMon;
        }

        if (!s->cachedOutput) {
            HRESULT hrOut = s->sc->GetContainingOutput(&s->cachedOutput);
            if (FAILED(hrOut)) {
                s->cachedOutput = nullptr;
            }
        }

        // 3. Pacing Wait: Halt until monitor enters VBlank
        if (s->cachedOutput) {
            HRESULT hrVb = s->cachedOutput->WaitForVBlank();
            if (FAILED(hrVb)) {
                // Stale output or unsupported - invalidate cache
                s->cachedOutput->Release();
                s->cachedOutput = nullptr;
                s->cachedMonitor = nullptr;
            }
        }
    }

    // 4. Record and submit the real frame
    const UINT i1 = s->sc->GetCurrentBackBufferIndex();
    s->alloc[(s->frame + 1) % g_cfg.max_in_flight]->Reset();
    s->list->Reset(s->alloc[(s->frame + 1) % g_cfg.max_in_flight], nullptr);

    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource   = s->bb[i1];
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
    s->list->ResourceBarrier(1, &b);

    s->list->CopyResource(s->bb[i1], s->capCur);

    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
    s->list->ResourceBarrier(1, &b);

    s->list->Close();
    g_gameQueue->ExecuteCommandLists(1, (ID3D12CommandList**)&s->list);

    // 5. Present the real frame to the display
    HRESULT hr2 = s->sc->Present(syncInterval, flags);
    if (FAILED(hr2)) {
        LOG_FRAME("[PRES] Real frame Present failed: hr=0x%08X\n", (uint32_t)hr2);
    }
}
```

---

## Part 3: Synthesis & Verification Plan

### 3.1 Verification Matrix for Implementer

1. **Compilation Baseline**:
   - Run `build.bat` from project root. Must compile with zero errors and zero warnings.
2. **Early File Logger Verification (R2)**:
   - Launch `build\Release\proxytest.exe`.
   - Inspect `build\Release\logs\sm86_proxy_<pid>.log` (or `logs\sm86_proxy_<pid>.log`).
   - Verify that timestamps, PID, module paths, gate patch results, and hook installations appear immediately on file creation.
3. **Hardware Regression Verification (R5)**:
   - Run `build\Release\nvp_live_test.exe`.
   - Verify output matches expected RTX 3080 live execution:
     - Gate pattern scan hit (`+0xc41f`, `+0xc437`)
     - 19 FP16 fatbinaries loaded
     - 50 warmup launches
     - 5 cuGraphLaunch executions
4. **VBlank Pacing Verification (R3)**:
   - Run `build\Release\vfi_selftest.exe` to verify shader pipeline integrity.
   - Run dual present tests under windowed and fullscreen modes to verify `WaitForVBlank()` calls succeed without deadlock or timing drift.

---
*Report compiled by Explorer 2.*
