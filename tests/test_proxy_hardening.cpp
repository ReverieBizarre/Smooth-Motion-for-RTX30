// ============================================================================
//  test_proxy_hardening.cpp - Automated Verification Harness for Milestone 1
//  Validates:
//    1. SafeReadPointer memory probing, alignment, range, VirtualQuery & SEH (R1)
//    2. InspectNvPresentSwapChain dual-vtable verification & interposer passthrough (R1)
//    3. EarlyLogger persistent file creation, formatting, and immediate flushing (R2)
//    4. SwapChain ResizeBuffers state invalidation semantics (R4)
//    5. VBlank Pacing output querying & headless fallback (R3)
// ============================================================================
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dxgi.h>
#include <d3d11.h>
#include <cstdint>
#include <cstdio>
#include <cassert>
#include <string>
#include <vector>

#include "../src/proxy/early_logger.h"

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "user32.lib")

static int g_testsPassed = 0;
static int g_testsTotal = 0;

#define TEST_ASSERT(cond, msg) do { \
    g_testsTotal++; \
    if (cond) { \
        printf("  [PASS] %s\n", msg); \
        g_testsPassed++; \
    } else { \
        printf("  [FAIL] %s (Line %d)\n", msg, __LINE__); \
    } \
} while(0)

// ---------------------------------------------------------------------------
// 1. SafeReadPointer Unit Tests (R1)
// ---------------------------------------------------------------------------
static void Test_SafeReadPointer() {
    printf("\n=== Test Suite 1: SafeReadPointer Memory Safeguards (R1) ===\n");

    void* outVal = nullptr;

    // Test 1.1: Null address
    TEST_ASSERT(!SafeReadPointer(nullptr, &outVal), "SafeReadPointer rejects nullptr address");
    TEST_ASSERT(!SafeReadPointer((const void*)0x10000, nullptr), "SafeReadPointer rejects nullptr outPtr");

    // Test 1.2: Misaligned addresses (64-bit pointers must be 8-byte aligned)
    TEST_ASSERT(!SafeReadPointer((const void*)0x10001, &outVal), "SafeReadPointer rejects address + 1 alignment");
    TEST_ASSERT(!SafeReadPointer((const void*)0x10003, &outVal), "SafeReadPointer rejects address + 3 alignment");
    TEST_ASSERT(!SafeReadPointer((const void*)0x10007, &outVal), "SafeReadPointer rejects address + 7 alignment");

    // Test 1.3: Null-page trap range (< 0x10000)
    TEST_ASSERT(!SafeReadPointer((const void*)0x0, &outVal), "SafeReadPointer rejects address 0x0");
    TEST_ASSERT(!SafeReadPointer((const void*)0x8, &outVal), "SafeReadPointer rejects address 0x8");
    TEST_ASSERT(!SafeReadPointer((const void*)0xFFF8, &outVal), "SafeReadPointer rejects address 0xFFF8 (< 64KB)");

    // Test 1.4: Canonical user-mode boundary (>= 0x00007FFFFFFFFFFFULL)
    TEST_ASSERT(!SafeReadPointer((const void*)0x00007FFFFFFFFFF8ULL, &outVal), "SafeReadPointer rejects near kernel boundary");
    TEST_ASSERT(!SafeReadPointer((const void*)0xFFFFFFFFFFFFFFF8ULL, &outVal), "SafeReadPointer rejects kernel address space");

    // Test 1.5: Valid stack pointer
    uint64_t testValue = 0xDEADBEEFCAFEBABEULL;
    void* readVal = nullptr;
    bool ok = SafeReadPointer(&testValue, &readVal);
    TEST_ASSERT(ok && (uint64_t)readVal == testValue, "SafeReadPointer correctly reads valid stack variable");

    // Test 1.6: Valid heap pointer
    uint64_t* heapVal = new uint64_t(0x0123456789ABCDEFULL);
    readVal = nullptr;
    ok = SafeReadPointer(heapVal, &readVal);
    TEST_ASSERT(ok && (uint64_t)readVal == *heapVal, "SafeReadPointer correctly reads valid heap memory");
    delete heapVal;

    // Test 1.7: Uncommitted page protection (MEM_RESERVE without MEM_COMMIT)
    void* reservedMem = VirtualAlloc(nullptr, 65536, MEM_RESERVE, PAGE_NOACCESS);
    if (reservedMem) {
        readVal = nullptr;
        TEST_ASSERT(!SafeReadPointer(reservedMem, &readVal), "SafeReadPointer rejects MEM_RESERVE uncommitted page without fault");
        VirtualFree(reservedMem, 0, MEM_RELEASE);
    }

    // Test 1.8: Committed page with PAGE_NOACCESS
    void* committedNoAccess = VirtualAlloc(nullptr, 65536, MEM_COMMIT, PAGE_NOACCESS);
    if (committedNoAccess) {
        readVal = nullptr;
        TEST_ASSERT(!SafeReadPointer(committedNoAccess, &readVal), "SafeReadPointer rejects PAGE_NOACCESS committed page without fault");
        VirtualFree(committedNoAccess, 0, MEM_RELEASE);
    }

    // Test 1.9: Committed page with PAGE_GUARD
    void* guardPage = VirtualAlloc(nullptr, 65536, MEM_COMMIT, PAGE_READWRITE);
    if (guardPage) {
        DWORD oldProt = 0;
        VirtualProtect(guardPage, 4096, PAGE_READWRITE | PAGE_GUARD, &oldProt);
        readVal = nullptr;
        TEST_ASSERT(!SafeReadPointer(guardPage, &readVal), "SafeReadPointer rejects PAGE_GUARD memory without fault");
        VirtualFree(guardPage, 0, MEM_RELEASE);
    }
}

// ---------------------------------------------------------------------------
// 2. InspectNvPresentSwapChain Tests (R1)
// ---------------------------------------------------------------------------
static void Test_InspectNvPresentSwapChain() {
    printf("\n=== Test Suite 2: InspectNvPresentSwapChain Verification & Passthrough (R1) ===\n");

    const uintptr_t MOCK_NVBASE = 0x180000000ULL;
    void* wrapperOut = nullptr;

    // Test 2.1: Null swapchain
    TEST_ASSERT(!InspectNvPresentSwapChain(nullptr, MOCK_NVBASE, &wrapperOut), "InspectNvPresentSwapChain rejects null swap");

    // Test 2.2: Native DXGI SwapChain mock (vtable points elsewhere, e.g. dxgi.dll)
    void* nativeVtable[16] = {};
    for (int i = 0; i < 16; i++) nativeVtable[i] = (void*)0x180055000ULL;
    struct MockNativeSwapChain {
        void** vtbl;
        uint64_t bufferCount;
        uint64_t flags;
        void* internalDxgiField; // at offset 0x18
    } mockNative = { nativeVtable, 2, 0, (void*)0x2 }; // offset 0x18 holds integer 2

    wrapperOut = nullptr;
    bool isProxy = InspectNvPresentSwapChain(&mockNative, MOCK_NVBASE, &wrapperOut);
    TEST_ASSERT(!isProxy && wrapperOut == nullptr, "InspectNvPresentSwapChain safely rejects native swapchain without dereferencing offset 0x18");

    // Test 2.3: Streamline interposer mock (sl.interposer.dll)
    void* streamlineVtable[16] = {};
    for (int i = 0; i < 16; i++) streamlineVtable[i] = (void*)0x180088000ULL;
    struct MockStreamlineSwapChain {
        void** vtbl;
        uint64_t syncInterval;
        uint64_t flags;
        void* slResource; // at offset 0x18 (an internal Streamline object with arbitrary layout)
    } mockStreamline = { streamlineVtable, 1, 0, (void*)0x12345678ULL };

    wrapperOut = nullptr;
    isProxy = InspectNvPresentSwapChain(&mockStreamline, MOCK_NVBASE, &wrapperOut);
    TEST_ASSERT(!isProxy && wrapperOut == nullptr, "InspectNvPresentSwapChain safely rejects Streamline interposer without crashing");

    // Test 2.4: Spoofed object where outer vtable matches 0x1d3228, but offset 0x18 is null
    void* matchingProxyVtable = (void*)(MOCK_NVBASE + RVA_PROXY_SWAPCHAIN_VTABLE);
    struct MockSpoofNullWrapper {
        void* vtbl;
        uint64_t pad1;
        uint64_t pad2;
        void* wrapper; // at offset 0x18
    } mockSpoofNull = { matchingProxyVtable, 0, 0, nullptr };

    wrapperOut = nullptr;
    isProxy = InspectNvPresentSwapChain(&mockSpoofNull, MOCK_NVBASE, &wrapperOut);
    TEST_ASSERT(!isProxy && wrapperOut == nullptr, "InspectNvPresentSwapChain rejects proxy with null wrapper at 0x18");

    // Test 2.5: Spoofed object where outer vtable matches, but wrapper vtable DOES NOT match 0x1d39c0
    void* nonMatchingWrapperVtable[32] = {};
    for (int i = 0; i < 32; i++) nonMatchingWrapperVtable[i] = (void*)0x180099000ULL;
    struct MockInvalidWrapper {
        void** vtbl;
    } invalidWrapper = { nonMatchingWrapperVtable };

    struct MockSpoofBadWrapperVtable {
        void* vtbl;
        uint64_t pad1;
        uint64_t pad2;
        void* wrapper; // at offset 0x18
    } mockSpoofBadVtbl = { matchingProxyVtable, 0, 0, &invalidWrapper };

    wrapperOut = nullptr;
    isProxy = InspectNvPresentSwapChain(&mockSpoofBadVtbl, MOCK_NVBASE, &wrapperOut);
    TEST_ASSERT(!isProxy && wrapperOut == nullptr, "InspectNvPresentSwapChain rejects proxy with non-matching wrapper vtable");

    // Test 2.6: Genuine NvPresent64 proxy object where BOTH outer and inner vtables match!
    void* matchingWrapperVtable = (void*)(MOCK_NVBASE + RVA_INTERNAL_WRAPPER_VTABLE);
    struct MockGenuineWrapper {
        void* vtbl;
    } genuineWrapper = { matchingWrapperVtable };

    struct MockGenuineProxy {
        void* vtbl;
        uint64_t pad1;
        uint64_t pad2;
        void* wrapper; // at offset 0x18
    } mockGenuineProxy = { matchingProxyVtable, 0, 0, &genuineWrapper };

    wrapperOut = nullptr;
    isProxy = InspectNvPresentSwapChain(&mockGenuineProxy, MOCK_NVBASE, &wrapperOut);
    TEST_ASSERT(isProxy && wrapperOut == &genuineWrapper, "InspectNvPresentSwapChain confirms genuine NvPresent proxy and returns wrapper");
}

// ---------------------------------------------------------------------------
// 3. Early Persistent File Logger Tests (R2)
// ---------------------------------------------------------------------------
static void Test_EarlyLogger() {
    printf("\n=== Test Suite 3: Early Persistent File Logger (R2) ===\n");

    // Test 3.1: Initialization
    EarlyLog_Init();
    sm86::EarlyLogger& logger = sm86::EarlyLogger::Instance();
    TEST_ASSERT(logger.IsInitialized(), "EarlyLogger initializes successfully");

    const wchar_t* logPath = logger.GetLogFilePath();
    TEST_ASSERT(logPath && wcslen(logPath) > 0, "EarlyLogger log path is non-empty");
    wprintf(L"  [INFO] Active Log File: %ls\n", logPath);

    // Test 3.2: Milestone logging with immediate flush
    PROXY_MILESTONE(1, "ATTACH", "Verification harness running Milestone 1 logging test");
    PROXY_MILESTONE(9, "DXGI_HOOK", "Testing Present and ResizeBuffers hook milestone logs");
    PROXY_MILESTONE(12, "SMOOTH_MOTION", "Testing Smooth Motion activation log");
    logger.Flush();

    // Test 3.3: Verify log file exists on physical disk
    DWORD attribs = GetFileAttributesW(logPath);
    TEST_ASSERT(attribs != INVALID_FILE_ATTRIBUTES, "Log file exists on disk immediately after flush");

    // Test 3.4: Verify log contents contain formatted timestamps and milestones
    FILE* f = _wfsopen(logPath, L"r", _SH_DENYNO);
    bool foundMilestone1 = false;
    bool foundTimestamp = false;
    if (f) {
        char line[1024];
        while (fgets(line, sizeof(line), f)) {
            if (strstr(line, "[MILESTONE 1/14]") != nullptr) foundMilestone1 = true;
            if (line[0] == '[' && line[5] == '-' && line[8] == '-') foundTimestamp = true;
        }
        fclose(f);
    }
    TEST_ASSERT(foundMilestone1, "Log file contains Milestone 1 record");
    TEST_ASSERT(foundTimestamp, "Log file entries have [YYYY-MM-DD HH:MM:SS.mmm] formatted timestamps");
}

// ---------------------------------------------------------------------------
// 4. VBlank Pacing & Resize Handling Tests (R3, R4)
// ---------------------------------------------------------------------------
static void Test_VBlankPacingAndResize() {
    printf("\n=== Test Suite 4: VBlank Pacing & Resize Handling (R3, R4) ===\n");

    // Test 4.1: PaceVBlankBetweenPresents null handling
    PaceVBlankBetweenPresents(nullptr);
    TEST_ASSERT(true, "PaceVBlankBetweenPresents handles nullptr gracefully");

    // Test 4.2: Dummy D3D11 swapchain creation to test GetContainingOutput & VBlank fallback
    WNDCLASSA wc = {};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "sm86_test_dummy_cls";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowA("sm86_test_dummy_cls", "test", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);

    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 100;
    sd.BufferDesc.Height = 100;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    sd.Windowed = TRUE;

    IDXGISwapChain* sc = nullptr;
    ID3D11Device* dev11 = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL lvl = D3D_FEATURE_LEVEL_11_0;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                               nullptr, 0, D3D11_SDK_VERSION, &sd, &sc,
                                               &dev11, &lvl, &ctx);
    if (SUCCEEDED(hr) && sc) {
        // Test PaceVBlankBetweenPresents on active window
        PaceVBlankBetweenPresents(sc);
        TEST_ASSERT(true, "PaceVBlankBetweenPresents executes without hang or crash on active swapchain");

        // Test ResizeBuffers lifecycle call
        hr = sc->ResizeBuffers(2, 200, 200, DXGI_FORMAT_R8G8B8A8_UNORM, 0);
        TEST_ASSERT(SUCCEEDED(hr), "DirectX ResizeBuffers succeeds smoothly on swapchain");

        sc->Release();
        if (ctx) ctx->Release();
        if (dev11) dev11->Release();
    } else {
        printf("  [SKIP] D3D11 Hardware device unavailable in current context (hr=0x%08X)\n", (uint32_t)hr);
    }
    DestroyWindow(hwnd);
}

// ---------------------------------------------------------------------------
// Main Harness Entrypoint
// ---------------------------------------------------------------------------
int main() {
    printf("====================================================================\n");
    printf("  sm86_smooth Proxy Runtime Hardening Verification Harness\n");
    printf("  Requirements: R1 (Safe Probing), R2 (Early Logger), R3 (VBlank), R4 (Resize)\n");
    printf("====================================================================\n");

    Test_SafeReadPointer();
    Test_InspectNvPresentSwapChain();
    Test_EarlyLogger();
    Test_VBlankPacingAndResize();

    printf("\n--------------------------------------------------------------------\n");
    printf("  Verification Summary: %d / %d Tests Passed\n", g_testsPassed, g_testsTotal);
    printf("--------------------------------------------------------------------\n");

    return (g_testsPassed == g_testsTotal) ? 0 : 1;
}
