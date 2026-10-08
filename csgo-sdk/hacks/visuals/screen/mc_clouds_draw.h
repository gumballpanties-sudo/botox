#pragma once
#include "../edicts/mc_clouds_math.h"
#include "fx_compat.h"
#include "stream_guard.h"

#include <cstdio>
#include <d3d9.h>
#include <d3dx9.h>
#include <iterator>

// d3d only, no game state: tools/mc_sky_test.cpp drives these on a real device
namespace n_mc_clouds
{
	inline IDirect3DPixelShader9* build_shader( IDirect3DDevice9* device, char* error = nullptr, const std::size_t error_size = 0 )
	{
		ID3DXBuffer* code   = nullptr;
		ID3DXBuffer* errors = nullptr;
		IDirect3DPixelShader9* shader = nullptr;

		if ( FAILED( D3DXCompileShader( k_shader, sizeof( k_shader ) - 1, nullptr, nullptr, "main", "ps_3_0", 0, &code, &errors, nullptr ) ) || !code ||
		     FAILED( device->CreatePixelShader( static_cast< const DWORD* >( code->GetBufferPointer( ) ), &shader ) ) )
			shader = nullptr;

		if ( !shader && error && error_size )
			std::snprintf( error, error_size, "%s", errors ? static_cast< const char* >( errors->GetBufferPointer( ) ) : "CreatePixelShader failed" );

		if ( code )
			code->Release( );

		if ( errors )
			errors->Release( );

		return shader;
	}

	// csgo runs d3d9ex: MANAGED is refused there, DEFAULT + DYNAMIC instead ( released before Reset )
	inline IDirect3DTexture9* build_cells( IDirect3DDevice9* device )
	{
		IDirect3DTexture9* cells = nullptr;

		if ( FAILED( device->CreateTexture( 256, 256, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &cells, nullptr ) ) &&
		     FAILED( device->CreateTexture( 256, 256, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &cells, nullptr ) ) )
			return nullptr;

		D3DLOCKED_RECT locked{ };

		if ( !cells || FAILED( cells->LockRect( 0, &locked, nullptr, 0 ) ) ) {
			if ( cells )
				cells->Release( );

			return nullptr;
		}

		cells_argb( static_cast< std::uint32_t* >( locked.pBits ), locked.Pitch / 4 );
		cells->UnlockRect( 0 );
		return cells;
	}

	/* d3d thread, right behind a view's sky + opaque world: reads that view's cViewProj ( vs c8, common_vs_fxc.h ),
	   cLightScale ( ps c30, common_ps_fxc.h ), depth func + viewport. quad at the depth clear value, so only sky pixels
	   pass. every state touched is put back ( engine caches them, no state blocks on this device ) */
	inline bool draw( IDirect3DDevice9* device, IDirect3DPixelShader9* shader, IDirect3DTexture9* cells, const float offset[ 2 ] )
	{
		float rows[ 4 ][ 4 ]{ }, light[ 4 ]{ }, rays[ 4 ][ 3 ]{ };
		D3DVIEWPORT9 viewport{ };

		if ( !device || !shader || !cells || FAILED( device->GetVertexShaderConstantF( 8, rows[ 0 ], 4 ) ) || FAILED( device->GetViewport( &viewport ) ) ||
		     !corner_rays( rows, static_cast< float >( viewport.Width ), static_cast< float >( viewport.Height ), rays ) )
			return false;

		if ( FAILED( device->GetPixelShaderConstantF( 30, light, 1 ) ) )
			light[ 3 ] = 1.f;

		DWORD z_func = D3DCMP_LESSEQUAL;
		device->GetRenderState( D3DRS_ZFUNC, &z_func );
		const bool reversed = z_func == D3DCMP_GREATER || z_func == D3DCMP_GREATEREQUAL;

		struct vertex_t {
			float m_x, m_y, m_z, m_rhw, m_ray[ 4 ], m_info[ 4 ];
		};

		vertex_t quad[ 4 ]{ };
		for ( int i = 0; i < 4; i++ )
			quad[ i ] = { static_cast< float >( viewport.X ) + ( ( i & 1 ) ? static_cast< float >( viewport.Width ) : 0.f ) - 0.5f,
				          static_cast< float >( viewport.Y ) + ( ( i & 2 ) ? static_cast< float >( viewport.Height ) : 0.f ) - 0.5f,
				          reversed ? 0.f : 1.f,
				          1.f,
				          { rays[ i ][ 0 ], rays[ i ][ 1 ], rays[ i ][ 2 ], 0.f },
				          { offset[ 0 ], offset[ 1 ], gamma_scale( light[ 3 ] ), 0.f } };

		static constexpr D3DRENDERSTATETYPE k_states[] = {
			D3DRS_ZENABLE,         D3DRS_ZWRITEENABLE, D3DRS_ZFUNC,           D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND,      D3DRS_DESTBLEND,
			D3DRS_BLENDOP,         D3DRS_SEPARATEALPHABLENDENABLE,            D3DRS_ALPHATESTENABLE,  D3DRS_CULLMODE,      D3DRS_SRGBWRITEENABLE,
			D3DRS_COLORWRITEENABLE, D3DRS_STENCILENABLE, D3DRS_CLIPPLANEENABLE, D3DRS_FILLMODE,        D3DRS_FOGENABLE };
		const DWORD values[] = { D3DZB_TRUE,
			                     FALSE,
			                     static_cast< DWORD >( reversed ? D3DCMP_GREATEREQUAL : D3DCMP_LESSEQUAL ),
			                     TRUE,
			                     D3DBLEND_SRCALPHA,
			                     D3DBLEND_INVSRCALPHA,
			                     D3DBLENDOP_ADD,
			                     FALSE,
			                     FALSE,
			                     D3DCULL_NONE,
			                     FALSE,
			                     D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE,
			                     FALSE,
			                     0,
			                     D3DFILL_SOLID,
			                     FALSE };
		static_assert( std::size( k_states ) == std::size( values ) );

		static constexpr D3DSAMPLERSTATETYPE k_samplers[] = { D3DSAMP_ADDRESSU,  D3DSAMP_ADDRESSV,  D3DSAMP_MINFILTER,
			                                                  D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER, D3DSAMP_SRGBTEXTURE };
		static constexpr DWORD k_sampler_values[]         = { D3DTADDRESS_WRAP, D3DTADDRESS_WRAP, D3DTEXF_POINT, D3DTEXF_POINT, D3DTEXF_NONE, 0 };

		DWORD old_states[ std::size( k_states ) ]{ }, old_samplers[ std::size( k_samplers ) ]{ }, old_fvf = 0;
		IDirect3DPixelShader9* old_pixel_shader             = nullptr;
		IDirect3DVertexShader9* old_vertex_shader           = nullptr;
		IDirect3DVertexDeclaration9* old_vertex_declaration = nullptr;
		IDirect3DBaseTexture9* old_texture                  = nullptr;

		for ( std::size_t i = 0; i < std::size( k_states ); i++ )
			device->GetRenderState( k_states[ i ], &old_states[ i ] );

		for ( std::size_t i = 0; i < std::size( k_samplers ); i++ )
			device->GetSamplerState( 0, k_samplers[ i ], &old_samplers[ i ] );

		device->GetPixelShader( &old_pixel_shader );
		device->GetVertexShader( &old_vertex_shader );
		device->GetVertexDeclaration( &old_vertex_declaration );
		device->GetFVF( &old_fvf );
		device->GetTexture( 0, &old_texture );

		n_stream_guard::state_t streams{ };
		streams.capture( device );

		for ( std::size_t i = 0; i < std::size( k_states ); i++ )
			device->SetRenderState( k_states[ i ], values[ i ] );

		for ( std::size_t i = 0; i < std::size( k_samplers ); i++ )
			device->SetSamplerState( 0, k_samplers[ i ], k_sampler_values[ i ] );

		device->SetTexture( 0, cells );
		device->SetPixelShader( shader );
		const bool drawn = SUCCEEDED( n_fx_compat::draw_up( device, D3DPT_TRIANGLESTRIP, 2, quad, sizeof( vertex_t ) ) );

		device->SetPixelShader( old_pixel_shader );
		device->SetVertexShader( old_vertex_shader );

		if ( old_fvf )
			device->SetFVF( old_fvf );

		device->SetVertexDeclaration( old_vertex_declaration );
		device->SetTexture( 0, old_texture );

		streams.restore( device );

		for ( std::size_t i = 0; i < std::size( k_samplers ); i++ )
			device->SetSamplerState( 0, k_samplers[ i ], old_samplers[ i ] );

		for ( std::size_t i = 0; i < std::size( k_states ); i++ )
			device->SetRenderState( k_states[ i ], old_states[ i ] );

		for ( IUnknown* held : { static_cast< IUnknown* >( old_pixel_shader ), static_cast< IUnknown* >( old_vertex_shader ),
		                         static_cast< IUnknown* >( old_vertex_declaration ), static_cast< IUnknown* >( old_texture ) } )
			if ( held )
				held->Release( );

		return drawn;
	}
}
