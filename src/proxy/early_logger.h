// ============================================================================
//  early_logger.h - Production High-Reliability Early Persistent File Logger
//  and Safe SwapChain Memory Probing Primitives for sm86_smooth (version.dll)
// ============================================================================
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dxgi.h>
#include <cstdint>
#include <cstdio>
#include <cstdarg>

namespace sm86 {

class EarlyLogger {
public:
    static EarlyLogger& Instance() {
        static EarlyLogger s_instance;
        return s_instance;
    }

    void Initialize(HMODULE hModule = nullptr) {
        AcquireSRWLockExclusive(&m_lock);
        if (m_initialized) {
            ReleaseSRWLockExclusive(&m_lock);
            return;
        }

        m_pid = GetCurrentProcessId();

        wchar_t modPath[MAX_PATH] = {};
        if (!hModule) {
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCWSTR)&EarlyLogger::Instance, &hModule);
        }
        if (hModule) {
            GetModuleFileNameW(hModule, modPath, MAX_PATH);
        } else {
            GetModuleFileNameW(nullptr, modPath, MAX_PATH);
        }

        wchar_t* lastSlash = wcsrchr(modPath, L'\\');
        if (lastSlash) {
            *(lastSlash + 1) = L'\0';
        }

        // Location 1: <ModuleDir>\logs\sm86_proxy_<pid>.log
        wchar_t logDir[MAX_PATH] = {};
        swprintf_s(logDir, L"%slogs", modPath);
        CreateDirectoryW(logDir, nullptr);

        swprintf_s(m_filePath, L"%s\\sm86_proxy_%lu.log", logDir, m_pid);
        m_hFile = CreateFileW(m_filePath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

        // Fallback 1: %LOCALAPPDATA%\sm86_smooth\logs\sm86_proxy_<pid>.log
        if (m_hFile == INVALID_HANDLE_VALUE) {
            wchar_t localApp[MAX_PATH] = {};
            if (GetEnvironmentVariableW(L"LOCALAPPDATA", localApp, MAX_PATH) > 0) {
                wchar_t appDir[MAX_PATH] = {};
                swprintf_s(appDir, L"%s\\sm86_smooth", localApp);
                CreateDirectoryW(appDir, nullptr);
                swprintf_s(logDir, L"%s\\sm86_smooth\\logs", localApp);
                CreateDirectoryW(logDir, nullptr);
                swprintf_s(m_filePath, L"%s\\sm86_proxy_%lu.log", logDir, m_pid);
                m_hFile = CreateFileW(m_filePath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                      nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            }
        }

        // Fallback 2: .\logs\sm86_proxy_<pid>.log or .\sm86_proxy_<pid>.log
        if (m_hFile == INVALID_HANDLE_VALUE) {
            CreateDirectoryW(L"logs", nullptr);
            swprintf_s(m_filePath, L"logs\\sm86_proxy_%lu.log", m_pid);
            m_hFile = CreateFileW(m_filePath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        }

        m_initialized = (m_hFile != INVALID_HANDLE_VALUE);
        ReleaseSRWLockExclusive(&m_lock);

        Log(true, "INIT", "[EarlyLogger] Logging initialized. PID=%lu, LogPath=%ls\n", m_pid, m_filePath);
    }

    void Log(bool flushNow, const char* level, const char* fmt, ...) {
        va_list args;
        va_start(args, fmt);
        LogV(flushNow, level, fmt, args);
        va_end(args);
    }

    void LogV(bool flushNow, const char* level, const char* fmt, va_list args) {
        char msgBuf[2048];
        int len = vsnprintf(msgBuf, sizeof(msgBuf), fmt, args);
        if (len <= 0) return;

        SYSTEMTIME st;
        GetLocalTime(&st);
        DWORD tid = GetCurrentThreadId();

        const char* lvl = (level && level[0]) ? level : "INFO";

        char lineBuf[2400];
        int totalLen = snprintf(lineBuf, sizeof(lineBuf),
                                "[%04d-%02d-%02d %02d:%02d:%02d.%03d] [%lu:%lu] [%s] %s",
                                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                                m_pid ? m_pid : GetCurrentProcessId(), tid, lvl, msgBuf);

        if (totalLen > 0 && totalLen < (int)sizeof(lineBuf) - 1) {
            if (lineBuf[totalLen - 1] != '\n') {
                lineBuf[totalLen++] = '\n';
                lineBuf[totalLen] = '\0';
            }
        }

        OutputDebugStringA(lineBuf);

        AcquireSRWLockExclusive(&m_lock);
        if (m_hFile != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(m_hFile, lineBuf, (DWORD)totalLen, &written, nullptr);
            if (flushNow) {
                FlushFileBuffers(m_hFile);
            }
        }
        ReleaseSRWLockExclusive(&m_lock);
    }

    void Flush() {
        AcquireSRWLockExclusive(&m_lock);
        if (m_hFile != INVALID_HANDLE_VALUE) {
            FlushFileBuffers(m_hFile);
        }
        ReleaseSRWLockExclusive(&m_lock);
    }

    void Shutdown() {
        AcquireSRWLockExclusive(&m_lock);
        if (m_hFile != INVALID_HANDLE_VALUE) {
            FlushFileBuffers(m_hFile);
            CloseHandle(m_hFile);
            m_hFile = INVALID_HANDLE_VALUE;
        }
        m_initialized = false;
        ReleaseSRWLockExclusive(&m_lock);
    }

    bool IsInitialized() const { return m_initialized; }
    const wchar_t* GetLogFilePath() const { return m_filePath; }

private:
    EarlyLogger() : m_lock(SRWLOCK_INIT), m_hFile(INVALID_HANDLE_VALUE), m_pid(0), m_initialized(false) {
        m_filePath[0] = L'\0';
    }
    ~EarlyLogger() {
        Shutdown();
    }

    SRWLOCK m_lock;
    HANDLE  m_hFile;
    DWORD   m_pid;
    wchar_t m_filePath[MAX_PATH];
    bool    m_initialized;
};

} // namespace sm86

// ---------------------------------------------------------------------------
// Interface Contracts specified in PROJECT.md
// ---------------------------------------------------------------------------
inline void EarlyLog_Init(HMODULE hModule = nullptr) {
    sm86::EarlyLogger::Instance().Initialize(hModule);
}

inline void EarlyLog_Shutdown() {
    sm86::EarlyLogger::Instance().Shutdown();
}

inline void EarlyLog_Write(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    sm86::EarlyLogger::Instance().LogV(false, "INFO", fmt, args);
    va_end(args);
}

inline void EarlyLog_WriteMilestone(int id, const char* name, const char* fmt, ...) {
    char combinedFmt[512];
    snprintf(combinedFmt, sizeof(combinedFmt), "[MILESTONE %d/14] [%s] %s", id, name, fmt);
    va_list args;
    va_start(args, fmt);
    sm86::EarlyLogger::Instance().LogV(true, "MILESTONE", combinedFmt, args);
    va_end(args);
}

#define PROXY_LOG(fmt, ...) sm86::EarlyLogger::Instance().Log(false, "INFO", fmt, ##__VA_ARGS__)
#define PROXY_LOG_FLUSH(fmt, ...) sm86::EarlyLogger::Instance().Log(true, "INFO", fmt, ##__VA_ARGS__)
#define PROXY_MILESTONE(id, name, fmt, ...) EarlyLog_WriteMilestone(id, name, fmt, ##__VA_ARGS__)

// ---------------------------------------------------------------------------
// Safe SwapChain Memory Probing Primitives (R1)
// ---------------------------------------------------------------------------
static const uintptr_t RVA_PROXY_SWAPCHAIN_VTABLE  = 0x1d3228;
static const uintptr_t RVA_INTERNAL_WRAPPER_VTABLE = 0x1d39c0;

// Pure C-style memory probe function - safe under MSVC /EHsc (no C++ object unwinding in scope)
static inline bool SafeReadPointer(const void* address, void** outPtr) {
    if (!address || !outPtr) return false;

    // 1. Basic alignment check (64-bit pointers must be 8-byte aligned)
    const uintptr_t addr = (uintptr_t)address;
    if ((addr & 0x7) != 0) return false;

    // 2. Canonical user-mode address check on x64
    // Addresses < 0x10000 are null-pointer traps; addresses >= 0x00007FFFFFFFFFFFULL are kernel/invalid
    if (addr < 0x10000 || addr >= 0x00007FFFFFFFFFFFULL) return false;

    // 3. Page Protection Validation via VirtualQuery
    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(address, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
    if (mbi.State != MEM_COMMIT) return false;
    const DWORD prot = (mbi.Protect & 0xFF);
    if (prot != PAGE_READONLY && prot != PAGE_READWRITE && prot != PAGE_WRITECOPY &&
        prot != PAGE_EXECUTE_READ && prot != PAGE_EXECUTE_READWRITE && prot != PAGE_EXECUTE_WRITECOPY) {
        return false;
    }
    if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;

    // 4. Hardware SEH Trap (guards against TOCTOU deallocation race)
    __try {
        *outPtr = *(void* const*)address;
        return true;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION 
                ? EXCEPTION_EXECUTE_HANDLER 
                : EXCEPTION_CONTINUE_SEARCH) {
        *outPtr = nullptr;
        return false;
    }
}

// InspectNvPresentSwapChain: verifies swapchain is genuinely wrapped by NvPresent64.dll
static inline bool InspectNvPresentSwapChain(void* swap, uintptr_t nvpresentBase, void** outWrapper) {
    if (outWrapper) *outWrapper = nullptr;
    if (!swap) return false;

    if (!nvpresentBase) {
        HMODULE hNv = GetModuleHandleA("NvPresent64.dll");
        if (!hNv) return false;
        nvpresentBase = (uintptr_t)hNv;
    }

    const uintptr_t expectedProxyVtbl   = nvpresentBase + RVA_PROXY_SWAPCHAIN_VTABLE;
    const uintptr_t expectedWrapperVtbl = nvpresentBase + RVA_INTERNAL_WRAPPER_VTABLE;

    // Step 1: Safely read swap vtable
    void* swapVtbl = nullptr;
    if (!SafeReadPointer(swap, &swapVtbl) || !swapVtbl) return false;

    // Step 2: Compare vtable against expected proxy vtable RVA
    if ((uintptr_t)swapVtbl != expectedProxyVtbl) return false;

    // Step 3: Safely read internal wrapper pointer at offset +0x18
    void* wrapper = nullptr;
    if (!SafeReadPointer((const uint8_t*)swap + 0x18, &wrapper) || !wrapper) return false;

    // Step 4: Safely read wrapper vtable
    void* wrapperVtbl = nullptr;
    if (!SafeReadPointer(wrapper, &wrapperVtbl) || !wrapperVtbl) return false;

    // Step 5: Verify wrapper vtable matches expected wrapper RVA
    if ((uintptr_t)wrapperVtbl != expectedWrapperVtbl) return false;

    if (outWrapper) {
        *outWrapper = wrapper;
    }
    return true;
}

// ---------------------------------------------------------------------------
// VBlank Pacing Between Presents (R3)
// ---------------------------------------------------------------------------
static inline void PaceVBlankBetweenPresents(IDXGISwapChain* swap) {
    if (!swap) return;

    HWND hwnd = nullptr;
    DXGI_SWAP_CHAIN_DESC scDesc = {};
    if (SUCCEEDED(swap->GetDesc(&scDesc))) {
        hwnd = scDesc.OutputWindow;
    }

    if (hwnd && IsIconic(hwnd)) {
        return; // Minimized window -> skip wait
    }

    IDXGIOutput* output = nullptr;
    HRESULT hr = swap->GetContainingOutput(&output);
    if (SUCCEEDED(hr) && output) {
        output->WaitForVBlank();
        output->Release();
    }
}
