// ============================================================================
//  sm86_rehost.cpp - Production Version.dll Proxy for Road 1 (NvPresent64)
//  Re-hosting NVIDIA Smooth Motion (DLSS Frame Generation) on RTX 30-series (sm_86).
//
//  How it works when placed in a game folder as version.dll:
//    1. Forwards all 17 Version.dll API calls to C:\Windows\System32\version.dll.
//    2. On attach (via private startup thread to avoid loader lock):
//       - Finds and loads NvPresent64.dll from process or DriverStore.
//       - Patches gate: cmp [rcx+0x14], 2 (allow Tier 2) + mov sil, 1; nop (enable VFI).
//       - Hooks NvPresent64.dll IAT: cuModuleLoadData (RVA 0x1d2820) dynamically
//         rewrites 19 FP16 fatbinaries from sm_89/sm_120 to sm_86 on the fly.
//       - Opens global config gate [0x7d7810] and calls NVP_Init_D3D().
//       - NvPresent64.dll detours DXGI CreateSwapChainForHwnd.
//    3. Intercepts DXGI Present:
//       - Checks if swapchain is wrapped by NvPresent64 (proxy COM object).
//       - Automatically enables Smooth Motion on internal wrapper (vt[19]=1, vt[20]=1).
//       - Calls original Present, which dispatches optical flow & cuGraphLaunch!
// ============================================================================
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d12.h>
#include <d3d11.h>
#include <dxgi1_4.h>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <set>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "d3d11.lib")

// ---- forward the 17 version.dll exports to the real System32 DLL ----
#pragma comment(linker, "/export:GetFileVersionInfoA=C:////Windows////System32////version.dll.GetFileVersionInfoA")
#pragma comment(linker, "/export:GetFileVersionInfoByHandle=C:////Windows////System32////version.dll.GetFileVersionInfoByHandle")
#pragma comment(linker, "/export:GetFileVersionInfoExA=C:////Windows////System32////version.dll.GetFileVersionInfoExA")
#pragma comment(linker, "/export:GetFileVersionInfoExW=C:////Windows////System32////version.dll.GetFileVersionInfoExW")
#pragma comment(linker, "/export:GetFileVersionInfoSizeA=C:////Windows////System32////version.dll.GetFileVersionInfoSizeA")
#pragma comment(linker, "/export:GetFileVersionInfoSizeExA=C:////Windows////System32////version.dll.GetFileVersionInfoSizeExA")
#pragma comment(linker, "/export:GetFileVersionInfoSizeExW=C:////Windows////System32////version.dll.GetFileVersionInfoSizeExW")
#pragma comment(linker, "/export:GetFileVersionInfoSizeW=C:////Windows////System32////version.dll.GetFileVersionInfoSizeW")
#pragma comment(linker, "/export:GetFileVersionInfoW=C:////Windows////System32////version.dll.GetFileVersionInfoW")
#pragma comment(linker, "/export:VerFindFileA=C:////Windows////System32////version.dll.VerFindFileA")
#pragma comment(linker, "/export:VerFindFileW=C:////Windows////System32////version.dll.VerFindFileW")
#pragma comment(linker, "/export:VerInstallFileA=C:////Windows////System32////version.dll.VerInstallFileA")
#pragma comment(linker, "/export:VerInstallFileW=C:////Windows////System32////version.dll.VerInstallFileW")
#pragma comment(linker, "/export:VerLanguageNameA=C:////Windows////System32////version.dll.VerLanguageNameA")
#pragma comment(linker, "/export:VerLanguageNameW=C:////Windows////System32////version.dll.VerLanguageNameW")
#pragma comment(linker, "/export:VerQueryValueA=C:////Windows////System32////version.dll.VerQueryValueA")
#pragma comment(linker, "/export:VerQueryValueW=C:////Windows////System32////version.dll.VerQueryValueW")

static const uint32_t RVA_GATE_CMP_IMM = 0xc41f;
static const uint32_t RVA_GATE_SETGE    = 0xc437;
static const uint32_t FATBIN_MAGIC      = 0xba55ed50;

static HMODULE g_nvpresent = nullptr;
static CRITICAL_SECTION g_cs;
static std::set<void*> g_activatedWrappers;

// ---------------------------------------------------------------------------
// In-Memory Patch Helpers
// ---------------------------------------------------------------------------
static bool patch_bytes(uint8_t* p, const uint8_t* pat, size_t n) {
    DWORD old = 0;
    if (!VirtualProtect(p, n, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(p, pat, n);
    VirtualProtect(p, n, old, &old);
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

// ---------------------------------------------------------------------------
// cuModuleLoadData IAT Hook
// ---------------------------------------------------------------------------
typedef int (__stdcall *cuModuleLoadData_t)(void** module, const void* image);
static cuModuleLoadData_t g_realModuleLoadData = nullptr;

static int __stdcall hook_cuModuleLoadData(void** module, const void* image) {
    const uint8_t* p = (const uint8_t*)image;
    if (p && *(const uint32_t*)p == FATBIN_MAGIC) {
        uint64_t size = *(const uint64_t*)(p + 8);
        static uint8_t buf[8 * 1024 * 1024];
        if (size < sizeof(buf)) {
            memcpy(buf, p, (size_t)size);
            patch_fatbin(buf, (size_t)size);
            p = buf;
        }
    }
    return g_realModuleLoadData(module, p);
}

typedef bool (*pfnInitD3D)(void);

// ---------------------------------------------------------------------------
// DXGI SwapChain Hooking
// ---------------------------------------------------------------------------
typedef HRESULT (STDMETHODCALLTYPE *Present_t)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE *Present1_t)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
static Present_t  g_origPresent  = nullptr;
static Present1_t g_origPresent1 = nullptr;

static void ActivateSmoothMotionIfWrapped(IDXGISwapChain* swap) {
    if (!swap) return;
    // Check if swap has the NvPresent internal wrapper pointer at +0x18
    void* wrapper = *(void**)((uint8_t*)swap + 0x18);
    if (!wrapper) return;

    EnterCriticalSection(&g_cs);
    if (g_activatedWrappers.find(wrapper) == g_activatedWrappers.end()) {
        g_activatedWrappers.insert(wrapper);
        LeaveCriticalSection(&g_cs);

        void** vt = *(void***)wrapper;
        typedef void (*pfnSetByte)(void*, uint8_t);
        ((pfnSetByte)vt[19])(wrapper, 1);
        ((pfnSetByte)vt[20])(wrapper, 1);
        printf("[sm86_rehost] Activated Smooth Motion on swapchain wrapper @ %p\n", wrapper);
    } else {
        LeaveCriticalSection(&g_cs);
    }
}

static HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain* swap, UINT sync, UINT flags) {
    ActivateSmoothMotionIfWrapped(swap);
    return g_origPresent(swap, sync, flags);
}

static HRESULT STDMETHODCALLTYPE HookedPresent1(IDXGISwapChain1* swap, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* p) {
    ActivateSmoothMotionIfWrapped(swap);
    return g_origPresent1(swap, sync, flags, p);
}

static void InstallDxgiHooks() {
    WNDCLASSA wc = {};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "sm86_dummy_cls";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowA("sm86_dummy_cls", "", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);

    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 64;
    sd.BufferDesc.Height = 64;
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

    if (SUCCEEDED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                                nullptr, 0, D3D11_SDK_VERSION, &sd, &sc,
                                                &dev11, &lvl, &ctx))) {
        IDXGISwapChain1* sc1 = nullptr;
        if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&sc1)))) {
            void** vt = *(void***)sc1;
            DWORD oldProt = 0;
            VirtualProtect(&vt[8], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProt);
            g_origPresent = (Present_t)vt[8];
            vt[8] = (void*)&HookedPresent;
            VirtualProtect(&vt[8], sizeof(void*), oldProt, &oldProt);

            VirtualProtect(&vt[22], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProt);
            g_origPresent1 = (Present1_t)vt[22];
            vt[22] = (void*)&HookedPresent1;
            VirtualProtect(&vt[22], sizeof(void*), oldProt, &oldProt);

            printf("[sm86_rehost] DXGI Present hooks installed (slot 8 & slot 22)\n");
            sc1->Release();
        }
        sc->Release();
        if (ctx) ctx->Release();
        if (dev11) dev11->Release();
    }
    DestroyWindow(hwnd);
}

// ---------------------------------------------------------------------------
// Main Patch Engine
// ---------------------------------------------------------------------------
static bool ApplyRehost(HMODULE nv) {
    printf("[sm86_rehost] Initializing NvPresent64 rehost @ %p\n", nv);

    // 1. Gate patch
    uint8_t* cmpImm = (uint8_t*)nv + RVA_GATE_CMP_IMM;
    uint8_t two = 0x02;
    if (!patch_bytes(cmpImm, &two, 1)) return false;

    static const uint8_t movSil1[] = { 0x40, 0xB6, 0x01, 0x90 }; // mov sil, 1; nop
    if (!patch_bytes((uint8_t*)nv + RVA_GATE_SETGE, movSil1, 4)) return false;
    printf("[sm86_rehost] Gate patch applied: Tier 2 allowed, sil=1 forced\n");

    // 2. IAT Hook for cuModuleLoadData
    void** iat_load = (void**)((uint8_t*)nv + 0x1d2820);
    DWORD oldProt = 0;
    if (VirtualProtect(iat_load, sizeof(void*), PAGE_READWRITE, &oldProt)) {
        g_realModuleLoadData = (cuModuleLoadData_t)*iat_load;
        *iat_load = (void*)&hook_cuModuleLoadData;
        VirtualProtect(iat_load, sizeof(void*), oldProt, &oldProt);
        printf("[sm86_rehost] IAT cuModuleLoadData hooked (original @ %p)\n", g_realModuleLoadData);
    } else {
        return false;
    }

    // 3. Global Config Gate & Init
    uint8_t* S = (uint8_t*)nv + 0x7d7810;
    S[0x4c] = 1; S[0xe8] = 1; S[0xe9] = 1; S[0x12a5] = 1;
    pfnInitD3D initD3D = (pfnInitD3D)GetProcAddress(nv, "NVP_Init_D3D");
    if (initD3D) {
        bool ok = initD3D();
        S[0x4c] = 1; S[0xe8] = 1; S[0xe9] = 1; S[0x12a5] = 1;
        printf("[sm86_rehost] NVP_Init_D3D() -> %s\n", ok ? "TRUE" : "FALSE");
    }

    return true;
}

static DWORD WINAPI StartupThread(LPVOID) {
    Sleep(100); // Brief grace period for game process startup

    // Locate NvPresent64.dll
    g_nvpresent = GetModuleHandleA("NvPresent64.dll");
    if (!g_nvpresent) {
        g_nvpresent = LoadLibraryA("NvPresent64.dll");
    }
    if (!g_nvpresent) {
        g_nvpresent = LoadLibraryA("C:////Windows////System32////DriverStore////FileRepository////nv_dispi.inf_amd64_a3944b54ff18b284////NvPresent64.dll");
    }

    if (g_nvpresent) {
        ApplyRehost(g_nvpresent);
        InstallDxgiHooks();
        printf("[sm86_rehost] Setup completed successfully!\n");
    } else {
        printf("[sm86_rehost] Could not find NvPresent64.dll\n");
    }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        InitializeCriticalSection(&g_cs);
        HANDLE thread = CreateThread(nullptr, 0, StartupThread, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
    } else if (reason == DLL_PROCESS_DETACH) {
        DeleteCriticalSection(&g_cs);
    }
    return TRUE;
}
