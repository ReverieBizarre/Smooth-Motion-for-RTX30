# Explorer 3 Handoff Report: Milestone 1 Remediation (Iteration 2)

**Agent**: Explorer 3 (`teamwork_preview_explorer`)  
**Role**: Teamwork Explorer (Investigation & Synthesis)  
**Working Directory**: `C:\Users\lsp\Documents\antigravity\calm-carson\.agents\explorer_rem_3`  
**Date**: 2026-09-19  
**Handoff Type**: Hard (Investigation complete)  

---

## 1. Observation

1. **Missing GPU Flush and Asynchronous Teardown in `D3D11ToD3D12Bridge::Shutdown()`**:
   - Location: `src/proxy/d3d11_to_d3d12_bridge.cpp` lines 194–244.
   - Code observed:
     ```cpp
     void D3D11ToD3D12Bridge::Shutdown() {
         DumpDiagSummary();
         if (m_hwnd && g_subclassedParentHwnd == m_hwnd && g_subclassedParentOrigProc && IsWindow(m_hwnd)) {
             SetWindowLongPtrA(m_hwnd, GWLP_WNDPROC, (LONG_PTR)g_subclassedParentOrigProc);
             g_subclassedParentOrigProc = nullptr;
             g_subclassedParentHwnd = nullptr;
         }
         m_origParentWndProc = nullptr;

         if (m_sharedHandle) {
             CloseHandle(m_sharedHandle);
             m_sharedHandle = nullptr;
         }
         if (m_fenceEvent12) {
             CloseHandle(m_fenceEvent12);
             m_fenceEvent12 = nullptr;
         }
         if (m_fence12)        { m_fence12->Release(); m_fence12 = nullptr; }
         for (int i = 0; i < 8; i++) {
             if (m_backbuffers12[i]) { m_backbuffers12[i]->Release(); m_backbuffers12[i] = nullptr; }
         }
         if (m_query11)        { m_query11->Release(); m_query11 = nullptr; }
         // The waitable handle is owned by the swapchain - do NOT CloseHandle it.
         m_frameLatencyWaitable = nullptr;
         if (m_swap2_12)       { m_swap2_12->Release(); m_swap2_12 = nullptr; }
         if (m_sharedTex12)    { m_sharedTex12->Release(); m_sharedTex12 = nullptr; }
         if (m_swap3_12)       { m_swap3_12->Release(); m_swap3_12 = nullptr; }
         if (m_swap12)         { m_swap12->Release(); m_swap12 = nullptr; }
         if (m_cl12)           { m_cl12->Release(); m_cl12 = nullptr; }
         if (m_alloc12)        { m_alloc12->Release(); m_alloc12 = nullptr; }
         if (m_cq12)           { m_cq12->Release(); m_cq12 = nullptr; }
         if (m_dev12)          { m_dev12->Release(); m_dev12 = nullptr; }
         if (m_sharedTex11)    { m_sharedTex11->Release(); m_sharedTex11 = nullptr; }
         if (m_ctx11)          { m_ctx11->Release(); m_ctx11 = nullptr; }
         if (m_childHwnd && IsWindow(m_childHwnd)) {
             ShowWindow(m_childHwnd, SW_HIDE);
             DestroyWindow(m_childHwnd);
             m_childHwnd = nullptr;
         }
     ```
   - In contrast, `D3D11ToD3D12Bridge::Present` invokes `m_swap12->Present(shadowSync, shadowFlags)` (line 751), which submits work asynchronously to the display flip queue and NvPresent64's background worker threads. `Shutdown()` immediately closes handles, releases backbuffers and swapchain interfaces, and calls `DestroyWindow(m_childHwnd)` without waiting for GPU execution to complete.

2. **Blind SwapChain Dereference in Test Binaries**:
   - `tools/nvp_live_test.cpp` line 336:
     ```cpp
     void* wrapper = *(void**)((uint8_t*)swap + 0x18);
     printf("[+] SwapChain created @ %p (Proxy COM Object, Internal Wrapper @ %p)\n", swap, wrapper);
     ```
   - `tools/nvp_perf_bench.cpp` line 144:
     ```cpp
     void* wrapper = *(void**)((uint8_t*)swap + 0x18);
     if (wrapper) {
         void** vt = *(void***)wrapper;
         typedef void (*pfnSetByte)(void*, uint8_t);
         ((pfnSetByte)vt[19])(wrapper, 1);
         ((pfnSetByte)vt[20])(wrapper, 1);
     }
     ```
   - Both directly dereference offset `+0x18` of `IDXGISwapChain1* swap` without verifying the proxy vtable RVA (`base + 0x1d3228`) or using SEH memory probing.

3. **Existing Safe Probing Function `InspectNvPresentSwapChain`**:
   - Location: `src/proxy/early_logger.h` lines 245–280.
   - Accurately checks 5 safety stages: alignment, outer proxy vtable (`0x1d3228`), SEH memory read of `+0x18`, wrapper vtable (`0x1d39c0`), and returns the verified pointer. If `nvpresentBase == 0`, it dynamically resolves `GetModuleHandleA("NvPresent64.dll")`.

4. **Forensic Auditor Finding on Residual Hardcoded Paths**:
   - `src/proxy/osd_overlay.cpp` line 23:
     `FILE* f = fopen("C:\\Users\\lsp\\Documents\\antigravity\\calm-carson\\sm86_debug.log", "a");`
   - `src/proxy/d3d11_to_d3d12_bridge.cpp` line 119:
     `FILE* f = fopen("C:\\Users\\lsp\\Documents\\antigravity\\calm-carson\\sm86_present.csv", "a");`

5. **Challenger 2 Confirmed Defect on Re-entrancy & Dummy SwapChain**:
   - `HookedResizeBuffers` (`sm86_rehost.cpp:262`) lacks `thread_local bool t_inResize = false;`, causing inner subordinate DXGI swapchains to trigger heap corruption and a `0xC0000005` crash.
   - `InstallDxgiHooks` (`sm86_rehost.cpp:583`) creates a dummy swapchain with `sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD`, causing `ResizeBuffers1` (slot 39) to return `0x887A0001` (`DXGI_ERROR_INVALID_CALL`).

---

## 2. Logic Chain

1. **GPU Command Queue Flush Necessity in `Shutdown()`**:
   - From Observation 1, `Present` queues asynchronous work to the GPU and driver.
   - When the user resizes a window or changes display modes, `HookedResizeBuffers` calls `InvalidateSwapChainState`, which triggers `Shutdown()`.
   - Releasing `m_swap12` and backbuffers while the GPU is still rendering or the presentation engine is flipping results in race conditions (`DXGI_ERROR_DEVICE_REMOVED`, use-after-free in NVIDIA driver threads, or black flashes).
   - Calling `m_cq12->Signal(m_fence12, ++m_fenceVal12)` and `WaitForSingleObject(m_fenceEvent12, 2000)` guarantees that all GPU work submitted prior to shutdown is retired before CPU destruction begins.
   - Furthermore, waiting on `m_frameLatencyWaitable` ensures the swapchain has retired the in-flight frame.

2. **Blind Dereference Elimination in Test Tools**:
   - From Observation 2, test tools `nvp_live_test.cpp` and `nvp_perf_bench.cpp` perform blind dereferencing of `[swap + 0x18]`.
   - If an interposer (e.g. Streamline, Reflex, ReShade, OBS) or native swapchain is active, offset `+0x18` contains non-wrapper data or uncommitted memory, leading to an immediate access violation.
   - From Observation 3, `InspectNvPresentSwapChain` in `early_logger.h` provides guaranteed protection against this failure mode. Including `early_logger.h` in the tools aligns all project binaries with Requirement R1.

3. **Harmonized Remediation**:
   - From Observations 4 and 5, combining the GPU synchronization fix with the re-entrancy guard, flip discard dummy swapchain, and removal of hardcoded paths creates a comprehensive remediation plan that satisfies both Challenger 2 and Auditor 1.

---

## 3. Caveats

- **Timeouts**: The fence wait uses a 2000 ms timeout rather than `INFINITE` to prevent deadlocks in case of severe GPU hang / TDR. If a GPU hang occurs, the timeout expires and resources are still reclaimed.
- **Waitable Object Ownership**: `m_frameLatencyWaitable` is returned by `IDXGISwapChain2::GetFrameLatencyWaitableObject()`. It is owned by DXGI; the caller must NOT call `CloseHandle()` on it (simply set `m_frameLatencyWaitable = nullptr`).

---

## 4. Conclusion

The defects identified by Challenger 2 and Auditor 1 have clear root causes and concrete, non-breaking remedies:
1. In `src/proxy/d3d11_to_d3d12_bridge.cpp:Shutdown()`, insert D3D11 flush, D3D12 command queue signal/fence wait (2000 ms), and frame-latency wait (1000 ms) before releasing resources.
2. In `tools/nvp_live_test.cpp` and `tools/nvp_perf_bench.cpp`, replace `*(void**)((uint8_t*)swap + 0x18)` with `InspectNvPresentSwapChain`.
3. In `src/proxy/sm86_rehost.cpp`, add `static thread_local bool t_inResize = false;` to `HookedResizeBuffers` and `HookedResizeBuffers1`, and set `sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;` in `InstallDxgiHooks`.
4. Purge hardcoded paths in `src/proxy/osd_overlay.cpp` and `src/proxy/d3d11_to_d3d12_bridge.cpp`.

---

## 5. Verification Method

Worker 2 can independently verify this remediation using:
1. **Compilation**:
   ```cmd
   cmd.exe /c "build.bat"
   ```
   *Expected*: Exit code 0, zero warnings.
2. **Challenger Stress Test**:
   ```cmd
   build\Release\test_challenger_stress.exe
   ```
   *Expected*: All 19 tests pass (exit code 0), including Step 4 (20 live Present/Resize cycles without `0xC0000005`).
3. **Hardening Verification Suite**:
   ```cmd
   build\Release\test_proxy_hardening.exe
   ```
   *Expected*: All 29 tests pass (exit code 0).
4. **Hardware Benchmarks on RTX 3080**:
   ```cmd
   build\Release\nvp_live_test.exe
   build\Release\nvp_perf_bench.exe
   build\Release\vfi_selftest.exe
   build\Release\test_d3d11_bridge_nvp.exe
   ```
   *Expected*: All exit with code 0.
5. **Zero Hardcoded Paths Check**:
   ```cmd
   git grep "sm86_debug.log" src/
   git grep "sm86_present.csv" src/
   ```
   *Expected*: Zero hits.
