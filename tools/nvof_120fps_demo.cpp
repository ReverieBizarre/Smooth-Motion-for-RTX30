// ============================================================================
//  nvof_120fps_demo - generate a 60fps 3D animation, interpolate it to 120fps
//  with NVIDIA hardware optical flow + the warp/blend stage, and dump the
//  frames so an HTML player can show the result.
//
//  3D scene: a rotating flat-shaded cube, an orbiting shaded sphere, and a
//  translating starfield background (parallax).  Rendered in software with a
//  z-buffer, at 60fps, then every in-between frame is synthesised by NVOFA+VFI.
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
static const uint32_t W=640, H=360, GRID=4, NFRAMES=60;

// ---- D3D12 + NVOFA + VFI plumbing (proven pattern) ----
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

// ---- software 3D renderer: rotating cube + orbiting sphere + starfield ----
struct V3 { float x,y,z; };
static V3 rot(V3 v,float a,float b){ // rotate around Y then X
    float cx=cosf(a),sx=sinf(a),cy=cosf(b),sy=sinf(b);
    float y1=v.y*cy-v.z*sy, z1=v.y*sy+v.z*cy;
    float x2=v.x*cx+z1*sx, z2=-v.x*sx+z1*cx;
    return {x2,y1,z2};
}
static uint32_t hsh2(uint32_t x){x^=x>>16;x*=0x7feb352du;x^=x>>15;x*=0x846ca68bu;x^=x>>16;return x;}
static void render3D(uint8_t* px,float t){
    std::vector<float> zb((size_t)W*H, 1e30f);
    std::vector<uint8_t> col((size_t)W*H*3, 0);
    // background: gradient + translating starfield
    for(uint32_t y=0;y<H;++y)for(uint32_t x=0;x<W;++x){
        float g=0.08f+0.12f*(float)y/H;
        col[((size_t)y*W+x)*3+0]=(uint8_t)(g*255);
        col[((size_t)y*W+x)*3+1]=(uint8_t)(g*255);
        col[((size_t)y*W+x)*3+2]=(uint8_t)(g*255);
    }
    for(int i=0;i<80;++i){
        float sx=fmodf((float)(hsh2((uint32_t)i*7)*0.001f) + t*40.0f, W);
        float sy=(float)(hsh2((uint32_t)i*13)%H);
        int x=(int)sx, y=(int)sy;
        if(x>=0&&x<(int)W&&y>=0&&y<(int)H){col[((size_t)y*W+x)*3+0]=255;col[((size_t)y*W+x)*3+1]=255;col[((size_t)y*W+x)*3+2]=255;}
    }
    auto proj=[&](V3 p){ float f=500.0f; float px=p.x*f/(p.z+f)+W*0.5f; float py=-p.y*f/(p.z+f)+H*0.5f; return V3{px,py,p.z}; };
    auto tri=[&](V3 a,V3 b,V3 c,uint8_t r,uint8_t g,uint8_t bl){
        V3 pa=proj(a),pb=proj(b),pc=proj(c);
        // backface cull via screen-space winding
        float cr=(pb.x-pa.x)*(pc.y-pa.y)-(pc.x-pa.x)*(pb.y-pa.y);
        if(cr<=0) return;
        int x0=(int)std::max(0.0f,std::min((float)W-1,std::min({pa.x,pb.x,pc.x})));
        int x1=(int)std::max(0.0f,std::min((float)W-1,std::max({pa.x,pb.x,pc.x})));
        int y0=(int)std::max(0.0f,std::min((float)H-1,std::min({pa.y,pb.y,pc.y})));
        int y1=(int)std::max(0.0f,std::min((float)H-1,std::max({pa.y,pb.y,pc.y})));
        float area=(pb.x-pa.x)*(pc.y-pa.y)-(pc.x-pa.x)*(pb.y-pa.y);
        for(int y=y0;y<=y1;++y)for(int x=x0;x<=x1;++x){
            float px=x+0.5f,py=y+0.5f;
            float w0=((pb.x-px)*(pc.y-py)-(pc.x-px)*(pb.y-py))/area;
            float w1=((pc.x-px)*(pa.y-py)-(pa.x-px)*(pc.y-py))/area;
            float w2=1-w0-w1;
            if(w0<0||w1<0||w2<0) continue;
            float z=pa.z*w0+pb.z*w1+pc.z*w2;
            size_t idx=(size_t)y*W+x;
            if(z<zb[idx]){ zb[idx]=z;
                col[idx*3+0]=r; col[idx*3+1]=g; col[idx*3+2]=bl; }
        }
    };
    // cube (rotating), flat shaded
    float a=t*1.6f, b=t*0.7f;
    V3 verts[8]={{-1,-1,-1},{1,-1,-1},{1,1,-1},{-1,1,-1},{-1,-1,1},{1,-1,1},{1,1,1},{-1,1,1}};
    for(int i=0;i<8;++i){ verts[i]=rot(verts[i],a,b); verts[i].x*=60; verts[i].y*=60; verts[i].z*=60; verts[i].z+=140; }
    int faces[12][3]={{0,1,2},{0,2,3},{4,5,6},{4,6,7},{0,4,7},{0,7,3},{1,5,6},{1,6,2},{0,1,5},{0,5,4},{3,2,6},{3,6,7}};
    float shade[12];
    for(int f=0;f<12;++f){ V3 p0=verts[faces[f][0]],p1=verts[faces[f][1]],p2=verts[faces[f][2]];
        V3 n={ (p1.y-p0.y)*(p2.z-p0.z)-(p1.z-p0.z)*(p2.y-p0.y),
               (p1.z-p0.z)*(p2.x-p0.x)-(p1.x-p0.x)*(p2.z-p0.z),
               (p1.x-p0.x)*(p2.y-p0.y)-(p1.y-p0.y)*(p2.x-p0.x) };
        float L=sqrtf(n.x*n.x+n.y*n.y+n.z*n.z)+1e-6f; shade[f]=fabsf(n.z/L)*0.8f+0.2f; }
    for(int f=0;f<12;++f){ uint8_t s=(uint8_t)(shade[f]*255);
        tri(verts[faces[f][0]],verts[faces[f][1]],verts[faces[f][2]],(uint8_t)(s*0.9f),s,(uint8_t)(s*0.5f)); }
    // sphere orbiting
    float sa=t*2.0f;
    V3 sc={cosf(sa)*90.0f, sinf(sa)*30.0f, 120.0f};
    V3 sp=proj(sc); int sr=(int)(22.0f*500.0f/(sc.z+500.0f));
    for(int y=(int)sp.y-sr;y<(int)sp.y+sr;++y)for(int x=(int)sp.x-sr;x<(int)sp.x+sr;++x){
        if(x<0||x>=(int)W||y<0||y>=(int)H)continue;
        float dx=(x-sp.x)/sr,dy=(y-sp.y)/sr,d=sqrtf(dx*dx+dy*dy);
        if(d<=1.0f){ float z=sc.z+sr*(1.0f-d);
            size_t idx=(size_t)y*W+x; if(z<zb[idx]){ zb[idx]=z;
                float sh=0.4f+0.6f*(1-d*d);
                col[idx*3+0]=(uint8_t)(sh*255); col[idx*3+1]=(uint8_t)(sh*180); col[idx*3+2]=(uint8_t)(sh*80); } }
    }
    for(uint32_t y=0;y<H;++y)for(uint32_t x=0;x<W;++x){ size_t i=((size_t)y*W+x)*4;
        px[i]=col[((size_t)y*W+x)*3+0]; px[i+1]=col[((size_t)y*W+x)*3+1]; px[i+2]=col[((size_t)y*W+x)*3+2]; px[i+3]=255; }
}
static std::vector<uint8_t> lumaOf(const uint8_t* rgba){std::vector<uint8_t> l((size_t)W*H);
    for(size_t i=0,j=0;i<(size_t)W*H*4;i+=4,++j)l[j]=(uint8_t)((rgba[i]*54+rgba[i+1]*183+rgba[i+2]*19)>>8);return l;}

static void pngChunk(FILE* f,const char* t,const uint8_t* d,uint32_t n){
    uint8_t h[8]; h[0]=n>>24;h[1]=n>>16;h[2]=n>>8;h[3]=n; memcpy(h+4,t,4); fwrite(h,1,8,f);
    uint32_t c=0xffffffffu; auto up=[&](const uint8_t* p,uint32_t k){for(uint32_t i=0;i<k;++i){c^=p[i];for(int j=0;j<8;++j)c=(c>>1)^(0xedb88320u&(uint32_t)(-(int)(c&1)));}};
    up((const uint8_t*)t,4); up(d,n); fwrite(d,1,n,f);
    uint8_t tl[4]={(uint8_t)((c^0xffffffffu)>>24),(uint8_t)((c^0xffffffffu)>>16),(uint8_t)((c^0xffffffffu)>>8),(uint8_t)(c^0xffffffffu)}; fwrite(tl,1,4,f);
}
static void writePNG(const char* path,const uint8_t* rgba,int w,int h){
    FILE* f=fopen(path,"wb"); if(!f)return;
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
}

int main(){
    setvbuf(stdout,nullptr,_IONBF,0);
    Gpu g; if(!g.init()){printf("D3D12 init failed\n");return 1;}
    HMODULE h=LoadLibraryA("nvofapi64.dll");
    auto pInst=(decltype(NvOFAPICreateInstanceD3D12)*)GetProcAddress(h,"NvOFAPICreateInstanceD3D12");
    NV_OF_D3D12_API_FUNCTION_LIST of={}; uint32_t ver=0;
    ((decltype(NvOFGetMaxSupportedApiVersion)*)GetProcAddress(h,"NvOFGetMaxSupportedApiVersion"))(&ver); pInst(ver,&of);
    NvOFHandle hOf=nullptr; of.nvCreateOpticalFlowD3D12(g.dev,&hOf);
    NV_OF_INIT_PARAMS ip={}; ip.width=W; ip.height=H; ip.outGridSize=NV_OF_OUTPUT_VECTOR_GRID_SIZE_4;
    ip.hintGridSize=NV_OF_HINT_VECTOR_GRID_SIZE_4; ip.mode=NV_OF_MODE_OPTICALFLOW;
    ip.perfLevel=NV_OF_PERF_LEVEL_FAST; ip.inputBufferFormat=NV_OF_BUFFER_FORMAT_GRAYSCALE8;
    ip.predDirection=NV_OF_PRED_DIRECTION_FORWARD; of.nvOFInit(hOf,&ip);
    uint32_t ow=W/GRID,oh=H/GRID;

    VfiEngine eng; std::string err; VfiOptions opt; opt.temporal_aa=false; opt.luma_fp16=true;
    wchar_t wpath[MAX_PATH*2]; MultiByteToWideChar(CP_UTF8,0,"src/shaders/vfi.hlsl",-1,wpath,MAX_PATH*2);
    if(!eng.init(g.dev,DXGI_FORMAT_R8G8B8A8_UNORM,W,H,opt,wpath,&err)){printf("VFI init failed: %s\n",err.c_str());return 1;}

    // persistent resources (reused every pair)
    ID3D12Resource* tA=g.tex(DXGI_FORMAT_R8_UNORM,W,H,D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource* tB=g.tex(DXGI_FORMAT_R8_UNORM,W,H,D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource* tF=g.tex(DXGI_FORMAT_R16G16_SINT,ow,oh,D3D12_RESOURCE_STATE_COMMON);
    ID3D12Resource* tfab=g.tex(DXGI_FORMAT_R32G32_FLOAT,W,H,D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource* tfba=g.tex(DXGI_FORMAT_R32G32_FLOAT,W,H,D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource* tcost=g.tex(DXGI_FORMAT_R32_FLOAT,W,H,D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource* texA=g.tex(DXGI_FORMAT_R8G8B8A8_UNORM,W,H,D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource* texB=g.tex(DXGI_FORMAT_R8G8B8A8_UNORM,W,H,D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource* texOut=g.tex(DXGI_FORMAT_R8G8B8A8_UNORM,W,H,D3D12_RESOURCE_STATE_COMMON);
    auto reg=[&](ID3D12Resource* t,NvOFGPUBufferHandle* hh){NV_OF_REGISTER_RESOURCE_PARAMS_D3D12 p={};p.resource=t;p.hOFGpuBuffer=hh;return of.nvOFRegisterResourceD3D12(hOf,&p);};
    NvOFGPUBufferHandle hA=nullptr,hB=nullptr,hF=nullptr; reg(tA,&hA);reg(tB,&hB);reg(tF,&hF);
    ID3D12Fence* nof=nullptr; g.dev->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&nof));
    HANDLE fe=CreateEvent(nullptr,FALSE,FALSE,nullptr);
    std::vector<float> cost((size_t)W*H,0.0f);
    g.upload(tcost,cost.data(),W,H,4); g.transition(tcost,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);
    eng.set_flow_override(tfab,tfba,tcost);

    std::vector<uint8_t> A((size_t)W*H*4),B((size_t)W*H*4),gen((size_t)W*H*4);
    std::vector<int16_t> gf((size_t)ow*oh*2);
    std::vector<float> fab((size_t)W*H*2), fba((size_t)W*H*2);

    double sumPSNR=0, sumMs=0;
    LARGE_INTEGER fr; QueryPerformanceFrequency(&fr);
    char dir[256]="demo_120"; CreateDirectoryA(dir,nullptr);
    for(uint32_t k=0;k<NFRAMES;++k){
        float t0=(float)k/60.0f, t1=(float)(k+1)/60.0f;
        render3D(A.data(),t0); render3D(B.data(),t1);
        // source frame (60fps) - save A
        char nm[256]; snprintf(nm,sizeof(nm),"%s/f_%03d_60fps.png",dir,k); writePNG(nm,A.data(),W,H);
        // luma -> NVOFA
        auto lA=lumaOf(A.data()), lB=lumaOf(B.data());
        g.upload(tA,lA.data(),W,H,1); g.upload(tB,lB.data(),W,H,1);
        g.transition(tA,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);
        g.transition(tB,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);
        NV_OF_FENCE_POINT inFp={nof,(uint64_t)k}, outFp={nof,(uint64_t)k+1};
        NV_OF_EXECUTE_INPUT_PARAMS_D3D12 in={}; in.inputFrame=hA; in.referenceFrame=hB; in.disableTemporalHints=(NV_OF_BOOL)1; in.numFencePoints=1; in.fencePoint=&inFp;
        NV_OF_EXECUTE_OUTPUT_PARAMS_D3D12 out={}; out.outputBuffer=hF; out.fencePoint=&outFp;
        LARGE_INTEGER m0,m1; QueryPerformanceCounter(&m0);
        of.nvOFExecuteD3D12(hOf,&in,&out);
        while(nof->GetCompletedValue()< (k+1)) Sleep(0);
        QueryPerformanceCounter(&m1);
        g.readback(tF,gf.data(),ow,oh,4);
        // upsample + upload flow
        for(uint32_t y=0;y<H;++y)for(uint32_t x=0;x<W;++x){
            float gx=((float)x+0.5f)/(float)GRID-0.5f,gy=((float)y+0.5f)/(float)GRID-0.5f;
            int x0=std::max(0,std::min((int)ow-2,(int)floorf(gx))),y0=std::max(0,std::min((int)oh-2,(int)floorf(gy)));
            float fx=gx-x0,fy=gy-y0,dx=0,dy=0;
            for(int yy=0;yy<2;++yy)for(int xx=0;xx<2;++xx){float w=(xx?fx:1-fx)*(yy?fy:1-fy);int cx=x0+xx,cy=y0+yy;
                dx+=w*(gf[(size_t)cy*ow*2+cx*2]/32.0f); dy+=w*(gf[(size_t)cy*ow*2+cx*2+1]/32.0f);}
            fab[((size_t)y*W+x)*2]=dx; fab[((size_t)y*W+x)*2+1]=dy; fba[((size_t)y*W+x)*2]=-dx; fba[((size_t)y*W+x)*2+1]=-dy; }
        g.upload(tfab,fab.data(),W,H,8); g.upload(tfba,fba.data(),W,H,8);
        g.transition(tfab,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);
        g.transition(tfba,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);
        g.upload(texA,A.data(),W,H,4); g.upload(texB,B.data(),W,H,4);
        g.transition(texA,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);
        g.transition(texB,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);
        g.alloc->Reset(); g.list->Reset(g.alloc,nullptr);
        eng.record(g.list,0,texA,D3D12_RESOURCE_STATE_COMMON,texB,D3D12_RESOURCE_STATE_COMMON,texOut,D3D12_RESOURCE_STATE_COMMON,0.5f,false);
        g.list->Close(); g.flush();
        g.readback(texOut,gen.data(),W,H,4);
        // interpolated frame (120fps) - save
        snprintf(nm,sizeof(nm),"%s/f_%03d_120fps.png",dir,k); writePNG(nm,gen.data(),W,H);
        // truth for PSNR (render at midpoint)
        std::vector<uint8_t> M((size_t)W*H*4); render3D(M.data(),(t0+t1)*0.5f);
        double se=0; for(size_t i=0;i<(size_t)W*H*4;i+=4)for(int c=0;c<3;++c){double d=(double)gen[i+c]-M[i+c];se+=d*d;}
        double ps=10.0*log10(255.0*255.0/(se/(W*H*3.0)));
        sumPSNR+=ps; sumMs+=1000.0*(double)(m1.QuadPart-m0.QuadPart)/fr.QuadPart;
        if(k%10==0) printf("  frame %3u/%u  psnr %.2f dB  nvof %.3f ms\n", k, NFRAMES, ps, 1000.0*(double)(m1.QuadPart-m0.QuadPart)/fr.QuadPart);
    }
    printf("\n60fps->120fps demo done: %u source + %u interpolated frames\n", NFRAMES, NFRAMES);
    printf("avg PSNR %.2f dB, avg NVOFA %.3f ms/frame\n", sumPSNR/NFRAMES, sumMs/NFRAMES);
    of.nvOFDestroy(hOf);
    return 0;
}
