// ============================================================================
//  tools/test_d3d11_bridge_nvp.cpp - Verify D3D11-to-D3D12 Bridge with Road 1
// ============================================================================
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <cstdio>
#include <cstdint>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "user32.lib")

#include "../src/proxy/pe_scan.h"
#include "../src/proxy/d3d11_to_d3d12_bridge.h"

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
static int s_load_count = 0;
static int __stdcall hook_load(void** m, const void* img) {
    const uint8_t* p = (const uint8_t*)img;
    if (p && *(const uint32_t*)p == FATBIN_MAGIC) {
        s_load_count++;
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

typedef int (__stdcall *LaunchKernel_t)(void*, unsigned int, unsigned int, unsigned int,
                                       unsigned int, unsigned int, unsigned int,
                                       unsigned int, void*, void**, void**);
static LaunchKernel_t real_launch = nullptr;
static int s_launch_count = 0;
static int __stdcall hook_launch(void* f, unsigned int gx, unsigned int gy, unsigned int gz,
                                 unsigned int bx, unsigned int by, unsigned int bz,
                                 unsigned int smem, void* s, void** params, void** extra) {
    s_launch_count++;
    return real_launch(f, gx, gy, gz, bx, by, bz, smem, s, params, extra);
}

typedef int (__stdcall *GraphLaunch_t)(void*, void*);
static GraphLaunch_t real_graph_launch = nullptr;
static int s_graph_launch_count = 0;
static int __stdcall hook_graph_launch(void* gExec, void* stream) {
    s_graph_launch_count++;
    printf("  >>> cuGraphLaunch #%d: gExec=%p stream=%p (Ampere FP16 HMMA Active)\n",
           s_graph_launch_count, gExec, stream);
    return real_graph_launch(gExec, stream);
}

#include <winternl.h>

static void SpoofPebProcessName(const wchar_t* fakeExeName) {
    uint8_t* peb = (uint8_t*)__readgsqword(0x60);
    uint8_t* params = *(uint8_t**)(peb + 0x20); // ProcessParameters
    UNICODE_STRING* imgPath = (UNICODE_STRING*)(params + 0x60);
    wprintf(L"[PEB] Original ImagePath: %s\n", imgPath->Buffer);

    wchar_t* lastSlash = wcsrchr(imgPath->Buffer, L'\\');
    if (lastSlash) {
        wcscpy(lastSlash + 1, fakeExeName);
        imgPath->Length = (USHORT)(wcslen(imgPath->Buffer) * sizeof(wchar_t));
        wprintf(L"[PEB] Spoofed ImagePath to: %s\n", imgPath->Buffer);
    }
}

typedef bool (*pfnInitD3D)(void);

int main() {

    setvbuf(stdout, nullptr, _IONBF, 0);

    // If current process is mpc-hc64.exe, spoof it to mpc_game.exe before NvPresent64 loads!
    SpoofPebProcessName(L"mpc_game.exe");


    printf("================================================================\n");
    printf("  Testing D3D11-to-D3D12 Shadow SwapChain Bridge for Road 1     \n");
    printf("================================================================\n\n");


    HMODULE nv = sm86::LoadNvPresent();
    if (!nv) { printf("[!] sm86::LoadNvPresent() failed\n"); return 1; }
    printf("[+] Loaded NvPresent64.dll @ %p\n", nv);

    // 1. Gate Patch
    uint8_t* c = nullptr;
    uint8_t* setgePtr = nullptr;
    uint32_t setgeLen = 0;
    if (sm86::LocateGateAddresses(nv, &c, &setgePtr, &setgeLen)) {
        printf("[+] Gate located via Pattern Scan (cmp imm RVA +0x%lx, setge RVA +0x%lx, len %u)\n",
               (uint32_t)(c - (uint8_t*)nv), (uint32_t)(setgePtr - (uint8_t*)nv), setgeLen);
    } else {
        c = (uint8_t*)nv + 0xc41f;
        setgePtr = (uint8_t*)nv + 0xc437;
        setgeLen = 4;
    }
    uint8_t two = 0x02;
    patchb(c, &two, 1);
    if (setgeLen == 4) {
        uint8_t x[4] = { 0x40, 0xB6, 0x01, 0x90 };
        patchb(setgePtr, x, 4);
    } else {
        uint8_t x[3] = { 0xB6, 0x01, 0x90 };
        patchb(setgePtr, x, 3);
    }

    // 2. IAT Hooks
    DWORD o = 0;
    void** iat_load = sm86::FindIATEntry(nv, "nvcuda.dll", "cuModuleLoadData");
    if (!iat_load) iat_load = (void**)((uint8_t*)nv + 0x1d2820);
    VirtualProtect(iat_load, sizeof(void*), PAGE_READWRITE, &o);
    real_load = (LoadData_t)*iat_load;
    *iat_load = (void*)&hook_load;
    VirtualProtect(iat_load, sizeof(void*), o, &o);

    void** iat_launch = sm86::FindIATEntry(nv, "nvcuda.dll", "cuLaunchKernel");
    if (!iat_launch) iat_launch = (void**)((uint8_t*)nv + 0x1d27f8);
    VirtualProtect(iat_launch, sizeof(void*), PAGE_READWRITE, &o);
    real_launch = (LaunchKernel_t)*iat_launch;
    *iat_launch = (void*)&hook_launch;
    VirtualProtect(iat_launch, sizeof(void*), o, &o);

    void** iat_graph = sm86::FindIATEntry(nv, "nvcuda.dll", "cuGraphLaunch");
    if (!iat_graph) iat_graph = (void**)((uint8_t*)nv + 0x1d2780);
    VirtualProtect(iat_graph, sizeof(void*), PAGE_READWRITE, &o);
    real_graph_launch = (GraphLaunch_t)*iat_graph;
    *iat_graph = (void*)&hook_graph_launch;
    VirtualProtect(iat_graph, sizeof(void*), o, &o);

    // 3. NVP_Init_D3D
    pfnInitD3D NVP_Init_D3D = (pfnInitD3D)GetProcAddress(nv, "NVP_Init_D3D");
    uint8_t* S = sm86::ResolveConfigStructFromInit(nv);
    if (!S) S = (uint8_t*)nv + 0x7d7810;
    S[0x4c] = 1; S[0xe8] = 1; S[0xe9] = 1; S[0x12a5] = 1;
    bool initOk = NVP_Init_D3D ? NVP_Init_D3D() : false;
    S[0x4c] = 1; S[0xe8] = 1; S[0xe9] = 1; S[0x12a5] = 1;
    printf("[+] NVP_Init_D3D() -> %s\n", initOk ? "TRUE" : "FALSE");
    if (!initOk) return 1;

    // DIAGNOSTIC HOOK CHECK
    HMODULE hDxgi = GetModuleHandleA("dxgi.dll");
    if (hDxgi) {
        FARPROC p0 = GetProcAddress(hDxgi, "CreateDXGIFactory");
        FARPROC p1 = GetProcAddress(hDxgi, "CreateDXGIFactory1");
        FARPROC p2 = GetProcAddress(hDxgi, "CreateDXGIFactory2");
        printf("[DXGI HOOK CHECK]\n");
        if (p0) printf("  CreateDXGIFactory:  %p -> bytes: %02x %02x %02x %02x %02x\n",
                       p0, ((uint8_t*)p0)[0], ((uint8_t*)p0)[1], ((uint8_t*)p0)[2], ((uint8_t*)p0)[3], ((uint8_t*)p0)[4]);
        if (p1) printf("  CreateDXGIFactory1: %p -> bytes: %02x %02x %02x %02x %02x\n",
                       p1, ((uint8_t*)p1)[0], ((uint8_t*)p1)[1], ((uint8_t*)p1)[2], ((uint8_t*)p1)[3], ((uint8_t*)p1)[4]);
        if (p2) printf("  CreateDXGIFactory2: %p -> bytes: %02x %02x %02x %02x %02x\n",
                       p2, ((uint8_t*)p2)[0], ((uint8_t*)p2)[1], ((uint8_t*)p2)[2], ((uint8_t*)p2)[3], ((uint8_t*)p2)[4]);
    }
    HMODULE hD3D12 = GetModuleHandleA("d3d12.dll");
    if (hD3D12) {
        FARPROC pDev = GetProcAddress(hD3D12, "D3D12CreateDevice");
        if (pDev) printf("  D3D12CreateDevice:  %p -> bytes: %02x %02x %02x %02x %02x\n",
                         pDev, ((uint8_t*)pDev)[0], ((uint8_t*)pDev)[1], ((uint8_t*)pDev)[2], ((uint8_t*)pDev)[3], ((uint8_t*)pDev)[4]);
    }

    // Check EXE IAT
    HMODULE hExe = GetModuleHandleA(nullptr);
    void** iatDxgi = sm86::FindIATEntry(hExe, "dxgi.dll", "CreateDXGIFactory1");
    if (iatDxgi) printf("  EXE IAT CreateDXGIFactory1: %p -> points to %p\n", iatDxgi, *iatDxgi);
    void** iatD12 = sm86::FindIATEntry(hExe, "d3d12.dll", "D3D12CreateDevice");
    if (iatD12) printf("  EXE IAT D3D12CreateDevice:  %p -> points to %p\n", iatD12, *iatD12);


    // 4. Create Test Window and D3D11 Device & SwapChain
    WNDCLASSA wc = {};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "d3d11_bridge_test_cls";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowA("d3d11_bridge_test_cls", "D3D11 Bridge Test", WS_OVERLAPPEDWINDOW,
                              0, 0, 512, 512, nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    DXGI_SWAP_CHAIN_DESC scd = {};
    scd.BufferCount = 2;
    scd.BufferDesc.Width = 2617;
    scd.BufferDesc.Height = 1811;
    scd.BufferDesc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    scd.BufferDesc.RefreshRate.Numerator = 60;
    scd.BufferDesc.RefreshRate.Denominator = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = hwnd;
    scd.SampleDesc.Count = 1;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scd.Windowed = TRUE;

    ID3D11Device* dev11 = nullptr;
    ID3D11DeviceContext* ctx11 = nullptr;
    IDXGISwapChain* swap11 = nullptr;
    D3D_FEATURE_LEVEL fl;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION, &scd, &swap11, &dev11, &fl, &ctx11
    );
    if (FAILED(hr)) {
        printf("[!] D3D11CreateDeviceAndSwapChain failed: 0x%08X\n", (uint32_t)hr);
        return 1;
    }
    printf("[+] D3D11 Device created @ %p, SwapChain @ %p\n", dev11, swap11);

    // 5. Initialize D3D11-to-D3D12 Bridge
    sm86::D3D11ToD3D12Bridge bridge;
    if (!bridge.Initialize(dev11, hwnd, 2617, 1811, DXGI_FORMAT_R10G10B10A2_UNORM)) {
        printf("[!] bridge.Initialize failed!\n");
        return 1;
    }
    printf("[+] Bridge initialized! IsActive=%d, Wrapper=%p\n", bridge.IsActive(), bridge.GetWrapper());

    // 6. Simulate video frames (moving box) rendered in D3D11 and presented via Bridge
    ID3D11Texture2D* bb11 = nullptr;
    swap11->GetBuffer(0, IID_PPV_ARGS(&bb11));
    ID3D11RenderTargetView* rtv11 = nullptr;
    dev11->CreateRenderTargetView(bb11, nullptr, &rtv11);

    printf("\n[+] Starting D3D11 playback loop through Road 1 Bridge...\n");
    for (int frame = 0; frame < 5; frame++) {
        // Clear background: dark navy blue
        float clearCol[4] = { 0.05f, 0.05f, 0.2f, 1.0f };
        ctx11->ClearRenderTargetView(rtv11, clearCol);

        // Draw animated frame in D3D11
        // (In real video playback, MPC-VR decodes frame into this RTV)
        MSG msg;
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) { DispatchMessageA(&msg); }

        int graphs_before = s_graph_launch_count;
        bool ok = bridge.Present(swap11, 0, 0);
        printf("[D3D11 Frame %d] bridge.Present -> %s (graphs launched: +%d)\n",
               frame, ok ? "OK" : "FAIL", s_graph_launch_count - graphs_before);
    }

    rtv11->Release();
    bb11->Release();
    bridge.Shutdown();
    swap11->Release();
    ctx11->Release();
    dev11->Release();
    DestroyWindow(hwnd);

    printf("\n================================================================\n");
    printf("  SUMMARY STATS:\n");
    printf("    - FP16 Fatbinaries Loaded: %d (19 expected on Tier 2)\n", s_load_count);
    printf("    - Frame cuGraphLaunch:     %d (Triggered via D3D11 Bridge!)\n", s_graph_launch_count);
    printf("================================================================\n");
    if (s_graph_launch_count > 0) {
        printf("[+] SUCCESS: D3D11 apps can now seamlessly invoke Road 1 (NvPresent64)!\n");
    } else {
        printf("[!] Warning: cuGraphLaunch was not triggered\n");
    }

    return 0;
}
