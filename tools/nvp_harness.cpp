// ============================================================================
//  nvp_harness - drive the re-hosted Smooth Motion entry points end to end.
//
//  Loads NvPresent64.dll, applies the verified double patch (allow tier 2 +
//  force FP16), hooks cuModuleLoadData to patch the fatbinaries, then calls the
//  NVP_* entry points in driver order against a real D3D12 device + swapchain
//  and reports every status code.
//
//  This is the last missing piece for re-hosting Smooth Motion: everything up
//  to and including kernel load is proven; this exercises the host itself.
// ============================================================================
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <cstdio>
#include <cstdint>
#include <cstring>

#pragma comment(lib,"d3d12.lib")
#pragma comment(lib,"dxgi.lib")

// ---- verified patch offsets (.text RVA 0x1000 -> raw 0x400, so raw = RVA-0xC00) ----
static const uint32_t RVA_CMP = 0xc41f;   // cmp [rcx+0x14], imm8
static const uint32_t RVA_SETGE = 0xc437; // setge sil
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

// ---- NVP_* function pointers (x64 unified calling convention) ----
typedef bool  (*pfnInitD3D)(void);
typedef void* (*pfnCreateDevice)(void*);
typedef void* (*pfnCreateSwapchainD3D12)(void*);
typedef void* (*pfnDestroyDevice)(void*);
typedef void* (*pfnDestroySwapchain)(void*, void*);

int main(){
    setvbuf(stdout,nullptr,_IONBF,0);
    HMODULE nv=LoadLibraryA("C:////Windows////System32////DriverStore////FileRepository////nv_dispi.inf_amd64_a3944b54ff18b284////NvPresent64.dll");
    if(!nv){ printf("LoadLibrary NvPresent64 failed\n"); return 1; }
    printf("loaded NvPresent64 @ %p\n", nv);

    // 1. gate double patch
    uint8_t* c=(uint8_t*)nv+RVA_CMP; uint8_t two=0x02;
    uint8_t x[4]={0x40,0x32,0xF6,0x90};
    if(*c!=0x03){ printf("gate immediate mismatch %#x\n",*c); return 1; }
    patchb(c,&two,1); patchb((uint8_t*)nv+RVA_SETGE,x,4);
    printf("gate double-patched (cmp 3->2 + setge->xor sil)\n");

    // 2. fatbin hook
    HMODULE cu=LoadLibraryA("nvcuda.dll");
    void* ld=(void*)GetProcAddress(cu,"cuModuleLoadData");
    if(ld&&hook(ld,(void*)&hook_load,(void**)&real_load)){ real_load=(LoadData_t)real_load; printf("cuModuleLoadData hooked\n"); }

    // 3. NVP_Init_D3D
    pfnInitD3D NVP_Init_D3D=(pfnInitD3D)GetProcAddress(nv,"NVP_Init_D3D");
    // dump the global config struct's gate fields before/after
    uint8_t* S = (uint8_t*)nv + 0x7d7810;
    auto dump = [&](const char* tag){
        printf("  [%s] s2=%02x s3=%02x s48=%08x s4c=%02x s6c=%08x se9=%02x | 12a1=%02x 12a2=%02x 12a3=%02x 12a4=%02x 12a5=%02x\n",
            tag, S[2], S[3], *(uint32_t*)(S+0x48), S[0x4c], *(uint32_t*)(S+0x6c), S[0xe9],
            S[0x12a1], S[0x12a2], S[0x12a3], S[0x12a4], S[0x12a5]);
    };
    dump("before Init");
    // ---- 3rd gate (NVP_Init_D3D feature check) ----
    // The init walks a global config struct @ base+0x7d7810.  Its verdict is:
    //   struct[0x12a5] = struct[0x12a1] & struct[0x12a3] & struct[0x12a4]
    // where [0x12a4] is gated by case-3 of the initializer 0x9fd0:
    //   test [0x48],3 ; cmp [0x4c],0  ->  if ([0x48]&3)==0 OR [0x4c]!=0 then
    //                                     [0x12a4]=[0xe9]  else [0x12a4]=0
    // On this box [0x48] is pre-filled 0xffffffff and [0xe9] is NEVER written by
    // NvPresent64 (it is driver-supplied), so in a bare harness the verdict is 0.
    // Injecting the two driver-supplied capability flags opens the gate and the
    // subsequent NVP_CreateDevice / NVP_CreateSwapchain_D3D12 both succeed.
    S[0x4c] = 1; S[0xe9] = 1;
    printf("  [inject capability flags 0x4c=1, 0xe9=1]\n");
    bool ok = NVP_Init_D3D ? NVP_Init_D3D() : false;
    dump("after  Init");
    printf("NVP_Init_D3D() -> %s\n", ok?"TRUE":"FALSE");
    if(!ok){ printf("gate did not open (feature check failed)\n"); return 0; }

    // 4. device + swapchain
    ID3D12Device* dev=nullptr;
    if(FAILED(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&dev)))){printf("D3D12CreateDevice failed\n");return 1;}
    ID3D12CommandQueue* cq=nullptr;
    D3D12_COMMAND_QUEUE_DESC qd={}; qd.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
    dev->CreateCommandQueue(&qd,IID_PPV_ARGS(&cq));
    // minimal hidden window + swapchain
    WNDCLASSA wc={}; wc.lpfnWndProc=DefWindowProcA; wc.hInstance=GetModuleHandleA(nullptr); wc.lpszClassName="nvph";
    RegisterClassA(&wc);
    HWND wnd=CreateWindowA("nvph","nvph",WS_OVERLAPPEDWINDOW,0,0,64,64,nullptr,nullptr,wc.hInstance,nullptr);
    IDXGIFactory4* f=nullptr; CreateDXGIFactory1(IID_PPV_ARGS(&f));
    DXGI_SWAP_CHAIN_DESC1 sd={}; sd.Width=64; sd.Height=64; sd.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count=1; sd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount=2; sd.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
    IDXGISwapChain1* swap=nullptr; f->CreateSwapChainForHwnd(cq,wnd,&sd,nullptr,nullptr,&swap);
    if(!swap){ printf("swapchain create failed\n"); return 1; }

    // 5. NVP_CreateDevice + NVP_CreateSwapchain_D3D12
    pfnCreateDevice NVP_CreateDevice=(pfnCreateDevice)GetProcAddress(nv,"NVP_CreateDevice");
    pfnCreateSwapchainD3D12 NVP_CS=(pfnCreateSwapchainD3D12)GetProcAddress(nv,"NVP_CreateSwapchain_D3D12");
    void* r1 = NVP_CreateDevice ? NVP_CreateDevice(dev) : (void*)0xdead;
    printf("NVP_CreateDevice(dev) -> %p\n", r1);
    void* r2 = (NVP_CS && swap) ? NVP_CS(swap) : (void*)0xdead;
    printf("NVP_CreateSwapchain_D3D12(swap) -> %p\n", r2);

    printf("\nharness reached the NVP entry points; see statuses above.\n");
    printf("(further present-loop exercise needs a real render loop + the driver's\n");
    printf(" expected context; the entry points themselves are now callable.)\n");
    return 0;
}
