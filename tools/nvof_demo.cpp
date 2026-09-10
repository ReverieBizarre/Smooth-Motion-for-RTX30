// ============================================================================
//  nvof_demo - a visual demo of the full hardware frame-interpolation pipeline.
//
//  Renders a synthetic scene at t=0, 0.5, 1, runs NVIDIA's hardware optical
//  flow (NVOFA) on the two frames, feeds the flow into the warp/blend stage,
//  and writes the actual frames out as PNGs so the result can be *seen*:
//      A.png, B.png, truth.png, gen.png, mix5050.png, diff.png
//  plus two extra interpolated times (gen_025.png, gen_075.png) to show real
//  interpolation between frames.
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
#pragma comment(lib,"d3d12.lib")
#pragma comment(lib,"dxgi.lib")
#pragma comment(lib,"d3dcompiler.lib")

using namespace sm86;
static const uint32_t W=1920, H=1080, GRID=4;

struct Gpu {
    ID3D12Device* dev=nullptr; ID3D12CommandQueue* q=nullptr;
    ID3D12CommandAllocator* alloc=nullptr; ID3D12GraphicsCommandList* list=nullptr;
    ID3D12Fence* fence=nullptr; UINT64 fv=0; HANDLE ev=nullptr;
    bool init(){if(FAILED(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&dev))))return false;
        D3D12_COMMAND_QUEUE_DESC qd={};qd.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
        if(FAILED(dev->CreateCommandQueue(&qd,IID_PPV_ARGS(&q))))return false;
        if(FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&alloc))))return false;
        if(FAILED(dev->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,alloc,nullptr,IID_PPV_ARGS(&list))))return false;
        list->Close(); if(FAILED(dev->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence))))return false;
        ev=CreateEvent(nullptr,FALSE,FALSE,nullptr);return true;}
    void submit(){q->ExecuteCommandLists(1,(ID3D12CommandList**)&list);++fv;q->Signal(fence,fv);}
    void flush(){submit();fence->SetEventOnCompletion(fv,ev);WaitForSingleObject(ev,INFINITE);}
    void transition(ID3D12Resource* r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b){
        alloc->Reset();list->Reset(alloc,nullptr);
        D3D12_RESOURCE_BARRIER br={};br.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        br.Transition.pResource=r;br.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        br.Transition.StateBefore=a;br.Transition.StateAfter=b;list->ResourceBarrier(1,&br);list->Close();flush();}
    ID3D12Resource* tex(DXGI_FORMAT fmt,uint32_t w,uint32_t h,D3D12_RESOURCE_STATES st){
        D3D12_HEAP_PROPERTIES hp={};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd={};rd.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width=w;rd.Height=h;rd.DepthOrArraySize=1;rd.MipLevels=1;
        rd.Format=fmt;rd.SampleDesc.Count=1;rd.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ID3D12Resource* r=nullptr; dev->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&rd,st,nullptr,IID_PPV_ARGS(&r));return r;}
    void upload(ID3D12Resource* t,const void* data,uint32_t w,uint32_t h,uint32_t bpp){
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp={};UINT rows=0;UINT64 rs=0,tot=0;
        dev->GetCopyableFootprints(&t->GetDesc(),0,1,0,&fp,&rows,&rs,&tot);
        D3D12_HEAP_PROPERTIES up={};up.Type=D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bd={};bd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width=tot;bd.Height=1;bd.DepthOrArraySize=1;bd.MipLevels=1;
        bd.Format=DXGI_FORMAT_UNKNOWN;bd.SampleDesc.Count=1;bd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ID3D12Resource* ub=nullptr; dev->CreateCommittedResource(&up,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&ub));
        void* p=nullptr;ub->Map(0,nullptr,&p);
        for(UINT y=0;y<rows;++y)memcpy((uint8_t*)p+fp.Offset+y*fp.Footprint.RowPitch,(const uint8_t*)data+(size_t)y*w*bpp,(size_t)w*bpp);
        ub->Unmap(0,nullptr);
        alloc->Reset();list->Reset(alloc,nullptr);
        D3D12_TEXTURE_COPY_LOCATION dst={};dst.pResource=t;dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION src={};src.pResource=ub;src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint=fp;
        list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);list->Close();flush();ub->Release();}
    void readback(ID3D12Resource* t,void* out,uint32_t w,uint32_t h,uint32_t bpp){
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp={};UINT rows=0;UINT64 rs=0,tot=0;
        dev->GetCopyableFootprints(&t->GetDesc(),0,1,0,&fp,&rows,&rs,&tot);
        D3D12_HEAP_PROPERTIES rb={};rb.Type=D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC bd={};bd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width=tot;bd.Height=1;bd.DepthOrArraySize=1;bd.MipLevels=1;
        bd.Format=DXGI_FORMAT_UNKNOWN;bd.SampleDesc.Count=1;bd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ID3D12Resource* bb=nullptr; dev->CreateCommittedResource(&rb,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&bb));
        transition(t,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);
        alloc->Reset();list->Reset(alloc,nullptr);
        D3D12_TEXTURE_COPY_LOCATION dst={};dst.pResource=bb;dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=fp;
        D3D12_TEXTURE_COPY_LOCATION src={};src.pResource=t;src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);list->Close();flush();
        void* p=nullptr;bb->Map(0,nullptr,&p);
        for(UINT y=0;y<h;++y)memcpy((uint8_t*)out+(size_t)y*w*bpp,(uint8_t*)p+fp.Offset+y*fp.Footprint.RowPitch,(size_t)w*bpp);
        bb->Unmap(0,nullptr);bb->Release();}
};

static uint32_t hsh(uint32_t x){x^=x>>16;x*=0x7feb352du;x^=x>>15;x*=0x846ca68bu;x^=x>>16;return x;}
static void renderScene(uint8_t* px,float t){
    const float D=W*0.04f,ang=0.60f*t,ca=cosf(ang),sa=sinf(ang);
    const float bcx=W*0.62f,bcy=H*0.40f,bhx=W*0.19f*0.5f,bhy=H*0.055f*0.5f;
    const float dcx=W*0.22f+W*0.24f*t,dcy=H*0.62f,dr=H*0.14f;
    const float bx[5]={0.10f,0.34f,0.53f,0.72f,0.90f},by[5]={0.24f,0.76f,0.40f,0.86f,0.30f},br[5]={0.085f,0.12f,0.07f,0.10f,0.08f};
    for(uint32_t y=0;y<H;++y)for(uint32_t x=0;x<W;++x){
        float u=(float)x-D*t,v=(float)y,bg=0.30f+0.26f*(v/H);
        for(int i=0;i<5;++i){float dx=(u/W-bx[i])/br[i],dy=(v/H-by[i])/(br[i]*((float)W/H));bg+=0.30f*expf(-(dx*dx+dy*dy));}
        int cu=(int)floorf(u/2.5f),cv=(int)floorf(v/2.5f);bg+=0.16f*((float)(hsh((uint32_t)cu*73856093u^(uint32_t)cv*19349663u)&0xffffu)/65535.0f-0.5f);
        float r=bg,g=bg,b=bg;
        float rx=(float)x-bcx,ry=(float)y-bcy,lx=ca*rx+sa*ry,ly=-sa*rx+ca*ry;
        if(fabsf(lx)<bhx&&fabsf(ly)<bhy){r=0.95f;g=0.34f;b=0.12f;}
        float dx=(float)x-dcx,dy=(float)y-dcy; if(dx*dx+dy*dy<dr*dr){r=0.15f;g=0.55f;b=0.95f;}
        size_t i=((size_t)y*W+x)*4;
        px[i]=(uint8_t)(std::min(1.0f,std::max(0.0f,r))*255+0.5f);
        px[i+1]=(uint8_t)(std::min(1.0f,std::max(0.0f,g))*255+0.5f);
        px[i+2]=(uint8_t)(std::min(1.0f,std::max(0.0f,b))*255+0.5f);
        px[i+3]=255;}
}
static std::vector<uint8_t> lumaOf(const uint8_t* rgba){std::vector<uint8_t> l((size_t)W*H);
    for(size_t i=0,j=0;i<(size_t)W*H*4;i+=4,++j)l[j]=(uint8_t)((rgba[i]*54+rgba[i+1]*183+rgba[i+2]*19)>>8);return l;}
static double psnr(const uint8_t* a,const uint8_t* b){double se=0;size_t n=0;
    for(size_t i=0;i<(size_t)W*H*4;i+=4)for(int c=0;c<3;++c){double d=(double)a[i+c]-b[i+c];se+=d*d;++n;}
    double mse=se/n;return mse<=1e-12?99.0:10.0*log10(255.0*255.0/mse);}
static void mix5050(const uint8_t* a,const uint8_t* b,uint8_t* o){
    for(size_t i=0;i<(size_t)W*H*4;i+=4){for(int c=0;c<3;++c)o[i+c]=(uint8_t)(((int)a[i+c]+(int)b[i+c]+1)>>1);o[i+3]=255;}}

// ---- minimal PNG (stored deflate) ----
static void pngChunk(FILE* f,const char* t,const uint8_t* d,uint32_t n){
    uint8_t h[8]; h[0]=n>>24;h[1]=n>>16;h[2]=n>>8;h[3]=n; memcpy(h+4,t,4); fwrite(h,1,8,f);
    uint32_t c=0xffffffffu; auto up=[&](const uint8_t* p,uint32_t k){for(uint32_t i=0;i<k;++i){c^=p[i];for(int j=0;j<8;++j)c=(c>>1)^(0xedb88320u&(uint32_t)(-(int)(c&1)));}};
    up((const uint8_t*)t,4); up(d,n); fwrite(d,1,n,f);
    uint8_t tl[4]={(uint8_t)((c^0xffffffffu)>>24),(uint8_t)((c^0xffffffffu)>>16),(uint8_t)((c^0xffffffffu)>>8),(uint8_t)(c^0xffffffffu)}; fwrite(tl,1,4,f);
}
static void writePNG(const char* path,const uint8_t* rgba,int w,int h){
    FILE* f=fopen(path,"wb"); if(!f){printf("cannot write %s\n",path);return;}
    static const uint8_t sig[8]={0x89,'P','N','G','\r','\n',0x1a,'\n'}; fwrite(sig,1,8,f);
    uint8_t ihdr[13]={}; ihdr[0]=w>>24;ihdr[1]=w>>16;ihdr[2]=w>>8;ihdr[3]=w; ihdr[4]=h>>24;ihdr[5]=h>>16;ihdr[6]=h>>8;ihdr[7]=h; ihdr[8]=8; ihdr[9]=2;
    pngChunk(f,"IHDR",ihdr,13);
    std::vector<uint8_t> raw; raw.reserve((size_t)w*3*h+h);
    for(int y=0;y<h;++y){raw.push_back(0); const uint8_t* r=rgba+(size_t)y*w*4;
        for(int x=0;x<w;++x){raw.push_back(r[x*4]);raw.push_back(r[x*4+1]);raw.push_back(r[x*4+2]);}}
    std::vector<uint8_t> z; z.push_back(0x78);z.push_back(0x01); size_t off=0;
    while(off<raw.size()){size_t n=std::min<size_t>(65535,raw.size()-off);
        z.push_back((off+n>=raw.size())?1:0); z.push_back(n&0xff);z.push_back(n>>8); z.push_back(~n&0xff);z.push_back((~n>>8)&0xff);
        z.insert(z.end(),raw.begin()+off,raw.begin()+off+n); off+=n;}
    uint32_t a=1,b=0; for(size_t i=0;i<raw.size();++i){a=(a+raw[i])%65521;b=(b+a)%65521;} uint32_t ad=(b<<16)|a;
    z.push_back(ad>>24);z.push_back(ad>>16);z.push_back(ad>>8);z.push_back(ad);
    pngChunk(f,"IDAT",z.data(),(uint32_t)z.size()); pngChunk(f,"IEND",nullptr,0); fclose(f);
    printf("  wrote %s\n",path);
}

int main(){
    setvbuf(stdout,nullptr,_IONBF,0);
    Gpu g; if(!g.init()){printf("D3D12 init failed\n");return 1;}
    std::vector<uint8_t> A((size_t)W*H*4),B((size_t)W*H*4),M((size_t)W*H*4);
    renderScene(A.data(),0.0f); renderScene(M.data(),0.5f); renderScene(B.data(),1.0f);
    auto lA=lumaOf(A.data()),lB=lumaOf(B.data());

    // NVOFA flow
    HMODULE h=LoadLibraryA("nvofapi64.dll");
    auto pInst=(decltype(NvOFAPICreateInstanceD3D12)*)GetProcAddress(h,"NvOFAPICreateInstanceD3D12");
    NV_OF_D3D12_API_FUNCTION_LIST of={}; uint32_t ver=0;
    ((decltype(NvOFGetMaxSupportedApiVersion)*)GetProcAddress(h,"NvOFGetMaxSupportedApiVersion"))(&ver); pInst(ver,&of);
    NvOFHandle hOf=nullptr; of.nvCreateOpticalFlowD3D12(g.dev,&hOf);
    NV_OF_INIT_PARAMS ip={}; ip.width=W; ip.height=H; ip.outGridSize=NV_OF_OUTPUT_VECTOR_GRID_SIZE_4;
    ip.hintGridSize=NV_OF_HINT_VECTOR_GRID_SIZE_4; ip.mode=NV_OF_MODE_OPTICALFLOW;
    ip.perfLevel=NV_OF_PERF_LEVEL_FAST; ip.inputBufferFormat=NV_OF_BUFFER_FORMAT_GRAYSCALE8;
    ip.predDirection=NV_OF_PRED_DIRECTION_FORWARD; of.nvOFInit(hOf,&ip);
    ID3D12Resource* tA=g.tex(DXGI_FORMAT_R8_UNORM,W,H,D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource* tB=g.tex(DXGI_FORMAT_R8_UNORM,W,H,D3D12_RESOURCE_STATE_COPY_DEST);
    g.upload(tA,lA.data(),W,H,1); g.upload(tB,lB.data(),W,H,1);
    g.transition(tA,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);
    g.transition(tB,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);
    uint32_t ow=W/GRID,oh=H/GRID;
    ID3D12Resource* tF=g.tex(DXGI_FORMAT_R16G16_SINT,ow,oh,D3D12_RESOURCE_STATE_COMMON);
    auto reg=[&](ID3D12Resource* t,NvOFGPUBufferHandle* hh){NV_OF_REGISTER_RESOURCE_PARAMS_D3D12 p={};p.resource=t;p.hOFGpuBuffer=hh;return of.nvOFRegisterResourceD3D12(hOf,&p);};
    NvOFGPUBufferHandle hA=nullptr,hB=nullptr,hF=nullptr; reg(tA,&hA);reg(tB,&hB);reg(tF,&hF);
    ID3D12Fence* nof=nullptr; g.dev->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&nof));
    HANDLE fe=CreateEvent(nullptr,FALSE,FALSE,nullptr);
    NV_OF_FENCE_POINT inFp={nof,0},outFp={nof,1};
    NV_OF_EXECUTE_INPUT_PARAMS_D3D12 in={}; in.inputFrame=hA; in.referenceFrame=hB; in.disableTemporalHints=(NV_OF_BOOL)1; in.numFencePoints=1; in.fencePoint=&inFp;
    NV_OF_EXECUTE_OUTPUT_PARAMS_D3D12 out={}; out.outputBuffer=hF; out.fencePoint=&outFp;
    int es=of.nvOFExecuteD3D12(hOf,&in,&out);
    while(nof->GetCompletedValue()<1) Sleep(0);
    std::vector<int16_t> gf((size_t)ow*oh*2); g.readback(tF,gf.data(),ow,oh,4);

    // upsample flow to full res
    std::vector<float> fab((size_t)W*H*2);
    for(uint32_t y=0;y<H;++y)for(uint32_t x=0;x<W;++x){
        float gx=((float)x+0.5f)/(float)GRID-0.5f,gy=((float)y+0.5f)/(float)GRID-0.5f;
        int x0=std::max(0,std::min((int)ow-2,(int)floorf(gx))),y0=std::max(0,std::min((int)oh-2,(int)floorf(gy)));
        float fx=gx-x0,fy=gy-y0,dx=0,dy=0;
        for(int yy=0;yy<2;++yy)for(int xx=0;xx<2;++xx){float w=(xx?fx:1-fx)*(yy?fy:1-fy);int cx=x0+xx,cy=y0+yy;
            dx+=w*(gf[(size_t)cy*ow*2+cx*2]/32.0f); dy+=w*(gf[(size_t)cy*ow*2+cx*2+1]/32.0f);}
        fab[((size_t)y*W+x)*2]=dx; fab[((size_t)y*W+x)*2+1]=dy;}
    std::vector<float> fba=fab; for(auto& v:fba)v=-v;
    std::vector<float> cost((size_t)W*H,0.0f);

    ID3D12Resource* tfab=g.tex(DXGI_FORMAT_R32G32_FLOAT,W,H,D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource* tfba=g.tex(DXGI_FORMAT_R32G32_FLOAT,W,H,D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource* tcost=g.tex(DXGI_FORMAT_R32_FLOAT,W,H,D3D12_RESOURCE_STATE_COPY_DEST);
    g.upload(tfab,fab.data(),W,H,8); g.upload(tfba,fba.data(),W,H,8); g.upload(tcost,cost.data(),W,H,4);
    g.transition(tfab,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);
    g.transition(tfba,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);
    g.transition(tcost,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);

    ID3D12Resource* texA=g.tex(DXGI_FORMAT_R8G8B8A8_UNORM,W,H,D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource* texB=g.tex(DXGI_FORMAT_R8G8B8A8_UNORM,W,H,D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource* texOut=g.tex(DXGI_FORMAT_R8G8B8A8_UNORM,W,H,D3D12_RESOURCE_STATE_COMMON);
    g.upload(texA,A.data(),W,H,4); g.upload(texB,B.data(),W,H,4);
    g.transition(texA,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);
    g.transition(texB,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);

    VfiEngine eng; std::string err; VfiOptions opt; opt.temporal_aa=false; opt.luma_fp16=true;
    wchar_t wpath[MAX_PATH*2]; MultiByteToWideChar(CP_UTF8,0,"src/shaders/vfi.hlsl",-1,wpath,MAX_PATH*2);
    if(!eng.init(g.dev,DXGI_FORMAT_R8G8B8A8_UNORM,W,H,opt,wpath,&err)){printf("VFI init failed: %s\n",err.c_str());return 1;}
    eng.set_flow_override(tfab,tfba,tcost);

    // interpolate at three times to demonstrate real interpolation
    std::vector<uint8_t> gen((size_t)W*H*4);
    for(float t : {0.25f,0.5f,0.75f}){
        g.alloc->Reset(); g.list->Reset(g.alloc,nullptr);
        eng.record(g.list,0,texA,D3D12_RESOURCE_STATE_COMMON,texB,D3D12_RESOURCE_STATE_COMMON,
                   texOut,D3D12_RESOURCE_STATE_COMMON,t,false);
        g.list->Close(); g.flush();
        g.readback(texOut,gen.data(),W,H,4);
        char nm[64]; snprintf(nm,sizeof(nm),"nvof_demo_gen_%03d.png",(int)(t*100));
        writePNG(nm,gen.data(),W,H);
    }
    // reference images + diff at t=0.5
    writePNG("nvof_demo_A.png",A.data(),W,H);
    writePNG("nvof_demo_B.png",B.data(),W,H);
    writePNG("nvof_demo_truth.png",M.data(),W,H);
    // t=0.5 gen again + mix + diff
    g.alloc->Reset(); g.list->Reset(g.alloc,nullptr);
    eng.record(g.list,0,texA,D3D12_RESOURCE_STATE_COMMON,texB,D3D12_RESOURCE_STATE_COMMON,texOut,D3D12_RESOURCE_STATE_COMMON,0.5f,false);
    g.list->Close(); g.flush(); g.readback(texOut,gen.data(),W,H,4);
    writePNG("nvof_demo_gen.png",gen.data(),W,H);
    std::vector<uint8_t> mix((size_t)W*H*4); mix5050(A.data(),B.data(),mix.data());
    writePNG("nvof_demo_mix5050.png",mix.data(),W,H);
    std::vector<uint8_t> diff((size_t)W*H*4); for(size_t i=0;i<(size_t)W*H*4;i+=4){int d=0;
        for(int c=0;c<3;++c)d=std::max(d,abs((int)gen[i+c]-(int)M[i+c])); int v=std::min(255,d*4);
        diff[i]=v;diff[i+1]=v;diff[i+2]=v;diff[i+3]=255;}
    writePNG("nvof_demo_diff.png",diff.data(),W,H);

    printf("\n=== 对撞结果 @1080p ===\n");
    printf("  50/50 直接平均   : %.2f dB\n", psnr(mix.data(),M.data()));
    printf("  硬件光流 + 混合  : %.2f dB\n", psnr(gen.data(),M.data()));
    of.nvOFDestroy(hOf);
    return 0;
}
