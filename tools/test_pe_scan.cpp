#include <windows.h>
#include <cstdio>
#include "../src/proxy/pe_scan.h"

int main() {
    printf("================================================================\n");
    printf("  Testing Dynamic PE Scan & Pattern Scanner on NvPresent64.dll  \n");
    printf("================================================================\n");

    const char* path = "C:\\Windows\\System32\\DriverStore\\FileRepository\\nv_dispi.inf_amd64_a3944b54ff18b284\\NvPresent64.dll";
    HMODULE nv = LoadLibraryExA(path, nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!nv) {
        nv = LoadLibraryA("NvPresent64.dll");
    }
    if (!nv) {
        printf("[!] Failed to load NvPresent64.dll (GetLastError = %lu)\n", GetLastError());
        return 1;
    }
    printf("[+] NvPresent64.dll loaded @ %p\n", nv);

    // 1. Test Gate Resolution
    uint8_t* cmpImm = nullptr;
    uint8_t* setgePtr = nullptr;
    uint32_t setgeLen = 0;
    bool gateOk = sm86::LocateGateAddresses(nv, &cmpImm, &setgePtr, &setgeLen);
    if (gateOk) {
        uint32_t rvaCmp = (uint32_t)(cmpImm - (uint8_t*)nv);
        uint32_t rvaSetge = (uint32_t)(setgePtr - (uint8_t*)nv);
        printf("[+] Gate Scan: SUCCESS\n");
        printf("    cmp [rcx+0x14], 3 imm @ %p (RVA 0x%x, expected 0xc41f) -> %s\n",
               cmpImm, rvaCmp, (rvaCmp == 0xc41f) ? "MATCH" : "SHIFTED_BUT_FOUND");
        printf("    setge sil @ %p (RVA 0x%x, len %u, expected 0xc437) -> %s\n",
               setgePtr, rvaSetge, setgeLen, (rvaSetge == 0xc437) ? "MATCH" : "SHIFTED_BUT_FOUND");
    } else {
        printf("[!] Gate Scan: FAILED\n");
    }

    // 2. Test IAT Resolution
    void** iatLoad = sm86::FindIATEntry(nv, "nvcuda.dll", "cuModuleLoadData");
    void** iatLaunch = sm86::FindIATEntry(nv, "nvcuda.dll", "cuLaunchKernel");
    void** iatGraph = sm86::FindIATEntry(nv, "nvcuda.dll", "cuGraphLaunch");

    if (iatLoad) {
        uint32_t rvaLoad = (uint32_t)((uint8_t*)iatLoad - (uint8_t*)nv);
        printf("[+] IAT cuModuleLoadData @ %p (RVA 0x%x, expected 0x1d2820) -> %s\n",
               iatLoad, rvaLoad, (rvaLoad == 0x1d2820) ? "MATCH" : "SHIFTED_BUT_FOUND");
    } else {
        printf("[!] IAT cuModuleLoadData: NOT FOUND\n");
    }

    if (iatLaunch) {
        uint32_t rvaLaunch = (uint32_t)((uint8_t*)iatLaunch - (uint8_t*)nv);
        printf("[+] IAT cuLaunchKernel   @ %p (RVA 0x%x, expected 0x1d27f8) -> %s\n",
               iatLaunch, rvaLaunch, (rvaLaunch == 0x1d27f8) ? "MATCH" : "SHIFTED_BUT_FOUND");
    } else {
        printf("[!] IAT cuLaunchKernel: NOT FOUND\n");
    }

    if (iatGraph) {
        uint32_t rvaGraph = (uint32_t)((uint8_t*)iatGraph - (uint8_t*)nv);
        printf("[+] IAT cuGraphLaunch    @ %p (RVA 0x%x, expected 0x1d2780) -> %s\n",
               iatGraph, rvaGraph, (rvaGraph == 0x1d2780) ? "MATCH" : "SHIFTED_BUT_FOUND");
    } else {
        printf("[!] IAT cuGraphLaunch: NOT FOUND\n");
    }

    // 3. Test Config Struct Resolution
    uint8_t* config = sm86::ResolveConfigStructFromInit(nv);
    if (config) {
        uint32_t rvaConfig = (uint32_t)(config - (uint8_t*)nv);
        printf("[+] Config Struct @ %p (RVA 0x%x, expected 0x7d7810) -> %s\n",
               config, rvaConfig, (rvaConfig == 0x7d7810) ? "MATCH" : "SHIFTED_BUT_FOUND");
    } else {
        printf("[!] Config Struct: NOT FOUND\n");
    }

    bool allOk = gateOk && iatLoad && iatLaunch && iatGraph && config;
    printf("================================================================\n");
    printf("Result: %s\n", allOk ? "ALL DYNAMIC CHECKS PASSED!" : "SOME CHECKS FAILED!");
    printf("================================================================\n");

    FreeLibrary(nv);
    return allOk ? 0 : 1;
}
