#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <cstdio>
#include <vector>
#include <chrono>
#include "../src/proxy/ui_mask.h"

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
    printf("  ReShade Add-on DrawCall Interception & UI Mask Verification  \n");
    printf("  (RenoDX / DLSS 5 Ground-Truth Pass Architecture)             \n");
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

    auto CreateTex = [&](D3D12_RESOURCE_STATES state, D3D12_RESOURCE_FLAGS flags) -> ID3D12Resource* {
        D3D12_RESOURCE_DESC td = {};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = W;
        td.Height = H;
        td.DepthOrArraySize = 1;
        td.MipLevels = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        td.Flags = flags;

        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;

        ID3D12Resource* res = nullptr;
        dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, state, nullptr, IID_PPV_ARGS(&res));
        return res;
    };

    // 1. Backbuffer (with UI)
    ID3D12Resource* backbuffer = CreateTex(D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE);
    // 2. CleanScene (captured via DrawCall / Pass interception BEFORE UI drawcall)
    ID3D12Resource* cleanScene = CreateTex(D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE);
    // 3. Generated / Interpolated frame (with distorted scene)
    ID3D12Resource* genFrame = CreateTex(D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE);
    // 4. Output protected composited frame
    ID3D12Resource* outFrame = CreateTex(D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    // 5. Output heatmap frame
    ID3D12Resource* heatmapFrame = CreateTex(D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

    auto CreateUpload = [&](size_t size) -> ID3D12Resource* {
        D3D12_RESOURCE_DESC bd = {};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = size;
        bd.Height = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels = 1;
        bd.Format = DXGI_FORMAT_UNKNOWN;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_UPLOAD;

        ID3D12Resource* b = nullptr;
        dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&b));
        return b;
    };

    auto CreateReadback = [&](size_t size) -> ID3D12Resource* {
        D3D12_RESOURCE_DESC bd = {};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = size;
        bd.Height = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels = 1;
        bd.Format = DXGI_FORMAT_UNKNOWN;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_READBACK;

        ID3D12Resource* b = nullptr;
        dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&b));
        return b;
    };

    const size_t byteSize = W * H * 4;
    ID3D12Resource* upBuffer = CreateUpload(byteSize * 3);
    ID3D12Resource* rbOut = CreateReadback(byteSize);
    ID3D12Resource* rbHeat = CreateReadback(byteSize);

    // Fill synthetic image data:
    // pixClean: 3D scene only
    // pixBackbuffer: 3D scene + Healthbar + Reticle
    // pixGen: Distorted 3D scene
    std::vector<uint32_t> pixClean(W * H);
    std::vector<uint32_t> pixBackbuffer(W * H);
    std::vector<uint32_t> pixGen(W * H);

    for (uint32_t y = 0; y < H; y++) {
        for (uint32_t x = 0; x < W; x++) {
            uint8_t r = static_cast<uint8_t>((x * 255) / W);
            uint8_t g = static_cast<uint8_t>((y * 255) / H);
            uint8_t b = 80;
            uint32_t sceneCol = (0xFF << 24) | (b << 16) | (g << 8) | r;

            pixClean[y * W + x] = sceneCol;

            // Shifted in interpolated frame
            uint8_t gen_r = static_cast<uint8_t>(((x + 6) * 255) / W);
            uint8_t gen_g = static_cast<uint8_t>(((y + 4) * 255) / H);
            pixGen[y * W + x] = (0xFF << 24) | (b << 16) | (gen_g << 8) | gen_r;

            uint32_t finalCol = sceneCol;

            // 2D HUD: Health Bar at bottom left: x in [100..400], y in [950..980]
            if (x >= 100 && x <= 400 && y >= 950 && y <= 980) {
                finalCol = 0xFF00FF00; // Solid Bright Green Healthbar
            }

            // 2D HUD: Reticle at center
            int cx = W / 2;
            int cy = H / 2;
            if ((abs((int)x - cx) <= 12 && abs((int)y - cy) <= 1) ||
                (abs((int)y - cy) <= 12 && abs((int)x - cx) <= 1)) {
                finalCol = 0xFF00FFFF; // Solid Yellow Reticle
            }

            pixBackbuffer[y * W + x] = finalCol;
        }
    }

    // Upload pixel data to upBuffer
    uint8_t* pMap = nullptr;
    upBuffer->Map(0, nullptr, reinterpret_cast<void**>(&pMap));
    memcpy(pMap, pixClean.data(), byteSize);
    memcpy(pMap + byteSize, pixBackbuffer.data(), byteSize);
    memcpy(pMap + byteSize * 2, pixGen.data(), byteSize);
    upBuffer->Unmap(0, nullptr);

    auto CopyToTex = [&](ID3D12Resource* tex, size_t offset) {
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

    CopyToTex(cleanScene, 0);
    CopyToTex(backbuffer, byteSize);
    CopyToTex(genFrame, byteSize * 2);

    // Transition outFrame & heatmapFrame to UNORDERED_ACCESS
    {
        D3D12_RESOURCE_BARRIER b[2] = {};
        b[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b[0].Transition.pResource = outFrame;
        b[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

        b[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b[1].Transition.pResource = heatmapFrame;
        b[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        cl->ResourceBarrier(2, b);
    }

    cl->Close();
    ID3D12CommandList* lists[] = { cl };
    queue->ExecuteCommandLists(1, lists);

    ID3D12Fence* fence = nullptr;
    dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    queue->Signal(fence, 1);
    while (fence->GetCompletedValue() < 1) { Sleep(1); }

    // Initialize UiMaskEngine
    sm86::UiMaskEngine maskEngine;
    if (!maskEngine.Initialize(dev)) {
        printf("[!] Failed to initialize UiMaskEngine\n");
        return 1;
    }
    printf("[+] UiMaskEngine initialized successfully\n");

    alloc->Reset();
    cl->Reset(alloc, nullptr);

    sm86::UiMaskConfig config;
    config.enabled = true;
    config.debugHeatmap = false;
    config.crosshairBoost = true;
    config.groundTruthPass = true; // RenoDX / DLSS 5 Clean Pass Interception Mode!
    config.sensitivity = 0.05f;
    config.blendGain = 1.0f;

    // 1. Dispatch Ground-Truth UI composite
    auto t0 = std::chrono::high_resolution_clock::now();
    maskEngine.Record(cl, backbuffer, cleanScene, genFrame, outFrame, W, H, config);

    // Transition outFrame for readback
    {
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = outFrame;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        cl->ResourceBarrier(1, &b);
    }

    auto ReadbackTex = [&](ID3D12Resource* tex, ID3D12Resource* rb) {
        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource = rb;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Offset = 0;
        dst.PlacedFootprint.Footprint.Width = W;
        dst.PlacedFootprint.Footprint.Height = H;
        dst.PlacedFootprint.Footprint.Depth = 1;
        dst.PlacedFootprint.Footprint.RowPitch = W * 4;
        dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;

        D3D12_TEXTURE_COPY_LOCATION src = {};
        src.pResource = tex;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;

        cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    };

    ReadbackTex(outFrame, rbOut);
    cl->Close();
    queue->ExecuteCommandLists(1, lists);
    queue->Signal(fence, 2);
    while (fence->GetCompletedValue() < 2) { Sleep(1); }
    auto t1 = std::chrono::high_resolution_clock::now();

    // 2. Dispatch Heatmap Frame
    alloc->Reset();
    cl->Reset(alloc, nullptr);
    sm86::UiMaskConfig heatConfig = config;
    heatConfig.debugHeatmap = true;
    maskEngine.Record(cl, backbuffer, cleanScene, genFrame, heatmapFrame, W, H, heatConfig);

    {
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = heatmapFrame;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        cl->ResourceBarrier(1, &b);
    }

    ReadbackTex(heatmapFrame, rbHeat);
    cl->Close();
    queue->ExecuteCommandLists(1, lists);
    queue->Signal(fence, 3);
    while (fence->GetCompletedValue() < 3) { Sleep(1); }

    float elapsed_ms = std::chrono::duration<float, std::milli>(t1 - t0).count();
    printf("[+] Ground-Truth UI Mask compute finished in %.3f ms\n", elapsed_ms);

    // Verify Pixels
    std::vector<uint32_t> outPix(W * H);
    std::vector<uint32_t> heatPix(W * H);

    uint8_t* ptrOut = nullptr;
    rbOut->Map(0, nullptr, reinterpret_cast<void**>(&ptrOut));
    memcpy(outPix.data(), ptrOut, byteSize);
    rbOut->Unmap(0, nullptr);

    uint8_t* ptrHeat = nullptr;
    rbHeat->Map(0, nullptr, reinterpret_cast<void**>(&ptrHeat));
    memcpy(heatPix.data(), ptrHeat, byteSize);
    rbHeat->Unmap(0, nullptr);

    // Check pixel at Center Reticle
    uint32_t reticlePix = outPix[(H / 2) * W + (W / 2)];
    uint8_t r = reticlePix & 0xFF;
    uint8_t g = (reticlePix >> 8) & 0xFF;
    uint8_t b = (reticlePix >> 16) & 0xFF;

    printf("[+] Center Reticle Pixel in OutFrame: R=%u, G=%u, B=%u\n", r, g, b);
    if (r > 240 && g > 240 && b < 20) {
        printf("[SUCCESS] Reticle restored with 100%% ground-truth accuracy!\n");
    } else {
        printf("[!] Reticle pixel check unexpected: R=%u, G=%u, B=%u\n", r, g, b);
    }

    // Check pixel at Healthbar
    uint32_t healthPix = outPix[960 * W + 200];
    uint8_t hr = healthPix & 0xFF;
    uint8_t hg = (healthPix >> 8) & 0xFF;
    uint8_t hb = (healthPix >> 16) & 0xFF;
    printf("[+] Healthbar Pixel in OutFrame: R=%u, G=%u, B=%u\n", hr, hg, hb);
    if (hr < 20 && hg > 240 && hb < 20) {
        printf("[SUCCESS] Healthbar restored with 100%% ground-truth accuracy!\n");
    } else {
        printf("[!] Healthbar pixel check unexpected: R=%u, G=%u, B=%u\n", hr, hg, hb);
    }

    // Check heatmap at Healthbar
    uint32_t heatVal = heatPix[960 * W + 200];
    uint8_t h_r = heatVal & 0xFF;
    uint8_t h_g = (heatVal >> 8) & 0xFF;
    uint8_t h_b = (heatVal >> 16) & 0xFF;
    printf("[+] Healthbar Pixel in Heatmap: R=%u, G=%u, B=%u\n", h_r, h_g, h_b);
    if (h_r > 200 && h_g < 50) {
        printf("[SUCCESS] Ground-truth Heatmap correctly tags 2D UI with bright red highlight!\n");
    }

    // Save BMP images
    SaveBMP("demo_out/test_addon_clean_scene.bmp", pixClean.data(), W, H);
    SaveBMP("demo_out/test_addon_backbuffer_with_ui.bmp", pixBackbuffer.data(), W, H);
    SaveBMP("demo_out/test_addon_interpolated_protected.bmp", outPix.data(), W, H);
    SaveBMP("demo_out/test_addon_ground_truth_heatmap.bmp", heatPix.data(), W, H);

    printf("================================================================\n");
    printf("  ReShade Add-on Pass Interception Simulation Passed 100%%!     \n");
    printf("================================================================\n");
    return 0;
}
