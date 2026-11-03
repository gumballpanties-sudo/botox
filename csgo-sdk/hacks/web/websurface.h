#pragma once
#include <cstdint>
#include <d3d9.h>
#include <string>

// botox: botox network web share. m_seq bumps on own place / remove / url change
struct web_share_t {
    bool        m_placed = false;
    float       m_pos[3]{}, m_normal[3]{}, m_right[3]{};
    float       m_scale  = 1.f;
    int         m_aspect = 0;
    unsigned    m_seq    = 0;
    std::string m_url;
    bool        m_media  = false;   // main video seen in the last 3s
    float       m_time   = 0.f;
    bool        m_paused = false;
};
bool Web_ShareGet(web_share_t& out);                  // main thread, false = web off
bool Web_On();
// main thread. one screen + browser per sender, never touches our own screen
bool Web_PeerShow(int sender, const web_share_t& in); // false = not shown (web off, no WebView2, all slots busy)
void Web_PeerHide(int sender);
void Web_PeerMedia(int sender, float time, bool paused);

void Web_Present(IDirect3DDevice9* dev);             // render thread, inside the ImGui frame (end_scene overlay)
void Web_MainTick();                                  // main thread every frame (FSN start), menu too
void Web_OnPostScreenSpaceEffects(const void* setup);
void Web_LevelShutdown();
void Web_Destroy();
void Web_InvalidateTexture();
bool Web_OnInput(unsigned msg, uintptr_t wParam);
void Web_PlaceAtCrosshair();
void Web_Navigate(const char* url);                   // menu / panel address bar, any thread
void Web_Unplace();
bool Web_BlockFire();
void Web_RestoreOverColorCorr(IDirect3DDevice9* dev, IDirect3DTexture9* preCC, int w, int h);
void Web_ClearCache();
void Web_ClearMemory();
const char* Web_Status();
