#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <cstdio>
#include <cstdint>
#include <cstring>

#pragma comment(lib,"d3d12.lib")
#pragma comment(lib,"dxgi.lib")
#pragma comment(lib,"user32.lib")

static const uint32_t RVA_CMP = 0xc41f;
static const uint32_t RVA_SETGE = 0xc437;
static const uint32_t FATBIN_MAGIC = 0xba55ed50;

static bool patchb(uint8_t* p, const uint8_t* pat, size_t n){
    DWORD o=0; if(!VirtualProtect(p,n,PAGE_EXECUTE_READWRITE,&o))return false;
    memcpy(p,pat,n); VirtualProtect(p,n,o,&o); FlushInstructionCache(GetCurrentProcess(),p,n); return true;
}

static void patch_fatbin(uint8_t* b, size_t size){
    for(size_t off=0; off+3<size; off+=4)
        if((b[off]==0x78||b[off]==0x59)&&b[off+1]==0&&b[off+2]==0&&b[off+3]==0) b[off]=0x56;
    for(size_t off=0; off+0x34<size; ++off)
        if(b[off]==0x7f&&b[off+1]=='E'&&b[off+2]=='L'&&b[off+3]=='F'){ uint32_t fl=0x560556; memcpy(b+off+0x30,&fl,4); }
}

typedef int (__stdcall *LoadData_t)(void**, const void*);
static LoadData_t real_load = nullptr;
static int __stdcall hook_load(void** m, const void* img){
    const uint8_t* p=(const uint8_t*)img;
    if(p && *(const uint32_t*)p==FATBIN_MAGIC){
        static uint8_t buf[8*1024*1024];
        uint64_t sz=*(const uint64_t*)(p+8);
        if(sz<sizeof(buf)){ memcpy(buf,p,(size_t)sz); patch_fatbin(buf,(size_t)sz); p=buf; }
    }
    return real_load(m,p);
}
static bool hook(void* t, void* d, void** out){
    uint8_t* tr=(uint8_t*)VirtualAlloc(nullptr,32,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE);
    memcpy(tr,t,12); tr[12]=0x48; tr[13]=0xB8; *(uint64_t*)(tr+14)=(uint64_t)((uint8_t*)t+12); tr[22]=0xFF; tr[23]=0xE0;
    uint8_t j[12]={}; j[0]=0x48; j[1]=0xB8; *(uint64_t*)(j+2)=(uint64_t)d; j[10]=0xFF; j[11]=0xE0;
    if(!patchb((uint8_t*)t,j,12))return false; *out=tr; return true;
}

typedef bool (*pfnInitD3D)(void);
typedef bool (*pfnCreateDevice)(void*);
typedef bool (*pfnCreateSwapchainD3D12)(void*);

struct NVP_DEVICE_DESC {
    uint32_t api_type;     // 0 = D3D12
    uint32_t pad;
    ID3D12Device* pDevice; // offset 8
};

static LONG WINAPI CrashHandler(EXCEPTION_POINTERS* ep) {
    printf("\n!!! CRASH: Exception code 0x%08X at address %p\n",
           ep->ExceptionRecord->ExceptionCode, ep->ExceptionRecord->ExceptionAddress);
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        printf("  Access violation type: %s at target address %p\n",
               ep->ExceptionRecord->ExceptionInformation[0] == 0 ? "READ" : "WRITE",
               (void*)ep->ExceptionRecord->ExceptionInformation[1]);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

int main(){
    setvbuf(stdout,nullptr,_IONBF,0);
    SetUnhandledExceptionFilter(CrashHandler);

    HMODULE nv=LoadLibraryA("C:////Windows////System32////DriverStore////FileRepository////nv_dispi.inf_amd64_a3944b54ff18b284////NvPresent64.dll");
    if(!nv){ printf("LoadLibrary NvPresent64 failed\n"); return 1; }
    printf("loaded NvPresent64 @ %p\n", nv);

    // 1. gate double patch
    uint8_t* c=(uint8_t*)nv+RVA_CMP; uint8_t two=0x02;
    uint8_t x[4]={0x40,0x32,0xF6,0x90};
    patchb(c,&two,1); patchb((uint8_t*)nv+RVA_SETGE,x,4);
    printf("gate double-patched\n");

    // 2. fatbin hook
    HMODULE cu=LoadLibraryA("nvcuda.dll");
    void* ld=(void*)GetProcAddress(cu,"cuModuleLoadData");
    if(ld&&hook(ld,(void*)&hook_load,(void**)&real_load)){ real_load=(LoadData_t)real_load; printf("cuModuleLoadData hooked\n"); }

    // 3. NVP_Init_D3D
    pfnInitD3D NVP_Init_D3D=(pfnInitD3D)GetProcAddress(nv,"NVP_Init_D3D");
    uint8_t* S = (uint8_t*)nv + 0x7d7810;
    S[0x4c] = 1; S[0xe9] = 1;
    bool initOk = NVP_Init_D3D ? NVP_Init_D3D() : false;
    printf("NVP_Init_D3D() -> %s\n", initOk?"TRUE":"FALSE");
    if(!initOk) return 1;

    // 4. Create real D3D12 device + command queue + swapchain
    ID3D12Device* dev=nullptr;
    if(FAILED(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&dev)))){printf("D3D12CreateDevice failed\n");return 1;}
    ID3D12CommandQueue* cq=nullptr;
    D3D12_COMMAND_QUEUE_DESC qd={}; qd.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
    dev->CreateCommandQueue(&qd,IID_PPV_ARGS(&cq));

    WNDCLASSA wc={}; wc.lpfnWndProc=DefWindowProcA; wc.hInstance=GetModuleHandleA(nullptr); wc.lpszClassName="nvph_real";
    RegisterClassA(&wc);
    HWND wnd=CreateWindowA("nvph_real","nvph_real",WS_OVERLAPPEDWINDOW,0,0,64,64,nullptr,nullptr,wc.hInstance,nullptr);
    IDXGIFactory4* f=nullptr; CreateDXGIFactory1(IID_PPV_ARGS(&f));
    DXGI_SWAP_CHAIN_DESC1 sd={}; sd.Width=64; sd.Height=64; sd.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count=1; sd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount=2; sd.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
    IDXGISwapChain1* swap=nullptr; f->CreateSwapChainForHwnd(cq,wnd,&sd,nullptr,nullptr,&swap);
    if(!swap){ printf("swapchain create failed\n"); return 1; }

    // 5. Call NVP_CreateDevice with NVP_DEVICE_DESC
    pfnCreateDevice NVP_CreateDevice=(pfnCreateDevice)GetProcAddress(nv,"NVP_CreateDevice");
    pfnCreateSwapchainD3D12 NVP_CS=(pfnCreateSwapchainD3D12)GetProcAddress(nv,"NVP_CreateSwapchain_D3D12");

    NVP_DEVICE_DESC ddesc = {};
    ddesc.api_type = 0; // D3D12
    ddesc.pDevice = dev;
    bool devOk = NVP_CreateDevice(&ddesc);
    printf("NVP_CreateDevice(&ddesc) -> %s\n", devOk ? "TRUE" : "FALSE");

    uint8_t sdesc_buf[512] = {};
    *(IDXGISwapChain1**)(sdesc_buf + 0x00) = swap;
    *(uint32_t*)(sdesc_buf + 0x08) = 2; // format enum: 2 = DXGI_FORMAT_R8G8B8A8_UNORM
    *(uint32_t*)(sdesc_buf + 0x0c) = sd.Width;
    *(uint32_t*)(sdesc_buf + 0x10) = sd.Height;
    *(uint32_t*)(sdesc_buf + 0x14) = sd.BufferCount;
    *(ID3D12Device**)(sdesc_buf + 0x18) = dev;
    *(ID3D12CommandQueue**)(sdesc_buf + 0x78) = cq;
    *(HWND*)(sdesc_buf + 0x80) = wnd;

    void** vt_before = *(void***)swap;
    void* pres_before = vt_before[8];
    void* pres1_before = vt_before[22];
    printf("Before NVP_CS: vtable=%p, Present=%p, Present1=%p\n", vt_before, pres_before, pres1_before);

    __try {
        printf("Calling NVP_CreateSwapchain_D3D12(sdesc_buf)...\n");
        bool swapOk = NVP_CS(sdesc_buf);
        printf("NVP_CreateSwapchain_D3D12(sdesc_buf) -> %s\n", swapOk ? "TRUE" : "FALSE");
    } __except(CrashHandler(GetExceptionInformation())) {
        printf("Caught crash in NVP_CreateSwapchain_D3D12\n");
    }

    void** vt_after = *(void***)swap;
    void* pres_after = vt_after[8];
    void* pres1_after = vt_after[22];
    printf("After NVP_CS:  vtable=%p, Present=%p, Present1=%p\n", vt_after, pres_after, pres1_after);

    HRESULT hr = swap->Present(0, 0);
    printf("swap->Present(0, 0) -> 0x%08X\n", hr);

    return 0;
}
