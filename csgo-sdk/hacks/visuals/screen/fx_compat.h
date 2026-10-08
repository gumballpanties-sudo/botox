#pragma once
#include "../../../utilities/perf/perf_watch.h"

#include <cstring>
#include <d3d9.h>
#include <d3dx9.h>
#include <iterator>
#include <vector>

/* amd safety for every screen pass. d3d thread only.
   draw_up: ps_3_0 under fixed function vertex ( XYZRHW, no vs ) is off spec. nvidia links it anyway, amd may draw
   garbage / nothing / fault. same quads go through a pass through vs_3_0 instead ( reshade does vs_3_0 + ps_3_0 too ):
   pixel coords -> clip space on the cpu against the bound viewport, so no vs constant is touched ( engine caches those ).
   rasterises the same pixels. vs path fails to build = old fixed function draw.
   run: access violation inside a pass ( driver fault ) = that pass off until reload + bindings put back, game keeps going.
   faults the driver defers past our call ( empty botox stack in the crash log ) can't be caught here. */
namespace n_fx_compat
{
	inline IDirect3DVertexShader9* s_shader[ 2 ]{ };
	inline IDirect3DVertexDeclaration9* s_declaration[ 2 ]{ };
	inline bool s_failed = false;

	// layout 0: xyzrhw + uv ( every quad ). layout 1: xyzrhw + 2x float4 ( player_stencil capsules )
	inline constexpr DWORD k_fvf[ 2 ] = { D3DFVF_XYZRHW | D3DFVF_TEX1,
		                                  D3DFVF_XYZRHW | D3DFVF_TEX2 | D3DFVF_TEXCOORDSIZE4( 0 ) | D3DFVF_TEXCOORDSIZE4( 1 ) };

	inline void on_device_lost( )
	{
		for ( int layout = 0; layout < 2; ++layout ) {
			if ( s_shader[ layout ] )
				s_shader[ layout ]->Release( );

			if ( s_declaration[ layout ] )
				s_declaration[ layout ]->Release( );

			s_shader[ layout ]      = nullptr;
			s_declaration[ layout ] = nullptr;
		}
	}

	inline bool ready( IDirect3DDevice9* device )
	{
		if ( s_shader[ 1 ] && s_declaration[ 1 ] )
			return true;

		if ( s_failed || !device )
			return false;

		static const char k_source[] = "struct v_t{float4 p:POSITION;float4 t0:TEXCOORD0;\n#ifdef TWO\nfloat4 t1:TEXCOORD1;\n#endif\n};"
		                               "v_t main(v_t v){return v;}";

		static const D3DVERTEXELEMENT9 k_one[] = { { 0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
			                                       { 0, 16, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
			                                       D3DDECL_END( ) };

		static const D3DVERTEXELEMENT9 k_two[] = { { 0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
			                                       { 0, 16, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
			                                       { 0, 32, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1 },
			                                       D3DDECL_END( ) };

		static const D3DXMACRO k_macros[] = { { "TWO", "1" }, { nullptr, nullptr } };

		bool built = true;

		for ( int layout = 0; layout < 2 && built; ++layout ) {
			ID3DXBuffer* code = nullptr;

			built = SUCCEEDED( D3DXCompileShader( k_source, sizeof( k_source ) - 1, layout ? k_macros : nullptr, nullptr, "main", "vs_3_0", 0, &code,
			                                      nullptr, nullptr ) ) &&
			        code && SUCCEEDED( device->CreateVertexShader( static_cast< const DWORD* >( code->GetBufferPointer( ) ), &s_shader[ layout ] ) ) &&
			        SUCCEEDED( device->CreateVertexDeclaration( layout ? k_two : k_one, &s_declaration[ layout ] ) );

			if ( code )
				code->Release( );
		}

		if ( !built ) {
			on_device_lost( );
			s_failed = true;
			botox_dbg_log( "FXVS: vs_3_0 path failed to build, screen passes on fixed function vertex ( amd may draw them wrong )" );
			return false;
		}

		botox_dbg_log( "FXVS: screen passes on vs_3_0" );
		return true;
	}

	inline HRESULT draw_up( IDirect3DDevice9* device, const D3DPRIMITIVETYPE type, const UINT count, const void* data, const UINT stride )
	{
		const int layout = stride == 6 * sizeof( float ) ? 0 : stride == 12 * sizeof( float ) ? 1 : -1;
		const UINT vertices = type == D3DPT_TRIANGLELIST ? count * 3 : type == D3DPT_TRIANGLESTRIP || type == D3DPT_TRIANGLEFAN ? count + 2 : 0;

		D3DVIEWPORT9 viewport{ };

		if ( layout < 0 || !vertices || !data || !ready( device ) || FAILED( device->GetViewport( &viewport ) ) || !viewport.Width ||
		     !viewport.Height ) {
			// an earlier draw this pass may have left our vs bound
			if ( layout >= 0 ) {
				device->SetVertexShader( nullptr );
				device->SetFVF( k_fvf[ layout ] );
			}

			return device->DrawPrimitiveUP( type, count, data, stride );
		}

		const UINT step   = stride / sizeof( float );
		const UINT floats = vertices * step;

		float stack[ 64 ];
		std::vector< float > heap;
		float* out = stack;

		if ( floats > std::size( stack ) ) {
			heap.resize( floats );
			out = heap.data( );
		}

		std::memcpy( out, data, floats * sizeof( float ) );

		// inverse of the d3d9 viewport transform. the -0.5 texel offsets in the data carry over unchanged
		const float scale_x  = 2.f / static_cast< float >( viewport.Width );
		const float scale_y  = -2.f / static_cast< float >( viewport.Height );
		const float offset_x = -1.f - static_cast< float >( viewport.X ) * scale_x;
		const float offset_y = 1.f - static_cast< float >( viewport.Y ) * scale_y;

		for ( float* vertex = out; vertex < out + floats; vertex += step ) {
			const float w = vertex[ 3 ] > 0.f ? 1.f / vertex[ 3 ] : 1.f;

			vertex[ 0 ] = ( vertex[ 0 ] * scale_x + offset_x ) * w;
			vertex[ 1 ] = ( vertex[ 1 ] * scale_y + offset_y ) * w;
			vertex[ 2 ] *= w;
			vertex[ 3 ] = w;
		}

		device->SetVertexDeclaration( s_declaration[ layout ] );
		device->SetVertexShader( s_shader[ layout ] );

		return device->DrawPrimitiveUP( type, count, out, stride );
	}

	inline const char* s_dead[ 16 ]{ };
	inline int s_dead_count = 0;

	inline bool dead( const char* name )
	{
		for ( int i = 0; i < s_dead_count; ++i )
			if ( !std::strcmp( s_dead[ i ], name ) )
				return true;

		return false;
	}

	inline int filter( const unsigned long code )
	{
		return code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION || code == EXCEPTION_INT_DIVIDE_BY_ZERO
		           ? EXCEPTION_EXECUTE_HANDLER
		           : EXCEPTION_CONTINUE_SEARCH;
	}

	// no unwinding objects allowed in a __try frame ( C2712 ): the callable lives in the caller
	template< typename fn_t >
	inline bool call( fn_t& fn )
	{
		__try {
			fn( );
		} __except ( filter( GetExceptionCode( ) ) ) {
			return false;
		}

		return true;
	}

	struct bindings_t {
		IDirect3DSurface9* m_target                = nullptr;
		IDirect3DSurface9* m_depth                 = nullptr;
		IDirect3DVertexShader9* m_vertex_shader    = nullptr;
		IDirect3DPixelShader9* m_pixel_shader      = nullptr;
		IDirect3DVertexDeclaration9* m_declaration = nullptr;
		DWORD m_fvf                                = 0;
		D3DVIEWPORT9 m_viewport{ };

		void capture( IDirect3DDevice9* device )
		{
			device->GetRenderTarget( 0, &this->m_target );
			device->GetDepthStencilSurface( &this->m_depth );
			device->GetVertexShader( &this->m_vertex_shader );
			device->GetPixelShader( &this->m_pixel_shader );
			device->GetVertexDeclaration( &this->m_declaration );
			device->GetFVF( &this->m_fvf );
			device->GetViewport( &this->m_viewport );
		}

		void restore( IDirect3DDevice9* device ) const
		{
			if ( this->m_target )
				device->SetRenderTarget( 0, this->m_target );

			device->SetDepthStencilSurface( this->m_depth );
			device->SetViewport( &this->m_viewport );
			device->SetVertexShader( this->m_vertex_shader );
			device->SetPixelShader( this->m_pixel_shader );

			if ( this->m_fvf )
				device->SetFVF( this->m_fvf );

			device->SetVertexDeclaration( this->m_declaration );
		}

		void release( )
		{
			for ( IUnknown* held : { static_cast< IUnknown* >( this->m_target ), static_cast< IUnknown* >( this->m_depth ),
			                         static_cast< IUnknown* >( this->m_vertex_shader ), static_cast< IUnknown* >( this->m_pixel_shader ),
			                         static_cast< IUnknown* >( this->m_declaration ) } )
				if ( held )
					held->Release( );

			*this = { };
		}
	};

	// false = pass is dead ( faulted now or earlier )
	template< typename fn_t >
	inline bool run( IDirect3DDevice9* device, const char* name, fn_t&& fn )
	{
		if ( dead( name ) )
			return false;

		bindings_t saved{ };

		if ( device )
			saved.capture( device );

		const bool clean = call( fn );

		if ( !clean ) {
			if ( s_dead_count < static_cast< int >( std::size( s_dead ) ) )
				s_dead[ s_dead_count++ ] = name;

			// a fault mid pass leaves our targets bound under the engine's state cache
			auto put_back = [ & ] { saved.restore( device ); };

			if ( device )
				call( put_back );

			botox_dbg_log( "FX: %s faulted ( driver ), off until reload. send C:\\botox\\botox_crash.log", name );
		}

		saved.release( );

		return clean;
	}
}
