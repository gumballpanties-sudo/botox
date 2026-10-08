#include <cstdio>
#include <cstring>
#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"
#include "../hooks.h"
#include "flash_probe.h"

void __fastcall n_detoured_functions::view_draw_fade( void* ecx, void* edx, unsigned char* color, c_material* material,
                                                      bool map_full_texture_to_screen )
{
	static auto original = g_hooks.m_view_draw_fade.get_original< decltype( &n_detoured_functions::view_draw_fade ) >( );
	HOOK_SCOPE_OR_BAIL( original( ecx, edx, color, material, map_full_texture_to_screen ) );

	if ( color && material ) {
		const char* name = material->get_name( );

		if ( name && std::strstr( name, "flashbang" ) ) {
			++g_flash_probe.m_flash_fades;

			for ( int i = 0; i < 4; i++ )
				g_flash_probe.m_last_color[ i ] = static_cast< int >( color[ i ] );

			std::snprintf( g_flash_probe.m_last_name, sizeof( g_flash_probe.m_last_name ), "%s", name );
		} else
			++g_flash_probe.m_other_fades;
	}

	original( ecx, edx, color, material, map_full_texture_to_screen );
}

void flash_probe_sample( IDirect3DDevice9* device, const int stage )
{
	if ( !device || !g_flash_probe.m_blind )
		return;

	static unsigned long last = 0;
	static bool paired        = false;

	if ( stage == 0 ) {
		const unsigned long now = GetTickCount( );

		if ( now - last < 250 )
			return;

		last   = now;
		paired = true;
	} else if ( !paired )
		return;

	int( *out )[ 3 ] = stage == 0 ? g_flash_probe.m_pre : stage == 1 ? g_flash_probe.m_post : g_flash_probe.m_final;

	for ( int point = 0; point < 5; point++ )
		out[ point ][ 0 ] = out[ point ][ 1 ] = out[ point ][ 2 ] = -1;

	IDirect3DSurface9* back_buffer = nullptr;

	if ( FAILED( device->GetBackBuffer( 0, 0, D3DBACKBUFFER_TYPE_MONO, &back_buffer ) ) || !back_buffer )
		return;

	D3DSURFACE_DESC description{ };

	if ( FAILED( back_buffer->GetDesc( &description ) ) ) {
		back_buffer->Release( );
		return;
	}

	IDirect3DSurface9* resolve = nullptr;
	IDirect3DSurface9* one     = nullptr;
	IDirect3DSurface9* sysmem  = nullptr;

	const bool multisampled = description.MultiSampleType != D3DMULTISAMPLE_NONE;

	bool ok = SUCCEEDED( device->CreateRenderTarget( 1, 1, description.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &one, nullptr ) ) &&
	          SUCCEEDED( device->CreateOffscreenPlainSurface( 1, 1, description.Format, D3DPOOL_SYSTEMMEM, &sysmem, nullptr ) );

	if ( ok && multisampled )
		ok = SUCCEEDED( device->CreateRenderTarget( description.Width, description.Height, description.Format, D3DMULTISAMPLE_NONE, 0, FALSE,
		                                            &resolve, nullptr ) ) &&
		     SUCCEEDED( device->StretchRect( back_buffer, nullptr, resolve, nullptr, D3DTEXF_NONE ) );

	const float fractions[ 5 ][ 2 ] = { { 0.5f, 0.5f }, { 0.25f, 0.25f }, { 0.75f, 0.25f }, { 0.25f, 0.75f }, { 0.75f, 0.75f } };

	for ( int point = 0; ok && point < 5; point++ ) {
		const long x = static_cast< long >( description.Width * fractions[ point ][ 0 ] );
		const long y = static_cast< long >( description.Height * fractions[ point ][ 1 ] );

		RECT pixel_rect{ x, y, x + 1, y + 1 };

		if ( FAILED( device->StretchRect( multisampled ? resolve : back_buffer, &pixel_rect, one, nullptr, D3DTEXF_NONE ) ) )
			break;

		D3DLOCKED_RECT locked{ };

		if ( FAILED( device->GetRenderTargetData( one, sysmem ) ) || FAILED( sysmem->LockRect( &locked, nullptr, D3DLOCK_READONLY ) ) )
			break;

		const unsigned int pixel = *static_cast< const unsigned int* >( locked.pBits );

		out[ point ][ 0 ] = static_cast< int >( ( pixel >> 16 ) & 0xff );
		out[ point ][ 1 ] = static_cast< int >( ( pixel >> 8 ) & 0xff );
		out[ point ][ 2 ] = static_cast< int >( pixel & 0xff );

		sysmem->UnlockRect( );
	}

	if ( stage == 2 ) {
		paired                = false;
		g_flash_probe.m_fresh = true;
	}

	if ( sysmem )
		sysmem->Release( );

	if ( one )
		one->Release( );

	if ( resolve )
		resolve->Release( );

	back_buffer->Release( );
}
