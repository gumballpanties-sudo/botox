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

enum CmdOp { C_NAV, C_JS, C_START_JS, C_SIZE, C_MOUSE, C_KEY, C_BACK, C_FWD, C_RELOAD, C_MUTE, C_CLEAR, C_BW, C_MEM, C_JSR };
struct Cmd { CmdOp op; std::string s; int a = 0, b = 0, c = 0, d = 0; };

constexpr UINT WM_WV2_CMD  = WM_APP + 0x57;
constexpr UINT WM_WV2_QUIT = WM_APP + 0x58;

// botox: one per browser, slot 0 = own screen, 1..k_peers = botox network screens
struct Inst {
    int                    slot = 0;
    std::mutex             qLock;
    std::deque<Cmd>        cmds;
    std::deque<wv2::Event> events;
    std::string            diagNote;

    HANDLE              thread = nullptr;
    std::atomic<bool>   running{ false };
    std::atomic<HWND>   hwnd{ nullptr };
    std::atomic<bool>   ready{ false };
    std::atomic<long>   frames{ 0 };
    std::atomic<int>    capW{ 0 }, capH{ 0 };
    wv2::FrameSink      sink     = nullptr;
    wv2::PeerSink       peerSink = nullptr;
    wv2::NavFilter      filter   = nullptr;
    std::string         args;
    int                 startW = 1280, startH = 720;

    ComPtr<ICoreWebView2Environment> env;
    ComPtr<ICoreWebView2Controller>  ctl;
    ComPtr<ICoreWebView2>            wv;
    int  w = 0, h = 0;
    WPARAM btn = 0;
    DWORD lastPressMs = 0;
    int   lastPressX = 0, lastPressY = 0, clicks = 0;
    const char* lastPressBtn = "";

    std::mutex                                      capLock;
    ComPtr<ID3D11Device>                            dev;
    ComPtr<ID3D11DeviceContext>                     ctx;
    ComPtr<ID3D11Texture2D>                         stage;
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice rtDev{ nullptr };
    wgc::GraphicsCaptureItem                        item{ nullptr };
    wgc::Direct3D11CaptureFramePool                 pool{ nullptr };
    wgc::GraphicsCaptureSession                     session{ nullptr };
    winrt::Windows::Graphics::SizeInt32             poolSize{ 0, 0 };
    winrt::event_token                              frameTok{};
    std::atomic<bool>                               capStopping{ false };
    std::atomic<int>                                inFrame{ 0 };
    std::atomic<bool>                               capOn{ true };
    LONGLONG                                        nextFrame = 0;
};

Inst g_inst[1 + wv2::k_peers];
Inst* PeerInst(int slot) { return slot >= 1 && slot <= wv2::k_peers ? &g_inst[slot] : nullptr; }

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

void Emit(Inst& I, wv2::EvType t, std::string a = {}, std::string b = {}, int n = 0)
{
    std::lock_guard<std::mutex> lk(I.qLock);
    if (I.events.size() < 256) I.events.push_back(wv2::Event{ t, std::move(a), std::move(b), n });
}
void Note(Inst& I, const std::string& s) { std::lock_guard<std::mutex> lk(I.qLock); I.diagNote = s; }

void Post(Inst& I, Cmd c)
{
    {
        std::lock_guard<std::mutex> lk(I.qLock);
        // Mouse moves can flood while the engine is still starting; never let that grow unbounded.
        if (I.cmds.size() >= 512) return;
        I.cmds.push_back(std::move(c));
    }
    if (HWND h = I.hwnd.load()) PostMessageW(h, WM_WV2_CMD, 0, 0);
}

void OnFrameBody(Inst& I, const wgc::Direct3D11CaptureFramePool& pool);

void OnFrame(Inst& I, const wgc::Direct3D11CaptureFramePool& pool)
{
    I.inFrame.fetch_add(1);
    if (!I.capStopping) { try { OnFrameBody(I, pool); } catch (...) {} }
    I.inFrame.fetch_sub(1);
}

bool FrameDue(Inst& I)
{
    static LARGE_INTEGER s_freq{};
    if (!s_freq.QuadPart) QueryPerformanceFrequency(&s_freq);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    if (t.QuadPart < I.nextFrame) return false;
    I.nextFrame = t.QuadPart + s_freq.QuadPart / k_capMaxFps;   // from now, so a late frame never bursts
    return true;
}

void CapCopy(Inst& I, const wgc::Direct3D11CaptureFrame& frame, const winrt::Windows::Graphics::SizeInt32& size)
{
    auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    ComPtr<ID3D11Texture2D> tex;
    if (FAILED(access->GetInterface(IID_PPV_ARGS(&tex))) || !tex) return;

    D3D11_TEXTURE2D_DESC td{};
    tex->GetDesc(&td);
    const int w = (int)(std::min)((UINT)size.Width,  td.Width);
    const int h = (int)(std::min)((UINT)size.Height, td.Height);
    if (w <= 0 || h <= 0) return;

    std::lock_guard<std::mutex> lk(I.capLock);
    if (!I.ctx) return;
    D3D11_TEXTURE2D_DESC sd{};
    if (I.stage) I.stage->GetDesc(&sd);
    if (!I.stage || sd.Width != td.Width || sd.Height != td.Height) {
        I.stage.Reset();
        D3D11_TEXTURE2D_DESC nd = td;
        nd.Usage = D3D11_USAGE_STAGING; nd.BindFlags = 0; nd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        nd.MiscFlags = 0; nd.MipLevels = 1; nd.ArraySize = 1; nd.SampleDesc = { 1, 0 };
        if (FAILED(I.dev->CreateTexture2D(&nd, nullptr, &I.stage))) return;
    }
    I.ctx->CopyResource(I.stage.Get(), tex.Get());
    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(I.ctx->Map(I.stage.Get(), 0, D3D11_MAP_READ, 0, &m))) {
        if (I.sink)          I.sink((const uint8_t*)m.pData, w, h, (int)m.RowPitch);
        else if (I.peerSink) I.peerSink(I.slot, (const uint8_t*)m.pData, w, h, (int)m.RowPitch);
        I.ctx->Unmap(I.stage.Get(), 0);
        I.frames.fetch_add(1);
        I.capW = w; I.capH = h;
    }
}

void OnFrameBody(Inst& I, const wgc::Direct3D11CaptureFramePool& pool)
{
    auto frame = pool.TryGetNextFrame();
    if (!frame) return;
    const auto size = frame.ContentSize();
    const bool resized = size.Width != I.poolSize.Width || size.Height != I.poolSize.Height;

    if ((I.capOn.load() && FrameDue(I)) || resized) CapCopy(I, frame, size);

    if (resized) {
        frame.Close();
        I.poolSize = size;
        try { pool.Recreate(I.rtDev, wgdx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size); } catch (...) {}
    }
}

bool CapStart(Inst& I, HWND hwnd)
{
    try {
        using WinRtDev_t = HRESULT(WINAPI*)(IDXGIDevice*, ::IInspectable**);
        static HMODULE s_d3d11 = LoadLibraryW(L"d3d11.dll");
        const auto D3D11CreateDevice = s_d3d11 ? (PFN_D3D11_CREATE_DEVICE)GetProcAddress(s_d3d11, "D3D11CreateDevice") : nullptr;
        const auto CreateDirect3D11DeviceFromDXGIDevice =
            s_d3d11 ? (WinRtDev_t)GetProcAddress(s_d3d11, "CreateDirect3D11DeviceFromDXGIDevice") : nullptr;
        if (!D3D11CreateDevice || !CreateDirect3D11DeviceFromDXGIDevice) { Note(I, "capture: d3d11.dll exports missing"); return false; }

        D3D_FEATURE_LEVEL fl;
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                       nullptr, 0, D3D11_SDK_VERSION, &I.dev, &fl, &I.ctx);
        if (FAILED(hr))
            hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                   nullptr, 0, D3D11_SDK_VERSION, &I.dev, &fl, &I.ctx);
        if (FAILED(hr)) { Note(I, "capture: no D3D11 device"); return false; }

        ComPtr<IDXGIDevice> dxgi;
        I.dev.As(&dxgi);
        winrt::com_ptr<::IInspectable> insp;
        if (FAILED(CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(), insp.put()))) { Note(I, "capture: no WinRT device"); return false; }
        I.rtDev = insp.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();

        auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        if (FAILED(interop->CreateForWindow(hwnd, winrt::guid_of<wgc::GraphicsCaptureItem>(), winrt::put_abi(I.item))) || !I.item) {
            Note(I, "capture: CreateForWindow failed"); return false;
        }
        I.poolSize = I.item.Size();
        I.pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
            I.rtDev, wgdx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, I.poolSize);
        I.capStopping = false;
        Inst* self = &I;
        I.frameTok = I.pool.FrameArrived([self](const wgc::Direct3D11CaptureFramePool& pool, const winrt::Windows::Foundation::IInspectable&) {
            OnFrame(*self, pool);
        });
        I.session = I.pool.CreateCaptureSession(I.item);
        try { I.session.IsCursorCaptureEnabled(false); } catch (...) {}
        try { I.session.IsBorderRequired(false); }      catch (...) {}
        I.session.StartCapture();
        return true;
    } catch (const winrt::hresult_error& e) {
        Note(I, "capture: " + Utf8(e.message().c_str()));
        return false;
    } catch (...) {
        Note(I, "capture: exception");
        return false;
    }
}

void CapStop(Inst& I)
{
    I.capStopping = true;
    try { if (I.pool && I.frameTok.value) I.pool.FrameArrived(I.frameTok); } catch (...) {}
    I.frameTok = {};
    try { if (I.session) I.session.Close(); } catch (...) {}
    try { if (I.pool)    I.pool.Close();    } catch (...) {}
    for (int i = 0; i < 200 && I.inFrame.load() > 0; ++i) Sleep(5);
    std::lock_guard<std::mutex> lk(I.capLock);
    I.session = nullptr; I.pool = nullptr; I.item = nullptr; I.rtDev = nullptr;
    I.stage.Reset(); I.ctx.Reset(); I.dev.Reset();
}

void EnsureFocus(Inst& I)
{
    HWND host = I.hwnd.load();
    HWND f = GetFocus();
    if (I.ctl && !(f && f != host && IsChild(host, f)))
        I.ctl->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
}

void Cdp(Inst& I, const wchar_t* method, const std::string& json);

void DoMouse(Inst& I, const Cmd& c)
{
    const int x = c.b, y = c.c;
    const char* btn = "none";
    const char* type = "mouseMoved";
    switch ((wv2::Mouse)c.a) {
    case wv2::M_MOVE:  break;
    case wv2::M_LDOWN: EnsureFocus(I); I.btn |= MK_LBUTTON; type = "mousePressed";  btn = "left";  break;
    case wv2::M_LUP:   I.btn &= ~(WPARAM)MK_LBUTTON;        type = "mouseReleased"; btn = "left";  break;
    case wv2::M_RDOWN: EnsureFocus(I); I.btn |= MK_RBUTTON; type = "mousePressed";  btn = "right"; break;
    case wv2::M_RUP:   I.btn &= ~(WPARAM)MK_RBUTTON;        type = "mouseReleased"; btn = "right"; break;
    case wv2::M_WHEEL:
        Cdp(I, L"Input.dispatchMouseEvent", "{\"type\":\"mouseWheel\",\"x\":" + std::to_string(x) + ",\"y\":" +
            std::to_string(y) + ",\"deltaX\":0,\"deltaY\":" + std::to_string(-c.d) + "}");
        return;
    }
    const int buttons = ((I.btn & MK_LBUTTON) ? 1 : 0) | ((I.btn & MK_RBUTTON) ? 2 : 0);
    if (!strcmp(type, "mouseMoved")) btn = (I.btn & MK_LBUTTON) ? "left" : ((I.btn & MK_RBUTTON) ? "right" : "none");
    if (!strcmp(type, "mousePressed")) {
        const DWORD now = GetTickCount();
        const bool again = !strcmp(btn, I.lastPressBtn) && now - I.lastPressMs <= GetDoubleClickTime() &&
                           abs(x - I.lastPressX) <= GetSystemMetrics(SM_CXDOUBLECLK) / 2 &&
                           abs(y - I.lastPressY) <= GetSystemMetrics(SM_CYDOUBLECLK) / 2;
        I.clicks = again ? I.clicks + 1 : 1;
        I.lastPressMs = now; I.lastPressX = x; I.lastPressY = y; I.lastPressBtn = btn;
    }
    const int clicks = strcmp(type, "mouseMoved") ? I.clicks : 0;
    Cdp(I, L"Input.dispatchMouseEvent", std::string("{\"type\":\"") + type + "\",\"x\":" + std::to_string(x) +
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

void Cdp(Inst& I, const wchar_t* method, const std::string& json)
{
    if (I.wv) I.wv->CallDevToolsProtocolMethod(method, Wide(json).c_str(), nullptr);
}
void Cdp(Inst& I, const std::string& json) { Cdp(I, L"Input.dispatchKeyEvent", json); }

void DoKey(Inst& I, const Cmd& c)
{
    EnsureFocus(I);
    const UINT code = (UINT)c.b;
    const int  mods = c.c;
    if (c.a == wv2::K_CHAR) {
        if (code < 0x20) return;
        wchar_t w[2] = { (wchar_t)code, 0 };
        const std::string t = Utf8(w);
        Cdp(I, "{\"type\":\"char\",\"text\":" + JsonStr(t) + ",\"unmodifiedText\":" + JsonStr(t) +
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
    Cdp(I, j + "}");
    if (c.a == wv2::K_DOWN && code == VK_RETURN)
        Cdp(I, "{\"type\":\"char\",\"text\":\"\\r\",\"unmodifiedText\":\"\\r\",\"key\":\"Enter\",\"code\":\"Enter\","
            "\"windowsVirtualKeyCode\":13,\"modifiers\":" + std::to_string(mods) + "}");
}

void DoResize(Inst& I, int w, int h)
{
    if (w < 16 || h < 16) return;
    I.w = w; I.h = h;
    HWND host = I.hwnd.load();
    SetWindowPos(host, nullptr, 0, 0, w, h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    if (I.ctl) I.ctl->put_Bounds(RECT{ 0, 0, w, h });
}

void MemLevel(Inst& I, bool low)
{
    ComPtr<ICoreWebView2_19> w19;
    if (I.wv && SUCCEEDED(I.wv.As(&w19)))
        w19->put_MemoryUsageTargetLevel(low ? COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL_LOW : COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL_NORMAL);
}

void DrainCmds(Inst& I)
{
    if (!I.ready) return;
    std::deque<Cmd> q;
    { std::lock_guard<std::mutex> lk(I.qLock); q.swap(I.cmds); }
    Inst* self = &I;
    for (const Cmd& c : q) {
        switch (c.op) {
        case C_NAV:
            if (FAILED(I.wv->Navigate(Wide(c.s).c_str()))) Emit(I, wv2::EV_FINISHED, c.s, "not a valid URL", 0);
            break;
        case C_JS:       I.wv->ExecuteScript(Wide(c.s).c_str(), nullptr); break;
        case C_JSR: {
            const int tag = c.a;
            I.wv->ExecuteScript(Wide(c.s).c_str(), Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
                [self, tag](HRESULT hr, LPCWSTR json) -> HRESULT {
                    if (SUCCEEDED(hr)) Emit(*self, wv2::EV_JS, Utf8(json), {}, tag);
                    return S_OK;
                }).Get());
            break;
        }
        case C_START_JS: I.wv->AddScriptToExecuteOnDocumentCreated(Wide(c.s).c_str(), nullptr); break;
        case C_SIZE:     DoResize(I, c.a, c.b); break;
        case C_MOUSE:    DoMouse(I, c); break;
        case C_KEY:      DoKey(I, c); break;
        case C_BW:
            I.wv->AddScriptToExecuteOnDocumentCreated(Wide(c.s).c_str(), nullptr);
            I.wv->ExecuteScript(Wide(c.s).c_str(), nullptr);
            break;
        case C_BACK:     I.wv->GoBack(); break;
        case C_FWD:      I.wv->GoForward(); break;
        case C_RELOAD:   I.wv->Reload(); break;
        case C_MEM:      MemLevel(I, c.a != 0); break;
        case C_MUTE: {
            ComPtr<ICoreWebView2_8> w8;
            if (SUCCEEDED(I.wv.As(&w8))) w8->put_IsMuted(c.a ? TRUE : FALSE);
            break;
        }
        case C_CLEAR: {
            ComPtr<ICoreWebView2_13> w13; ComPtr<ICoreWebView2Profile> prof; ComPtr<ICoreWebView2Profile2> prof2;
            if (SUCCEEDED(I.wv.As(&w13)) && SUCCEEDED(w13->get_Profile(&prof)) && SUCCEEDED(prof.As(&prof2))) {
                prof2->ClearBrowsingDataAll(Callback<ICoreWebView2ClearBrowsingDataCompletedHandler>(
                    [self](HRESULT hr) -> HRESULT { Emit(*self, wv2::EV_CLEARED, SUCCEEDED(hr) ? "ok" : "failed"); return S_OK; }).Get());
            } else {
                Emit(I, wv2::EV_CLEARED, "unsupported runtime");
            }
            break;
        }
        }
    }
}

void Fail(Inst& I, const char* what, HRESULT hr)
{
    char b[160];
    snprintf(b, sizeof(b), "%s (hr=0x%08lX)", what, (unsigned long)hr);
    Note(I, b);
    Emit(I, wv2::EV_FAILED, b);
}

std::string CurrentTitle(Inst& I)
{
    LPWSTR t = nullptr;
    if (I.wv && SUCCEEDED(I.wv->get_DocumentTitle(&t))) return Take(t);
    return {};
}
std::string CurrentUrl(Inst& I)
{
    LPWSTR u = nullptr;
    if (I.wv && SUCCEEDED(I.wv->get_Source(&u))) return Take(u);
    return {};
}

void Configure(Inst& I)
{
    Inst* self = &I;
    ComPtr<ICoreWebView2Settings> st;
    if (SUCCEEDED(I.wv->get_Settings(&st))) {
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
    I.wv->add_NavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>(
        [self](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* a) -> HRESULT {
            LPWSTR u = nullptr;
            a->get_Uri(&u);
            const std::string url = Take(u);
            if (self->filter && self->filter(url.c_str())) a->put_Cancel(TRUE);
            return S_OK;
        }).Get(), &tok);
    I.wv->add_SourceChanged(Callback<ICoreWebView2SourceChangedEventHandler>(
        [self](ICoreWebView2*, ICoreWebView2SourceChangedEventArgs*) -> HRESULT {
            Emit(*self, wv2::EV_URL, CurrentUrl(*self));
            return S_OK;
        }).Get(), &tok);
    I.wv->add_NavigationCompleted(Callback<ICoreWebView2NavigationCompletedEventHandler>(
        [self](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* a) -> HRESULT {
            BOOL ok = FALSE; a->get_IsSuccess(&ok);
            COREWEBVIEW2_WEB_ERROR_STATUS es = COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN; a->get_WebErrorStatus(&es);
            if (!ok && es == COREWEBVIEW2_WEB_ERROR_STATUS_OPERATION_CANCELED) return S_OK;
            Emit(*self, wv2::EV_FINISHED, CurrentUrl(*self), ok ? CurrentTitle(*self) : ("error " + std::to_string((int)es)), ok ? 1 : 0);
            return S_OK;
        }).Get(), &tok);
    I.wv->add_DocumentTitleChanged(Callback<ICoreWebView2DocumentTitleChangedEventHandler>(
        [self](ICoreWebView2*, IUnknown*) -> HRESULT { Emit(*self, wv2::EV_TITLE, CurrentTitle(*self)); return S_OK; }).Get(), &tok);
    I.wv->add_NewWindowRequested(Callback<ICoreWebView2NewWindowRequestedEventHandler>(
        [](ICoreWebView2* wv, ICoreWebView2NewWindowRequestedEventArgs* a) -> HRESULT {
            LPWSTR u = nullptr; a->get_Uri(&u);
            a->put_Handled(TRUE);
            if (u) { wv->Navigate(u); CoTaskMemFree(u); }
            return S_OK;
        }).Get(), &tok);
    I.wv->add_ScriptDialogOpening(Callback<ICoreWebView2ScriptDialogOpeningEventHandler>(
        [](ICoreWebView2*, ICoreWebView2ScriptDialogOpeningEventArgs* a) -> HRESULT { a->Accept(); return S_OK; }).Get(), &tok);
    I.wv->add_PermissionRequested(Callback<ICoreWebView2PermissionRequestedEventHandler>(
        [](ICoreWebView2*, ICoreWebView2PermissionRequestedEventArgs* a) -> HRESULT {
            a->put_State(COREWEBVIEW2_PERMISSION_STATE_DENY);
            return S_OK;
        }).Get(), &tok);
    ComPtr<ICoreWebView2_4> w4;
    if (SUCCEEDED(I.wv.As(&w4)))
        w4->add_DownloadStarting(Callback<ICoreWebView2DownloadStartingEventHandler>(
            [](ICoreWebView2*, ICoreWebView2DownloadStartingEventArgs* a) -> HRESULT { a->put_Cancel(TRUE); return S_OK; }).Get(), &tok);
    I.wv->add_ProcessFailed(Callback<ICoreWebView2ProcessFailedEventHandler>(
        [self](ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs* a) -> HRESULT {
            COREWEBVIEW2_PROCESS_FAILED_KIND k = COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED;
            a->get_ProcessFailedKind(&k);
            if (k == COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED) {
                Emit(*self, wv2::EV_CRASHED, "browser process exited");
            } else if (k == COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_EXITED ||
                       k == COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_UNRESPONSIVE) {
                if (self->wv) self->wv->Reload();
                Note(*self, "renderer died, reloaded");
            }
            return S_OK;
        }).Get(), &tok);
}

HRESULT OnController(Inst& I, HRESULT hr, ICoreWebView2Controller* ctl)
{
    if (FAILED(hr) || !ctl) { Fail(I, "CreateCoreWebView2Controller failed", hr); return S_OK; }
    I.ctl = ctl;
    I.ctl->get_CoreWebView2(&I.wv);
    if (!I.wv) { Fail(I, "controller has no CoreWebView2", E_FAIL); return S_OK; }

    ComPtr<ICoreWebView2Controller3> c3;
    if (SUCCEEDED(I.ctl.As(&c3))) {
        c3->put_ShouldDetectMonitorScaleChanges(FALSE);
        c3->put_RasterizationScale(1.0);
    }
    I.ctl->put_Bounds(RECT{ 0, 0, I.w, I.h });
    I.ctl->put_IsVisible(TRUE);
    Configure(I);
    MemLevel(I, !I.capOn.load());

    if (!CapStart(I, I.hwnd.load())) { Fail(I, "window capture unavailable", E_FAIL); return S_OK; }
    I.ready = true;
    Emit(I, wv2::EV_READY);
    DrainCmds(I);
    return S_OK;
}

LRESULT CALLBACK HostProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_WV2_CMD:
        if (Inst* I = (Inst*)GetWindowLongPtrW(h, GWLP_USERDATA)) DrainCmds(*I);
        return 0;
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

std::once_flag g_trimOnce;

// botox: call_once also holds every other engine thread until the trim is done
void TrimProfile(Inst& I)
{
    const std::wstring root = UserDataDir() + L"\\EBWebView\\";
    if (!DeleteFileW((root + L"lockfile").c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND) {
        Note(I, "profile in use, cache kept");
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

bool WvLoader(Inst& I)
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
        Note(I, "loader: WebView2Loader.dll did not load (err " + std::to_string(GetLastError()) + ")");
        return false;
    }
    return true;
}

std::mutex g_classLock;
int        g_classRefs = 0;
const wchar_t* const k_class = L"botox_wv2_host";

// botox: one class for every engine thread; unregistered with the last window, so a reinject never meets our old wndproc
void ClassAcquire(HINSTANCE inst)
{
    std::lock_guard<std::mutex> lk(g_classLock);
    if (g_classRefs++) return;
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc   = HostProc;
    wc.hInstance     = inst;
    wc.lpszClassName = k_class;
    RegisterClassExW(&wc);
}

void ClassRelease(HINSTANCE inst)
{
    std::lock_guard<std::mutex> lk(g_classLock);
    if (--g_classRefs == 0) UnregisterClassW(k_class, inst);
}

DWORD WINAPI EngineThread(LPVOID p)
{
    Inst& I = *(Inst*)p;
    Inst* self = &I;
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HRESULT coHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    const HINSTANCE hinst = GetModuleHandleW(nullptr);
    ClassAcquire(hinst);

    // Beyond the right edge of every monitor: never seen, never clicked, never in alt-tab
    // (tool window), never activated. DWM still composes it, which is all capture needs.
    const int x = GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN) + 64;
    const int y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    I.w = I.startW; I.h = I.startH;
    I.btn = 0;   // a restart mid-drag must not start with the button held
    HWND host = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, k_class, L"botox web",
                                WS_POPUP, x, y, I.w, I.h, nullptr, nullptr, hinst, nullptr);
    if (!host) {
        Fail(I, "CreateWindowEx failed", HRESULT_FROM_WIN32(GetLastError()));
    } else {
        SetWindowLongPtrW(host, GWLP_USERDATA, (LONG_PTR)self);
        ShowWindow(host, SW_SHOWNOACTIVATE);
        I.hwnd = host;
        if (!I.running) PostMessageW(host, WM_WV2_QUIT, 0, 0);

        std::call_once(g_trimOnce, [self] { TrimProfile(*self); });   // botox: off the game thread, before the browser locks the files
        auto opts = Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();
        opts->put_AdditionalBrowserArguments(Wide(I.args).c_str());
        const HRESULT hr = !WvLoader(I) ? E_NOINTERFACE :
            g_pfnCreateEnv(nullptr, UserDataDir().c_str(), opts.Get(),
            Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
                [self](HRESULT hr, ICoreWebView2Environment* env) -> HRESULT {
                    if (FAILED(hr) || !env) { Fail(*self, "WebView2 environment failed", hr); return S_OK; }
                    self->env = env;
                    const HRESULT h2 = env->CreateCoreWebView2Controller(self->hwnd.load(),
                        Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                            [self](HRESULT hr, ICoreWebView2Controller* ctl) -> HRESULT { return OnController(*self, hr, ctl); }).Get());
                    if (FAILED(h2)) Fail(*self, "CreateCoreWebView2Controller refused", h2);
                    return S_OK;
                }).Get());
        if (FAILED(hr)) Fail(I, "CreateCoreWebView2EnvironmentWithOptions refused", hr);

        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    I.ready = false;
    CapStop(I);
    if (I.ctl) I.ctl->Close();
    I.wv.Reset(); I.ctl.Reset(); I.env.Reset();
    if (host) DestroyWindow(host);
    I.hwnd = nullptr;
    ClassRelease(hinst);
    if (SUCCEEDED(coHr)) CoUninitialize();
    return 0;
}

std::atomic<float>     g_bwMbps{ -1.f };
std::atomic<HINTERNET> g_bwSession{ nullptr };
HANDLE                 g_bwThread = nullptr;

void PostBandwidth(Inst& I)
{
    const float mbps = g_bwMbps.load();
    if (mbps <= 0.f || !I.running) return;
    char js[64];
    snprintf(js, sizeof(js), "window.__fpsBw=%.1f", mbps);
    Post(I, Cmd{ C_BW, js });
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
    for (Inst& I : g_inst) PostBandwidth(I);
    return 0;
}

bool StartInst(Inst& I, int w, int h, const char* extraArgs)
{
    I.startW = w; I.startH = h;
    I.args = "--disable-features=CalculateNativeWinOcclusion "
             "--disable-backgrounding-occluded-windows --disable-renderer-backgrounding "
             "--disable-background-timer-throttling --autoplay-policy=no-user-gesture-required "
             "--disk-cache-size=33554432";
    if (extraArgs && *extraArgs) { I.args += ' '; I.args += extraArgs; }
    { std::lock_guard<std::mutex> lk(I.qLock); I.cmds.clear(); I.events.clear(); I.diagNote.clear(); }
    I.frames = 0; I.capW = 0; I.capH = 0; I.ready = false;
    I.running = true;   // before the thread: it posts itself quit when this is false
    I.thread = CreateThread(nullptr, 0, EngineThread, &I, 0, nullptr);
    I.running = I.thread != nullptr;
    return I.running;
}

void StopInst(Inst& I)
{
    if (!I.running) return;
    I.running = false;
    for (int i = 0; i < 100 && !I.hwnd.load(); ++i) {
        if (WaitForSingleObject(I.thread, 20) == WAIT_OBJECT_0) break;
    }
    if (HWND h = I.hwnd.load()) PostMessageW(h, WM_WV2_QUIT, 0, 0);
    if (WaitForSingleObject(I.thread, 5000) != WAIT_OBJECT_0)
        Note(I, "engine thread did not exit in 5s");
    CloseHandle(I.thread);
    I.thread = nullptr;
}

bool PollInst(Inst& I, wv2::Event& out)
{
    std::lock_guard<std::mutex> lk(I.qLock);
    if (I.events.empty()) return false;
    out = std::move(I.events.front());
    I.events.pop_front();
    return true;
}

void SetCapInst(Inst& I, bool on) { if (I.capOn.exchange(on) != on) Post(I, Cmd{ C_MEM, {}, on ? 0 : 1 }); }

}

namespace wv2 {

bool RuntimeVersion(char* out, size_t cap)
{
    LPWSTR v = nullptr;
    if (!WvLoader(g_inst[0])) return false;
    if (FAILED(g_pfnGetVersion(nullptr, &v)) || !v) return false;
    const std::string s = Take(v);
    if (out && cap) snprintf(out, cap, "%s", s.c_str());
    return !s.empty();
}

bool Start(int w, int h, FrameSink sink, NavFilter filter, const char* extraArgs)
{
    Inst& I = g_inst[0];
    if (I.running) return true;
    I.sink = sink; I.filter = filter;
    if (!StartInst(I, w, h, extraArgs)) return false;
    if (g_bwMbps.load() < 0.f && !g_bwThread) g_bwThread = CreateThread(nullptr, 0, BandwidthThread, nullptr, 0, nullptr);
    else PostBandwidth(I);
    return true;
}

void Stop()
{
    StopInst(g_inst[0]);
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
    for (Inst& I : g_inst) {
        I.wv.Detach(); I.ctl.Detach(); I.env.Detach();
        I.stage.Detach(); I.ctx.Detach(); I.dev.Detach();
        winrt::detach_abi(I.session); winrt::detach_abi(I.pool); winrt::detach_abi(I.item); winrt::detach_abi(I.rtDev);
    }
}

void SetCaptureEnabled(bool on)    { SetCapInst(g_inst[0], on); }

void Navigate(const char* url)     { if (url && *url) Post(g_inst[0], Cmd{ C_NAV, url }); }
void ExecJs(const char* js)        { if (js && *js)   Post(g_inst[0], Cmd{ C_JS, js }); }
void ExecJsResult(const char* js, int tag) { if (js && *js) Post(g_inst[0], Cmd{ C_JSR, js, tag }); }
void AddStartScript(const char* js){ if (js && *js)   Post(g_inst[0], Cmd{ C_START_JS, js }); }
void Resize(int w, int h)          { Post(g_inst[0], Cmd{ C_SIZE, {}, w, h }); }
void MouseInput(Mouse m, int x, int y, int wheelDelta) { Post(g_inst[0], Cmd{ C_MOUSE, {}, (int)m, x, y, wheelDelta }); }
void KeyInput(Key k, uint32_t code, int mods) { Post(g_inst[0], Cmd{ C_KEY, {}, (int)k, (int)code, mods }); }
void Back()                        { Post(g_inst[0], Cmd{ C_BACK }); }
void Forward()                     { Post(g_inst[0], Cmd{ C_FWD }); }
void Reload()                      { Post(g_inst[0], Cmd{ C_RELOAD }); }
void ClearData()                   { Post(g_inst[0], Cmd{ C_CLEAR }); }

bool PollEvent(Event& out)         { return PollInst(g_inst[0], out); }

std::string Diag()
{
    Inst& I = g_inst[0];
    char b[256];
    std::string note;
    { std::lock_guard<std::mutex> lk(I.qLock); note = I.diagNote; }
    snprintf(b, sizeof(b), "webview2 %s frames=%ld cap=%dx%d net=%.1fMbps%s%s",
             I.ready ? "ready" : (I.running ? "starting" : "off"), I.frames.load(), I.capW.load(), I.capH.load(),
             g_bwMbps.load(),
             note.empty() ? "" : " | ", note.c_str());
    return b;
}

bool PeerStart(int slot, int w, int h, PeerSink sink, NavFilter filter)
{
    Inst* I = PeerInst(slot);
    if (!I || !sink || !WvLoader(*I)) return false;   // loader on the caller's thread: engine threads never race its first load
    if (I->running) return true;
    I->slot = slot; I->sink = nullptr; I->peerSink = sink; I->filter = filter;
    if (!StartInst(*I, w, h, nullptr)) return false;
    PostBandwidth(*I);
    return true;
}

void PeerStop(int slot)                            { if (Inst* I = PeerInst(slot)) StopInst(*I); }
void PeerNavigate(int slot, const char* url)       { Inst* I = PeerInst(slot); if (I && url && *url) Post(*I, Cmd{ C_NAV, url }); }
void PeerExecJs(int slot, const char* js)          { Inst* I = PeerInst(slot); if (I && js && *js) Post(*I, Cmd{ C_JS, js }); }
void PeerAddStartScript(int slot, const char* js)  { Inst* I = PeerInst(slot); if (I && js && *js) Post(*I, Cmd{ C_START_JS, js }); }
void PeerResize(int slot, int w, int h)            { if (Inst* I = PeerInst(slot)) Post(*I, Cmd{ C_SIZE, {}, w, h }); }
bool PeerPollEvent(int slot, Event& out)           { Inst* I = PeerInst(slot); return I && PollInst(*I, out); }

}
