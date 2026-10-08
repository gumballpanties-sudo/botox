#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "webview2host.h"

#include <windows.h>
#include <winhttp.h>
#include <wrl.h>
#include <d3d11.h>
#include <atomic>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <vector>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include "../../dependencies/webview2/include/WebView2.h"
#include "../../dependencies/webview2/include/WebView2EnvironmentOptions.h"
#include "../../dependencies/webview2/webview2_loader_x86.h"

#pragma comment(lib, "winhttp.lib")

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
namespace wgc  = winrt::Windows::Graphics::Capture;
namespace wgdx = winrt::Windows::Graphics::DirectX;

namespace {

enum CmdOp { C_NAV, C_JS, C_START_JS, C_SIZE, C_MOUSE, C_KEY, C_BACK, C_FWD, C_RELOAD, C_MUTE, C_CLEAR, C_BW, C_MEM };
struct Cmd { CmdOp op; std::string s; int a = 0, b = 0, c = 0, d = 0; };

constexpr UINT WM_WV2_CMD  = WM_APP + 0x57;
constexpr UINT WM_WV2_QUIT = WM_APP + 0x58;

std::mutex             g_qLock;
std::deque<Cmd>        g_cmds;
std::deque<wv2::Event> g_events;
std::string            g_diagNote;

HANDLE              g_thread = nullptr;
std::atomic<bool>   g_running{ false };
std::atomic<HWND>   g_hwnd{ nullptr };
std::atomic<bool>   g_ready{ false };
std::atomic<long>   g_frames{ 0 };
std::atomic<int>    g_capW{ 0 }, g_capH{ 0 };
wv2::FrameSink      g_sink   = nullptr;
wv2::NavFilter      g_filter = nullptr;
std::string         g_args;
int                 g_startW = 1280, g_startH = 720;

ComPtr<ICoreWebView2Environment> s_env;
ComPtr<ICoreWebView2Controller>  s_ctl;
ComPtr<ICoreWebView2>            s_wv;
int  s_w = 0, s_h = 0;
WPARAM s_btn = 0;

std::mutex                                      s_capLock;
ComPtr<ID3D11Device>                            s_dev;
ComPtr<ID3D11DeviceContext>                     s_ctx;
ComPtr<ID3D11Texture2D>                         s_stage;
winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice s_rtDev{ nullptr };
wgc::GraphicsCaptureItem                        s_item{ nullptr };
wgc::Direct3D11CaptureFramePool                 s_pool{ nullptr };
wgc::GraphicsCaptureSession                     s_session{ nullptr };
winrt::Windows::Graphics::SizeInt32             s_poolSize{ 0, 0 };
winrt::event_token                              s_frameTok{};
std::atomic<bool>                               s_capStopping{ false };
std::atomic<int>                                s_inFrame{ 0 };
std::atomic<bool>                               s_capOn{ true };

constexpr int k_capMaxFps = 60;

std::string Utf8(const wchar_t* w)
{
    if (!w) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s((size_t)n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}
std::wstring Wide(const std::string& s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}
std::string Take(LPWSTR p) { std::string s = Utf8(p); if (p) CoTaskMemFree(p); return s; }

void Emit(wv2::EvType t, std::string a = {}, std::string b = {}, int n = 0)
{
    std::lock_guard<std::mutex> lk(g_qLock);
    if (g_events.size() < 256) g_events.push_back(wv2::Event{ t, std::move(a), std::move(b), n });
}
void Note(const std::string& s) { std::lock_guard<std::mutex> lk(g_qLock); g_diagNote = s; }

void Post(Cmd c)
{
    {
        std::lock_guard<std::mutex> lk(g_qLock);
        // Mouse moves can flood while the engine is still starting; never let that grow unbounded.
        if (g_cmds.size() >= 512) return;
        g_cmds.push_back(std::move(c));
    }
    if (HWND h = g_hwnd.load()) PostMessageW(h, WM_WV2_CMD, 0, 0);
}

void OnFrameBody(const wgc::Direct3D11CaptureFramePool& pool);

void OnFrame(const wgc::Direct3D11CaptureFramePool& pool, const winrt::Windows::Foundation::IInspectable&)
{
    s_inFrame.fetch_add(1);
    if (!s_capStopping) { try { OnFrameBody(pool); } catch (...) {} }
    s_inFrame.fetch_sub(1);
}

bool FrameDue()
{
    static LARGE_INTEGER s_freq{};
    static LONGLONG      s_next = 0;
    if (!s_freq.QuadPart) QueryPerformanceFrequency(&s_freq);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    if (t.QuadPart < s_next) return false;
    s_next = t.QuadPart + s_freq.QuadPart / k_capMaxFps;   // from now, so a late frame never bursts
    return true;
}

void CapCopy(const wgc::Direct3D11CaptureFrame& frame, const winrt::Windows::Graphics::SizeInt32& size)
{
    auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    ComPtr<ID3D11Texture2D> tex;
    if (FAILED(access->GetInterface(IID_PPV_ARGS(&tex))) || !tex) return;

    D3D11_TEXTURE2D_DESC td{};
    tex->GetDesc(&td);
    const int w = (int)(std::min)((UINT)size.Width,  td.Width);
    const int h = (int)(std::min)((UINT)size.Height, td.Height);
    if (w <= 0 || h <= 0) return;

    std::lock_guard<std::mutex> lk(s_capLock);
    if (!s_ctx) return;
    D3D11_TEXTURE2D_DESC sd{};
    if (s_stage) s_stage->GetDesc(&sd);
    if (!s_stage || sd.Width != td.Width || sd.Height != td.Height) {
        s_stage.Reset();
        D3D11_TEXTURE2D_DESC nd = td;
        nd.Usage = D3D11_USAGE_STAGING; nd.BindFlags = 0; nd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        nd.MiscFlags = 0; nd.MipLevels = 1; nd.ArraySize = 1; nd.SampleDesc = { 1, 0 };
        if (FAILED(s_dev->CreateTexture2D(&nd, nullptr, &s_stage))) return;
    }
    s_ctx->CopyResource(s_stage.Get(), tex.Get());
    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(s_ctx->Map(s_stage.Get(), 0, D3D11_MAP_READ, 0, &m))) {
        if (g_sink) g_sink((const uint8_t*)m.pData, w, h, (int)m.RowPitch);
        s_ctx->Unmap(s_stage.Get(), 0);
        g_frames.fetch_add(1);
        g_capW = w; g_capH = h;
    }
}

void OnFrameBody(const wgc::Direct3D11CaptureFramePool& pool)
{
    auto frame = pool.TryGetNextFrame();
    if (!frame) return;
    const auto size = frame.ContentSize();
    const bool resized = size.Width != s_poolSize.Width || size.Height != s_poolSize.Height;

    if ((s_capOn.load() && FrameDue()) || resized) CapCopy(frame, size);

    if (resized) {
        frame.Close();
        s_poolSize = size;
        try { pool.Recreate(s_rtDev, wgdx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size); } catch (...) {}
    }
}

bool CapStart(HWND hwnd)
{
    try {
        using WinRtDev_t = HRESULT(WINAPI*)(IDXGIDevice*, ::IInspectable**);
        static HMODULE s_d3d11 = LoadLibraryW(L"d3d11.dll");
        const auto D3D11CreateDevice = s_d3d11 ? (PFN_D3D11_CREATE_DEVICE)GetProcAddress(s_d3d11, "D3D11CreateDevice") : nullptr;
        const auto CreateDirect3D11DeviceFromDXGIDevice =
            s_d3d11 ? (WinRtDev_t)GetProcAddress(s_d3d11, "CreateDirect3D11DeviceFromDXGIDevice") : nullptr;
        if (!D3D11CreateDevice || !CreateDirect3D11DeviceFromDXGIDevice) { Note("capture: d3d11.dll exports missing"); return false; }

        D3D_FEATURE_LEVEL fl;
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                       nullptr, 0, D3D11_SDK_VERSION, &s_dev, &fl, &s_ctx);
        if (FAILED(hr))
            hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                   nullptr, 0, D3D11_SDK_VERSION, &s_dev, &fl, &s_ctx);
        if (FAILED(hr)) { Note("capture: no D3D11 device"); return false; }

        ComPtr<IDXGIDevice> dxgi;
        s_dev.As(&dxgi);
        winrt::com_ptr<::IInspectable> insp;
        if (FAILED(CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(), insp.put()))) { Note("capture: no WinRT device"); return false; }
        s_rtDev = insp.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();

        auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        if (FAILED(interop->CreateForWindow(hwnd, winrt::guid_of<wgc::GraphicsCaptureItem>(), winrt::put_abi(s_item))) || !s_item) {
            Note("capture: CreateForWindow failed"); return false;
        }
        s_poolSize = s_item.Size();
        s_pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
            s_rtDev, wgdx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, s_poolSize);
        s_capStopping = false;
        s_frameTok = s_pool.FrameArrived(&OnFrame);
        s_session = s_pool.CreateCaptureSession(s_item);
        try { s_session.IsCursorCaptureEnabled(false); } catch (...) {}
        try { s_session.IsBorderRequired(false); }      catch (...) {}
        s_session.StartCapture();
        return true;
    } catch (const winrt::hresult_error& e) {
        Note("capture: " + Utf8(e.message().c_str()));
        return false;
    } catch (...) {
        Note("capture: exception");
        return false;
    }
}

void CapStop()
{
    s_capStopping = true;
    try { if (s_pool && s_frameTok.value) s_pool.FrameArrived(s_frameTok); } catch (...) {}
    s_frameTok = {};
    try { if (s_session) s_session.Close(); } catch (...) {}
    try { if (s_pool)    s_pool.Close();    } catch (...) {}
    for (int i = 0; i < 200 && s_inFrame.load() > 0; ++i) Sleep(5);
    std::lock_guard<std::mutex> lk(s_capLock);
    s_session = nullptr; s_pool = nullptr; s_item = nullptr; s_rtDev = nullptr;
    s_stage.Reset(); s_ctx.Reset(); s_dev.Reset();
}

void EnsureFocus()
{
    HWND host = g_hwnd.load();
    HWND f = GetFocus();
    if (s_ctl && !(f && f != host && IsChild(host, f)))
        s_ctl->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
}

void Cdp(const wchar_t* method, const std::string& json);

void DoMouse(const Cmd& c)
{
    static DWORD s_lastPressMs = 0;
    static int   s_lastPressX = 0, s_lastPressY = 0, s_clicks = 0;
    static const char* s_lastPressBtn = "";

    const int x = c.b, y = c.c;
    const char* btn = "none";
    const char* type = "mouseMoved";
    switch ((wv2::Mouse)c.a) {
    case wv2::M_MOVE:  break;
    case wv2::M_LDOWN: EnsureFocus(); s_btn |= MK_LBUTTON; type = "mousePressed";  btn = "left";  break;
    case wv2::M_LUP:   s_btn &= ~(WPARAM)MK_LBUTTON;       type = "mouseReleased"; btn = "left";  break;
    case wv2::M_RDOWN: EnsureFocus(); s_btn |= MK_RBUTTON; type = "mousePressed";  btn = "right"; break;
    case wv2::M_RUP:   s_btn &= ~(WPARAM)MK_RBUTTON;       type = "mouseReleased"; btn = "right"; break;
    case wv2::M_WHEEL:
        Cdp(L"Input.dispatchMouseEvent", "{\"type\":\"mouseWheel\",\"x\":" + std::to_string(x) + ",\"y\":" +
            std::to_string(y) + ",\"deltaX\":0,\"deltaY\":" + std::to_string(-c.d) + "}");
        return;
    }
    const int buttons = ((s_btn & MK_LBUTTON) ? 1 : 0) | ((s_btn & MK_RBUTTON) ? 2 : 0);
    if (!strcmp(type, "mouseMoved")) btn = (s_btn & MK_LBUTTON) ? "left" : ((s_btn & MK_RBUTTON) ? "right" : "none");
    if (!strcmp(type, "mousePressed")) {
        const DWORD now = GetTickCount();
        const bool again = !strcmp(btn, s_lastPressBtn) && now - s_lastPressMs <= GetDoubleClickTime() &&
                           abs(x - s_lastPressX) <= GetSystemMetrics(SM_CXDOUBLECLK) / 2 &&
                           abs(y - s_lastPressY) <= GetSystemMetrics(SM_CYDOUBLECLK) / 2;
        s_clicks = again ? s_clicks + 1 : 1;
        s_lastPressMs = now; s_lastPressX = x; s_lastPressY = y; s_lastPressBtn = btn;
    }
    const int clicks = strcmp(type, "mouseMoved") ? s_clicks : 0;
    Cdp(L"Input.dispatchMouseEvent", std::string("{\"type\":\"") + type + "\",\"x\":" + std::to_string(x) +
        ",\"y\":" + std::to_string(y) + ",\"button\":\"" + btn + "\",\"buttons\":" + std::to_string(buttons) +
        ",\"clickCount\":" + std::to_string(clicks) + "}");
}

struct KeyName { UINT vk; const char* key; const char* code; };
const KeyName k_keyNames[] = {
    { VK_RETURN, "Enter", "Enter" },      { VK_BACK, "Backspace", "Backspace" }, { VK_TAB, "Tab", "Tab" },
    { VK_ESCAPE, "Escape", "Escape" },    { VK_SPACE, " ", "Space" },            { VK_DELETE, "Delete", "Delete" },
    { VK_INSERT, "Insert", "Insert" },    { VK_HOME, "Home", "Home" },           { VK_END, "End", "End" },
    { VK_PRIOR, "PageUp", "PageUp" },     { VK_NEXT, "PageDown", "PageDown" },   { VK_LEFT, "ArrowLeft", "ArrowLeft" },
    { VK_RIGHT, "ArrowRight", "ArrowRight" }, { VK_UP, "ArrowUp", "ArrowUp" },   { VK_DOWN, "ArrowDown", "ArrowDown" },
    { VK_SHIFT, "Shift", "ShiftLeft" },   { VK_CONTROL, "Control", "ControlLeft" }, { VK_MENU, "Alt", "AltLeft" },
};

std::string JsonStr(const std::string& s)
{
    std::string o = "\"";
    for (unsigned char ch : s) {
        if (ch == '"' || ch == '\\') { o += '\\'; o += (char)ch; }
        else if (ch < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", ch); o += b; }
        else o += (char)ch;
    }
    return o + "\"";
}

void Cdp(const wchar_t* method, const std::string& json)
{
    if (s_wv) s_wv->CallDevToolsProtocolMethod(method, Wide(json).c_str(), nullptr);
}
void Cdp(const std::string& json) { Cdp(L"Input.dispatchKeyEvent", json); }

void DoKey(const Cmd& c)
{
    EnsureFocus();
    const UINT code = (UINT)c.b;
    const int  mods = c.c;
    if (c.a == wv2::K_CHAR) {
        if (code < 0x20) return;
        wchar_t w[2] = { (wchar_t)code, 0 };
        const std::string t = Utf8(w);
        Cdp("{\"type\":\"char\",\"text\":" + JsonStr(t) + ",\"unmodifiedText\":" + JsonStr(t) +
            ",\"modifiers\":" + std::to_string(mods) + "}");
        return;
    }
    std::string key, kcode;
    for (const KeyName& k : k_keyNames) if (k.vk == code) { key = k.key; kcode = k.code; break; }
    if (key.empty()) {
        if (code >= 'A' && code <= 'Z') {
            key = std::string(1, (char)((mods & wv2::KMOD_SHIFT) ? code : code + 32));
            kcode = std::string("Key") + (char)code;
        } else if (code >= '0' && code <= '9') {
            key = std::string(1, (char)code); kcode = std::string("Digit") + (char)code;
        } else if (code >= VK_F1 && code <= VK_F12) {
            key = kcode = "F" + std::to_string(code - VK_F1 + 1);
        } else {
            const UINT ch = MapVirtualKeyW(code, MAPVK_VK_TO_CHAR) & 0x7FFF;
            if (ch) { wchar_t w[2] = { (wchar_t)ch, 0 }; key = Utf8(w); }
        }
    }
    std::string j = std::string("{\"type\":\"") + (c.a == wv2::K_UP ? "keyUp" : "rawKeyDown") + "\"" +
                    ",\"windowsVirtualKeyCode\":" + std::to_string(code) +
                    ",\"nativeVirtualKeyCode\":" + std::to_string(code) +
                    ",\"modifiers\":" + std::to_string(mods);
    if (!key.empty())   j += ",\"key\":" + JsonStr(key);
    if (!kcode.empty()) j += ",\"code\":" + JsonStr(kcode);
    Cdp(j + "}");
    if (c.a == wv2::K_DOWN && code == VK_RETURN)
        Cdp("{\"type\":\"char\",\"text\":\"\\r\",\"unmodifiedText\":\"\\r\",\"key\":\"Enter\",\"code\":\"Enter\","
            "\"windowsVirtualKeyCode\":13,\"modifiers\":" + std::to_string(mods) + "}");
}

void DoResize(int w, int h)
{
    if (w < 16 || h < 16) return;
    s_w = w; s_h = h;
    HWND host = g_hwnd.load();
    SetWindowPos(host, nullptr, 0, 0, w, h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    if (s_ctl) s_ctl->put_Bounds(RECT{ 0, 0, w, h });
}

void MemLevel(bool low)
{
    ComPtr<ICoreWebView2_19> w19;
    if (s_wv && SUCCEEDED(s_wv.As(&w19)))
        w19->put_MemoryUsageTargetLevel(low ? COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL_LOW : COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL_NORMAL);
}

void DrainCmds()
{
    if (!g_ready) return;
    std::deque<Cmd> q;
    { std::lock_guard<std::mutex> lk(g_qLock); q.swap(g_cmds); }
    for (const Cmd& c : q) {
        switch (c.op) {
        case C_NAV:
            if (FAILED(s_wv->Navigate(Wide(c.s).c_str()))) Emit(wv2::EV_FINISHED, c.s, "not a valid URL", 0);
            break;
        case C_JS:       s_wv->ExecuteScript(Wide(c.s).c_str(), nullptr); break;
        case C_START_JS: s_wv->AddScriptToExecuteOnDocumentCreated(Wide(c.s).c_str(), nullptr); break;
        case C_SIZE:     DoResize(c.a, c.b); break;
        case C_MOUSE:    DoMouse(c); break;
        case C_KEY:      DoKey(c); break;
        case C_BW:
            s_wv->AddScriptToExecuteOnDocumentCreated(Wide(c.s).c_str(), nullptr);
            s_wv->ExecuteScript(Wide(c.s).c_str(), nullptr);
            break;
        case C_BACK:     s_wv->GoBack(); break;
        case C_FWD:      s_wv->GoForward(); break;
        case C_RELOAD:   s_wv->Reload(); break;
        case C_MEM:      MemLevel(c.a != 0); break;
        case C_MUTE: {
            ComPtr<ICoreWebView2_8> w8;
            if (SUCCEEDED(s_wv.As(&w8))) w8->put_IsMuted(c.a ? TRUE : FALSE);
            break;
        }
        case C_CLEAR: {
            ComPtr<ICoreWebView2_13> w13; ComPtr<ICoreWebView2Profile> prof; ComPtr<ICoreWebView2Profile2> prof2;
            if (SUCCEEDED(s_wv.As(&w13)) && SUCCEEDED(w13->get_Profile(&prof)) && SUCCEEDED(prof.As(&prof2))) {
                prof2->ClearBrowsingDataAll(Callback<ICoreWebView2ClearBrowsingDataCompletedHandler>(
                    [](HRESULT hr) -> HRESULT { Emit(wv2::EV_CLEARED, SUCCEEDED(hr) ? "ok" : "failed"); return S_OK; }).Get());
            } else {
                Emit(wv2::EV_CLEARED, "unsupported runtime");
            }
            break;
        }
        }
    }
}

void Fail(const char* what, HRESULT hr)
{
    char b[160];
    snprintf(b, sizeof(b), "%s (hr=0x%08lX)", what, (unsigned long)hr);
    Note(b);
    Emit(wv2::EV_FAILED, b);
}

std::string CurrentTitle()
{
    LPWSTR t = nullptr;
    if (s_wv && SUCCEEDED(s_wv->get_DocumentTitle(&t))) return Take(t);
    return {};
}
std::string CurrentUrl()
{
    LPWSTR u = nullptr;
    if (s_wv && SUCCEEDED(s_wv->get_Source(&u))) return Take(u);
    return {};
}

void Configure()
{
    ComPtr<ICoreWebView2Settings> st;
    if (SUCCEEDED(s_wv->get_Settings(&st))) {
        st->put_AreDefaultScriptDialogsEnabled(FALSE);
        st->put_AreDefaultContextMenusEnabled(FALSE);
        st->put_AreDevToolsEnabled(FALSE);
        st->put_IsStatusBarEnabled(FALSE);
        st->put_IsZoomControlEnabled(FALSE);
        ComPtr<ICoreWebView2Settings3> s3; if (SUCCEEDED(st.As(&s3))) s3->put_AreBrowserAcceleratorKeysEnabled(FALSE);
        ComPtr<ICoreWebView2Settings4> s4; if (SUCCEEDED(st.As(&s4))) { s4->put_IsGeneralAutofillEnabled(FALSE); s4->put_IsPasswordAutosaveEnabled(FALSE); }
        ComPtr<ICoreWebView2Settings5> s5; if (SUCCEEDED(st.As(&s5))) s5->put_IsPinchZoomEnabled(FALSE);
        ComPtr<ICoreWebView2Settings6> s6; if (SUCCEEDED(st.As(&s6))) s6->put_IsSwipeNavigationEnabled(FALSE);
    }

    EventRegistrationToken tok;
    s_wv->add_NavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>(
        [](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* a) -> HRESULT {
            LPWSTR u = nullptr;
            a->get_Uri(&u);
            const std::string url = Take(u);
            if (g_filter && g_filter(url.c_str())) a->put_Cancel(TRUE);
            return S_OK;
        }).Get(), &tok);
    s_wv->add_SourceChanged(Callback<ICoreWebView2SourceChangedEventHandler>(
        [](ICoreWebView2*, ICoreWebView2SourceChangedEventArgs*) -> HRESULT {
            Emit(wv2::EV_URL, CurrentUrl());
            return S_OK;
        }).Get(), &tok);
    s_wv->add_NavigationCompleted(Callback<ICoreWebView2NavigationCompletedEventHandler>(
        [](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* a) -> HRESULT {
            BOOL ok = FALSE; a->get_IsSuccess(&ok);
            COREWEBVIEW2_WEB_ERROR_STATUS es = COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN; a->get_WebErrorStatus(&es);
            if (!ok && es == COREWEBVIEW2_WEB_ERROR_STATUS_OPERATION_CANCELED) return S_OK;
            Emit(wv2::EV_FINISHED, CurrentUrl(), ok ? CurrentTitle() : ("error " + std::to_string((int)es)), ok ? 1 : 0);
            return S_OK;
        }).Get(), &tok);
    s_wv->add_DocumentTitleChanged(Callback<ICoreWebView2DocumentTitleChangedEventHandler>(
        [](ICoreWebView2*, IUnknown*) -> HRESULT { Emit(wv2::EV_TITLE, CurrentTitle()); return S_OK; }).Get(), &tok);
    s_wv->add_NewWindowRequested(Callback<ICoreWebView2NewWindowRequestedEventHandler>(
        [](ICoreWebView2* wv, ICoreWebView2NewWindowRequestedEventArgs* a) -> HRESULT {
            LPWSTR u = nullptr; a->get_Uri(&u);
            a->put_Handled(TRUE);
            if (u) { wv->Navigate(u); CoTaskMemFree(u); }
            return S_OK;
        }).Get(), &tok);
    s_wv->add_ScriptDialogOpening(Callback<ICoreWebView2ScriptDialogOpeningEventHandler>(
        [](ICoreWebView2*, ICoreWebView2ScriptDialogOpeningEventArgs* a) -> HRESULT { a->Accept(); return S_OK; }).Get(), &tok);
    s_wv->add_PermissionRequested(Callback<ICoreWebView2PermissionRequestedEventHandler>(
        [](ICoreWebView2*, ICoreWebView2PermissionRequestedEventArgs* a) -> HRESULT {
            a->put_State(COREWEBVIEW2_PERMISSION_STATE_DENY);
            return S_OK;
        }).Get(), &tok);
    ComPtr<ICoreWebView2_4> w4;
    if (SUCCEEDED(s_wv.As(&w4)))
        w4->add_DownloadStarting(Callback<ICoreWebView2DownloadStartingEventHandler>(
            [](ICoreWebView2*, ICoreWebView2DownloadStartingEventArgs* a) -> HRESULT { a->put_Cancel(TRUE); return S_OK; }).Get(), &tok);
    s_wv->add_ProcessFailed(Callback<ICoreWebView2ProcessFailedEventHandler>(
        [](ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs* a) -> HRESULT {
            COREWEBVIEW2_PROCESS_FAILED_KIND k = COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED;
            a->get_ProcessFailedKind(&k);
            if (k == COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED) {
                Emit(wv2::EV_CRASHED, "browser process exited");
            } else if (k == COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_EXITED ||
                       k == COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_UNRESPONSIVE) {
                if (s_wv) s_wv->Reload();
                Note("renderer died, reloaded");
            }
            return S_OK;
        }).Get(), &tok);
}

HRESULT OnController(HRESULT hr, ICoreWebView2Controller* ctl)
{
    if (FAILED(hr) || !ctl) { Fail("CreateCoreWebView2Controller failed", hr); return S_OK; }
    s_ctl = ctl;
    s_ctl->get_CoreWebView2(&s_wv);
    if (!s_wv) { Fail("controller has no CoreWebView2", E_FAIL); return S_OK; }

    ComPtr<ICoreWebView2Controller3> c3;
    if (SUCCEEDED(s_ctl.As(&c3))) {
        c3->put_ShouldDetectMonitorScaleChanges(FALSE);
        c3->put_RasterizationScale(1.0);
    }
    s_ctl->put_Bounds(RECT{ 0, 0, s_w, s_h });
    s_ctl->put_IsVisible(TRUE);
    Configure();
    MemLevel(!s_capOn.load());

    if (!CapStart(g_hwnd.load())) { Fail("window capture unavailable", E_FAIL); return S_OK; }
    g_ready = true;
    Emit(wv2::EV_READY);
    DrainCmds();
    return S_OK;
}

LRESULT CALLBACK HostProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_WV2_CMD:      DrainCmds(); return 0;
    case WM_WV2_QUIT:     PostQuitMessage(0); return 0;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_CLOSE:        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

std::wstring UserDataDir()
{
    std::wstring d = L"C:\\botox";
    CreateDirectoryW(d.c_str(), nullptr);
    d += L"\\webview2";
    CreateDirectoryW(d.c_str(), nullptr);
    return d;
}

void TrimProfile()
{
    static bool s_done = false;
    if (s_done) return;
    s_done = true;
    const std::wstring root = UserDataDir() + L"\\EBWebView\\";
    if (!DeleteFileW((root + L"lockfile").c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND) {
        Note("profile in use, cache kept");
        return;
    }
    static const wchar_t* const k_junk[] = {
        L"Default\\Cache", L"Default\\Code Cache", L"Default\\GPUCache", L"Default\\DawnGraphiteCache",
        L"Default\\DawnWebGPUCache", L"GrShaderCache", L"ShaderCache", L"GPUPersistentCache",
        L"Crashpad", L"Local Traces", L"component_crx_cache", L"extensions_crx_cache",
    };
    std::error_code ec;
    for (const wchar_t* d : k_junk)
        std::filesystem::remove_all(root + d, ec);
}

using GetVersion_t = HRESULT(STDAPICALLTYPE*)(PCWSTR, LPWSTR*);
using CreateEnv_t  = HRESULT(STDAPICALLTYPE*)(PCWSTR, PCWSTR, ICoreWebView2EnvironmentOptions*,
                                              ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*);
GetVersion_t g_pfnGetVersion = nullptr;
CreateEnv_t  g_pfnCreateEnv  = nullptr;

bool WvLoader()
{
    if (g_pfnGetVersion && g_pfnCreateEnv) return true;
    static bool s_failed = false;
    if (s_failed) return false;

    const std::wstring path = UserDataDir() + L"\\WebView2Loader.dll";
    bool same = false;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER sz{};
        if (GetFileSizeEx(f, &sz) && sz.QuadPart == (LONGLONG)webview2_loader_x86_size) {
            std::vector<uint8_t> have(webview2_loader_x86_size);
            DWORD got = 0;
            same = ReadFile(f, have.data(), (DWORD)have.size(), &got, nullptr) && got == (DWORD)have.size() &&
                   !memcmp(have.data(), webview2_loader_x86, have.size());
        }
        CloseHandle(f);
    }
    if (!same) {
        f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            DWORD put = 0;
            WriteFile(f, webview2_loader_x86, webview2_loader_x86_size, &put, nullptr);
            CloseHandle(f);
        }
    }

    if (HMODULE m = LoadLibraryW(path.c_str())) {
        g_pfnGetVersion = (GetVersion_t)GetProcAddress(m, "GetAvailableCoreWebView2BrowserVersionString");
        g_pfnCreateEnv  = (CreateEnv_t)GetProcAddress(m, "CreateCoreWebView2EnvironmentWithOptions");
    }
    if (!g_pfnGetVersion || !g_pfnCreateEnv) {
        s_failed = true;
        Note("loader: WebView2Loader.dll did not load (err " + std::to_string(GetLastError()) + ")");
        return false;
    }
    return true;
}

DWORD WINAPI EngineThread(LPVOID)
{
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HRESULT coHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc   = HostProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"botox_wv2_host";
    RegisterClassExW(&wc);

    // Beyond the right edge of every monitor: never seen, never clicked, never in alt-tab
    // (tool window), never activated. DWM still composes it, which is all capture needs.
    const int x = GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN) + 64;
    const int y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    s_w = g_startW; s_h = g_startH;
    s_btn = 0;   // a restart mid-drag must not start with the button held
    HWND host = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"botox web",
                                WS_POPUP, x, y, s_w, s_h, nullptr, nullptr, wc.hInstance, nullptr);
    if (!host) {
        Fail("CreateWindowEx failed", HRESULT_FROM_WIN32(GetLastError()));
    } else {
        ShowWindow(host, SW_SHOWNOACTIVATE);
        g_hwnd = host;
        if (!g_running) PostMessageW(host, WM_WV2_QUIT, 0, 0);

        TrimProfile();   // botox: off the game thread, before the browser locks the files
        auto opts = Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();
        opts->put_AdditionalBrowserArguments(Wide(g_args).c_str());
        const HRESULT hr = !WvLoader() ? E_NOINTERFACE :
            g_pfnCreateEnv(nullptr, UserDataDir().c_str(), opts.Get(),
            Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
                [](HRESULT hr, ICoreWebView2Environment* env) -> HRESULT {
                    if (FAILED(hr) || !env) { Fail("WebView2 environment failed", hr); return S_OK; }
                    s_env = env;
                    const HRESULT h2 = env->CreateCoreWebView2Controller(g_hwnd.load(),
                        Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(&OnController).Get());
                    if (FAILED(h2)) Fail("CreateCoreWebView2Controller refused", h2);
                    return S_OK;
                }).Get());
        if (FAILED(hr)) Fail("CreateCoreWebView2EnvironmentWithOptions refused", hr);

        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    g_ready = false;
    CapStop();
    if (s_ctl) s_ctl->Close();
    s_wv.Reset(); s_ctl.Reset(); s_env.Reset();
    if (host) DestroyWindow(host);
    g_hwnd = nullptr;
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    if (SUCCEEDED(coHr)) CoUninitialize();
    return 0;
}

std::atomic<float>     g_bwMbps{ -1.f };
std::atomic<HINTERNET> g_bwSession{ nullptr };
HANDLE                 g_bwThread = nullptr;

void PostBandwidth()
{
    const float mbps = g_bwMbps.load();
    if (mbps <= 0.f) return;
    char js[64];
    snprintf(js, sizeof(js), "window.__fpsBw=%.1f", mbps);
    Post(Cmd{ C_BW, js });
}

DWORD WINAPI BandwidthThread(LPVOID)
{
    HINTERNET s = WinHttpOpen(L"botox/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) { g_bwMbps = 0.f; return 0; }
    g_bwSession = s;
    WinHttpSetTimeouts(s, 4000, 4000, 4000, 8000);
    float mbps = 0.f;
    if (HINTERNET c = WinHttpConnect(s, L"speed.cloudflare.com", INTERNET_DEFAULT_HTTPS_PORT, 0)) {
        if (HINTERNET r = WinHttpOpenRequest(c, L"GET", L"/__down?bytes=4000000", nullptr, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)) {
            if (WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(r, nullptr)) {
                static char buf[64 * 1024];
                LARGE_INTEGER f, t0{}, t1{};
                QueryPerformanceFrequency(&f);
                size_t total = 0;
                DWORD got = 0;
                bool first = true;
                while (WinHttpReadData(r, buf, sizeof(buf), &got) && got) {
                    if (first) { QueryPerformanceCounter(&t0); first = false; continue; }
                    total += got;
                }
                QueryPerformanceCounter(&t1);
                const double sec = (double)(t1.QuadPart - t0.QuadPart) / (double)f.QuadPart;
                if (total > 1000000 && sec > 0.02) mbps = (float)((double)total * 8.0 / sec / 1e6);
            }
            WinHttpCloseHandle(r);
        }
        WinHttpCloseHandle(c);
    }
    HINTERNET mine = s;
    if (!g_bwSession.compare_exchange_strong(mine, nullptr)) return 0;
    WinHttpCloseHandle(s);
    g_bwMbps = mbps;
    PostBandwidth();
    return 0;
}

}

namespace wv2 {

bool RuntimeVersion(char* out, size_t cap)
{
    LPWSTR v = nullptr;
    if (!WvLoader()) return false;
    if (FAILED(g_pfnGetVersion(nullptr, &v)) || !v) return false;
    const std::string s = Take(v);
    if (out && cap) snprintf(out, cap, "%s", s.c_str());
    return !s.empty();
}

bool Start(int w, int h, FrameSink sink, NavFilter filter, const char* extraArgs)
{
    if (g_running) return true;
    g_sink = sink; g_filter = filter;
    g_startW = w; g_startH = h;
    g_args = "--disable-features=CalculateNativeWinOcclusion "
             "--disable-backgrounding-occluded-windows --disable-renderer-backgrounding "
             "--disable-background-timer-throttling --autoplay-policy=no-user-gesture-required "
             "--disk-cache-size=33554432";
    if (extraArgs && *extraArgs) { g_args += ' '; g_args += extraArgs; }
    { std::lock_guard<std::mutex> lk(g_qLock); g_cmds.clear(); g_events.clear(); g_diagNote.clear(); }
    g_frames = 0; g_capW = 0; g_capH = 0; g_ready = false;
    g_thread = CreateThread(nullptr, 0, EngineThread, nullptr, 0, nullptr);
    g_running = g_thread != nullptr;
    if (g_running) {
        if (g_bwMbps.load() < 0.f && !g_bwThread) g_bwThread = CreateThread(nullptr, 0, BandwidthThread, nullptr, 0, nullptr);
        else PostBandwidth();
    }
    return g_running;
}

void Stop()
{
    if (!g_running) return;
    g_running = false;
    for (int i = 0; i < 100 && !g_hwnd.load(); ++i) {
        if (WaitForSingleObject(g_thread, 20) == WAIT_OBJECT_0) break;
    }
    if (HWND h = g_hwnd.load()) PostMessageW(h, WM_WV2_QUIT, 0, 0);
    if (WaitForSingleObject(g_thread, 5000) != WAIT_OBJECT_0)
        Note("engine thread did not exit in 5s");
    CloseHandle(g_thread);
    g_thread = nullptr;
    if (g_bwThread) {
        if (HINTERNET s = g_bwSession.exchange(nullptr)) WinHttpCloseHandle(s);
        WaitForSingleObject(g_bwThread, 3000);
        CloseHandle(g_bwThread);
        g_bwThread = nullptr;
    }
}

// botox: game quit with the browser up. The engine thread is already killed, so the static dtors
// would Release out-of-proc proxies over RPC after the thread pool is gone. Leak them instead.
void AbandonForExit()
{
    s_wv.Detach(); s_ctl.Detach(); s_env.Detach();
    s_stage.Detach(); s_ctx.Detach(); s_dev.Detach();
    winrt::detach_abi(s_session); winrt::detach_abi(s_pool); winrt::detach_abi(s_item); winrt::detach_abi(s_rtDev);
}

void SetCaptureEnabled(bool on)    { if (s_capOn.exchange(on) != on) Post(Cmd{ C_MEM, {}, on ? 0 : 1 }); }

void Navigate(const char* url)     { if (url && *url) Post(Cmd{ C_NAV, url }); }
void ExecJs(const char* js)        { if (js && *js)   Post(Cmd{ C_JS, js }); }
void AddStartScript(const char* js){ if (js && *js)   Post(Cmd{ C_START_JS, js }); }
void Resize(int w, int h)          { Post(Cmd{ C_SIZE, {}, w, h }); }
void MouseInput(Mouse m, int x, int y, int wheelDelta) { Post(Cmd{ C_MOUSE, {}, (int)m, x, y, wheelDelta }); }
void KeyInput(Key k, uint32_t code, int mods) { Post(Cmd{ C_KEY, {}, (int)k, (int)code, mods }); }
void Back()                        { Post(Cmd{ C_BACK }); }
void Forward()                     { Post(Cmd{ C_FWD }); }
void Reload()                      { Post(Cmd{ C_RELOAD }); }
void ClearData()                   { Post(Cmd{ C_CLEAR }); }

bool PollEvent(Event& out)
{
    std::lock_guard<std::mutex> lk(g_qLock);
    if (g_events.empty()) return false;
    out = std::move(g_events.front());
    g_events.pop_front();
    return true;
}

std::string Diag()
{
    char b[256];
    std::string note;
    { std::lock_guard<std::mutex> lk(g_qLock); note = g_diagNote; }
    snprintf(b, sizeof(b), "webview2 %s frames=%ld cap=%dx%d net=%.1fMbps%s%s",
             g_ready ? "ready" : (g_running ? "starting" : "off"), g_frames.load(), g_capW.load(), g_capH.load(),
             g_bwMbps.load(),
             note.empty() ? "" : " | ", note.c_str());
    return b;
}

}
