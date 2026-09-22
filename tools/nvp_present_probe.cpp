// ============================================================================
//  tools/nvp_present_probe.cpp - Minimal D3D12 app that loads the proxy DLL
//  (version.dll next to the executable, or NVP_TEST_PROXY) and presents a
//  moving pattern. Lets the proxy be exercised without a game: the proxy log
//  shows whether NvPresent wrapped the swapchain and launched its graphs.
//
//  Knobs (env): NVP_TEST_FMT (28=R8G8B8A8, 24=R10G10B10A2), NVP_TEST_W/H,
//               NVP_TEST_FLAGS (0x802 = ALLOW_TEARING|ALLOW_MODE_SWITCH),
//               NVP_TEST_FRAMES (default 240), NVP_TEST_SYNC (default 0),
//               NVP_TEST_PRESENT_FLAGS (default 0x200 = ALLOW_TEARING when sync=0).
// ============================================================================
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <cstdio>
#include <cstdlib>
#include <cstdint>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "user32.lib")

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);

    char self[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, self, MAX_PATH);
    char* slash = strrchr(self, '\\');
    const char* proxyName = getenv("NVP_TEST_PROXY");
    if (slash) strcpy_s(slash + 1, MAX_PATH - (slash + 1 - self), proxyName && *proxyName ? proxyName : "version.dll");
    HMODULE proxy = LoadLibraryA(self);
    printf("[+] proxy %s -> %p (err %lu)\n", self, proxy, proxy ? 0 : GetLastError());
    if (!proxy) return 1;
    Sleep(2000); // let the proxy's StartupThread install NvPresent + hooks

    int TW = 512, TH = 512, frames = 240; UINT sync = 0, pflags = 0x200;
    DXGI_FORMAT fmt = DXGI_FORMAT_R8G8B8A8_UNORM; UINT sflags = 0;
    if (const char* e = getenv("NVP_TEST_W")) TW = atoi(e);
    if (const char* e = getenv("NVP_TEST_H")) TH = atoi(e);
    if (const char* e = getenv("NVP_TEST_FMT")) fmt = (DXGI_FORMAT)atoi(e);
    if (const char* e = getenv("NVP_TEST_FLAGS")) sflags = (UINT)strtoul(e, nullptr, 0);
    if (const char* e = getenv("NVP_TEST_FRAMES")) frames = atoi(e);
    if (const char* e = getenv("NVP_TEST_SYNC")) sync = (UINT)atoi(e);
    if (const char* e = getenv("NVP_TEST_PRESENT_FLAGS")) pflags = (UINT)strtoul(e, nullptr, 0);
    if (sync) pflags &= ~0x200u;
    if (!(sflags & 0x800)) pflags &= ~0x200u; // ALLOW_TEARING present flag is only legal on a tearing-capable swapchain
    printf("[+] swapchain %dx%d fmt %d flags 0x%X; present sync=%u flags=0x%X; %d frames\n", TW, TH, (int)fmt, sflags, sync, pflags, frames);

    ID3D12Device* dev = nullptr;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev)))) { printf("[!] D3D12CreateDevice failed\n"); return 1; }
    ID3D12CommandQueue* cq = nullptr;
    D3D12_COMMAND_QUEUE_DESC qd = {}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&cq));

    WNDCLASSA wc = {}; wc.lpfnWndProc = DefWindowProcA; wc.hInstance = GetModuleHandleA(nullptr); wc.lpszClassName = "nvp_present_probe";
    RegisterClassA(&wc);
    HWND wnd = CreateWindowA("nvp_present_probe", "NvPresent Present Probe", WS_POPUP, 0, 0, TW, TH, nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(wnd, SW_SHOWNOACTIVATE);

    IDXGIFactory4* f = nullptr; CreateDXGIFactory1(IID_PPV_ARGS(&f));
    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width = TW; sd.Height = TH; sd.Format = fmt; sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = 3;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD; sd.Flags = sflags;
    IDXGISwapChain1* swap = nullptr;
    HRESULT hr = f->CreateSwapChainForHwnd(cq, wnd, &sd, nullptr, nullptr, &swap);
    if (FAILED(hr) || !swap) { printf("[!] CreateSwapChainForHwnd failed 0x%08X\n", (uint32_t)hr); return 1; }
    IDXGISwapChain3* swap3 = nullptr; swap->QueryInterface(IID_PPV_ARGS(&swap3));

    ID3D12CommandAllocator* ca = nullptr; dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&ca));
    ID3D12GraphicsCommandList* cl = nullptr; dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, ca, nullptr, IID_PPV_ARGS(&cl)); cl->Close();
    ID3D12DescriptorHeap* heap = nullptr;
    D3D12_DESCRIPTOR_HEAP_DESC hd = {}; hd.NumDescriptors = 3; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap));
    SIZE_T inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    ID3D12Resource* bb[3] = {};
    for (UINT i = 0; i < 3; ++i) {
        swap->GetBuffer(i, IID_PPV_ARGS(&bb[i]));
        D3D12_CPU_DESCRIPTOR_HANDLE h = heap->GetCPUDescriptorHandleForHeapStart(); h.ptr += i * inc;
        dev->CreateRenderTargetView(bb[i], nullptr, h);
    }
    ID3D12Fence* fence = nullptr; dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    HANDLE ev = CreateEventA(nullptr, FALSE, FALSE, nullptr); UINT64 fv = 0;

    for (int frame = 0; frame < frames; ++frame) {
        UINT idx = swap3 ? swap3->GetCurrentBackBufferIndex() : (UINT)(frame % 3);
        ca->Reset(); cl->Reset(ca, nullptr);
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = bb[idx];
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT; b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cl->ResourceBarrier(1, &b);
        D3D12_CPU_DESCRIPTOR_HANDLE h = heap->GetCPUDescriptorHandleForHeapStart(); h.ptr += idx * inc;
        float bg[4] = { 0.10f, 0.12f, 0.35f, 1.0f };
        cl->ClearRenderTargetView(h, bg, 0, nullptr);
        int bx = (frame * 7) % (TW - 120);
        D3D12_RECT r = { bx, TH / 3, bx + 120, TH / 3 + 120 };
        float white[4] = { 1, 1, 1, 1 };
        cl->ClearRenderTargetView(h, white, 1, &r);
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET; b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        cl->ResourceBarrier(1, &b);
        cl->Close();
        ID3D12CommandList* lists[] = { cl }; cq->ExecuteCommandLists(1, lists);
        cq->Signal(fence, ++fv);
        if (fence->GetCompletedValue() < fv) { fence->SetEventOnCompletion(fv, ev); WaitForSingleObject(ev, 2000); }
        HRESULT ph = swap->Present(sync, pflags);
        if (frame % 60 == 0) printf("[frame %d] Present -> 0x%08X\n", frame, (uint32_t)ph);
        MSG msg; while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageA(&msg);
        Sleep(16);
    }
    printf("[+] done\n");
    return 0;
}
