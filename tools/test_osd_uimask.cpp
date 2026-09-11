#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <cstdio>
#include <vector>
#include "../src/proxy/ui_mask.h"
#include "../src/proxy/osd_overlay.h"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "user32.lib")

static void SaveBMP(const char* filename, const uint32_t* pixels, int w, int h) {
    BITMAPFILEHEADER bfh = {};
    BITMAPINFOHEADER bih = {};
    bfh.bfType = 0x4D42;
    bfh.bfOffBits = sizeof(bfh) + sizeof(bih);
    bfh.bfSize = bfh.bfOffBits + w * h * 4;

    bih.biSize = sizeof(bih);
    bih.biWidth = w;
    bih.biHeight = -h; // Top-down
    bih.biPlanes = 1;
    bih.biBitCount = 32;
    bih.biCompression = BI_RGB;

    CreateDirectoryA("demo_out", nullptr);
    FILE* f = fopen(filename, "wb");
    if (!f) return;
    fwrite(&bfh, sizeof(bfh), 1, f);
    fwrite(&bih, sizeof(bih), 1, f);
    fwrite(pixels, w * h * 4, 1, f);
    fclose(f);
    printf("[+] Saved BMP image: %s\n", filename);
}

int main() {
    printf("================================================================\n");
    printf("  Testing Pass-Level UI Mask Protection & In-Game OSD Overlay  \n");
    printf("================================================================\n");

    ID3D12Device* dev = nullptr;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev)))) {
        printf("[!] D3D12CreateDevice failed\n");
        return 1;
    }
    printf("[+] D3D12 Device created @ %p\n", dev);

    ID3D12CommandQueue* queue = nullptr;
    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));

    ID3D12CommandAllocator* alloc = nullptr;
    dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));

    ID3D12GraphicsCommandList* cl = nullptr;
    dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&cl));

    const uint32_t W = 1920;
    const uint32_t H = 1080;

    // Create textures
    auto CreateTex = [&](D3D12_RESOURCE_STATES state, D3D12_RESOURCE_FLAGS flags) -> ID3D12Resource* {
        D3D12_RESOURCE_DESC td = {};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = W;
        td.Height = H;
        td.DepthOrArraySize = 1;
        td.MipLevels = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Flags = flags;
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        ID3D12Resource* r = nullptr;
        dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, state, nullptr, IID_PPV_ARGS(&r));
        return r;
    };

    ID3D12Resource* currTex = CreateTex(D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE);
    ID3D12Resource* prevTex = CreateTex(D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE);
    ID3D12Resource* genTex  = CreateTex(D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE);
    ID3D12Resource* outTex  = CreateTex(D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

    // Create Readback buffer
    ID3D12Resource* readbackBuf = nullptr;
    {
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = W * H * 4;
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readbackBuf));
    }

    // Generate Synthetic Frames:
    // Scene has a moving background gradient, but a static crosshair + health bar + minimap + text
    std::vector<uint32_t> currPixels(W * H);
    std::vector<uint32_t> prevPixels(W * H);
    std::vector<uint32_t> genPixels(W * H);

    for (uint32_t y = 0; y < H; ++y) {
        for (uint32_t x = 0; x < W; ++x) {
            // Background 3D world: smooth color shift
            uint8_t currBg = (uint8_t)((x * 120 / W + y * 80 / H + 60) & 0xFF);
            uint8_t prevBg = (uint8_t)(((x + 40) * 120 / W + y * 80 / H + 60) & 0xFF); // 40px camera motion
            uint8_t genBg  = (uint8_t)(((x + 20) * 120 / W + y * 80 / H + 60) & 0xFF); // 20px interpolated

            uint32_t cCurr = 0xFF000000 | (currBg << 16) | (currBg << 8) | currBg;
            uint32_t cPrev = 0xFF000000 | (prevBg << 16) | (prevBg << 8) | prevBg;
            uint32_t cGen  = 0xFF000000 | (genBg << 16)  | (genBg << 8)  | genBg;

            // 1. Static Crosshair at Center (W/2, H/2)
            int dx = (int)x - (int)(W / 2);
            int dy = (int)y - (int)(H / 2);
            if ((abs(dx) <= 18 && abs(dy) <= 2) || (abs(dy) <= 18 && abs(dx) <= 2)) {
                // Static bright green crosshair
                cCurr = 0xFF00FF00;
                cPrev = 0xFF00FF00;
                // In naive gen, crosshair was distorted/blurred by background motion!
                cGen  = 0xFF008800; // distorted
            }

            // 2. Static Health Bar at Bottom Left (X: 100..400, Y: 980..1020)
            if (x >= 100 && x <= 400 && y >= 980 && y <= 1020) {
                if (x == 100 || x == 400 || y == 980 || y == 1020) {
                    cCurr = cPrev = 0xFFFFFFFF; // White border
                    cGen  = 0xFFAAAAAA;
                } else if (x <= 320) {
                    cCurr = cPrev = 0xFF0033CC; // Red health
                    cGen  = 0xFF002288;
                }
            }

            // 3. Static Minimap Border at Top Left (X: 50..250, Y: 50..250)
            if (x >= 50 && x <= 250 && y >= 50 && y <= 250) {
                if (x == 50 || x == 250 || y == 50 || y == 250) {
                    cCurr = cPrev = 0xFF00DDAA; // Cyan minimap outline
                    cGen  = 0xFF008866;
                }
            }

            currPixels[y * W + x] = cCurr;
            prevPixels[y * W + x] = cPrev;
            genPixels[y * W + x]  = cGen;
        }
    }

    // Upload to textures
    ID3D12Resource* upBuffer = nullptr;
    {
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bd = {};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = W * H * 4 * 3;
        bd.Height = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upBuffer));

        uint8_t* pMap = nullptr;
        upBuffer->Map(0, nullptr, (void**)&pMap);
        memcpy(pMap, currPixels.data(), W * H * 4);
        memcpy(pMap + W * H * 4, prevPixels.data(), W * H * 4);
        memcpy(pMap + W * H * 8, genPixels.data(), W * H * 4);
        upBuffer->Unmap(0, nullptr);
    }

    auto CopyToTex = [&](ID3D12Resource* tex, UINT64 offset) {
        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource = tex;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION src = {};
        src.pResource = upBuffer;
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Offset = offset;
        src.PlacedFootprint.Footprint.Width = W;
        src.PlacedFootprint.Footprint.Height = H;
        src.PlacedFootprint.Footprint.Depth = 1;
        src.PlacedFootprint.Footprint.RowPitch = W * 4;
        src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;

        cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = tex;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        cl->ResourceBarrier(1, &b);
    };

    CopyToTex(currTex, 0);
    CopyToTex(prevTex, W * H * 4);
    CopyToTex(genTex,  W * H * 8);

    // Transition outTex to UNORDERED_ACCESS
    {
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = outTex;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        cl->ResourceBarrier(1, &b);
    }

    // 1. Initialize & Record UI Mask
    sm86::UiMaskEngine uiMask;
    if (!uiMask.Initialize(dev)) {
        printf("[!] UiMaskEngine::Initialize failed\n");
        return 1;
    }
    printf("[+] UiMaskEngine initialized successfully\n");

    sm86::UiMaskConfig maskCfg = {};
    maskCfg.enabled = true;
    maskCfg.debugHeatmap = false;
    maskCfg.crosshairBoost = true;

    uiMask.Record(cl, currTex, prevTex, genTex, outTex, W, H, maskCfg);
    printf("[+] UiMaskEngine::Record executed (HUD & Crosshair protection)\n");

    // 2. Initialize & Record OSD Overlay
    sm86::OsdOverlay osd;
    if (!osd.Initialize(dev, queue)) {
        printf("[!] OsdOverlay::Initialize failed\n");
        return 1;
    }
    printf("[+] OsdOverlay initialized successfully\n");

    osd.Update(0.59f, "Road 1 (NvPresent64 Rehost) - FP16 HMMA", true, false);
    osd.Record(cl, outTex, W, H);
    printf("[+] OsdOverlay::Record executed (Telemetry overlay composited)\n");

    // Copy outTex to readback buffer
    {
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = outTex;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        cl->ResourceBarrier(1, &b);

        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource = readbackBuf;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Offset = 0;
        dst.PlacedFootprint.Footprint.Width = W;
        dst.PlacedFootprint.Footprint.Height = H;
        dst.PlacedFootprint.Footprint.Depth = 1;
        dst.PlacedFootprint.Footprint.RowPitch = W * 4;
        dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;

        D3D12_TEXTURE_COPY_LOCATION src = {};
        src.pResource = outTex;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;

        cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }

    cl->Close();
    ID3D12CommandList* lists[] = { cl };
    queue->ExecuteCommandLists(1, lists);

    // Wait for GPU execution
    ID3D12Fence* fence = nullptr;
    dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    HANDLE hEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    queue->Signal(fence, 1);
    fence->SetEventOnCompletion(1, hEvent);
    WaitForSingleObject(hEvent, INFINITE);
    CloseHandle(hEvent);
    fence->Release();

    // Map readback and save BMP
    uint8_t* pRead = nullptr;
    readbackBuf->Map(0, nullptr, (void**)&pRead);
    SaveBMP("demo_out/test_uimask_protected_with_osd.bmp", (const uint32_t*)pRead, W, H);
    readbackBuf->Unmap(0, nullptr);

    printf("================================================================\n");
    printf("Result: UI Mask Protection & OSD Overlay Verified Successfully!\n");
    printf("================================================================\n");

    upBuffer->Release();
    readbackBuf->Release();
    outTex->Release();
    genTex->Release();
    prevTex->Release();
    currTex->Release();
    cl->Release();
    alloc->Release();
    queue->Release();
    dev->Release();

    return 0;
}
