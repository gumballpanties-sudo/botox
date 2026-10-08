#pragma once
#include <cstdint>
#include <d3d9.h>

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
