#pragma once
// ============================================================================
//  pe_scan.h - Dynamic PE Parsing & Pattern Signature Scanning Engine
//  Eliminates hardcoded RVAs in sm86_smooth for multi-driver resilience.
// ============================================================================
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>

#pragma comment(lib, "version.lib")
#pragma comment(lib, "advapi32.lib")

namespace sm86 {

// ---------------------------------------------------------------------------
// 1. NvPresent64 loader
//
// DriverStore keeps packages from earlier driver installs, so the first
// nv_dispi.inf_amd64_* folder is often a stale build. The loader picks the
// package whose nvlddmkm.sys version equals the installed display driver and
// falls back to the newest one. A file named sm86_nvpresent_path.txt next to
// the proxy overrides the choice with an explicit path.
//
// The chosen file is loaded through a private copy next to the proxy. The
// display driver loads its own NvPresent64 for some games (the NVIDIA App
// profile path); patching that instance makes D3D12CreateDevice fail with
// DXGI_ERROR_UNSUPPORTED. A copy on a different path is a separate module,
// and the driver's own instance simply declines on RTX 30 as before.
// ---------------------------------------------------------------------------
inline uint64_t FileVersionOf(const char* path) {
    DWORD dummy = 0;
    DWORD sz = GetFileVersionInfoSizeA(path, &dummy);
    if (!sz) return 0;
    uint8_t* buf = (uint8_t*)malloc(sz);
    if (!buf) return 0;
    uint64_t v = 0;
    if (GetFileVersionInfoA(path, 0, sz, buf)) {
        VS_FIXEDFILEINFO* fi = nullptr; UINT len = 0;
        if (VerQueryValueA(buf, "\\", (void**)&fi, &len) && fi) {
            v = ((uint64_t)fi->dwFileVersionMS << 32) | fi->dwFileVersionLS;
        }
    }
    free(buf);
    return v;
}

inline uint64_t ParseVersionString(const char* s) {
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (sscanf_s(s, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return 0;
    return ((uint64_t)((a << 16) | b) << 32) | (uint64_t)((c << 16) | d);
}

inline uint64_t ActiveNvidiaDriverVersion() {
    HKEY cls = nullptr;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                      "SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}",
                      0, KEY_READ, &cls) != ERROR_SUCCESS) return 0;
    uint64_t result = 0;
    for (DWORD i = 0; i < 64 && !result; ++i) {
        char sub[16];
        snprintf(sub, sizeof(sub), "%04lu", i);
        HKEY k = nullptr;
        if (RegOpenKeyExA(cls, sub, 0, KEY_READ, &k) != ERROR_SUCCESS) continue;
        char provider[128] = {}; DWORD plen = sizeof(provider);
        char ver[64] = {};      DWORD vlen = sizeof(ver);
        if (RegQueryValueExA(k, "ProviderName", nullptr, nullptr, (BYTE*)provider, &plen) == ERROR_SUCCESS &&
            strstr(provider, "NVIDIA") &&
            RegQueryValueExA(k, "DriverVersion", nullptr, nullptr, (BYTE*)ver, &vlen) == ERROR_SUCCESS) {
            result = ParseVersionString(ver);
        }
        RegCloseKey(k);
    }
    RegCloseKey(cls);
    return result;
}

inline char g_nvpLoadInfo[512] = {};

inline bool ProxyDirectory(char* dir, size_t len) {
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&ProxyDirectory, &self);
    if (!self || !GetModuleFileNameA(self, dir, (DWORD)len)) return false;
    char* slash = strrchr(dir, '\\');
    if (!slash) return false;
    *(slash + 1) = 0;
    return true;
}

inline HMODULE LoadPrivateCopy(const char* src, char* infoTail, size_t infoTailLen) {
    char dir[MAX_PATH] = {};
    if (ProxyDirectory(dir, sizeof(dir))) {
        strcat_s(dir, sizeof(dir), "nvp_private");
        CreateDirectoryA(dir, nullptr);
        char dst[MAX_PATH]; snprintf(dst, sizeof(dst), "%s\\NvPresent64.dll", dir);
        WIN32_FILE_ATTRIBUTE_DATA a = {}, b = {};
        bool same = GetFileAttributesExA(src, GetFileExInfoStandard, &a) && GetFileAttributesExA(dst, GetFileExInfoStandard, &b) &&
                    a.nFileSizeLow == b.nFileSizeLow && a.nFileSizeHigh == b.nFileSizeHigh &&
                    CompareFileTime(&a.ftLastWriteTime, &b.ftLastWriteTime) == 0;
        if (!same) CopyFileA(src, dst, FALSE);
        HMODULE m = LoadLibraryA(dst);
        if (m) { snprintf(infoTail, infoTailLen, " [private copy %s]", dst); return m; }
        snprintf(infoTail, infoTailLen, " [private copy failed err %lu, loading source path]", GetLastError());
    }
    return LoadLibraryA(src);
}

inline HMODULE LoadNvPresent() {
    HMODULE m = nullptr;
    if (HMODULE pre = GetModuleHandleA("NvPresent64.dll")) {
        char p[MAX_PATH] = {};
        GetModuleFileNameA(pre, p, sizeof(p));
        snprintf(g_nvpLoadInfo, sizeof(g_nvpLoadInfo), "driver instance already present (%s), left untouched; ", p);
    }

    char cfg[MAX_PATH] = {};
    if (ProxyDirectory(cfg, sizeof(cfg))) {
        strcat_s(cfg, sizeof(cfg), "sm86_nvpresent_path.txt");
        FILE* f = nullptr;
        if (fopen_s(&f, cfg, "r") == 0 && f) {
            char line[MAX_PATH] = {};
            if (fgets(line, sizeof(line), f)) {
                size_t n = strlen(line);
                while (n && (line[n-1] == '\n' || line[n-1] == '\r' || line[n-1] == ' ' || line[n-1] == '"')) line[--n] = 0;
                const char* s = line; while (*s == ' ' || *s == '"') ++s;
                if (*s) {
                    char tail[MAX_PATH + 40] = {};
                    m = LoadPrivateCopy(s, tail, sizeof(tail));
                    if (m) {
                        size_t used = strlen(g_nvpLoadInfo);
                        snprintf(g_nvpLoadInfo + used, sizeof(g_nvpLoadInfo) - used, "override from sm86_nvpresent_path.txt: %s%s", s, tail);
                        fclose(f);
                        return m;
                    }
                }
            }
            fclose(f);
        }
    }

    const uint64_t active = ActiveNvidiaDriverVersion();
    char bestPath[MAX_PATH] = {};
    uint64_t bestVer = 0;
    bool bestExact = false;

    WIN32_FIND_DATAA fd = {};
    HANDLE hFind = FindFirstFileA("C:\\Windows\\System32\\DriverStore\\FileRepository\\nv_dispi.inf_amd64_*", &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            char dir[MAX_PATH], nvp[MAX_PATH], kmd[MAX_PATH];
            snprintf(dir, sizeof(dir), "C:\\Windows\\System32\\DriverStore\\FileRepository\\%s", fd.cFileName);
            snprintf(nvp, sizeof(nvp), "%s\\NvPresent64.dll", dir);
            snprintf(kmd, sizeof(kmd), "%s\\nvlddmkm.sys", dir);
            if (GetFileAttributesA(nvp) == INVALID_FILE_ATTRIBUTES) continue;
            uint64_t ver = FileVersionOf(kmd);
            if (!ver) ver = FileVersionOf(nvp);
            bool exact = (active != 0 && ver == active);
            if (bestPath[0] == 0 || (exact && !bestExact) || (exact == bestExact && ver > bestVer)) {
                strncpy_s(bestPath, nvp, _TRUNCATE);
                bestVer = ver;
                bestExact = exact;
            }
        } while (FindNextFileA(hFind, &fd));
        FindClose(hFind);
    }

    if (bestPath[0]) {
        char tail[MAX_PATH + 40] = {};
        m = LoadPrivateCopy(bestPath, tail, sizeof(tail));
        if (m) {
            size_t used = strlen(g_nvpLoadInfo);
            snprintf(g_nvpLoadInfo + used, sizeof(g_nvpLoadInfo) - used,
                     "%s%s (package driver %u.%u.%u.%u, active driver %u.%u.%u.%u, %s)", bestPath, tail,
                     (unsigned)(bestVer >> 48) & 0xFFFF, (unsigned)(bestVer >> 32) & 0xFFFF,
                     (unsigned)(bestVer >> 16) & 0xFFFF, (unsigned)bestVer & 0xFFFF,
                     (unsigned)(active >> 48) & 0xFFFF, (unsigned)(active >> 32) & 0xFFFF,
                     (unsigned)(active >> 16) & 0xFFFF, (unsigned)active & 0xFFFF,
                     bestExact ? "matches the active driver" : "no exact match, newest package used");
            return m;
        }
    }

    m = LoadLibraryA("NvPresent64.dll");
    if (m) snprintf(g_nvpLoadInfo, sizeof(g_nvpLoadInfo), "default search order, no DriverStore candidate");
    return m;
}

// ---------------------------------------------------------------------------
// 2. Get PE Section Information
// ---------------------------------------------------------------------------
inline bool GetSectionInfo(HMODULE module, const char* sectionName, const uint8_t** outBase, size_t* outSize) {
    if (!module || !sectionName) return false;
    PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)module;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;

    PIMAGE_NT_HEADERS nt = (PIMAGE_NT_HEADERS)((uint8_t*)module + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

    PIMAGE_SECTION_HEADER sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        char name[9] = {};
        memcpy(name, sec[i].Name, 8);
        if (_stricmp(name, sectionName) == 0) {
            if (outBase) *outBase = (const uint8_t*)module + sec[i].VirtualAddress;
            if (outSize) *outSize = (size_t)sec[i].Misc.VirtualSize;
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// 3. Pattern Matching with Wildcard Mask ('x' = match, '?' = wildcard)
// ---------------------------------------------------------------------------
inline const uint8_t* FindPattern(const uint8_t* base, size_t size, const char* pattern, const char* mask) {
    if (!base || size == 0 || !pattern || !mask) return nullptr;
    size_t patternLen = strlen(mask);
    if (patternLen == 0 || size < patternLen) return nullptr;

    const size_t end = size - patternLen;
    for (size_t i = 0; i <= end; ++i) {
        bool match = true;
        for (size_t j = 0; j < patternLen; ++j) {
            if (mask[j] == 'x' && base[i + j] != (uint8_t)pattern[j]) {
                match = false;
                break;
            }
        }
        if (match) {
            return base + i;
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// 4. Dynamic IAT Entry Resolver (Walks IMAGE_DIRECTORY_ENTRY_IMPORT)
// ---------------------------------------------------------------------------
inline void** FindIATEntry(HMODULE module, const char* targetDll, const char* targetFunc) {
    if (!module || !targetDll || !targetFunc) return nullptr;

    PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)module;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;

    PIMAGE_NT_HEADERS nt = (PIMAGE_NT_HEADERS)((uint8_t*)module + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;

    IMAGE_DATA_DIRECTORY importDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (importDir.VirtualAddress == 0 || importDir.Size == 0) return nullptr;

    PIMAGE_IMPORT_DESCRIPTOR desc = (PIMAGE_IMPORT_DESCRIPTOR)((uint8_t*)module + importDir.VirtualAddress);
    for (; desc->Name != 0; ++desc) {
        const char* modName = (const char*)((uint8_t*)module + desc->Name);
        if (_stricmp(modName, targetDll) == 0) {
            DWORD oftRva = desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk;
            PIMAGE_THUNK_DATA thunkOFT = (PIMAGE_THUNK_DATA)((uint8_t*)module + oftRva);
            PIMAGE_THUNK_DATA thunkFT  = (PIMAGE_THUNK_DATA)((uint8_t*)module + desc->FirstThunk);

            for (size_t i = 0; thunkOFT[i].u1.AddressOfData != 0; ++i) {
                if (!(thunkOFT[i].u1.Ordinal & IMAGE_ORDINAL_FLAG64)) {
                    PIMAGE_IMPORT_BY_NAME ibn = (PIMAGE_IMPORT_BY_NAME)((uint8_t*)module + thunkOFT[i].u1.AddressOfData);
                    if (strcmp((const char*)ibn->Name, targetFunc) == 0) {
                        return (void**)&thunkFT[i].u1.Function;
                    }
                }
            }
            break;
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// 5. Dynamic Dual-Gate Resolver (cmp [rcx+0x14], 3 ... setge sil)
// ---------------------------------------------------------------------------
inline bool LocateGateAddresses(HMODULE module, uint8_t** outCmpImm, uint8_t** outSetgePtr, uint32_t* outSetgeLen) {
    const uint8_t* textBase = nullptr;
    size_t textSize = 0;
    if (!GetSectionInfo(module, ".text", &textBase, &textSize)) {
        textBase = (const uint8_t*)module;
        textSize = 0x200000;
    }

    // Pattern for: cmp dword ptr [rcx + 0x14], 3 (or 2 if already patched)
    // Opcode: 83 79 14 ??
    const char cmpPat[]  = "\x83\x79\x14\x00";
    const char cmpMask[] = "xxx?";

    const uint8_t* cur = textBase;
    size_t remSize = textSize;

    while (remSize >= 4) {
        const uint8_t* hit = FindPattern(cur, remSize, cmpPat, cmpMask);
        if (!hit) break;

        // Verify that immediate is either 3 (unpatched) or 2 (already patched)
        if (hit[3] == 0x03 || hit[3] == 0x02) {
            const size_t scanWindow = 40;
            const size_t avail = (size_t)(textBase + textSize - hit);
            const size_t lookAhead = (avail < scanWindow) ? avail : scanWindow;

            for (size_t k = 4; k + 3 <= lookAhead; ++k) {
                // Check for 40 0F 9D C6 (setge sil) or 40 B6 01 90 (mov sil, 1; nop)
                if (k + 4 <= lookAhead && hit[k] == 0x40) {
                    if ((hit[k+1] == 0x0F && hit[k+2] == 0x9D && hit[k+3] == 0xC6) ||
                        (hit[k+1] == 0xB6 && hit[k+2] == 0x01 && hit[k+3] == 0x90)) {
                        if (outCmpImm)   *outCmpImm   = const_cast<uint8_t*>(hit + 3);
                        if (outSetgePtr) *outSetgePtr = const_cast<uint8_t*>(hit + k);
                        if (outSetgeLen) *outSetgeLen = 4;
                        return true;
                    }
                }
                // Check for 0F 9D C6 (setge sil) or B6 01 90 (mov sil, 1; nop)
                if ((hit[k] == 0x0F && hit[k+1] == 0x9D && hit[k+2] == 0xC6) ||
                    (hit[k] == 0xB6 && hit[k+1] == 0x01 && hit[k+2] == 0x90)) {
                    if (outCmpImm)   *outCmpImm   = const_cast<uint8_t*>(hit + 3);
                    if (outSetgePtr) *outSetgePtr = const_cast<uint8_t*>(hit + k);
                    if (outSetgeLen) *outSetgeLen = 3;
                    return true;
                }
            }
        }

        remSize -= (hit - cur + 1);
        cur = hit + 1;
    }

    return false;
}

// ---------------------------------------------------------------------------
// 6. Dynamic Global Config Struct Resolver from NVP_Init_D3D
// ---------------------------------------------------------------------------
inline uint8_t* ResolveConfigStructFromInit(HMODULE module) {
    if (!module) return nullptr;

    FARPROC pInit = GetProcAddress(module, "NVP_Init_D3D");
    if (!pInit) return nullptr;

    const uint8_t* code = (const uint8_t*)pInit;
    // Scan the first 48 bytes of NVP_Init_D3D for:
    // lea rcx, [rip + disp32] -> 48 8D 0D [disp32]
    for (size_t i = 0; i < 48; ++i) {
        if (code[i] == 0x48 && code[i+1] == 0x8D && code[i+2] == 0x0D) {
            int32_t disp = *(const int32_t*)(code + i + 3);
            const uint8_t* target = code + i + 7 + disp;
            return const_cast<uint8_t*>(target);
        }
    }

    return nullptr;
}

} // namespace sm86
