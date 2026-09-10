// End-to-end hardware optical flow on the RTX 3080, using the REAL SDK 5.0.7
// headers.  Frame B = frame A shifted right by SHIFT px.  Correct result:
// dx ~= +SHIFT, dy ~= 0, in S10.5 fixed point (/32.0 = pixels).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include <algorithm>
#include "nvOpticalFlowD3D12.h"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

static const uint32_t W = 1920, H = 1080, GRID = 4, SHIFT = 8;

struct Gpu {
    ID3D12Device* dev = nullptr;  ID3D12CommandQueue* q = nullptr;
    ID3D12CommandAllocator* alloc = nullptr;  ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr; UINT64 fv = 0; HANDLE ev = nullptr;
    bool init() {
        if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev)))) return false;
        D3D12_COMMAND_QUEUE_DESC qd = {}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q)))) return false;
        if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)))) return false;
        if (FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&list)))) return false;
        list->Close();
        if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return false;
        ev = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        return true;
    }
    void submit() { q->ExecuteCommandLists(1, (ID3D12CommandList**)&list); ++fv; q->Signal(fence, fv); }
    void flush() { submit(); fence->SetEventOnCompletion(fv, ev); WaitForSingleObject(ev, INFINITE); }
    void transition(ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
        alloc->Reset(); list->Reset(alloc, nullptr);
        D3D12_RESOURCE_BARRIER br = {}; br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        br.Transition.pResource = r; br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        br.Transition.StateBefore = a; br.Transition.StateAfter = b;
        list->ResourceBarrier(1, &br);
        list->Close(); flush();
    }
    ID3D12Resource* tex(DXGI_FORMAT fmt, uint32_t w, uint32_t h, D3D12_RESOURCE_STATES st) {
        D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd = {}; rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = w; rd.Height = h; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.Format = fmt; rd.SampleDesc.Count = 1;
        ID3D12Resource* r = nullptr;
        dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, st, nullptr, IID_PPV_ARGS(&r));
        return r;
    }
    void upload(ID3D12Resource* t, const void* data, uint32_t w, uint32_t h, uint32_t bpp) {
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {}; UINT rows = 0; UINT64 rs = 0, tot = 0;
        dev->GetCopyableFootprints(&t->GetDesc(), 0, 1, 0, &fp, &rows, &rs, &tot);
        D3D12_HEAP_PROPERTIES up = {}; up.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bd = {}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = tot; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ID3D12Resource* ub = nullptr;
        dev->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&ub));
        void* p = nullptr; ub->Map(0, nullptr, &p);
        for (UINT y = 0; y < rows; ++y)
            memcpy((uint8_t*)p + fp.Offset + y * fp.Footprint.RowPitch, (const uint8_t*)data + (size_t)y * w * bpp, (size_t)w * bpp);
        ub->Unmap(0, nullptr);
        alloc->Reset(); list->Reset(alloc, nullptr);
        D3D12_TEXTURE_COPY_LOCATION dst = {}; dst.pResource = t; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION src = {}; src.pResource = ub; src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint = fp;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        list->Close(); flush(); ub->Release();
    }
    void readback(ID3D12Resource* t, void* out, uint32_t w, uint32_t h, uint32_t bpp) {
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {}; UINT rows = 0; UINT64 rs = 0, tot = 0;
        dev->GetCopyableFootprints(&t->GetDesc(), 0, 1, 0, &fp, &rows, &rs, &tot);
        D3D12_HEAP_PROPERTIES rb = {}; rb.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC bd = {}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = tot; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ID3D12Resource* bb = nullptr;
        dev->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&bb));
        transition(t, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
        alloc->Reset(); list->Reset(alloc, nullptr);
        D3D12_TEXTURE_COPY_LOCATION dst = {}; dst.pResource = bb; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = fp;
        D3D12_TEXTURE_COPY_LOCATION src = {}; src.pResource = t; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        list->Close(); flush();
        void* p = nullptr; bb->Map(0, nullptr, &p);
        for (UINT y = 0; y < h; ++y)
            memcpy((uint8_t*)out + (size_t)y * w * bpp, (uint8_t*)p + fp.Offset + y * fp.Footprint.RowPitch, (size_t)w * bpp);
        bb->Unmap(0, nullptr); bb->Release();
    }
};

static uint32_t hsh(uint32_t x){ x^=x>>16; x*=0x7feb352du; x^=x>>15; x*=0x846ca68bu; x^=x>>16; return x; }
static std::vector<uint8_t> frame(int shift) {
    std::vector<uint8_t> f((size_t)W*H);
    for (uint32_t y=0;y<H;++y) for (uint32_t x=0;x<W;++x) {
        int sx=(int)x-shift;
        f[(size_t)y*W+x] = (sx<0||sx>=(int)W) ? 0 : (uint8_t)(hsh((uint32_t)sx*73856093u ^ y*19349663u)&0xff);
    }
    return f;
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    Gpu g; if(!g.init()){printf("D3D12 init failed\n");return 1;}
    HMODULE h = LoadLibraryA("nvofapi64.dll");
    auto pInst = (decltype(NvOFAPICreateInstanceD3D12)*)GetProcAddress(h,"NvOFAPICreateInstanceD3D12");
    NV_OF_D3D12_API_FUNCTION_LIST of={}; uint32_t ver=0;
    ((decltype(NvOFGetMaxSupportedApiVersion)*)GetProcAddress(h,"NvOFGetMaxSupportedApiVersion"))(&ver);
    printf("api %#x  CreateInstance=%d\n", ver, pInst(ver,&of));

    NvOFHandle hOf=nullptr;
    printf("Create=%d\n", of.nvCreateOpticalFlowD3D12(g.dev,&hOf));

    NV_OF_INIT_PARAMS ip={}; ip.width=W; ip.height=H;
    ip.outGridSize=NV_OF_OUTPUT_VECTOR_GRID_SIZE_4; ip.hintGridSize=NV_OF_HINT_VECTOR_GRID_SIZE_4;
    ip.mode=NV_OF_MODE_OPTICALFLOW; ip.perfLevel=NV_OF_PERF_LEVEL_FAST;
    ip.inputBufferFormat=NV_OF_BUFFER_FORMAT_GRAYSCALE8; ip.predDirection=NV_OF_PRED_DIRECTION_FORWARD;
    printf("Init=%d\n", of.nvOFInit(hOf,&ip));

    auto fA=frame(0), fB=frame(SHIFT);
    ID3D12Resource* tA=g.tex(DXGI_FORMAT_R8_UNORM,W,H,D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource* tB=g.tex(DXGI_FORMAT_R8_UNORM,W,H,D3D12_RESOURCE_STATE_COPY_DEST);
    g.upload(tA,fA.data(),W,H,1); g.upload(tB,fB.data(),W,H,1);
    g.transition(tA,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);
    g.transition(tB,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);
    uint32_t ow=(W+GRID-1)/GRID, oh=(H+GRID-1)/GRID;
    ID3D12Resource* tF=g.tex(DXGI_FORMAT_R16G16_SINT,ow,oh,D3D12_RESOURCE_STATE_COMMON);

    auto reg=[&](ID3D12Resource* t,NvOFGPUBufferHandle* hh){
        NV_OF_REGISTER_RESOURCE_PARAMS_D3D12 p={}; p.resource=t; p.hOFGpuBuffer=hh;
        return of.nvOFRegisterResourceD3D12(hOf,&p);
    };
    NvOFGPUBufferHandle hA=nullptr,hB=nullptr,hF=nullptr;
    printf("register A=%d B=%d F=%d\n", reg(tA,&hA), reg(tB,&hB), reg(tF,&hF));

    // NVOFA requires non-zero fence points (the earlier INVALID_PARAM)
    ID3D12Fence* nof=nullptr; g.dev->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&nof));
    HANDLE fe=CreateEvent(nullptr,FALSE,FALSE,nullptr);
    NV_OF_FENCE_POINT inFp={nof,0}, outFp={nof,1};

    NV_OF_EXECUTE_INPUT_PARAMS_D3D12 in={}; in.inputFrame=hA; in.referenceFrame=hB;
    in.disableTemporalHints=(NV_OF_BOOL)1; in.numFencePoints=1; in.fencePoint=&inFp;
    NV_OF_EXECUTE_OUTPUT_PARAMS_D3D12 out={}; out.outputBuffer=hF; out.fencePoint=&outFp;
    printf("Execute=%d\n", of.nvOFExecuteD3D12(hOf,&in,&out));
    nof->SetEventOnCompletion(1,fe); WaitForSingleObject(fe,INFINITE);

    std::vector<int16_t> flow((size_t)ow*oh*2);
    g.readback(tF, flow.data(), ow, oh, 4);
    std::vector<float> dx,dy;
    for(uint32_t y=oh/4;y<oh*3/4;++y) for(uint32_t x=ow/4;x<ow*3/4;++x){
        dx.push_back(flow[(size_t)y*ow*2+x*2]/32.0f);
        dy.push_back(flow[(size_t)y*ow*2+x*2+1]/32.0f);
    }
    std::sort(dx.begin(),dx.end()); std::sort(dy.begin(),dy.end());
    printf("\nflow: median dx=%+.2f px (expect +%d), dy=%+.2f px (expect 0)\n",
           dx[dx.size()/2], SHIFT, dy[dy.size()/2]);

    LARGE_INTEGER fr,t0,t1; QueryPerformanceFrequency(&fr); QueryPerformanceCounter(&t0);
    UINT64 nv=1;
    for(int i=0;i<60;++i){
        nv+=1; inFp.value=nv-1; outFp.value=nv;
        of.nvOFExecuteD3D12(hOf,&in,&out);
        nof->SetEventOnCompletion(nv,fe); WaitForSingleObject(fe,INFINITE);
    }
    QueryPerformanceCounter(&t1);
    printf("Execute+wait: %.3f ms/frame @ %ux%u grid %u\n",
           1000.0*(double)(t1.QuadPart-t0.QuadPart)/fr.QuadPart/60.0, W,H,GRID);
    of.nvOFDestroy(hOf);
    return 0;
}
