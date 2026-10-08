// dear imgui: Renderer for DirectX9
// This needs to be used along with a Platform Binding (e.g. Win32)

// Implemented features:
//  [X] Renderer: User texture binding. Use 'LPDIRECT3DTEXTURE9' as ImTextureID. Read the FAQ about ImTextureID!
//  [X] Renderer: Support for large meshes (64k+ vertices) with 16-bit indices.

// You can copy and use unmodified imgui_impl_* files in your project. See main.cpp for an example of using this.
// If you are new to dear imgui, read examples/README.txt and read the documentation at the top of imgui.cpp.
// https://github.com/ocornut/imgui

// CHANGELOG
// (minor and older changes stripped away, please see git history for details)
//  2019-05-29: DirectX9: Added support for large mesh (64K+ vertices), enable ImGuiBackendFlags_RendererHasVtxOffset flag.
//  2019-04-30: DirectX9: Added support for special ImDrawCallback_ResetRenderState callback to reset render state.
//  2019-03-29: Misc: Fixed erroneous assert in ImGui_ImplDX9_InvalidateDeviceObjects().
//  2019-01-16: Misc: Disabled fog before drawing UI's. Fixes issue #2288.
//  2018-11-30: Misc: Setting up io.BackendRendererName so it can be displayed in the About Window.
//  2018-06-08: Misc: Extracted imgui_impl_dx9.cpp/.h away from the old combined DX9+Win32 example.
//  2018-06-08: DirectX9: Use draw_data->DisplayPos and draw_data->DisplaySize to setup projection matrix and clipping rectangle.
//  2018-05-07: Render: Saving/restoring Transform because they don't seem to be included in the StateBlock. Setting shading mode to Gouraud.
//  2018-02-16: Misc: Obsoleted the io.RenderDrawListsFn callback and exposed ImGui_ImplDX9_RenderDrawData() in the .h file so you can call it
//  yourself. 2018-02-06: Misc: Removed call to ImGui::Shutdown() which is not available from 1.60 WIP, user needs to call
//  CreateContext/DestroyContext themselves.

#include "imgui_impl_dx9.h"
#include "imgui.h"

// DirectX
#include <d3d9.h>
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>

// DirectX data
static LPDIRECT3DDEVICE9 g_pd3dDevice   = NULL;
static LPDIRECT3DVERTEXBUFFER9 g_pVB    = NULL;
static LPDIRECT3DINDEXBUFFER9 g_pIB     = NULL;
static LPDIRECT3DTEXTURE9 g_FontTexture = NULL;
static int g_VertexBufferSize = 5000, g_IndexBufferSize = 10000;

// botox: font texture as D3DFMT_A8 ( 1/4 of the argb size: ~34 vs ~136 mb at 4096x8722 ). the argb upload failed
// after a Reset in a 2.7 gb 32 bit csgo -> TexID null -> every glyph a solid block until a retry got through
static bool g_FontTextureA8 = false;
// botox: font texture levels ( 100 / 50 / 25 % ), the dpi scale's floor is 25 %
static const int k_font_mip_levels = 3;
void botox_dbg_log( const char* fmt, ... ); // globals/logger/debug_log.cpp

// botox: offscreen layer for ImGui_ImplDX9_RenderDrawDataFaded, D3DPOOL_DEFAULT so it goes with the other device objects
static LPDIRECT3DTEXTURE9 g_LayerTexture = NULL;
static UINT g_LayerWidth = 0, g_LayerHeight = 0;
static bool g_LayerPass = false;

struct CUSTOMVERTEX {
	float pos[ 3 ];
	D3DCOLOR col;
	float uv[ 2 ];
};
#define D3DFVF_CUSTOMVERTEX ( D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1 )

static void ImGui_ImplDX9_SetupRenderState( ImDrawData* draw_data )
{
	// Setup viewport
	D3DVIEWPORT9 vp;
	vp.X = vp.Y = 0;
	vp.Width    = ( DWORD )draw_data->DisplaySize.x;
	vp.Height   = ( DWORD )draw_data->DisplaySize.y;
	vp.MinZ     = 0.0f;
	vp.MaxZ     = 1.0f;
	g_pd3dDevice->SetViewport( &vp );

	// Setup render state: fixed-pipeline, alpha-blending, no face culling, no depth testing, shade mode (for gradient)
	g_pd3dDevice->SetPixelShader( NULL );
	g_pd3dDevice->SetVertexShader( NULL );
	g_pd3dDevice->SetRenderState( D3DRS_CULLMODE, D3DCULL_NONE );
	g_pd3dDevice->SetRenderState( D3DRS_LIGHTING, false );
	g_pd3dDevice->SetRenderState( D3DRS_ZENABLE, false );
	g_pd3dDevice->SetRenderState( D3DRS_ZWRITEENABLE, false );
	g_pd3dDevice->SetRenderState( D3DRS_ALPHABLENDENABLE, true );
	g_pd3dDevice->SetRenderState( D3DRS_ALPHATESTENABLE, false );
	g_pd3dDevice->SetRenderState( D3DRS_BLENDOP, D3DBLENDOP_ADD );
	g_pd3dDevice->SetRenderState( D3DRS_SRCBLEND, D3DBLEND_SRCALPHA );
	g_pd3dDevice->SetRenderState( D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA );
	// botox: into the fade layer the ALPHA channel must add up as coverage ( one + inv src ), or the
	// layer's alpha comes out as a*a and the composite punches holes where panels overlap. alpha
	// writes forced on: the game may have left them masked. srgb off: the layer holds raw values.
	if ( g_LayerPass ) {
		g_pd3dDevice->SetRenderState( D3DRS_SEPARATEALPHABLENDENABLE, true );
		g_pd3dDevice->SetRenderState( D3DRS_BLENDOPALPHA, D3DBLENDOP_ADD );
		g_pd3dDevice->SetRenderState( D3DRS_SRCBLENDALPHA, D3DBLEND_ONE );
		g_pd3dDevice->SetRenderState( D3DRS_DESTBLENDALPHA, D3DBLEND_INVSRCALPHA );
		g_pd3dDevice->SetRenderState( D3DRS_COLORWRITEENABLE, 0xF );
		g_pd3dDevice->SetRenderState( D3DRS_SRGBWRITEENABLE, false );
	}
	g_pd3dDevice->SetRenderState( D3DRS_SCISSORTESTENABLE, true );
	g_pd3dDevice->SetRenderState( D3DRS_SHADEMODE, D3DSHADE_GOURAUD );
	g_pd3dDevice->SetRenderState( D3DRS_FOGENABLE, false );
	g_pd3dDevice->SetTextureStageState( 0, D3DTSS_COLOROP, D3DTOP_MODULATE );
	g_pd3dDevice->SetTextureStageState( 0, D3DTSS_COLORARG1, D3DTA_TEXTURE );
	g_pd3dDevice->SetTextureStageState( 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE );
	g_pd3dDevice->SetTextureStageState( 0, D3DTSS_ALPHAOP, D3DTOP_MODULATE );
	g_pd3dDevice->SetTextureStageState( 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE );
	g_pd3dDevice->SetTextureStageState( 0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE );
	g_pd3dDevice->SetSamplerState( 0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR );
	g_pd3dDevice->SetSamplerState( 0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR );
	// botox: the rest of what a glyph quad samples through. the game and our screen passes leave their
	// last values here and they change frame to frame: a leftover texture transform / texcoord source or
	// a stage 1 still modulating drew every glyph as blocks for a while, then clean again. the two vertex
	// side ones (texcoord index, transform flags) are put back by RenderDrawData, the block misses them
	g_pd3dDevice->SetRenderState( D3DRS_FILLMODE, D3DFILL_SOLID );
	g_pd3dDevice->SetRenderState( D3DRS_WRAP0, 0 );
	g_pd3dDevice->SetTextureStageState( 0, D3DTSS_TEXCOORDINDEX, 0 );
	g_pd3dDevice->SetTextureStageState( 0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE );
	g_pd3dDevice->SetTextureStageState( 1, D3DTSS_COLOROP, D3DTOP_DISABLE );
	g_pd3dDevice->SetTextureStageState( 1, D3DTSS_ALPHAOP, D3DTOP_DISABLE );
	// botox: trilinear for the font mip chain ( dpi scale < 100 % ). 1 level textures and >= 100 % sample level 0 as before
	g_pd3dDevice->SetSamplerState( 0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR );
	g_pd3dDevice->SetSamplerState( 0, D3DSAMP_MIPMAPLODBIAS, 0 );
	g_pd3dDevice->SetSamplerState( 0, D3DSAMP_MAXMIPLEVEL, 0 );
	g_pd3dDevice->SetSamplerState( 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP );
	g_pd3dDevice->SetSamplerState( 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP );
	// botox: rest of the end_scene post pass list ( every screen pass pins these ): a leftover stage 0 result
	// into TEMP drops the texture = solid quads, clip planes / stencil cut glyphs
	g_pd3dDevice->SetTextureStageState( 0, D3DTSS_RESULTARG, D3DTA_CURRENT );
	g_pd3dDevice->SetRenderState( D3DRS_CLIPPLANEENABLE, 0 );
	g_pd3dDevice->SetRenderState( D3DRS_STENCILENABLE, false );

	// Setup orthographic projection matrix
	// Our visible imgui space lies from draw_data->DisplayPos (top left) to draw_data->DisplayPos+data_data->DisplaySize (bottom right). DisplayPos
	// is (0,0) for single viewport apps. Being agnostic of whether <d3dx9.h> or <DirectXMath.h> can be used, we aren't relying on
	// D3DXMatrixIdentity()/D3DXMatrixOrthoOffCenterLH() or DirectX::XMMatrixIdentity()/DirectX::XMMatrixOrthographicOffCenterLH()
	{
		float L                  = draw_data->DisplayPos.x + 0.5f;
		float R                  = draw_data->DisplayPos.x + draw_data->DisplaySize.x + 0.5f;
		float T                  = draw_data->DisplayPos.y + 0.5f;
		float B                  = draw_data->DisplayPos.y + draw_data->DisplaySize.y + 0.5f;
		D3DMATRIX mat_identity   = { { { 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f } } };
		D3DMATRIX mat_projection = { { { 2.0f / ( R - L ), 0.0f, 0.0f, 0.0f, 0.0f, 2.0f / ( T - B ), 0.0f, 0.0f, 0.0f, 0.0f, 0.5f, 0.0f,
			                             ( L + R ) / ( L - R ), ( T + B ) / ( B - T ), 0.5f, 1.0f } } };
		g_pd3dDevice->SetTransform( D3DTS_WORLD, &mat_identity );
		g_pd3dDevice->SetTransform( D3DTS_VIEW, &mat_identity );
		g_pd3dDevice->SetTransform( D3DTS_PROJECTION, &mat_projection );
	}
}

// Render function.
// (this used to be set in io.RenderDrawListsFn and called by ImGui::Render(), but you can now call this directly from your main loop)
void ImGui_ImplDX9_RenderDrawData( ImDrawData* draw_data )
{
	// Avoid rendering when minimized
	if ( draw_data->DisplaySize.x <= 0.0f || draw_data->DisplaySize.y <= 0.0f )
		return;

	// Create and grow buffers if needed
	if ( !g_pVB || g_VertexBufferSize < draw_data->TotalVtxCount ) {
		if ( g_pVB ) {
			g_pVB->Release( );
			g_pVB = NULL;
		}
		g_VertexBufferSize = draw_data->TotalVtxCount + 5000;
		if ( g_pd3dDevice->CreateVertexBuffer( g_VertexBufferSize * sizeof( CUSTOMVERTEX ), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY,
		                                       D3DFVF_CUSTOMVERTEX, D3DPOOL_DEFAULT, &g_pVB, NULL ) < 0 )
			return;
	}
	if ( !g_pIB || g_IndexBufferSize < draw_data->TotalIdxCount ) {
		if ( g_pIB ) {
			g_pIB->Release( );
			g_pIB = NULL;
		}
		g_IndexBufferSize = draw_data->TotalIdxCount + 10000;
		if ( g_pd3dDevice->CreateIndexBuffer( g_IndexBufferSize * sizeof( ImDrawIdx ), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY,
		                                      sizeof( ImDrawIdx ) == 2 ? D3DFMT_INDEX16 : D3DFMT_INDEX32, D3DPOOL_DEFAULT, &g_pIB, NULL ) < 0 )
			return;
	}

	// Copy and convert all vertices into a single contiguous buffer, convert colors to DX9 default format.
	// FIXME-OPT: This is a waste of resource, the ideal is to use imconfig.h and
	//  1) to avoid repacking colors:   #define IMGUI_USE_BGRA_PACKED_COLOR
	//  2) to avoid repacking vertices: #define IMGUI_OVERRIDE_DRAWVERT_STRUCT_LAYOUT struct ImDrawVert { ImVec2 pos; float z; ImU32 col; ImVec2 uv; }
	// botox: before any state backup, so a failed lock returns with nothing held (it leaked the state block + two refs)
	CUSTOMVERTEX* vtx_dst;
	ImDrawIdx* idx_dst;
	if ( g_pVB->Lock( 0, ( UINT )( draw_data->TotalVtxCount * sizeof( CUSTOMVERTEX ) ), ( void** )&vtx_dst, D3DLOCK_DISCARD ) < 0 )
		return;
	if ( g_pIB->Lock( 0, ( UINT )( draw_data->TotalIdxCount * sizeof( ImDrawIdx ) ), ( void** )&idx_dst, D3DLOCK_DISCARD ) < 0 ) {
		g_pVB->Unlock( );
		return;
	}
	for ( int n = 0; n < draw_data->CmdListsCount; n++ ) {
		const ImDrawList* cmd_list = draw_data->CmdLists[ n ];
		const ImDrawVert* vtx_src  = cmd_list->VtxBuffer.Data;
		for ( int i = 0; i < cmd_list->VtxBuffer.Size; i++ ) {
			vtx_dst->pos[ 0 ] = vtx_src->pos.x;
			vtx_dst->pos[ 1 ] = vtx_src->pos.y;
			vtx_dst->pos[ 2 ] = 0.0f;
			vtx_dst->col      = ( vtx_src->col & 0xFF00FF00 ) | ( ( vtx_src->col & 0xFF0000 ) >> 16 ) |
			               ( ( vtx_src->col & 0xFF ) << 16 ); // RGBA --> ARGB for DirectX9
			vtx_dst->uv[ 0 ] = vtx_src->uv.x;
			vtx_dst->uv[ 1 ] = vtx_src->uv.y;
			vtx_dst++;
			vtx_src++;
		}
		memcpy( idx_dst, cmd_list->IdxBuffer.Data, cmd_list->IdxBuffer.Size * sizeof( ImDrawIdx ) );
		idx_dst += cmd_list->IdxBuffer.Size;
	}
	g_pVB->Unlock( );
	g_pIB->Unlock( );

	// Backup the DX9 state
	// modified by qo0
	// used D3DSBT_PIXELSTATE instead D3DSBT_ALL to fix game material render artifacts
	IDirect3DStateBlock9* d3d9_state_block = NULL;
	if ( g_pd3dDevice->CreateStateBlock( D3DSBT_PIXELSTATE, &d3d9_state_block ) != D3D_OK )
		return;

	// @credits: T0b1
	if ( d3d9_state_block->Capture( ) != D3D_OK ) {
		d3d9_state_block->Release( );
		return;
	}

	// modified by qo0
	// backup vertex states to compensate stateblock changes
	IDirect3DVertexDeclaration9* vertex_declaration = NULL;
	IDirect3DVertexShader9* vertex_shader           = NULL;
	g_pd3dDevice->GetVertexDeclaration( &vertex_declaration );
	g_pd3dDevice->GetVertexShader( &vertex_shader );

	// Backup the DX9 transform (DX9 documentation suggests that it is included in the StateBlock but it doesn't appear to)
	D3DMATRIX last_world, last_view, last_projection;
	g_pd3dDevice->GetTransform( D3DTS_WORLD, &last_world );
	g_pd3dDevice->GetTransform( D3DTS_VIEW, &last_view );
	g_pd3dDevice->GetTransform( D3DTS_PROJECTION, &last_projection );

	// botox: vertex state, the pixel state block does not carry it (SetupRenderState pins both)
	DWORD last_texcoord_index = 0, last_texture_transform = D3DTTFF_DISABLE, last_clip_planes = 0;
	g_pd3dDevice->GetTextureStageState( 0, D3DTSS_TEXCOORDINDEX, &last_texcoord_index );
	g_pd3dDevice->GetTextureStageState( 0, D3DTSS_TEXTURETRANSFORMFLAGS, &last_texture_transform );
	g_pd3dDevice->GetRenderState( D3DRS_CLIPPLANEENABLE, &last_clip_planes );

	/* botox: the vertex buffer bindings, which NO state block type carries. source skips its own
	   SetStreamSource / SetIndices whenever its cache still names the same buffer
	   ( meshdx8.cpp:1224 / :3746 ), so a mesh of the game's that lands on a cache hit after this
	   would draw out of OUR buffers. see hacks/visuals/screen/stream_guard.h. */
	IDirect3DVertexBuffer9* old_stream  = nullptr;
	IDirect3DIndexBuffer9* old_indices  = nullptr;
	unsigned int old_stream_offset      = 0;
	unsigned int old_stream_stride      = 0;

	if ( FAILED( g_pd3dDevice->GetStreamSource( 0, &old_stream, &old_stream_offset, &old_stream_stride ) ) )
		old_stream = nullptr;

	if ( FAILED( g_pd3dDevice->GetIndices( &old_indices ) ) )
		old_indices = nullptr;

	g_pd3dDevice->SetStreamSource( 0, g_pVB, 0, sizeof( CUSTOMVERTEX ) );
	g_pd3dDevice->SetIndices( g_pIB );
	g_pd3dDevice->SetFVF( D3DFVF_CUSTOMVERTEX );

	// Setup desired DX state
	ImGui_ImplDX9_SetupRenderState( draw_data );

	// Render command lists
	// (Because we merged all buffers into a single one, we maintain our own offset into them)
	int global_vtx_offset = 0;
	int global_idx_offset = 0;
	ImVec2 clip_off       = draw_data->DisplayPos;
	// botox: A8 font texture has no colour, take it from the vertex. other textures ( avatars, icons ) modulate
	int color_from_vertex = -1; // -1 = unknown, set on the next draw
	for ( int n = 0; n < draw_data->CmdListsCount; n++ ) {
		const ImDrawList* cmd_list = draw_data->CmdLists[ n ];
		for ( int cmd_i = 0; cmd_i < cmd_list->CmdBuffer.Size; cmd_i++ ) {
			const ImDrawCmd* pcmd = &cmd_list->CmdBuffer[ cmd_i ];
			if ( pcmd->UserCallback != NULL ) {
				// User callback, registered via ImDrawList::AddCallback()
				// (ImDrawCallback_ResetRenderState is a special callback value used by the user to request the renderer to reset render state.)
				if ( pcmd->UserCallback == ImDrawCallback_ResetRenderState )
					ImGui_ImplDX9_SetupRenderState( draw_data );
				else
					pcmd->UserCallback( cmd_list, pcmd );
				color_from_vertex = -1; // either path may have rewritten stage 0
			} else {
				const RECT r                     = { ( LONG )( pcmd->ClipRect.x - clip_off.x ), ( LONG )( pcmd->ClipRect.y - clip_off.y ),
					                                 ( LONG )( pcmd->ClipRect.z - clip_off.x ), ( LONG )( pcmd->ClipRect.w - clip_off.y ) };
				const LPDIRECT3DTEXTURE9 texture = ( LPDIRECT3DTEXTURE9 )pcmd->TextureId;
				if ( const int want = g_FontTextureA8 && texture == g_FontTexture; want != color_from_vertex ) {
					color_from_vertex = want;
					g_pd3dDevice->SetTextureStageState( 0, D3DTSS_COLOROP, want ? D3DTOP_SELECTARG2 : D3DTOP_MODULATE );
				}
				g_pd3dDevice->SetTexture( 0, texture );
				g_pd3dDevice->SetScissorRect( &r );
				g_pd3dDevice->DrawIndexedPrimitive( D3DPT_TRIANGLELIST, pcmd->VtxOffset + global_vtx_offset, 0, ( UINT )cmd_list->VtxBuffer.Size,
				                                    pcmd->IdxOffset + global_idx_offset, pcmd->ElemCount / 3 );
			}
		}
		global_idx_offset += cmd_list->IdxBuffer.Size;
		global_vtx_offset += cmd_list->VtxBuffer.Size;
	}

	// Restore the DX9 transform
	g_pd3dDevice->SetTransform( D3DTS_WORLD, &last_world );
	g_pd3dDevice->SetTransform( D3DTS_VIEW, &last_view );
	g_pd3dDevice->SetTransform( D3DTS_PROJECTION, &last_projection );
	g_pd3dDevice->SetTextureStageState( 0, D3DTSS_TEXCOORDINDEX, last_texcoord_index );
	g_pd3dDevice->SetTextureStageState( 0, D3DTSS_TEXTURETRANSFORMFLAGS, last_texture_transform );
	g_pd3dDevice->SetRenderState( D3DRS_CLIPPLANEENABLE, last_clip_planes );

	// modified by qo0
	// Restore the DX9 vertex states
	g_pd3dDevice->SetVertexDeclaration( vertex_declaration );
	g_pd3dDevice->SetVertexShader( vertex_shader );

	// botox: Get* AddRef'd both. unreleased = every decl / shader the game ever had bound pinned forever, one ref a frame
	if ( vertex_declaration )
		vertex_declaration->Release( );

	if ( vertex_shader )
		vertex_shader->Release( );

	// botox: and the buffers, exactly as found, so the game's own bind cache is telling the truth
	g_pd3dDevice->SetStreamSource( 0, old_stream, old_stream_offset, old_stream_stride );
	g_pd3dDevice->SetIndices( old_indices );

	if ( old_stream )
		old_stream->Release( );

	if ( old_indices )
		old_indices->Release( );

	// Restore the DX9 state
	d3d9_state_block->Apply( );
	d3d9_state_block->Release( );
}

// botox: the menu fade. draw_data goes into an offscreen layer at full alpha, then onto the current
// target ONCE, scaled by alpha, so every pixel of it fades by the same amount. scaling each vertex
// instead fades every stacked panel on its own: window bg + child bg + frame read 50/75/87% at the
// same moment. any failure draws unfaded rather than not at all.
void ImGui_ImplDX9_RenderDrawDataFaded( ImDrawData* draw_data, float alpha )
{
	IDirect3DSurface9* old_target = NULL;
	if ( g_pd3dDevice->GetRenderTarget( 0, &old_target ) != D3D_OK || !old_target ) {
		ImGui_ImplDX9_RenderDrawData( draw_data );
		return;
	}

	D3DSURFACE_DESC desc;
	old_target->GetDesc( &desc );

	if ( g_LayerTexture && ( g_LayerWidth != desc.Width || g_LayerHeight != desc.Height ) ) {
		g_LayerTexture->Release( );
		g_LayerTexture = NULL;
	}

	if ( !g_LayerTexture ) {
		if ( g_pd3dDevice->CreateTexture( desc.Width, desc.Height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g_LayerTexture,
		                                  NULL ) != D3D_OK )
			g_LayerTexture = NULL;

		g_LayerWidth  = desc.Width;
		g_LayerHeight = desc.Height;
	}

	IDirect3DSurface9* layer = NULL;
	if ( !g_LayerTexture || g_LayerTexture->GetSurfaceLevel( 0, &layer ) != D3D_OK ) {
		old_target->Release( );
		ImGui_ImplDX9_RenderDrawData( draw_data );
		return;
	}

	IDirect3DSurface9* old_depth = NULL;
	if ( g_pd3dDevice->GetDepthStencilSurface( &old_depth ) != D3D_OK )
		old_depth = NULL;

	D3DVIEWPORT9 old_viewport;
	g_pd3dDevice->GetViewport( &old_viewport );

	// everything below is pixel state except the vertex side, which is saved by hand like RenderDrawData does
	IDirect3DStateBlock9* state = NULL;
	if ( g_pd3dDevice->CreateStateBlock( D3DSBT_PIXELSTATE, &state ) != D3D_OK || state->Capture( ) != D3D_OK ) {
		if ( state )
			state->Release( );
		layer->Release( );
		old_target->Release( );
		if ( old_depth )
			old_depth->Release( );
		ImGui_ImplDX9_RenderDrawData( draw_data );
		return;
	}

	// ── layer pass. depth unbound: z is off, and a depth surface smaller than the target fails the draw.
	// scissor off for the clear, which honours it.
	g_pd3dDevice->SetRenderTarget( 0, layer );
	g_pd3dDevice->SetDepthStencilSurface( NULL );
	g_pd3dDevice->SetRenderState( D3DRS_SCISSORTESTENABLE, false );
	g_pd3dDevice->SetRenderState( D3DRS_COLORWRITEENABLE, 0xF );
	g_pd3dDevice->Clear( 0, NULL, D3DCLEAR_TARGET, D3DCOLOR_ARGB( 0, 0, 0, 0 ), 1.f, 0 );

	g_LayerPass = true;
	ImGui_ImplDX9_RenderDrawData( draw_data );
	g_LayerPass = false;

	g_pd3dDevice->SetRenderTarget( 0, old_target );
	g_pd3dDevice->SetDepthStencilSurface( old_depth );
	g_pd3dDevice->SetViewport( &old_viewport );

	// ── composite. the layer is premultiplied ( src alpha colour blend onto 0 ), so ONE / INVSRCALPHA,
	// and the texture factor scales colour and alpha together = the whole layer at `alpha`.
	IDirect3DVertexDeclaration9* old_declaration = NULL;
	IDirect3DVertexShader9* old_vertex_shader    = NULL;
	IDirect3DVertexBuffer9* old_stream           = NULL;
	IDirect3DBaseTexture9* old_texture           = NULL;
	UINT old_stream_offset = 0, old_stream_stride = 0;
	DWORD old_texcoord_index = 0, old_texture_transform = 0;

	g_pd3dDevice->GetVertexDeclaration( &old_declaration );
	g_pd3dDevice->GetVertexShader( &old_vertex_shader );
	if ( g_pd3dDevice->GetStreamSource( 0, &old_stream, &old_stream_offset, &old_stream_stride ) != D3D_OK )
		old_stream = NULL;
	g_pd3dDevice->GetTexture( 0, &old_texture );
	// vertex state, not pixel: the block above does not carry these two
	g_pd3dDevice->GetTextureStageState( 0, D3DTSS_TEXCOORDINDEX, &old_texcoord_index );
	g_pd3dDevice->GetTextureStageState( 0, D3DTSS_TEXTURETRANSFORMFLAGS, &old_texture_transform );

	const DWORD factor = ( DWORD )( ( alpha < 0.f ? 0.f : alpha > 1.f ? 1.f : alpha ) * 255.f + 0.5f );

	g_pd3dDevice->SetPixelShader( NULL );
	g_pd3dDevice->SetVertexShader( NULL );
	g_pd3dDevice->SetFVF( D3DFVF_XYZRHW | D3DFVF_TEX1 );
	g_pd3dDevice->SetTexture( 0, g_LayerTexture );

	g_pd3dDevice->SetRenderState( D3DRS_ZENABLE, false );
	g_pd3dDevice->SetRenderState( D3DRS_ZWRITEENABLE, false );
	g_pd3dDevice->SetRenderState( D3DRS_STENCILENABLE, false );
	g_pd3dDevice->SetRenderState( D3DRS_ALPHATESTENABLE, false );
	g_pd3dDevice->SetRenderState( D3DRS_CULLMODE, D3DCULL_NONE );
	g_pd3dDevice->SetRenderState( D3DRS_FOGENABLE, false );
	g_pd3dDevice->SetRenderState( D3DRS_SCISSORTESTENABLE, false );
	g_pd3dDevice->SetRenderState( D3DRS_ALPHABLENDENABLE, true );
	g_pd3dDevice->SetRenderState( D3DRS_SEPARATEALPHABLENDENABLE, false );
	g_pd3dDevice->SetRenderState( D3DRS_BLENDOP, D3DBLENDOP_ADD );
	g_pd3dDevice->SetRenderState( D3DRS_SRCBLEND, D3DBLEND_ONE );
	g_pd3dDevice->SetRenderState( D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA );
	g_pd3dDevice->SetRenderState( D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE );
	g_pd3dDevice->SetRenderState( D3DRS_TEXTUREFACTOR, D3DCOLOR_ARGB( factor, factor, factor, factor ) );

	g_pd3dDevice->SetTextureStageState( 0, D3DTSS_COLOROP, D3DTOP_MODULATE );
	g_pd3dDevice->SetTextureStageState( 0, D3DTSS_COLORARG1, D3DTA_TEXTURE );
	g_pd3dDevice->SetTextureStageState( 0, D3DTSS_COLORARG2, D3DTA_TFACTOR );
	g_pd3dDevice->SetTextureStageState( 0, D3DTSS_ALPHAOP, D3DTOP_MODULATE );
	g_pd3dDevice->SetTextureStageState( 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE );
	g_pd3dDevice->SetTextureStageState( 0, D3DTSS_ALPHAARG2, D3DTA_TFACTOR );
	g_pd3dDevice->SetTextureStageState( 0, D3DTSS_TEXCOORDINDEX, 0 );
	g_pd3dDevice->SetTextureStageState( 0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE );
	g_pd3dDevice->SetTextureStageState( 1, D3DTSS_COLOROP, D3DTOP_DISABLE );
	g_pd3dDevice->SetTextureStageState( 1, D3DTSS_ALPHAOP, D3DTOP_DISABLE );

	// same size as the target and -0.5 on xyzrhw: each pixel samples exactly its own texel
	g_pd3dDevice->SetSamplerState( 0, D3DSAMP_MINFILTER, D3DTEXF_POINT );
	g_pd3dDevice->SetSamplerState( 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT );
	g_pd3dDevice->SetSamplerState( 0, D3DSAMP_MIPFILTER, D3DTEXF_NONE );
	g_pd3dDevice->SetSamplerState( 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP );
	g_pd3dDevice->SetSamplerState( 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP );
	g_pd3dDevice->SetSamplerState( 0, D3DSAMP_SRGBTEXTURE, false );

	struct layer_vertex_t {
		float x, y, z, rhw, u, v;
	};

	const float w                 = ( float )desc.Width - 0.5f;
	const float h                 = ( float )desc.Height - 0.5f;
	const layer_vertex_t quad[ 4 ] = { { -0.5f, -0.5f, 0.f, 1.f, 0.f, 0.f },
		                               { w, -0.5f, 0.f, 1.f, 1.f, 0.f },
		                               { -0.5f, h, 0.f, 1.f, 0.f, 1.f },
		                               { w, h, 0.f, 1.f, 1.f, 1.f } };

	g_pd3dDevice->DrawPrimitiveUP( D3DPT_TRIANGLESTRIP, 2, quad, sizeof( layer_vertex_t ) );

	// DrawPrimitiveUP nulled stream 0 — put back what the game's bind cache thinks is there
	g_pd3dDevice->SetStreamSource( 0, old_stream, old_stream_offset, old_stream_stride );
	g_pd3dDevice->SetVertexDeclaration( old_declaration );
	g_pd3dDevice->SetVertexShader( old_vertex_shader );
	g_pd3dDevice->SetTexture( 0, old_texture );
	g_pd3dDevice->SetTextureStageState( 0, D3DTSS_TEXCOORDINDEX, old_texcoord_index );
	g_pd3dDevice->SetTextureStageState( 0, D3DTSS_TEXTURETRANSFORMFLAGS, old_texture_transform );

	state->Apply( );
	state->Release( );

	IUnknown* const held[] = { old_declaration, old_vertex_shader, old_stream, old_texture, old_depth, layer, old_target };
	for ( IUnknown* reference : held ) {
		if ( reference )
			reference->Release( );
	}
}

void ImGui_ImplDX9_DestroyFontsTexture( )
{
	if ( g_FontTexture ) {
		g_FontTexture->Release( );
		g_FontTexture                = nullptr;
		ImGui::GetIO( ).Fonts->TexID = nullptr;
	} // We copied g_pFontTextureView to io.Fonts->TexID so let's clear that as well.
}

bool ImGui_ImplDX9_Init( IDirect3DDevice9* device )
{
	// Setup back-end capabilities flags
	ImGuiIO& io            = ImGui::GetIO( );
	io.BackendRendererName = "imgui_impl_dx9";
	io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset; // We can honor the ImDrawCmd::VtxOffset field, allowing for large meshes.

	g_pd3dDevice = device;
	g_pd3dDevice->AddRef( );
	return true;
}

void ImGui_ImplDX9_Shutdown( )
{
	ImGui_ImplDX9_InvalidateDeviceObjects( );
	if ( g_pd3dDevice ) {
		g_pd3dDevice->Release( );
		g_pd3dDevice = NULL;
	}
}

static bool ImGui_ImplDX9_CreateFontsTexture( )
{
	/* botox: expand alpha8 straight into the locked rect. GetTexDataAsRGBA32 kept a W*H*4 copy in ONE
	   malloc (~100mb for our cjk atlas), unchecked - in a 3gb 32 bit csgo it failed at random and
	   imgui wrote through null (crash on config load). a colour build ( LoadColor ) is still rgba. */
	ImGuiIO& io              = ImGui::GetIO( );
	ImFontAtlas* const atlas = io.Fonts;
	unsigned char* alpha     = NULL;
	int width = atlas->TexWidth, height = atlas->TexHeight;
	if ( !atlas->TexPixelsRGBA32 )
		atlas->GetTexDataAsAlpha8( &alpha, &width, &height );
	if ( !atlas->TexPixelsRGBA32 && !alpha )
		return false;

	// botox: alpha builds go up as A8 when the card samples it ( all dx9 parts do, checked anyway )
	bool a8 = false;
	if ( !atlas->TexPixelsRGBA32 ) {
		IDirect3D9* d3d = NULL;
		D3DDEVICE_CREATION_PARAMETERS cp;
		D3DDISPLAYMODE mode;
		if ( g_pd3dDevice->GetDirect3D( &d3d ) == D3D_OK && d3d ) {
			a8 = g_pd3dDevice->GetCreationParameters( &cp ) == D3D_OK && g_pd3dDevice->GetDisplayMode( 0, &mode ) == D3D_OK &&
			     d3d->CheckDeviceFormat( cp.AdapterOrdinal, cp.DeviceType, mode.Format, D3DUSAGE_DYNAMIC, D3DRTYPE_TEXTURE, D3DFMT_A8 ) == D3D_OK;
			d3d->Release( );
		}
	}

	// botox: mip chain for the dpi scale. under 100 % glyphs minify: plain bilinear skips texels and the 1 bit
	// ( monochrome ) menu font loses whole strokes. 2x2 box levels + MIPFILTER LINEAR = real grey coverage.
	// level 2 = 25 %. alpha builds only ( a colour LoadColor build stays 1 level ). a level that can't be
	// allocated ends the chain there: never an unfilled level
	unsigned char* levels_px[ k_font_mip_levels ] = { alpha };
	int levels_w[ k_font_mip_levels ] = { width }, levels_h[ k_font_mip_levels ] = { height };
	int levels = 1;
	if ( !atlas->TexPixelsRGBA32 ) {
		for ( ; levels < k_font_mip_levels; levels++ ) {
			const int pw = levels_w[ levels - 1 ], ph = levels_h[ levels - 1 ];
			const int w = pw > 1 ? pw >> 1 : 1, h = ph > 1 ? ph >> 1 : 1;
			unsigned char* const dst = ( unsigned char* )IM_ALLOC( ( size_t )w * h );
			if ( !dst )
				break;
			const unsigned char* const src = levels_px[ levels - 1 ];
			for ( int y = 0; y < h; y++ ) {
				const unsigned char* const r0 = src + ( size_t )pw * ( y * 2 < ph ? y * 2 : ph - 1 );
				const unsigned char* const r1 = src + ( size_t )pw * ( y * 2 + 1 < ph ? y * 2 + 1 : ph - 1 );
				for ( int x = 0; x < w; x++ ) {
					const int x0 = x * 2 < pw ? x * 2 : pw - 1, x1 = x * 2 + 1 < pw ? x * 2 + 1 : pw - 1;
					dst[ ( size_t )w * y + x ] = ( unsigned char )( ( r0[ x0 ] + r0[ x1 ] + r1[ x0 ] + r1[ x1 ] + 2 ) / 4 );
				}
			}
			levels_px[ levels ] = dst;
			levels_w[ levels ]  = w;
			levels_h[ levels ]  = h;
		}
	}
	const auto free_levels = [ & ]( ) {
		for ( int l = 1; l < levels; l++ )
			IM_FREE( levels_px[ l ] );
	};

	// Upload texture to graphics system
	g_FontTexture = NULL;
	HRESULT hr    = g_pd3dDevice->CreateTexture( width, height, levels, D3DUSAGE_DYNAMIC, a8 ? D3DFMT_A8 : D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
	                                             &g_FontTexture, NULL );
	// botox: no room for the chain = old single level, text just minifies rough again
	if ( hr < 0 && levels > 1 ) {
		free_levels( );
		levels = 1;
		hr     = g_pd3dDevice->CreateTexture( width, height, 1, D3DUSAGE_DYNAMIC, a8 ? D3DFMT_A8 : D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
		                                      &g_FontTexture, NULL );
	}
	if ( hr < 0 ) {
		// botox: one line per failure streak, NewFrame retries every frame ( text = blocks meanwhile )
		static int s_fails = 0;
		if ( s_fails++ % 256 == 0 )
			botox_dbg_log( "FONT: texture create failed hr=%08x %dx%d a8=%d fails=%d", ( unsigned )hr, width, height, ( int )a8, s_fails );
		g_FontTexture = NULL;
		return false;
	}
	for ( int level = 0; level < levels; level++ ) {
		const int lw = levels_w[ level ], lh = levels_h[ level ];
		D3DLOCKED_RECT tex_locked_rect;
		if ( g_FontTexture->LockRect( level, &tex_locked_rect, NULL, 0 ) != D3D_OK ) {
			// botox: drop it, or NewFrame sees a texture and never retries - text stays blank
			botox_dbg_log( "FONT: texture lock failed %dx%d a8=%d level=%d", width, height, ( int )a8, level );
			free_levels( );
			g_FontTexture->Release( );
			g_FontTexture = NULL;
			return false;
		}
		for ( int y = 0; y < lh; y++ ) {
			unsigned char* const row = ( unsigned char* )tex_locked_rect.pBits + tex_locked_rect.Pitch * y;
			if ( atlas->TexPixelsRGBA32 ) {
				memcpy( row, atlas->TexPixelsRGBA32 + ( size_t )lw * y, ( size_t )lw * 4 );
				continue;
			}
			const unsigned char* const src = levels_px[ level ] + ( size_t )lw * y;
			if ( a8 ) {
				memcpy( row, src, ( size_t )lw );
				continue;
			}
			unsigned int* const dst = ( unsigned int* )row;
			for ( int x = 0; x < lw; x++ )
				dst[ x ] = IM_COL32( 255, 255, 255, src[ x ] );
		}
		g_FontTexture->UnlockRect( level );
	}
	free_levels( );
	g_FontTextureA8 = a8;
	double bytes    = 0.0;
	for ( int level = 0; level < levels; level++ )
		bytes += ( double )levels_w[ level ] * levels_h[ level ] * ( a8 ? 1 : 4 );
	botox_dbg_log( "FONT: texture up %dx%d %s levels=%d = %.1f mb", width, height, a8 ? "a8" : "argb", levels, bytes / ( 1024.0 * 1024.0 ) );

	// Store our identifier
	io.Fonts->TexID = ( ImTextureID )g_FontTexture;

	return true;
}

bool ImGui_ImplDX9_CreateDeviceObjects( )
{
	if ( !g_pd3dDevice )
		return false;
	if ( !ImGui_ImplDX9_CreateFontsTexture( ) )
		return false;
	return true;
}

void ImGui_ImplDX9_InvalidateDeviceObjects( )
{
	if ( !g_pd3dDevice )
		return;
	if ( g_pVB ) {
		g_pVB->Release( );
		g_pVB = NULL;
	}
	if ( g_pIB ) {
		g_pIB->Release( );
		g_pIB = NULL;
	}
	if ( g_FontTexture ) {
		g_FontTexture->Release( );
		g_FontTexture                = NULL;
		ImGui::GetIO( ).Fonts->TexID = NULL;
	} // We copied g_pFontTextureView to io.Fonts->TexID so let's clear that as well.
	// botox: the fade layer, so Reset ( reset.cpp ) and unload ( Shutdown ) both drop it
	if ( g_LayerTexture ) {
		g_LayerTexture->Release( );
		g_LayerTexture = NULL;
	}
}

void ImGui_ImplDX9_NewFrame( )
{
	if ( !g_FontTexture )
		ImGui_ImplDX9_CreateDeviceObjects( );
}
