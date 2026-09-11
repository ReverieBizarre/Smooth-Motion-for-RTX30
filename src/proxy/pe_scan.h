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

namespace sm86 {

// ---------------------------------------------------------------------------
// 1. Dynamic NvPresent64 Loader (System32 + DriverStore Wildcard Search)
// ---------------------------------------------------------------------------
inline HMODULE LoadNvPresent() {
    HMODULE m = GetModuleHandleA("NvPresent64.dll");
    if (m) return m;
    m = LoadLibraryA("NvPresent64.dll");
    if (m) return m;

    WIN32_FIND_DATAA fd = {};
    HANDLE hFind = FindFirstFileA("C:\\Windows\\System32\\DriverStore\\FileRepository\\nv_dispi.inf_amd64_*", &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                char fullPath[MAX_PATH];
                snprintf(fullPath, sizeof(fullPath),
                         "C:\\Windows\\System32\\DriverStore\\FileRepository\\%s\\NvPresent64.dll", fd.cFileName);
                m = LoadLibraryA(fullPath);
                if (m) {
                    FindClose(hFind);
                    return m;
                }
            }
        } while (FindNextFileA(hFind, &fd));
        FindClose(hFind);
    }
    return nullptr;
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
