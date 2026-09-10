// ============================================================================
//  sm86_smooth - headless VFI self test
//
//  Renders a parametric scene at t = 0, 0.5 and 1 (analytically, on the CPU),
//  asks the VFI engine to synthesise t = 0.5 from t = 0 and t = 1, then compares
//  against the ground truth.  Also validates the optical flow against the
//  analytic motion field, and reports throughput at 1080p / 1440p.
//
//  No window, no swapchain: this exercises exactly the same shader chain the
//  injected runtime uses, so the numbers are representative.
//
//  run:  vfi_selftest.exe [shader_path] [out_dir]
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
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

#include "vfi.h"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

using namespace sm86;

static inline float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }

// ---------------------------------------------------------------------------
//  The scene deliberately avoids periodic structure (stripes, checker grids).
//  A repeating pattern makes the SAD surface multi-modal, so a block matcher
//  locks onto a wrong period and the PSNR number stops measuring the code.
//  Instead: smooth blobs + non-repeating hashed detail + two hard objects.
//
//  Motion, all as a function of t in [0,1]:
//    background : pure translation, +D px on X   (analytic flow = (+D, 0))
//    bar        : rotation                       (locally translational)
//    disc       : translation, occludes then disoccludes
// ---------------------------------------------------------------------------
static float gBackgroundShift = 0.0f;   // filled in by renderScene

static inline uint32_t ihash(uint32_t x)
{
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
    return x;
}

static const float kBlobX[5] = { 0.10f, 0.34f, 0.53f, 0.72f, 0.90f };
static const float kBlobY[5] = { 0.24f, 0.76f, 0.40f, 0.86f, 0.30f };
static const float kBlobR[5] = { 0.085f, 0.12f, 0.07f, 0.10f, 0.08f };

static void renderScene(uint8_t* px, int W, int H, float t)
{
    const float D    = W * 0.04f;                 // background translation
    const float ang  = 0.60f * t;
    const float ca   = cosf(ang), sa = sinf(ang);
    const float barCX = W * 0.62f, barCY = H * 0.40f;
    const float barL  = W * 0.19f, barT = H * 0.055f;
    const float discCX = W * 0.22f + W * 0.24f * t, discCY = H * 0.62f;
    const float discR  = H * 0.14f;
    const float aspect = (float)W / (float)H;

    gBackgroundShift = D;

    for (int y = 0; y < H; ++y)
    {
        for (int x = 0; x < W; ++x)
        {
            float u = (float)x - D * t;           // world coords ride with the bg
            float v = (float)y;

            float bg = 0.30f + 0.26f * (v / (float)H);
            for (int i = 0; i < 5; ++i)
            {
                float dx = (u / (float)W - kBlobX[i]) / kBlobR[i];
                float dy = (v / (float)H - kBlobY[i]) / (kBlobR[i] * aspect);
                bg += 0.30f * expf(-(dx * dx + dy * dy));
            }
            // non-repeating detail: ~2.5 px hashed cells
            int cu = (int)floorf(u / 2.5f), cv = (int)floorf(v / 2.5f);
            float n = (float)(ihash((uint32_t)cu * 73856093u ^ (uint32_t)cv * 19349663u) & 0xffffu)
                      / 65535.0f;
            bg += 0.16f * (n - 0.5f);

            float r = bg, g = bg, b = bg;

            float dx = (float)x - barCX, dy = (float)y - barCY;
            float lx =  ca * dx + sa * dy;
            float ly = -sa * dx + ca * dy;
            if (fabsf(lx) < barL * 0.5f && fabsf(ly) < barT * 0.5f) { r = 0.95f; g = 0.34f; b = 0.12f; }

            float ddx = (float)x - discCX, ddy = (float)y - discCY;
            if (ddx * ddx + ddy * ddy < discR * discR) { r = 0.15f; g = 0.55f; b = 0.95f; }

            int i = (y * W + x) * 4;
            px[i + 0] = (uint8_t)(clampf(r, 0.f, 1.f) * 255.0f + 0.5f);
            px[i + 1] = (uint8_t)(clampf(g, 0.f, 1.f) * 255.0f + 0.5f);
            px[i + 2] = (uint8_t)(clampf(b, 0.f, 1.f) * 255.0f + 0.5f);
            px[i + 3] = 255;
        }
    }
}

// ---------------------------------------------------------------------------
//  The analytic motion field of the same scene, in the engine's convention:
//    fAB(p) is the displacement at p such that  B(p + fAB(p)) ~= A(p)
//  Frame A is t=0 and frame B is t=1.  renderScene draws background, then bar,
//  then disc, so the membership tests below have to be applied in reverse
//  order - the disc sits on top of the bar sits on top of the background.
//
//  Note this is NOT a global constant vector: the bar rotates, so its field is
//  a real per-pixel rotation, and the disc moves 6x faster than the
//  background.  A single-vector oracle would have measured nothing.
// ---------------------------------------------------------------------------
static void analyticFlowAB(int W, int H, std::vector<float>& f)
{
    const float D   = W * 0.04f;
    const float ang = 0.60f;                       // rotation at t = 1
    const float ca  = cosf(ang), sa = sinf(ang);
    const float bcx = W * 0.62f, bcy = H * 0.40f;
    const float bhx = W * 0.19f * 0.5f, bhy = H * 0.055f * 0.5f;
    const float dcx = W * 0.22f, dcy = H * 0.62f, dr = H * 0.14f;
    const float dxs = W * 0.24f;

    f.assign((size_t)W * H * 2, 0.0f);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
        {
            const float px = (float)x, py = (float)y;
            float rx, ry;

            const float ex = px - dcx, ey = py - dcy;
            if (ex * ex + ey * ey < dr * dr) { rx = dxs; ry = 0.0f; }
            else
            {
                const float bx = px - bcx, by = py - bcy;
                if (fabsf(bx) < bhx && fabsf(by) < bhy)
                {
                    // bar local coords at t=0 are just (bx,by); the bar body is
                    // at c + R(ang) * local at t=1
                    const float lx = ca * bx - sa * by;
                    const float ly = sa * bx + ca * by;
                    rx = (bcx + lx) - px; ry = (bcy + ly) - py;
                }
                else { rx = D; ry = 0.0f; }
            }

            const size_t i = ((size_t)y * W + x) * 2;
            f[i] = rx; f[i + 1] = ry;
        }
}

static void analyticFlowBA(int W, int H, std::vector<float>& f)
{
    const float D   = W * 0.04f;
    const float ang = 0.60f;
    const float ca  = cosf(ang), sa = sinf(ang);
    const float bcx = W * 0.62f, bcy = H * 0.40f;
    const float bhx = W * 0.19f * 0.5f, bhy = H * 0.055f * 0.5f;
    const float dcx = W * 0.46f, dcy = H * 0.62f, dr = H * 0.14f;
    const float dxs = -W * 0.24f;                  // disc is at t=1 here

    f.assign((size_t)W * H * 2, 0.0f);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
        {
            const float px = (float)x, py = (float)y;
            float rx, ry;

            const float ex = px - dcx, ey = py - dcy;
            if (ex * ex + ey * ey < dr * dr) { rx = dxs; ry = 0.0f; }
            else
            {
                const float bx = px - bcx, by = py - bcy;
                // membership at t=1: local = R(-ang) * (p - c)
                const float lx =  ca * bx + sa * by;
                const float ly = -sa * bx + ca * by;
                if (fabsf(lx) < bhx && fabsf(ly) < bhy)
                {
                    rx = (bcx + lx) - px; ry = (bcy + ly) - py;
                }
                else { rx = -D; ry = 0.0f; }
            }

            const size_t i = ((size_t)y * W + x) * 2;
            f[i] = rx; f[i + 1] = ry;
        }
}

// 50/50 average - the floor any interpolator has to beat.  With no motion
// information at all this is the only thing you can do.
static std::vector<uint8_t> mix5050(const std::vector<uint8_t>& a,
                                    const std::vector<uint8_t>& b)
{
    std::vector<uint8_t> o(a.size());
    for (size_t i = 0; i < a.size(); i += 4)
    {
        for (int c = 0; c < 3; ++c)
            o[i + c] = (uint8_t)(((int)a[i + c] + (int)b[i + c] + 1) >> 1);
        o[i + 3] = 255;
    }
    return o;
}

// ---------------------------------------------------------------------------
struct Gpu
{
    ID3D12Device*              dev     = nullptr;
    ID3D12CommandQueue*        queue   = nullptr;
    ID3D12CommandAllocator*    alloc[4] = {};
    ID3D12GraphicsCommandList* list    = nullptr;
    ID3D12Fence*               fence   = nullptr;
    HANDLE                     ev      = nullptr;
    UINT64                     fenceVal = 0;
    UINT64                     tFreq   = 0;
};

static bool gpuInit(Gpu& g)
{
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g.dev))))
        return false;

    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(g.dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&g.queue)))) return false;

    for (int i = 0; i < 4; ++i)
        if (FAILED(g.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                IID_PPV_ARGS(&g.alloc[i])))) return false;

    if (FAILED(g.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.alloc[0],
                                        nullptr, IID_PPV_ARGS(&g.list)))) return false;
    g.list->Close();

    if (FAILED(g.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence)))) return false;
    g.ev = CreateEventA(nullptr, FALSE, FALSE, nullptr);

    LARGE_INTEGER f; QueryPerformanceFrequency(&f);
    g.tFreq = (UINT64)f.QuadPart;
    return true;
}

static void gpuWait(Gpu& g, UINT64 v)
{
    if (g.fence->GetCompletedValue() < v)
    {
        g.fence->SetEventOnCompletion(v, g.ev);
        WaitForSingleObject(g.ev, 20000);
    }
}

static void gpuFlush(Gpu& g)
{
    const UINT64 v = ++g.fenceVal;
    g.queue->Signal(g.fence, v);
    gpuWait(g, v);
}

// ---------------------------------------------------------------------------
static ID3D12Resource* makeTex2D(Gpu& g, int W, int H, DXGI_FORMAT fmt)
{
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = W; rd.Height = H; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = fmt; rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ID3D12Resource* r = nullptr;
    g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                   D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&r));
    return r;
}

static ID3D12Resource* uploadTexFmt(Gpu& g, int W, int H, const uint8_t* data,
                                    DXGI_FORMAT fmt, int bpp)
{
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = W; rd.Height = H; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = fmt; rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    ID3D12Resource* tex = nullptr;
    g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                   D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&tex));

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
    UINT rows = 0; UINT64 rowSize = 0, total = 0;
    g.dev->GetCopyableFootprints(&rd, 0, 1, 0, &fp, &rows, &rowSize, &total);

    D3D12_HEAP_PROPERTIES up = {};
    up.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* upBuf = nullptr;
    g.dev->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd,
                                   D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upBuf));

    void* p = nullptr;
    upBuf->Map(0, nullptr, &p);
    for (UINT y = 0; y < rows; ++y)
        memcpy((uint8_t*)p + fp.Offset + y * fp.Footprint.RowPitch,
               data + (size_t)y * W * bpp, (size_t)W * bpp);
    upBuf->Unmap(0, nullptr);

    g.alloc[0]->Reset();
    g.list->Reset(g.alloc[0], nullptr);
    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = tex; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = upBuf; src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = fp;
    g.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    g.list->Close();
    g.queue->ExecuteCommandLists(1, (ID3D12CommandList**)&g.list);
    gpuFlush(g);
    upBuf->Release();
    return tex;
}

static ID3D12Resource* uploadTex(Gpu& g, int W, int H, const uint8_t* data)
{
    return uploadTexFmt(g, W, H, data, DXGI_FORMAT_R8G8B8A8_UNORM, 4);
}

// generic readback: raw bytes + the row pitch the copy actually used
struct Raw { std::vector<uint8_t> data; UINT rowPitch = 0; UINT rows = 0; };

static Raw readBackRaw(Gpu& g, ID3D12Resource* tex, D3D12_RESOURCE_STATES before)
{
    D3D12_RESOURCE_DESC rd = tex->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
    UINT rows = 0; UINT64 rowSize = 0, total = 0;
    g.dev->GetCopyableFootprints(&rd, 0, 1, 0, &fp, &rows, &rowSize, &total);

    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* rb = nullptr;
    g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
                                   D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb));

    g.alloc[0]->Reset();
    g.list->Reset(g.alloc[0], nullptr);
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource   = tex;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
    g.list->ResourceBarrier(1, &b);

    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = rb; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = fp;
    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = tex; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    g.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    D3D12_RESOURCE_BARRIER b2 = b;
    b2.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b2.Transition.StateAfter  = before;
    g.list->ResourceBarrier(1, &b2);

    g.list->Close();
    g.queue->ExecuteCommandLists(1, (ID3D12CommandList**)&g.list);
    gpuFlush(g);

    Raw out;
    out.rowPitch = fp.Footprint.RowPitch;
    out.rows     = rows;
    out.data.resize((size_t)fp.Footprint.RowPitch * rows);
    void* p = nullptr;
    rb->Map(0, nullptr, &p);
    memcpy(out.data.data(), (uint8_t*)p + fp.Offset, out.data.size());
    rb->Unmap(0, nullptr);
    rb->Release();
    return out;
}

// RGBA8 convenience wrapper
static std::vector<uint8_t> readBackRGBA(Gpu& g, ID3D12Resource* tex, int W, int H,
                                         D3D12_RESOURCE_STATES before)
{
    Raw r = readBackRaw(g, tex, before);
    std::vector<uint8_t> out((size_t)W * H * 4);
    for (int y = 0; y < H; ++y)
        memcpy(out.data() + (size_t)y * W * 4,
               r.data.data() + (size_t)y * r.rowPitch, (size_t)W * 4);
    return out;
}

// ---------------------------------------------------------------------------
static double psnr(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b)
{
    double se = 0; size_t n = 0;
    for (size_t i = 0; i < a.size(); i += 4)
        for (int c = 0; c < 3; ++c)
        {
            double d = (double)a[i + c] - (double)b[i + c];
            se += d * d; ++n;
        }
    double mse = se / (double)n;
    if (mse <= 1e-12) return 99.0;
    return 10.0 * log10(255.0 * 255.0 / mse);
}

// PSNR restricted to a mask (1 = include).  Anything outside the mask is an
// occlusion boundary and is deliberately excluded.
static double psnrMasked(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b,
                         const std::vector<uint8_t>& mask, size_t* nOut)
{
    double se = 0; size_t n = 0;
    for (size_t i = 0, k = 0; i < a.size(); i += 4, ++k)
    {
        if (!mask[k]) continue;
        for (int c = 0; c < 3; ++c)
        {
            double d = (double)a[i + c] - (double)b[i + c];
            se += d * d; ++n;
        }
    }
    if (nOut) *nOut = n;
    if (n == 0) return 0.0;
    double mse = se / (double)n;
    if (mse <= 1e-12) return 99.0;
    return 10.0 * log10(255.0 * 255.0 / mse);
}

// 1 = "well posed": nowhere near the disc at any of t=0/0.5/1, nor the bar at
// any of those orientations, with a margin so that a wrong answer just outside
// a boundary does not leak in.
static void wellPosedMask(int W, int H, std::vector<uint8_t>& m, float margin)
{
    m.assign((size_t)W * H, 0);
    const float bhx = W * 0.19f * 0.5f + margin;
    const float bhy = H * 0.055f * 0.5f + margin;
    const float bcx = W * 0.62f, bcy = H * 0.40f;
    const float dr  = H * 0.14f + margin;
    const float dcy = H * 0.62f;
    const float dcx[3] = { W * 0.22f, W * 0.34f, W * 0.46f };

    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
        {
            const float px = (float)x, py = (float)y;
            bool obj = false;
            for (int k = 0; k < 3 && !obj; ++k)
            {
                const float ex = px - dcx[k], ey = py - dcy;
                if (ex * ex + ey * ey < dr * dr) obj = true;
            }
            for (int k = 0; k < 3 && !obj; ++k)
            {
                const float a  = 0.30f * (float)k;      // t = k/2
                const float ca = cosf(a), sa = sinf(a);
                const float bx = px - bcx, by = py - bcy;
                const float lx =  ca * bx + sa * by;
                const float ly = -sa * bx + ca * by;
                if (fabsf(lx) < bhx && fabsf(ly) < bhy) obj = true;
            }
            m[(size_t)y * W + x] = obj ? 0 : 1;
        }
}

static void writePPM(const char* path, const std::vector<uint8_t>& px, int W, int H)
{
    FILE* f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    std::vector<uint8_t> rgb((size_t)W * H * 3);
    for (size_t i = 0, j = 0; i < px.size(); i += 4, j += 3)
    { rgb[j] = px[i]; rgb[j + 1] = px[i + 1]; rgb[j + 2] = px[i + 2]; }
    fwrite(rgb.data(), 1, rgb.size(), f);
    fclose(f);
}

static void writeGrayPNG(const char* path, const std::vector<uint8_t>& g, int W, int H);

static void writeDiff(const char* path, const std::vector<uint8_t>& a,
                      const std::vector<uint8_t>& b, int W, int H)
{
    std::vector<uint8_t> g((size_t)W * H);
    for (size_t i = 0, j = 0; i < a.size(); i += 4, j++)
    {
        int d = 0;
        for (int c = 0; c < 3; ++c) d = std::max(d, abs((int)a[i + c] - (int)b[i + c]));
        g[j] = (uint8_t)std::min(255, d * 4);
    }
    writeGrayPNG(path, g, W, H);
}

// ---------------------------------------------------------------------------
// minimal PNG writer (zlib "stored" blocks - no dependency on a zip library)
// ---------------------------------------------------------------------------
static void pngChunk(FILE* f, const char* type, const uint8_t* data, uint32_t len)
{
    uint8_t hdr[8];
    hdr[0] = (len >> 24) & 0xff; hdr[1] = (len >> 16) & 0xff;
    hdr[2] = (len >>  8) & 0xff; hdr[3] = (len      ) & 0xff;
    memcpy(hdr + 4, type, 4);
    fwrite(hdr, 1, 8, f);

    uint32_t crc = 0xffffffffu;
    auto upd = [&](const uint8_t* p, uint32_t n) {
        for (uint32_t i = 0; i < n; ++i)
        {
            crc ^= p[i];
            for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xedb88320u & (uint32_t)(-(int)(crc & 1)));
        }
    };
    upd((const uint8_t*)type, 4);
    upd(data, len);
    fwrite(data, 1, len, f);
    uint8_t tail[4] = { (uint8_t)((crc ^ 0xffffffffu) >> 24), (uint8_t)((crc ^ 0xffffffffu) >> 16),
                        (uint8_t)((crc ^ 0xffffffffu) >> 8),  (uint8_t)(crc ^ 0xffffffffu) };
    fwrite(tail, 1, 4, f);
}

static void writePNG(const char* path, const std::vector<uint8_t>& rgb, int W, int H, int ch)
{
    FILE* f = fopen(path, "wb");
    if (!f) return;
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
    fwrite(sig, 1, 8, f);

    uint8_t ihdr[13] = {};
    ihdr[0] = (W >> 24) & 0xff; ihdr[1] = (W >> 16) & 0xff; ihdr[2] = (W >> 8) & 0xff; ihdr[3] = W & 0xff;
    ihdr[4] = (H >> 24) & 0xff; ihdr[5] = (H >> 16) & 0xff; ihdr[6] = (H >> 8) & 0xff; ihdr[7] = H & 0xff;
    ihdr[8] = 8;                             // bit depth
    ihdr[9] = (ch == 3) ? 2 : 0;             // colour type: truecolour / greyscale
    pngChunk(f, "IHDR", ihdr, 13);

    const size_t stride = (size_t)W * ch;
    std::vector<uint8_t> raw;
    raw.reserve((stride + 1) * H);
    for (int y = 0; y < H; ++y)
    {
        raw.push_back(0);
        raw.insert(raw.end(), rgb.begin() + (size_t)y * stride,
                               rgb.begin() + (size_t)(y + 1) * stride);
    }

    // zlib container with stored deflate blocks
    std::vector<uint8_t> z;
    z.push_back(0x78); z.push_back(0x01);
    size_t off = 0;
    while (off < raw.size())
    {
        const size_t n = std::min<size_t>(65535, raw.size() - off);
        z.push_back((off + n >= raw.size()) ? 1 : 0);
        z.push_back((uint8_t)(n & 0xff)); z.push_back((uint8_t)(n >> 8));
        z.push_back((uint8_t)(~n & 0xff)); z.push_back((uint8_t)((~n >> 8) & 0xff));
        z.insert(z.end(), raw.begin() + off, raw.begin() + off + n);
        off += n;
    }
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw.size(); ++i)
    {
        a = (a + raw[i]) % 65521;
        b = (b + a) % 65521;
    }
    const uint32_t adler = (b << 16) | a;
    z.push_back((adler >> 24) & 0xff); z.push_back((adler >> 16) & 0xff);
    z.push_back((adler >> 8) & 0xff);  z.push_back(adler & 0xff);

    pngChunk(f, "IDAT", z.data(), (uint32_t)z.size());
    pngChunk(f, "IEND", nullptr, 0);
    fclose(f);
}

static void writeGrayPNG(const char* path, const std::vector<uint8_t>& g, int W, int H)
{
    writePNG(path, g, W, H, 1);
}

static void writeRGBPNG(const char* path, const std::vector<uint8_t>& rgba, int W, int H)
{
    std::vector<uint8_t> rgb((size_t)W * H * 3);
    for (size_t i = 0, j = 0; i < rgba.size(); i += 4, j += 3)
    { rgb[j] = rgba[i]; rgb[j + 1] = rgba[i + 1]; rgb[j + 2] = rgba[i + 2]; }
    writePNG(path, rgb, W, H, 3);
}

// ---------------------------------------------------------------------------
static const char* findShader(int argc, char** argv)
{
    static std::string s;
    if (argc > 1) { s = argv[1]; return s.c_str(); }
    const char* cands[] = {
        "src/shaders/vfi.hlsl", "../src/shaders/vfi.hlsl",
        "shaders/vfi.hlsl", "../../src/shaders/vfi.hlsl", "vfi.hlsl",
    };
    for (const char* c : cands)
        if (GetFileAttributesA(c) != INVALID_FILE_ATTRIBUTES) { s = c; return s.c_str(); }
    s = "src/shaders/vfi.hlsl";
    return s.c_str();
}

static float medianOf(std::vector<float> v)
{
    if (v.empty()) return 0.0f;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

// ---------------------------------------------------------------------------
static int runSize(Gpu& g, const VfiOptions& opt, const char* shader,
                   int W, int H, bool visual, const char* outDir, int iters)
{
    printf("\n=== %dx%d ===\n", W, H);

    std::vector<uint8_t> cpuA((size_t)W * H * 4), cpuB((size_t)W * H * 4),
                         cpuM((size_t)W * H * 4);
    renderScene(cpuA.data(), W, H, 0.0f);
    const float expectedShift = gBackgroundShift;
    renderScene(cpuM.data(), W, H, 0.5f);
    renderScene(cpuB.data(), W, H, 1.0f);

    ID3D12Resource* A = uploadTex(g, W, H, cpuA.data());
    ID3D12Resource* B = uploadTex(g, W, H, cpuB.data());
    ID3D12Resource* M = uploadTex(g, W, H, cpuM.data());
    ID3D12Resource* outTex = makeTex2D(g, W, H, DXGI_FORMAT_R8G8B8A8_UNORM);

    VfiEngine eng;
    std::string err;
    wchar_t wpath[MAX_PATH * 2];
    MultiByteToWideChar(CP_UTF8, 0, shader, -1, wpath, MAX_PATH * 2);
    if (!eng.init(g.dev, DXGI_FORMAT_R8G8B8A8_UNORM, W, H, opt, wpath, &err))
    {
        printf("engine init failed: %s\n", err.c_str());
        return 1;
    }

    // ---- one generated frame: A,B -> midpoint ----
    g.alloc[0]->Reset();
    g.list->Reset(g.alloc[0], nullptr);
    if (!eng.record(g.list, 0, A, D3D12_RESOURCE_STATE_COPY_DEST,
                    B, D3D12_RESOURCE_STATE_COPY_DEST,
                    outTex, D3D12_RESOURCE_STATE_COMMON, 0.5f, false))
    {
        printf("record failed\n");
        return 1;
    }
    g.list->Close();
    g.queue->ExecuteCommandLists(1, (ID3D12CommandList**)&g.list);
    gpuFlush(g);
    const uint32_t estDisp = eng.dispatch_count();
    printf("dispatches per generated frame : %u\n", estDisp);
    printf("search reach (L3, 1/8 res)     : +-%d px  (auto)\n",
           eng.effective_radius_l3() * 8);

    std::vector<uint8_t> gen = readBackRGBA(g, outTex, W, H, D3D12_RESOURCE_STATE_COMMON);

    std::vector<uint8_t> mix = mix5050(cpuA, cpuB);
    const double psnrMix = psnr(mix, cpuM);
    const double psnrEst = psnr(gen, cpuM);

    printf("PSNR(truth vs A)               : %6.2f dB   <- difficulty reference\n", psnr(cpuA, cpuM));
    printf("PSNR(gen   vs truth)           : %6.2f dB\n", psnrEst);
    printf("PSNR(gen   vs A)               : %6.2f dB\n", psnr(gen, cpuA));
    printf("PSNR(gen   vs B)               : %6.2f dB\n", psnr(gen, cpuB));

    // ---- validate the estimated flow against the analytic field ----
    {
        Raw fl = readBackRaw(g, eng.debug_flow_ab(), eng.debug_state(eng.debug_flow_ab()));
        std::vector<float> xs, ys;
        const int x0 = W / 50, x1 = W / 7, y0 = H / 50, y1 = H / 5;  // pure background
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x)
            {
                const float* p = (const float*)(fl.data.data() + (size_t)y * fl.rowPitch) + x * 2;
                xs.push_back(p[0]);
                ys.push_back(p[1]);
            }
        printf("flow fAB.x in background       : %6.2f px   (analytic %6.2f)\n",
               medianOf(xs), expectedShift);
        printf("flow fAB.y in background       : %6.2f px   (analytic   0.00)\n", medianOf(ys));

        // error distribution: |fAB.x - analytic|, plus the fraction that is
        // within one pixel.  The latter is the number that actually predicts
        // the blend output - a field that is right on 95% of pixels and
        // garbage on 5% is not "5% wrong", it is a 5%-area artefact.
        std::vector<float> ax;
        ax.reserve(xs.size());
        double sumE = 0.0; size_t nGood = 0;
        for (size_t i = 0; i < xs.size(); ++i)
        {
            const float e = fabsf(xs[i] - expectedShift);
            ax.push_back(e);
            sumE += e;
            if (e <= 1.0f) ++nGood;
        }
        std::sort(ax.begin(), ax.end());
        auto pct = [&](double q) { return ax[(size_t)(q * (ax.size() - 1) + 0.5)]; };
        printf("flow |err| x: p50 %6.2f  p90 %6.2f  p99 %6.2f  max %6.2f px\n",
               pct(0.50), pct(0.90), pct(0.99), ax.back());
        printf("flow mean |err|                : %6.2f px\n", sumE / (double)xs.size());
        printf("flow within 1 px of analytic   : %6.1f %%   <- what the blend sees\n",
               100.0 * (double)nGood / (double)xs.size());

        // raw float dump, unconditional: the analysis script needs the real
        // values, and 8-bit normalised output cannot resolve a 1 px error
        if (outDir)
        {
            char pb[512];
            sprintf_s(pb, "%s/%dx%d_flowAB.f32", outDir, W, H);
            FILE* f = fopen(pb, "wb");
            if (f)
            {
                std::vector<float> tmp((size_t)W * H * 2);
                for (int y = 0; y < H; ++y)
                    memcpy(tmp.data() + (size_t)y * W * 2,
                           fl.data.data() + (size_t)y * fl.rowPitch, (size_t)W * 8);
                fwrite(tmp.data(), 4, tmp.size(), f);
                fclose(f);
            }
            Raw fb = readBackRaw(g, eng.debug_flow_ba(), eng.debug_state(eng.debug_flow_ba()));
            sprintf_s(pb, "%s/%dx%d_flowBA.f32", outDir, W, H);
            f = fopen(pb, "wb");
            if (f)
            {
                std::vector<float> tmp((size_t)W * H * 2);
                for (int y = 0; y < H; ++y)
                    memcpy(tmp.data() + (size_t)y * W * 2,
                           fb.data.data() + (size_t)y * fb.rowPitch, (size_t)W * 8);
                fwrite(tmp.data(), 4, tmp.size(), f);
                fclose(f);
            }
            Raw cs = readBackRaw(g, eng.debug_cost(), eng.debug_state(eng.debug_cost()));
            sprintf_s(pb, "%s/%dx%d_cost.f32", outDir, W, H);
            f = fopen(pb, "wb");
            if (f)
            {
                std::vector<float> tmp((size_t)W * H);
                for (int y = 0; y < H; ++y)
                    memcpy(tmp.data() + (size_t)y * W,
                           cs.data.data() + (size_t)y * cs.rowPitch, (size_t)W * 4);
                fwrite(tmp.data(), 4, tmp.size(), f);
                fclose(f);
            }
        }

        if (visual && outDir)
        {
            float mx = 0;
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x)
                {
                    const float* p = (const float*)(fl.data.data() + (size_t)y * fl.rowPitch) + x * 2;
                    mx = std::max(mx, fabsf(p[0]));
                }
            std::vector<uint8_t> fg((size_t)W * H);
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x)
                {
                    const float* p = (const float*)(fl.data.data() + (size_t)y * fl.rowPitch) + x * 2;
                    fg[(size_t)y * W + x] = (uint8_t)std::min(255.0f, 128.0f + 127.0f * p[0] / (mx + 1e-3f));
                }
            char p2[512];
            sprintf_s(p2, "%s/%dx%d_flowAB.pgm", outDir, W, H);
            FILE* f = fopen(p2, "wb");
            if (f) { fprintf(f, "P5\n%d %d\n255\n", W, H); fwrite(fg.data(), 1, fg.size(), f); fclose(f); }
        }
    }

    if (visual && outDir)
    {
        CreateDirectoryA(outDir, nullptr);
        char p[512];
        sprintf_s(p, "%s/%dx%d_A.pgm",     outDir, W, H); writePPM(p, cpuA, W, H);
        sprintf_s(p, "%s/%dx%d_B.pgm",     outDir, W, H); writePPM(p, cpuB, W, H);
        sprintf_s(p, "%s/%dx%d_truth.pgm", outDir, W, H); writePPM(p, cpuM, W, H);
        sprintf_s(p, "%s/%dx%d_gen.pgm",   outDir, W, H); writePPM(p, gen,  W, H);
        sprintf_s(p, "%s/%dx%d_A.png",     outDir, W, H); writeRGBPNG(p, cpuA, W, H);
        sprintf_s(p, "%s/%dx%d_B.png",     outDir, W, H); writeRGBPNG(p, cpuB, W, H);
        sprintf_s(p, "%s/%dx%d_truth.png", outDir, W, H); writeRGBPNG(p, cpuM, W, H);
        sprintf_s(p, "%s/%dx%d_gen.png",   outDir, W, H); writeRGBPNG(p, gen,  W, H);
        sprintf_s(p, "%s/%dx%d_diff.png",  outDir, W, H); writeDiff(p, gen, cpuM, W, H);
        printf("wrote %s/%dx%d_{A,B,truth,gen,diff}.png + flowAB.pgm\n", outDir, W, H);
    }

    // ---- throughput ----
    for (int i = 0; i < 8; ++i)
    {
        g.alloc[i % 4]->Reset();
        g.list->Reset(g.alloc[i % 4], nullptr);
        eng.record(g.list, i % 3, A, D3D12_RESOURCE_STATE_COPY_DEST,
                   B, D3D12_RESOURCE_STATE_COPY_DEST,
                   outTex, D3D12_RESOURCE_STATE_COMMON, 0.5f, false);
        g.list->Close();
        g.queue->ExecuteCommandLists(1, (ID3D12CommandList**)&g.list);
    }
    gpuFlush(g);

    LARGE_INTEGER t0, t1; QueryPerformanceCounter(&t0);
    {
        g.alloc[0]->Reset();
        g.list->Reset(g.alloc[0], nullptr);
        eng.record(g.list, 0, A, D3D12_RESOURCE_STATE_COPY_DEST,
                   B, D3D12_RESOURCE_STATE_COPY_DEST,
                   outTex, D3D12_RESOURCE_STATE_COMMON, 0.5f, false);
        g.list->Close();
        g.queue->ExecuteCommandLists(1, (ID3D12CommandList**)&g.list);
        gpuFlush(g);
    }
    QueryPerformanceCounter(&t1);
    const double latMs = 1000.0 * (double)(t1.QuadPart - t0.QuadPart) / (double)g.tFreq;

    QueryPerformanceCounter(&t0);
    for (int i = 0; i < iters; ++i)
    {
        g.alloc[i % 4]->Reset();
        g.list->Reset(g.alloc[i % 4], nullptr);
        eng.record(g.list, i % 3, A, D3D12_RESOURCE_STATE_COPY_DEST,
                   B, D3D12_RESOURCE_STATE_COPY_DEST,
                   outTex, D3D12_RESOURCE_STATE_COMMON, 0.5f, false);
        g.list->Close();
        g.queue->ExecuteCommandLists(1, (ID3D12CommandList**)&g.list);
        if ((i & 3) == 3) { g.queue->Signal(g.fence, ++g.fenceVal); gpuWait(g, g.fenceVal); }
    }
    gpuFlush(g);
    QueryPerformanceCounter(&t1);
    const double thrMs = 1000.0 * (double)(t1.QuadPart - t0.QuadPart) / (double)g.tFreq / iters;

    printf("latency  (1 frame, idle queue) : %6.3f ms\n", latMs);
    printf("throughput (sustained, n=%d)   : %6.3f ms/frame\n", iters, thrMs);

    // =======================================================================
    //  Ceiling measurement.
    //  Same shaders, same blend, same HF transfer - the only change is that the
    //  flow field is the analytic one instead of an estimate.  So the gap
    //  between psnrEst and psnrOracle is what the matcher costs, and the gap
    //  between psnrOracle and a perfect 99 dB is what the classical
    //  warp/blend formulation costs and no amount of tuning can recover.
    // =======================================================================
    uint32_t oracleDisp = 0;
    double   oraclePsnr = 0.0, oracleLat = 0.0, oracleThr = 0.0;
    std::vector<uint8_t> genO;
    {
        std::vector<float> fab, fba;
        analyticFlowAB(W, H, fab);
        analyticFlowBA(W, H, fba);
        std::vector<uint8_t> cost((size_t)W * H * 4, 0);   // 0 = fully trusted

        ID3D12Resource* tfAB  = uploadTexFmt(g, W, H, (const uint8_t*)fab.data(),
                                             DXGI_FORMAT_R32G32_FLOAT, 8);
        ID3D12Resource* tfBA  = uploadTexFmt(g, W, H, (const uint8_t*)fba.data(),
                                             DXGI_FORMAT_R32G32_FLOAT, 8);
        ID3D12Resource* tCost = uploadTexFmt(g, W, H, cost.data(),
                                             DXGI_FORMAT_R32_FLOAT, 4);

        eng.set_flow_override(tfAB, tfBA, tCost);

        g.alloc[0]->Reset();
        g.list->Reset(g.alloc[0], nullptr);
        eng.record(g.list, 0, A, D3D12_RESOURCE_STATE_COPY_DEST,
                   B, D3D12_RESOURCE_STATE_COPY_DEST,
                   outTex, D3D12_RESOURCE_STATE_COMMON, 0.5f, false);
        oracleDisp = eng.dispatch_count();
        g.list->Close();
        g.queue->ExecuteCommandLists(1, (ID3D12CommandList**)&g.list);
        gpuFlush(g);

        genO = readBackRGBA(g, outTex, W, H, D3D12_RESOURCE_STATE_COMMON);
        oraclePsnr = psnr(genO, cpuM);

        for (int i = 0; i < 8; ++i)
        {
            g.alloc[i % 4]->Reset();
            g.list->Reset(g.alloc[i % 4], nullptr);
            eng.record(g.list, i % 3, A, D3D12_RESOURCE_STATE_COPY_DEST,
                       B, D3D12_RESOURCE_STATE_COPY_DEST,
                       outTex, D3D12_RESOURCE_STATE_COMMON, 0.5f, false);
            g.list->Close();
            g.queue->ExecuteCommandLists(1, (ID3D12CommandList**)&g.list);
        }
        gpuFlush(g);

        LARGE_INTEGER o0, o1; QueryPerformanceCounter(&o0);
        {
            g.alloc[0]->Reset();
            g.list->Reset(g.alloc[0], nullptr);
            eng.record(g.list, 0, A, D3D12_RESOURCE_STATE_COPY_DEST,
                       B, D3D12_RESOURCE_STATE_COPY_DEST,
                       outTex, D3D12_RESOURCE_STATE_COMMON, 0.5f, false);
            g.list->Close();
            g.queue->ExecuteCommandLists(1, (ID3D12CommandList**)&g.list);
            gpuFlush(g);
        }
        QueryPerformanceCounter(&o1);
        oracleLat = 1000.0 * (double)(o1.QuadPart - o0.QuadPart) / (double)g.tFreq;

        QueryPerformanceCounter(&o0);
        for (int i = 0; i < iters; ++i)
        {
            g.alloc[i % 4]->Reset();
            g.list->Reset(g.alloc[i % 4], nullptr);
            eng.record(g.list, i % 3, A, D3D12_RESOURCE_STATE_COPY_DEST,
                       B, D3D12_RESOURCE_STATE_COPY_DEST,
                       outTex, D3D12_RESOURCE_STATE_COMMON, 0.5f, false);
            g.list->Close();
            g.queue->ExecuteCommandLists(1, (ID3D12CommandList**)&g.list);
            if ((i & 3) == 3) { g.queue->Signal(g.fence, ++g.fenceVal); gpuWait(g, g.fenceVal); }
        }
        gpuFlush(g);
        QueryPerformanceCounter(&o1);
        oracleThr = 1000.0 * (double)(o1.QuadPart - o0.QuadPart) / (double)g.tFreq / iters;

        eng.set_flow_override(nullptr, nullptr, nullptr);
        tfAB->Release(); tfBA->Release(); tCost->Release();
    }

    printf("\n---- warp/blend ceiling (analytic flow, matcher bypassed) ----\n");
    printf("dispatches per generated frame : %u   (vs %u with the matcher)\n",
           oracleDisp, estDisp);
    printf("PSNR(oracle vs truth)          : %6.2f dB\n", oraclePsnr);
    printf("latency  (1 frame, idle queue) : %6.3f ms\n", oracleLat);
    printf("throughput (sustained, n=%d)   : %6.3f ms/frame\n", iters, oracleThr);

    std::vector<uint8_t> wm;
    wellPosedMask(W, H, wm, (float)W * 0.01f);
    size_t wmN = 0;
    for (uint8_t v : wm) if (v) ++wmN;
    const double wellPct = 100.0 * (double)wmN / ((double)W * (double)H);
    const double mixWell = psnrMasked(mix,  cpuM, wm, nullptr);
    const double estWell = psnrMasked(gen,  cpuM, wm, nullptr);
    const double orcWell = psnrMasked(genO, cpuM, wm, nullptr);

    printf("\n---- %dx%d PSNR(gen vs truth), every pixel ----\n", W, H);
    printf("  no motion, 50/50 average     : %6.2f dB   <- floor\n", psnrMix);
    printf("  matcher + blend (this build) : %6.2f dB\n", psnrEst);
    printf("  analytic flow + blend        : %6.2f dB   <- ceiling of this blend\n", oraclePsnr);
    printf("  cost of the matcher          : %6.2f dB\n", oraclePsnr - psnrEst);

    printf("\n---- %dx%d PSNR over well-posed pixels only (%.1f%% of frame) ----\n",
           W, H, wellPct);
    printf("  no motion, 50/50 average     : %6.2f dB\n", mixWell);
    printf("  matcher + blend (this build) : %6.2f dB\n", estWell);
    printf("  analytic flow + blend        : %6.2f dB   <- blend ceiling, no occlusion\n", orcWell);
    printf("  everything outside this mask is an occlusion boundary, where neither\n");
    printf("  source warp contains the answer - that is the learned model's job.\n");

    if (visual && outDir)
    {
        char p[512];
        sprintf_s(p, "%s/%dx%d_oracle.png", outDir, W, H); writeRGBPNG(p, genO, W, H);
        sprintf_s(p, "%s/%dx%d_oracle_diff.png", outDir, W, H); writeDiff(p, genO, cpuM, W, H);
        sprintf_s(p, "%s/%dx%d_mix5050.png", outDir, W, H); writeRGBPNG(p, mix, W, H);
    }

    eng.shutdown();
    outTex->Release(); A->Release(); B->Release(); M->Release();
    return 0;
}

int main(int argc, char** argv)
{
    const char* shader = findShader(argc, argv);
    const char* outDir = (argc > 2) ? argv[2] : "vfi_out";

    printf("sm86_smooth VFI self-test\n");
    printf("shader : %s\n", shader);

    Gpu g;
    if (!gpuInit(g)) { printf("D3D12 device init failed\n"); return 1; }

    {
        IDXGIFactory4* f = nullptr;
        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&f))))
        {
            IDXGIAdapter1* ad0 = nullptr;
            if (f->EnumAdapterByLuid(g.dev->GetAdapterLuid(), IID_PPV_ARGS(&ad0)) == S_OK)
            {
                DXGI_ADAPTER_DESC ad = {};
                ad0->GetDesc(&ad);
                wprintf(L"adapter: %s  (VRAM %llu MB)\n", ad.Description,
                        (unsigned long long)(ad.DedicatedVideoMemory >> 20));
                ad0->Release();
            }
            f->Release();
        }
    }

    VfiOptions opt;
    for (int i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "--prewarp") == 0) opt.prewarp_seed = true;
        if (strcmp(argv[i], "--fp16")    == 0) opt.luma_fp16 = true;
        if (strcmp(argv[i], "--fp32")    == 0) opt.luma_fp16 = false;
    }
    printf("luma storage  : %s\n", opt.luma_fp16 ? "R16_FLOAT" : "R32_FLOAT");
    printf("seed handling : %s\n", opt.prewarp_seed
           ? "cs_seed_warp pre-shift (residual centred on 0)"
           : "seed inside tile index, global read on miss");

    if (runSize(g, opt, shader, 960, 540, true, outDir, 120)) return 1;
    if (runSize(g, opt, shader, 1920, 1080, false, outDir, 200)) return 1;
    if (runSize(g, opt, shader, 2560, 1440, false, outDir, 60)) return 1;

    printf("\ndone.\n");
    return 0;
}
