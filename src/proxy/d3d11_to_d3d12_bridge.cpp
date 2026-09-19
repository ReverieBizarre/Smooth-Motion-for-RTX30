// ============================================================================
//  d3d11_to_d3d12_bridge.cpp - Shadow D3D12 SwapChain Bridge for Road 1
//  Enables NvPresent64 (Smooth Motion / DLSS 3 Frame Gen) in D3D11 apps (e.g. MPC-HC)
// ============================================================================
#include "d3d11_to_d3d12_bridge.h"
#include <cstdio>
#include <cstdarg>
#include <cstdlib>

#include "early_logger.h"

static void LogBridge(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    sm86::EarlyLogger::Instance().LogV(false, "INFO", fmt, args);
    va_end(args);
}

static WNDPROC g_subclassedParentOrigProc = nullptr;
static HWND    g_subclassedParentHwnd     = nullptr;

static LRESULT CALLBACK SubclassedParentWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1; // Direct3D 12 on child window handles all display; suppress background erase to prevent black flash!
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        return 0; // Validate update region without GDI or D3D11 blitting over the D3D12 child window!
    }
    }
    if (g_subclassedParentOrigProc) {
        return CallWindowProcA(g_subclassedParentOrigProc, hwnd, msg, wp, lp);
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

namespace sm86 {

// ---------------------------------------------------------------------------
//  Present-level diagnostics
//
//  A black flash that a screen recorder cannot capture lives below DWM
//  composition: the desktop image is fine, but the plane that is scanned out
//  for one vblank is not. The only visible-from-user-space evidence for that
//  is the DXGI present cadence, so we log, per app Present:
//    * the QPC interval (how the app's own pacing looks),
//    * how long the D3D11->shared and shared->D3D12 copies blocked the thread,
//    * DXGI_FRAME_STATISTICS PresentCount / PresentRefreshCount, whose deltas
//      tell us how many vblanks actually elapsed per app frame and whether the
//      driver presented extra (frame-generated) frames in between.
// ---------------------------------------------------------------------------
static bool DiagEnabled() {
    static int cached = -1;
    if (cached < 0) {
        char buf[16] = {};
        DWORD n = GetEnvironmentVariableA("SM86_DIAG", buf, sizeof(buf));
        cached = (n > 0 && buf[0] == '1') ? 1 : 0;
    }
    return cached == 1;
}

// Shadow swapchain frame-latency contract. Defaults to ON (the fix); set
// SM86_SHADOW_WAITABLE=0 to fall back to the previous behaviour for an A/B
// test without rebuilding anything.
static bool ShadowWaitableEnabled() {
    static int cached = -1;
    if (cached < 0) {
        char buf[16] = {};
        DWORD n = GetEnvironmentVariableA("SM86_SHADOW_WAITABLE", buf, sizeof(buf));
        cached = (n > 0 && buf[0] == '0') ? 0 : 1;
    }
    return cached == 1;
}

// Maximum frames in flight on the shadow swapchain. 1 is the value that
// actually removes the tearing we measured with a slow-motion camera: before
// writing a back buffer we wait until the previous flip has been retired, so
// the display can never be scanning a buffer we are about to overwrite.
// Override with SM86_SHADOW_MAX_LATENCY=<1..3> for an A/B test.
static UINT ShadowMaxLatency() {
    static int cached = -1;
    if (cached < 0) {
        char buf[16] = {};
        DWORD n = GetEnvironmentVariableA("SM86_SHADOW_MAX_LATENCY", buf, sizeof(buf));
        int v = (n > 0) ? atoi(buf) : 1;
        if (v < 1) v = 1;
        if (v > 3) v = 3;
        cached = v;
    }
    return (UINT)cached;
}

// Shadow swapchain back buffer count. Frame generation needs to hold the
// previous real frame, the current real frame and a slot for the synthesised
// one, on top of whatever the app itself keeps in flight; too few buffers force
// the driver to recycle one that is still in use, which produces the torn
// half-screen flips we measured. Override with SM86_SHADOW_BUFFERS=<4..8>.
static UINT ShadowBufferCount() {
    static int cached = -1;
    if (cached < 0) {
        char buf[16] = {};
        DWORD n = GetEnvironmentVariableA("SM86_SHADOW_BUFFERS", buf, sizeof(buf));
        int v = (n > 0) ? atoi(buf) : 6;
        if (v < 4) v = 4;
        if (v > 8) v = 8;
        cached = v;
    }
    return (UINT)cached;
}

static void DiagCsv(const char* fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    static wchar_t s_csvPath[MAX_PATH] = {};
    static bool    s_resolved = false;
    static SRWLOCK s_diagLock = SRWLOCK_INIT;

    AcquireSRWLockExclusive(&s_diagLock);
    if (!s_resolved) {
        s_resolved = true;
        HMODULE hMod = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)&DiagCsv, &hMod);
        wchar_t modPath[MAX_PATH] = {};
        if (hMod) {
            GetModuleFileNameW(hMod, modPath, MAX_PATH);
        } else {
            GetModuleFileNameW(nullptr, modPath, MAX_PATH);
        }
        wchar_t* lastSlash = wcsrchr(modPath, L'\\');
        if (lastSlash) {
            *(lastSlash + 1) = L'\0';
        }

        DWORD pid = GetCurrentProcessId();
        wchar_t logDir[MAX_PATH] = {};
        swprintf_s(logDir, L"%slogs", modPath);
        CreateDirectoryW(logDir, nullptr);
        swprintf_s(s_csvPath, L"%s\\sm86_present_%lu.csv", logDir, pid);

        // Fallback to %LOCALAPPDATA% if module directory is write-restricted
        FILE* testF = nullptr;
        if (_wfopen_s(&testF, s_csvPath, L"a") == 0 && testF) {
            fclose(testF);
        } else {
            wchar_t localApp[MAX_PATH] = {};
            if (GetEnvironmentVariableW(L"LOCALAPPDATA", localApp, MAX_PATH) > 0) {
                wchar_t appDir[MAX_PATH] = {};
                swprintf_s(appDir, L"%s\\sm86_smooth", localApp);
                CreateDirectoryW(appDir, nullptr);
                swprintf_s(logDir, L"%s\\sm86_smooth\\logs", localApp);
                CreateDirectoryW(logDir, nullptr);
                swprintf_s(s_csvPath, L"%s\\sm86_present_%lu.csv", logDir, pid);
            }
        }
        sm86::EarlyLogger::Instance().Log(true, "DIAG", "[Bridge] Diagnostic CSV initialized: %ls\n", s_csvPath);
    }

    FILE* f = nullptr;
    if (_wfopen_s(&f, s_csvPath, L"a") == 0 && f) {
        fputs(buf, f);
        fclose(f);
    }
    ReleaseSRWLockExclusive(&s_diagLock);
}

void D3D11ToD3D12Bridge::DumpDiagSummary() {
    if (!m_diag) return;
    DiagCsv("# vblank gap histogram (gap = PresentRefreshCount delta per app Present)\n");
    for (int i = 0; i < 16; i++) {
        if (m_vblankHist[i]) {
            DiagCsv("#   gap=%2d vblanks : %llu\n", i, (unsigned long long)m_vblankHist[i]);
        }
    }
    DiagCsv("# app presents=%llu, stalled(bridge block > frame period)=%llu, tearing-flagged=%llu\n",
            (unsigned long long)m_presIndex, (unsigned long long)m_stallCount,
            (unsigned long long)m_tearingFlagCount);
    for (int i = 0; i < 16; i++) m_vblankHist[i] = 0;
    m_prevRefresh = 0;
    m_prevPresentCnt = 0;
}

void D3D11ToD3D12Bridge::DiagPresentRow(UINT sync, UINT flags, UINT presentSync, UINT idx,
                                        double copy11Us, double copy12Us, HRESULT hr) {
    if (!m_diag) return;

    if (!m_diagInit) {
        m_diagInit = true;
        QueryPerformanceFrequency(&m_qpcFreq);
        m_qpcPrev.QuadPart = 0;
        DiagCsv("# frame,qpc_ms,dt_ms,buf_idx,sync_req,flags_req,sync_used,copy11_us,copy12_us,"
                "presentCount,refreshCount,syncRefreshCount,hr\n");
    }

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double dtMs = 0.0;
    if (m_qpcPrev.QuadPart && m_qpcFreq.QuadPart) {
        dtMs = (double)(now.QuadPart - m_qpcPrev.QuadPart) * 1000.0 / (double)m_qpcFreq.QuadPart;
    }
    m_qpcPrev = now;
    double ms = (double)now.QuadPart * 1000.0 / (double)m_qpcFreq.QuadPart;

    DXGI_FRAME_STATISTICS st = {};
    HRESULT shr = m_swap12 ? m_swap12->GetFrameStatistics(&st) : E_FAIL;

    // PresentRefreshCount is in vblanks; its delta across two app Presents is
    // how many vblanks were consumed by (generated + real) frames.
    if (SUCCEEDED(shr) && m_prevRefresh) {
        UINT gap = st.PresentRefreshCount - m_prevRefresh;
        if (gap < 16) m_vblankHist[gap]++;
    }
    if (SUCCEEDED(shr)) m_prevRefresh = st.PresentRefreshCount;

    if (flags & DXGI_PRESENT_ALLOW_TEARING) m_tearingFlagCount++;
    // Our own blocking (two full GPU->CPU syncs) exceeding one app frame period
    // means the bridge, not the driver, is setting the pace.
    if (dtMs > 0.0 && (copy11Us + copy12Us) > dtMs * 1000.0) m_stallCount++;

    DiagCsv("%llu,%.3f,%.3f,%u,%u,0x%08X,%u,%.1f,%.1f,%u,%u,%u,0x%08X\n",
            (unsigned long long)m_presIndex, ms, dtMs, idx, sync, flags, presentSync,
            copy11Us, copy12Us,
            SUCCEEDED(shr) ? st.PresentCount : 0u,
            SUCCEEDED(shr) ? st.PresentRefreshCount : 0u,
            SUCCEEDED(shr) ? st.SyncRefreshCount : 0u,
            (uint32_t)hr);
}

D3D11ToD3D12Bridge::D3D11ToD3D12Bridge() = default;

D3D11ToD3D12Bridge::~D3D11ToD3D12Bridge() {
    Shutdown();
}

void D3D11ToD3D12Bridge::Shutdown() {
    DumpDiagSummary();

    // 1. Flush D3D11 device context
    if (m_ctx11) {
        m_ctx11->ClearState();
        m_ctx11->Flush();
    }

    // 2. Signal D3D12 command queue fence and wait
    if (m_cq12 && m_fence12 && m_fenceEvent12) {
        m_cq12->Signal(m_fence12, ++m_fenceVal12);
        m_fence12->SetEventOnCompletion(m_fenceVal12, m_fenceEvent12);
        WaitForSingleObject(m_fenceEvent12, 2000);
    }

    // 3. Wait on frame latency waitable if valid
    if (m_frameLatencyWaitable) {
        WaitForSingleObject(m_frameLatencyWaitable, 1000);
        m_frameLatencyWaitable = nullptr; // Owned by swapchain; do not CloseHandle
    }

    if (m_hwnd && g_subclassedParentHwnd == m_hwnd && g_subclassedParentOrigProc && IsWindow(m_hwnd)) {
        SetWindowLongPtrA(m_hwnd, GWLP_WNDPROC, (LONG_PTR)g_subclassedParentOrigProc);
        g_subclassedParentOrigProc = nullptr;
        g_subclassedParentHwnd = nullptr;
    }
    m_origParentWndProc = nullptr;

    if (m_sharedHandle) {
        CloseHandle(m_sharedHandle);
        m_sharedHandle = nullptr;
    }
    if (m_fenceEvent12) {
        CloseHandle(m_fenceEvent12);
        m_fenceEvent12 = nullptr;
    }
    if (m_fence12)        { m_fence12->Release(); m_fence12 = nullptr; }
    for (int i = 0; i < 8; i++) {
        if (m_backbuffers12[i]) { m_backbuffers12[i]->Release(); m_backbuffers12[i] = nullptr; }
    }
    if (m_query11)        { m_query11->Release(); m_query11 = nullptr; }
    // The waitable handle is owned by the swapchain - do NOT CloseHandle it.
    m_frameLatencyWaitable = nullptr;
    if (m_swap2_12)       { m_swap2_12->Release(); m_swap2_12 = nullptr; }
    if (m_sharedTex12)    { m_sharedTex12->Release(); m_sharedTex12 = nullptr; }
    if (m_swap3_12)       { m_swap3_12->Release(); m_swap3_12 = nullptr; }
    if (m_swap12)         { m_swap12->Release(); m_swap12 = nullptr; }
    if (m_cl12)           { m_cl12->Release(); m_cl12 = nullptr; }
    if (m_alloc12)        { m_alloc12->Release(); m_alloc12 = nullptr; }
    if (m_cq12)           { m_cq12->Release(); m_cq12 = nullptr; }
    if (m_dev12)          { m_dev12->Release(); m_dev12 = nullptr; }
    if (m_sharedTex11)    { m_sharedTex11->Release(); m_sharedTex11 = nullptr; }
    if (m_ctx11)          { m_ctx11->Release(); m_ctx11 = nullptr; }
    if (m_childHwnd && IsWindow(m_childHwnd)) {
        ShowWindow(m_childHwnd, SW_HIDE);
        DestroyWindow(m_childHwnd);
        m_childHwnd = nullptr;
    }
    m_dev11 = nullptr;
    m_wrapper = nullptr;
    m_hwnd = nullptr;
    m_childHwnd = nullptr;
    m_lastX = -1;
    m_lastY = -1;
    m_lastW = -1;
    m_lastH = -1;
    m_width = 0;
    m_height = 0;
    m_active = false;
}

bool D3D11ToD3D12Bridge::CreateSharedTexture(uint32_t width, uint32_t height, DXGI_FORMAT format) {
    if (!m_dev11 || !m_dev12) return false;

    DXGI_FORMAT texFormat = format;
    if (texFormat == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) texFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    else if (texFormat == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) texFormat = DXGI_FORMAT_B8G8R8A8_UNORM;

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = width;
    td.Height = height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = texFormat;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

    HRESULT hr = m_dev11->CreateTexture2D(&td, nullptr, &m_sharedTex11);
    if (FAILED(hr)) {
        LogBridge("[D3D11ToD3D12Bridge] CreateTexture2D with NTHANDLE failed: 0x%08X (fmt=%d), trying legacy SHARED\n",
                  (uint32_t)hr, (int)texFormat);
        td.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
        hr = m_dev11->CreateTexture2D(&td, nullptr, &m_sharedTex11);
        if (FAILED(hr)) {
            LogBridge("[D3D11ToD3D12Bridge] Failed to create shared D3D11 texture: 0x%08X (fmt=%d, %ux%u)\n",
                      (uint32_t)hr, (int)td.Format, td.Width, td.Height);
            return false;
        }
        IDXGIResource* res = nullptr;
        m_sharedTex11->QueryInterface(IID_PPV_ARGS(&res));
        res->GetSharedHandle(&m_sharedHandle);
        res->Release();
    } else {
        IDXGIResource1* res1 = nullptr;
        m_sharedTex11->QueryInterface(IID_PPV_ARGS(&res1));
        hr = res1->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &m_sharedHandle);
        res1->Release();
        LogBridge("[D3D11ToD3D12Bridge] CreateSharedHandle (NT handle) -> hr=0x%08X, handle=%p\n",
                  (uint32_t)hr, m_sharedHandle);
    }

    hr = m_dev12->OpenSharedHandle(m_sharedHandle, IID_PPV_ARGS(&m_sharedTex12));
    if (FAILED(hr)) {
        LogBridge("[D3D11ToD3D12Bridge] Failed to open shared handle in D3D12: 0x%08X\n", (uint32_t)hr);
        return false;
    }

    LogBridge("[D3D11ToD3D12Bridge] Shared texture successfully created & opened in D3D12 (tex11=%p, tex12=%p, fmt=%d)\n",
              m_sharedTex11, m_sharedTex12, (int)texFormat);
    return true;
}

static LRESULT CALLBACK BridgeWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1; // Direct3D 12 handles all drawing; never let GDI erase background!
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        return 0; // Validate update region without GDI painting
    }
    case WM_NCHITTEST:
        return HTTRANSPARENT; // Let all mouse clicks and gestures pass straight through to video window!
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static void RegisterBridgeWindowClass() {
    static bool s_registered = false;
    if (s_registered) return;
    WNDCLASSA wc = {};
    wc.style = 0;
    wc.lpfnWndProc = BridgeWndProc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "sm86_d3d12_overlay";
    wc.hbrBackground = nullptr; // NULL background brush prevents GDI flash!
    RegisterClassA(&wc);
    s_registered = true;
}

bool D3D11ToD3D12Bridge::CreateD3D12Resources(HWND hwnd, uint32_t width, uint32_t height, DXGI_FORMAT format) {
    HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_dev12));
    if (FAILED(hr)) {
        LogBridge("[D3D11ToD3D12Bridge] D3D12CreateDevice failed: 0x%08X\n", (uint32_t)hr);
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr = m_dev12->CreateCommandQueue(&qd, IID_PPV_ARGS(&m_cq12));
    if (FAILED(hr)) {
        LogBridge("[D3D11ToD3D12Bridge] CreateCommandQueue failed: 0x%08X\n", (uint32_t)hr);
        return false;
    }

    hr = m_dev12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_alloc12));
    if (FAILED(hr)) return false;

    hr = m_dev12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_alloc12, nullptr, IID_PPV_ARGS(&m_cl12));
    if (FAILED(hr)) return false;
    m_cl12->Close();

    auto logObj = [](const char* name, void* obj) {
        if (!obj) return;
        void* vt = *(void**)obj;
        HMODULE mod = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)vt, &mod);
        char modName[MAX_PATH] = {};
        GetModuleFileNameA(mod, modName, sizeof(modName));
        LogBridge("[BridgeDebug] %s = %p, vtable = %p (%s)\n", name, obj, vt, modName);
    };

    logObj("m_dev12", m_dev12);
    logObj("m_cq12", m_cq12);

    // Create D3D12 SwapChain on hwnd
    IDXGIFactory4* f = nullptr;
    CreateDXGIFactory1(IID_PPV_ARGS(&f));
    if (!f) {
        LogBridge("[D3D11ToD3D12Bridge] CreateDXGIFactory1 failed\n");
        return false;
    }
    logObj("factory", f);

    DXGI_FORMAT swapFormat = format;
    if (swapFormat == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) swapFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    else if (swapFormat == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) swapFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
    else if (swapFormat != DXGI_FORMAT_R10G10B10A2_UNORM &&
             swapFormat != DXGI_FORMAT_B8G8R8A8_UNORM &&
             swapFormat != DXGI_FORMAT_R16G16B16A16_FLOAT &&
             swapFormat != DXGI_FORMAT_R8G8B8A8_UNORM) {
        swapFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    }

    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width = width;
    sd.Height = height;
    sd.Format = swapFormat;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    m_swapBufferCount = ShadowBufferCount();
    sd.BufferCount = m_swapBufferCount;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    // Frame-latency waitable object: NVIDIA frame generation must know exactly
    // when a shadow back buffer is free before it parks a synthesised frame in
    // it. Without this flag the buffer can be handed over before its content
    // exists, which is visible as a black flash confined to the video
    // rectangle and only while frames are actually being generated.
    m_waitableShadow = ShadowWaitableEnabled();
    if (m_waitableShadow) {
        sd.Flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    }

    // Create D3D12 SwapChain
    // If the target window already has an active flip swapchain (e.g. MPC-VR on D3D11),
    // creating another flip swapchain on the same HWND fails with E_ACCESSDENIED (0x80070005).
    // We create a lightweight custom child window with NULL background brush to host the shadow swapchain.
    if (m_childHwnd && IsWindow(m_childHwnd)) {
        DestroyWindow(m_childHwnd);
        m_childHwnd = nullptr;
    }

    RECT rc = {};
    GetClientRect(hwnd, &rc);
    int cw = rc.right - rc.left;
    int ch = rc.bottom - rc.top;
    if (cw <= 0) cw = (int)width;
    if (ch <= 0) ch = (int)height;

    RegisterBridgeWindowClass();

    // 1. Enforce WS_CLIPCHILDREN on parent window so parent cannot paint or erase over child
    LONG_PTR parentStyle = GetWindowLongPtrA(hwnd, GWL_STYLE);
    if (!(parentStyle & WS_CLIPCHILDREN)) {
        SetWindowLongPtrA(hwnd, GWL_STYLE, parentStyle | WS_CLIPCHILDREN);
        LogBridge("[D3D11ToD3D12Bridge] Enforced WS_CLIPCHILDREN on parent HWND %p (was 0x%08llX)\n",
                  hwnd, (uint64_t)parentStyle);
    }

    // 2. Subclass parent window to suppress WM_ERASEBKGND and WM_PAINT over child
    if (g_subclassedParentHwnd != hwnd) {
        if (g_subclassedParentHwnd && g_subclassedParentOrigProc && IsWindow(g_subclassedParentHwnd)) {
            SetWindowLongPtrA(g_subclassedParentHwnd, GWLP_WNDPROC, (LONG_PTR)g_subclassedParentOrigProc);
        }
        g_subclassedParentOrigProc = (WNDPROC)SetWindowLongPtrA(hwnd, GWLP_WNDPROC, (LONG_PTR)SubclassedParentWndProc);
        g_subclassedParentHwnd = hwnd;
        m_origParentWndProc = g_subclassedParentOrigProc;
        LogBridge("[D3D11ToD3D12Bridge] Subclassed parent HWND %p to swallow WM_ERASEBKGND & WM_PAINT\n", hwnd);
    }

    // Try creating as WS_CHILD first (integrates cleanly into parent window hierarchy)
    // NOTE: Do NOT use WS_VISIBLE here! Window must stay hidden until first successful
    // D3D12 Present, otherwise DWM sees a black rectangle (no content) causing flicker.
    m_childHwnd = CreateWindowExA(
        WS_EX_NOPARENTNOTIFY,
        "sm86_d3d12_overlay",
        "",
        WS_CHILD | WS_CLIPSIBLINGS,
        0, 0, cw, ch,
        hwnd,
        nullptr,
        GetModuleHandleA(nullptr),
        nullptr
    );

    HWND targetHwnd = m_childHwnd ? m_childHwnd : hwnd;

    hr = f->CreateSwapChainForHwnd(m_cq12, targetHwnd, &sd, nullptr, nullptr, &m_swap12);
    if (FAILED(hr) && targetHwnd != hwnd) {
        LogBridge("[D3D11ToD3D12Bridge] CreateSwapChainForHwnd on WS_CHILD failed (0x%08X), trying WS_POPUP fallback\n",
                  (uint32_t)hr);
        if (m_childHwnd && IsWindow(m_childHwnd)) {
            DestroyWindow(m_childHwnd);
            m_childHwnd = nullptr;
        }
        POINT pt = { 0, 0 };
        ClientToScreen(hwnd, &pt);
        m_childHwnd = CreateWindowExA(
            WS_EX_NOPARENTNOTIFY | WS_EX_NOACTIVATE,
            "sm86_d3d12_overlay",
            "",
            WS_POPUP | WS_CLIPSIBLINGS | WS_DISABLED,
            pt.x, pt.y, cw, ch,
            hwnd,
            nullptr,
            GetModuleHandleA(nullptr),
            nullptr
        );
        targetHwnd = m_childHwnd ? m_childHwnd : hwnd;
        hr = f->CreateSwapChainForHwnd(m_cq12, targetHwnd, &sd, nullptr, nullptr, &m_swap12);
    }

    f->Release();
    if (FAILED(hr) || !m_swap12) {
        LogBridge("[D3D11ToD3D12Bridge] CreateSwapChainForHwnd failed on target HWND %p (parent %p): 0x%08X\n",
                  targetHwnd, hwnd, (uint32_t)hr);
        return false;
    }

    // Inspect if NvPresent64 proxy wrapper is present
    void* vtbl = *(void**)m_swap12;
    HMODULE mod = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)vtbl, &mod);
    char modName[MAX_PATH] = {};
    GetModuleFileNameA(mod, modName, sizeof(modName));
    LogBridge("[D3D11ToD3D12Bridge] m_swap12=%p, vtable=%p (Module: %s)\n", m_swap12, vtbl, modName);

    void** ptrs = (void**)m_swap12;
    for (int i = 0; i < 8; i++) {
        LogBridge("  m_swap12 + 0x%02x: %p\n", i * 8, ptrs[i]);
    }

    HMODULE hNv = GetModuleHandleA("NvPresent64.dll");
    void* wrapper = nullptr;
    if (InspectNvPresentSwapChain(m_swap12, (uintptr_t)hNv, &wrapper) && wrapper) {
        m_wrapper = wrapper;
        void** vt = *(void***)m_wrapper;
        typedef void (*pfnSetByte)(void*, uint8_t);
        ((pfnSetByte)vt[19])(m_wrapper, 1);
        ((pfnSetByte)vt[20])(m_wrapper, 1);
        LogBridge("[D3D11ToD3D12Bridge] >>> SUCCESS: NvPresent64 wrapped D3D12 shadow swapchain @ %p (wrapper @ %p) <<<\n",
                  m_swap12, m_wrapper);
    } else {
        LogBridge("[D3D11ToD3D12Bridge] Warning: NvPresent64 wrapper not found on shadow swapchain %p, aborting bridge\n",
                  m_swap12);
        return false;
    }

    hr = m_swap12->QueryInterface(IID_PPV_ARGS(&m_swap3_12));
    LogBridge("[D3D11ToD3D12Bridge] QueryInterface swap3_12 -> hr=0x%08X, swap3=%p\n", (uint32_t)hr, m_swap3_12);

    // Establish the frame-latency contract on the shadow swapchain.
    if (m_waitableShadow && SUCCEEDED(m_swap12->QueryInterface(IID_PPV_ARGS(&m_swap2_12)))) {
        UINT latency = ShadowMaxLatency();
        HRESULT hlat = m_swap2_12->SetMaximumFrameLatency(latency);
        m_frameLatencyWaitable = m_swap2_12->GetFrameLatencyWaitableObject();
        LogBridge("[D3D11ToD3D12Bridge] Shadow frame-latency contract: SetMaximumFrameLatency(%u) hr=0x%08X, waitable=%p\n",
                  latency, (uint32_t)hlat, m_frameLatencyWaitable);
    } else {
        LogBridge("[D3D11ToD3D12Bridge] Shadow frame-latency waitable DISABLED (SM86_SHADOW_WAITABLE=0 or no IDXGISwapChain2)\n");
    }
    for (UINT i = 0; i < m_swapBufferCount && i < 8; i++) {
        m_swap12->GetBuffer(i, IID_PPV_ARGS(&m_backbuffers12[i]));
    }

    if (m_dev11 && !m_query11) {
        D3D11_QUERY_DESC qd = { D3D11_QUERY_EVENT, 0 };
        m_dev11->CreateQuery(&qd, &m_query11);
    }

    hr = m_dev12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence12));
    if (FAILED(hr)) return false;
    m_fenceEvent12 = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    m_fenceVal12 = 0;

    return true;
}

bool D3D11ToD3D12Bridge::Initialize(ID3D11Device* dev11, HWND hwnd, uint32_t width, uint32_t height, DXGI_FORMAT format) {
    LogBridge("[D3D11ToD3D12Bridge::Initialize] dev11=%p, hwnd=%p, res=%ux%u, fmt=%d\n",
              dev11, hwnd, width, height, (int)format);

    if (!dev11 || !hwnd) {
        LogBridge("[D3D11ToD3D12Bridge::Initialize] FAILED: dev11 or hwnd is NULL\n");
        return false;
    }
    // NvPresent64 resolution guard: Width >= 480 and Height >= 480
    if (width < 480 || height < 480) {
        LogBridge("[D3D11ToD3D12Bridge] Resolution %ux%u below NvPresent 480p threshold, passthrough.\n", width, height);
        return false;
    }

    if (m_active && m_hwnd == hwnd && m_width == width && m_height == height) {
        return true;
    }

    Shutdown();

    m_dev11 = dev11;
    m_dev11->GetImmediateContext(&m_ctx11);
    m_hwnd = hwnd;
    m_width = width;
    m_height = height;
    m_format = format;

    if (!CreateD3D12Resources(hwnd, width, height, format)) {
        LogBridge("[D3D11ToD3D12Bridge::Initialize] CreateD3D12Resources FAILED\n");
        Shutdown();
        return false;
    }

    if (!CreateSharedTexture(width, height, format)) {
        LogBridge("[D3D11ToD3D12Bridge::Initialize] CreateSharedTexture FAILED\n");
        Shutdown();
        return false;
    }

    m_active = true;
    LogBridge("[D3D11ToD3D12Bridge] Shadow D3D12 SwapChain Bridge ACTIVE for %ux%u (HWND %p, Wrapper %p)\n",
              width, height, hwnd, m_wrapper);
    return true;
}

ID3D12Resource* D3D11ToD3D12Bridge::GetCurrentBackbuffer() const {
    if (!m_swap3_12) return m_backbuffers12[0];
    UINT idx = m_swap3_12->GetCurrentBackBufferIndex();
    return m_backbuffers12[idx];
}

bool D3D11ToD3D12Bridge::Present(IDXGISwapChain* swap11, UINT sync, UINT flags) {
    if (!m_active || !swap11 || !m_ctx11 || !m_sharedTex11 || !m_swap12)
        return false;

    if (!m_diag && DiagEnabled()) {
        m_diag = true;
        QueryPerformanceFrequency(&m_qpcFreq);
    }

    // Keep overlay window matching parent client size & screen position
    if (m_childHwnd && m_hwnd && IsWindow(m_childHwnd)) {
        if (IsIconic(m_hwnd) || !IsWindowVisible(m_hwnd)) {
            if (IsWindowVisible(m_childHwnd)) {
                ShowWindow(m_childHwnd, SW_HIDE);
            }
        } else {
            LONG style = GetWindowLongA(m_childHwnd, GWL_STYLE);
            if (style & WS_CHILD) {
                RECT pr = {};
                GetClientRect(m_hwnd, &pr);
                int pw = pr.right - pr.left;
                int ph = pr.bottom - pr.top;
                if (pw > 0 && ph > 0 && (m_lastW != pw || m_lastH != ph)) {
                    SetWindowPos(m_childHwnd, HWND_TOP, 0, 0, pw, ph,
                                 SWP_NOACTIVATE | SWP_NOREDRAW | SWP_NOCOPYBITS);
                    m_lastW = pw; m_lastH = ph;
                }
            } else {
                POINT pt = { 0, 0 };
                ClientToScreen(m_hwnd, &pt);
                RECT pr = {};
                GetClientRect(m_hwnd, &pr);
                int pw = pr.right - pr.left;
                int ph = pr.bottom - pr.top;
                if (pw > 0 && ph > 0) {
                    if (m_lastX != pt.x || m_lastY != pt.y || m_lastW != pw || m_lastH != ph) {
                        SetWindowPos(m_childHwnd, HWND_TOP, pt.x, pt.y, pw, ph,
                                     SWP_NOACTIVATE | SWP_NOREDRAW | SWP_NOCOPYBITS);
                        m_lastX = pt.x; m_lastY = pt.y; m_lastW = pw; m_lastH = ph;
                    }
                }
            }
        }
    }

    LARGE_INTEGER tStage0 = {};
    if (m_diag) QueryPerformanceCounter(&tStage0);

    // Wait for the shadow swapchain to hand us a free back buffer. This is the
    // other half of the frame-latency contract: we must not write a buffer that
    // frame generation may still be holding.
    if (m_frameLatencyWaitable) {
        DWORD wr = WaitForSingleObject(m_frameLatencyWaitable, 1000);
        static int s_timeouts = 0;
        if (wr != WAIT_OBJECT_0 && s_timeouts++ < 5) {
            LogBridge("[D3D11ToD3D12Bridge::Present] frame-latency wait timed out (wr=%lu)\n", wr);
        }
    }

    // 1. Get D3D11 backbuffer (using current backbuffer index if flip model) and copy to shared texture
    UINT bbIndex = 0;
    IDXGISwapChain3* sc3_11 = nullptr;
    if (SUCCEEDED(swap11->QueryInterface(IID_PPV_ARGS(&sc3_11)))) {
        bbIndex = sc3_11->GetCurrentBackBufferIndex();
        sc3_11->Release();
    }

    ID3D11Texture2D* bb11 = nullptr;
    HRESULT hr = swap11->GetBuffer(bbIndex, IID_PPV_ARGS(&bb11));
    if (FAILED(hr) && bbIndex != 0) {
        hr = swap11->GetBuffer(0, IID_PPV_ARGS(&bb11));
    }
    if (FAILED(hr) || !bb11) return false;

    m_ctx11->CopyResource(m_sharedTex11, bb11);
    bb11->Release();

    if (m_query11) {
        m_ctx11->End(m_query11);
        m_ctx11->Flush();
        while (m_ctx11->GetData(m_query11, nullptr, 0, 0) == S_FALSE) {
            YieldProcessor();
        }
    } else {
        m_ctx11->Flush();
    }

    double copy11Us = 0.0;
    if (m_diag) {
        LARGE_INTEGER tNow = {};
        QueryPerformanceCounter(&tNow);
        copy11Us = (double)(tNow.QuadPart - tStage0.QuadPart) * 1e6 / (double)m_qpcFreq.QuadPart;
    }

    // 2. In D3D12, copy shared texture to current D3D12 backbuffer

    LARGE_INTEGER tStage1 = {};
    if (m_diag) QueryPerformanceCounter(&tStage1);

    UINT idx = m_swap3_12 ? m_swap3_12->GetCurrentBackBufferIndex() : 0;
    static int s_idxLog = 0;
    if (s_idxLog++ < 10) {
        LogBridge("[D3D11ToD3D12Bridge::Present] Frame #%d: idx=%u (swap3=%p)\n", s_idxLog, idx, m_swap3_12);
    }

    ID3D12Resource* bb12 = m_backbuffers12[idx];

    m_alloc12->Reset();
    m_cl12->Reset(m_alloc12, nullptr);

    D3D12_RESOURCE_BARRIER barriers[2] = {};
    barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[0].Transition.pResource = bb12;
    barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[1].Transition.pResource = m_sharedTex12;
    barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_cl12->ResourceBarrier(2, barriers);

    m_cl12->CopyResource(bb12, m_sharedTex12);

    barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;

    barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    m_cl12->ResourceBarrier(2, barriers);

    m_cl12->Close();

    ID3D12CommandList* lists[] = { m_cl12 };
    m_cq12->ExecuteCommandLists(1, lists);

    m_fenceVal12++;
    m_cq12->Signal(m_fence12, m_fenceVal12);
    if (m_fence12->GetCompletedValue() < m_fenceVal12) {
        m_fence12->SetEventOnCompletion(m_fenceVal12, m_fenceEvent12);
        WaitForSingleObject(m_fenceEvent12, INFINITE);
    }

    // Strip DXGI_PRESENT_ALLOW_TEARING (0x200) - shadow swapchain was NOT created with
    // DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING, passing this flag is a DXGI contract violation
    // and causes scanout-level black frame flicker that cannot be captured by screen recording.
    UINT shadowFlags = flags & ~0x200u; // strip DXGI_PRESENT_ALLOW_TEARING
    // Force VSync for NvPresent64 frame generation - it needs stable VBlank pulses
    // to correctly time interpolated frames. sync=0 causes tearing at scanout.
    UINT shadowSync = (sync > 0) ? sync : 1;

    hr = m_swap12->Present(shadowSync, shadowFlags);

    if (m_diag) {
        double copy12Us = 0.0;
        LARGE_INTEGER tNow = {};
        QueryPerformanceCounter(&tNow);
        copy12Us = (double)(tNow.QuadPart - tStage1.QuadPart) * 1e6 / (double)m_qpcFreq.QuadPart;
        m_presIndex++;
        DiagPresentRow(sync, flags, shadowSync, idx, copy11Us, copy12Us, hr);
    }

    if (SUCCEEDED(hr)) {
        if (m_childHwnd && !IsWindowVisible(m_childHwnd)) {
            ShowWindow(m_childHwnd, SW_SHOWNA);
        }
        static uint64_t s_pCount = 0;
        if (++s_pCount <= 10 || s_pCount % 60 == 0) {
            LogBridge("[D3D11ToD3D12Bridge::Present] Frame #%llu (app: sync=%u flags=0x%X -> shadow: sync=%u flags=0x%X) hr=0x%08X\n",
                      s_pCount, sync, flags, shadowSync, shadowFlags, (uint32_t)hr);
        }
        return true;
    }

    return false;
}

} // namespace sm86
