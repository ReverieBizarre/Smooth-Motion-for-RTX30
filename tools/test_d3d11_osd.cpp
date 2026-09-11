#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <cstdio>
#include <vector>
#include "../src/proxy/osd_overlay.h"

#pragma comment(lib, "d3d11.lib")
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
    printf("  Testing Direct3D 11 In-Game OSD Overlay (MPC-HC Fallback)    \n");
    printf("================================================================\n");

    WNDCLASSA wc = {};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "d3d11_test_window_cls";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowA("d3d11_test_window_cls", "D3D11 OSD Test", WS_OVERLAPPEDWINDOW, 0, 0, 1920, 1080, nullptr, nullptr, wc.hInstance, nullptr);

    DXGI_SWAP_CHAIN_DESC scd = {};
    scd.BufferCount = 1;
    scd.BufferDesc.Width = 1920;
    scd.BufferDesc.Height = 1080;
    scd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.BufferDesc.RefreshRate.Numerator = 60;
    scd.BufferDesc.RefreshRate.Denominator = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = hwnd;
    scd.SampleDesc.Count = 1;
    scd.Windowed = TRUE;

    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    IDXGISwapChain* swap = nullptr;
    D3D_FEATURE_LEVEL fl;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION, &scd, &swap, &dev, &fl, &ctx
    );

    if (FAILED(hr)) {
        printf("[!] D3D11CreateDeviceAndSwapChain failed: 0x%08X\n", (uint32_t)hr);
        return 1;
    }
    printf("[+] D3D11 Device created @ %p, SwapChain @ %p\n", dev, swap);

    // Fill the backbuffer with simulated video playback frame (dark navy blue)
    ID3D11Texture2D* backbuffer = nullptr;
    swap->GetBuffer(0, IID_PPV_ARGS(&backbuffer));
    ID3D11RenderTargetView* rtv = nullptr;
    dev->CreateRenderTargetView(backbuffer, nullptr, &rtv);
    const float clearColor[4] = { 0.08f, 0.12f, 0.22f, 1.0f };
    ctx->ClearRenderTargetView(rtv, clearColor);
    rtv->Release();

    // Initialize and Render D3D11 OSD
    sm86::OsdOverlay osd;
    if (!osd.InitializeD3D11(dev)) {
        printf("[!] osd.InitializeD3D11 failed!\n");
        return 1;
    }
    printf("[+] OsdOverlay::InitializeD3D11 succeeded\n");

    osd.Update(0.59f, "MPC-HC Video (D3D11 DXGI Intercept)", false, false);

    if (!osd.RenderD3D11(swap)) {
        printf("[!] osd.RenderD3D11 failed!\n");
        return 1;
    }
    printf("[+] OsdOverlay::RenderD3D11 executed successfully!\n");

    // Create staging texture to read back backbuffer
    D3D11_TEXTURE2D_DESC stdDesc = {};
    backbuffer->GetDesc(&stdDesc);
    stdDesc.Usage = D3D11_USAGE_STAGING;
    stdDesc.BindFlags = 0;
    stdDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* staging = nullptr;
    dev->CreateTexture2D(&stdDesc, nullptr, &staging);
    ctx->CopyResource(staging, backbuffer);

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    ctx->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
    std::vector<uint32_t> pixels(1920 * 1080);
    for (int y = 0; y < 1080; ++y) {
        memcpy(pixels.data() + y * 1920, (uint8_t*)mapped.pData + y * mapped.RowPitch, 1920 * 4);
    }
    ctx->Unmap(staging, 0);

    SaveBMP("demo_out/test_d3d11_osd.bmp", pixels.data(), 1920, 1080);

    staging->Release();
    backbuffer->Release();
    swap->Release();
    ctx->Release();
    dev->Release();
    DestroyWindow(hwnd);

    printf("================================================================\n");
    printf("Result: Direct3D 11 In-Game OSD Overlay Verified Successfully!  \n");
    printf("================================================================\n");
    return 0;
}
