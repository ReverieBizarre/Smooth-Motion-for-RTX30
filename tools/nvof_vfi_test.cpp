// ============================================================================
//  nvof_vfi_test - the payoff: hardware optical flow feeding MY warp/blend.
//
//  Renders a synthetic scene (translation + a disc that occludes + a rotating
//  bar) at t=0/0.5/1, runs NVOFA grid-4 on the two frames to get a real motion
//  field, upscales it to full resolution, and feeds it through the VFI engine's
//  set_flow_override() path - the same blend the software matcher uses.  Then
//  reports PSNR against the ground-truth midpoint and the time.
//
//  This isolates the question that matters: with a hardware flow field instead
//  of my block matcher, what does the pipeline achieve?
// ============================================================================
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include <algorithm>
#include <cmath>

#include "nvOpticalFlowD3D12.h"
#include "vfi.h"
#include <d3dcompiler.h>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

using namespace sm86;
static const uint32_t W = 1920, H = 1080, GRID = 4;

// ---- D3D12 helper (compact) ----
struct Gpu {
    ID3D12Device* dev=nullptr; ID3D12CommandQueue* q=nullptr;
    ID3D12CommandAllocator* alloc=nullptr; ID3D12GraphicsCommandList* list=nullptr;
    ID3D12Fence* fence=nullptr; UINT64 fv=0; HANDLE ev=nullptr;
    bool init(){
        if(FAILED(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&dev))))return false;
        D3D12_COMMAND_QUEUE_DESC qd={};qd.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
        if(FAILED(dev->CreateCommandQueue(&qd,IID_PPV_ARGS(&q))))return false;
        if(FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&alloc))))return false;
        if(FAILED(dev->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,alloc,nullptr,IID_PPV_ARGS(&list))))return false;
        list->Close();
        if(FAILED(dev->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence))))return false;
        ev=CreateEvent(nullptr,FALSE,FALSE,nullptr);return true;
    }
    void submit(){q->ExecuteCommandLists(1,(ID3D12CommandList**)&list);++fv;q->Signal(fence,fv);}
    void flush(){submit();fence->SetEventOnCompletion(fv,ev);WaitForSingleObject(ev,INFINITE);}
    void transition(ID3D12Resource* r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b){
        alloc->Reset();list->Reset(alloc,nullptr);
        D3D12_RESOURCE_BARRIER br={};br.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        br.Transition.pResource=r;br.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        br.Transition.StateBefore=a;br.Transition.StateAfter=b;
        list->ResourceBarrier(1,&br);list->Close();flush();
    }
    ID3D12Resource* tex(DXGI_FORMAT fmt,uint32_t w,uint32_t h,D3D12_RESOURCE_STATES st){
        D3D12_HEAP_PROPERTIES hp={};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd={};rd.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width=w;rd.Height=h;rd.DepthOrArraySize=1;rd.MipLevels=1;
        rd.Format=fmt;rd.SampleDesc.Count=1;
        rd.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ID3D12Resource* r=nullptr;
        dev->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&rd,st,nullptr,IID_PPV_ARGS(&r));
        return r;
    }
    void upload(ID3D12Resource* t,const void* data,uint32_t w,uint32_t h,uint32_t bpp){
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp={};UINT rows=0;UINT64 rs=0,tot=0;
        dev->GetCopyableFootprints(&t->GetDesc(),0,1,0,&fp,&rows,&rs,&tot);
        D3D12_HEAP_PROPERTIES up={};up.Type=D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bd={};bd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width=tot;bd.Height=1;bd.DepthOrArraySize=1;bd.MipLevels=1;
        bd.Format=DXGI_FORMAT_UNKNOWN;bd.SampleDesc.Count=1;bd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ID3D12Resource* ub=nullptr;
        dev->CreateCommittedResource(&up,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&ub));
        void* p=nullptr;ub->Map(0,nullptr,&p);
        for(UINT y=0;y<rows;++y)memcpy((uint8_t*)p+fp.Offset+y*fp.Footprint.RowPitch,(const uint8_t*)data+(size_t)y*w*bpp,(size_t)w*bpp);
        ub->Unmap(0,nullptr);
        alloc->Reset();list->Reset(alloc,nullptr);
        D3D12_TEXTURE_COPY_LOCATION dst={};dst.pResource=t;dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION src={};src.pResource=ub;src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint=fp;
        list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);list->Close();flush();ub->Release();
    }
    void readback(ID3D12Resource* t,void* out,uint32_t w,uint32_t h,uint32_t bpp){
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp={};UINT rows=0;UINT64 rs=0,tot=0;
        dev->GetCopyableFootprints(&t->GetDesc(),0,1,0,&fp,&rows,&rs,&tot);
        D3D12_HEAP_PROPERTIES rb={};rb.Type=D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC bd={};bd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width=tot;bd.Height=1;bd.DepthOrArraySize=1;bd.MipLevels=1;
        bd.Format=DXGI_FORMAT_UNKNOWN;bd.SampleDesc.Count=1;bd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ID3D12Resource* bb=nullptr;
        dev->CreateCommittedResource(&rb,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&bb));
        transition(t,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);
        alloc->Reset();list->Reset(alloc,nullptr);
        D3D12_TEXTURE_COPY_LOCATION dst={};dst.pResource=bb;dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=fp;
        D3D12_TEXTURE_COPY_LOCATION src={};src.pResource=t;src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);list->Close();flush();
        void* p=nullptr;bb->Map(0,nullptr,&p);
        for(UINT y=0;y<h;++y)memcpy((uint8_t*)out+(size_t)y*w*bpp,(uint8_t*)p+fp.Offset+y*fp.Footprint.RowPitch,(size_t)w*bpp);
        bb->Unmap(0,nullptr);bb->Release();
    }
};

// ---- GPU flow/cost upsampler (joint-bilateral, edge-aware) ----
// Upscales NVOFA grid-4 output to full res entirely on the GPU (no CPU
// round-trip) and keeps the flow discontinuity aligned with the luma edge.
struct FlowUpsampler {
    ID3D12PipelineState*  psoFlow = nullptr;
    ID3D12PipelineState*  psoCost = nullptr;
    ID3D12RootSignature*  root    = nullptr;
    ID3D12DescriptorHeap* heap    = nullptr;
    ID3D12Device*         dev     = nullptr;
    UINT                  inc     = 0;

    bool init(ID3D12Device* d, const wchar_t* path, std::string* err){
        dev = d;
        ID3DBlob *cs = nullptr, *eb = nullptr;
        auto compile = [&](const char* entry, ID3DBlob** out){
            if (FAILED(D3DCompileFromFile(path, nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                    entry, "cs_5_0", 0, 0, out, &eb))){
                if (eb) *err = std::string((const char*)eb->GetBufferPointer(), eb->GetBufferSize());
                else    *err = std::string("shader compile failed: ") + entry;
                return false;
            }
            return true;
        };
        ID3DBlob *csFlow = nullptr, *csCost = nullptr;
        if (!compile("cs_nvof_up", &csFlow)) { printf("  [compile flow] %s\n", err->c_str()); return false; }
        printf("  flow shader compiled\n");
        if (!compile("cs_cost_up", &csCost)) { printf("  [compile cost] %s\n", err->c_str()); return false; }
        printf("  cost shader compiled\n");

        // root signature: b0 (6 x 32-bit constants) + t0..t2 SRV table + u0 UAV table
        D3D12_DESCRIPTOR_RANGE sr; sr.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        sr.NumDescriptors = 3; sr.BaseShaderRegister = 0; sr.RegisterSpace = 0;
        sr.OffsetInDescriptorsFromTableStart = 0;
        D3D12_DESCRIPTOR_RANGE ur; ur.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ur.NumDescriptors = 1; ur.BaseShaderRegister = 0; ur.RegisterSpace = 0;
        ur.OffsetInDescriptorsFromTableStart = 0;
        D3D12_ROOT_PARAMETER rp[3] = {};
        rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        rp[0].Constants.Num32BitValues = 6; rp[0].Constants.ShaderRegister = 0; rp[0].Constants.RegisterSpace = 0;
        rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rp[1].DescriptorTable.NumDescriptorRanges = 1; rp[1].DescriptorTable.pDescriptorRanges = &sr;
        rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rp[2].DescriptorTable.NumDescriptorRanges = 1; rp[2].DescriptorTable.pDescriptorRanges = &ur;
        rp[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC rsd = {}; rsd.NumParameters = 3; rsd.pParameters = rp;
        rsd.NumStaticSamplers = 0; rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
        ID3DBlob* sig = nullptr;
        if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, nullptr)))
        { *err = "SerializeRootSignature failed"; return false; }
        dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&root));
        sig->Release();
        printf("  root sig ok\n");

        D3D12_COMPUTE_PIPELINE_STATE_DESC cd = {}; cd.pRootSignature = root;
        cd.CS.pShaderBytecode = csFlow->GetBufferPointer(); cd.CS.BytecodeLength = csFlow->GetBufferSize();
        if (FAILED(dev->CreateComputePipelineState(&cd, IID_PPV_ARGS(&psoFlow)))) { *err = "pso flow failed"; return false; }
        cd.CS.pShaderBytecode = csCost->GetBufferPointer(); cd.CS.BytecodeLength = csCost->GetBufferSize();
        if (FAILED(dev->CreateComputePipelineState(&cd, IID_PPV_ARGS(&psoCost)))) { *err = "pso cost failed"; return false; }
        csFlow->Release(); csCost->Release();

        D3D12_DESCRIPTOR_HEAP_DESC hd = {}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 12; hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap));
        inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        printf("  upsampler init ok\n");
        return true;
    }

    // Write descriptors for one (flowG,costG,luma)->outFlow pass into heap slots base..base+3.
    void bind(ID3D12Resource* flowG, ID3D12Resource* costG, ID3D12Resource* luma,
              ID3D12Resource* outFlow, UINT base)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE c = heap->GetCPUDescriptorHandleForHeapStart();
        D3D12_CPU_DESCRIPTOR_HANDLE h[4];
        for (int i = 0; i < 4; ++i) h[i] = { c.ptr + (SIZE_T)inc * (base + i) };

        D3D12_SHADER_RESOURCE_VIEW_DESC sv = {}; sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; sv.Texture2D.MipLevels = 1;
        sv.Format = DXGI_FORMAT_R16G16_SINT;  dev->CreateShaderResourceView(flowG, &sv, h[0]);
        sv.Format = DXGI_FORMAT_R8_UINT;      dev->CreateShaderResourceView(costG, &sv, h[1]);
        sv.Format = DXGI_FORMAT_R8_UNORM;     dev->CreateShaderResourceView(luma,  &sv, h[2]);

        D3D12_UNORDERED_ACCESS_VIEW_DESC uv = {}; uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uv.Format = DXGI_FORMAT_R32G32_FLOAT; dev->CreateUnorderedAccessView(outFlow, nullptr, &uv, h[3]);
    }

    // barrier helper: transition a list of resources
    static void bar(ID3D12GraphicsCommandList* cl, ID3D12Resource** rs, D3D12_RESOURCE_STATES* a,
                    D3D12_RESOURCE_STATES* b, int n)
    {
        for (int i = 0; i < n; ++i) {
            D3D12_RESOURCE_BARRIER br = {}; br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            br.Transition.pResource = rs[i]; br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            br.Transition.StateBefore = a[i]; br.Transition.StateAfter = b[i];
            cl->ResourceBarrier(1, &br);
        }
    }

    // Upsample flow (edge-aware) from grid -> full res.  All resources assumed COMMON on entry.
    void runFlow(ID3D12GraphicsCommandList* cl, ID3D12Resource* flowG, ID3D12Resource* costG,
                 ID3D12Resource* luma, ID3D12Resource* outFlow, uint32_t ow, uint32_t oh,
                 uint32_t W, uint32_t H, float sigmaLuma, float sigmaCost, UINT base)
    {
        bind(flowG, costG, luma, outFlow, base);
        cl->SetDescriptorHeaps(1, &heap);
        ID3D12Resource* rs[4] = { flowG, costG, luma, outFlow };
        D3D12_RESOURCE_STATES a[4] = { D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON,
                                       D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON };
        D3D12_RESOURCE_STATES b[4] = { D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS };
        bar(cl, rs, a, b, 4);

        uint32_t c[6] = { ow, oh, W, H };
        memcpy(&c[4], &sigmaLuma, 4); memcpy(&c[5], &sigmaCost, 4);
        D3D12_GPU_DESCRIPTOR_HANDLE g = heap->GetGPUDescriptorHandleForHeapStart();
        g.ptr += (SIZE_T)inc * base;
        D3D12_GPU_DESCRIPTOR_HANDLE guav; guav.ptr = g.ptr + (SIZE_T)inc * 3;
        cl->SetPipelineState(psoFlow);
        cl->SetComputeRootSignature(root);
        cl->SetComputeRoot32BitConstants(0, 6, c, 0);
        cl->SetComputeRootDescriptorTable(1, g);
        cl->SetComputeRootDescriptorTable(2, guav);
        cl->Dispatch((W + 7) / 8, (H + 7) / 8, 1);

        // return to COMMON (VFI expects COMMON override textures)
        bar(cl, rs, b, a, 4);
    }

    // Bilinear-upsample cost -> full res.
    void runCost(ID3D12GraphicsCommandList* cl, ID3D12Resource* costG, ID3D12Resource* outCost,
                 uint32_t ow, uint32_t oh, uint32_t W, uint32_t H, UINT base)
    {
        cl->SetDescriptorHeaps(1, &heap);
        D3D12_CPU_DESCRIPTOR_HANDLE c = heap->GetCPUDescriptorHandleForHeapStart();
        D3D12_SHADER_RESOURCE_VIEW_DESC sv = {}; sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; sv.Texture2D.MipLevels = 1;
        sv.Format = DXGI_FORMAT_R8_UINT;
        D3D12_CPU_DESCRIPTOR_HANDLE s0 = { c.ptr + (SIZE_T)inc * base };
        dev->CreateShaderResourceView(costG, &sv, s0);
        D3D12_UNORDERED_ACCESS_VIEW_DESC uv = {}; uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uv.Format = DXGI_FORMAT_R32_FLOAT;
        D3D12_CPU_DESCRIPTOR_HANDLE u3 = { c.ptr + (SIZE_T)inc * (base + 3) };
        dev->CreateUnorderedAccessView(outCost, nullptr, &uv, u3);

        ID3D12Resource* rs[2] = { costG, outCost };
        D3D12_RESOURCE_STATES a[2] = { D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON };
        D3D12_RESOURCE_STATES b[2] = { D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS };
        bar(cl, rs, a, b, 2);

        uint32_t cc[6] = { ow, oh, W, H, 0, 0 };
        D3D12_GPU_DESCRIPTOR_HANDLE g = heap->GetGPUDescriptorHandleForHeapStart();
        g.ptr += (SIZE_T)inc * base;
        cl->SetPipelineState(psoCost);
        cl->SetComputeRootSignature(root);
        cl->SetComputeRoot32BitConstants(0, 6, cc, 0);
        cl->SetComputeRootDescriptorTable(1, g);
        cl->SetComputeRootDescriptorTable(2, { g.ptr + (SIZE_T)inc * 3 });
        cl->Dispatch((W + 7) / 8, (H + 7) / 8, 1);

        bar(cl, rs, b, a, 2);
    }
};

// ---- the scene (matches selftest: translation + disc occlusion + bar) ----
static float gBgShift = 0.0f;
static uint32_t hsh(uint32_t x){x^=x>>16;x*=0x7feb352du;x^=x>>15;x*=0x846ca68bu;x^=x>>16;return x;}
static void renderScene(uint8_t* px, float t){
    const float D=W*0.04f; const float ang=0.60f*t; const float ca=cosf(ang), sa=sinf(ang);
    const float barCX=W*0.62f, barCY=H*0.40f, barL=W*0.19f*0.5f, barT=H*0.055f*0.5f;
    const float discCX=W*0.22f+W*0.24f*t, discCY=H*0.62f, discR=H*0.14f;
    gBgShift=D;
    const float blx[5]={0.10f,0.34f,0.53f,0.72f,0.90f};
    const float bly[5]={0.24f,0.76f,0.40f,0.86f,0.30f};
    const float blr[5]={0.085f,0.12f,0.07f,0.10f,0.08f};
    for(uint32_t y=0;y<H;++y)for(uint32_t x=0;x<W;++x){
        float u=(float)x-D*t, v=(float)y;
        float bg=0.30f+0.26f*(v/(float)H);
        for(int i=0;i<5;++i){float dx=(u/(float)W-blx[i])/blr[i],dy=(v/(float)H-bly[i])/(blr[i]*((float)W/(float)H));bg+=0.30f*expf(-(dx*dx+dy*dy));}
        int cu=(int)floorf(u/2.5f),cv=(int)floorf(v/2.5f);
        bg+=0.16f*((float)(hsh((uint32_t)cu*73856093u^(uint32_t)cv*19349663u)&0xffffu)/65535.0f-0.5f);
        float r=bg,g=bg,b=bg;
        float bx=(float)x-barCX,by=(float)y-barCY;
        float lx=ca*bx+sa*by, ly=-sa*bx+ca*by;
        if(fabsf(lx)<barL&&fabsf(ly)<barT){r=0.95f;g=0.34f;b=0.12f;}
        float ddx=(float)x-discCX,ddy=(float)y-discCY;
        if(ddx*ddx+ddy*ddy<discR*discR){r=0.15f;g=0.55f;b=0.95f;}
        size_t i=((size_t)y*W+x)*4;
        px[i]= (uint8_t)(std::min(1.0f,std::max(0.0f,r))*255+0.5f);
        px[i+1]=(uint8_t)(std::min(1.0f,std::max(0.0f,g))*255+0.5f);
        px[i+2]=(uint8_t)(std::min(1.0f,std::max(0.0f,b))*255+0.5f);
        px[i+3]=255;
    }
}
static std::vector<uint8_t> lumaOf(const uint8_t* rgba){
    std::vector<uint8_t> l((size_t)W*H);
    for(size_t i=0,j=0;i<(size_t)W*H*4;i+=4,++j)
        l[j]=(uint8_t)((rgba[i]*54+rgba[i+1]*183+rgba[i+2]*19)>>8);
    return l;
}
static double psnr(const uint8_t* a,const uint8_t* b){
    double se=0;size_t n=0;
    for(size_t i=0;i<(size_t)W*H*4;i+=4)for(int c=0;c<3;++c){double d=(double)a[i+c]-b[i+c];se+=d*d;++n;}
    double mse=se/n; return mse<=1e-12?99.0:10.0*log10(255.0*255.0/mse);
}

int main(){
    setvbuf(stdout,nullptr,_IONBF,0);
    Gpu g; if(!g.init()){printf("D3D12 init failed\n");return 1;}
    printf("d3d12 ok\n");

    // render
    std::vector<uint8_t> A((size_t)W*H*4), B((size_t)W*H*4), M((size_t)W*H*4);
    renderScene(A.data(),0.0f); renderScene(M.data(),0.5f); renderScene(B.data(),1.0f);
    auto lA=lumaOf(A.data()), lB=lumaOf(B.data());

    // ---- NVOFA: hardware flow A->B and B->A (BOTH) + cost ----
    HMODULE h=LoadLibraryA("nvofapi64.dll");
    auto pInst=(decltype(NvOFAPICreateInstanceD3D12)*)GetProcAddress(h,"NvOFAPICreateInstanceD3D12");
    NV_OF_D3D12_API_FUNCTION_LIST of={}; uint32_t ver=0;
    ((decltype(NvOFGetMaxSupportedApiVersion)*)GetProcAddress(h,"NvOFGetMaxSupportedApiVersion"))(&ver);
    pInst(ver,&of);
    NvOFHandle hOf=nullptr; of.nvCreateOpticalFlowD3D12(g.dev,&hOf);
    NV_OF_INIT_PARAMS ip={}; ip.width=W; ip.height=H;
    ip.outGridSize=NV_OF_OUTPUT_VECTOR_GRID_SIZE_4; ip.hintGridSize=NV_OF_HINT_VECTOR_GRID_SIZE_4;
    ip.mode=NV_OF_MODE_OPTICALFLOW; ip.perfLevel=NV_OF_PERF_LEVEL_FAST;
    ip.inputBufferFormat=NV_OF_BUFFER_FORMAT_GRAYSCALE8;
    ip.predDirection=NV_OF_PRED_DIRECTION_FORWARD;
    ip.enableOutputCost=(NV_OF_BOOL)1;
    of.nvOFInit(hOf,&ip);

    ID3D12Resource* tA=g.tex(DXGI_FORMAT_R8_UNORM,W,H,D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource* tB=g.tex(DXGI_FORMAT_R8_UNORM,W,H,D3D12_RESOURCE_STATE_COPY_DEST);
    g.upload(tA,lA.data(),W,H,1); g.upload(tB,lB.data(),W,H,1);
    g.transition(tA,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);
    g.transition(tB,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);
    uint32_t ow=W/GRID, oh=H/GRID;
    ID3D12Resource* tF=g.tex(DXGI_FORMAT_R16G16_SINT,ow,oh,D3D12_RESOURCE_STATE_COMMON);
    ID3D12Resource* tBwd=g.tex(DXGI_FORMAT_R16G16_SINT,ow,oh,D3D12_RESOURCE_STATE_COMMON);
    ID3D12Resource* tC=g.tex(DXGI_FORMAT_R8_UINT,ow,oh,D3D12_RESOURCE_STATE_COMMON);
    ID3D12Resource* tC2=g.tex(DXGI_FORMAT_R8_UINT,ow,oh,D3D12_RESOURCE_STATE_COMMON);
    auto reg=[&](ID3D12Resource* t,NvOFGPUBufferHandle* hh){
        NV_OF_REGISTER_RESOURCE_PARAMS_D3D12 p={}; p.resource=t; p.hOFGpuBuffer=hh;
        return of.nvOFRegisterResourceD3D12(hOf,&p);
    };
    NvOFGPUBufferHandle hA=nullptr,hB=nullptr,hF=nullptr,hBwd=nullptr,hC=nullptr,hC2=nullptr;
    reg(tA,&hA);reg(tB,&hB);reg(tF,&hF);reg(tBwd,&hBwd);reg(tC,&hC);reg(tC2,&hC2);

    ID3D12Fence* nof=nullptr; g.dev->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&nof));
    HANDLE fe=CreateEvent(nullptr,FALSE,FALSE,nullptr);
    // pass 1: A->B  -> forward flow (fab) + cost
    {
        NV_OF_FENCE_POINT inFp={nof,0}, outFp={nof,1};
        NV_OF_EXECUTE_INPUT_PARAMS_D3D12 in={}; in.inputFrame=hA; in.referenceFrame=hB;
        in.disableTemporalHints=(NV_OF_BOOL)1; in.numFencePoints=1; in.fencePoint=&inFp;
        NV_OF_EXECUTE_OUTPUT_PARAMS_D3D12 out={}; out.outputBuffer=hF; out.outputCostBuffer=hC; out.fencePoint=&outFp;
        int es=of.nvOFExecuteD3D12(hOf,&in,&out);
        printf("NVOFA Execute(A->B)=%d\n", es);
        while(nof->GetCompletedValue()<1) Sleep(0);
    }
    // pass 2: B->A  -> TRUE backward flow (fba)
    {
        NV_OF_FENCE_POINT inFp={nof,1}, outFp={nof,2};
        NV_OF_EXECUTE_INPUT_PARAMS_D3D12 in={}; in.inputFrame=hB; in.referenceFrame=hA;
        in.disableTemporalHints=(NV_OF_BOOL)1; in.numFencePoints=1; in.fencePoint=&inFp;
        NV_OF_EXECUTE_OUTPUT_PARAMS_D3D12 out={}; out.outputBuffer=hBwd; out.outputCostBuffer=hC2; out.fencePoint=&outFp;
        int es=of.nvOFExecuteD3D12(hOf,&in,&out);
        printf("NVOFA Execute(B->A)=%d\n", es);
        while(nof->GetCompletedValue()<2) Sleep(0);
    }
    printf("fence completed=%llu\n", (unsigned long long)nof->GetCompletedValue());

    // ---- GPU upsample grid flow -> full res (edge-aware, no CPU round-trip) ----
    printf("GPU upsample flow...\n");
    FlowUpsampler up; std::string uerr;
    if(!up.init(g.dev, L"src/shaders/nvof_up.hlsl", &uerr)){ printf("upsampler init failed: %s\n", uerr.c_str()); return 1; }
    ID3D12Resource* tfab =g.tex(DXGI_FORMAT_R32G32_FLOAT,W,H,D3D12_RESOURCE_STATE_COMMON);
    ID3D12Resource* tfba =g.tex(DXGI_FORMAT_R32G32_FLOAT,W,H,D3D12_RESOURCE_STATE_COMMON);
    ID3D12Resource* tcost=g.tex(DXGI_FORMAT_R32_FLOAT,W,H,D3D12_RESOURCE_STATE_COMMON);

    g.alloc->Reset(); g.list->Reset(g.alloc,nullptr);
    up.runFlow(g.list, tF,   tC,  tA, tfab,  ow, oh, W, H, 0.15f, 0.30f, 0);  // A->B, guide = luma A
    up.runFlow(g.list, tBwd, tC2, tB, tfba,  ow, oh, W, H, 0.15f, 0.30f, 4);  // B->A, guide = luma B
    up.runCost(g.list, tC, tcost, ow, oh, W, H, 8);
    g.list->Close();
    printf("  list closed, flushing...\n");
    g.q->Wait(nof, 2);   // cross-queue sync: NVOFA wrote tF/tBwd/tC on its own queue
    g.flush();
    HRESULT rr = g.dev->GetDeviceRemovedReason();
    if (rr != S_OK) printf("  [DEVICE REMOVED: 0x%08x]\n", (unsigned)rr);
    printf("GPU upsample ok\n");

    // ---- VFI engine with the hardware flow as the override ----
    ID3D12Resource* texA=g.tex(DXGI_FORMAT_R8G8B8A8_UNORM,W,H,D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource* texB=g.tex(DXGI_FORMAT_R8G8B8A8_UNORM,W,H,D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource* texOut=g.tex(DXGI_FORMAT_R8G8B8A8_UNORM,W,H,D3D12_RESOURCE_STATE_COMMON);
    g.upload(texA,A.data(),W,H,4); g.upload(texB,B.data(),W,H,4);
    g.transition(texA,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);
    g.transition(texB,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);

    VfiEngine eng; std::string err;
    VfiOptions opt; opt.temporal_aa=false; opt.luma_fp16=true;
    wchar_t wpath[MAX_PATH*2]; MultiByteToWideChar(CP_UTF8,0,"src/shaders/vfi.hlsl",-1,wpath,MAX_PATH*2);
    printf("VFI init...\n");
    if(!eng.init(g.dev,DXGI_FORMAT_R8G8B8A8_UNORM,W,H,opt,wpath,&err)){printf("VFI init failed: %s\n",err.c_str());return 1;}
    printf("VFI init ok\n");
    printf("feeding override flow...\n");
    eng.set_flow_override(tfab,tfba,tcost);

    g.alloc->Reset(); g.list->Reset(g.alloc,nullptr);
    eng.record(g.list,0,texA,D3D12_RESOURCE_STATE_COMMON,texB,D3D12_RESOURCE_STATE_COMMON,
               texOut,D3D12_RESOURCE_STATE_COMMON,0.5f,false);
    g.list->Close(); g.flush();

    std::vector<uint8_t> gen((size_t)W*H*4);
    g.readback(texOut,gen.data(),W,H,4);
    printf("\nPSNR(50/50 avg vs truth)  : %.2f dB   <- floor\n", 0.0); // placeholder, print below
    printf("PSNR(hw-flow + blend vs truth): %.2f dB\n", psnr(gen.data(),M.data()));
    printf("dispatches (blend-only): %u\n", eng.dispatch_count());

    of.nvOFDestroy(hOf);
    return 0;
}
