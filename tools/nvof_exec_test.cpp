// ============================================================================
//  nvof_exec_test.cpp - drive NVIDIA hardware optical flow (NVOFA) on an
//  RTX 3080 via the reconstructed D3D12 interface, and MEASURE it.
//
//  Builds on tools/nvof_d3d12_probe.cpp (device + CreateInstance + Init).
//  Pipeline:
//    1. D3D12 device + NV_OF handle + Init(grid, FAST)
//    2. query OUTPUT surface format (so we create the flow texture right)
//    3. two 1920x1080 R8_UNORM textures, frame B = frame A shifted RIGHT 8px
//    4. register inputs (INPUT) + output (OUTPUT), create a GPU fence
//    5. Execute 60x with input/output fence points; wait on the out-fence
//    6. read back the flow, decode S10.5, median dx/dy over a central region
//    7. time just Execute(+fence wait), report ms/frame
//
//  Every API call prints its numeric status; on failure we keep going so a
//  partial result + a precise failure point is still produced.
// ============================================================================
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#undef WIN32_LEAN_AND_MEAN
#include <d3d12.h>
#include <dxgi1_4.h>
#include <stdio.h>
#include <stdint.h>
#include <vector>
#include <algorithm>
#include <numeric>
#include "nvof/nvofapi.h"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

// Minimal CD3DX12 helpers (d3dx12.h is not shipped with this SDK path).
struct CD3DX12_HEAP_PROPERTIES : D3D12_HEAP_PROPERTIES {
    CD3DX12_HEAP_PROPERTIES() = default;
    CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE t) {
        Type = t; CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN; CreationNodeMask = 1; VisibleNodeMask = 1;
    }
};
struct CD3DX12_RESOURCE_DESC : D3D12_RESOURCE_DESC {
    CD3DX12_RESOURCE_DESC() = default;
    static CD3DX12_RESOURCE_DESC Buffer(UINT64 size) {
        CD3DX12_RESOURCE_DESC d = {};
        d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = size; d.Height = 1;
        d.DepthOrArraySize = 1; d.MipLevels = 1; d.Format = DXGI_FORMAT_UNKNOWN;
        d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        d.Flags = D3D12_RESOURCE_FLAG_NONE; return d;
    }
};
struct CD3DX12_RESOURCE_BARRIER : D3D12_RESOURCE_BARRIER {
    static CD3DX12_RESOURCE_BARRIER MakeTransition(ID3D12Resource* p,
            D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after, UINT sub = 0) {
        CD3DX12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        b.Transition.pResource = p; b.Transition.Subresource = sub;
        b.Transition.StateBefore = before; b.Transition.StateAfter = after; return b;
    }
};

static const char* sname(int s) {
    switch (s) {
    case 0: return "NV_OF_SUCCESS";
    case 1: return "NV_OF_ERR_OF_NOT_AVAILABLE";
    case 2: return "NV_OF_ERR_UNSUPPORTED_DEVICE";
    case 3: return "NV_OF_ERR_DEVICE_DOES_NOT_EXIST";
    case 4: return "NV_OF_ERR_INVALID_PTR";
    case 5: return "NV_OF_ERR_INVALID_PARAM";
    case 6: return "NV_OF_ERR_INVALID_CALL";
    case 7: return "NV_OF_ERR_INVALID_VERSION";
    case 8: return "NV_OF_ERR_OUT_OF_MEMORY";
    case 9: return "NV_OF_ERR_NOT_INITIALIZED";
    case 10: return "NV_OF_ERR_UNSUPPORTED_FEATURE";
    case 11: return "NV_OF_ERR_GENERIC";
    default: return "?";
    }
}
static void perr(const char* step, int s) {
    printf("  [FAIL] %-28s -> %d %s\n", step, s, sname(s));
}
static void pchk(const char* step, int s) {
    if (s != 0) perr(step, s);
    else printf("  [ok]   %s\n", step);
}

static ID3D12Device*            g_dev = nullptr;
static ID3D12CommandQueue*      g_q   = nullptr;
static ID3D12CommandAllocator*  g_alloc = nullptr;
static ID3D12GraphicsCommandList* g_cl = nullptr;
static ID3D12Fence*             g_fence = nullptr;
static HANDLE                   g_fenceEv = nullptr;
static UINT64                   g_fenceVal = 0;

static void waitFence(UINT64 val) {
    if (g_fence->GetCompletedValue() >= val) return;
    g_fence->SetEventOnCompletion(val, g_fenceEv);
    WaitForSingleObject(g_fenceEv, INFINITE);
}
static void submitAndWait() {
    g_q->ExecuteCommandLists(1, (ID3D12CommandList* const*)&g_cl);
    g_fenceVal++;
    g_q->Signal(g_fence, g_fenceVal);
    waitFence(g_fenceVal);
}

// Create a DEFAULT-heap texture and upload `data` (row-major, w*bpp bytes/row).
static ID3D12Resource* makeTexture(DXGI_FORMAT fmt, int w, int h,
                                   const void* data, int bpp,
                                   D3D12_RESOURCE_STATES after,
                                   const char* tag) {
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Alignment = 0; rd.Width = w; rd.Height = h; rd.DepthOrArraySize = 1;
    rd.MipLevels = 1; rd.Format = fmt; rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN; rd.Flags = D3D12_RESOURCE_FLAG_NONE;

    ID3D12Resource* tex = nullptr;
    printf("  [mk] enter %s fmt=%d %dx%d data=%p bpp=%d\n", tag, (int)fmt, w, h, data, bpp);
    HRESULT hr = g_dev->CreateCommittedResource(
        &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT), D3D12_HEAP_FLAG_NONE,
        &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&tex));
    if (FAILED(hr)) { printf("  makeTexture(%s) CreateCommittedResource FAILED %#x\n", tag, hr); return nullptr; }

    if (data) {
        // ---- upload path: build a staging buffer and copy into the texture ----
        D3D12_RESOURCE_DESC src = rd;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT foot;
        UINT nrows; UINT64 rowPitch; UINT64 total;
        g_dev->GetCopyableFootprints(&src, 0, 1, 0, &foot, &nrows, &rowPitch, &total);

        ID3D12Resource* up = nullptr;
        hr = g_dev->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD), D3D12_HEAP_FLAG_NONE,
            &CD3DX12_RESOURCE_DESC::Buffer(total), D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr, IID_PPV_ARGS(&up));
        if (FAILED(hr)) { tex->Release(); printf("  makeTexture(%s) upload alloc FAILED %#x\n", tag, hr); return nullptr; }

        void* mapped = nullptr;
        up->Map(0, nullptr, &mapped);
        const uint8_t* srcRows = (const uint8_t*)data;
        uint8_t* dst = (uint8_t*)mapped;
        for (UINT y = 0; y < nrows; y++) {
            memcpy(dst + y*rowPitch, srcRows + y*(UINT)w*bpp, (UINT)w*bpp);
        }
        up->Unmap(0, nullptr);

        g_alloc->Reset();
        g_cl->Reset(g_alloc, nullptr);
        D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
        dstLoc.pResource = tex; dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dstLoc.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
        srcLoc.pResource = up; srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; srcLoc.PlacedFootprint = foot;
        g_cl->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);
        if (after != D3D12_RESOURCE_STATE_COPY_DEST) {
            CD3DX12_RESOURCE_BARRIER bar = CD3DX12_RESOURCE_BARRIER::MakeTransition(tex, D3D12_RESOURCE_STATE_COPY_DEST, after);
            g_cl->ResourceBarrier(1, &bar);
        }
        g_cl->Close();
        submitAndWait();
        up->Release();
    } else {
        // ---- no initial data (e.g. an NVOFA output buffer): just transition to `after` ----
        if (after != D3D12_RESOURCE_STATE_COPY_DEST) {
            g_alloc->Reset();
            g_cl->Reset(g_alloc, nullptr);
            CD3DX12_RESOURCE_BARRIER bar = CD3DX12_RESOURCE_BARRIER::MakeTransition(tex, D3D12_RESOURCE_STATE_COPY_DEST, after);
            g_cl->ResourceBarrier(1, &bar);
            g_cl->Close();
            submitAndWait();
        }
    }
    return tex;
}

// Copy a DEFAULT texture to a READBACK buffer and return mapped uint8* (caller frees via readback->Release after use).
static ID3D12Resource* readbackTexture(ID3D12Resource* tex, int w, int h, int bpp, void** out, UINT64* outRowPitch) {
    D3D12_RESOURCE_DESC src = tex->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT foot;
    UINT nrows; UINT64 rowPitch; UINT64 total;
    g_dev->GetCopyableFootprints(&src, 0, 1, 0, &foot, &nrows, &rowPitch, &total);
    *outRowPitch = rowPitch;

    ID3D12Resource* rb = nullptr;
    HRESULT hr = g_dev->CreateCommittedResource(
        &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK), D3D12_HEAP_FLAG_NONE,
        &CD3DX12_RESOURCE_DESC::Buffer(total), D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr, IID_PPV_ARGS(&rb));
    if (FAILED(hr)) { printf("  readback alloc FAILED %#x\n", hr); return nullptr; }

    g_alloc->Reset();
    g_cl->Reset(g_alloc, nullptr);
    CD3DX12_RESOURCE_BARRIER bar = CD3DX12_RESOURCE_BARRIER::MakeTransition(tex, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    g_cl->ResourceBarrier(1, &bar);
    D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
    dstLoc.pResource = rb; dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dstLoc.PlacedFootprint = foot;
    D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
    srcLoc.pResource = tex; srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; srcLoc.SubresourceIndex = 0;
    g_cl->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);
    g_cl->Close();
    submitAndWait();

    void* mapped = nullptr;
    rb->Map(0, nullptr, &mapped);
    *out = mapped;
    return rb;
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);

    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g_dev)))) {
        printf("D3D12CreateDevice failed\n"); return 1;
    }
    D3D12_COMMAND_QUEUE_DESC qd = {}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT; qd.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    if (FAILED(g_dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_q)))) { printf("CreateCommandQueue failed\n"); return 1; }
    if (FAILED(g_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_alloc)))) { printf("CreateCommandAllocator failed\n"); return 1; }
    if (FAILED(g_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_alloc, nullptr, IID_PPV_ARGS(&g_cl)))) { printf("CreateCommandList failed\n"); return 1; }
    if (FAILED(g_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)))) { printf("CreateFence failed\n"); return 1; }
    g_fenceEv = CreateEvent(nullptr, FALSE, FALSE, nullptr);

    HMODULE h = LoadLibraryA("nvofapi64.dll");
    if (!h) { printf("LoadLibrary nvofapi64.dll failed\n"); return 1; }
    auto pMax  = (PFN_NVOF_GET_MAX_SUPPORTED_API_VERSION)GetProcAddress(h, "NvOFGetMaxSupportedApiVersion");
    auto pInst = (PFN_NVOF_CREATE_INSTANCE_D3D12)GetProcAddress(h, "NvOFAPICreateInstanceD3D12");

    uint32_t ver = 0; int s = pMax ? pMax(&ver) : -1;
    printf("max api version: %#x\n", ver);

    NV_OF_D3D12_API_FUNCTION_LIST L = {};
    s = pInst(ver, &L);
    if (s != 0) { printf("CreateInstanceD3D12 %d\n", s); return 1; }

    NvOFHandle hOf = nullptr;
    s = L.nvCreateOpticalFlowD3D12(g_dev, &hOf);
    pchk("nvCreateOpticalFlowD3D12", s);
    if (s != 0) return 1;

    // ---- Init (grid size chosen below; try 4 then 1) ----
    auto doInit = [&](uint32_t grid) -> bool {
        alignas(16) unsigned char buf[256] = {};
        auto d = [&](size_t o, uint32_t v) { memcpy(buf+o, &v, 4); };
        d(0x00, 1920); d(0x04, 1080); d(0x08, grid); d(0x0c, grid);
        d(0x10, 1); d(0x14, (uint32_t)NV_OF_PERF_LEVEL_FAST);
        int r = L.nvOFInit(hOf, (const NV_OF_INIT_PARAMS*)buf);
        printf("  Init grid=%u : %d %s\n", grid, r, sname(r));
        return r == 0;
    };
    uint32_t grid = 4;
    if (!doInit(grid)) {
        printf("  -> trying grid=1\n");
        grid = 1;
        if (!doInit(grid)) { printf("  Init failed for both grids; aborting exec test\n"); L.nvOFDestroy(hOf); return 1; }
    }
    printf("  using grid size = %u\n", grid);

    // ---- print the actual function-list shim addresses (RVAs) ----
    printf("  [fn] register shim RVA %#x  execute shim RVA %#x\n",
           (unsigned)((uintptr_t)L.nvOFRegisterResourceD3D12 - 0x180000000),
           (unsigned)((uintptr_t)L.nvOFExecuteD3D12 - 0x180000000));

    // ---- query OUTPUT surface format ----
    // NOTE: calling nvOFGetSurfaceFormatCount/GetSurfaceFormatD3D12 with
    // BUFFER_USAGE_OUTPUT crashes this process (stack corruption inside the
    // driver writeback). Hardcode R32_UINT (the first advertised OUTPUT format
    // when the call succeeded once) and proceed.
    DXGI_FORMAT outFmt = DXGI_FORMAT_R32_UINT;
    printf("  [mark] skipping OUTPUT-format query (crash), outFmt=%d\n", (int)outFmt);
    if (outFmt == DXGI_FORMAT_UNKNOWN) { printf("  no OUTPUT format -> using R32_UINT fallback\n"); outFmt = DXGI_FORMAT_R32_UINT; }

    // ---- build the two input frames ----
    printf("  [mark] building frames...\n");
    const int W = 1920, H = 1080, SHIFT = 8;
    std::vector<uint8_t> A((size_t)W*H), B((size_t)W*H);
    srand(12345);
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        // smooth-ish noise: low-frequency via sin, plus a little randomness
        int v = (int)(128 + 80*sinf((float)(x*0.03 + y*0.02)) + 40*sinf((float)(x*0.011 - y*0.017)) + (rand()%21 - 10));
        A[y*W + x] = (uint8_t)(v & 0xFF);
        int xs = (x + SHIFT) % W;            // frame B = A shifted RIGHT by SHIFT px
        B[y*W + xs] = A[y*W + x];
    }
    printf("  [mark] frames built (A[0]=%d B[0]=%d)\n", (int)A[0], (int)B[0]);

    printf("  [mark] makeTexture inputA...\n");
    ID3D12Resource* texA = makeTexture(DXGI_FORMAT_R8_UNORM, W, H, A.data(), 1, D3D12_RESOURCE_STATE_COMMON, "inputA");
    printf("  [mark] inputA done=%p\n", (void*)texA);
    printf("  [mark] makeTexture inputB...\n");
    ID3D12Resource* texB = makeTexture(DXGI_FORMAT_R8_UNORM, W, H, B.data(), 1, D3D12_RESOURCE_STATE_COMMON, "inputB");
    printf("  [mark] inputB done=%p\n", (void*)texB);
    if (!texA || !texB) { printf("input texture creation failed\n"); L.nvOFDestroy(hOf); return 1; }

    int gw = W / (int)grid, gh = H / (int)grid;
    ID3D12Resource* texOut = makeTexture(outFmt, gw, gh, nullptr, 4, D3D12_RESOURCE_STATE_COMMON, "output");
    if (!texOut) { printf("output texture creation failed\n"); L.nvOFDestroy(hOf); return 1; }

    // ---- register resources ----
    auto reg = [&](ID3D12Resource* tex, NV_OF_BUFFER_USAGE usage, const char* tag) -> NvOFGPUBufferHandle {
        alignas(16) unsigned char rb[256] = {};
        memcpy(rb+0x00, &tex, 8);                 // pResource @0x00
        uint32_t bu = (uint32_t)usage; memcpy(rb+0x08, &bu, 4);  // bufferUsage @0x08
        // fence points @0x10 / @0x20 left zero (null fence, value 0)
        int r = L.nvOFRegisterResourceD3D12(hOf, (const void*)rb);
        printf("  RegisterResource(%s) : %d %s\n", tag, r, sname(r));
        if (r != 0) return nullptr;
        // Locate the returned handle: scan the (zeroed) params buffer for a qword
        // that is a plausible user-mode pointer and is not our input resource.
        NvOFGPUBufferHandle out = nullptr;
        uintptr_t texu = (uintptr_t)tex;
        for (int i = 0; i < 16; i++) {
            uintptr_t q; memcpy(&q, rb + i*8, 8);
            if (q && q != texu && q >= 0x10000 && q < 0x800000000000ULL) {
                // print every candidate so we can see the layout
                printf("      candidate handle @+0x%02x = %p\n", i*8, (void*)q);
                if (!out) out = (NvOFGPUBufferHandle)q;
            }
        }
        if (!out) { printf("      WARNING: no handle found in params buffer\n"); }
        return out;
    };

    NvOFGPUBufferHandle hInA = reg(texA, NV_OF_BUFFER_USAGE_INPUT, "inputA");
    NvOFGPUBufferHandle hInB = reg(texB, NV_OF_BUFFER_USAGE_INPUT, "inputB");
    NvOFGPUBufferHandle hOut = reg(texOut, NV_OF_BUFFER_USAGE_OUTPUT, "output");
    if (!hInA || !hInB || !hOut) {
        printf("  registration incomplete (hInA=%p hInB=%p hOut=%p). Attempting execute anyway to learn the failure point.\n", hInA, hInB, hOut);
    }

    // ---- execute ----
    alignas(16) unsigned char inb[256] = {};
    alignas(16) unsigned char outb[128] = {};
    // execute input: commonParams @0, inputFencePoint[N] @0x38
    auto di = [&](size_t o, uint64_t v, int n) { memcpy(inb+o, &v, n); };
    auto do_ = [&](size_t o, uint64_t v, int n) { memcpy(outb+o, &v, n); };
    memcpy(inb+0x00, &hInA, 8);    // inputFrame
    memcpy(inb+0x08, &hInB, 8);    // referenceFrame
    // externalHints @0x10 = 0 (already zero), disableTemporalHints @0x18 = 1
    uint32_t dt = 1; memcpy(inb+0x18, &dt, 4);
    // hPrivData/numRois/roiData zero

    memcpy(outb+0x00, &hOut, 8);   // outputBuffer
    // costBuffer @0x08 = 0

    // Set fence points: all input fence points wait on current completed value
    // (already passed); output fence point signals the next value.
    UINT64 outVal = g_fenceVal + 1;
    for (int i = 0; i < NV_OF_MAX_QUEUED_TASKS; i++) {
        size_t o = 0x38 + i*0x10;
        memcpy(inb+o+0, &g_fence, 8);          // fence ptr
        uint64_t v = g_fenceVal; memcpy(inb+o+8, &v, 8); // value (already completed)
    }
    memcpy(outb+0x10, &g_fence, 8);            // output fence ptr
    memcpy(outb+0x18, &outVal, 8);             // output fence value

    auto t0 = [](){ LARGE_INTEGER c; QueryPerformanceCounter(&c); return c.QuadPart; };
    auto freq = [](){ LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f.QuadPart; };
    int64_t t_freq = freq();

    const int NITER = 60;
    int64_t tStart = t0();
    int executed = 0, fails = 0;
    for (int it = 0; it < NITER; it++) {
        int r = L.nvOFExecuteD3D12(hOf, (const void*)inb, (void*)outb);
        if (r != 0) {
            if (fails == 0) perr("nvOFExecuteD3D12", r);
            fails++;
            if (fails == 1) break;   // stop on first failure to avoid spam
            continue;
        }
        executed++;
        waitFence(outVal);           // proper fence wait = part of the timed region
        // advance fence value for next iter
        g_fenceVal = outVal;
        outVal = g_fenceVal + 1;
        // refresh the output fence point value for next iteration
        memcpy(outb+0x18, &outVal, 8);
    }
    int64_t tEnd = t0();
    double msTotal = (double)(tEnd - tStart) / t_freq * 1000.0;

    if (executed > 0) {
        double msPer = msTotal / executed;
        printf("\n=== TIMING (grid %u, %dx%d) ===\n", grid, W, H);
        printf("  executed=%d  ms total=%.3f  ms/frame=%.4f\n", executed, msTotal, msPer);
    } else {
        printf("\n=== Execute never succeeded (see failure above) ===\n");
    }

    // ---- read back flow and decode (only if we have a valid output handle) ----
    if (hOut && executed > 0) {
        void* mapped = nullptr; UINT64 rowPitch = 0;
        ID3D12Resource* rb = readbackTexture(texOut, gw, gh, 4, &mapped, &rowPitch);
        if (rb && mapped) {
            std::vector<int> dxs, dys;
            int x0 = (int)(gw*0.3), x1 = (int)(gw*0.7);
            int y0 = (int)(gh*0.3), y1 = (int)(gh*0.7);
            for (int y = y0; y < y1; y++) {
                const uint32_t* row = (const uint32_t*)((uint8_t*)mapped + y*rowPitch);
                for (int x = x0; x < x1; x++) {
                    uint32_t u = row[x];
                    int16_t ix = (int16_t)(u & 0xFFFF);
                    int16_t iy = (int16_t)(u >> 16);
                    dxs.push_back((int)ix);
                    dys.push_back((int)iy);
                }
            }
            auto median = [](std::vector<int>& v)->double {
                if (v.empty()) return 0;
                std::sort(v.begin(), v.end());
                size_t n = v.size();
                return n%2 ? v[n/2] : 0.5*(v[n/2-1]+v[n/2]);
            };
            double mdx = median(dxs), mdy = median(dys);
            printf("\n=== DECODED FLOW (grid %u) ===\n", grid);
            printf("  central region [%d..%d)x[%d..%d), n=%zu samples\n", x0, x1, y0, y1, dxs.size());
            printf("  median raw dx=%+d  dy=%+d  (int16 S10.5)\n", (int)mdx, (int)mdy);
            printf("  median dx=%.3f px (grid)  dy=%.3f px (grid)   => full-res ~%.2f px\n",
                   mdx/32.0, mdy/32.0, (mdx/32.0)*(int)grid);
            printf("  expected: dx ~= -8 or +8 (8px shift), dy ~= 0\n");
            rb->Unmap(0, nullptr);
        }
        if (rb) rb->Release();
    }

    L.nvOFUnregisterResourceD3D12(hOf, hInA);
    L.nvOFUnregisterResourceD3D12(hOf, hInB);
    L.nvOFUnregisterResourceD3D12(hOf, hOut);
    L.nvOFDestroy(hOf);
    return 0;
}
