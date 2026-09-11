// ============================================================================
//  osd_overlay.cpp - Lightweight Direct3D In-Game OSD Implementation
// ============================================================================
#include "osd_overlay.h"

#include <d3dcompiler.h>
#include <cstdio>
#include <cstring>
#include <cmath>

#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "gdi32.lib")

namespace sm86 {

static const char* g_osdShaderSource = R"(
cbuffer OSDParams : register(b0)
{
    uint  DstX;
    uint  DstY;
    uint  OverlayW;
    uint  OverlayH;
    float MasterAlpha;
    uint  BackbufferW;
    uint  BackbufferH;
    uint  Pad;
};

Texture2D<float4>   t_overlay    : register(t0);
RWTexture2D<float4> u_backbuffer : register(u0);

[numthreads(16, 16, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= OverlayW || id.y >= OverlayH)
        return;

    uint dstPx = DstX + id.x;
    uint dstPy = DstY + id.y;

    if (dstPx >= BackbufferW || dstPy >= BackbufferH)
        return;

    float4 src = t_overlay.Load(int3(id.xy, 0));
    float srcAlpha = src.a * MasterAlpha;

    if (srcAlpha <= 0.001f)
        return;

    float4 dst = u_backbuffer[int2(dstPx, dstPy)];
    float3 blended = lerp(dst.rgb, src.rgb, srcAlpha);
    u_backbuffer[int2(dstPx, dstPy)] = float4(blended, dst.a);
}
)";

OsdOverlay::OsdOverlay() {
    QueryPerformanceFrequency(&m_perfFreq);
    QueryPerformanceCounter(&m_lastTime);
}

OsdOverlay::~OsdOverlay() {
    Shutdown();
}

bool OsdOverlay::Initialize(ID3D12Device* device, ID3D12CommandQueue* queue) {
    if (!device) return false;
    m_device = device;
    m_queue = queue;
    m_descriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    InitGdi();
    if (!CreateGpuResources()) return false;

    m_initialized = true;
    return true;
}

void OsdOverlay::Shutdown() {
    if (m_uploadBuffer) { m_uploadBuffer->Unmap(0, nullptr); m_uploadBuffer->Release(); m_uploadBuffer = nullptr; }
    if (m_overlayTex)   { m_overlayTex->Release(); m_overlayTex = nullptr; }
    if (m_srvUavHeap)   { m_srvUavHeap->Release(); m_srvUavHeap = nullptr; }
    if (m_pso)          { m_pso->Release(); m_pso = nullptr; }
    if (m_rootSig)      { m_rootSig->Release(); m_rootSig = nullptr; }

    if (m_memDC) {
        SelectObject(m_memDC, m_hOldBitmap);
        DeleteDC(m_memDC);
        m_memDC = nullptr;
    }
    if (m_hBitmap)    { DeleteObject(m_hBitmap); m_hBitmap = nullptr; }
    if (m_hFontTitle) { DeleteObject(m_hFontTitle); m_hFontTitle = nullptr; }
    if (m_hFontText)  { DeleteObject(m_hFontText); m_hFontText = nullptr; }

    m_device = nullptr;
    m_initialized = false;
}

void OsdOverlay::InitGdi() {
    HDC screenDC = GetDC(nullptr);
    m_memDC = CreateCompatibleDC(screenDC);
    ReleaseDC(nullptr, screenDC);

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = (LONG)OSD_WIDTH;
    bmi.bmiHeader.biHeight = -(LONG)OSD_HEIGHT; // Top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    m_hBitmap = CreateDIBSection(m_memDC, &bmi, DIB_RGB_COLORS, (void**)&m_pBits, nullptr, 0);
    m_hOldBitmap = (HBITMAP)SelectObject(m_memDC, m_hBitmap);

    m_hFontTitle = CreateFontA(-15, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");

    m_hFontText = CreateFontA(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");
}

bool OsdOverlay::CreateGpuResources() {
    // 1. Root Signature
    D3D12_DESCRIPTOR_RANGE ranges[2] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 1;
    ranges[0].BaseShaderRegister = 0; // t0
    ranges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 1;
    ranges[1].BaseShaderRegister = 0; // u0
    ranges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER params[3] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;
    params[0].Constants.Num32BitValues = sizeof(OsdParams) / 4;

    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &ranges[0];

    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable.NumDescriptorRanges = 1;
    params[2].DescriptorTable.pDescriptorRanges = &ranges[1];

    D3D12_ROOT_SIGNATURE_DESC sigDesc = {};
    sigDesc.NumParameters = 3;
    sigDesc.pParameters = params;

    ID3DBlob* blob = nullptr;
    ID3DBlob* err = nullptr;
    if (FAILED(D3D12SerializeRootSignature(&sigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err))) {
        if (err) err->Release();
        return false;
    }
    m_device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&m_rootSig));
    blob->Release();

    // 2. Pipeline State
    ID3DBlob* csBlob = nullptr;
    UINT compileFlags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
    HRESULT hr = D3DCompileFromFile(L"shaders\\osd.hlsl", nullptr, nullptr, "CSMain", "cs_5_0", compileFlags, 0, &csBlob, &err);
    if (FAILED(hr)) {
        if (err) { err->Release(); err = nullptr; }
        hr = D3DCompile(g_osdShaderSource, strlen(g_osdShaderSource), "osd.hlsl", nullptr, nullptr, "CSMain", "cs_5_0", compileFlags, 0, &csBlob, &err);
    }
    if (FAILED(hr)) {
        if (err) err->Release();
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = m_rootSig;
    psoDesc.CS = { csBlob->GetBufferPointer(), csBlob->GetBufferSize() };
    m_device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&m_pso));
    csBlob->Release();

    // 3. Descriptor Heap (1 SRV + 1 UAV)
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.NumDescriptors = 2;
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    m_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_srvUavHeap));

    // 4. Overlay GPU Texture (B8G8R8A8_UNORM)
    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = OSD_WIDTH;
    texDesc.Height = OSD_HEIGHT;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    D3D12_HEAP_PROPERTIES defHeap = {};
    defHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    m_device->CreateCommittedResource(&defHeap, D3D12_HEAP_FLAG_NONE, &texDesc,
                                      D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_overlayTex));

    // 5. Upload Buffer
    UINT64 uploadSize = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    m_device->GetCopyableFootprints(&texDesc, 0, 1, 0, &footprint, nullptr, nullptr, &uploadSize);

    D3D12_HEAP_PROPERTIES upHeap = {};
    upHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bufDesc = {};
    bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufDesc.Width = uploadSize;
    bufDesc.Height = 1;
    bufDesc.DepthOrArraySize = 1;
    bufDesc.MipLevels = 1;
    bufDesc.SampleDesc.Count = 1;
    bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    m_device->CreateCommittedResource(&upHeap, D3D12_HEAP_FLAG_NONE, &bufDesc,
                                      D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_uploadBuffer));

    m_uploadBuffer->Map(0, nullptr, (void**)&m_mappedUpload);
    return true;
}

void OsdOverlay::Update(float frameGenMs, const char* engineName, bool uiMaskEnabled, bool debugHeatmap) {
    // 1. Hotkey Handling
    if (GetAsyncKeyState(VK_F11) & 1) {
        m_visible = !m_visible;
    }
    if (GetAsyncKeyState(VK_F10) & 1) {
        m_uiMaskEnabled = !m_uiMaskEnabled;
    }
    if (GetAsyncKeyState(VK_F9) & 1) {
        m_debugHeatmap = !m_debugHeatmap;
    }

    // 2. FPS Measurement
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double dt = (double)(now.QuadPart - m_lastTime.QuadPart) / (double)m_perfFreq.QuadPart;
    m_lastTime = now;

    if (dt > 0.0001) {
        float instantFps = (float)(1.0 / dt);
        m_fpsHistory[m_fpsIndex] = instantFps;
        m_fpsIndex = (m_fpsIndex + 1) % 30;

        float sum = 0.0f;
        for (int i = 0; i < 30; ++i) sum += m_fpsHistory[i];
        m_currentFps = sum / 30.0f;
    }
    m_frameCount++;

    // 3. Re-render GDI Surface
    RenderGdiSurface(frameGenMs, engineName, uiMaskEnabled, debugHeatmap);
}

void OsdOverlay::RenderGdiSurface(float frameGenMs, const char* engineName, bool uiMaskEnabled, bool debugHeatmap) {
    if (!m_pBits) return;

    // Fill background with dark tinted glass
    // In 32-bit BGRA: B=0x18, G=0x12, R=0x10, A=0xD8
    const uint32_t bgPixel = 0xD8101218;
    for (size_t i = 0; i < OSD_WIDTH * OSD_HEIGHT; ++i) {
        m_pBits[i] = bgPixel;
    }

    // Draw NVIDIA green outline border
    HPEN hPen = CreatePen(PS_SOLID, 2, RGB(118, 185, 0));
    HPEN hOldPen = (HPEN)SelectObject(m_memDC, hPen);
    HBRUSH hOldBrush = (HBRUSH)SelectObject(m_memDC, GetStockObject(NULL_BRUSH));
    RoundRect(m_memDC, 2, 2, OSD_WIDTH - 2, OSD_HEIGHT - 2, 10, 10);
    SelectObject(m_memDC, hOldPen);
    SelectObject(m_memDC, hOldBrush);
    DeleteObject(hPen);

    SetBkMode(m_memDC, TRANSPARENT);

    // Line 0: Header / Brand
    SelectObject(m_memDC, m_hFontTitle);
    SetTextColor(m_memDC, RGB(118, 185, 0));
    TextOutA(m_memDC, 16, 12, "[sm86_smooth]", 13);
    SetTextColor(m_memDC, RGB(240, 240, 240));
    TextOutA(m_memDC, 135, 12, "NVIDIA RTX 30-Series (sm_86 Ampere)", 35);

    // Line 1: Engine info
    SelectObject(m_memDC, m_hFontText);
    char buf[128];
    snprintf(buf, sizeof(buf), "Engine : %s", engineName ? engineName : "Road 1 (NvPresent64 Rehost)");
    SetTextColor(m_memDC, RGB(200, 210, 225));
    TextOutA(m_memDC, 16, 38, buf, (int)strlen(buf));

    // Line 2: FPS & Latency
    float baseFps = m_currentFps * 0.5f;
    snprintf(buf, sizeof(buf), "Display: %5.1f FPS (Base: %4.1f) | FG Latency: %4.2f ms",
             m_currentFps, baseFps, frameGenMs);
    SetTextColor(m_memDC, RGB(255, 220, 80));
    TextOutA(m_memDC, 16, 62, buf, (int)strlen(buf));

    // Line 3: UI Mask Status
    snprintf(buf, sizeof(buf), "UI Mask: [F10] %s (HUD/Crosshair Protection)",
             uiMaskEnabled ? "ENABLED " : "DISABLED");
    SetTextColor(m_memDC, uiMaskEnabled ? RGB(0, 230, 210) : RGB(160, 160, 160));
    TextOutA(m_memDC, 16, 86, buf, (int)strlen(buf));

    // Line 4: Heatmap & Controls
    snprintf(buf, sizeof(buf), "Hotkeys: [F11] Hide OSD | [F9] Heatmap: %s",
             debugHeatmap ? "ON (Red View)" : "OFF");
    SetTextColor(m_memDC, debugHeatmap ? RGB(255, 80, 80) : RGB(170, 175, 190));
    TextOutA(m_memDC, 16, 112, buf, (int)strlen(buf));

    // Post-process alpha channel: Ensure text pixels are fully opaque (Alpha = 0xFF)
    for (size_t i = 0; i < OSD_WIDTH * OSD_HEIGHT; ++i) {
        if (m_pBits[i] != bgPixel) {
            m_pBits[i] |= 0xFF000000;
        }
    }
}

bool OsdOverlay::Record(ID3D12GraphicsCommandList* cl,
                        ID3D12Resource* backbuffer,
                        uint32_t backbufferW, uint32_t backbufferH) {
    if (!m_initialized || !m_visible || !cl || !backbuffer)
        return false;

    // 1. Copy DIB image to mapped upload buffer
    D3D12_RESOURCE_DESC texDesc = m_overlayTex->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT64 totalBytes = 0;
    m_device->GetCopyableFootprints(&texDesc, 0, 1, 0, &footprint, nullptr, nullptr, &totalBytes);

    for (uint32_t y = 0; y < OSD_HEIGHT; ++y) {
        memcpy(m_mappedUpload + footprint.Offset + y * footprint.Footprint.RowPitch,
               m_pBits + y * OSD_WIDTH,
               OSD_WIDTH * 4);
    }

    // 2. Transition overlay texture to COPY_DEST, copy from upload buffer
    D3D12_RESOURCE_BARRIER b1 = {};
    b1.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b1.Transition.pResource = m_overlayTex;
    b1.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b1.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    b1.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    cl->ResourceBarrier(1, &b1);

    D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
    dstLoc.pResource = m_overlayTex;
    dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dstLoc.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
    srcLoc.pResource = m_uploadBuffer;
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcLoc.PlacedFootprint = footprint;

    cl->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

    // 3. Transition overlay texture to NON_PIXEL_SHADER_RESOURCE
    b1.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b1.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    cl->ResourceBarrier(1, &b1);

    // 4. Setup Descriptors (t0 for overlay, u0 for backbuffer)
    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = m_srvUavHeap->GetCPUDescriptorHandleForHeapStart();

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    m_device->CreateShaderResourceView(m_overlayTex, &srvDesc, cpuHandle);
    cpuHandle.ptr += m_descriptorSize;

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.Format = backbuffer->GetDesc().Format;
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    m_device->CreateUnorderedAccessView(backbuffer, nullptr, &uavDesc, cpuHandle);

    // 5. Dispatch Compute Shader
    cl->SetComputeRootSignature(m_rootSig);
    cl->SetPipelineState(m_pso);

    ID3D12DescriptorHeap* heaps[] = { m_srvUavHeap };
    cl->SetDescriptorHeaps(1, heaps);

    OsdParams p = {};
    p.DstX = 24;
    p.DstY = 24;
    p.OverlayW = OSD_WIDTH;
    p.OverlayH = OSD_HEIGHT;
    p.MasterAlpha = 0.95f;
    p.BackbufferW = backbufferW;
    p.BackbufferH = backbufferH;

    cl->SetComputeRoot32BitConstants(0, sizeof(OsdParams) / 4, &p, 0);

    D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = m_srvUavHeap->GetGPUDescriptorHandleForHeapStart();
    cl->SetComputeRootDescriptorTable(1, gpuHandle); // t0

    gpuHandle.ptr += m_descriptorSize;
    cl->SetComputeRootDescriptorTable(2, gpuHandle); // u0

    cl->Dispatch((OSD_WIDTH + 15) / 16, (OSD_HEIGHT + 15) / 16, 1);
    return true;
}

} // namespace sm86
