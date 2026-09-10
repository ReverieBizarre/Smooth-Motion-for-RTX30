// Validate the reconstructed NVOFA D3D12 interface against a REAL D3D12 device,
// then probe nvOFInit.  The INIT_PARAMS layout is transcribed from the public
// SDK 2.0 header while this driver is SDK 5.0, which renamed perfLevel->preset
// and may have changed the legal values.  So instead of guessing once, sweep
// the level/preset field and report every status.  NV_OF_ERR_INVALID_PARAM (5)
// means the driver rejected the value; 0 means we found the right one.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <stdio.h>
#include "nvof/nvofapi.h"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

static const char* sname(int s)
{
    switch (s) {
    case 0: return "NV_OF_SUCCESS";
    case 1: return "NV_OF_ERR_OF_NOT_AVAILABLE";
    case 2: return "NV_OF_ERR_UNSUPPORTED_DEVICE";
    case 3: return "NV_OF_ERR_DEVICE_DOES_NOT_EXIST";
    case 4: return "NV_OF_ERR_INVALID_PTR";
    case 5: return "NV_OF_ERR_INVALID_PARAM";
    case 6: return "NV_OF_ERR_INVALID_CALL";
    case 7: return "NV_OF_ERR_INVALID_VERSION";
    case 8: return "NV_OF_ERR_OUT_OF_MEMORY";
    case 9: return "NV_OF_ERR_NOT_INITIALIZED";
    case 10: return "NV_OF_ERR_UNSUPPORTED_FEATURE";
    case 11: return "NV_OF_ERR_GENERIC";
    default: return "?";
    }
}

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);

    ID3D12Device* dev = nullptr;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev)))) {
        printf("D3D12CreateDevice failed\n"); return 1;
    }
    IDXGIAdapter1* ad = nullptr;
    { IDXGIFactory4* f=nullptr;
      if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&f)))) {
        f->EnumAdapterByLuid(dev->GetAdapterLuid(), IID_PPV_ARGS(&ad)); f->Release(); } }
    if (ad) { DXGI_ADAPTER_DESC d={}; ad->GetDesc(&d);
              wprintf(L"adapter: %s\n", d.Description); ad->Release(); }

    HMODULE h = LoadLibraryA("nvofapi64.dll");
    if (!h) { printf("LoadLibrary failed\n"); return 1; }
    auto pMax  = (PFN_NVOF_GET_MAX_SUPPORTED_API_VERSION)GetProcAddress(h, "NvOFGetMaxSupportedApiVersion");
    auto pInst = (PFN_NVOF_CREATE_INSTANCE_D3D12)GetProcAddress(h, "NvOFAPICreateInstanceD3D12");

    uint32_t ver=0; NV_OF_STATUS s = pMax ? pMax(&ver) : (NV_OF_STATUS)-1;
    printf("max api version: status %d raw %#x (major %u minor %u)\n", s, ver, ver>>4, ver&0xf);

    NV_OF_D3D12_API_FUNCTION_LIST L = {};
    s = pInst(ver, &L);
    printf("CreateInstanceD3D12: %d %s\n", s, sname(s));
    if (s != 0) return 1;

    NvOFHandle hOf = nullptr;
    s = L.nvCreateOpticalFlowD3D12(dev, &hOf);
    printf("nvCreateOpticalFlowD3D12(dev,&hOf): %d %s  hOf=%p\n", s, sname(s), hOf);
    if (s != 0) return 1;

    // The copy thunk at RVA 0x57e0 reads offsets 0x30/0x34/0x38, which are
    // OUTSIDE the 56-byte SDK 2.0 struct - so passing that struct feeds the
    // driver stack garbage.  Use an oversized zeroed buffer and set fields by
    // offset instead.
    printf("\n--- nvOFInit with a 256-byte zeroed params buffer ---\n");
    int good = -1;
    const uint32_t levels2[] = {0,1,2,3,4,5,10,20,21};
    for (uint32_t lv : levels2) {
        alignas(16) unsigned char buf[256] = {};
        auto d = [&](size_t o, uint32_t v){ memcpy(buf+o, &v, 4); };
        d(0x00, 1920);            // width
        d(0x04, 1080);            // height
        d(0x08, 4);               // outGridSize
        d(0x0c, 4);               // hintGridSize
        d(0x10, 1);               // mode = OPTICALFLOW
        d(0x14, lv);              // level / preset
        s = L.nvOFInit(hOf, (const NV_OF_INIT_PARAMS*)buf);
        printf("    level %-3u : %d %s\n", lv, s, sname(s));
        if (s == 0 && good < 0) good = (int)lv;
    }
    if (good >= 0) {
        printf("\n=> ACCEPTED with level %u. Querying capabilities://n", good);
        uint32_t cnt = 0;
        s = L.nvOFGetSurfaceFormatCountD3D12(hOf, NV_OF_BUFFER_USAGE_INPUT,
                                             NV_OF_MODE_OPTICALFLOW, &cnt);
        printf("   GetSurfaceFormatCountD3D12(INPUT): %d %s count=%u\n", s, sname(s), cnt);
        if (s == 0 && cnt && cnt < 32) {
            DXGI_FORMAT fmts[32] = {};
            s = L.nvOFGetSurfaceFormatD3D12(hOf, NV_OF_BUFFER_USAGE_INPUT,
                                            NV_OF_MODE_OPTICALFLOW, fmts);
            printf("   GetSurfaceFormatD3D12: %d %s\n", s, sname(s));
            for (uint32_t i = 0; i < cnt && i < 12; i++)
                printf("     [%u] DXGI_FORMAT %d\n", i, (int)fmts[i]);
        }
    } else {
        printf("\n=> still rejected: not the struct size either.\n");
    }
    L.nvOFDestroy(hOf);
    printf("nvOFDestroy: called\n");
    dev->Release();
    return 0;
}
