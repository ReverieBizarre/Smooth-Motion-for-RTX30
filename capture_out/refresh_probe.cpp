// refresh_probe.cpp - what refresh rate is the compositor ACTUALLY running at?
//
// The cadence analysis of the Smooth Motion flicker depends on this number:
// 24fps video doubled by frame generation is 48 fps, which is a clean 3:1 hold
// on a 144 Hz panel but an awkward 4:5 pattern on 60 Hz. EnumDisplaySettings
// reports the negotiated mode; DWM reports the timing it actually composes at.
// This prints both, plus what DXGI advertises for the output.
//
// build (from the project root, output to a temp dir so nothing in the repo
// or in build/Release gets touched):
//   cl /nologo /std:c++17 /EHsc /O2 refresh_probe.cpp /Fe:out.exe ^
//      /link user32.lib dwmapi.lib dxgi.lib d3d11.lib ole32.lib
#include <windows.h>
#include <dwmapi.h>
#include <dxgi1_4.h>
#include <cstdio>
#include <cstdint>

static double g_qpcFreq = 0.0;

int main() {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_qpcFreq = (double)f.QuadPart;

    printf("=== DWM composition timing ===\n");
    for (int i = 0; i < 3; i++) {
        DWM_TIMING_INFO ti = {};
        ti.cbSize = sizeof(ti);
        HRESULT hr = DwmGetCompositionTimingInfo(nullptr, &ti);
        if (FAILED(hr)) {
            printf("  DwmGetCompositionTimingInfo failed: 0x%08X\n", (uint32_t)hr);
            break;
        }
        double periodMs = ti.qpcRefreshPeriod * 1000.0 / g_qpcFreq;
        printf("  compose rate   : %u/%u\n", ti.rateCompose.uiNumerator, ti.rateCompose.uiDenominator);
        printf("  refresh period : %.3f ms  -> %.2f Hz\n", periodMs,
               ti.qpcRefreshPeriod ? g_qpcFreq / (double)ti.qpcRefreshPeriod : 0.0);
        printf("  cRefresh=%llu cFrame=%llu cDXPresent=%llu\n",
               (unsigned long long)ti.cRefresh, (unsigned long long)ti.cFrame,
               (unsigned long long)ti.cDXPresent);
        Sleep(1000);
    }

    printf("\n=== DXGI outputs (what the driver tells the app) ===\n");
    IDXGIFactory1* factory = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        IDXGIAdapter1* ad = nullptr;
        for (UINT a = 0; factory->EnumAdapters1(a, &ad) == S_OK; a++) {
            DXGI_ADAPTER_DESC1 adesc = {};
            ad->GetDesc1(&adesc);
            wprintf(L"  adapter %u: %s\n", a, adesc.Description);
            IDXGIOutput* out = nullptr;
            for (UINT o = 0; ad->EnumOutputs(o, &out) == S_OK; o++) {
                DXGI_OUTPUT_DESC od = {};
                out->GetDesc(&od);
                DXGI_MODE_DESC cur = {};
                if (od.AttachedToDesktop) {
                    DXGI_MODE_DESC want = {};
                    want.Width = od.DesktopCoordinates.right - od.DesktopCoordinates.left;
                    want.Height = od.DesktopCoordinates.bottom - od.DesktopCoordinates.top;
                    out->FindClosestMatchingMode(&want, &cur, nullptr);
                }
                printf("    output %u: %ls  %ldx%ld  attached=%d\n", o, od.DeviceName,
                       od.DesktopCoordinates.right - od.DesktopCoordinates.left,
                       od.DesktopCoordinates.bottom - od.DesktopCoordinates.top,
                       (int)od.AttachedToDesktop);
                printf("      closest mode to desktop size: %ux%u @ %u/%u Hz\n",
                       cur.Width, cur.Height, cur.RefreshRate.Numerator, cur.RefreshRate.Denominator);
                out->Release();
            }
            ad->Release();
        }
        factory->Release();
    }

    printf("\n=== EnumDisplaySettings (negotiated mode) ===\n");
    DEVMODEW dm = {};
    dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm)) {
        printf("  %lux%lu @ %lu Hz  %lu bpp\n",
               dm.dmPelsWidth, dm.dmPelsHeight, dm.dmDisplayFrequency, dm.dmBitsPerPel);
    }

    printf("\n=== DPI / scaling ===\n");
    HDC dc = GetDC(nullptr);
    int dpi = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(nullptr, dc);
    printf("  system DPI: %d (%.0f%% scaling)  -> a DPI-unaware process sees the desktop as %dx%d\n",
           dpi, 100.0 * dpi / 96.0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
    return 0;
}
