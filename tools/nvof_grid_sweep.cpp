// NVOFA grid sweep: measure flow correctness + cost + timing at grid 4/2/1.
// Verifies the convention (inputFrame->referenceFrame = +SHIFT) at every density
// and shows what a denser field costs, which decides the VFI integration shape.
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

static const uint32_t W = 1920, H = 1080, SHIFT = 8;

struct Gpu {
    ID3D12Device* dev=nullptr; ID3D12CommandQueue* q=nullptr;
    ID3D12CommandAllocator* alloc=nullptr; ID3D12GraphicsCommandList* list=nullptr;
    ID3D12Fence* fence=nullptr; UINT64 fv=0; HANDLE ev=nullptr;
    bool init(){
        if(FAILED(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&dev))))return false;
        D3D12_COMMAND_QUEUE_DESC qd={}; qd.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
        if(FAILED(dev->CreateCommandQueue(&qd,IID_PPV_ARGS(&q))))return false;
        if(FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&alloc))))return false;
        if(FAILED(dev->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,alloc,nullptr,IID_PPV_ARGS(&list))))return false;
        list->Close();
        if(FAILED(dev->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence))))return false;
        ev=CreateEvent(nullptr,FALSE,FALSE,nullptr); return true;
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

static uint32_t hsh(uint32_t x){x^=x>>16;x*=0x7feb352du;x^=x>>15;x*=0x846ca68bu;x^=x>>16;return x;}
// A structured scene (NOT white noise): smooth gradients give every pixel a local
// gradient (needed for dense flow), blobs give larger structure, 4-px hashed
// detail gives medium-frequency texture.  frame(shift) shifts the whole thing.
static std::vector<uint8_t> frame(int shift){
    std::vector<uint8_t> f((size_t)W*H);
    const float bx[5]={0.15f,0.38f,0.55f,0.72f,0.88f};
    const float by[5]={0.30f,0.70f,0.42f,0.80f,0.55f};
    const float br[5]={0.10f,0.14f,0.08f,0.12f,0.09f};
    for(uint32_t y=0;y<H;++y)for(uint32_t x=0;x<W;++x){
        int sx=(int)x-shift;
        float u=(float)sx, v=(float)y;
        float val=0.35f + 0.18f*sinf(u*0.018f) + 0.18f*cosf(v*0.024f); // gradients
        for(int i=0;i<5;++i){
            float dx=(u/(float)W-bx[i])/br[i], dy=(v/(float)H-by[i])/(br[i]*((float)W/(float)H));
            val += 0.30f*expf(-(dx*dx+dy*dy));
        }
        int cu=sx/4, cv=y/4;   // 4-px detail (survives grid 2, partial at grid 1)
        val += 0.08f*((float)(hsh((uint32_t)cu*73856093u^(uint32_t)cv*19349663u)&0xffffu)/65535.0f - 0.5f);
        float c = val<0?0:(val>1?1:val);
        f[(size_t)y*W+x]=(uint8_t)(c*255.0f+0.5f);
    }
    return f;
}

int main(){
    setvbuf(stdout,nullptr,_IONBF,0);
    Gpu g; if(!g.init()){printf("D3D12 init failed\n");return 1;}
    HMODULE h=LoadLibraryA("nvofapi64.dll");
    auto pInst=(decltype(NvOFAPICreateInstanceD3D12)*)GetProcAddress(h,"NvOFAPICreateInstanceD3D12");
    NV_OF_D3D12_API_FUNCTION_LIST of={}; uint32_t ver=0;
    ((decltype(NvOFGetMaxSupportedApiVersion)*)GetProcAddress(h,"NvOFGetMaxSupportedApiVersion"))(&ver);
    printf("api %#x\n",ver); pInst(ver,&of);

    auto fA=frame(0), fB=frame(SHIFT);
    ID3D12Resource* tA=g.tex(DXGI_FORMAT_R8_UNORM,W,H,D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource* tB=g.tex(DXGI_FORMAT_R8_UNORM,W,H,D3D12_RESOURCE_STATE_COPY_DEST);
    g.upload(tA,fA.data(),W,H,1); g.upload(tB,fB.data(),W,H,1);
    g.transition(tA,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);
    g.transition(tB,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);

    ID3D12Fence* nof=nullptr; g.dev->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&nof));
    HANDLE fe=CreateEvent(nullptr,FALSE,FALSE,nullptr);

    printf("\n%-6s %-14s %-14s %-14s %-12s %-12s\n","grid","median dx","median dy","cost p50","ms/frame","est GPU-only");
    for(uint32_t GRID : {4u,2u,1u}){
        NvOFHandle hOf=nullptr;
        of.nvCreateOpticalFlowD3D12(g.dev,&hOf);
        NV_OF_INIT_PARAMS ip={}; ip.width=W; ip.height=H;
        ip.outGridSize=(NV_OF_OUTPUT_VECTOR_GRID_SIZE)GRID;
        ip.hintGridSize=(NV_OF_HINT_VECTOR_GRID_SIZE)GRID;
        ip.mode=NV_OF_MODE_OPTICALFLOW; ip.perfLevel=NV_OF_PERF_LEVEL_FAST;
        ip.inputBufferFormat=NV_OF_BUFFER_FORMAT_GRAYSCALE8;
        ip.predDirection=NV_OF_PRED_DIRECTION_FORWARD;
        ip.enableOutputCost=(NV_OF_BOOL)1;          // also get a confidence/cost buffer
        int inits = of.nvOFInit(hOf,&ip);

        uint32_t ow=(W+GRID-1)/GRID, oh=(H+GRID-1)/GRID;
        ID3D12Resource* tF=g.tex(DXGI_FORMAT_R16G16_SINT,ow,oh,D3D12_RESOURCE_STATE_COMMON);
        ID3D12Resource* tC=g.tex(DXGI_FORMAT_R8_UINT,ow,oh,D3D12_RESOURCE_STATE_COMMON);

        auto reg=[&](ID3D12Resource* t,NvOFGPUBufferHandle* hh){
            NV_OF_REGISTER_RESOURCE_PARAMS_D3D12 p={}; p.resource=t; p.hOFGpuBuffer=hh;
            return of.nvOFRegisterResourceD3D12(hOf,&p);
        };
        NvOFGPUBufferHandle hA=nullptr,hB=nullptr,hF=nullptr,hC=nullptr;
        reg(tA,&hA); reg(tB,&hB); reg(tF,&hF); reg(tC,&hC);

        NV_OF_FENCE_POINT inFp={nof,0}, outFp={nof,1};
        NV_OF_EXECUTE_INPUT_PARAMS_D3D12 in={}; in.inputFrame=hA; in.referenceFrame=hB;
        in.disableTemporalHints=(NV_OF_BOOL)1; in.numFencePoints=1; in.fencePoint=&inFp;
        NV_OF_EXECUTE_OUTPUT_PARAMS_D3D12 out={}; out.outputBuffer=hF; out.outputCostBuffer=hC; out.fencePoint=&outFp;
        int es=of.nvOFExecuteD3D12(hOf,&in,&out);
        nof->SetEventOnCompletion(1,fe); WaitForSingleObject(fe,INFINITE);

        std::vector<int16_t> flow((size_t)ow*oh*2);
        std::vector<uint8_t> cost((size_t)ow*oh);
        g.readback(tF,flow.data(),ow,oh,4);
        g.readback(tC,cost.data(),ow,oh,1);
        std::vector<float> dx,dy,c;
        for(uint32_t y=oh/4;y<oh*3/4;++y)for(uint32_t x=ow/4;x<ow*3/4;++x){
            dx.push_back(flow[(size_t)y*ow*2+x*2]/32.0f);
            dy.push_back(flow[(size_t)y*ow*2+x*2+1]/32.0f);
            c.push_back((float)cost[(size_t)y*ow+x]);
        }
        std::sort(dx.begin(),dx.end()); std::sort(dy.begin(),dy.end()); std::sort(c.begin(),c.end());

        // timing
        LARGE_INTEGER fr,t0,t1; QueryPerformanceFrequency(&fr); QueryPerformanceCounter(&t0);
        UINT64 nv=1;
        for(int i=0;i<40;++i){ nv+=1; inFp.value=nv-1; outFp.value=nv;
            of.nvOFExecuteD3D12(hOf,&in,&out); nof->SetEventOnCompletion(nv,fe); WaitForSingleObject(fe,INFINITE); }
        QueryPerformanceCounter(&t1);
        double ms=1000.0*(double)(t1.QuadPart-t0.QuadPart)/fr.QuadPart/40.0;

        printf("%-6u %-14.2f %-14.2f %-14.1f %-12.3f %-12s (Init=%d)\n",
               GRID, dx[dx.size()/2], dy[dy.size()/2], c[c.size()/2], ms, "-", inits);
        of.nvOFDestroy(hOf);
    }
    printf("\n(dx expect +%d, dy expect 0; cost=0 means perfect confidence)\n", SHIFT);
    return 0;
}
