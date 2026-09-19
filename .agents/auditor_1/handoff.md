# Milestone 1 Auditor Handoff Report: Forensic Integrity Verdict

**Auditor**: Forensic Integrity Auditor (`teamwork_preview_auditor`)  
**Assignment**: Milestone 1: Proxy Runtime Hardening Verification  
**Date**: 2026-09-19  
**Verdict**: 🔴 **INTEGRITY VIOLATION** (Work Product Rejected)

---

## 1. Observation

1. **Hardcoded Developer Log Path in `src/proxy/osd_overlay.cpp` (Line 23)**:
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
   `src/proxy/osd_overlay.cpp` is linked into `version.dll` (`CMakeLists.txt:39`). Calls to `LogBridge` occur at lines 605 and 734 during D3D11 OSD rendering.

2. **Hardcoded Developer CSV Path in `src/proxy/d3d11_to_d3d12_bridge.cpp` (Line 119)**:
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
   `src/proxy/d3d11_to_d3d12_bridge.cpp` is linked into `version.dll` (`CMakeLists.txt:39`). Calls to `DiagCsv` occur at lines 128, 131, and 134 in `DumpDiagSummary()`.

3. **False Claim of Complete Removal in Worker 1 Deliverables**:
   Worker 1 reported in `worker_m1\changes.md` line 27:
   > *"Replaced legacy hardcoded log path (`C:\Users\lsp\Documents\antigravity\calm-carson\sm86_debug.log`) with `PROXY_LOG`."*
   And in `worker_m1\handoff.md` line 27:
   > *"This path fails in any deployed game directory."*
   Worker 1 purged the path from `sm86_rehost.cpp`, but failed to remove it from `osd_overlay.cpp` and `d3d11_to_d3d12_bridge.cpp`.

4. **Authenticity of Other Hardening Subsystems**:
   - `SafeReadPointer` (`src/proxy/early_logger.h:210–242`): Genuine alignment, canonical user address boundary, `VirtualQuery` (`MEM_COMMIT`, non-guard, readable), and hardware `__try / __except` SEH block.
   - `InspectNvPresentSwapChain` (`src/proxy/early_logger.h:245–280`): Genuine dual-vtable validation comparing swapchain against `nvBase + 0x1d3228` and internal wrapper against `nvBase + 0x1d39c0`.
   - `ResizeBuffers` & `ResizeBuffers1` (`src/proxy/sm86_rehost.cpp:606, 624`): Genuine hooking of slots 13 and 39 with `InvalidateSwapChainState` call.
   - VBlank Pacing (`src/proxy/proxy.cpp:361`): Genuine `paceVBlank(s)` with `WaitForVBlank()`.
   - Automated tests (`build\Release\test_proxy_hardening.exe`): Exits 0, 29/29 tests passed.
   - Live hardware execution (`build\Release\nvp_live_test.exe`): Exits 0, 19 FP16 fatbinaries loaded, 5 `cuGraphLaunch` executions on RTX 3080.

---

## 2. Logic Chain

1. Audit Scope & Mandatory Checks Rule 2 explicitly mandates:
   *"Verify that the hardcoded developer path (`C:\Users\lsp\Documents\antigravity\calm-carson\sm86_debug.log`) was completely removed."*
2. System Prompt Integrity Forensics Principle mandates:
   *"If ANY check fails, your verdict is INTEGRITY VIOLATION and you MUST reject the work product."*
3. From Observation 1, `src/proxy/osd_overlay.cpp` line 23 contains verbatim:
   `FILE* f = fopen("C:\\Users\\lsp\\Documents\\antigravity\\calm-carson\\sm86_debug.log", "a");`
4. From Observation 1 and `CMakeLists.txt` line 39, `osd_overlay.cpp` is an active compilation unit of the milestone's primary artifact: `version.dll`.
5. From Observation 3, Worker 1 formally claimed complete replacement of the legacy hardcoded log path with `PROXY_LOG`. This claim is empirically false.
6. Therefore, Check 2 failed, and the verdict must be `INTEGRITY VIOLATION`.

---

## 3. Caveats

- The failure is isolated to residual hardcoded developer path literals in auxiliary logging functions (`osd_overlay.cpp:23` and `d3d11_to_d3d12_bridge.cpp:119`).
- Core safety mechanisms (SEH pointer probing, wrapper inspection, dynamic IAT redirection, vtable swapchain hooking, and VBlank pacing) are technically genuine, robust, and verified on local hardware.
- No malicious backdoors or fabricated test assertions were observed.

---

## 4. Conclusion

**Verdict**: 🔴 **INTEGRITY VIOLATION**  
**Action**: Work product is **REJECTED** and returned to Worker 1 for correction.

Worker 1 must:
1. Replace `LogBridge` in `src/proxy/osd_overlay.cpp:16–28` with `PROXY_LOG` from `early_logger.h`.
2. Replace `DiagCsv` in `src/proxy/d3d11_to_d3d12_bridge.cpp:113–124` with module-relative logging or `EarlyLogger`.
3. Optionally clean `tools/nvp_live_test.cpp:336` to use `InspectNvPresentSwapChain`.
4. Rebuild and re-verify cleanly via `build.bat`.

---

## 5. Verification Method

To independently reproduce this finding:

1. **Grep Search for Hardcoded Paths**:
   ```cmd
   git grep -n "sm86_debug.log" src/
   ```
   *Observed Output*: `src/proxy/osd_overlay.cpp:23: FILE* f = fopen("C:\\Users\\lsp\\Documents\\antigravity\\calm-carson\\sm86_debug.log", "a");`

2. **Grep Search for Developer Directory**:
   ```cmd
   git grep -n "C:\\\\Users\\\\lsp" src/
   ```
   *Observed Output*:
   - `src/proxy/osd_overlay.cpp:23`
   - `src/proxy/d3d11_to_d3d12_bridge.cpp:119`
   - `src/addon/sm86_addon.cpp:41`
