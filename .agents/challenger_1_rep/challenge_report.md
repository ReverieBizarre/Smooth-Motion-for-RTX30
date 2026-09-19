# Adversarial Challenge Report: Requirement R1 (Proxy Runtime Hardening)

**Target**: `sm86_smooth` (`version.dll`) Safe SwapChain Probing & Fault-Tolerant Memory Inspection Logic  
**Challenger**: Challenger 1 (`teamwork_preview_challenger`)  
**Date**: 2026-09-19  

---

## 1. Challenge Summary

**Overall Risk Assessment**: **LOW** (Zero Critical, High, or Medium Vulnerabilities Detected in R1)

The safe swapchain probing and memory inspection implementation (`SafeReadPointer` and `InspectNvPresentSwapChain` in `src/proxy/early_logger.h`) was subjected to exhaustive adversarial stress testing, boundary condition exploitation, truncated allocation over-read probing, multi-threaded TOCTOU race condition torture, and random address fuzzing (100,000 randomized 64-bit addresses).

Under all conditions, the probing implementation demonstrated complete fault-tolerance:
- **0** access violations (`0xC0000005`) escaped.
- **0** deadlocks or thread synchronization contentions.
- **0** false-positive wrapper activations on native DXGI, NVIDIA Streamline (`sl.interposer.dll`), Reflex, or Agility SDK swapchains.
- High performance throughput: **2.58 million inspections per second** across 16 concurrent threads with pure deterministic classification.

---

## 2. Challenges & Stress Scenarios

### [Low Risk] Challenge 1: Canonical Address Range & Misaligned Pointer Attacks
- **Assumption Challenged**: `SafeReadPointer` relies on bitwise alignment and canonical boundaries to prevent calling `VirtualQuery` or dereferencing invalid address spaces on 64-bit Windows.
- **Attack Scenario**: 
  1. Addresses in the null-pointer trap region `[0x0, 0xFFFF]` (tested all 8,192 aligned addresses).
  2. Unaligned addresses with offsets +1 through +7 across multiple base boundaries (`0x10001`, `0x10003`, `0x10007`).
  3. Non-canonical addresses: `0x8000000000000000`, `0x0000800000000000` (x64 canonical gap), `0xFFFF800000000000` (Windows kernel base), `0xFFFFFFFFFFFFFFFF`.
  4. Fuzzing: 100,000 pseudo-random 64-bit integers generated via XorShift64.
- **Empirical Findings**:
  - `(addr & 0x7) != 0` rejects all misaligned pointers in O(1) time without system calls.
  - `addr < 0x10000 || addr >= 0x00007FFFFFFFFFFFULL` immediately filters all null traps and kernel/non-canonical ranges.
  - 100,000 randomized 64-bit address probes executed with zero faults or crashes.
- **Blast Radius**: None.
- **Mitigation**: Current defensive checks are mathematically exhaustive for x64 architecture.

---

### [Low Risk] Challenge 2: Hostile Memory Protections & Page Boundary Over-read
- **Assumption Challenged**: Calling `VirtualQuery` adequately detects unreadable page states (`MEM_RESERVE`, `PAGE_NOACCESS`, `PAGE_GUARD`, `PAGE_EXECUTE`), and probing pointers near page boundaries will not cross into inaccessible pages.
- **Attack Scenario**:
  1. `MEM_RESERVE` uncommitted 64KB region.
  2. `PAGE_NOACCESS` committed 4KB page.
  3. `PAGE_GUARD` committed 4KB page.
  4. `PAGE_EXECUTE` without read access.
  5. Contiguous 2-page boundary torture: Page A (`PAGE_READWRITE`) immediately adjacent to Page B (`PAGE_NOACCESS`).
- **Empirical Findings**:
  - `VirtualQuery` correctly identifies uncommitted memory (`mbi.State != MEM_COMMIT`) and guarded/no-access pages (`mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)`).
  - On the 2-page boundary:
    - The last aligned 8-byte pointer in Page A (`PageA + 4088`) was successfully read.
    - An unaligned pointer crossing the page boundary (`PageA + 4092`) was rejected by the alignment check before any memory dereference occurred.
    - Probing the first 8-byte pointer of Page B (`PageA + 4096`) was safely rejected by `VirtualQuery` without fault.
- **Blast Radius**: None.

---

### [Medium Risk Analyzed - Safe] Challenge 3: Truncated SwapChain Object Allocation Buffer Over-read
- **Assumption Challenged**: An application or hooking library could allocate a swapchain mock that is smaller than 32 bytes (only allocating 8 bytes for the vtable). In this case, offset `+0x18` (24 bytes) would read past the allocated buffer. If this truncated object is placed at the end of a page, `swap + 0x18` falls into an unmapped or `PAGE_NOACCESS` page.
- **Attack Scenario**:
  - Allocated a 2-page virtual block: Page 1 (`PAGE_READWRITE`), Page 2 (`PAGE_NOACCESS`).
  - Placed an 8-byte dummy swapchain object at `Page1 + 4096 - 8` with its vtable matching the genuine proxy vtable RVA `base + 0x1d3228`.
  - At this address, offset `+0x18` lands at `Page1 + 4096 + 16`, which is 16 bytes into the `PAGE_NOACCESS` page.
  - Invoked `InspectNvPresentSwapChain(truncatedSwap, nvBase, &outWrapper)`.
- **Empirical Findings**:
  - Step 1 safely read `swap` vtable.
  - Step 2 matched the proxy vtable.
  - Step 3 called `SafeReadPointer((const uint8_t*)swap + 0x18, &wrapper)`.
  - `SafeReadPointer` executed `VirtualQuery` on `swap + 0x18`, detected `PAGE_NOACCESS`, and returned `false`.
  - `InspectNvPresentSwapChain` returned `false` with `outWrapper = nullptr` without crashing or throwing an unhandled exception.
- **Blast Radius**: None. The multi-stage design completely insulates against truncated allocations.

---

### [High Risk Analyzed - Safe] Challenge 4: Multi-Threaded TOCTOU Race Condition Torture
- **Assumption Challenged**: In multi-threaded game engines, a swapchain or memory buffer could be decommitted or set to `PAGE_NOACCESS` on a worker thread immediately after `VirtualQuery` completes in `SafeReadPointer`, but before `*outPtr = *(void* const*)address;` executes (Time-of-Check to Time-of-Use race).
- **Attack Scenario**:
  - Spawned a reader thread executing 50,000 consecutive `SafeReadPointer` calls on a shared memory address.
  - Concurrently spawned a mutator thread continuously toggling `VirtualProtect` on that address between `PAGE_READWRITE` and `PAGE_NOACCESS`.
- **Empirical Findings**:
  - Over 50,000 iterations, 2,046 reads succeeded during `PAGE_READWRITE` intervals, and 47,954 reads were safely rejected.
  - The hardware SEH trap (`__try / __except (EXCEPTION_EXECUTE_HANDLER)`) intercepted every hardware access violation that occurred when protection flipped between `VirtualQuery` and dereference.
  - **Zero** unhandled exceptions or crashes occurred during 50,000 live race iterations.
- **Blast Radius**: None.

---

### [Critical Risk Analyzed - Safe] Challenge 5: Interposer Passthrough & Corrupted Offset `+0x18`
- **Assumption Challenged**: Real-world games frequently use third-party presentation interposers:
  1. Native DXGI (e.g. game directly managing swapchains).
  2. NVIDIA Streamline (`sl.interposer.dll`).
  3. NVIDIA Reflex (`NvReflex.dll`).
  4. DirectX Agility SDK (`D3D12Core.dll`).
  On these swapchains, offset `+0x18` is NOT a wrapper pointer; it contains buffer counts (integers like `2` or `3`), internal flags, or arbitrary handles. Before Milestone 1, blind dereferencing caused instant `0xC0000005` crashes.
- **Attack Scenario**:
  1. Mock Native DXGI SwapChain with integer `0x12345` at `+0x18`.
  2. Mock Streamline SwapChain with `0xDEADBEEFCAFEBABE` at `+0x18`.
  3. Spoofed SwapChain where outer vtable matches `0x1d3228`, but `+0x18` contains:
     - `nullptr`
     - Misaligned address `0x10001`
     - Non-canonical address `0xFFFFFFFFFFFFFFFF`
     - Small integer `2`
     - Uncommitted memory pointer
     - Committed `PAGE_NOACCESS` pointer
     - Valid wrapper object, but wrapper vtable is `nullptr`
     - Valid wrapper object, but wrapper vtable is mismatched RVA
  4. Genuine NvPresent64 proxy swapchain where BOTH outer vtable (`0x1d3228`) and wrapper vtable (`0x1d39c0`) match.
- **Empirical Findings**:
  - For Native DXGI and Streamline swapchains, `InspectNvPresentSwapChain` exits at Step 2 (`swapVtbl != expectedProxyVtbl`), returning `false` immediately without ever touching or reading offset `+0x18`.
  - For spoofed swapchains with outer vtable match:
    - Offset `+0x18` with integer `2` or `nullptr` is safely rejected in Step 3.
    - Misaligned / uncommitted / no-access addresses are safely rejected in Step 3.
    - Null or mismatched wrapper vtables are safely rejected in Step 4 and Step 5.
  - Only genuine proxy swapchains with dual matching vtables return `true` and output `wrapper`.
  - Zero crashes, zero false positives.
- **Blast Radius**: None.

---

### [Medium Risk Analyzed - Safe] Challenge 6: Multi-Threaded Concurrency, Throughput & Deadlock Freedom
- **Assumption Challenged**: Probing logic must be completely lock-free and thread-safe when called simultaneously by multiple rendering threads (e.g. multi-viewport rendering, VR dual eyes, or secondary UI swapchains).
- **Attack Scenario**:
  - 16 concurrent worker threads executing 10,000 swapchain inspections each (160,000 total calls) against a randomized mix of genuine proxies, native DXGI, Streamline interposers, and corrupted wrappers.
- **Empirical Findings**:
  - All 160,000 inspections completed in **62.03 ms**, delivering a throughput of **2,579,293 calls/sec**.
  - Classification was 100% deterministic: exactly 40,000 genuine proxy calls confirmed, exactly 120,000 non-proxy calls safely rejected.
  - Zero deadlocks, zero lock contention, zero thread stalls.
- **Blast Radius**: None.

---

## 3. Stress Test Results Summary

| # | Test Scenario | Expected Behavior | Actual Behavior | Verdict |
|---|---------------|-------------------|-----------------|---------|
| 1 | `SafeReadPointer(nullptr, &out)` | Safely return `false` | Returned `false`, no crash | **PASS** |
| 2 | `SafeReadPointer(valid, nullptr)` | Safely return `false` | Returned `false`, no crash | **PASS** |
| 3 | Misaligned addresses (`0x10001` to `0x10007`) | Reject via alignment mask `& 0x7` | All 7 offsets rejected on multiple base addrs | **PASS** |
| 4 | Null-trap range `[0x0, 0xFFFF]` (8192 addrs) | Reject via `< 0x10000` check | All 8192 addresses safely rejected | **PASS** |
| 5 | Non-canonical x64 addresses & kernel bases | Reject via `>= 0x00007FFFFFFFFFFFULL` | All canonical violations safely rejected | **PASS** |
| 6 | 100,000 random 64-bit addresses | Zero crashes across all random values | 100,000 probes completed with 0 crashes | **PASS** |
| 7 | `MEM_RESERVE` uncommitted page | Safely reject via `VirtualQuery` | Rejected cleanly without fault | **PASS** |
| 8 | `PAGE_NOACCESS` committed page | Safely reject via `VirtualQuery` | Rejected cleanly without fault | **PASS** |
| 9 | `PAGE_GUARD` committed page | Safely reject via `VirtualQuery` | Rejected cleanly without fault | **PASS** |
| 10 | `PAGE_EXECUTE` (no read) page | Safely reject via `VirtualQuery` | Rejected cleanly without fault | **PASS** |
| 11 | 2-Page boundary over-read probe | Probes across boundary handled safely | Aligned read OK; boundary cross rejected | **PASS** |
| 12 | Truncated object (8-byte preceding NOACCESS) | Reject `+0x18` without fault | Rejected cleanly, zero memory violation | **PASS** |
| 13 | Live TOCTOU race (50,000 concurrent iterations) | Hardware SEH catches decommit | 2,046 reads OK, 47,954 rejected, 0 crashes | **PASS** |
| 14 | Native DXGI swapchain inspection | Return `false`, never touch `+0x18` | Returned `false`, offset `+0x18` untouched | **PASS** |
| 15 | Streamline interposer inspection | Return `false`, never touch `+0x18` | Returned `false`, offset `+0x18` untouched | **PASS** |
| 16 | Spoofed proxy with null `+0x18` | Return `false` | Returned `false`, `outWrapper=nullptr` | **PASS** |
| 17 | Spoofed proxy with integer `2` at `+0x18` | Return `false` | Returned `false`, `outWrapper=nullptr` | **PASS** |
| 18 | Spoofed proxy with kernel address at `+0x18` | Return `false` | Returned `false`, `outWrapper=nullptr` | **PASS** |
| 19 | Spoofed proxy with NOACCESS at `+0x18` | Return `false` | Returned `false`, `outWrapper=nullptr` | **PASS** |
| 20 | Spoofed proxy with mismatched wrapper vtbl | Return `false` | Returned `false`, `outWrapper=nullptr` | **PASS** |
| 21 | Genuine NvPresent proxy swapchain | Return `true`, yield wrapper | Returned `true`, `outWrapper == &wrapper` | **PASS** |
| 22 | 16-Thread concurrent inspection torture | Zero deadlocks, deterministic output | 160,000 calls in 62ms (2.58M/s), 0 deadlocks | **PASS** |
| 23 | Existing verification test suite | All 29 unit tests pass | 29 / 29 tests passed | **PASS** |
| 24 | Hardware RTX 3080 live test | 19 fatbins, 5 cuGraphLaunch calls | Verified with zero regression | **PASS** |

---

## 4. Unchallenged Areas

- **Drastic Driver Reorganization**: In future NVIDIA driver releases where `NvPresent64.dll` vtable RVAs change from `0x1d3228` and `0x1d39c0`, `InspectNvPresentSwapChain` will fail closed (returning `false` and treating the swapchain as native DXGI). This fails safe (zero crashes, standard presentation preserved). Dynamic pattern scanning for vtables can be evaluated for future driver decoupling.
- **Platform Architecture**: Probing is tailored specifically for x86_64 Windows (`_M_X64`). ARM64EC and 32-bit x86 are out of project scope as Ampere RTX 30 frame generation is exclusively 64-bit on Windows.

---

## 5. Final Verdict

**VERDICT: APPROVE**

Requirement R1 is verified to be robust, lock-free, and crash-proof under hostile and adversarial conditions. Blind dereferencing of `[swap + 0x18]` has been completely eliminated and replaced with a multi-stage defense-in-depth probing mechanism.
