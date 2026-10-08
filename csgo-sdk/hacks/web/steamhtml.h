#pragma once
#include <cstdint>

typedef uint32_t HHTMLBrowser;
typedef uint64_t SteamAPICall_t;
typedef int32_t  HSteamUser;
typedef uint32_t RTime32;

const uint32_t INVALID_HTMLBROWSER = 0;
#include "../../dependencies/steam/isteamclient.h"

#define STEAMHTMLSURFACE_INTERFACE_VERSION "STEAMHTMLSURFACE_INTERFACE_VERSION_005"

class ISteamHTMLSurface
{
public:
    virtual ~ISteamHTMLSurface() {}

    virtual bool Init() = 0;
    virtual bool Shutdown() = 0;

    virtual SteamAPICall_t CreateBrowser(const char* pchUserAgent, const char* pchUserCSS) = 0;
    virtual void RemoveBrowser(HHTMLBrowser unBrowserHandle) = 0;
    virtual void LoadURL(HHTMLBrowser unBrowserHandle, const char* pchURL, const char* pchPostData) = 0;
    virtual void SetSize(HHTMLBrowser unBrowserHandle, uint32_t unWidth, uint32_t unHeight) = 0;
    virtual void StopLoad(HHTMLBrowser unBrowserHandle) = 0;
    virtual void Reload(HHTMLBrowser unBrowserHandle) = 0;
    virtual void GoBack(HHTMLBrowser unBrowserHandle) = 0;
    virtual void GoForward(HHTMLBrowser unBrowserHandle) = 0;
    virtual void AddHeader(HHTMLBrowser unBrowserHandle, const char* pchKey, const char* pchValue) = 0;
    virtual void ExecuteJavascript(HHTMLBrowser unBrowserHandle, const char* pchScript) = 0;

    enum EHTMLMouseButton
    {
        eHTMLMouseButton_Left   = 0,
        eHTMLMouseButton_Right  = 1,
        eHTMLMouseButton_Middle = 2,
    };

    virtual void MouseUp(HHTMLBrowser unBrowserHandle, EHTMLMouseButton eMouseButton) = 0;
    virtual void MouseDown(HHTMLBrowser unBrowserHandle, EHTMLMouseButton eMouseButton) = 0;
    virtual void MouseDoubleClick(HHTMLBrowser unBrowserHandle, EHTMLMouseButton eMouseButton) = 0;
    virtual void MouseMove(HHTMLBrowser unBrowserHandle, int x, int y) = 0;
    virtual void MouseWheel(HHTMLBrowser unBrowserHandle, int32_t nDelta) = 0;

    enum EHTMLKeyModifiers
    {
        k_eHTMLKeyModifier_None      = 0,
        k_eHTMLKeyModifier_AltDown   = 1 << 0,
        k_eHTMLKeyModifier_CtrlDown  = 1 << 1,
        k_eHTMLKeyModifier_ShiftDown = 1 << 2,
    };

    virtual void KeyDown(HHTMLBrowser unBrowserHandle, uint32_t nNativeKeyCode, EHTMLKeyModifiers eHTMLKeyModifiers, bool bIsSystemKey = false) = 0;
    virtual void KeyUp(HHTMLBrowser unBrowserHandle, uint32_t nNativeKeyCode, EHTMLKeyModifiers eHTMLKeyModifiers) = 0;
    virtual void KeyChar(HHTMLBrowser unBrowserHandle, uint32_t cUnicodeChar, EHTMLKeyModifiers eHTMLKeyModifiers) = 0;
    virtual void SetHorizontalScroll(HHTMLBrowser unBrowserHandle, uint32_t nAbsolutePixelScroll) = 0;
    virtual void SetVerticalScroll(HHTMLBrowser unBrowserHandle, uint32_t nAbsolutePixelScroll) = 0;
    virtual void SetKeyFocus(HHTMLBrowser unBrowserHandle, bool bHasKeyFocus) = 0;
    virtual void ViewSource(HHTMLBrowser unBrowserHandle) = 0;
    virtual void CopyToClipboard(HHTMLBrowser unBrowserHandle) = 0;
    virtual void PasteFromClipboard(HHTMLBrowser unBrowserHandle) = 0;
    virtual void Find(HHTMLBrowser unBrowserHandle, const char* pchSearchStr, bool bCurrentlyInFind, bool bReverse) = 0;
    virtual void StopFind(HHTMLBrowser unBrowserHandle) = 0;
    virtual void GetLinkAtPosition(HHTMLBrowser unBrowserHandle, int x, int y) = 0;
    virtual void SetCookie(const char* pchHostname, const char* pchKey, const char* pchValue,
                           const char* pchPath = "/", RTime32 nExpires = 0,
                           bool bSecure = false, bool bHTTPOnly = false) = 0;
    virtual void SetPageScaleFactor(HHTMLBrowser unBrowserHandle, float flZoom, int nPointX, int nPointY) = 0;
    virtual void SetBackgroundMode(HHTMLBrowser unBrowserHandle, bool bBackgroundMode) = 0;
    virtual void SetDPIScalingFactor(HHTMLBrowser unBrowserHandle, float flDPIScaling) = 0;
    virtual void OpenDeveloperTools(HHTMLBrowser unBrowserHandle) = 0;
    virtual void AllowStartRequest(HHTMLBrowser unBrowserHandle, bool bAllowed) = 0;
    virtual void JSDialogResponse(HHTMLBrowser unBrowserHandle, bool bResult) = 0;
    virtual void FileLoadDialogResponse(HHTMLBrowser unBrowserHandle, const char** pchSelectedFiles) = 0;
};

#pragma pack(push, 8)

struct HTML_BrowserReady_t {
    enum { k_iCallback = k_iSteamHTMLSurfaceCallbacks + 1 };
    HHTMLBrowser unBrowserHandle;
};

struct HTML_NeedsPaint_t {
    enum { k_iCallback = k_iSteamHTMLSurfaceCallbacks + 2 };
    HHTMLBrowser unBrowserHandle;
    const char*  pBGRA;
    uint32_t unWide, unTall;
    uint32_t unUpdateX, unUpdateY, unUpdateWide, unUpdateTall;
    uint32_t unScrollX, unScrollY;
    float    flPageScale;
    uint32_t unPageSerial;
};

struct HTML_StartRequest_t {
    enum { k_iCallback = k_iSteamHTMLSurfaceCallbacks + 3 };
    HHTMLBrowser unBrowserHandle;
    const char* pchURL;
    const char* pchTarget;
    const char* pchPostData;
    bool bIsRedirect;
};

struct HTML_ChangedTitle_t {
    enum { k_iCallback = k_iSteamHTMLSurfaceCallbacks + 8 };
    HHTMLBrowser unBrowserHandle;
    const char* pchTitle;
};

struct HTML_CloseBrowser_t {
    enum { k_iCallback = k_iSteamHTMLSurfaceCallbacks + 4 };
    HHTMLBrowser unBrowserHandle;
};

struct HTML_URLChanged_t {
    enum { k_iCallback = k_iSteamHTMLSurfaceCallbacks + 5 };
    HHTMLBrowser unBrowserHandle;
    const char* pchURL;
    const char* pchPostData;
    bool bIsRedirect;
    const char* pchPageTitle;
    bool bNewNavigation;
};

struct HTML_FinishedRequest_t {
    enum { k_iCallback = k_iSteamHTMLSurfaceCallbacks + 6 };
    HHTMLBrowser unBrowserHandle;
    const char* pchURL;
    const char* pchPageTitle;
};

struct HTML_JSAlert_t {
    enum { k_iCallback = k_iSteamHTMLSurfaceCallbacks + 14 };
    HHTMLBrowser unBrowserHandle;
    const char* pchMessage;
};

struct HTML_JSConfirm_t {
    enum { k_iCallback = k_iSteamHTMLSurfaceCallbacks + 15 };
    HHTMLBrowser unBrowserHandle;
    const char* pchMessage;
};

struct HTML_FileOpenDialog_t {
    enum { k_iCallback = k_iSteamHTMLSurfaceCallbacks + 16 };
    HHTMLBrowser unBrowserHandle;
    const char* pchTitle;
    const char* pchInitialFile;
};

struct HTML_NewWindow_t {
    enum { k_iCallback = k_iSteamHTMLSurfaceCallbacks + 21 };
    HHTMLBrowser unBrowserHandle;
    const char* pchURL;
    uint32_t unX, unY, unWide, unTall;
    HHTMLBrowser unNewWindow_BrowserHandle;
};

struct HTML_VerticalScroll_t {
    enum { k_iCallback = k_iSteamHTMLSurfaceCallbacks + 12 };
    HHTMLBrowser unBrowserHandle;
    uint32_t unScrollMax;
    uint32_t unScrollCurrent;
    float    flPageScale;
    bool     bVisible;
    uint32_t unPageSize;
};

#pragma pack(pop)

// valve's exact layout: steam calls through this vtable
class CCallbackBase
{
public:
    CCallbackBase() { m_nCallbackFlags = 0; m_iCallback = 0; }
    virtual void Run(void* pvParam) = 0;
    virtual void Run(void* pvParam, bool bIOFailure, SteamAPICall_t hSteamAPICall) = 0;
    int GetICallback() { return m_iCallback; }
    virtual int GetCallbackSizeBytes() = 0;

protected:
    enum { k_ECallbackFlagsRegistered = 0x01, k_ECallbackFlagsGameServer = 0x02 };
    uint8_t m_nCallbackFlags;
    int     m_iCallback;

private:
    CCallbackBase(const CCallbackBase&);
    CCallbackBase& operator=(const CCallbackBase&);
};

template <typename P>
class WsCallResult : public CCallbackBase
{
public:
    using Fn = void (*)(P*);
    explicit WsCallResult(Fn fn) : m_fn(fn) { m_iCallback = P::k_iCallback; }
    void Run(void* pvParam) override { if (m_fn && pvParam) m_fn(static_cast<P*>(pvParam)); }
    void Run(void* pvParam, bool bIOFailure, SteamAPICall_t) override
    {
        if (!bIOFailure) Run(pvParam);
    }
    int GetCallbackSizeBytes() override { return sizeof(P); }
private:
    Fn m_fn;
};

template <typename P>
class WsCallback : public CCallbackBase
{
public:
    using Fn = void (*)(P*);
    explicit WsCallback(Fn fn) : m_fn(fn) { m_iCallback = P::k_iCallback; }
    void Run(void* pvParam) override { if (m_fn && pvParam) m_fn(static_cast<P*>(pvParam)); }
    void Run(void* pvParam, bool, SteamAPICall_t) override { Run(pvParam); }
    int  GetCallbackSizeBytes() override { return sizeof(P); }
private:
    Fn m_fn;
};
