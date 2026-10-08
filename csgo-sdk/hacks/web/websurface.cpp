#include "websurface.h"
#include "websurf_math.h"
#include "websurf_js.h"
#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"
#include "steamhtml.h"
#include "webview2host.h"
#include "../visuals/screen/color_correction.h"
#include "../visuals/screen/render_queue.h"

#include <cstddef>
#include <string>
#include <vector>

void botox_dbg_log(const char* fmt, ...);
#define DebugLog botox_dbg_log

#define g_webEnable           GET_VARIABLE(g_variables.m_web, bool)
#define g_webPanel            GET_VARIABLE(g_variables.m_web_panel, bool)
#define g_webPanelOpacity     GET_VARIABLE(g_variables.m_web_panel_opacity, float)
#define g_webResH             GET_VARIABLE(g_variables.m_web_resolution, int)
#define g_webVolume           GET_VARIABLE(g_variables.m_web_volume, float)
#define g_webUrl              GET_VARIABLE(g_variables.m_web_url, std::string)
#define g_webPlaceKey         (GET_VARIABLE(g_variables.m_web_place_key, key_bind_t).m_key)
#define g_webClickKey         (GET_VARIABLE(g_variables.m_web_click_key, key_bind_t).m_key)
#define g_webRClickKey        (GET_VARIABLE(g_variables.m_web_right_click_key, key_bind_t).m_key)
#define g_webRemoveKey        (GET_VARIABLE(g_variables.m_web_remove_key, key_bind_t).m_key)
#define g_webScale            GET_VARIABLE(g_variables.m_web_scale, float)
#define g_webAspect           GET_VARIABLE(g_variables.m_web_aspect, int)
#define g_webWorldOpacity     GET_VARIABLE(g_variables.m_web_world_opacity, float)
#define g_webDepthMode        GET_VARIABLE(g_variables.m_web_depth_mode, int)
#define g_webBackMode         GET_VARIABLE(g_variables.m_web_back_mode, int)
#define g_webRound            GET_VARIABLE(g_variables.m_web_round, float)
#define g_webPanelAlways      GET_VARIABLE(g_variables.m_web_panel_always, bool)
#define g_webVolDistance      GET_VARIABLE(g_variables.m_web_volume_distance, bool)
#define g_webVolNear          GET_VARIABLE(g_variables.m_web_volume_near, float)
#define g_webVolFar           GET_VARIABLE(g_variables.m_web_volume_far, float)
#define g_webCrosshairPointer GET_VARIABLE(g_variables.m_web_crosshair_pointer, bool)
#define g_webClickOnly        GET_VARIABLE(g_variables.m_web_click_only, bool)
#define g_webScrollInput      GET_VARIABLE(g_variables.m_web_scroll_input, bool)
#define g_webBlockFire        GET_VARIABLE(g_variables.m_web_block_fire, bool)
#define g_webAutoPlay         GET_VARIABLE(g_variables.m_web_auto_play, bool)
#define g_webPlayFromStart    GET_VARIABLE(g_variables.m_web_play_from_start, bool)
#define g_webAutoRemove       GET_VARIABLE(g_variables.m_web_auto_remove, bool)
#define g_webPanelScale       1.f
#define g_webYtEmbed          true
#define g_webHideSteam        true
#define g_webAdblock          true
#define g_webSmooth           true

#define g_eject               (g_ctx.m_unloading.load(std::memory_order_relaxed))
#define g_showMenu            (g_menu.m_opened)
#define g_hwnd                (g_input.m_window)
#define g_d3dDev              (g_interfaces.m_direct_device)
#define g_colorCorrEnable     (g_color_correction.wants_pass())
#define g_menuFont            ((ImFont*)nullptr)

static bool  IsInGame()     { return g_interfaces.m_engine_client && g_interfaces.m_engine_client->is_in_game(); }
static bool  Menu_Visible() { return g_menu.m_opened || g_menu.m_anim_progress > 0.f; }
static float Menu_Alpha()   { return g_menu.m_anim_progress; }

using FindOrCreate_t = void* (*)(HSteamUser, const char*);
using GetHUser_t     = HSteamUser (*)();
using RegisterCb_t   = void (*)(CCallbackBase*, int);
using UnregisterCb_t = void (*)(CCallbackBase*);
using RegisterCr_t   = void (*)(CCallbackBase*, SteamAPICall_t);
using UnregisterCr_t = void (*)(CCallbackBase*, SteamAPICall_t);
using RunCallbacks_t = void (*)();

static ISteamHTMLSurface* s_surface     = nullptr;
static RegisterCb_t       s_pfnRegister = nullptr;
static UnregisterCb_t     s_pfnUnreg    = nullptr;
static RegisterCr_t       s_pfnRegCr    = nullptr;
static UnregisterCr_t     s_pfnUnregCr  = nullptr;
static RunCallbacks_t     s_pfnRun      = nullptr;
static SteamAPICall_t     s_createCall  = 0;
static volatile long      s_cbSeen      = 0;
static volatile long      s_paints      = 0;
static volatile long      s_draws       = 0;

static HHTMLBrowser s_browser   = INVALID_HTMLBROWSER;
static bool  s_steamTried       = false;
static bool  s_steamDead        = false;
static bool  s_created          = false;
static bool  s_registered       = false;
static DWORD s_createdAtMs      = 0;
static bool  s_pumpSelf         = false;   // the game never dispatched: pump SteamAPI_RunCallbacks ourselves
static char  s_status[128]      = "off";

static bool s_wv2      = false;
static bool s_wv2Tried = false;
static const HHTMLBrowser k_wv2Handle = 1;
static int  s_mouseX = 0, s_mouseY = 0;

static bool WebAlive()      { return s_browser != INVALID_HTMLBROWSER && (s_wv2 || s_surface); }
static bool WebHaveEngine() { return s_wv2 || s_surface; }
static std::string s_curUrl;      // the page on screen now (main thread)
static std::string s_resumeUrl;

static CRITICAL_SECTION s_lock;
static bool  s_lockInit = false;
static struct WsLockInit { WsLockInit() { InitializeCriticalSection(&s_lock); s_lockInit = true; } } s_lockInitOnce;

static std::vector<uint8_t> s_pixIn;
static std::vector<uint8_t> s_pixels;
static std::vector<uint8_t> s_pixOut;    // render-thread-owned: what the texture was built from
static int   s_pixW = 0, s_pixH = 0;
static bool  s_pixDirty = false;
static int   s_outW = 0, s_outH = 0;     // render thread: the size s_pixOut holds
static volatile bool s_pixWanted = true;
static volatile bool s_freePixels = false;   // main thread asked the render thread to let go

static IDirect3DTexture9* s_tex = nullptr;
static int  s_texW = 0, s_texH = 0;
static bool s_texDynamic = false;

// Producer thread. pitch is the source's row stride in bytes.
static void WebPublishPixels(const uint8_t* bgra, int w, int h, int pitch)
{
    if (!s_lockInit || !bgra || w <= 0 || h <= 0 || !s_pixWanted) return;

    const size_t row = (size_t)w * 4, bytes = row * (size_t)h;
    if (s_pixIn.size() != bytes) s_pixIn.resize(bytes);
    if (pitch == (int)row) memcpy(s_pixIn.data(), bgra, bytes);
    else for (int y = 0; y < h; ++y) memcpy(&s_pixIn[(size_t)y * row], bgra + (size_t)y * pitch, row);

    EnterCriticalSection(&s_lock);
    s_pixels.swap(s_pixIn);
    s_pixW = w; s_pixH = h;
    s_pixDirty = true;
    LeaveCriticalSection(&s_lock);
}

static void WebFreePixels()
{
    if (!s_lockInit) return;
    EnterCriticalSection(&s_lock);
    std::vector<uint8_t>().swap(s_pixels);
    s_pixW = s_pixH = 0;
    s_pixDirty = false;
    LeaveCriticalSection(&s_lock);
    std::vector<uint8_t>().swap(s_pixIn);
    s_freePixels = true;
}

// Eject: every buffer at once, on the render thread (Web_Destroy). Own function because
// Web_Destroy uses __try, which cannot share a frame with temporaries that unwind (C2712).
__declspec(noinline) static void WebFreeAllPixels()
{
    std::vector<uint8_t>().swap(s_pixels);
    std::vector<uint8_t>().swap(s_pixIn);
    std::vector<uint8_t>().swap(s_pixOut);
    s_pixW = s_pixH = s_outW = s_outH = 0;
    s_pixDirty = false;
}

static float  s_vp[16]      = {};
static bool   s_vpValid     = false;
static WsVec3 s_viewOrigin, s_viewFwd;
static bool   s_viewValid   = false;
static DWORD  s_presentTid  = 0;

static bool   s_placed = false;
static WsVec3 s_pos, s_normal, s_right, s_up;
static bool   s_rebake = true;

struct WsVert { float x, y, z; DWORD col; float u, v; };
inline constexpr int k_wsCornerSeg = 8;
static const DWORD k_wsFvf = D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1;

static std::vector<WsVert>  s_verts;
static std::vector<WsVert>  s_vertsBack;
static std::vector<uint16_t> s_idx;
static unsigned s_meshEpoch = 0;   // bumped by every bake, so the render thread's copy is dropped

static CRITICAL_SECTION s_meshLock;
static struct WsMeshLockInit { WsMeshLockInit() { InitializeCriticalSection(&s_meshLock); } } s_meshLockOnce;

// Placement traces the collision world, which belongs to the main thread. The menu button and
// the bind both just ask for it here; the post slot does it.
static volatile bool s_placeRequested = false;

static volatile float s_distAtten = 1.f;
static bool s_placeDown = false;
static bool s_mouseDownWorld = false;   // the crosshair's button, main thread
static bool s_mouseDownPanel = false;   // the panel's button, render thread
static volatile bool  s_crossInside = false;   // crosshair on the placed screen, main thread
static volatile float s_crossU = 0.5f, s_crossV = 0.5f;

enum WsOp { WS_NAV, WS_BACK, WS_FWD, WS_RELOAD, WS_MMOVE, WS_MDOWN, WS_MUP, WS_RDOWN, WS_RUP,
            WS_WHEEL, WS_KEYCHAR, WS_KEYDOWN, WS_KEYUP, WS_SIZE, WS_VOL, WS_CLEARCACHE, WS_PAUSE, WS_CLEARMEM };
struct WsCmd { int op; int a; int b; std::string s; };
static CRITICAL_SECTION s_cmdLock;
static struct WsCmdLockInit { WsCmdLockInit() { InitializeCriticalSection(&s_cmdLock); } } s_cmdLockOnce;
static std::vector<WsCmd> s_cmds;

static void WebPost(int op, int a = 0, int b = 0, const char* str = nullptr)
{
    EnterCriticalSection(&s_cmdLock);
    // A stalled main thread must not let a mouse-move flood grow without bound.
    if (s_cmds.size() < 512) s_cmds.push_back(WsCmd{ op, a, b, str ? std::string(str) : std::string() });
    LeaveCriticalSection(&s_cmdLock);
}

static void WebRes(int& w, int& h)           { WsAspectRes(g_webAspect, g_webResH, w, h); }
static void WebWorldDims(float& w, float& h) { WsAspectDims(g_webAspect, g_webScale, w, h); }

// Keys are read with GetAsyncKeyState, which sees the keyboard whatever window has it. Alt-tabbed
// to a browser or Discord, typing must not place, click or type into the page.
static bool WebGameFocused() { return g_hwnd && GetForegroundWindow() == g_hwnd; }

static void WebSetStatus(const char* s) { strncpy_s(s_status, s, _TRUNCATE); }

static const char* k_adCss =
    "ins.adsbygoogle,.adsbygoogle,[id^='google_ads'],[id^='div-gpt-ad'],iframe[src*='doubleclick'],"
    "iframe[src*='googlesyndication'],iframe[src*='adservice'],.ad-container,.ad-banner,.advertisement,"
    "[class*='sponsored-ad'],[data-ad-slot],ytd-promoted-video-renderer,ytd-display-ad-renderer,"
    "ytd-ad-slot-renderer,.ytp-ad-overlay-container,#player-ads,#masthead-ad,"
    "ytd-banner-promo-renderer,ytd-in-feed-ad-layout-renderer,ytd-companion-slot-renderer,"
    "ytd-statement-banner-renderer,ytd-merch-shelf-renderer,ytd-rich-item-renderer:has(ytd-ad-slot-renderer),"
    "ytd-rich-section-renderer:has(ytd-statement-banner-renderer),ytm-promoted-sparkles-web-renderer"
    "{display:none!important;visibility:hidden!important;height:0!important}";

static const char* k_adJs = R"JS((function(){try{
if(window.__fpsAd)return;window.__fpsAd=1;
var YT=/(^|\.)youtube(-nocookie)?\.com$/.test(location.hostname);
if(YT){
  var AD=['adPlacements','playerAds','adSlots'];
  var prune=function(o){try{
    if(!o||typeof o!=='object')return o;
    var roots=[o,o.playerResponse];
    if(Array.isArray(o))o.forEach(function(x){if(x)roots.push(x,x.playerResponse);});
    roots.forEach(function(r){if(r&&typeof r==='object')AD.forEach(function(k){if(k in r)delete r[k];});});
    if(Array.isArray(o.entries))o.entries=o.entries.filter(function(e){try{return !e.command.reelWatchEndpoint.adClientParams.isAd;}catch(x){return true;}});
  }catch(e){}return o;};
  var hit=function(u){return /\/youtubei\/v1\/(player|next|get_watch|reel\/reel_watch_sequence)|\/playlist\?/.test(String(u||''));};
  var scrub=function(t){return typeof t==='string'?t.replace(/"adPlacements"/g,'"no_ads"').replace(/"adSlots"/g,'"no_ads"').replace(/"playerAds"/g,'"no_ads"'):t;};
  var jp=JSON.parse;JSON.parse=function(){return prune(jp.apply(this,arguments));};
  var f=window.fetch;
  if(f)window.fetch=function(input){
    var u=input&&input.url?input.url:input;
    var p=f.apply(this,arguments);
    if(!hit(u))return p;
    return p.then(function(r){return r.clone().text().then(function(t){
      return new Response(scrub(t),{status:r.status,statusText:r.statusText,headers:r.headers});
    },function(){return r;});});
  };
  var X=XMLHttpRequest.prototype,xo=X.open;
  X.open=function(m,u){this.__fpsU=u;return xo.apply(this,arguments);};
  ['responseText','response'].forEach(function(k){
    var d=Object.getOwnPropertyDescriptor(X,k);if(!d||!d.get)return;
    Object.defineProperty(X,k,{configurable:true,enumerable:d.enumerable,get:function(){
      var v=d.get.call(this);if(!hit(this.__fpsU))return v;
      if(typeof v==='string')return scrub(v);
      return prune(v);}});
  });
}
var css=document.createElement('style');css.id='__fpsAdCss';
css.textContent="%CSS%";
(document.head||document.documentElement).appendChild(css);
var sped=false;
// The sweep is the YouTube ad player's business and runs only there. Everywhere else it was
// four querySelectorAll over the whole document, four times a second, for as long as the
// browser stayed open -- and on a page whose DOM keeps growing (a chat, an infinite feed) each
// pass costs more than the last. That renderer CPU is the game's CPU.
if(YT)setInterval(function(){try{
  if(!document.getElementById('__fpsAdCss'))(document.head||document.documentElement).appendChild(css);
  var p=document.querySelector('.html5-video-player'),v=p&&p.querySelector('video');
  // The skip buttons live inside the player, so the scan stops at it instead of the document.
  if(p)p.querySelectorAll('.ytp-ad-skip-button,.ytp-skip-ad-button,.ytp-ad-skip-button-modern,.ytp-ad-skip-button-container button,.ytp-ad-overlay-close-button').forEach(function(b){b.click();});
  if(p&&v&&p.classList.contains('ad-showing')){
    v.muted=true;v.playbackRate=16;sped=true;
    if(isFinite(v.duration)&&v.duration>0)v.currentTime=v.duration;
  }else if(sped&&v){v.playbackRate=1;sped=false;}
  document.querySelectorAll('ytd-enforcement-message-view-model').forEach(function(e){
    var d=e.closest('tp-yt-paper-dialog');(d||e).remove();
    var w=document.querySelector('video');if(w&&w.paused)w.play();});
}catch(e){}},250);
}catch(e){}})())JS";

static const std::string& WebAdJs()
{
    static std::string s;
    if (s.empty()) {
        std::string css;
        for (const char* c = k_adCss; *c; ++c) { if (*c == '"' || *c == '\\') css += '\\'; css += *c; }
        s = k_adJs;
        s.replace(s.find("%CSS%"), 5, css);
    }
    return s;
}

// Main thread. Cheap when the page already has it: the guard returns on the first line.
__declspec(noinline) static void WebExecJs_SEH(const char* js)
{
    if (s_wv2) { wv2::ExecJs(js); return; }
    __try { s_surface->ExecuteJavascript(s_browser, js); } __except(EXCEPTION_EXECUTE_HANDLER) {}
}

static void WebApplyAdblock()
{
    if (!g_webAdblock || !WebAlive()) return;
    WebExecJs_SEH(WebAdJs().c_str());
}

static void WebApplyMediaJs()
{
    if (!WebAlive()) return;
    WebExecJs_SEH(k_wsEndedJs);
    if (g_webSmooth) WebExecJs_SEH(k_wsSmoothJs);
    WebExecJs_SEH(g_webSmooth ? "window.__fpsQOff=0" : "window.__fpsQOff=1");
}

static const char* const k_adHosts[] = {
    "doubleclick.net", "googlesyndication.com", "googleadservices.com",
    "adservice.google.com", "popads.net", "popcash.net", "propellerads.com",
    "onclickads.net", "adnxs.com",
};

static bool WebUrlBlocked(const char* url)
{
    if (!g_webAdblock || !url) return false;
    for (const char* h : k_adHosts)
        if (strstr(url, h)) return true;
    return false;
}

static std::string WebRewriteUrl(const std::string& url)
{
    if (!g_webYtEmbed || s_wv2) return url;
    const char* p = strstr(url.c_str(), "youtube.com/watch?v=");
    if (p) {
        std::string id(p + 20);
        const size_t amp = id.find('&');
        if (amp != std::string::npos) id.resize(amp);
        if (!id.empty()) return "https://www.youtube-nocookie.com/embed/" + id + "?autoplay=1";
    }
    p = strstr(url.c_str(), "youtu.be/");
    if (p) {
        std::string id(p + 9);
        const size_t q = id.find_first_of("?&");
        if (q != std::string::npos) id.resize(q);
        if (!id.empty()) return "https://www.youtube-nocookie.com/embed/" + id + "?autoplay=1";
    }
    return url;
}

static void WebApplyVolume()
{
    if (!WebAlive()) return;
    float vol = g_webVolume;
    if (vol < 0.f) vol = 0.f;
    if (vol > 1.f) vol = 1.f;
    if (g_webVolDistance) vol *= s_distAtten;

    char js[256];
    snprintf(js, sizeof(js),
             "(function(){try{var v=%.3f;document.querySelectorAll('video,audio')"
             ".forEach(function(e){e.volume=v;e.muted=(v<=0);});}catch(e){}})()", vol);
    WebExecJs_SEH(js);
}

__declspec(noinline) static void WebLoadUrl_SEH(const char* url)
{
    if (s_wv2) { wv2::Navigate(url); return; }
    __try { s_surface->LoadURL(s_browser, url, nullptr); } __except(EXCEPTION_EXECUTE_HANDLER) {}
}

static void WebNavigate(const std::string& typed)
{
    const std::string url = WsNormalizeUrl(typed);
    if (!WebAlive() || url.empty()) return;
    WebLoadUrl_SEH(WebRewriteUrl(url).c_str());
}

static DWORD s_readyAtMs = 0;

static void OnBrowserReady(HTML_BrowserReady_t* p)
{
    if (s_browser != INVALID_HTMLBROWSER) return;
    s_browser   = p->unBrowserHandle;
    s_readyAtMs = GetTickCount();
    WebSetStatus("ready");
    DebugLog("web: browser ready handle=%u", (unsigned)s_browser);
    int rw, rh; WebRes(rw, rh);
    __try {
        s_surface->SetSize(s_browser, (uint32_t)rw, (uint32_t)rh);
        s_surface->SetKeyFocus(s_browser, true);
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
    WebNavigate(s_resumeUrl.empty() ? g_webUrl : s_resumeUrl);
    s_resumeUrl.clear();
}

static void OnNeedsPaint(HTML_NeedsPaint_t* p)
{
    InterlockedIncrement(&s_cbSeen);
    if (InterlockedIncrement(&s_paints) == 1)
        DebugLog("web: first paint %ux%u update %u,%u %ux%u",
                 p->unWide, p->unTall, p->unUpdateX, p->unUpdateY, p->unUpdateWide, p->unUpdateTall);
    if (!p->pBGRA || p->unWide == 0 || p->unTall == 0) return;
    WebPublishPixels((const uint8_t*)p->pBGRA, (int)p->unWide, (int)p->unTall, (int)p->unWide * 4);
}

static void OnStartRequest(HTML_StartRequest_t* p)
{
    InterlockedIncrement(&s_cbSeen);
    const bool allow = !WebUrlBlocked(p->pchURL);
    DebugLog("web: start request %s -> %s", p->pchURL ? p->pchURL : "(null)", allow ? "allow" : "BLOCKED");
    __try { s_surface->AllowStartRequest(p->unBrowserHandle, allow); }
    __except(EXCEPTION_EXECUTE_HANDLER) {}
}

static const char* k_guardJs =
    "(function(){try{if(window.__fpsGuard)return;window.__fpsGuard=1;"
    "window.alert=function(){};window.confirm=function(){return true;};window.prompt=function(){return null;};"
    "window.open=function(u){try{if(u)location.href=u;}catch(e){}return null;};"
    "document.addEventListener('click',function(e){try{var t=e.target;"
    "var a=t&&t.closest&&t.closest('a[target]');if(a&&a.target!=='_self')a.target='_self';"
    "var f=t&&t.closest&&t.closest('input[type=file]');if(f)e.preventDefault();"
    "}catch(x){}},true);"
    "var pc=HTMLInputElement.prototype.click;HTMLInputElement.prototype.click=function(){"
    "if(this.type==='file')return;return pc.apply(this,arguments);};"
    "if(HTMLInputElement.prototype.showPicker)HTMLInputElement.prototype.showPicker=function(){};"
    "}catch(e){}})()";

static const char* k_mediaLogJs = R"JS((function(){try{
if(window.__fpsML)return;var L=window.__fpsML=[];
var sh=function(u){return String(u||'').replace(/^https?:\/\//,'').replace(/[?#].*$/,'').slice(0,44);};
var add=function(s){L.push(s);if(L.length>8)L.shift();};
document.addEventListener('loadstart',function(e){var t=e.target;
  if(t&&t.tagName==='VIDEO')add('L:'+sh(t.currentSrc||t.src)+(t.srcObject?'(obj)':''));},true);
document.addEventListener('error',function(e){var t=e.target;
  if(t&&t.tagName==='VIDEO'&&t.error)add('E'+t.error.code+':'+String(t.error.message||'').slice(0,48));},true);
}catch(e){}})())JS";

static const char* k_wipeJs = R"JS((function(){try{
var h=location.hostname.split('.'),D=[''],P=['/'],s=location.protocol==='https:'?';secure':'';
for(var i=0;i<h.length-1;i++){var d=h.slice(i).join('.');D.push(d,'.'+d);}
var p=location.pathname.split('/');for(var j=2;j<p.length;j++)P.push(p.slice(0,j).join('/'));
document.cookie.split(';').forEach(function(c){var n=c.split('=')[0].trim();if(!n)return;
  D.forEach(function(d){P.forEach(function(q){document.cookie=n+'=;expires=Thu, 01 Jan 1970 00:00:00 GMT;max-age=0;path='+q+(d?';domain='+d:'')+s;});});});
try{localStorage.clear();}catch(e){}try{sessionStorage.clear();}catch(e){}
try{indexedDB.databases().then(function(l){l.forEach(function(x){indexedDB.deleteDatabase(x.name);});});}catch(e){}
try{caches.keys().then(function(k){k.forEach(function(x){caches.delete(x);});});}catch(e){}
try{navigator.serviceWorker.getRegistrations().then(function(r){r.forEach(function(x){x.unregister();});});}catch(e){}
}catch(e){}})())JS";

// Main thread. Origins this browser has shown, for the clear sequence (WebWipeTick).
static std::vector<std::string> s_origins;
static size_t s_wipeIdx   = 0;
static int    s_wipePhase = 0;
static DWORD  s_wipeAt    = 0;

static void WebNoteOrigin(const char* url)
{
    if (!url || strncmp(url, "http", 4) || s_origins.size() >= 64) return;
    const char* h = strstr(url, "://");
    if (!h) return;
    const char* e = h + 3;
    while (*e && *e != '/' && *e != '?' && *e != '#') ++e;
    std::string o(url, e);
    for (const auto& x : s_origins) if (x == o) return;
    s_origins.push_back(std::move(o));
}

static const char* k_uaJs = R"JS((function(){try{
if(window.__fpsUA)return;
if(/(^|\.)tiktok\.com$/.test(location.hostname)){window.__fpsUA='skip';return;}
window.__fpsUA=document.readyState;
var V='150',F=V+'.0.0.0';
var ua='Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/'+F+' Safari/537.36';
var b=[{brand:'Chromium',version:V},{brand:'Google Chrome',version:V},{brand:'Not.A/Brand',version:'99'}];
var fb=[{brand:'Chromium',version:F},{brand:'Google Chrome',version:F},{brand:'Not.A/Brand',version:'99.0.0.0'}];
var hi={brands:b,fullVersionList:fb,mobile:false,platform:'Windows',platformVersion:'15.0.0',
  architecture:'x86',bitness:'64',model:'',uaFullVersion:F,wow64:false,formFactors:['Desktop']};
var uad={brands:b,mobile:false,platform:'Windows',
  getHighEntropyValues:function(h){var o={brands:b,mobile:false,platform:'Windows'};
    (h||[]).forEach(function(k){if(k in hi)o[k]=hi[k];});return Promise.resolve(o);},
  toJSON:function(){return {brands:b,mobile:false,platform:'Windows'};}};
var P=Navigator.prototype,d=function(k,v){try{Object.defineProperty(P,k,{get:function(){return v;},configurable:true});}catch(e){}};
d('userAgent',ua);d('appVersion',ua.slice(8));d('userAgentData',uad);d('vendor','Google Inc.');d('webdriver',false);
}catch(e){}})())JS";

static const char* k_codecJs = R"JS((function(){try{
if(window.__fpsCodec)return;window.__fpsCodec=1;
var P=function(g){try{
var bad=/hvc1|hev1|hevc|bytevc1|h265/i,said={};
var rep=g.document?function(m){g.__fpsWk=((g.__fpsWk||'')+' '+m).slice(-160);}
  :function(m){try{new BroadcastChannel('__fps').postMessage(m);}catch(e){}};
var once=function(m){if(!said[m]){said[m]=1;rep(m);}};
[g.MediaSource,g.ManagedMediaSource].forEach(function(M){
  if(!M||!M.isTypeSupported)return;var o=M.isTypeSupported;
  M.isTypeSupported=function(t){if(bad.test(String(t))){if(!g.document)once('wdeny');return false;}return o.call(M,t);};});
if(g.HTMLMediaElement){var cp=g.HTMLMediaElement.prototype.canPlayType;
  g.HTMLMediaElement.prototype.canPlayType=function(t){return bad.test(String(t))?'':cp.call(this,t);};}
var mc=g.navigator&&g.navigator.mediaCapabilities;
if(mc&&mc.decodingInfo){var d=mc.decodingInfo.bind(mc);
  if(g.document){g.__fpsHevcHw='?';
  d({type:'media-source',video:{contentType:'video/mp4; codecs="hvc1.1.6.L93.B0"',width:1280,height:720,bitrate:2000000,framerate:30}})
   .then(function(r){g.__fpsHevcHw=(r.supported?1:0)+'/'+(r.smooth?1:0)+'/'+(r.powerEfficient?1:0);},function(){g.__fpsHevcHw='err';});}
  mc.decodingInfo=function(c){try{var v=c&&c.video;if(v&&bad.test(String(v.contentType)))
    return Promise.resolve({supported:false,smooth:false,powerEfficient:false,keySystemAccess:null});}catch(e){}return d(c);};}
var MP=g.MediaSource&&g.MediaSource.prototype;
if(MP&&MP.addSourceBuffer){var asb=MP.addSourceBuffer;
  MP.addSourceBuffer=function(t){once((g.document?'':'w:')+String(t).replace(/\s|video\/|audio\/|codecs=/g,''));return asb.apply(this,arguments);};}
var VD=g.VideoDecoder;
if(VD&&VD.isConfigSupported){var vi=VD.isConfigSupported.bind(VD);
  VD.isConfigSupported=function(c){return c&&bad.test(String(c.codec))?Promise.resolve({supported:false,config:c}):vi(c);};}
var cut=function(u){return String(u).replace(/([?&]supported_codecs=)([^&#]*)/,function(a,k,v){
  var r=decodeURIComponent(v).split(',').filter(function(c){return c&&!/^(h265|hevc)$/i.test(c);}).join(',');
  once('usher '+decodeURIComponent(v)+'>'+r);return k+encodeURIComponent(r);});};
var has=function(u){return String(u).indexOf('supported_codecs=')>=0;};
if(g.fetch){var f=g.fetch;g.fetch=function(i,o){try{
  if(typeof i==='string'||i instanceof URL){if(has(i))i=cut(i);}
  else if(i&&i.url&&has(i.url))i=new Request(cut(i.url),i);}catch(e){}return f.call(this,i,o);};}
if(g.XMLHttpRequest){var xo=g.XMLHttpRequest.prototype.open;
  g.XMLHttpRequest.prototype.open=function(m,u){var a=[].slice.call(arguments);
    try{if(has(u))a[1]=cut(u);}catch(e){}return xo.apply(this,a);};}
}catch(e){}};
P(window);
try{(window.__fpsBC=new BroadcastChannel('__fps')).onmessage=function(e){window.__fpsWk=((window.__fpsWk||'')+' '+e.data).slice(-160);};}catch(e){}
var W=window.Worker;if(W){var pre='('+P.toString()+')(self);\n';
  var NW=function(u,o){var b=null;try{var s=String(u);if(!(o&&o.type==='module')&&s.indexOf('blob:')===0){
    var x=new XMLHttpRequest();x.open('GET',s,false);x.send();
    b=u=URL.createObjectURL(new Blob([pre+x.responseText],{type:'text/javascript'}));
    window.__fpsWorkers=(window.__fpsWorkers||0)+1;}}catch(e){}
    var w=new W(u,o);
    // Twitch starts a worker per playback session and the patched source is a new blob every
    // time; one never revoked is memory the page can never get back. 30s is long past load.
    if(b)setTimeout(function(){try{URL.revokeObjectURL(b);}catch(e){}},30000);
    return w;};
  NW.prototype=W.prototype;window.Worker=NW;}
}catch(e){}})())JS";

static void WebApplyGuard()
{
    if (!WebAlive()) return;
    WebExecJs_SEH(k_guardJs);
    WebExecJs_SEH(k_mediaLogJs);
    if (g_webHideSteam && !s_wv2) { WebExecJs_SEH(k_uaJs); WebExecJs_SEH(k_codecJs); }
}

static const char* k_probeTag = "__fpsprobe|";
static const char* k_probeJs =
    "(function(){try{var M=window.MediaSource,t=function(c){return M&&M.isTypeSupported(c)?1:0;};"
    "var s='h264='+t('video/mp4; codecs=\"avc1.4d401f\"')"
    "+'|hevc='+t('video/mp4; codecs=\"hvc1.1.6.L93.B0\"')"
    "+'|vp9='+t('video/webm; codecs=\"vp9\"')"
    "+'|av1='+t('video/mp4; codecs=\"av01.0.05M.08\"')"
    "+'|mse='+(M?1:0)+'|mms='+(window.ManagedMediaSource?1:0)"
    "+'|wasm='+(typeof WebAssembly==='object'?1:0)"
    "+'|webcodecs='+(typeof VideoDecoder==='function'?1:0)"
    "+'|webgl='+(function(){try{return document.createElement('canvas').getContext('webgl')?1:0;}catch(e){return 0;}})()"
    "+'|sab='+(typeof SharedArrayBuffer==='function'?1:0)"
    "+'|webdriver='+(navigator.webdriver?1:0)"
    "+'|chrome='+(window.chrome?1:0)"
    "+'|uad='+(navigator.userAgentData?navigator.userAgentData.brands.map(function(b){return b.brand+'/'+b.version;}).join(','):'none')"
    "+'|spoof='+(window.__fpsUA||'none')+'|host='+location.hostname;"
    // Real title kept in __fpsT0: two probes/diags overlapping must never "restore" a __fps title.
    "var t0=function(){var c=document.title;return /^__fps/.test(c)?(window.__fpsT0||''):(window.__fpsT0=c);};"
    "var send=function(w){var o=t0();document.title='__fpsprobe|'+s+'|widevine='+w+'|ua='+navigator.userAgent;"
    "setTimeout(function(){document.title=o;},200);};"
    "if(navigator.requestMediaKeySystemAccess)navigator.requestMediaKeySystemAccess('com.widevine.alpha',"
    "[{initDataTypes:['cenc'],videoCapabilities:[{contentType:'video/mp4; codecs=\"avc1.42E01E\"'}]}])"
    ".then(function(){send(1);},function(){send(0);});else send(0);"
    "if(!window.__fpsDiagArmed&&!window.__fpsNoDiag){window.__fpsDiagArmed=1;"
    "var say=function(a){var o=t0(),i=0,n=function(){if(i>=a.length)return;document.title=a[i++];"
    "setTimeout(function(){document.title=o;setTimeout(n,200);},200);};n();};"
    "var tv=function(u){return new Promise(function(res){try{var v=document.createElement('video'),d=0,"
    "f=function(r){if(d)return;d=1;res(r);try{v.removeAttribute('src');v.load();}catch(e){}};"
    "v.muted=true;v.preload='auto';v.onloadeddata=function(){f('ok'+v.videoWidth);};"
    "v.onerror=function(){f('err'+(v.error?v.error.code+':'+String(v.error.message||'').slice(0,40):''));};"
    "setTimeout(function(){f('to'+v.readyState);},7000);v.src=u;}catch(e){res('x');}});};"
    "var M='https://interactive-examples.mdn.mozilla.net/media/cc0-videos/flower.';"
    "var diag=function(){try{"
    "var ST=window.__fpsST||(window.__fpsST=Promise.all([tv(M+'mp4'),tv(M+'webm')]));"
    "var ve=[].slice.call(document.querySelectorAll('video')).map(function(v){"
    "return (v.error?'err'+v.error.code+':'+String(v.error.message||'').slice(0,60):'ok')+'/rs'+v.readyState"
    "+'/'+String(v.currentSrc||'').slice(0,40);}).join(';')||'none';"
    "var v0=[].slice.call(document.querySelectorAll('video')).filter(function(v){return /^https?:/.test(v.currentSrc||'');})[0];"
    "var st=v0?fetch(v0.currentSrc,{headers:{Range:'bytes=0-7'},credentials:'include'}).then(function(r){"
    "return r.status+' '+(r.headers.get('content-type')||'');},function(){return 'fetchfail';}):Promise.resolve('-');"
    "var tx=document.body?document.body.innerText:'',i=tx.search(/#\\d{4}|not supported|unsupported|supported browser|isn.t supported|update your browser|obs.ugiwan/i),"
    "m=i<0?'':tx.substr(Math.max(0,i-80),160).replace(/\\s+/g,' ');"
    "var vc=function(c,hw){return window.VideoDecoder?VideoDecoder.isConfigSupported({codec:c,hardwareAcceleration:hw})"
    ".then(function(r){return r.supported?1:0;},function(){return 'x';}):Promise.resolve('-');};"
    "var ac=window.AudioDecoder?AudioDecoder.isConfigSupported({codec:'mp4a.40.2',sampleRate:48000,numberOfChannels:2})"
    ".then(function(r){return r.supported?1:0;},function(){return 'x';}):Promise.resolve('-');"
    "Promise.all([vc('avc1.4d401f','prefer-software'),vc('avc1.4d401f','prefer-hardware'),"
    "vc('av01.0.05M.08','prefer-software'),vc('vp09.00.10.08','prefer-software'),ac,st,ST]).then(function(d){"
    "say(['__fpsdiag|'+location.hostname+'|test h264mp4='+d[6][0]+' vp8webm='+d[6][1]"
    "+'|workers='+(window.__fpsWorkers||0)+'|mse='+(window.__fpsWk||'-')"
    "+'|ml='+((window.__fpsML||[]).join(' ')||'-'),"
    "'__fpsdiag2|'+location.hostname+'|video='+ve+'|src='+d[5]"
    "+'|dec h264sw='+d[0]+' h264hw='+d[1]+' av1sw='+d[2]+' vp9sw='+d[3]+' aac='+d[4]"
    "+'|hevcHw='+(window.__fpsHevcHw||'-')+'|msg='+m]);});}catch(e){}};"
    "setTimeout(diag,8000);setTimeout(diag,30000);}"
    "}catch(e){}})()";
static int  s_hasH264  = -1;
static char s_probeLine[256] = "";

static void OnFinishedRequest(HTML_FinishedRequest_t* p)
{
    DebugLog("web: finished %.160s  title=\"%.80s\"",
             p->pchURL ? p->pchURL : "", p->pchPageTitle ? p->pchPageTitle : "");
    if (s_wipePhase == 1 && p->unBrowserHandle == s_browser) {
        WebExecJs_SEH(k_wipeJs);
        s_wipePhase = 2;
        s_wipeAt    = GetTickCount();
        return;
    }
    if (!s_wv2) { WebApplyGuard(); WebApplyAdblock(); WebApplyMediaJs(); }
    WebApplyVolume();
    if (!WebAlive()) return;
    if (!s_wv2) { WebExecJs_SEH(k_probeJs); return; }
    if (!s_probeLine[0]) { WebExecJs_SEH("window.__fpsNoDiag=1"); WebExecJs_SEH(k_probeJs); }
}

static void OnURLChanged(HTML_URLChanged_t* p)
{
    if (p->unBrowserHandle == s_browser) {
        WebNoteOrigin(p->pchURL);
        if (p->pchURL && !strncmp(p->pchURL, "http", 4)) s_curUrl = p->pchURL;
    }
    if (!s_wv2) WebApplyGuard();
}

static void OnChangedTitle(HTML_ChangedTitle_t* p)
{
    const char* m = p->pchTitle;
    if (m && !strcmp(m, k_wsEndedTag) && p->unBrowserHandle == s_browser) {
        if (g_webAutoRemove && s_placed) {
            DebugLog("web: video ended -- auto remove");
            Web_Unplace();
        }
        return;
    }
    if (m && !strncmp(m, "__fpsdiag", 9)) {
        DebugLog("web: %.490s", m + 5);
        if (strstr(m, "h264mp4=ok"))                   s_hasH264 = 1;
        else if (strstr(m, "h264mp4=err4:DECODER"))    s_hasH264 = 0;
        return;
    }
    if (!m || strncmp(m, k_probeTag, strlen(k_probeTag))) return;
    DebugLog("web: probe %s", m + strlen(k_probeTag));
    snprintf(s_probeLine, sizeof(s_probeLine), "%s", m + strlen(k_probeTag));
    if (char* ua = strstr(s_probeLine, "|ua=")) *ua = 0;
}

static void OnJSAlert(HTML_JSAlert_t* p)
{
    if (p->pchMessage) DebugLog("web: page alert got through the guard: %.200s", p->pchMessage);
    __try { s_surface->JSDialogResponse(p->unBrowserHandle, true); } __except(EXCEPTION_EXECUTE_HANDLER) {}
}
static void OnJSConfirm(HTML_JSConfirm_t* p)
{
    __try { s_surface->JSDialogResponse(p->unBrowserHandle, true); } __except(EXCEPTION_EXECUTE_HANDLER) {}
}
static void OnFileDialog(HTML_FileOpenDialog_t* p)
{
    __try { s_surface->FileLoadDialogResponse(p->unBrowserHandle, nullptr); } __except(EXCEPTION_EXECUTE_HANDLER) {}
}

__declspec(noinline) static void WebRemoveBrowser_SEH(HHTMLBrowser h)
{
    __try { s_surface->RemoveBrowser(h); } __except(EXCEPTION_EXECUTE_HANDLER) {}
}

static void OnNewWindow(HTML_NewWindow_t* p)
{
    if (p->unNewWindow_BrowserHandle != INVALID_HTMLBROWSER)
        WebRemoveBrowser_SEH(p->unNewWindow_BrowserHandle);
    if (p->pchURL && !WebUrlBlocked(p->pchURL)) WebNavigate(p->pchURL);
}

static void OnCloseBrowser(HTML_CloseBrowser_t*) { WebSetStatus("page closed itself"); }

template <typename P, void (*Fn)(P*)>
static void WsScoped(P* p)
{
    const n_ctx::hook_scope_t scope;
    if (!g_ctx.m_unloading.load(std::memory_order_relaxed)) Fn(p);
}

static WsCallback<HTML_BrowserReady_t>    s_cbReady   (&WsScoped<HTML_BrowserReady_t, &OnBrowserReady>);
static WsCallResult<HTML_BrowserReady_t>  s_crReady   (&WsScoped<HTML_BrowserReady_t, &OnBrowserReady>);
static WsCallback<HTML_NeedsPaint_t>      s_cbPaint   (&WsScoped<HTML_NeedsPaint_t, &OnNeedsPaint>);
static WsCallback<HTML_StartRequest_t>    s_cbStart   (&WsScoped<HTML_StartRequest_t, &OnStartRequest>);
static WsCallback<HTML_FinishedRequest_t> s_cbFinished(&WsScoped<HTML_FinishedRequest_t, &OnFinishedRequest>);
static WsCallback<HTML_JSAlert_t>         s_cbAlert   (&WsScoped<HTML_JSAlert_t, &OnJSAlert>);
static WsCallback<HTML_JSConfirm_t>       s_cbConfirm (&WsScoped<HTML_JSConfirm_t, &OnJSConfirm>);
static WsCallback<HTML_FileOpenDialog_t>  s_cbFile    (&WsScoped<HTML_FileOpenDialog_t, &OnFileDialog>);
static WsCallback<HTML_NewWindow_t>       s_cbNewWin  (&WsScoped<HTML_NewWindow_t, &OnNewWindow>);
static WsCallback<HTML_CloseBrowser_t>    s_cbClose   (&WsScoped<HTML_CloseBrowser_t, &OnCloseBrowser>);
static WsCallback<HTML_URLChanged_t>      s_cbUrl     (&WsScoped<HTML_URLChanged_t, &OnURLChanged>);
static WsCallback<HTML_ChangedTitle_t>    s_cbTitle   (&WsScoped<HTML_ChangedTitle_t, &OnChangedTitle>);

struct WsCbReg { CCallbackBase* cb; int id; };
static const WsCbReg k_cbs[] = {
    { &s_cbReady,    HTML_BrowserReady_t::k_iCallback },
    { &s_cbPaint,    HTML_NeedsPaint_t::k_iCallback },
    { &s_cbStart,    HTML_StartRequest_t::k_iCallback },
    { &s_cbFinished, HTML_FinishedRequest_t::k_iCallback },
    { &s_cbAlert,    HTML_JSAlert_t::k_iCallback },
    { &s_cbConfirm,  HTML_JSConfirm_t::k_iCallback },
    { &s_cbFile,     HTML_FileOpenDialog_t::k_iCallback },
    { &s_cbNewWin,   HTML_NewWindow_t::k_iCallback },
    { &s_cbClose,    HTML_CloseBrowser_t::k_iCallback },
    { &s_cbUrl,      HTML_URLChanged_t::k_iCallback },
    { &s_cbTitle,    HTML_ChangedTitle_t::k_iCallback },
};

static void WebSteamInit()
{
    if (s_steamTried) return;
    s_steamTried = true;

    HMODULE h = GetModuleHandleA("steam_api64.dll");
    if (!h) h = GetModuleHandleA("steam_api.dll");
    if (!h) { s_steamDead = true; WebSetStatus("steam_api not loaded"); return; }

    s_pfnRegister = (RegisterCb_t)  GetProcAddress(h, "SteamAPI_RegisterCallback");
    s_pfnUnreg    = (UnregisterCb_t)GetProcAddress(h, "SteamAPI_UnregisterCallback");
    s_pfnRegCr    = (RegisterCr_t)  GetProcAddress(h, "SteamAPI_RegisterCallResult");
    s_pfnUnregCr  = (UnregisterCr_t)GetProcAddress(h, "SteamAPI_UnregisterCallResult");
    s_pfnRun      = (RunCallbacks_t)GetProcAddress(h, "SteamAPI_RunCallbacks");

    if (auto flat = (void* (*)())GetProcAddress(h, "SteamAPI_SteamHTMLSurface_v005"))
        s_surface = (ISteamHTMLSurface*)flat();

    if (!s_surface) {
        auto foc  = (FindOrCreate_t)GetProcAddress(h, "SteamInternal_FindOrCreateUserInterface");
        auto user = (GetHUser_t)    GetProcAddress(h, "SteamAPI_GetHSteamUser");
        if (foc && user) s_surface = (ISteamHTMLSurface*)foc(user(), STEAMHTMLSURFACE_INTERFACE_VERSION);
    }

    if (!s_surface || !s_pfnRegister) {
        s_steamDead = true;
        WebSetStatus("Steam has no HTML surface");
        DebugLog("web: no ISteamHTMLSurface (surface=%p reg=%p)", (void*)s_surface, (void*)s_pfnRegister);
        return;
    }

    bool ok = false;
    __try { ok = s_surface->Init(); } __except(EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    if (!ok) {
        s_steamDead = true;
        WebSetStatus("HTMLSurface Init failed");
        DebugLog("web: ISteamHTMLSurface::Init returned false");
        return;
    }

    for (const WsCbReg& r : k_cbs) s_pfnRegister(r.cb, r.id);
    s_registered = true;
    DebugLog("web: ISteamHTMLSurface ready at %p (regCallResult=%p)",
             (void*)s_surface, (void*)s_pfnRegCr);
}

__declspec(noinline) static SteamAPICall_t WebCreateBrowser_SEH(const char* css)
{
    SteamAPICall_t h = 0;
    __try { h = s_surface->CreateBrowser(nullptr, css); }
    __except(EXCEPTION_EXECUTE_HANDLER) { h = 0; }
    return h;
}

static void WebCreateBrowser()
{
    if (s_created || !s_surface) return;
    s_created     = true;
    s_createdAtMs = GetTickCount();
    WebSetStatus("creating browser...");

    s_createCall = WebCreateBrowser_SEH(g_webAdblock ? k_adCss : nullptr);
    if (!s_createCall) {
        WebSetStatus("CreateBrowser returned nothing");
        DebugLog("web: CreateBrowser returned 0");
        return;
    }
    if (s_pfnRegCr) s_pfnRegCr(&s_crReady, s_createCall);
    DebugLog("web: CreateBrowser call=%llu callresult=%s",
             (unsigned long long)s_createCall, s_pfnRegCr ? "registered" : "NO EXPORT");
}

// Main thread, from the command queue. RemoveBrowser then a fresh CreateBrowser; the new
// call result is registered by WebCreateBrowser as usual.
static void WebRecreateBrowser()
{
    if (!s_surface) return;
    DebugLog("web: clear cache -- dropping browser %u", (unsigned)s_browser);
    if (s_pfnUnregCr && s_createCall) {
        __try { s_pfnUnregCr(&s_crReady, s_createCall); } __except(EXCEPTION_EXECUTE_HANDLER) {}
    }
    if (s_browser != INVALID_HTMLBROWSER) WebRemoveBrowser_SEH(s_browser);
    s_browser    = INVALID_HTMLBROWSER;
    s_createCall = 0;
    s_created    = false;
    s_readyAtMs  = 0;
    s_paints     = 0;
    WebSetStatus("clearing cache...");
    WebCreateBrowser();
}

static void WebWipeNext()
{
    if (s_wipeIdx >= s_origins.size() || s_browser == INVALID_HTMLBROWSER) {
        DebugLog("web: clear -- wiped %u of %u site(s), new browser", (unsigned)s_wipeIdx, (unsigned)s_origins.size());
        s_wipePhase = 0;
        s_origins.clear();
        WebRecreateBrowser();
        return;
    }
    char st[64];
    snprintf(st, sizeof(st), "clearing site data %u/%u", (unsigned)s_wipeIdx + 1, (unsigned)s_origins.size());
    WebSetStatus(st);
    s_wipePhase = 1;
    s_wipeAt    = GetTickCount();
    WebLoadUrl_SEH((s_origins[s_wipeIdx] + "/robots.txt").c_str());
}

static void WebWipeTick()
{
    if (!s_wipePhase) return;
    const DWORD dt = GetTickCount() - s_wipeAt;
    if (s_wipePhase == 1 && dt > 5000) {           // never finished loading: wipe what's there
        WebExecJs_SEH(k_wipeJs);
        s_wipePhase = 2;
        s_wipeAt    = GetTickCount();
    } else if (s_wipePhase == 2 && dt > 800) {
        ++s_wipeIdx;
        WebWipeNext();
    }
}

// Capture thread. OnNeedsPaint's job: hand the whole page to the render thread.
static void WebWv2Sink(const uint8_t* bgra, int w, int h, int pitch)
{
    InterlockedIncrement(&s_paints);
    WebPublishPixels(bgra, w, h, pitch);
}

// Engine thread, before every top-level navigation. Reads one bool and a const table.
static bool WebWv2Filter(const char* url) { return WebUrlBlocked(url); }

static int s_wv2Restarts = 0;

static void WebWv2TryStart()
{
    s_wv2Tried = true;
    char ver[64] = "";
    if (!wv2::RuntimeVersion(ver, sizeof(ver))) {
        DebugLog("web: Edge WebView2 runtime not installed -- Steam's browser (Twitch/TikTok video won't play)");
        return;
    }
    int rw, rh; WebRes(rw, rh);
    if (!wv2::Start(rw, rh, &WebWv2Sink, &WebWv2Filter)) {
        DebugLog("web: WebView2 engine thread did not start -- Steam's browser");
        return;
    }
    s_wv2        = true;
    s_created    = true;
    s_createdAtMs = GetTickCount();
    s_paints     = 0;
    WebSetStatus("starting Edge WebView2...");
    DebugLog("web: engine = Edge WebView2 %s", ver);
}

static void WebWv2Drop(bool retry)
{
    const DWORD t0 = GetTickCount();
    wv2::Stop();
    DebugLog("web: WebView2 stopped in %lu ms", GetTickCount() - t0);
    WebFreePixels();
    s_wv2     = false;
    s_browser = INVALID_HTMLBROWSER;
    s_created = false;
    s_readyAtMs = 0;
    s_wv2Tried = !retry;
}

static void WebWv2Pump()
{
    static bool s_frameLogged = false;
    if (!s_frameLogged && s_paints > 0) { s_frameLogged = true; DebugLog("web: first WebView2 frame -- %s", wv2::Diag().c_str()); }

    wv2::Event e;
    while (wv2::PollEvent(e)) {
        switch (e.type) {
        case wv2::EV_READY:
            s_browser   = k_wv2Handle;
            s_readyAtMs = GetTickCount();
            WebSetStatus("ready");
            DebugLog("web: WebView2 ready");
            wv2::AddStartScript(k_guardJs);
            wv2::AddStartScript(k_mediaLogJs);
            wv2::AddStartScript(k_wsEndedJs);
            if (g_webAdblock) wv2::AddStartScript(WebAdJs().c_str());
            if (g_webSmooth)  wv2::AddStartScript(k_wsSmoothJs);
            WebNavigate(s_resumeUrl.empty() ? g_webUrl : s_resumeUrl);
            s_resumeUrl.clear();
            break;
        case wv2::EV_FAILED:
            DebugLog("web: WebView2 failed: %s -- falling back to Steam's browser", e.a.c_str());
            WebWv2Drop(false);
            WebSetStatus("Edge WebView2 failed, using Steam's browser");
            return;
        case wv2::EV_CRASHED:
            DebugLog("web: WebView2 %s -- %s", e.a.c_str(), s_wv2Restarts < 3 ? "restarting" : "giving up, Steam's browser");
            WebWv2Drop(++s_wv2Restarts <= 3);
            return;
        case wv2::EV_CLEARED:
            s_resumeUrl = s_curUrl;
            DebugLog("web: cleared all browsing data (%s) -- fresh browser", e.a.c_str());
            WebWv2Drop(true);
            WebSetStatus("cleared -- cookies, cache, history");
            return;
        case wv2::EV_URL: {
            HTML_URLChanged_t u{};
            u.unBrowserHandle = s_browser;
            u.pchURL = e.a.c_str();
            OnURLChanged(&u);
            break;
        }
        case wv2::EV_FINISHED: {
            if (!e.n) DebugLog("web: load failed %.160s (%s)", e.a.c_str(), e.b.c_str());
            HTML_FinishedRequest_t f{};
            f.unBrowserHandle = s_browser;
            f.pchURL       = e.a.c_str();
            f.pchPageTitle = e.b.c_str();
            OnFinishedRequest(&f);
            break;
        }
        case wv2::EV_TITLE: {
            HTML_ChangedTitle_t t{};
            t.unBrowserHandle = s_browser;
            t.pchTitle = e.a.c_str();
            OnChangedTitle(&t);
            break;
        }
        }
    }
}

static void WebReleaseTexture()
{
    if (s_tex) { s_tex->Release(); s_tex = nullptr; }
    s_texW = s_texH = 0;
}

void Web_InvalidateTexture() { WebReleaseTexture(); }

static void WebUploadPixels(IDirect3DDevice9* dev)
{
    if (!dev || !s_lockInit) return;

    bool fresh = false;
    EnterCriticalSection(&s_lock);
    if (s_pixDirty) {
        s_pixOut.swap(s_pixels);
        s_outW = s_pixW; s_outH = s_pixH;
        s_pixDirty = false;
        fresh = true;
    }
    LeaveCriticalSection(&s_lock);

    const int w = s_outW, h = s_outH;
    if (w <= 0 || h <= 0 || s_pixOut.size() < (size_t)w * h * 4) return;

    if (!s_tex || s_texW != w || s_texH != h) {
        WebReleaseTexture();
        HRESULT hr = dev->CreateTexture(w, h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &s_tex, nullptr);
        s_texDynamic = false;
        if (FAILED(hr) || !s_tex) {
            s_tex = nullptr;
            hr = dev->CreateTexture(w, h, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8,
                                    D3DPOOL_DEFAULT, &s_tex, nullptr);
            s_texDynamic = true;
        }
        if (FAILED(hr) || !s_tex) {
            s_tex = nullptr;
            static bool s_logged = false;
            if (!s_logged) { s_logged = true; DebugLog("web: CreateTexture %dx%d failed hr=0x%08X", w, h, (unsigned)hr); }
            return;
        }
        s_texW = w; s_texH = h;
        fresh = true;
        DebugLog("web: texture created %dx%d tex=%p pool=%s", w, h, (void*)s_tex,
                 s_texDynamic ? "DEFAULT+DYNAMIC" : "MANAGED");
    } else if (!fresh) {
        return;
    }

    D3DLOCKED_RECT lr{};
    if (FAILED(s_tex->LockRect(0, &lr, nullptr, s_texDynamic ? D3DLOCK_DISCARD : 0))) return;
    const size_t row = (size_t)w * 4;
    if (lr.Pitch == (int)row) memcpy(lr.pBits, s_pixOut.data(), row * (size_t)h);
    else for (int y = 0; y < h; ++y)
        memcpy((uint8_t*)lr.pBits + (size_t)y * lr.Pitch, &s_pixOut[(size_t)y * row], row);
    s_tex->UnlockRect(0);
}

struct WsPlane { WsVec3 normal; };
struct WsTrace
{
    WsVec3  startpos, endpos;
    WsPlane plane;
    float   fraction = 1.f;
};

inline constexpr unsigned k_wsMask = contents_solid | contents_window | contents_grate | contents_moveable;

static bool WsGroupWalkable(int g)
{
    switch (g) {
    case 1:  case 2:  case 3:
    case 10: case 11: case 12: // IN_VEHICLE, WEAPON (the CS rule: "don't stand on weapons"), VEHICLE_CLIP
    case 14: case 16: case 17:
    case 21:
        return true;
    default:
        return false;
    }
}

// Engine thread of the trace (main). true = the ray stops here.
// botox: static prop ehandle has serial 1 << 15 and is no entity: world, always a wall.
static bool WsSolidToPlayer(c_base_entity* pHandle)
{
    if (!pHandle) return false;
    __try {
        if ((pHandle->get_ref_ehandle() >> 16) == (1u << 15)) return true;
        c_collideable* col = pHandle->get_collideable();
        if (!col) return true;
        if (col->get_solid() == 0) return false;
        if (col->get_solid_flags() & (0x4 | 0x8)) return false;
        if (col->get_collision_group() == 5) return false;
        if (WsGroupWalkable(col->get_collision_group())) return false;
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return true; }
}

struct WsFilter : public i_trace_filter
{
    bool should_hit_entity(c_base_entity* pEnt, int) override { return WsSolidToPlayer(pEnt); }
    e_trace_type get_trace_type() const override { return e_trace_type::trace_type_everything_filter_props; }
};

// locals must stay trivially destructible or __try won't compile (C2712)
__declspec(noinline) static void WebTraceRay_SEH(const WsVec3& start, const WsVec3& end, WsTrace* tr)
{
    WsFilter filter;
    const ray_t ray(c_vector(start.x, start.y, start.z), c_vector(end.x, end.y, end.z));
    trace_t t;
    __try {
        g_interfaces.m_engine_trace->trace_ray(ray, k_wsMask, &filter, &t);
        tr->fraction     = t.m_fraction;
        tr->endpos       = WsVec3(t.m_end.m_x, t.m_end.m_y, t.m_end.m_z);
        tr->plane.normal = WsVec3(t.m_plane.m_normal.m_x, t.m_plane.m_normal.m_y, t.m_plane.m_normal.m_z);
    } __except(EXCEPTION_EXECUTE_HANDLER) { tr->fraction = 1.f; tr->endpos = end; }
}

static void WebTrace(const WsVec3& start, const WsVec3& end, WsTrace* tr)
{
    tr->fraction = 1.f;
    tr->endpos   = end;
    if (!g_interfaces.m_engine_trace) return;
    WebTraceRay_SEH(start, end, tr);
}

static void WebBakeMesh()
{
    std::vector<WsVert>   verts, vertsBack;
    std::vector<uint16_t> idx;

    if (!s_placed) {
        EnterCriticalSection(&s_meshLock);
        s_verts.clear(); s_vertsBack.clear(); s_idx.clear();
        ++s_meshEpoch;
        LeaveCriticalSection(&s_meshLock);
        s_rebake = false;
        return;
    }

    float ww, wh; WebWorldDims(ww, wh);
    const float w = ww > 1.f ? ww : 1.f;
    const float h = wh > 1.f ? wh : 1.f;
    float rad = g_webRound;
    const float maxRad = (w < h ? w : h) * 0.5f;
    if (rad < 0.f)      rad = 0.f;
    if (rad > maxRad)   rad = maxRad;

    float uv[4 * (k_wsCornerSeg + 1) * 2];
    const int n = WsRoundedRectOutline(rad / w, rad / h, rad > 0.01f ? k_wsCornerSeg : 0,
                                       uv, 4 * (k_wsCornerSeg + 1));
    if (n < 3) { s_rebake = false; return; }

    {
        const WsVec3 c = WsQuadPoint(0.5f, 0.5f, s_pos, s_right, s_up, ww, wh);
        WsVert vt{ c.x, c.y, c.z, 0xFFFFFFFF, 0.5f, 0.5f };
        verts.push_back(vt);
        vertsBack.push_back(vt);
    }
    for (int i = 0; i < n; ++i) {
        const float u = uv[i * 2], v = uv[i * 2 + 1];
        const WsVec3 p = WsQuadPoint(u, v, s_pos, s_right, s_up, ww, wh);
        WsVert vt{ p.x, p.y, p.z, 0xFFFFFFFF, u, v };
        verts.push_back(vt);
        vt.u = 1.f - u;
        vertsBack.push_back(vt);
    }
    for (int i = 0; i < n; ++i) {
        idx.push_back(0);
        idx.push_back((uint16_t)(1 + i));
        idx.push_back((uint16_t)(1 + ((i + 1) % n)));
    }

    // Published in one go: the render thread never sees a half-built mesh.
    EnterCriticalSection(&s_meshLock);
    s_verts.swap(verts);
    s_vertsBack.swap(vertsBack);
    s_idx.swap(idx);
    ++s_meshEpoch;
    LeaveCriticalSection(&s_meshLock);
    s_rebake = false;
}

void Web_PlaceAtCrosshair() { s_placeRequested = true; }

static void WebDoPlace()
{
    if (!s_viewValid) return;

    WsTrace tr;
    WebTrace(s_viewOrigin, s_viewOrigin + s_viewFwd * 8192.f, &tr);
    if (tr.fraction >= 1.f || WsLen(tr.plane.normal) < 0.5f) return;

    s_normal = WsNorm(tr.plane.normal);
    s_pos    = tr.endpos + s_normal * 0.5f;
    WsBasisFromNormal(s_normal, s_viewFwd, s_right, s_up);
    s_placed = true;
    s_rebake = true;
    if (g_webAutoPlay && WebAlive()) {
        if (g_webPlayFromStart) WebExecJs_SEH("window.__fpsPlayFrom0=1");
        WebExecJs_SEH(k_wsPlayJs);
    }

    float ww, wh; WebWorldDims(ww, wh);
    const WsVec3 tl = s_pos + s_right * (-ww * 0.5f) + s_up * (wh * 0.5f);
    const WsVec3 br = s_pos + s_right * ( ww * 0.5f) + s_up * (-wh * 0.5f);
    DebugLog("web: placed at %.1f %.1f %.1f n=%.2f %.2f %.2f r=%.2f %.2f %.2f u=%.2f %.2f %.2f",
             s_pos.x, s_pos.y, s_pos.z, s_normal.x, s_normal.y, s_normal.z,
             s_right.x, s_right.y, s_right.z, s_up.x, s_up.y, s_up.z);
    DebugLog("web:   corners tl=%.1f %.1f %.1f br=%.1f %.1f %.1f  eye=%.1f %.1f %.1f "
             "eyeDist=%.1f planeOff=%.1f",
             tl.x, tl.y, tl.z, br.x, br.y, br.z,
             s_viewOrigin.x, s_viewOrigin.y, s_viewOrigin.z,
             WsLen(s_viewOrigin - s_pos), WsDot(s_viewOrigin - s_pos, s_normal));
}

struct WsSavedState
{
    IDirect3DPixelShader9*       ps;
    IDirect3DVertexShader9*      vs;
    IDirect3DVertexDeclaration9* decl;
    IDirect3DBaseTexture9*       tex0;
    IDirect3DVertexBuffer9*      stream;
    IDirect3DIndexBuffer9*       indices;
    UINT   streamOffset, streamStride;
    DWORD  fvf;
    D3DMATRIX world, view, proj;
    DWORD  rs[24];
    DWORD  samp[6];
    DWORD  tss[8];
    DWORD  tss1[2];
};

static const D3DRENDERSTATETYPE k_wsRS[24] = {
    D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ZFUNC, D3DRS_ALPHABLENDENABLE,
    D3DRS_SRCBLEND, D3DRS_DESTBLEND, D3DRS_ALPHATESTENABLE, D3DRS_CULLMODE,
    D3DRS_LIGHTING, D3DRS_FOGENABLE, D3DRS_SCISSORTESTENABLE, D3DRS_STENCILENABLE,
    D3DRS_COLORWRITEENABLE, D3DRS_SRGBWRITEENABLE,
    D3DRS_STENCILFUNC, D3DRS_STENCILREF, D3DRS_STENCILMASK, D3DRS_STENCILWRITEMASK,
    D3DRS_STENCILPASS, D3DRS_STENCILFAIL, D3DRS_STENCILZFAIL, D3DRS_TWOSIDEDSTENCILMODE,
    D3DRS_CLIPPLANEENABLE, D3DRS_FILLMODE,
};
static_assert(sizeof(k_wsRS) / sizeof(k_wsRS[0]) == sizeof(WsSavedState::rs) / sizeof(DWORD), "save list");
static const D3DSAMPLERSTATETYPE k_wsSamp[6] = {
    D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_MAGFILTER,
    D3DSAMP_MINFILTER, D3DSAMP_MIPFILTER, D3DSAMP_SRGBTEXTURE,
};
static const D3DTEXTURESTAGESTATETYPE k_wsTSS[8] = {
    D3DTSS_COLOROP, D3DTSS_COLORARG1, D3DTSS_COLORARG2,
    D3DTSS_ALPHAOP, D3DTSS_ALPHAARG1, D3DTSS_ALPHAARG2,
    D3DTSS_TEXCOORDINDEX, D3DTSS_TEXTURETRANSFORMFLAGS,
};

static void WsSave(IDirect3DDevice9* dev, WsSavedState& s)
{
    dev->GetPixelShader(&s.ps);
    dev->GetVertexShader(&s.vs);
    dev->GetVertexDeclaration(&s.decl);
    dev->GetTexture(0, &s.tex0);
    dev->GetStreamSource(0, &s.stream, &s.streamOffset, &s.streamStride);
    dev->GetIndices(&s.indices);
    dev->GetFVF(&s.fvf);
    dev->GetTransform(D3DTS_WORLD, &s.world);
    dev->GetTransform(D3DTS_VIEW, &s.view);
    dev->GetTransform(D3DTS_PROJECTION, &s.proj);
    for (int i = 0; i < 24; ++i) dev->GetRenderState(k_wsRS[i], &s.rs[i]);
    for (int i = 0; i < 6;  ++i) dev->GetSamplerState(0, k_wsSamp[i], &s.samp[i]);
    for (int i = 0; i < 8;  ++i) dev->GetTextureStageState(0, k_wsTSS[i], &s.tss[i]);
    dev->GetTextureStageState(1, D3DTSS_COLOROP, &s.tss1[0]);
    dev->GetTextureStageState(1, D3DTSS_ALPHAOP, &s.tss1[1]);
}

static void WsRestore(IDirect3DDevice9* dev, WsSavedState& s)
{
    for (int i = 0; i < 24; ++i) dev->SetRenderState(k_wsRS[i], s.rs[i]);
    for (int i = 0; i < 6;  ++i) dev->SetSamplerState(0, k_wsSamp[i], s.samp[i]);
    for (int i = 0; i < 8;  ++i) dev->SetTextureStageState(0, k_wsTSS[i], s.tss[i]);
    dev->SetTextureStageState(1, D3DTSS_COLOROP, s.tss1[0]);
    dev->SetTextureStageState(1, D3DTSS_ALPHAOP, s.tss1[1]);
    dev->SetTransform(D3DTS_WORLD, &s.world);
    dev->SetTransform(D3DTS_VIEW, &s.view);
    dev->SetTransform(D3DTS_PROJECTION, &s.proj);
    dev->SetTexture(0, s.tex0);
    dev->SetPixelShader(s.ps);
    dev->SetVertexShader(s.vs);
    if (s.decl) dev->SetVertexDeclaration(s.decl); else dev->SetFVF(s.fvf);
    dev->SetStreamSource(0, s.stream, s.streamOffset, s.streamStride);
    dev->SetIndices(s.indices);

    if (s.tex0)    s.tex0->Release();
    if (s.ps)      s.ps->Release();
    if (s.vs)      s.vs->Release();
    if (s.decl)    s.decl->Release();
    if (s.stream)  s.stream->Release();
    if (s.indices) s.indices->Release();
}

static const D3DMATRIX k_identity = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };

static constexpr DWORD k_wsStencilBit = 0x80;
static IDirect3DSurface9* s_markDs  = nullptr;   // identity only, never dereferenced or released
static bool               s_marked  = false;

static bool WsDsHasStencil(IDirect3DSurface9* ds)
{
    D3DSURFACE_DESC d{};
    if (!ds || FAILED(ds->GetDesc(&d))) return false;
    return d.Format == D3DFMT_D24S8 || d.Format == D3DFMT_D24FS8 || d.Format == D3DFMT_D24X4S4 ||
           d.Format == D3DFMT_D15S1;
}

__declspec(noinline) static void WebDrawWorld(IDirect3DDevice9* dev, bool depthTest)
{
    if (!dev || !s_tex || !s_placed || !s_vpValid) return;

    const bool facing = WsDot(s_viewOrigin - s_pos, s_normal) > 0.f;
    if (!facing && g_webBackMode == 1) return;
    const bool wantBack = (!facing && g_webBackMode == 2);

    float a = g_webWorldOpacity;
    if (a <= 0.003f) return;
    if (a > 1.f) a = 1.f;
    const DWORD col = D3DCOLOR_ARGB((int)(a * 255.f + 0.5f), 255, 255, 255);

    static std::vector<WsVert>   s_draw;
    static std::vector<uint16_t> s_drawIdx;
    static DWORD    s_drawCol   = 0;
    static bool     s_drawBack  = false;
    static unsigned s_drawEpoch = 0xFFFFFFFFu;
    if (s_drawCol != col || s_drawBack != wantBack || s_drawEpoch != s_meshEpoch) {
        EnterCriticalSection(&s_meshLock);
        s_draw      = wantBack ? s_vertsBack : s_verts;
        s_drawIdx   = s_idx;
        s_drawEpoch = s_meshEpoch;
        LeaveCriticalSection(&s_meshLock);
        for (WsVert& v : s_draw) v.col = col;
        s_drawCol  = col;
        s_drawBack = wantBack;
    }
    if (s_draw.empty() || s_drawIdx.empty()) return;

    WsSavedState st{};
    __try {
        WsSave(dev, st);

        dev->SetVertexShader(nullptr);
        dev->SetPixelShader(nullptr);
        dev->SetVertexDeclaration(nullptr);
        dev->SetFVF(k_wsFvf);

        D3DMATRIX proj;
        memcpy(&proj, s_vp, sizeof(proj));
        dev->SetTransform(D3DTS_WORLD, &k_identity);
        dev->SetTransform(D3DTS_VIEW, &k_identity);
        dev->SetTransform(D3DTS_PROJECTION, &proj);

        dev->SetRenderState(D3DRS_ZENABLE, depthTest ? TRUE : FALSE);
        dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
        dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        dev->SetRenderState(D3DRS_LIGHTING, FALSE);
        dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
        dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        bool mark = false;
        if (g_colorCorrEnable) {
            IDirect3DSurface9* ds = nullptr;
            if (SUCCEEDED(dev->GetDepthStencilSurface(&ds)) && ds) {
                mark = WsDsHasStencil(ds);
                if (mark) s_markDs = ds;
                ds->Release();
            }
        }
        dev->SetRenderState(D3DRS_STENCILENABLE, mark ? TRUE : FALSE);
        if (mark) {
            dev->SetRenderState(D3DRS_TWOSIDEDSTENCILMODE, FALSE);
            dev->SetRenderState(D3DRS_STENCILFUNC, D3DCMP_ALWAYS);
            dev->SetRenderState(D3DRS_STENCILREF, k_wsStencilBit);
            dev->SetRenderState(D3DRS_STENCILMASK, k_wsStencilBit);
            dev->SetRenderState(D3DRS_STENCILWRITEMASK, k_wsStencilBit);
            dev->SetRenderState(D3DRS_STENCILPASS, D3DSTENCILOP_REPLACE);
            dev->SetRenderState(D3DRS_STENCILFAIL, D3DSTENCILOP_KEEP);
            dev->SetRenderState(D3DRS_STENCILZFAIL, D3DSTENCILOP_KEEP);
            s_marked = true;
        }
        dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
        dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);

        dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);

        dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
        dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
        dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
        dev->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
        dev->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
        dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);

        dev->SetTexture(0, s_tex);
        if (InterlockedIncrement(&s_draws) == 1)
            DebugLog("web: first world draw verts=%d tris=%d depthTest=%d",
                     (int)s_draw.size(), (int)(s_drawIdx.size() / 3), (int)depthTest);
        dev->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, (UINT)s_draw.size(),
                                    (UINT)(s_drawIdx.size() / 3), s_drawIdx.data(), D3DFMT_INDEX16,
                                    s_draw.data(), sizeof(WsVert));
    } __except(EXCEPTION_EXECUTE_HANDLER) {}

    __try { WsRestore(dev, st); } __except(EXCEPTION_EXECUTE_HANDLER) {}
}

static bool s_cqRefused = false;
static volatile bool s_cqRan = false;
static int  s_cqQueued = 0;

static bool WebQueueOnRenderThread()
{
    const bool depthTest = g_webDepthMode == 0;
    return n_render_queue::submit([depthTest] {
        s_cqRan = true;
        WebDrawWorld(g_d3dDev, depthTest);
    });
}

static ISteamHTMLSurface::EHTMLKeyModifiers WebMods()
{
    int m = 0;
    if (GetAsyncKeyState(VK_MENU)    & 0x8000) m |= ISteamHTMLSurface::k_eHTMLKeyModifier_AltDown;
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000) m |= ISteamHTMLSurface::k_eHTMLKeyModifier_CtrlDown;
    if (GetAsyncKeyState(VK_SHIFT)   & 0x8000) m |= ISteamHTMLSurface::k_eHTMLKeyModifier_ShiftDown;
    return (ISteamHTMLSurface::EHTMLKeyModifiers)m;
}

static int WebWv2Mods()
{
    const int m = WebMods();
    return ((m & ISteamHTMLSurface::k_eHTMLKeyModifier_AltDown)   ? wv2::KMOD_ALT   : 0) |
           ((m & ISteamHTMLSurface::k_eHTMLKeyModifier_CtrlDown)  ? wv2::KMOD_CTRL  : 0) |
           ((m & ISteamHTMLSurface::k_eHTMLKeyModifier_ShiftDown) ? wv2::KMOD_SHIFT : 0);
}

__declspec(noinline) static void WebMouseMovePx_SEH(int x, int y)
{
    s_mouseX = x; s_mouseY = y;
    if (s_wv2) { wv2::MouseInput(wv2::M_MOVE, x, y); return; }
    __try { s_surface->MouseMove(s_browser, x, y); } __except(EXCEPTION_EXECUTE_HANDLER) {}
}

__declspec(noinline) static void WebWheel_SEH(int delta)
{
    if (s_wv2) { wv2::MouseInput(wv2::M_WHEEL, s_mouseX, s_mouseY, delta * WHEEL_DELTA / 100); return; }
    __try { s_surface->MouseWheel(s_browser, (int32_t)delta); } __except(EXCEPTION_EXECUTE_HANDLER) {}
}

__declspec(noinline) static void WebKeyChar_SEH(uint32_t c)
{
    if (s_wv2) { wv2::KeyInput(wv2::K_CHAR, c, WebWv2Mods()); return; }
    __try { s_surface->KeyChar(s_browser, c, WebMods()); } __except(EXCEPTION_EXECUTE_HANDLER) {}
}

__declspec(noinline) static void WebKey_SEH(bool down, uint32_t vk)
{
    if (s_wv2) { wv2::KeyInput(down ? wv2::K_DOWN : wv2::K_UP, vk, WebWv2Mods()); return; }
    __try {
        if (down) s_surface->KeyDown(s_browser, vk, WebMods(), false);
        else      s_surface->KeyUp  (s_browser, vk, WebMods());
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
}

__declspec(noinline) static void WebSetSize_SEH(uint32_t w, uint32_t h)
{
    if (s_wv2) { wv2::Resize((int)w, (int)h); return; }
    __try { s_surface->SetSize(s_browser, w, h); } __except(EXCEPTION_EXECUTE_HANDLER) {}
}

__declspec(noinline) static void WebNudgePaint_SEH(uint32_t w, uint32_t h)
{
    __try {
        s_surface->SetBackgroundMode(s_browser, false);
        s_surface->SetSize(s_browser, w + 1, h + 1);
        s_surface->SetSize(s_browser, w, h);
        s_surface->SetKeyFocus(s_browser, true);
        s_surface->Reload(s_browser);
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
}

__declspec(noinline) static void WebSimple_SEH(int which)
{
    if (s_wv2) { if (which == 0) wv2::Back(); else if (which == 1) wv2::Forward(); else wv2::Reload(); return; }
    __try {
        if      (which == 0) s_surface->GoBack(s_browser);
        else if (which == 1) s_surface->GoForward(s_browser);
        else                 s_surface->Reload(s_browser);
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
}

// Main thread. u/v are the page's own 0..1 coordinates.
static void WebMouseAt(float u, float v)
{
    if (!WebAlive()) return;
    int rw, rh; WebRes(rw, rh);
    WebMouseMovePx_SEH((int)(u * (float)rw), (int)(v * (float)rh));
}

static const int k_wsAway = -1;

static void WebMousePark() { if (WebAlive()) WebMouseMovePx_SEH(k_wsAway, k_wsAway); }

static void WebMouseButton(bool down)
{
    if (!WebAlive()) return;
    if (s_wv2) { wv2::MouseInput(down ? wv2::M_LDOWN : wv2::M_LUP, s_mouseX, s_mouseY); return; }
    __try {
        if (down) s_surface->MouseDown(s_browser, ISteamHTMLSurface::eHTMLMouseButton_Left);
        else      s_surface->MouseUp  (s_browser, ISteamHTMLSurface::eHTMLMouseButton_Left);
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
}

static void WebMouseRight(bool down)
{
    if (!WebAlive()) return;
    if (s_wv2) { wv2::MouseInput(down ? wv2::M_RDOWN : wv2::M_RUP, s_mouseX, s_mouseY); return; }
    __try {
        if (down) s_surface->MouseDown(s_browser, ISteamHTMLSurface::eHTMLMouseButton_Right);
        else      s_surface->MouseUp  (s_browser, ISteamHTMLSurface::eHTMLMouseButton_Right);
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
}

// Main thread only. Each call is already wrapped in its own SEH helper.
static void WebDrainCommands()
{
    if (!WebAlive()) return;

    std::vector<WsCmd> cmds;
    EnterCriticalSection(&s_cmdLock);
    cmds.swap(s_cmds);
    LeaveCriticalSection(&s_cmdLock);

    for (const WsCmd& c : cmds) {
        switch (c.op) {
        case WS_NAV:     WebNavigate(c.s); break;
        case WS_BACK:    WebSimple_SEH(0); break;
        case WS_FWD:     WebSimple_SEH(1); break;
        case WS_RELOAD:  WebSimple_SEH(2); break;
        case WS_MMOVE:   WebMouseMovePx_SEH(c.a, c.b); break;
        case WS_MDOWN:   WebMouseButton(true); break;
        case WS_MUP:     WebMouseButton(false); break;
        case WS_WHEEL:   WebWheel_SEH(c.a); break;
        case WS_KEYCHAR: WebKeyChar_SEH((uint32_t)c.a); break;
        case WS_KEYDOWN: WebKey_SEH(true,  (uint32_t)c.a); break;
        case WS_KEYUP:   WebKey_SEH(false, (uint32_t)c.a); break;
        case WS_SIZE:    WebSetSize_SEH((uint32_t)c.a, (uint32_t)c.b); break;
        case WS_VOL: {
            static int s_lastSmooth = -1;
            if (!s_wv2) {
                WebApplyGuard(); WebApplyAdblock(); WebApplyMediaJs();
            } else if (s_lastSmooth != (int)g_webSmooth) {
                s_lastSmooth = (int)g_webSmooth;
                WebApplyMediaJs();
            }
            WebApplyVolume();
            break;
        }
        case WS_PAUSE:   WebExecJs_SEH(k_wsPauseJs); break;
        case WS_CLEARMEM:
            s_resumeUrl = s_curUrl;
            DebugLog("web: clear memory -- restarting the browser at %.160s", s_resumeUrl.c_str());
            if (s_wv2) { WebWv2Drop(true); WebSetStatus("restarting browser..."); }
            else       WebRecreateBrowser();
            return;
        case WS_RDOWN:   WebMouseRight(true); break;
        case WS_RUP:     WebMouseRight(false); break;
        case WS_CLEARCACHE:
            if (s_wv2) { WebSetStatus("clearing browsing data..."); wv2::ClearData(); }
            else if (!s_wipePhase) { s_wipeIdx = 0; WebWipeNext(); }
            break;
        default: break;
        }
    }
}

void Web_ClearMemory() { WebPost(WS_CLEARMEM); }

// Any thread: the address bar in the menu runs on the render thread.
void Web_Navigate(const char* url) { if (url && *url) WebPost(WS_NAV, 0, 0, url); }

void Web_ClearCache() { WebPost(WS_CLEARCACHE); }

void Web_Unplace()
{
    if (s_placed) WebPost(WS_PAUSE);
    s_placed = false;
    s_rebake = true;
}

// Main thread (CreateMove). s_crossInside is only ever true with the menu closed.
bool Web_BlockFire()
{
    return g_webEnable && g_webBlockFire && g_webCrosshairPointer && s_placed && s_crossInside;
}


bool Web_OnInput(unsigned msg, uintptr_t wParam)
{
    if (!g_webEnable || !g_webScrollInput || g_showMenu) return false;
    if (!WebAlive()) return false;

    float u, v;
    if (s_placed && s_crossInside)                 { u = s_crossU; v = s_crossV; }
    else if (!s_placed && g_webPanel && g_webPanelAlways && s_tex) { u = 0.5f; v = 0.5f; }
    else return false;

    const bool isArrow = wParam == VK_UP || wParam == VK_DOWN;
    switch (msg) {
    case WM_MOUSEWHEEL: {
        int rw, rh; WebRes(rw, rh);
        WebPost(WS_MMOVE, (int)(u * (float)rw), (int)(v * (float)rh));
        WebPost(WS_WHEEL, (int)GET_WHEEL_DELTA_WPARAM(wParam) * 100 / WHEEL_DELTA);
        if (g_webClickOnly) WebPost(WS_MMOVE, k_wsAway, k_wsAway);
        return true;
    }
    case WM_KEYDOWN: if (isArrow) { WebPost(WS_KEYDOWN, (int)wParam); return true; } break;
    case WM_KEYUP:   if (isArrow) { WebPost(WS_KEYUP,   (int)wParam); return true; } break;
    default: break;
    }
    return false;
}

inline constexpr size_t k_vsOrigin = offsetof(c_view_setup, m_origin);
inline constexpr size_t k_vsAngles = offsetof(c_view_setup, m_angles);

void Web_MainTick()
{
    if (g_eject) return;
    if (!g_webEnable) {
        if (s_wv2) { WebWv2Drop(true); WebSetStatus("off"); }
        return;
    }

    static bool s_tickLogged = false;
    if (!s_tickLogged) { s_tickLogged = true; DebugLog("web: main tick up (inGame=%d)", (int)IsInGame()); }

    s_pixWanted = s_placed || (g_webPanel && (g_webPanelAlways || Menu_Visible()));
    wv2::SetCaptureEnabled(s_pixWanted);

    if (!s_wv2Tried) WebWv2TryStart();
    if (s_wv2) {
        WebWv2Pump();
        if (s_wv2) WebDrainCommands();
        return;
    }

    WebSteamInit();
    if (s_steamDead) return;
    if (!s_created) WebCreateBrowser();

    if (!s_pumpSelf && s_created && s_browser == INVALID_HTMLBROWSER && s_pfnRun &&
        GetTickCount() - s_createdAtMs > 5000) {
        s_pumpSelf = true;
        DebugLog("web: browser not ready after 5s (callbacks seen=%ld) -- pumping "
                 "SteamAPI_RunCallbacks ourselves", s_cbSeen);
    }
    if (s_pumpSelf && s_pfnRun) { __try { s_pfnRun(); } __except(EXCEPTION_EXECUTE_HANDLER) {} }

    // Whatever the panel and the menu asked for since the last frame, done here on the thread
    // Steam's own callbacks arrive on.
    WebDrainCommands();
    WebWipeTick();

    if (s_browser != INVALID_HTMLBROWSER && s_paints == 0 && s_readyAtMs) {
        static int   s_nudges = 0;
        static DWORD s_lastNudge = 0;
        const DWORD now = GetTickCount();
        if (s_nudges < 5 && now - s_readyAtMs > 3000 && now - s_lastNudge > 3000) {
            s_lastNudge = now;
            ++s_nudges;
            int rw, rh; WebRes(rw, rh);
            DebugLog("web: no paint %u ms after ready -- nudge %d (size %dx%d)",
                     now - s_readyAtMs, s_nudges, rw, rh);
            WebNudgePaint_SEH((uint32_t)rw, (uint32_t)rh);
        }
    }
}

void Web_OnPostScreenSpaceEffects(const void* setup)
{
    if (g_eject) return;

    if (!g_webEnable || !WebHaveEngine()) {
        s_viewValid = false;
        return;
    }

    // The view this frame is drawn from, and the matrix the draw needs. Taken here because the
    // draw may run later on the render thread, where neither is reachable.
    if (setup) {
        __try {
            const uintptr_t p = reinterpret_cast<uintptr_t>(setup);
            const float* o = reinterpret_cast<const float*>(p + k_vsOrigin);
            const float* a = reinterpret_cast<const float*>(p + k_vsAngles);
            const WsVec3 origin(o[0], o[1], o[2]);
            const WsVec3 angles(a[0], a[1], a[2]);
            WsVec3 fwd, right, up;
            WsAngleVectors(angles, fwd, right, up);
            s_viewOrigin = origin;
            s_viewFwd    = fwd;
            s_viewValid  = true;
        } __except(EXCEPTION_EXECUTE_HANDLER) { s_viewValid = false; }
    }

    if (g_interfaces.m_engine_client) {
        __try {
            const float* m = &g_interfaces.m_engine_client->get_world_to_screen_matrix().data[0][0];
            if (m && WsIsProjection(m)) { WsTranspose4(m, s_vp); s_vpValid = true; }
        } __except(EXCEPTION_EXECUTE_HANDLER) { s_vpValid = false; }
    }

    static bool s_removeDown = false;
    if (!g_showMenu && !g_input.keys_blocked() && WebGameFocused()) {
        const bool placeNow = g_webPlaceKey && (GetAsyncKeyState(g_webPlaceKey) & 0x8000) != 0;
        if (placeNow && !s_placeDown) s_placeRequested = true;
        s_placeDown = placeNow;
        const bool removeNow = g_webRemoveKey && (GetAsyncKeyState(g_webRemoveKey) & 0x8000) != 0;
        if (removeNow && !s_removeDown) Web_Unplace();
        s_removeDown = removeNow;
    } else {
        s_placeDown = false;
        s_removeDown = false;
    }
    if (s_placeRequested) { s_placeRequested = false; WebDoPlace(); }

    {
        static float s_lastW = -1.f, s_lastH = -1.f, s_lastRound = -1.f;
        float ww, wh; WebWorldDims(ww, wh);
        if (s_lastW != ww || s_lastH != wh || s_lastRound != g_webRound) {
            s_lastW = ww; s_lastH = wh; s_lastRound = g_webRound;
            s_rebake = true;
        }
    }
    if (s_rebake && s_placed) WebBakeMesh();

    if (g_webVolDistance && s_placed && s_viewValid) {
        const float d  = WsLen(s_viewOrigin - s_pos);
        const float n0 = g_webVolNear;
        const float f0 = g_webVolFar > n0 + 1.f ? g_webVolFar : n0 + 1.f;
        s_distAtten = (d <= n0) ? 1.f : (d >= f0 ? 0.f : 1.f - (d - n0) / (f0 - n0));
    } else {
        s_distAtten = 1.f;
    }

    // Where the crosshair meets the placed screen, for the scroll input (WndProc thread reads it).
    // Kept apart from the pointer below: scrolling works with "crosshair drives cursor" off too.
    {
        bool in = false;
        float t;
        if (s_placed && s_viewValid && !g_showMenu && WsRayPlane(s_viewOrigin, s_viewFwd, s_pos, s_normal, t)) {
            float u, v, ww, wh;
            WebWorldDims(ww, wh);
            WsQuadUV(s_viewOrigin + s_viewFwd * t, s_pos, s_right, s_up, ww, wh, u, v);
            in = WsInQuad(u, v);
            if (in && g_webDepthMode == 0 && t > 2.f) {
                WsTrace tr;
                WebTrace(s_viewOrigin, s_viewOrigin + s_viewFwd * (t - 2.f), &tr);
                if (tr.fraction < 1.f) in = false;
            }
            if (in) {
                const bool facing = WsDot(s_viewOrigin - s_pos, s_normal) > 0.f;
                s_crossU = (!facing && g_webBackMode == 2) ? 1.f - u : u;
                s_crossV = v;
            }
        }
        s_crossInside = in;
    }

    static bool s_ptrOnPage   = false;
    static bool s_rDownWorld  = false;
    if (s_placed && s_viewValid && g_webCrosshairPointer && !g_showMenu && !g_input.keys_blocked()) {
        const bool inside  = s_crossInside;
        const bool focused = WebGameFocused();
        const bool clickNow = inside && focused && g_webClickKey &&
                              (GetAsyncKeyState(g_webClickKey) & 0x8000) != 0;
        const bool rNow     = inside && focused && g_webRClickKey &&
                              (GetAsyncKeyState(g_webRClickKey) & 0x8000) != 0;
        if (inside && (!g_webClickOnly || clickNow || rNow || s_mouseDownWorld || s_rDownWorld)) {
            WebMouseAt(s_crossU, s_crossV);
            s_ptrOnPage = true;
        } else if (s_ptrOnPage && !inside) {
            WebMousePark();
            s_ptrOnPage = false;
        }
        if (clickNow != s_mouseDownWorld) {
            WebMouseButton(clickNow);
            s_mouseDownWorld = clickNow;
        }
        if (rNow != s_rDownWorld) {
            WebMouseRight(rNow);
            s_rDownWorld = rNow;
        }
        if (g_webClickOnly && s_ptrOnPage && !clickNow && !rNow) { WebMousePark(); s_ptrOnPage = false; }
    } else {
        if (s_mouseDownWorld) { WebMouseButton(false); s_mouseDownWorld = false; }
        if (s_rDownWorld)     { WebMouseRight(false);  s_rDownWorld = false; }
        if (s_ptrOnPage)      { WebMousePark();        s_ptrOnPage = false; }
    }

    if (!s_placed || g_webDepthMode == 2) return;
    if (!s_presentTid) return;

    if (WebQueueOnRenderThread()) {
        if (++s_cqQueued == 240 && !s_cqRan)
            DebugLog("web: call queue took %d functors and ran none -- the world screen is not drawing", s_cqQueued);
        if (s_cqRefused) { s_cqRefused = false; DebugLog("web: call queue back -- world screen on the render thread"); }
        return;
    }
    if (!s_cqRefused) { s_cqRefused = true; DebugLog("web: no call queue -- world screen drawn inline"); }
    WebDrawWorld(g_d3dDev, g_webDepthMode == 0);
}

// The keys that never arrive as characters. Anything else the page needs, it gets from KeyChar.
static const int k_wsNavKeys[] = { VK_BACK, VK_RETURN, VK_TAB, VK_DELETE, VK_ESCAPE,
                                   VK_LEFT, VK_RIGHT, VK_UP, VK_DOWN, VK_HOME, VK_END,
                                   VK_PRIOR, VK_NEXT };
static constexpr int k_wsNavCount = (int)(sizeof(k_wsNavKeys) / sizeof(k_wsNavKeys[0]));
static bool s_navDown[k_wsNavCount] = {};

static void WebPanelKeysRelease()
{
    for (int i = 0; i < k_wsNavCount; ++i)
        if (s_navDown[i]) { s_navDown[i] = false; WebPost(WS_KEYUP, k_wsNavKeys[i]); }
}

static void WebPanelKeys()
{
    if (!WebAlive()) return;
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput || !WebGameFocused()) { WebPanelKeysRelease(); return; }

    for (int i = 0; i < io.InputQueueCharacters.Size; ++i) {
        const ImWchar c = io.InputQueueCharacters[i];
        if (c >= 32) WebPost(WS_KEYCHAR, (int)c);
    }

    const int* k_nav = k_wsNavKeys;
    for (int i = 0; i < k_wsNavCount; ++i) {
        const bool down = (GetAsyncKeyState(k_nav[i]) & 0x8000) != 0;
        if (down == s_navDown[i]) continue;
        s_navDown[i] = down;
        WebPost(down ? WS_KEYDOWN : WS_KEYUP, k_nav[i]);
    }
}

static constexpr float kWebRound = 5.f;

static ImVec2 s_imgPos(0.f, 0.f);
static ImVec2 s_imgSize(0.f, 0.f);

static void WebPanelChromeless()
{
    if (!s_tex || s_imgSize.x < 2.f || s_imgSize.y < 2.f) return;

    ImGui::SetNextWindowPos(s_imgPos);
    ImGui::SetNextWindowSize(s_imgSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                                   ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("##websurf_bare", nullptr, flags)) {
        float pa = g_webPanelOpacity; if (pa < 0.f) pa = 0.f; if (pa > 1.f) pa = 1.f;
        ImGui::GetWindowDrawList()->AddImageRounded(
            (ImTextureID)s_tex, s_imgPos, ImVec2(s_imgPos.x + s_imgSize.x, s_imgPos.y + s_imgSize.y),
            ImVec2(0, 0), ImVec2(1, 1),
            IM_COL32(255, 255, 255, (int)(pa * 255.f + 0.5f)), kWebRound);
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

static bool WebBtn(const char* id, const char* label, ImVec2 size, int icon = 0)
{
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(id, size);
    const bool hov = ImGui::IsItemHovered(), held = ImGui::IsItemActive();

    ImGuiStorage* st = ImGui::GetStateStorage();
    const ImGuiID sid = ImGui::GetItemID();
    float ht = st->GetFloat(sid, 0.f);
    ht += ((hov ? 1.f : 0.f) - ht) * fminf(1.f, ImGui::GetIO().DeltaTime * 10.f);
    st->SetFloat(sid, ht);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 q(p.x + size.x, p.y + size.y);
    if (icon != 4) {
        dl->AddRectFilled(p, q, IM_COL32(255, 255, 255, held ? 20 : (int)(10 + ht * 8)), 5.f);
        dl->AddRect(p, q, IM_COL32(255, 255, 255, (int)(18 + ht * 14)), 5.f, 0, 1.f);
    }

    const int lv = held ? 210 : (int)(164 + ht * 30);
    const ImU32 tc = IM_COL32(lv, lv, lv + 4, 255);
    const ImVec2 c((p.x + q.x) * 0.5f, (p.y + q.y) * 0.5f);
    const float s = size.y * 0.2f;
    switch (icon) {
    case 1: case 2: {
        const float d = icon == 1 ? s * 0.55f : -s * 0.55f;
        const ImVec2 pts[3] = { ImVec2(c.x + d, c.y - s), ImVec2(c.x - d, c.y), ImVec2(c.x + d, c.y + s) };
        dl->AddPolyline(pts, 3, tc, 0, 1.6f);
        break;
    }
    case 3: {
        const float pi = 3.14159265f, a1 = pi * 1.8f;
        dl->PathArcTo(c, s, pi * 0.3f, a1, 16);
        dl->PathStroke(tc, 0, 1.6f);
        const ImVec2 e(c.x + cosf(a1) * s, c.y + sinf(a1) * s);
        const ImVec2 t(-sinf(a1), cosf(a1)), n(cosf(a1), sinf(a1));
        dl->AddTriangleFilled(ImVec2(e.x + t.x * s * 0.6f, e.y + t.y * s * 0.6f),
                              ImVec2(e.x + n.x * s * 0.45f, e.y + n.y * s * 0.45f),
                              ImVec2(e.x - n.x * s * 0.45f, e.y - n.y * s * 0.45f), tc);
        break;
    }
    case 4:
        dl->AddLine(ImVec2(c.x - s, c.y - s), ImVec2(c.x + s, c.y + s), tc, 1.6f);
        dl->AddLine(ImVec2(c.x - s, c.y + s), ImVec2(c.x + s, c.y - s), tc, 1.6f);
        break;
    default: {
        const ImVec2 ts = ImGui::CalcTextSize(label);
        dl->AddText(ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), tc, label);
    }
    }
    return pressed;
}

static void WebPanel()
{
    if (!WebHaveEngine()) return;

    if (g_webPanelAlways && Menu_Alpha() < 1.f) WebPanelChromeless();
    if (!Menu_Visible()) return;
    const bool live = g_showMenu;

    float pa = g_webPanelOpacity; if (pa < 0.f) pa = 0.f; if (pa > 1.f) pa = 1.f;
    const float aspect = s_texH > 0 ? (float)s_texW / (float)s_texH : 16.f / 9.f;
    const float w = 480.f * (g_webPanelScale <= 0.05f ? 1.f : g_webPanelScale);

    static int s_lastAspect = -1;
    const bool reshape = s_lastAspect != -1 && s_lastAspect != g_webAspect;
    s_lastAspect = g_webAspect;
    if (reshape) {
        const WsAspect& as = WsAspectAt(g_webAspect);
        const float ar = (float)as.w / (float)as.h;
        const ImVec2 sz = ar < 1.f ? ImVec2(w * 0.75f * ar, w * 0.75f + 110.f)
                                   : ImVec2(w, w / ar + 110.f);
        ImGui::SetNextWindowSize(sz, ImGuiCond_Always);
    } else {
        ImGui::SetNextWindowSize(ImVec2(w, w / aspect + 110.f), ImGuiCond_FirstUseEver);
    }

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, kWebRound);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.f, 10.f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(240.f, 160.f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.f, 5.f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.f, 6.f));
    ImGui::PushStyleColor(ImGuiCol_ResizeGrip,        IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ResizeGripHovered, IM_COL32(255, 255, 255, 20));
    ImGui::PushStyleColor(ImGuiCol_ResizeGripActive,  IM_COL32(255, 255, 255, 40));
    ImGui::PushStyleColor(ImGuiCol_FrameBg,           IM_COL32(255, 255, 255, 10));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,    IM_COL32(255, 255, 255, 18));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive,     IM_COL32(255, 255, 255, 20));
    ImGui::PushStyleColor(ImGuiCol_Border,            IM_COL32(255, 255, 255, 18));
    ImGui::PushStyleColor(ImGuiCol_Text,              IM_COL32(210, 210, 214, 255));
    ImGui::PushFont(g_menuFont ? g_menuFont : ImGui::GetIO().FontDefault);
    auto popAll = [] { ImGui::End(); ImGui::PopFont(); ImGui::PopStyleColor(8); ImGui::PopStyleVar(8); };

    if (!ImGui::Begin("Web##websurf", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                                               ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar |
                                               ImGuiWindowFlags_NoScrollWithMouse |
                                               (live ? 0 : ImGuiWindowFlags_NoInputs))) {
        popAll();
        return;
    }

    const float hdrH = 25.f;
    {
        const ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
        const ImVec2 we(wp.x + ws.x, wp.y + ws.y);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float wr = kWebRound;
        ImGui::PushClipRect(wp, we, false);
        const ImVec4 bg = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
        dl->AddRectFilled(wp, we, ImColor(bg.x, bg.y, bg.z, bg.w * pa), wr);
        dl->AddRectFilled(wp, ImVec2(we.x, wp.y + hdrH), ImColor(25 / 255.f, 25 / 255.f, 25 / 255.f, pa), wr,
                          ImDrawFlags_RoundCornersTop);
        dl->AddRect(wp, we, IM_COL32(50, 50, 50, 255), wr);
        RenderFadedGradientLine(dl, ImVec2(wp.x, wp.y + hdrH - 1.f), ImVec2(ws.x, 1.f), ImGui::GetColorU32(ImGuiCol_Accent));
        ImGui::PopClipRect();

        if (ImFont* tf = g_render.m_fonts[e_font_names::font_name_verdana_bd_11]) {
            const ImVec2 ts = tf->CalcTextSizeA(tf->FontSize, FLT_MAX, 0.f, "web");
            dl->AddText(tf, tf->FontSize, ImVec2(wp.x + (ws.x - ts.x) * 0.5f, wp.y + (hdrH - ts.y) * 0.5f),
                        IM_COL32(255, 255, 255, 255), "web");
        }

        ImGui::SetCursorScreenPos(ImVec2(we.x - 24.f, wp.y + (hdrH - 18.f) * 0.5f));
        if (WebBtn("##webclose", nullptr, ImVec2(18.f, 18.f), 4)) g_webPanel = false;
        ImGui::SetCursorPos(ImVec2(ImGui::GetStyle().WindowPadding.x, hdrH + 10.f));
    }

    static char s_url[512] = {};
    static bool s_urlInit = false;
    if (!s_urlInit) { strncpy_s(s_url, g_webUrl.c_str(), _TRUNCATE); s_urlInit = true; }

    const float fh  = 17.f;
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    if (WebBtn("##webback",   nullptr, ImVec2(fh, fh), 1)) WebPost(WS_BACK);
    ImGui::SameLine();
    if (WebBtn("##webfwd",    nullptr, ImVec2(fh, fh), 2)) WebPost(WS_FWD);
    ImGui::SameLine();
    if (WebBtn("##webreload", nullptr, ImVec2(fh, fh), 3)) WebPost(WS_RELOAD);
    ImGui::SameLine();
    bool entered = false;
    if (ImGui::BeginChild("##weburlrow", ImVec2(ImGui::GetContentRegionAvail().x - 44.f - gap, fh), false,
                          ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse, false)) {
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 10.f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.f, 0.f));
        ImGui::SetNextItemWidth(-FLT_MIN);
        entered = ImGui::InputTextWithHint("##weburl", "https://...", s_url, sizeof(s_url),
                                           ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::PopStyleVar();
    }
    ImGui::EndChild();
    ImGui::SameLine();
    if (WebBtn("##webgo", "Go", ImVec2(44.f, fh)) || entered) {
        g_webUrl = s_url;
        WebPost(WS_NAV, 0, 0, g_webUrl.c_str());
    }

    const float btnW = (ImGui::GetContentRegionAvail().x - gap * 3.f) / 4.f;
    const bool  narrow = btnW < ImGui::CalcTextSize("Clear memory").x + 12.f;
    if (WebBtn("##webplace",  "Place",  ImVec2(btnW, fh))) Web_PlaceAtCrosshair();
    ImGui::SameLine();
    if (WebBtn("##webremove", "Remove", ImVec2(btnW, fh))) Web_Unplace();
    ImGui::SameLine();
    if (WebBtn("##webclear",  narrow ? "Cache" : "Clear cache", ImVec2(btnW, fh))) Web_ClearCache();
    ImGui::SameLine();
    if (WebBtn("##webclrmem", narrow ? "Memory" : "Clear memory", ImVec2(btnW, fh))) Web_ClearMemory();

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    float dw = avail.x, dh = dw / aspect;
    if (dh > avail.y && avail.y > 1.f) { dh = avail.y; dw = dh * aspect; }

    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    if (!s_tex) {
        ImGui::TextDisabled("%s", Web_Status());
        popAll();
        return;
    }

    ImGui::Dummy(ImVec2(dw, dh));
    ImGui::GetWindowDrawList()->AddImageRounded(
        (ImTextureID)s_tex, p0, ImVec2(p0.x + dw, p0.y + dh), ImVec2(0, 0), ImVec2(1, 1),
        IM_COL32(255, 255, 255, (int)(pa * 255.f + 0.5f)), kWebRound);

    s_imgPos  = p0;
    s_imgSize = ImVec2(dw, dh);

    const bool down  = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    const bool rdown = ImGui::IsMouseDown(ImGuiMouseButton_Right);
    static bool s_rDownPanel = false;
    static bool s_panelPtrIn = false;
    if (ImGui::IsItemHovered() && dw > 1.f && dh > 1.f) {
        const ImVec2 m = ImGui::GetIO().MousePos;
        const float u = (m.x - p0.x) / dw, v = (m.y - p0.y) / dh;
        int rw, rh; WebRes(rw, rh);
        const int px = (int)(u * (float)rw), py = (int)(v * (float)rh);
        const float wheel = ImGui::GetIO().MouseWheel;
        if (!g_webClickOnly || down || rdown || s_mouseDownPanel || s_rDownPanel || wheel != 0.f) {
            WebPost(WS_MMOVE, px, py);
            s_panelPtrIn = true;
        }
        if (down != s_mouseDownPanel) { WebPost(down ? WS_MDOWN : WS_MUP); s_mouseDownPanel = down; }
        if (rdown != s_rDownPanel)    { WebPost(rdown ? WS_RDOWN : WS_RUP); s_rDownPanel = rdown; }
        if (wheel != 0.f) WebPost(WS_WHEEL, (int)(wheel * 100.f));
        if (g_webClickOnly && !down && !rdown && (wheel != 0.f || ImGui::IsMouseReleased(ImGuiMouseButton_Left) ||
                                                  ImGui::IsMouseReleased(ImGuiMouseButton_Right))) {
            WebPost(WS_MMOVE, k_wsAway, k_wsAway);
            s_panelPtrIn = false;
        }
    } else {
        if (s_mouseDownPanel && !down) { WebPost(WS_MUP); s_mouseDownPanel = false; }
        if (s_rDownPanel && !rdown)    { WebPost(WS_RUP); s_rDownPanel = false; }
        if (s_panelPtrIn && !s_mouseDownPanel && !s_rDownPanel) {
            WebPost(WS_MMOVE, k_wsAway, k_wsAway);
            s_panelPtrIn = false;
        }
    }
    if (live && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) WebPanelKeys();
    else WebPanelKeysRelease();

    popAll();
}

void Web_RestoreOverColorCorr(IDirect3DDevice9* dev, IDirect3DTexture9* preCC, int w, int h)
{
    if (!s_marked) return;
    s_marked = false;
    if (!dev || !preCC || w <= 0 || h <= 0) return;

    IDirect3DSurface9 *ds = nullptr, *rt = nullptr, *bb = nullptr;
    dev->GetDepthStencilSurface(&ds);
    dev->GetRenderTarget(0, &rt);
    dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
    const bool same = ds && ds == s_markDs && rt && rt == bb;
    if (ds) ds->Release();
    if (rt) rt->Release();
    if (bb) bb->Release();
    if (!same) {
        static bool s_logged = false;
        if (!s_logged) { s_logged = true; DebugLog("web: depth surface at Present is not the one marked -- CC not undone on the screen"); }
        return;
    }

    struct V { float x, y, z, rhw, u, v; };
    const float W = (float)w, H = (float)h;
    const V quad[4] = {
        { -0.5f,     -0.5f,     0.f, 1.f, 0.f, 0.f }, { W - 0.5f, -0.5f,     0.f, 1.f, 1.f, 0.f },
        { -0.5f,     H - 0.5f,  0.f, 1.f, 0.f, 1.f }, { W - 0.5f, H - 0.5f,  0.f, 1.f, 1.f, 1.f },
    };
    WsSavedState st{};
    D3DVIEWPORT9 vpOld{};
    __try {
        WsSave(dev, st);
        dev->GetViewport(&vpOld);
        D3DVIEWPORT9 vp{ 0, 0, (DWORD)w, (DWORD)h, 0.f, 1.f };
        dev->SetViewport(&vp);
        dev->SetVertexShader(nullptr);
        dev->SetPixelShader(nullptr);
        dev->SetVertexDeclaration(nullptr);
        dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
        dev->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
        dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
        dev->SetRenderState(D3DRS_ZENABLE, FALSE);
        dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        dev->SetRenderState(D3DRS_LIGHTING, FALSE);
        dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
        dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
        dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
        dev->SetRenderState(D3DRS_STENCILENABLE, TRUE);
        dev->SetRenderState(D3DRS_TWOSIDEDSTENCILMODE, FALSE);
        dev->SetRenderState(D3DRS_STENCILFUNC, D3DCMP_EQUAL);
        dev->SetRenderState(D3DRS_STENCILREF, k_wsStencilBit);
        dev->SetRenderState(D3DRS_STENCILMASK, k_wsStencilBit);
        dev->SetRenderState(D3DRS_STENCILWRITEMASK, k_wsStencilBit);
        dev->SetRenderState(D3DRS_STENCILPASS, D3DSTENCILOP_ZERO);
        dev->SetRenderState(D3DRS_STENCILFAIL, D3DSTENCILOP_KEEP);
        dev->SetRenderState(D3DRS_STENCILZFAIL, D3DSTENCILOP_KEEP);
        dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
        dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        dev->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
        dev->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
        dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        dev->SetTexture(0, preCC);
        const bool ownScene = SUCCEEDED(dev->BeginScene());
        dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(V));
        if (ownScene) dev->EndScene();
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
    __try { dev->SetViewport(&vpOld); WsRestore(dev, st); } __except(EXCEPTION_EXECUTE_HANDLER) {}
}

void Web_Present(IDirect3DDevice9* dev)
{
    s_presentTid = GetCurrentThreadId();
    // The engine went away and asked for the page back. Both of these are this thread's.
    if (s_freePixels) {
        s_freePixels = false;
        WebReleaseTexture();
        std::vector<uint8_t>().swap(s_pixOut);
        s_outW = s_outH = 0;
    }
    if (g_eject || !g_webEnable || (s_steamDead && !s_wv2)) return;

    if (WebAlive()) {
        static int s_lastW = 0, s_lastH = 0;
        int rw, rh; WebRes(rw, rh);
        if (s_lastW != rw || s_lastH != rh) {
            s_lastW = rw; s_lastH = rh;
            WebPost(WS_SIZE, rw, rh);
        }
        static float s_lastVol = -1.f, s_lastAtten = -1.f;
        static DWORD s_lastVolMs = 0;
        const DWORD now = GetTickCount();
        const float atten = g_webVolDistance ? s_distAtten : 1.f;
        const bool  moved = fabsf(atten - s_lastAtten) > 0.02f && now - s_lastVolMs > 150;
        if (s_lastVol != g_webVolume || moved || now - s_lastVolMs > 1000) {
            s_lastVol = g_webVolume; s_lastAtten = atten; s_lastVolMs = now;
            WebPost(WS_VOL);
        }
    }

    WebUploadPixels(dev);

    {
        static DWORD s_lastLog = 0;
        const DWORD now = GetTickCount();
        if (now - s_lastLog > 5000) {
            s_lastLog = now;
            DebugLog("web: state browser=%u paints=%ld tex=%p %dx%d placed=%d panel=%d "
                     "mode=%d draws=%ld cqQ=%d cqRan=%d",
                     (unsigned)s_browser, s_paints, (void*)s_tex, s_texW, s_texH,
                     (int)s_placed, (int)g_webPanel, g_webDepthMode, s_draws,
                     s_cqQueued, (int)s_cqRan);
        }
    }

    if (g_webPanel) WebPanel();

    if (s_placed && g_webDepthMode == 2) WebDrawWorld(dev, false);
}

void Web_LevelShutdown()
{
    if (s_placed) WebPost(WS_PAUSE);
    s_placed = false;
    s_verts.clear(); s_vertsBack.clear(); s_idx.clear();
}

void Web_Destroy()
{
    if (s_wv2) WebWv2Drop(false);   // joins the engine thread: nothing of it outlives the DLL
    if (s_registered && s_pfnUnreg) {
        for (const WsCbReg& r : k_cbs) { __try { s_pfnUnreg(r.cb); } __except(EXCEPTION_EXECUTE_HANDLER) {} }
        if (s_pfnUnregCr && s_createCall) {
            __try { s_pfnUnregCr(&s_crReady, s_createCall); } __except(EXCEPTION_EXECUTE_HANDLER) {}
        }
        s_registered = false;
    }
    s_createCall = 0;
    if (s_surface && s_browser != INVALID_HTMLBROWSER) {
        __try { s_surface->RemoveBrowser(s_browser); } __except(EXCEPTION_EXECUTE_HANDLER) {}
        s_browser = INVALID_HTMLBROWSER;
    }
    WebReleaseTexture();
    WebFreeAllPixels();
    s_created = false;
    s_placed  = false;
    s_verts.clear(); s_vertsBack.clear(); s_idx.clear();
}


const char* Web_Status()
{
    if (!g_webEnable) return "";
    if (s_steamDead && !s_wv2) return s_status;
    if (s_browser == INVALID_HTMLBROWSER) return s_status;
    if (!s_tex) return s_paints ? "painting, texture not up yet"
                                : "browser up, no paint delivered yet";
    return s_cqRefused ? "drawing (not queued, inline)" : "drawing";
}
