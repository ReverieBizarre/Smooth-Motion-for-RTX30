// ============================================================================
//  tools/nvp_perf_bench.cpp - Empirical Performance & Overhead Benchmark
//  Measures real-world GPU latency (ms) and VRAM footprint (MB) of
//  NvPresent64.dll Smooth Motion on RTX 3080 sm_86.
// ============================================================================
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include <numeric>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "user32.lib")

#include "../src/proxy/pe_scan.h"
#include "../src/proxy/early_logger.h"

static const uint32_t FATBIN_MAGIC = 0xba55ed50;

static bool patchb(uint8_t* p, const uint8_t* pat, size_t n) {
    DWORD o = 0;
    if (!VirtualProtect(p, n, PAGE_EXECUTE_READWRITE, &o)) return false;
    memcpy(p, pat, n);
    VirtualProtect(p, n, o, &o);
    FlushInstructionCache(GetCurrentProcess(), p, n);
    return true;
}

static void patch_fatbin(uint8_t* b, size_t size) {
    for (size_t off = 0; off + 3 < size; off += 4)
        if ((b[off] == 0x78 || b[off] == 0x59) && b[off+1] == 0 && b[off+2] == 0 && b[off+3] == 0)
            b[off] = 0x56;
    for (size_t off = 0; off + 0x34 < size; ++off)
        if (b[off] == 0x7f && b[off+1] == 'E' && b[off+2] == 'L' && b[off+3] == 'F') {
            uint32_t fl = 0x560556;
            memcpy(b + off + 0x30, &fl, 4);
        }
}

typedef int (__stdcall *LoadData_t)(void**, const void*);
static LoadData_t real_load = nullptr;
static int __stdcall hook_load(void** m, const void* img) {
    const uint8_t* p = (const uint8_t*)img;
    if (p && *(const uint32_t*)p == FATBIN_MAGIC) {
        static uint8_t buf[8 * 1024 * 1024];
        uint64_t sz = *(const uint64_t*)(p + 8);
        if (sz < sizeof(buf)) {
            memcpy(buf, p, (size_t)sz);
            patch_fatbin(buf, (size_t)sz);
            p = buf;
        }
    }
    return real_load(m, p);
}

typedef int (__stdcall *EventCreate_t)(void**, unsigned int);
typedef int (__stdcall *EventRecord_t)(void*, void*);
typedef int (__stdcall *EventSync_t)(void*);
typedef int (__stdcall *EventElapsed_t)(float*, void*, void*);
typedef int (__stdcall *EventDestroy_t)(void*);
typedef int (__stdcall *MemInfo_t)(size_t*, size_t*);

static EventCreate_t  pfnEventCreate = nullptr;
static EventRecord_t  pfnEventRecord = nullptr;
static EventSync_t    pfnEventSync = nullptr;
static EventElapsed_t pfnEventElapsed = nullptr;
static EventDestroy_t pfnEventDestroy = nullptr;
static MemInfo_t      pfnMemInfo = nullptr;

typedef int (__stdcall *GraphLaunch_t)(void*, void*);
static GraphLaunch_t real_graph_launch = nullptr;
static std::vector<float> s_graph_times;

static int __stdcall hook_graph_launch(void* gExec, void* stream) {
    void *evStart = nullptr, *evEnd = nullptr;
    if (pfnEventCreate) {
        pfnEventCreate(&evStart, 0);
        pfnEventCreate(&evEnd, 0);
        pfnEventRecord(evStart, stream);
    }

    int res = real_graph_launch(gExec, stream);

    if (pfnEventCreate && evStart && evEnd) {
        pfnEventRecord(evEnd, stream);
        pfnEventSync(evEnd);
        float ms = 0.0f;
        pfnEventElapsed(&ms, evStart, evEnd);
        s_graph_times.push_back(ms);
        pfnEventDestroy(evStart);
        pfnEventDestroy(evEnd);
    }
    return res;
}

typedef bool (*pfnInitD3D)(void);

static void RunBenchmark(int width, int height, int totalFrames) {
    printf("\n>>> Running Benchmark @ %dx%d (%d frames)...\n", width, height, totalFrames);
    s_graph_times.clear();

    ID3D12Device* dev = nullptr;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev)))) return;

    ID3D12CommandQueue* cq = nullptr;
    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&cq));

    WNDCLASSA wc = {};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    char clsName[64]; sprintf(clsName, "bench_%dx%d", width, height);
    wc.lpszClassName = clsName;
    RegisterClassA(&wc);
    HWND wnd = CreateWindowA(clsName, "Perf Bench", WS_OVERLAPPEDWINDOW, 0, 0, width, height, nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(wnd, SW_SHOW);
    UpdateWindow(wnd);

    IDXGIFactory4* f = nullptr;
    CreateDXGIFactory1(IID_PPV_ARGS(&f));
    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width = width;
    sd.Height = height;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    IDXGISwapChain1* swap = nullptr;
    f->CreateSwapChainForHwnd(cq, wnd, &sd, nullptr, nullptr, &swap);
    if (!swap) { printf("[!] CreateSwapChainForHwnd failed\n"); return; }

    void* wrapper = nullptr;
    if (InspectNvPresentSwapChain(swap, 0, &wrapper) && wrapper) {
        void** vt = *(void***)wrapper;
        typedef void (*pfnSetByte)(void*, uint8_t);
        ((pfnSetByte)vt[19])(wrapper, 1);
        ((pfnSetByte)vt[20])(wrapper, 1);
    }

    ID3D12CommandAllocator* ca = nullptr;
    dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&ca));
    ID3D12GraphicsCommandList* cl = nullptr;
    dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, ca, nullptr, IID_PPV_ARGS(&cl));

    ID3D12DescriptorHeap* rtvHeap = nullptr;
    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc = {};
    rtvDesc.NumDescriptors = 2;
    rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    dev->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&rtvHeap));
    SIZE_T rtvSize = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    ID3D12Resource* rtvResources[2] = {};
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < 2; i++) {
        swap->GetBuffer(i, IID_PPV_ARGS(&rtvResources[i]));
        dev->CreateRenderTargetView(rtvResources[i], nullptr, rtvHandle);
        rtvHandle.ptr += rtvSize;
    }

    ID3D12Fence* fence = nullptr;
    dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    UINT64 fenceVal = 1;
    HANDLE fenceEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    IDXGISwapChain3* swap3 = nullptr;
    swap->QueryInterface(IID_PPV_ARGS(&swap3));

    // Render loop
    for (int frame = 0; frame < totalFrames; frame++) {
        ca->Reset();
        cl->Reset(ca, nullptr);

        UINT backIndex = swap3 ? swap3->GetCurrentBackBufferIndex() : (frame % 2);

        D3D12_RESOURCE_BARRIER b1 = {};
        b1.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b1.Transition.pResource = rtvResources[backIndex];
        b1.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        b1.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        b1.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cl->ResourceBarrier(1, &b1);

        D3D12_CPU_DESCRIPTOR_HANDLE curRtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        curRtv.ptr += backIndex * rtvSize;

        float bgColor[4] = { 0.1f, 0.1f, 0.15f, 1.0f };
        cl->ClearRenderTargetView(curRtv, bgColor, 0, nullptr);

        // moving bar
        int barX = (frame * 15) % width;
        D3D12_RECT rect = { (LONG)barX, 100, (LONG)(barX + 80), (LONG)(height - 100) };
        float barColor[4] = { 0.9f, 0.9f, 0.9f, 1.0f };
        cl->ClearRenderTargetView(curRtv, barColor, 1, &rect);

        D3D12_RESOURCE_BARRIER b2 = b1;
        b2.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        b2.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        cl->ResourceBarrier(1, &b2);

        cl->Close();
        ID3D12CommandList* cls[] = { cl };
        cq->ExecuteCommandLists(1, cls);

        cq->Signal(fence, fenceVal);
        if (fence->GetCompletedValue() < fenceVal) {
            fence->SetEventOnCompletion(fenceVal, fenceEvent);
            WaitForSingleObject(fenceEvent, INFINITE);
        }
        fenceVal++;

        MSG msg;
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) { DispatchMessageA(&msg); }

        swap->Present(0, 0);
    }

    // Statistics
    if (!s_graph_times.empty()) {
        // Discard first 2 frames as warmup / pipeline fill
        size_t startIdx = (s_graph_times.size() > 3) ? 2 : 0;
        float sum = 0.0f;
        float minTime = 9999.0f, maxTime = 0.0f;
        size_t count = 0;
        for (size_t i = startIdx; i < s_graph_times.size(); i++) {
            float t = s_graph_times[i];
            sum += t;
            if (t < minTime) minTime = t;
            if (t > maxTime) maxTime = t;
            count++;
        }
        float avgTime = sum / count;
        printf("--- Results for %dx%d (%zu steady frames) ---\n", width, height, count);
        printf("    Avg Frame Interpolation GPU Time: %.3f ms\n", avgTime);
        printf("    Min: %.3f ms, Max: %.3f ms\n", minTime, maxTime);
        printf("    Effective Theoretical Throughput: %.1f FPS\n", 1000.0f / avgTime);
    } else {
        printf("    [!] No cuGraphLaunch timings captured\n");
    }

    if (swap3) swap3->Release();
    CloseHandle(fenceEvent);
    fence->Release();
    for (int i = 0; i < 2; i++) if (rtvResources[i]) rtvResources[i]->Release();
    rtvHeap->Release();
    cl->Release();
    ca->Release();
    swap->Release();
    f->Release();
    cq->Release();
    dev->Release();
    DestroyWindow(wnd);
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);

    printf("================================================================\n");
    printf("  NvPresent64 Smooth Motion (Road 1) Performance Benchmark\n");
    printf("  GPU: NVIDIA GeForce RTX 3080 (sm_86, Ampere)\n");
    printf("================================================================\n");

    HMODULE cu = LoadLibraryA("nvcuda.dll");
    if (cu) {
        pfnEventCreate  = (EventCreate_t)GetProcAddress(cu, "cuEventCreate");
        pfnEventRecord  = (EventRecord_t)GetProcAddress(cu, "cuEventRecord");
        pfnEventSync    = (EventSync_t)GetProcAddress(cu, "cuEventSynchronize");
        pfnEventElapsed = (EventElapsed_t)GetProcAddress(cu, "cuEventElapsedTime");
        pfnEventDestroy = (EventDestroy_t)GetProcAddress(cu, "cuEventDestroy_v2");
        pfnMemInfo      = (MemInfo_t)GetProcAddress(cu, "cuMemGetInfo_v2");
    }

    HMODULE nv = sm86::LoadNvPresent();
    if (!nv) { printf("[!] sm86::LoadNvPresent() failed\n"); return 1; }

    // Gate patch (Dynamic Pattern Scan)
    uint8_t* c = nullptr;
    uint8_t* setgePtr = nullptr;
    uint32_t setgeLen = 0;
    if (sm86::LocateGateAddresses(nv, &c, &setgePtr, &setgeLen)) {
        printf("[+] Gate located via Pattern Scan (cmp imm RVA +0x%lx, setge RVA +0x%lx, len %u)\n",
               (uint32_t)(c - (uint8_t*)nv), (uint32_t)(setgePtr - (uint8_t*)nv), setgeLen);
    } else {
        printf("[!] Gate pattern scan failed! Falling back to 0xc41f / 0xc437\n");
        c = (uint8_t*)nv + 0xc41f;
        setgePtr = (uint8_t*)nv + 0xc437;
        setgeLen = 4;
    }
    uint8_t two = 0x02;
    patchb(c, &two, 1);
    if (setgeLen == 4) {
        uint8_t x[4] = { 0x40, 0xB6, 0x01, 0x90 }; // mov sil, 1; nop
        patchb(setgePtr, x, 4);
    } else {
        uint8_t x[3] = { 0xB6, 0x01, 0x90 }; // mov sil, 1; nop
        patchb(setgePtr, x, 3);
    }

    // IAT hooks (Dynamic PE Import Directory Walker)
    DWORD o = 0;
    void** iat_load = sm86::FindIATEntry(nv, "nvcuda.dll", "cuModuleLoadData");
    if (!iat_load) iat_load = (void**)((uint8_t*)nv + 0x1d2820);
    VirtualProtect(iat_load, sizeof(void*), PAGE_READWRITE, &o);
    real_load = (LoadData_t)*iat_load;
    *iat_load = (void*)&hook_load;
    VirtualProtect(iat_load, sizeof(void*), o, &o);

    void** iat_graph = sm86::FindIATEntry(nv, "nvcuda.dll", "cuGraphLaunch");
    if (!iat_graph) iat_graph = (void**)((uint8_t*)nv + 0x1d2780);
    VirtualProtect(iat_graph, sizeof(void*), PAGE_READWRITE, &o);
    real_graph_launch = (GraphLaunch_t)*iat_graph;
    *iat_graph = (void*)&hook_graph_launch;
    VirtualProtect(iat_graph, sizeof(void*), o, &o);

    // Global Config Gate
    pfnInitD3D NVP_Init_D3D = (pfnInitD3D)GetProcAddress(nv, "NVP_Init_D3D");
    uint8_t* S = sm86::ResolveConfigStructFromInit(nv);
    if (!S) S = (uint8_t*)nv + 0x7d7810;
    S[0x4c] = 1; S[0xe8] = 1; S[0xe9] = 1; S[0x12a5] = 1;
    bool initOk = NVP_Init_D3D ? NVP_Init_D3D() : false;
    S[0x4c] = 1; S[0xe8] = 1; S[0xe9] = 1; S[0x12a5] = 1;

    IDXGIFactory4* fMem = nullptr; CreateDXGIFactory1(IID_PPV_ARGS(&fMem));
    IDXGIAdapter* aMem = nullptr; fMem->EnumAdapters(0, &aMem);
    IDXGIAdapter3* a3Mem = nullptr; aMem->QueryInterface(IID_PPV_ARGS(&a3Mem));
    DXGI_QUERY_VIDEO_MEMORY_INFO vmiBef = {}, vmiAft = {};
    if (a3Mem) a3Mem->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &vmiBef);

    // Run benchmarks at 1080p, 1440p, and 4K
    RunBenchmark(1920, 1080, 20);

    if (a3Mem) {
        a3Mem->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &vmiAft);
        printf("\n>>> VRAM Delta after 1080p pipeline initialization: %.1f MB\n",
               (float)(vmiAft.CurrentUsage - vmiBef.CurrentUsage) / (1024.0f * 1024.0f));
    }

    RunBenchmark(2560, 1440, 20);
    RunBenchmark(3840, 2160, 20);

    if (a3Mem) {
        a3Mem->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &vmiAft);
        printf("\n>>> Total VRAM Usage at 4K: %.1f MB (Delta from baseline: %.1f MB)\n",
               (float)vmiAft.CurrentUsage / (1024.0f * 1024.0f),
               (float)(vmiAft.CurrentUsage - vmiBef.CurrentUsage) / (1024.0f * 1024.0f));
        a3Mem->Release();
    }
    if (aMem) aMem->Release();
    if (fMem) fMem->Release();

    printf("\n================================================================\n");
    printf("  BENCHMARK COMPLETE\n");
    printf("================================================================\n");

    return 0;
}
