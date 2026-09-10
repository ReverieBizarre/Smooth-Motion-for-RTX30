// ============================================================================
//  sm86_smooth - injected runtime
//
//  Ships as a `version.dll` placed in the game directory.  Windows resolves
//  `version.dll` out of the application directory before System32, so the game
//  loads us; every version API is forwarded to the real System32 DLL (see
//  version.def) so Version.dll consumers keep working.
//
//  Once inside the process we:
//    1. grab the DXGI vtables from a throwaway D3D11 device + swapchain
//    2. patch CreateSwapChainForHwnd so we can force the flip model, raise the
//       buffer count and enable tearing
//    3. patch IDXGISwapChain::Present / Present1
//    4. on each Present, insert one synthesised frame between the previous and
//       the current back buffer, exactly the way NvPresent64.dll does it
//
//  The synthesised frame is produced by VfiEngine (../vfi.cpp) on the game's
//  own D3D12 queue, so ordering against the game's rendering is free.
//
//  NOT FOR USE IN ONLINE GAMES.  This is DLL injection plus present-path
//  hooking; every anti-cheat will treat it as hostile, and rightly so.
// ============================================================================
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <windows.h>
#include <d3d12.h>
#include <d3d11.h>
#include <dxgi1_4.h>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <string>
#include <map>

#include "vfi.h"

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

namespace {

// ---------------------------------------------------------------------------
// config (sm86_smooth.ini next to the DLL, or next to the executable)
// ---------------------------------------------------------------------------
struct Config
{
    bool  enabled        = true;
    bool  temporal_aa    = true;
    int   radius_l3      = 0;      // 0 = auto
    int   radius_l2      = 4;
    int   radius_l1      = 4;
    int   radius_l0      = 2;
    float fb_thresh      = 1.5f;
    float hf_amount      = 1.0f;
    float w_bias         = 0.5f;
    float extrapolate    = 0.0f;   // >0 -> extrapolation past B by this many frames
    int   buffer_bump    = 2;
    int   max_in_flight  = 3;
    bool  log_enabled    = true;
};

static std::wstring g_dir;        // directory of this DLL
static Config       g_cfg;

static void readConfig()
{
    wchar_t ini[MAX_PATH * 2];
    wcsncpy_s(ini, g_dir.c_str(), _TRUNCATE);
    wcsncat_s(ini, L"sm86_smooth.ini", _TRUNCATE);
    if (GetFileAttributesW(ini) == INVALID_FILE_ATTRIBUTES)
        return;

    auto ri = [&](const wchar_t* k, int d) {
        return (int)GetPrivateProfileIntW(L"frame_gen", k, d, ini); };
    auto rf = [&](const wchar_t* k, float d) {
        wchar_t buf[64]; wchar_t def[64];
        swprintf_s(def, L"%g", d);
        GetPrivateProfileStringW(L"frame_gen", k, def, buf, 64, ini);
        return (float)_wtof(buf); };

    g_cfg.enabled     = ri(L"enabled", 1) != 0;
    g_cfg.temporal_aa = ri(L"temporal_aa", 1) != 0;
    g_cfg.radius_l3   = ri(L"radius_l3", 0);
    g_cfg.radius_l2   = ri(L"radius_l2", 4);
    g_cfg.radius_l1   = ri(L"radius_l1", 4);
    g_cfg.radius_l0   = ri(L"radius_l0", 2);
    g_cfg.fb_thresh   = rf(L"fb_thresh", 1.5f);
    g_cfg.hf_amount   = rf(L"hf_amount", 1.0f);
    g_cfg.w_bias      = rf(L"w_bias", 0.5f);
    g_cfg.extrapolate = rf(L"extrapolate", 0.0f);
    g_cfg.buffer_bump = ri(L"buffer_bump", 2);
    g_cfg.max_in_flight = ri(L"max_in_flight", 3);
    g_cfg.log_enabled = ri(L"log", 1) != 0;
}

// ---------------------------------------------------------------------------
// logging: one JSONL record per line, so it is greppable and jq-able
// ---------------------------------------------------------------------------
static CRITICAL_SECTION g_logCs;
static FILE*            g_log = nullptr;

static void logOpen()
{
    if (!g_cfg.log_enabled) return;
    std::wstring d = g_dir + L"logs\\";
    CreateDirectoryW(d.c_str(), nullptr);
    wchar_t path[MAX_PATH * 2];
    swprintf_s(path, L"%snative_%lu.jsonl", d.c_str(), GetCurrentProcessId());
    _wfopen_s(&g_log, path, L"ab");
}

static void LOGF(const char* fmt, ...)
{
    if (!g_log) return;
    EnterCriticalSection(&g_logCs);
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(g_log, "{\"t\":\"%04d-%02d-%02dT%02d:%02d:%02d.%03d\",\"msg\":\"",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap; va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fprintf(g_log, "\"}\n");
    fflush(g_log);
    LeaveCriticalSection(&g_logCs);
}

// ---------------------------------------------------------------------------
// DXGI hooks
// ---------------------------------------------------------------------------
typedef HRESULT(STDMETHODCALLTYPE* Present_t)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* Present1_t)(IDXGISwapChain1*, UINT, UINT,
                                               const DXGI_PRESENT_PARAMETERS*);
typedef HRESULT(STDMETHODCALLTYPE* CreateSCForHwnd_t)(IDXGIFactory2*, IUnknown*, HWND,
                                                      const DXGI_SWAP_CHAIN_DESC1*,
                                                      const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*,
                                                      IDXGIOutput*, IDXGISwapChain1**);
typedef HRESULT(STDMETHODCALLTYPE* CreateSC_t)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*,
                                               IDXGISwapChain**);
typedef void(STDMETHODCALLTYPE* ExecLists_t)(ID3D12CommandQueue*, UINT,
                                             ID3D12CommandList* const*);

static Present_t           g_origPresent   = nullptr;
static Present1_t          g_origPresent1  = nullptr;
static CreateSCForHwnd_t   g_origCreateSC  = nullptr;
static ExecLists_t         g_origExecLists = nullptr;

static ID3D12CommandQueue* g_gameQueue = nullptr;   // first DIRECT queue we see
static IDXGISwapChain1*    g_dummySC1    = nullptr; // keeps the patched vtable alive

// ---------------------------------------------------------------------------
// per-swapchain runtime state
// ---------------------------------------------------------------------------
struct SwapState
{
    IDXGISwapChain3*   sc      = nullptr;
    ID3D12Device*      dev     = nullptr;
    sm86::VfiEngine*   engine  = nullptr;
    ID3D12Resource*    capPrev = nullptr;
    ID3D12Resource*    capCur  = nullptr;
    ID3D12Resource*    gen     = nullptr;
    ID3D12CommandAllocator* alloc[4] = {};
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence*       fence   = nullptr;
    UINT64             fenceVal = 0;
    UINT64             frame    = 0;
    UINT               bufferCount = 0;
    DXGI_FORMAT        format   = DXGI_FORMAT_UNKNOWN;
    UINT               width = 0, height = 0, slot = 0;
    bool               tearing = false;
    volatile LONG      ready   = 0;    // 0 init pending, 1 ready, -1 failed
    bool               warned  = false;
    ID3D12Resource*    bb[16] = {};
    D3D12_RESOURCE_STATES bbState[16];
};

static void registerSwapchain(IDXGISwapChain1* sc, const DXGI_SWAP_CHAIN_DESC1& d);



static CRITICAL_SECTION         g_cs;
static std::map<IDXGISwapChain*, SwapState*> g_swaps;
static volatile LONG            g_initStarted = 0;

static void releaseResources(SwapState* s)
{
    auto rel = [](ID3D12Resource*& r) { if (r) { r->Release(); r = nullptr; } };
    for (int i = 0; i < 16; ++i) rel(s->bb[i]);
    rel(s->capPrev); rel(s->capCur); rel(s->gen);
    for (int i = 0; i < 4; ++i)
        if (s->alloc[i]) { s->alloc[i]->Release(); s->alloc[i] = nullptr; }
    if (s->list)  { s->list->Release();  s->list  = nullptr; }
    if (s->fence) { s->fence->Release(); s->fence = nullptr; }
    if (s->engine) { s->engine->shutdown(); delete s->engine; s->engine = nullptr; }
    s->ready = false;
}

static ID3D12Resource* makeTex(ID3D12Device* dev, UINT w, UINT h, DXGI_FORMAT fmt)
{
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = w; rd.Height = h; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = fmt; rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ID3D12Resource* r = nullptr;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                            D3D12_RESOURCE_STATE_COMMON, nullptr,
                                            IID_PPV_ARGS(&r))))
        return nullptr;
    return r;
}

static bool ensureReady(SwapState* s);

// ---------------------------------------------------------------------------
// the frame-gen work for one Present
// ---------------------------------------------------------------------------
static void doFrameGen(SwapState* s, UINT syncInterval, UINT flags)
{
    auto& pc = g_cfg;
    const float t = (pc.extrapolate > 0.0f) ? (1.0f + pc.extrapolate) : 0.5f;

    const UINT i0 = s->sc->GetCurrentBackBufferIndex();
    if (i0 >= s->bufferCount) { s->sc->Present(syncInterval, flags); return; }

    ID3D12CommandAllocator* alloc = s->alloc[s->frame % pc.max_in_flight];
    if (s->fence->GetCompletedValue() < s->fenceVal) { /* let it ride */ }
    alloc->Reset();
    s->list->Reset(alloc, nullptr);

    // 1. capture the frame the game just finished
    {
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource   = s->bb[i0];
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        b.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
        s->list->ResourceBarrier(1, &b);
        s->list->CopyResource(s->capCur, s->bb[i0]);
        D3D12_RESOURCE_BARRIER b2 = b;
        // leave it as COPY_DEST: we are about to overwrite it with the
        // synthesised frame anyway
        b2.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b2.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
        s->list->ResourceBarrier(1, &b2);
    }

    // 2. synthesise
    const bool bgra = (s->format == DXGI_FORMAT_B8G8R8A8_UNORM ||
                       s->format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
    s->engine->record(s->list, s->slot,
                      s->capPrev, D3D12_RESOURCE_STATE_COPY_DEST,
                      s->capCur,  D3D12_RESOURCE_STATE_COPY_DEST,
                      s->gen,     D3D12_RESOURCE_STATE_COPY_DEST,
                      t, bgra);

    // 3. put it into the buffer we are about to present, then present it
    s->list->CopyResource(s->bb[i0], s->gen);
    {
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource   = s->bb[i0];
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
        s->list->ResourceBarrier(1, &b);
    }
    s->list->Close();

    ID3D12CommandQueue* q = g_gameQueue;
    if (!q) { s->sc->Present(syncInterval, flags); return; }
    q->ExecuteCommandLists(1, (ID3D12CommandList**)&s->list);

    const UINT f1 = s->tearing ? DXGI_PRESENT_ALLOW_TEARING : 0;
    s->sc->Present(0, f1);                       // -> the synthesised frame

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
    s->sc->Present(syncInterval, flags);         // -> the real frame

    std::swap(s->capPrev, s->capCur);
    ++s->frame;
    ++s->slot;
    if (s->slot >= 3) s->slot = 0;

    const UINT64 v = ++s->fenceVal;
    q->Signal(s->fence, v);
    if ((s->frame & 63) == 0)
        LOGF("frame %llu  %ux%u  fmt %d", (unsigned long long)s->frame,
             s->width, s->height, (int)s->format);
}

// ---------------------------------------------------------------------------
static HRESULT STDMETHODCALLTYPE hookPresent(IDXGISwapChain* sc, UINT sync, UINT flags)
{
    SwapState* st = nullptr;
    EnterCriticalSection(&g_cs);
    auto it = g_swaps.find(sc);
    if (it != g_swaps.end()) st = it->second;
    LeaveCriticalSection(&g_cs);

    if (!g_cfg.enabled || !st) return g_origPresent(sc, sync, flags);
    if (!ensureReady(st))     return g_origPresent(sc, sync, flags);
    doFrameGen(st, sync, flags);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE hookPresent1(IDXGISwapChain1* sc, UINT sync, UINT flags,
                                              const DXGI_PRESENT_PARAMETERS* pp)
{
    SwapState* st = nullptr;
    EnterCriticalSection(&g_cs);
    auto it = g_swaps.find(sc);
    if (it != g_swaps.end()) st = it->second;
    LeaveCriticalSection(&g_cs);

    if (!g_cfg.enabled || !st) return g_origPresent1(sc, sync, flags, pp);
    if (!ensureReady(st))     return g_origPresent1(sc, sync, flags, pp);
    doFrameGen(st, sync, flags);
    return S_OK;
}

// ---------------------------------------------------------------------------
static HRESULT STDMETHODCALLTYPE hookCreateSCForHwnd(IDXGIFactory2* f, IUnknown* dev, HWND hwnd,
                                                     const DXGI_SWAP_CHAIN_DESC1* desc,
                                                     const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs,
                                                     IDXGIOutput* out, IDXGISwapChain1** ppSC)
{
    DXGI_SWAP_CHAIN_DESC1 d = *desc;

    // We need at least 3 buffers because each game frame consumes two
    // presentations (the synthesised one, then the real one).
    const UINT want = (d.BufferCount < 3 ? 3 : d.BufferCount) + (UINT)g_cfg.buffer_bump;
    if (want > 16) d.BufferCount = 16; else d.BufferCount = want;

    if (d.SwapEffect != DXGI_SWAP_EFFECT_FLIP_DISCARD &&
        d.SwapEffect != DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL)
        d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    d.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    if (d.SampleDesc.Count != 1) { d.SampleDesc.Count = 1; d.SampleDesc.Quality = 0; }

    LOGF("CreateSwapChainForHwnd: %ux%u fmt %d buffers %u -> %u effect %d",
         d.Width, d.Height, (int)d.Format, desc->BufferCount, d.BufferCount, (int)d.SwapEffect);

    HRESULT hr = g_origCreateSC(f, dev, hwnd, &d, fs, out, ppSC);
    if (SUCCEEDED(hr) && ppSC && *ppSC) registerSwapchain(*ppSC, d);
    return hr;
}

static void STDMETHODCALLTYPE hookExecLists(ID3D12CommandQueue* q, UINT n,
                                            ID3D12CommandList* const* lists)
{
    if (!g_gameQueue && q && q->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT)
    {
        g_gameQueue = q;
        q->AddRef();
        LOGF("captured the game's DIRECT command queue");
    }
    if (g_origExecLists) g_origExecLists(q, n, lists);
}

} // namespace

// ---------------------------------------------------------------------------
// vtable patching (no Detours dependency - we only need five entries)
// ---------------------------------------------------------------------------
static void patchSlot(void** vtbl, UINT index, void* fn, void** outOrig)
{
    DWORD old = 0;
    VirtualProtect(&vtbl[index], sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
    if (outOrig) *outOrig = vtbl[index];
    vtbl[index] = fn;
    VirtualProtect(&vtbl[index], sizeof(void*), old, &old);
    FlushInstructionCache(GetCurrentProcess(), &vtbl[index], sizeof(void*));
}

// ---------------------------------------------------------------------------
// swapchain registration + lazy engine bring-up
// ---------------------------------------------------------------------------
namespace {

static void registerSwapchain(IDXGISwapChain1* sc, const DXGI_SWAP_CHAIN_DESC1& d)
{
    IDXGISwapChain3* sc3 = nullptr;
    if (FAILED(sc->QueryInterface(IID_PPV_ARGS(&sc3)))) return;

    ID3D12Resource* bb0 = nullptr;
    if (FAILED(sc3->GetBuffer(0, IID_PPV_ARGS(&bb0)))) { sc3->Release(); return; }
    ID3D12Device* dev = nullptr;
    if (FAILED(bb0->GetDevice(IID_PPV_ARGS(&dev)))) { bb0->Release(); sc3->Release(); return; }
    bb0->Release();
    dev->Release();                       // owned by the resource, we re-get it later

    SwapState* s = new SwapState();
    s->sc = sc3;
    s->format = d.Format;
    s->width = d.Width; s->height = d.Height;
    s->bufferCount = d.BufferCount;
    s->tearing = (d.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) != 0;

    bb0 = nullptr;
    if (FAILED(sc3->GetBuffer(0, IID_PPV_ARGS(&bb0)))) { releaseResources(s); delete s; return; }
    if (FAILED(bb0->GetDevice(IID_PPV_ARGS(&s->dev)))) { bb0->Release(); releaseResources(s); delete s; return; }
    bb0->Release();

    for (UINT i = 0; i < s->bufferCount && i < 16; ++i)
    {
        if (FAILED(sc3->GetBuffer(i, IID_PPV_ARGS(&s->bb[i]))))
        {
            LOGF("GetBuffer(%u) failed", i);
            releaseResources(s); delete s; return;
        }
        s->bbState[i] = D3D12_RESOURCE_STATE_PRESENT;
    }
    for (int i = 0; i < 4; ++i)
        s->dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&s->alloc[i]));
    s->dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s->alloc[0], nullptr,
                              IID_PPV_ARGS(&s->list));
    s->list->Close();
    s->dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&s->fence));

    s->capPrev = makeTex(s->dev, d.Width, d.Height, d.Format);
    s->capCur  = makeTex(s->dev, d.Width, d.Height, d.Format);
    s->gen     = makeTex(s->dev, d.Width, d.Height, d.Format);
    if (!s->capPrev || !s->capCur || !s->gen)
    {
        LOGF("capture texture allocation failed");
        releaseResources(s); delete s; return;
    }

    EnterCriticalSection(&g_cs);
    g_swaps[sc] = s;
    LeaveCriticalSection(&g_cs);
    LOGF("swapchain registered: %ux%u fmt %d bufferCount %u tearing %d",
         d.Width, d.Height, (int)d.Format, d.BufferCount, s->tearing ? 1 : 0);
}

static DWORD WINAPI initThread(LPVOID p)
{
    SwapState* s = (SwapState*)p;

    sm86::VfiOptions opt;
    opt.radius_l3   = g_cfg.radius_l3;
    opt.radius_l2   = g_cfg.radius_l2;
    opt.radius_l1   = g_cfg.radius_l1;
    opt.radius_l0   = g_cfg.radius_l0;
    opt.fb_thresh   = g_cfg.fb_thresh;
    opt.hf_amount   = g_cfg.hf_amount;
    opt.w_bias      = g_cfg.w_bias;
    opt.temporal_aa = g_cfg.temporal_aa;

    std::wstring sp = g_dir + L"shaders\\vfi.hlsl";
    std::string err;

    LARGE_INTEGER a, b, f; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&a);
    sm86::VfiEngine* e = new sm86::VfiEngine();
    if (e->init(s->dev, s->format, s->width, s->height, opt, sp.c_str(), &err))
    {
        QueryPerformanceCounter(&b);
        s->engine = e;
        InterlockedExchange(&s->ready, 1);
        LOGF("engine ready in %lld ms (%u dispatches/frame, L3 reach +-%d px)",
             (long long)((b.QuadPart - a.QuadPart) * 1000 / f.QuadPart),
             e->dispatch_count(), e->effective_radius_l3() * 8);
    }
    else
    {
        delete e;
        LOGF("engine init failed: %s", err.c_str());
        InterlockedExchange(&s->ready, -1);
    }
    return 0;
}

static bool ensureReady(SwapState* s)
{
    if (s->ready == 1) return true;
    if (s->ready < 0)  return false;
    if (InterlockedCompareExchange(&g_initStarted, 1, 0) != 0) return false;
    HANDLE h = CreateThread(nullptr, 0, initThread, s, 0, nullptr);
    if (h) CloseHandle(h);
    return false;
}

// ---------------------------------------------------------------------------
// hook installation
// ---------------------------------------------------------------------------
static void installHooks()
{
    // A throwaway window + D3D11 device is enough: we only need the vtables,
    // which are shared by every instance of the interface.
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"sm86_smooth_dummy";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, L"sm86_smooth_dummy", L"", WS_OVERLAPPEDWINDOW,
                                0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);

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

    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                               nullptr, 0, D3D11_SDK_VERSION, &sd, &sc,
                                               &dev11, &lvl, &ctx);
    if (FAILED(hr) || !sc)
    {
        LOGF("dummy swapchain creation failed 0x%08lx - hooks not installed", (unsigned long)hr);
        return;
    }

    if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&g_dummySC1))))
    {
        void** vt = *(void***)g_dummySC1;
        patchSlot(vt, 8,  (void*)hookPresent,  (void**)&g_origPresent);
        patchSlot(vt, 22, (void*)hookPresent1, (void**)&g_origPresent1);
        LOGF("patched IDXGISwapChain::Present (slot 8) and Present1 (slot 22)");
    }

    {
        IDXGIDevice* dxdev = nullptr;
        IDXGIAdapter* ad = nullptr;
        IDXGIFactory2* fac = nullptr;
        if (dev11 && SUCCEEDED(dev11->QueryInterface(IID_PPV_ARGS(&dxdev))) &&
            SUCCEEDED(dxdev->GetAdapter(&ad)) &&
            SUCCEEDED(ad->GetParent(IID_PPV_ARGS(&fac))))
        {
            void** vt = *(void***)fac;
            patchSlot(vt, 15, (void*)hookCreateSCForHwnd, (void**)&g_origCreateSC);
            LOGF("patched IDXGIFactory2::CreateSwapChainForHwnd (slot 15)");
        }
        if (fac)  fac->Release();
        if (ad)   ad->Release();
        if (dxdev) dxdev->Release();
    }

    {
        ID3D12Device* d12 = nullptr;
        if (SUCCEEDED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d12))))
        {
            D3D12_COMMAND_QUEUE_DESC qd = {};
            qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            ID3D12CommandQueue* q = nullptr;
            if (SUCCEEDED(d12->CreateCommandQueue(&qd, IID_PPV_ARGS(&q))))
            {
                void** vt = *(void***)q;
                patchSlot(vt, 10, (void*)hookExecLists, (void**)&g_origExecLists);
                LOGF("patched ID3D12CommandQueue::ExecuteCommandLists (slot 10)");
                q->Release();
            }
            d12->Release();
        }
    }

    if (g_dummySC1) { g_dummySC1->Release(); g_dummySC1 = nullptr; }
    if (ctx)   ctx->Release();
    if (dev11) dev11->Release();
    if (sc)    sc->Release();
    if (hwnd)  DestroyWindow(hwnd);
}

static DWORD WINAPI startupThread(LPVOID)
{
    installHooks();
    return 0;
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hInst);
        InitializeCriticalSection(&g_cs);
        InitializeCriticalSection(&g_logCs);

        wchar_t p[MAX_PATH * 2];
        GetModuleFileNameW(hInst, p, MAX_PATH * 2);
        wchar_t* slash = wcsrchr(p, L'\\');
        if (slash) *slash = 0;
        g_dir = p; g_dir += L'\\';

        readConfig();
        logOpen();
        LOGF("sm86_smooth attached (pid %lu, enabled %d, extrapolate %g)",
             GetCurrentProcessId(), g_cfg.enabled ? 1 : 0, g_cfg.extrapolate);

        // keep the loader lock clear: creating a D3D device inside DllMain can
        // deadlock, so all of it happens on a private thread
        HANDLE h = CreateThread(nullptr, 0, startupThread, nullptr, 0, nullptr);
        if (h) CloseHandle(h);
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        EnterCriticalSection(&g_cs);
        for (auto& kv : g_swaps) { releaseResources(kv.second); delete kv.second; }
        g_swaps.clear();
        LeaveCriticalSection(&g_cs);
        if (g_gameQueue) { g_gameQueue->Release(); g_gameQueue = nullptr; }
        LOGF("sm86_smooth detached");
        if (g_log) { fclose(g_log); g_log = nullptr; }
    }
    return TRUE;
}
