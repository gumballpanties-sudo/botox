// dear imgui: Renderer for DirectX9
// This needs to be used along with a Platform Binding (e.g. Win32)

// Implemented features:
//  [X] Renderer: User texture binding. Use 'LPDIRECT3DTEXTURE9' as ImTextureID. Read the FAQ about ImTextureID!
//  [X] Renderer: Support for large meshes (64k+ vertices) with 16-bit indices.

// You can copy and use unmodified imgui_impl_* files in your project. See main.cpp for an example of using this.
// If you are new to dear imgui, read examples/README.txt and read the documentation at the top of imgui.cpp.
// https://github.com/ocornut/imgui

#pragma once

struct IDirect3DDevice9;
struct ImDrawData;

bool ImGui_ImplDX9_Init( IDirect3DDevice9* device );
void ImGui_ImplDX9_Shutdown( );
void ImGui_ImplDX9_NewFrame( );
void ImGui_ImplDX9_RenderDrawData( ImDrawData* draw_data );
// botox: draws draw_data through an offscreen layer, composited once at alpha ( uniform fade )
void ImGui_ImplDX9_RenderDrawDataFaded( ImDrawData* draw_data, float alpha );
void ImGui_ImplDX9_DestroyFontsTexture( );
// botox: one more D3DFMT_A8 texture ( render.cpp dpi font twins ) that takes its colour from the vertex, null = none
void ImGui_ImplDX9_SetTwinTextureA8( void* texture );
// botox: font texture in stages ( render.cpp reload ). stage: any thread, cpu only, null = no pixels. step: render
// thread, 1 done / 0 more frames / -1 failed. install: render thread, io.Fonts already = the staged atlas, frees the stage
struct ImFontAtlas;
void* ImGui_ImplDX9_StageFontsTexture( ImFontAtlas* atlas );
int ImGui_ImplDX9_UploadFontsStep( void* stage, unsigned int budget_bytes );
void ImGui_ImplDX9_InstallFontsTexture( void* stage );
void ImGui_ImplDX9_FreeFontsStage( void* stage );

// Use if you want to reset your rendering device without losing ImGui state.
 bool ImGui_ImplDX9_CreateDeviceObjects( );
 void ImGui_ImplDX9_InvalidateDeviceObjects( );