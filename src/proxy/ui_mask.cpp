// ============================================================================
//  ui_mask.cpp - Pass-Level UI Mask Protection Engine Implementation
// ============================================================================
#include "ui_mask.h"

#include <d3dcompiler.h>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "d3dcompiler.lib")

namespace sm86 {

// Embedded fallback source code for ui_mask.hlsl
static const char* g_uiMaskShaderSource = R"(
cbuffer UIMaskParams : register(b0)
{
    uint   Width;
    uint   Height;
    float  Sensitivity;
    float  BlendGain;
    uint   Flags;
    float  CrosshairRadius;
    float  CornerRadius;
    float  Pad0;
};

Texture2D<float4>   t_currFrame : register(t0);
Texture2D<float4>   t_prevFrame : register(t1);
Texture2D<float4>   t_genFrame  : register(t2);
RWTexture2D<float4> u_outFrame  : register(u0);

float GetLuma(float3 rgb)
{
    return dot(rgb, float3(0.2126f, 0.7152f, 0.0722f));
}

[numthreads(16, 16, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= Width || id.y >= Height)
        return;

    int2 pos = int2(id.xy);
    float4 currColor = t_currFrame.Load(int3(pos, 0));
    float4 prevColor = t_prevFrame.Load(int3(pos, 0));
    float4 genColor  = t_genFrame.Load(int3(pos, 0));

    if ((Flags & 1) == 0)
    {
        u_outFrame[pos] = genColor;
        return;
    }

    float2 uv = float2(id.x + 0.5f, id.y + 0.5f) / float2(Width, Height);

    float3 colDiff = abs(currColor.rgb - prevColor.rgb);
    float maxDiff  = max(colDiff.r, max(colDiff.g, colDiff.b));

    float lumaL = GetLuma(t_currFrame.Load(int3(max(pos.x - 1, 0), pos.y, 0)).rgb);
    float lumaR = GetLuma(t_currFrame.Load(int3(min(pos.x + 1, (int)Width - 1), pos.y, 0)).rgb);
    float lumaU = GetLuma(t_currFrame.Load(int3(pos.x, max(pos.y - 1, 0), 0)).rgb);
    float lumaD = GetLuma(t_currFrame.Load(int3(pos.x, min(pos.y + 1, (int)Height - 1), 0)).rgb);
    float edgeGrad = (abs(lumaR - lumaL) + abs(lumaD - lumaU)) * 0.5f;

    float staticScore = saturate(1.0f - (maxDiff / max(Sensitivity, 0.001f)));

    float2 centerDist = (uv - float2(0.5f, 0.5f)) * float2((float)Width / (float)Height, 1.0f);
    float distSq = dot(centerDist, centerDist);
    float isCrosshairZone = ((Flags & 4) != 0 && distSq < (CrosshairRadius * CrosshairRadius)) ? 1.0f : 0.0f;

    float isCornerZone = (uv.x < CornerRadius || uv.x > (1.0f - CornerRadius) ||
                          uv.y < CornerRadius || uv.y > (1.0f - CornerRadius)) ? 0.4f : 0.0f;

    float uiProb = 0.0f;
    if ((Flags & 8) != 0)
    {
        // Ground-Truth Clean Pass Interception mode (RenoDX / DLSS 5 style)
        float3 passDiff = abs(currColor.rgb - prevColor.rgb);
        float maxPassDiff = max(passDiff.r, max(passDiff.g, passDiff.b));
        float isDiff = (maxPassDiff > Sensitivity) ? 1.0f : 0.0f;
        uiProb = isDiff * saturate((maxPassDiff - Sensitivity) * 15.0f + 0.6f);
        if (isCrosshairZone > 0.0f && maxPassDiff > 0.015f)
            uiProb = 1.0f;
    }
    else
    {
        if (isCrosshairZone > 0.0f)
        {
            uiProb = staticScore;
        }
        else if (isCornerZone > 0.0f)
        {
            uiProb = staticScore * saturate(edgeGrad * 4.0f + 0.85f);
        }
        else
        {
            uiProb = staticScore * saturate(edgeGrad * 5.0f + 0.25f);
        }
    }

    float mask = saturate(uiProb * BlendGain);

    if ((Flags & 2) != 0)
    {
        float3 debugView = lerp(genColor.rgb * 0.4f, float3(1.0f, 0.1f, 0.15f), mask);
        u_outFrame[pos] = float4(debugView, 1.0f);
        return;
    }

    float3 finalRgb = lerp(genColor.rgb, currColor.rgb, mask);
    u_outFrame[pos] = float4(finalRgb, currColor.a);
}
)";

UiMaskEngine::UiMaskEngine() = default;

UiMaskEngine::~UiMaskEngine() {
    Shutdown();
}

bool UiMaskEngine::Initialize(ID3D12Device* device) {
    if (!device) return false;
    m_device = device;
    m_descriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    if (!CreateRootSignature()) return false;
    if (!CreatePipelineState()) return false;

    // Descriptor heap for 3 SRVs + 1 UAV
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.NumDescriptors = 4;
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    HRESULT hr = m_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_srvUavHeap));
    if (FAILED(hr)) return false;

    m_initialized = true;
    return true;
}

void UiMaskEngine::Shutdown() {
    if (m_srvUavHeap) { m_srvUavHeap->Release(); m_srvUavHeap = nullptr; }
    if (m_pso)        { m_pso->Release(); m_pso = nullptr; }
    if (m_rootSig)    { m_rootSig->Release(); m_rootSig = nullptr; }
    m_device = nullptr;
    m_initialized = false;
}

bool UiMaskEngine::CreateRootSignature() {
    // 3 Parameters:
    // Param 0: 32-bit constants (b0) - 8 DWORDs
    // Param 1: Descriptor Table (t0..t2) - 3 SRVs
    // Param 2: Descriptor Table (u0) - 1 UAV
    D3D12_DESCRIPTOR_RANGE ranges[2] = {};
    // SRVs
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 3;
    ranges[0].BaseShaderRegister = 0;
    ranges[0].RegisterSpace = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    // UAVs
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 1;
    ranges[1].BaseShaderRegister = 0;
    ranges[1].RegisterSpace = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER params[3] = {};
    // Param 0: Root Constants
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;
    params[0].Constants.RegisterSpace = 0;
    params[0].Constants.Num32BitValues = sizeof(UiMaskConstants) / 4;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // Param 1: SRVs Table
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &ranges[0];
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // Param 2: UAV Table
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable.NumDescriptorRanges = 1;
    params[2].DescriptorTable.pDescriptorRanges = &ranges[1];
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.NumParameters = 3;
    desc.pParameters = params;
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ID3DBlob* blob = nullptr;
    ID3DBlob* err = nullptr;
    HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
    if (FAILED(hr)) {
        if (err) err->Release();
        return false;
    }

    hr = m_device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&m_rootSig));
    blob->Release();
    return SUCCEEDED(hr);
}

bool UiMaskEngine::CreatePipelineState() {
    ID3DBlob* csBlob = nullptr;
    ID3DBlob* err = nullptr;
    UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_ENABLE_STRICTNESS;

    // Try compiling from file first, fallback to embedded source
    HRESULT hr = D3DCompileFromFile(L"shaders\\ui_mask.hlsl", nullptr, nullptr, "CSMain", "cs_5_0", flags, 0, &csBlob, &err);
    if (FAILED(hr)) {
        if (err) { err->Release(); err = nullptr; }
        hr = D3DCompile(g_uiMaskShaderSource, strlen(g_uiMaskShaderSource), "ui_mask.hlsl", nullptr, nullptr,
                        "CSMain", "cs_5_0", flags, 0, &csBlob, &err);
    }
    if (FAILED(hr)) {
        if (err) {
            printf("[ui_mask] Shader compile error: %s\n", (char*)err->GetBufferPointer());
            err->Release();
        }
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = m_rootSig;
    psoDesc.CS = { csBlob->GetBufferPointer(), csBlob->GetBufferSize() };

    hr = m_device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&m_pso));
    csBlob->Release();
    return SUCCEEDED(hr);
}

bool UiMaskEngine::Record(ID3D12GraphicsCommandList* cl,
                          ID3D12Resource* currFrame,
                          ID3D12Resource* prevFrame,
                          ID3D12Resource* genFrame,
                          ID3D12Resource* outFrame,
                          uint32_t width, uint32_t height,
                          const UiMaskConfig& config) {
    if (!m_initialized || !cl || !currFrame || !prevFrame || !genFrame || !outFrame)
        return false;

    // Setup Descriptors:
    // Slot 0..2: SRVs for curr, prev, gen
    // Slot 3: UAV for outFrame
    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = m_srvUavHeap->GetCPUDescriptorHandleForHeapStart();

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = currFrame->GetDesc().Format;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;

    // Curr Frame (t0)
    m_device->CreateShaderResourceView(currFrame, &srvDesc, cpuHandle);
    cpuHandle.ptr += m_descriptorSize;

    // Prev Frame (t1)
    srvDesc.Format = prevFrame->GetDesc().Format;
    m_device->CreateShaderResourceView(prevFrame, &srvDesc, cpuHandle);
    cpuHandle.ptr += m_descriptorSize;

    // Gen Frame (t2)
    srvDesc.Format = genFrame->GetDesc().Format;
    m_device->CreateShaderResourceView(genFrame, &srvDesc, cpuHandle);
    cpuHandle.ptr += m_descriptorSize;

    // Output UAV (u0)
    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.Format = outFrame->GetDesc().Format;
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    m_device->CreateUnorderedAccessView(outFrame, nullptr, &uavDesc, cpuHandle);

    // Setup Constants
    UiMaskConstants c = {};
    c.Width = width;
    c.Height = height;
    c.Sensitivity = config.sensitivity;
    c.BlendGain = config.blendGain;
    c.Flags = (config.enabled ? 1 : 0) |
              (config.debugHeatmap ? 2 : 0) |
              (config.crosshairBoost ? 4 : 0) |
              (config.groundTruthPass ? 8 : 0);
    c.CrosshairRadius = config.crosshairRadius;
    c.CornerRadius = config.cornerMargin;

    // Bind Pipeline & Heap
    cl->SetComputeRootSignature(m_rootSig);
    cl->SetPipelineState(m_pso);

    ID3D12DescriptorHeap* heaps[] = { m_srvUavHeap };
    cl->SetDescriptorHeaps(1, heaps);

    // Param 0: Root Constants
    cl->SetComputeRoot32BitConstants(0, sizeof(UiMaskConstants) / 4, &c, 0);

    // Param 1: SRVs Table (offset 0)
    D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = m_srvUavHeap->GetGPUDescriptorHandleForHeapStart();
    cl->SetComputeRootDescriptorTable(1, gpuHandle);

    // Param 2: UAV Table (offset 3)
    gpuHandle.ptr += m_descriptorSize * 3;
    cl->SetComputeRootDescriptorTable(2, gpuHandle);

    // Dispatch 16x16
    uint32_t gx = (width + 15) / 16;
    uint32_t gy = (height + 15) / 16;
    cl->Dispatch(gx, gy, 1);

    return true;
}

} // namespace sm86
