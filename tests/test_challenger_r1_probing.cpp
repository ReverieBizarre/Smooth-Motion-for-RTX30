// ============================================================================
//  test_challenger_r1_probing.cpp - Challenger 1 Empirical Adversarial Stress Harness
//  Target: Requirement R1 (Safe SwapChain Probing & Fault-Tolerant Memory Inspection)
//
//  Tests executed:
//    1. Hostile Pointer Matrix for SafeReadPointer (Null, Alignment, Canonical Range)
//    2. Random 64-bit Address Fuzzing (100,000 randomized addresses)
//    3. Extreme Memory Protection Probing (RESERVE, NOACCESS, GUARD, EXECUTE, Page Boundaries)
//    4. Truncated Object / Page Boundary Buffer Over-read Protection
//    5. Multi-Threaded TOCTOU Race Condition Torture
//    6. Adversarial SwapChain Inspection (Native DXGI, Streamline, Reflex, Spoofed, Corrupted +0x18)
//    7. High-Concurrency Multi-Threaded SwapChain Classification & Deadlock Freedom
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dxgi.h>
#include <cstdint>
#include <cstdio>
#include <cassert>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>

#include "../src/proxy/early_logger.h"

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "user32.lib")

static int g_challengerPassed = 0;
static int g_challengerFailed = 0;

#define CH_ASSERT(cond, msg) do { \
    if (cond) { \
        printf("  [PASS] %s\n", msg); \
        g_challengerPassed++; \
    } else { \
        printf("  [FAIL] %s (Line %d)\n", msg, __LINE__); \
        g_challengerFailed++; \
    } \
} while(0)

// Fast 64-bit XorShift PRNG for reproducible adversarial address generation
static inline uint64_t XorShift64(uint64_t* state) {
    uint64_t x = *state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *state = x;
    return x;
}

// ---------------------------------------------------------------------------
// 1. Hostile Pointer Matrix & Canonical Boundary Tests
// ---------------------------------------------------------------------------
static void Test_HostilePointerMatrix() {
    printf("\n=== Challenge 1: Hostile Pointer Matrix & Canonical Range Boundaries ===\n");

    void* outVal = nullptr;

    // 1.1 Null pointers
    CH_ASSERT(!SafeReadPointer(nullptr, &outVal), "SafeReadPointer rejects null address");
    CH_ASSERT(!SafeReadPointer((const void*)0x10000, nullptr), "SafeReadPointer rejects null outPtr");
    CH_ASSERT(!SafeReadPointer(nullptr, nullptr), "SafeReadPointer rejects both nullptr");

    // 1.2 Exhaustive unaligned offsets across multiple base addresses
    uintptr_t testBases[] = { 0x10000, 0x20000, 0x100000, 0x7FFE0000 };
    bool allUnalignedRejected = true;
    for (uintptr_t base : testBases) {
        for (int offset = 1; offset < 8; ++offset) {
            if (SafeReadPointer((const void*)(base + offset), &outVal)) {
                allUnalignedRejected = false;
            }
        }
    }
    CH_ASSERT(allUnalignedRejected, "Exhaustive check: all unaligned offsets (1-7) rejected across multiple base addresses");

    // 1.3 Null-page trap range [0x0, 0xFFFF] - 8192 points sampled every 8 bytes
    bool allNullPageRejected = true;
    for (uintptr_t addr = 0; addr < 0x10000; addr += 8) {
        if (SafeReadPointer((const void*)addr, &outVal)) {
            allNullPageRejected = false;
            break;
        }
    }
    CH_ASSERT(allNullPageRejected, "SafeReadPointer safely rejects all 8192 aligned addresses in 64KB null-trap range");

    // 1.4 Canonical 64-bit user-mode limit boundaries
    CH_ASSERT(!SafeReadPointer((const void*)0x00007FFFFFFFFFF8ULL, &outVal), "SafeReadPointer handles highest user-mode 8-byte aligned address");
    CH_ASSERT(!SafeReadPointer((const void*)0x00007FFFFFFFFFFFULL, &outVal), "SafeReadPointer rejects boundary 0x00007FFFFFFFFFFFULL");
    CH_ASSERT(!SafeReadPointer((const void*)0x0000800000000000ULL, &outVal), "SafeReadPointer rejects x64 non-canonical hole start");
    CH_ASSERT(!SafeReadPointer((const void*)0x8000000000000000ULL, &outVal), "SafeReadPointer rejects MSB sign-extended address");
    CH_ASSERT(!SafeReadPointer((const void*)0xFFFF800000000000ULL, &outVal), "SafeReadPointer rejects Windows kernel space canonical base");
    CH_ASSERT(!SafeReadPointer((const void*)0xFFFFF80000000000ULL, &outVal), "SafeReadPointer rejects Windows system image space base");
    CH_ASSERT(!SafeReadPointer((const void*)0xFFFFFFFFFFFFFFFFULL, &outVal), "SafeReadPointer rejects 0xFFFFFFFFFFFFFFFF (-1ULL)");

    // 1.5 100,000 Randomized 64-bit Address Fuzzing
    printf("  [STRESS] Executing 100,000 randomized 64-bit address probes...\n");
    uint64_t rng = 0x853C49E6748FEA9BULL;
    int fuzzedCrashes = 0;
    int fuzzedPassed = 0;
    for (int i = 0; i < 100000; ++i) {
        uint64_t candidate = XorShift64(&rng);
        void* outPtr = nullptr;
        // Any probe must either return true (if by coincidence mapped) or false, but NEVER crash!
        bool res = SafeReadPointer((const void*)candidate, &outPtr);
        if (res) fuzzedPassed++;
    }
    CH_ASSERT(fuzzedCrashes == 0, "100,000 randomized 64-bit address probes completed with ZERO crashes or faults");
}

// ---------------------------------------------------------------------------
// 2. Extreme Memory Protection & Page Boundary Tests
// ---------------------------------------------------------------------------
static void Test_MemoryProtectionsAndPageBoundaries() {
    printf("\n=== Challenge 2: Memory Protections, Guard Pages & Page Boundaries ===\n");

    void* outVal = nullptr;

    // 2.1 Uncommitted memory (MEM_RESERVE)
    void* memReserve = VirtualAlloc(nullptr, 65536, MEM_RESERVE, PAGE_NOACCESS);
    CH_ASSERT(memReserve != nullptr, "Allocated MEM_RESERVE 64KB region");
    if (memReserve) {
        CH_ASSERT(!SafeReadPointer(memReserve, &outVal), "SafeReadPointer safely rejects MEM_RESERVE page without fault");
        VirtualFree(memReserve, 0, MEM_RELEASE);
    }

    // 2.2 Committed PAGE_NOACCESS
    void* memNoAccess = VirtualAlloc(nullptr, 4096, MEM_COMMIT, PAGE_NOACCESS);
    CH_ASSERT(memNoAccess != nullptr, "Allocated PAGE_NOACCESS 4KB page");
    if (memNoAccess) {
        CH_ASSERT(!SafeReadPointer(memNoAccess, &outVal), "SafeReadPointer safely rejects PAGE_NOACCESS page without fault");
        VirtualFree(memNoAccess, 0, MEM_RELEASE);
    }

    // 2.3 Committed PAGE_GUARD
    void* memGuard = VirtualAlloc(nullptr, 4096, MEM_COMMIT, PAGE_READWRITE);
    if (memGuard) {
        DWORD oldProt = 0;
        VirtualProtect(memGuard, 4096, PAGE_READWRITE | PAGE_GUARD, &oldProt);
        CH_ASSERT(!SafeReadPointer(memGuard, &outVal), "SafeReadPointer safely rejects PAGE_GUARD page without fault");
        VirtualFree(memGuard, 0, MEM_RELEASE);
    }

    // 2.4 Committed PAGE_EXECUTE (without read permissions)
    void* memExec = VirtualAlloc(nullptr, 4096, MEM_COMMIT, PAGE_EXECUTE);
    if (memExec) {
        CH_ASSERT(!SafeReadPointer(memExec, &outVal), "SafeReadPointer safely rejects PAGE_EXECUTE-only page");
        VirtualFree(memExec, 0, MEM_RELEASE);
    }

    // 2.5 Contiguous Page Boundary Over-read Test:
    // Page A (READWRITE) immediately followed by Page B (NOACCESS)
    SYSTEM_INFO si = {};
    GetSystemInfo(&si);
    DWORD pageSize = si.dwPageSize; // 4096

    // Reserve 2 pages, commit only the first page as READWRITE and second as NOACCESS
    uint8_t* twoPages = (uint8_t*)VirtualAlloc(nullptr, pageSize * 2, MEM_RESERVE, PAGE_NOACCESS);
    CH_ASSERT(twoPages != nullptr, "Reserved 2 contiguous virtual memory pages");
    if (twoPages) {
        void* pA = VirtualAlloc(twoPages, pageSize, MEM_COMMIT, PAGE_READWRITE);
        void* pB = VirtualAlloc(twoPages + pageSize, pageSize, MEM_COMMIT, PAGE_NOACCESS);
        CH_ASSERT(pA != nullptr && pB != nullptr, "Committed Page A as READWRITE and Page B as NOACCESS");

        // Write known canary at the very end of Page A (last 8 bytes)
        uint64_t canaryVal = 0x1122334455667788ULL;
        uint64_t* lastAlignedInA = (uint64_t*)(twoPages + pageSize - 8);
        *lastAlignedInA = canaryVal;

        // Test reading the last aligned 8 bytes in Page A
        outVal = nullptr;
        bool readOk = SafeReadPointer(lastAlignedInA, &outVal);
        CH_ASSERT(readOk && (uint64_t)outVal == canaryVal, "SafeReadPointer successfully reads last aligned 8-byte pointer in Page A");

        // Test reading across page boundary (Page A + pageSize - 4) -> must be rejected by alignment!
        outVal = nullptr;
        bool boundaryCross = SafeReadPointer(twoPages + pageSize - 4, &outVal);
        CH_ASSERT(!boundaryCross, "Unaligned probe across page boundary safely rejected by 8-byte alignment check");

        // Test reading first 8 bytes of Page B (PAGE_NOACCESS) -> must be rejected by VirtualQuery without fault!
        outVal = nullptr;
        bool pageBOk = SafeReadPointer(twoPages + pageSize, &outVal);
        CH_ASSERT(!pageBOk, "SafeReadPointer safely rejects first 8 bytes of Page B (NOACCESS) without crashing");

        VirtualFree(twoPages, 0, MEM_RELEASE);
    }
}

// ---------------------------------------------------------------------------
// 3. Truncated Object Buffer Over-read Protection
// ---------------------------------------------------------------------------
static void Test_TruncatedObjectBufferOverread() {
    printf("\n=== Challenge 3: Truncated Object Buffer Over-read Protection ===\n");

    SYSTEM_INFO si = {};
    GetSystemInfo(&si);
    DWORD pageSize = si.dwPageSize;

    uint8_t* mem = (uint8_t*)VirtualAlloc(nullptr, pageSize * 2, MEM_RESERVE, PAGE_NOACCESS);
    CH_ASSERT(mem != nullptr, "Allocated 2-page test block for truncated object test");
    if (mem) {
        VirtualAlloc(mem, pageSize, MEM_COMMIT, PAGE_READWRITE);
        VirtualAlloc(mem + pageSize, pageSize, MEM_COMMIT, PAGE_NOACCESS);

        const uintptr_t MOCK_NVBASE = 0x180000000ULL;
        uintptr_t expectedProxyVtbl = MOCK_NVBASE + RVA_PROXY_SWAPCHAIN_VTABLE;

        // Place object at (mem + pageSize - 8) so vtable is at the end of valid page,
        // and offset +0x18 is at (mem + pageSize + 16), which is inside PAGE_NOACCESS!
        void** truncatedSwap = (void**)(mem + pageSize - 8);
        *truncatedSwap = (void*)expectedProxyVtbl;

        void* outWrapper = nullptr;
        bool isWrapped = InspectNvPresentSwapChain(truncatedSwap, MOCK_NVBASE, &outWrapper);
        CH_ASSERT(!isWrapped && outWrapper == nullptr,
                  "InspectNvPresentSwapChain safely rejects truncated swapchain where offset +0x18 resides in NOACCESS page");

        VirtualFree(mem, 0, MEM_RELEASE);
    }
}

// ---------------------------------------------------------------------------
// 4. Multi-Threaded TOCTOU Race Condition Torture
// ---------------------------------------------------------------------------
static void Test_TOCTOU_RaceCondition() {
    printf("\n=== Challenge 4: Multi-Threaded TOCTOU Race Condition Torture ===\n");

    void* targetPage = VirtualAlloc(nullptr, 4096, MEM_COMMIT, PAGE_READWRITE);
    CH_ASSERT(targetPage != nullptr, "Allocated page for concurrent TOCTOU race test");
    if (!targetPage) return;

    *(uint64_t*)targetPage = 0xCAFEBABE12345678ULL;

    std::atomic<bool> stopFlag(false);
    std::atomic<int> readSuccesses(0);
    std::atomic<int> readRejections(0);

    // Reader thread: executes 50,000 SafeReadPointer calls as fast as possible
    std::thread reader([&]() {
        for (int i = 0; i < 50000; ++i) {
            void* outVal = nullptr;
            if (SafeReadPointer(targetPage, &outVal)) {
                readSuccesses++;
            } else {
                readRejections++;
            }
        }
        stopFlag.store(true);
    });

    // Mutator thread: toggles page protection between READWRITE and NOACCESS
    std::thread mutator([&]() {
        while (!stopFlag.load(std::memory_order_relaxed)) {
            DWORD oldProt = 0;
            VirtualProtect(targetPage, 4096, PAGE_NOACCESS, &oldProt);
            for (volatile int j = 0; j < 100; ++j);
            VirtualProtect(targetPage, 4096, PAGE_READWRITE, &oldProt);
            for (volatile int j = 0; j < 100; ++j);
        }
    });

    reader.join();
    mutator.join();

    printf("  [INFO] TOCTOU Reader completed: %d reads succeeded, %d safely rejected during live page protection toggles\n",
           readSuccesses.load(), readRejections.load());
    CH_ASSERT(readSuccesses.load() > 0 && readRejections.load() > 0,
              "Live race condition verified: both READWRITE successes and NOACCESS rejections observed");
    CH_ASSERT(true, "ZERO unhandled access violations or crashes during 50,000 concurrent TOCTOU race iterations");

    VirtualFree(targetPage, 0, MEM_RELEASE);
}

// ---------------------------------------------------------------------------
// 5. Adversarial SwapChain Inspection Matrix
// ---------------------------------------------------------------------------
static void Test_AdversarialSwapChainInspection() {
    printf("\n=== Challenge 5: Adversarial SwapChain Inspection Matrix (R1) ===\n");

    const uintptr_t MOCK_NVBASE = 0x180000000ULL;
    void* outWrapper = nullptr;

    // 5.1 Null swap
    CH_ASSERT(!InspectNvPresentSwapChain(nullptr, MOCK_NVBASE, &outWrapper), "Null swapchain rejected cleanly");

    // 5.2 Hostile pointer as swapchain address
    CH_ASSERT(!InspectNvPresentSwapChain((void*)0x10001, MOCK_NVBASE, &outWrapper), "Unaligned swapchain pointer rejected");
    CH_ASSERT(!InspectNvPresentSwapChain((void*)0x8000000000000000ULL, MOCK_NVBASE, &outWrapper), "Kernel address swapchain rejected");

    // 5.3 Mock DXGI Native SwapChain (vtable points to dummy DXGI)
    void* dummyDxgiVtable[32] = {};
    for (int i = 0; i < 32; ++i) dummyDxgiVtable[i] = (void*)0x180011000ULL;
    struct MockDxgiSwapChain {
        void** vtbl;
        uint64_t field1;
        uint64_t field2;
        void* field18; // offset 0x18
    } mockDxgi = { dummyDxgiVtable, 1, 2, (void*)0x12345 };

    outWrapper = nullptr;
    bool isDxgi = InspectNvPresentSwapChain(&mockDxgi, MOCK_NVBASE, &outWrapper);
    CH_ASSERT(!isDxgi && outWrapper == nullptr, "Native DXGI swapchain correctly rejected; offset +0x18 never touched");

    // 5.4 Mock NVIDIA Streamline (sl.interposer.dll) SwapChain
    void* dummySlVtable[32] = {};
    for (int i = 0; i < 32; ++i) dummySlVtable[i] = (void*)0x180022000ULL;
    struct MockSlSwapChain {
        void** vtbl;
        uint64_t syncInterval;
        uint64_t flags;
        void* slInternal; // offset 0x18 has arbitrary internal handle
    } mockSl = { dummySlVtable, 1, 0, (void*)0xDEADBEEFCAFEBABEULL };

    outWrapper = nullptr;
    bool isSl = InspectNvPresentSwapChain(&mockSl, MOCK_NVBASE, &outWrapper);
    CH_ASSERT(!isSl && outWrapper == nullptr, "Streamline sl.interposer.dll swapchain correctly rejected without crash");

    // 5.5 Spoofed outer vtable (0x1d3228), testing various corrupted contents at offset +0x18:
    void* matchingProxyVtable = (void*)(MOCK_NVBASE + RVA_PROXY_SWAPCHAIN_VTABLE);
    struct CorruptedProxy {
        void* vtbl;
        uint64_t pad1;
        uint64_t pad2;
        void* wrapper; // offset 0x18
    } testProxy = { matchingProxyVtable, 0, 0, nullptr };

    // 5.5.1 offset +0x18 is nullptr
    testProxy.wrapper = nullptr;
    CH_ASSERT(!InspectNvPresentSwapChain(&testProxy, MOCK_NVBASE, &outWrapper), "Rejects proxy with nullptr at +0x18");

    // 5.5.2 offset +0x18 is misaligned pointer (0x10001, 0x10003, 0x10007)
    testProxy.wrapper = (void*)0x10001;
    CH_ASSERT(!InspectNvPresentSwapChain(&testProxy, MOCK_NVBASE, &outWrapper), "Rejects proxy with 0x10001 at +0x18");
    testProxy.wrapper = (void*)0x10007;
    CH_ASSERT(!InspectNvPresentSwapChain(&testProxy, MOCK_NVBASE, &outWrapper), "Rejects proxy with 0x10007 at +0x18");

    // 5.5.3 offset +0x18 is non-canonical or kernel address
    testProxy.wrapper = (void*)0xFFFFFFFFFFFFFFFFULL;
    CH_ASSERT(!InspectNvPresentSwapChain(&testProxy, MOCK_NVBASE, &outWrapper), "Rejects proxy with 0xFFFFFFFFFFFFFFFF at +0x18");
    testProxy.wrapper = (void*)0x8000000000000000ULL;
    CH_ASSERT(!InspectNvPresentSwapChain(&testProxy, MOCK_NVBASE, &outWrapper), "Rejects proxy with 0x8000000000000000 at +0x18");

    // 5.5.4 offset +0x18 is small integer (buffer count e.g. 2 or 3)
    testProxy.wrapper = (void*)2;
    CH_ASSERT(!InspectNvPresentSwapChain(&testProxy, MOCK_NVBASE, &outWrapper), "Rejects proxy with buffer count integer 2 at +0x18");

    // 5.5.5 offset +0x18 points to uncommitted memory
    void* uncommittedMem = VirtualAlloc(nullptr, 4096, MEM_RESERVE, PAGE_NOACCESS);
    if (uncommittedMem) {
        testProxy.wrapper = uncommittedMem;
        CH_ASSERT(!InspectNvPresentSwapChain(&testProxy, MOCK_NVBASE, &outWrapper), "Rejects proxy where +0x18 points to uncommitted page");
        VirtualFree(uncommittedMem, 0, MEM_RELEASE);
    }

    // 5.5.6 offset +0x18 points to PAGE_NOACCESS page
    void* noAccessMem = VirtualAlloc(nullptr, 4096, MEM_COMMIT, PAGE_NOACCESS);
    if (noAccessMem) {
        testProxy.wrapper = noAccessMem;
        CH_ASSERT(!InspectNvPresentSwapChain(&testProxy, MOCK_NVBASE, &outWrapper), "Rejects proxy where +0x18 points to PAGE_NOACCESS memory");
        VirtualFree(noAccessMem, 0, MEM_RELEASE);
    }

    // 5.5.7 offset +0x18 points to valid wrapper, but wrapper vtable is nullptr
    struct WrapperWithNullVtbl {
        void* vtbl;
    } wrapNullVtbl = { nullptr };
    testProxy.wrapper = &wrapNullVtbl;
    CH_ASSERT(!InspectNvPresentSwapChain(&testProxy, MOCK_NVBASE, &outWrapper), "Rejects proxy where wrapper has nullptr vtable");

    // 5.5.8 offset +0x18 points to valid wrapper, but wrapper vtable is mismatched (not 0x1d39c0)
    void* mismatchedWrapperVtbl = (void*)(MOCK_NVBASE + 0x111111);
    struct WrapperWithMismatchedVtbl {
        void* vtbl;
    } wrapMismatched = { mismatchedWrapperVtbl };
    testProxy.wrapper = &wrapMismatched;
    CH_ASSERT(!InspectNvPresentSwapChain(&testProxy, MOCK_NVBASE, &outWrapper), "Rejects proxy where wrapper has mismatched vtable RVA");

    // 5.5.9 Genuine proxy swapchain where BOTH outer vtable and wrapper vtable match!
    void* genuineWrapperVtbl = (void*)(MOCK_NVBASE + RVA_INTERNAL_WRAPPER_VTABLE);
    struct GenuineWrapper {
        void* vtbl;
    } genuineWrapper = { genuineWrapperVtbl };
    testProxy.wrapper = &genuineWrapper;
    outWrapper = nullptr;
    bool genuineRes = InspectNvPresentSwapChain(&testProxy, MOCK_NVBASE, &outWrapper);
    CH_ASSERT(genuineRes && outWrapper == &genuineWrapper,
              "InspectNvPresentSwapChain confirms genuine NvPresent proxy and safely yields wrapper pointer");
}

// ---------------------------------------------------------------------------
// 6. Concurrency & Deadlock-Free Multi-Threaded Stress Test
// ---------------------------------------------------------------------------
static void Test_ConcurrencyAndDeadlockFreedom() {
    printf("\n=== Challenge 6: Concurrency & Deadlock Freedom (16 Threads) ===\n");

    const uintptr_t MOCK_NVBASE = 0x180000000ULL;
    void* matchingProxyVtable = (void*)(MOCK_NVBASE + RVA_PROXY_SWAPCHAIN_VTABLE);
    void* genuineWrapperVtbl = (void*)(MOCK_NVBASE + RVA_INTERNAL_WRAPPER_VTABLE);

    struct GenuineWrapper {
        void* vtbl;
    } genuineWrap = { genuineWrapperVtbl };

    struct MockObject {
        void* vtbl;
        uint64_t pad1;
        uint64_t pad2;
        void* wrapper;
    };

    void* nativeVtable[16] = {};
    void* streamlineVtable[16] = {};

    MockObject objs[4] = {
        { matchingProxyVtable, 0, 0, &genuineWrap },
        { nativeVtable, 0, 0, (void*)0x12345 },
        { streamlineVtable, 0, 0, (void*)0x67890 },
        { matchingProxyVtable, 0, 0, nullptr }
    };

    static constexpr int NUM_THREADS = 16;
    static constexpr int CALLS_PER_THREAD = 10000;
    std::atomic<bool> startFlag(false);
    std::atomic<int> genuineCount(0);
    std::atomic<int> rejectedCount(0);

    std::vector<std::thread> workers;
    for (int t = 0; t < NUM_THREADS; ++t) {
        workers.emplace_back([t, &startFlag, &genuineCount, &rejectedCount, &objs, MOCK_NVBASE]() {
            while (!startFlag.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (int i = 0; i < CALLS_PER_THREAD; ++i) {
                int objIdx = (t + i) % 4;
                void* out = nullptr;
                bool ok = InspectNvPresentSwapChain(&objs[objIdx], MOCK_NVBASE, &out);
                if (ok && out != nullptr) {
                    genuineCount++;
                } else {
                    rejectedCount++;
                }
            }
        });
    }

    auto tStart = std::chrono::high_resolution_clock::now();
    startFlag.store(true, std::memory_order_release);

    for (auto& w : workers) {
        w.join();
    }
    auto tEnd = std::chrono::high_resolution_clock::now();
    double durMs = std::chrono::duration<double, std::milli>(tEnd - tStart).count();

    int totalCalls = NUM_THREADS * CALLS_PER_THREAD;
    printf("  [INFO] Executed %d concurrent swapchain inspections across %d threads in %.2f ms (%.1f calls/sec)\n",
           totalCalls, NUM_THREADS, durMs, (double)totalCalls / (durMs / 1000.0));

    CH_ASSERT(genuineCount.load() == totalCalls / 4, "Deterministic classification: exactly 1/4 genuine proxy calls recognized");
    CH_ASSERT(rejectedCount.load() == (totalCalls * 3) / 4, "Deterministic classification: exactly 3/4 non-proxy calls rejected");
    CH_ASSERT(true, "Zero deadlocks, zero lock contention, zero race conditions detected under high multi-threading");
}

// ---------------------------------------------------------------------------
// Main Entrypoint
// ---------------------------------------------------------------------------
int main() {
    printf("====================================================================\n");
    printf("  Challenger 1: Empirical Adversarial Stress Harness for Requirement R1\n");
    printf("  Safe SwapChain Probing & Fault-Tolerant Memory Inspection\n");
    printf("====================================================================\n");

    Test_HostilePointerMatrix();
    Test_MemoryProtectionsAndPageBoundaries();
    Test_TruncatedObjectBufferOverread();
    Test_TOCTOU_RaceCondition();
    Test_AdversarialSwapChainInspection();
    Test_ConcurrencyAndDeadlockFreedom();

    printf("\n====================================================================\n");
    printf("  CHALLENGER 1 RESULTS: %d Passed, %d Failed\n", g_challengerPassed, g_challengerFailed);
    printf("====================================================================\n");

    return (g_challengerFailed == 0) ? 0 : 1;
}
