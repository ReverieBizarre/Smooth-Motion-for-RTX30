// ============================================================================
//  nvof_probe - what of the hardware optical flow unit is reachable from here?
//
//  nvofapi64.dll ships with the display driver and is loadable from System32,
//  but the Optical Flow SDK headers are NOT installed on this machine, so the
//  full NV_OF_D3D12_API table (and the struct layouts it returns) cannot be
//  declared without guessing.  This probe answers the part that needs no
//  header: is the library loadable, what does it export, and what is the
//  highest interface version the driver advertises.
//
//  NvOFGetMaxSupportedApiVersion takes a pointer to NV_OF_API_VERSION, which is
//  { uint16_t major; uint16_t minor; } - a 4-byte POD.  Calling it with a
//  uint32_t is safe and does not depend on any layout we would have to guess.
//
//  build: cl nvof_probe.cpp /Fe:nvof_probe.exe
// ============================================================================
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cstring>

typedef int (*FnGetMaxVer)(uint32_t* pVersion);
typedef int (*FnCreateInstance)(uint32_t version, void* pApi);

int main()
{
    printf("nvof_probe - hardware optical flow availability\n\n");

    const char* names[] = {
        "NvOFGetMaxSupportedApiVersion",
        "NvOFAPICreateInstanceD3D12",
        "NvOFAPICreateInstanceD3D11",
        "NvOFAPICreateInstanceCuda",
        "NvOFAPICreateInstanceVk",
    };

    HMODULE m = LoadLibraryA("nvofapi64.dll");
    if (!m)
    {
        printf("LoadLibrary(nvofapi64.dll) failed: %lu\n", GetLastError());
        return 1;
    }
    printf("loadable      : yes\n");

    HWND dummy = nullptr; (void)dummy;
    char path[MAX_PATH] = {};
    if (GetModuleFileNameA(m, path, MAX_PATH)) printf("module path   : %s\n", path);

    printf("\nexports:\n");
    for (const char* n : names)
    {
        FARPROC p = GetProcAddress(m, n);
        printf("  %-32s %s\n", n, p ? "present" : "MISSING");
    }

    printf("\nstruct layouts assumed by this probe: none.\n");

    FnGetMaxVer gmv = (FnGetMaxVer)GetProcAddress(m, "NvOFGetMaxSupportedApiVersion");
    uint32_t ver = 0;
    if (gmv)
    {
        const int st = gmv(&ver);
        printf("\nNvOFGetMaxSupportedApiVersion -> status %d, raw %#010x\n", st, ver);
        printf("  low halfword  %u\n", ver & 0xffffu);
        printf("  high halfword %u\n", (ver >> 16) & 0xffffu);
    }

    // Now the self-validating part: hand that same value back to the D3D12
    // entry point.  If it is accepted (status 0) then the value really is the
    // interface version, and the hardware unit is reachable from user mode on
    // this machine.  Nothing here interprets the returned table - that needs
    // the SDK header's struct definition, which is not installed.
    FnCreateInstance ci = (FnCreateInstance)GetProcAddress(m, "NvOFAPICreateInstanceD3D12");
    if (ci)
    {
        const uint32_t cand[] = { 0x00000000u, ver, 0x00050000u, 0x00080000u };
        for (uint32_t v : cand)
        {
            static uint8_t table[1024];
            memset(table, 0, sizeof(table));
            const int st = ci(v, table);
            printf("CreateInstanceD3D12(raw %#010x) -> status %d %s\n",
                   v, st, st == 0 ? "(ACCEPTED)" : "(rejected)");
            if (st == 0)
            {
                // First qword of the returned table is the version tag; the
                // remaining qwords are function pointers in SDK-defined order.
                const uint64_t* q = (const uint64_t*)table;
                unsigned nz = 0;
                for (int i = 1; i < 40; ++i) if (q[i]) ++nz;
                printf("   table[0] (version tag) = %#llx, non-null entries after it: %u\n",
                       (unsigned long long)q[0], nz);
                break;
            }
        }
    }

    FreeLibrary(m);
    return 0;
}
