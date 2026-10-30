#pragma once
#include <cstdint>
#include <d3d9.h>
#include <string>

// botox: botox network web share. m_seq bumps only on the user's own place / remove / typed url
struct web_share_t {
    bool        m_placed = false;
    float       m_pos[3]{}, m_normal[3]{}, m_right[3]{};
    float       m_scale  = 1.f;
    int         m_aspect = 0;
    unsigned    m_seq    = 0;
    std::string m_url;
};
bool Web_ShareGet(web_share_t& out);                  // main thread, false = web off
void Web_ShareApply(const web_share_t& in);           // main thread, never bumps m_seq

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
