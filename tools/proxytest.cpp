// verifies that the proxy DLL's export forwarders actually resolve to the real
// System32\version.dll (the linker rewrites the path, so this cannot be assumed)
#include <windows.h>
#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv)
{
    wchar_t dll[MAX_PATH * 2];
    GetFullPathNameW((argc > 1) ? L"build\\version.dll" : L"build\\version.dll",
                     MAX_PATH * 2, dll, nullptr);

    HMODULE m = LoadLibraryW(dll);
    if (!m) { printf("LoadLibraryW failed, err %lu\n", GetLastError()); return 1; }
    printf("loaded: %ls\n", dll);

    struct { const char* name; } names[] = {
        { "GetFileVersionInfoSizeW" },
        { "GetFileVersionInfoW"     },
        { "VerQueryValueW"          },
        { "VerLanguageNameW"        },
    };

    int ok = 0;
    for (auto& n : names)
    {
        void* p = (void*)GetProcAddress(m, n.name);
        printf("  %-28s %s\n", n.name, p ? "present" : "MISSING");
        if (p) ++ok;
    }

    auto sizeFn = (DWORD(WINAPI*)(LPCWSTR, LPDWORD))GetProcAddress(m, "GetFileVersionInfoSizeW");
    if (!sizeFn) return 2;

    DWORD h = 0;
    SetLastError(0);
    DWORD sz = sizeFn(L"C:\\Windows\\System32\\notepad.exe", &h);
    printf("\nGetFileVersionInfoSizeW(notepad.exe) -> %lu  (GetLastError %lu)\n",
           sz, GetLastError());

    auto queryFn = (BOOL(WINAPI*)(LPVOID, LPCWSTR, LPVOID*, PUINT))GetProcAddress(m, "VerQueryValueW");
    if (sizeFn && queryFn && sz)
    {
        void* buf = malloc(sz);
        auto infoFn = (BOOL(WINAPI*)(LPCWSTR, DWORD, DWORD, LPVOID))GetProcAddress(m, "GetFileVersionInfoW");
        if (infoFn && infoFn(L"C:\\Windows\\System32\\notepad.exe", 0, sz, buf))
        {
            VS_FIXEDFILEINFO* fi = nullptr; UINT len = 0;
            if (queryFn(buf, L"\\", (void**)&fi, &len) && fi)
                printf("file version: %u.%u.%u.%u\n",
                       HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS),
                       HIWORD(fi->dwFileVersionLS), LOWORD(fi->dwFileVersionLS));
            else
                printf("VerQueryValueW(root) failed\n");
        }
        free(buf);
    }

    FreeLibrary(m);
    printf("\nforwarders %s\n", (ok == 4) ? "OK" : "INCOMPLETE");
    return 0;
}
