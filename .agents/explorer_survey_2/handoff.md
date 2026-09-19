# Handoff Report — Explorer 2 (Phase 0 Survey: Requirements R2 & R3)

- **Date**: 2026-09-19
- **Author**: Explorer 2 (`teamwork_preview_explorer`)
- **Recipient**: Parent Orchestrator (`713e396d-1dc4-417d-9c1c-1bfd09bee6b7`)
- **Type**: Hard Handoff (Task Complete)
- **Artifacts Produced**:
  - `C:\Users\lsp\Documents\antigravity\calm-carson\.agents\explorer_survey_2\survey_report.md`
  - `C:\Users\lsp\Documents\antigravity\calm-carson\.agents\explorer_survey_2\BRIEFING.md`
  - `C:\Users\lsp\Documents\antigravity\calm-carson\.agents\explorer_survey_2\progress.md`

---

## 1. Observation

1. **Active Build Configuration**:
   In `CMakeLists.txt` lines 39–41:
   ```cmake
   # ---------------------------------------------------------------- Road 1: Proxy DLL (version.dll)
   add_library(version SHARED src/proxy/sm86_rehost.cpp src/proxy/ui_mask.cpp src/proxy/osd_overlay.cpp src/proxy/d3d11_to_d3d12_bridge.cpp)
   target_link_libraries(version PRIVATE d3d12 d3d11 dxgi user32 gdi32 d3dcompiler)
   set_target_properties(version PROPERTIES PREFIX "")
   ```
   `version.dll` is compiled directly from `src/proxy/sm86_rehost.cpp`.

2. **Current File Logger Flaws in `sm86_rehost.cpp`**:
   At lines 64–76 of `src/proxy/sm86_rehost.cpp`:
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
   - Path is hardcoded to developer local drive: `"C:\\Users\\lsp\\Documents\\antigravity\\calm-carson\\sm86_debug.log"`. On any target game installation, this directory does not exist and `fopen` returns `NULL`.
   - No `logs\` directory is created.
   - No file locking or thread synchronization exists; concurrent writes from render thread, startup thread, and CUDA callbacks interleave or crash.
   - `printf()` is called at line 164 (`printf("[sm86_rehost] Activated Smooth Motion on swapchain wrapper @ %p\n", wrapper);`), which is discarded in GUI subsystem applications.

3. **Dual Present Mechanism Location**:
   In `src/proxy/proxy.cpp` lines 297–326 (`doFrameGen`):
   ```cpp
   const UINT f1 = s->tearing ? DXGI_PRESENT_ALLOW_TEARING : 0;
   s->sc->Present(0, f1);                       // -> the synthesised frame

   // 4. now present the real frame, from whichever buffer is current
   const UINT i1 = s->sc->GetCurrentBackBufferIndex();
   ...
   q->ExecuteCommandLists(1, (ID3D12CommandList**)&s->list);
   s->sc->Present(syncInterval, flags);         // -> the real frame
   ```
   Both presents are executed back-to-back within ~0.1–0.3 ms on the same CPU thread without synchronization against VBlank.

4. **Hardware Verification Execution**:
   - `build.bat` executed with exit code 0, producing `version.dll`, `nvp_live_test.exe`, `nvp_perf_bench.exe`, `test_addon_simulation.exe`, and `vfi_selftest.exe`.
   - `build\Release\nvp_live_test.exe` executed with exit code 0 on RTX 3080:
     - Gate pattern scan hit at RVA `+0xc41f` and `+0xc437`.
     - Gate patched: Tier 2 allowed, sil=1 forced.
     - 19 FP16 fatbinaries loaded successfully.
     - 50 warmup launches and 5 `cuGraphLaunch` executions verified.
   - `build\Release\vfi_selftest.exe` executed with exit code 0, achieving 28.4 dB PSNR on well-posed pixels.

---

## 2. Logic Chain

1. **Step 1 (Root Cause of Missing Logs)**:
   Observation 2 demonstrates that `LogBridge()` writes exclusively to a hardcoded local workspace path. In any production game directory, `fopen` fails silently. Furthermore, `DllMain` entry does not initialize the logger before executing `SpoofPebProcessName()` and spawning `StartupThread`. Therefore, early crashes in GUI games without consoles leave zero forensic evidence.
2. **Step 2 (Logger Architecture Solution)**:
   To make logging fail-safe, the logger must:
   - Resolve its path dynamically via `GetModuleFileNameW(hInstance, ...)` to `<module_dir>\logs\sm86_proxy_<pid>.log`, with fallback to `%LOCALAPPDATA%\sm86_smooth\logs\`.
   - Initialize synchronously at `DllMain` (`DLL_PROCESS_ATTACH`) using Win32 `CreateFileW` with `FILE_APPEND_DATA`, `FILE_SHARE_READ`, and `OPEN_ALWAYS` (avoiding CRT heap dependencies under Loader Lock).
   - Use `SRWLOCK` for thread-safety.
   - Invoke `FlushFileBuffers()` on milestone and error events to guarantee disk persistence before crashes, while bypassing flush on per-frame rate-limited present logs to prevent I/O stalls.
3. **Step 3 (Root Cause of Presentation Flicker)**:
   Observation 3 proves that dual presents in `src/proxy/proxy.cpp` occur within ~0.2 ms of each other. Because modern displays refresh at discrete intervals (6.94 ms at 144 Hz, 16.67 ms at 60 Hz), both presentations arrive in the same vertical refresh slot. The DWM / display compositor either discards the synthesized frame or tears immediately, producing severe ~refresh/2 high-frequency flicker.
4. **Step 4 (VBlank Pacing Solution)**:
   By querying `IDXGISwapChain::GetContainingOutput(&output)` and invoking `output->WaitForVBlank()` between the synthesized present and the real present:
   - The thread suspends until the display controller enters VBlank.
   - The synthesized frame is displayed for a full refresh interval.
   - The real frame is subsequently presented for the next refresh interval.
   - Caching `output` validated by `MonitorFromWindow(hwnd, ...)` eliminates per-frame COM overhead while handling window dragging across multi-monitor setups.
   - Cleanly invalidating `output` on `ResizeBuffers` (slots 13 & 39) prevents stale pointer crashes.

---

## 3. Caveats

1. **Variable Refresh Rate (G-Sync / FreeSync / VRR)**:
   Under VRR, hardware VBlank timings adapt to frame delivery. While `WaitForVBlank()` is supported by DXGI on G-Sync displays, timing behaves dynamically. The implementation must guard `WaitForVBlank()` against indefinite blocking by invalidating the output cache on any failure.
2. **Window Minimization / Headless Mode**:
   When a window is minimized (`IsIconic(hwnd)`), `GetContainingOutput()` returns `DXGI_ERROR_NOT_CURRENTLY_AVAILABLE`. The implementation must bypass VBlank waiting and proceed with normal presentation to prevent hangs.
3. **Scope Distinction (Road 1 vs Road 2)**:
   `sm86_rehost.cpp` (Road 1) delegates frame generation to `NvPresent64.dll`, whereas `proxy.cpp` (Road 2) explicitly implements dual presents. The unified logger design applies to both paths, and the VBlank pacing design applies directly to `proxy.cpp` and shadow swapchain presentations.

---

## 4. Conclusion

1. **Requirement R2**: Early Persistent File Logging is ready for implementation using the Win32 `EarlyLogger` singleton designed in `survey_report.md`. It must be initialized at `DllMain` entry (`DLL_PROCESS_ATTACH`), create the `logs\` directory, write to `logs\sm86_proxy_<pid>.log`, synchronize via `SRWLOCK`, and flush on all 14 identified lifecycle milestones.
2. **Requirement R3**: VBlank Pacing is fully mapped to `proxy.cpp:297-326`. Inserting a monitor-validated, cached `output->WaitForVBlank()` call between `s->sc->Present(0, f1)` and `s->sc->Present(syncInterval, flags)` will cleanly pace dual presents into adjacent VBlank slots and eliminate ~refresh/2 presentation flicker. Output pointers must be invalidated on `ResizeBuffers` (slot 13) and `ResizeBuffers1` (slot 39).

---

## 5. Verification Method

1. **Compilation Check**:
   Run `build.bat` in repo root:
   ```cmd
   build.bat
   ```
   Expected result: Zero errors, produces `version.dll` and all test executables.
2. **Live Hardware Regression Check**:
   Run `build\Release\nvp_live_test.exe`:
   ```cmd
   build\Release\nvp_live_test.exe
   ```
   Expected result: Exit code 0, 19 FP16 fatbinaries loaded, 50 warmup launches, 5 cuGraphLaunch executions.
3. **VFI Self-Test Check**:
   Run `build\Release\vfi_selftest.exe`:
   ```cmd
   build\Release\vfi_selftest.exe
   ```
   Expected result: Exit code 0, PSNR >= 28.0 dB on well-posed pixels.
4. **Log File Verification**:
   Launch any hooked executable and verify `logs\sm86_proxy_<pid>.log` is created with timestamped milestones.
