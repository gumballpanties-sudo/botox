#include "flip_world.h"
#include "../../../game/sdk/includes/includes.h"
#include "../../../globals/includes/includes.h"
#include "fx_compat.h"
#include "render_queue.h"
#include "stream_guard.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <d3dx9.h>
#include <iterator>

extern void botox_dbg_log( const char* fmt, ... );

namespace
{
	const float* s_candidate = nullptr;

	bool is_projection( const float* m )
	{
		return m[ 12 ] != 0.f || m[ 13 ] != 0.f || m[ 14 ] != 0.f;
	}

	bool same( const float* a, const float* b )
	{
		return std::memcmp( a, b, sizeof( float ) * 16 ) == 0;
	}

	bool near_equal( const float* a, const float* b )
	{
		for ( int i = 0; i < 16; ++i )
			if ( std::fabsf( a[ i ] - b[ i ] ) > 1e-4f * std::max( 1.f, std::fabsf( b[ i ] ) ) )
				return false;
		return true;
	}

	constexpr char k_copy_shader[] = "sampler2D s0:register(s0);"
	                                 "float4 main(float2 uv:TEXCOORD0):COLOR0{return tex2D(s0,uv);}";
}

bool n_flip_world::impl_t::wanted( ) const
{
	return GET_VARIABLE( g_variables.m_flip_world, bool ) && !this->m_failed.load( std::memory_order_relaxed ) &&
	       !g_ctx.m_unloading.load( std::memory_order_relaxed ) && g_interfaces.m_engine_client && g_interfaces.m_engine_client->is_in_game( );
}

void n_flip_world::impl_t::on_render_start( )
{
	this->m_game_thread.store( GetCurrentThreadId( ), std::memory_order_relaxed );

	if ( !GET_VARIABLE( g_variables.m_flip_world, bool ) )
		this->m_failed.store( false, std::memory_order_relaxed );

	if ( !this->wanted( ) ) {
		this->restore( this->m_engine );
		this->restore( this->m_client );

		if ( this->m_mirroring.exchange( false ) )
			botox_dbg_log( "FLIP: off" );
	}

	this->hand( this->m_mirroring.load( std::memory_order_relaxed ) && !GET_VARIABLE( g_variables.m_flip_world_mirror_hand, bool ) );

	this->m_frame_mirrored = false;

	// count must be 1 here (no view pushed) or this is a stack entry: push_2d_view only trusts a match
	if ( g_interfaces.m_engine_client )
		s_candidate = &g_interfaces.m_engine_client->get_world_to_screen_matrix( ).data[ 0 ][ 0 ];
}

void n_flip_world::impl_t::on_post_screen_space_effects( const c_view_setup* setup )
{
	// once per frame: a nested overlay RenderView would mirror the frame back
	if ( !setup || this->m_frame_mirrored || !this->wanted( ) )
		return;

	const view_rect_t rect{ setup->m_x, setup->m_y, setup->m_width, setup->m_height };

	// dead pass = no image flip, so drop the input / w2s mirror with it
	const auto pass = [ this, rect ] {
		if ( !n_fx_compat::run( g_interfaces.m_direct_device, "flip_world", [ & ] { this->execute( rect ); } ) )
			this->m_failed.store( true );
	};

	if ( !n_render_queue::submit( pass ) )
		pass( );

	this->m_frame_mirrored = true;

	if ( !this->m_mirroring.exchange( true ) )
		botox_dbg_log( "FLIP: on view %d,%d %dx%d", rect.x, rect.y, rect.w, rect.h );
}

// gun is inside the mirror: other hand model so it lands on the usual side, sway/tracers follow the mirror
void n_flip_world::impl_t::hand( const bool hold )
{
	static c_cconvar* righthand = g_convars[ HASH_BT( "cl_righthand" ) ];
	if ( !righthand )
		return;

	const int cur = righthand->get_int( ) ? 1 : 0;

	if ( hold ) {
		if ( this->m_hand_own < 0 )
			this->m_hand_own = cur;

		if ( const int want = this->m_hand_own ? 0 : 1; cur != want ) {
			righthand->set_value( want );
			botox_dbg_log( "FLIP: cl_righthand %d -> %d (own %d)", cur, want, this->m_hand_own );
		}
	} else if ( this->m_hand_own >= 0 ) {
		righthand->set_value( this->m_hand_own );
		botox_dbg_log( "FLIP: cl_righthand back to %d", this->m_hand_own );
		this->m_hand_own = -1;
	}
}

void n_flip_world::impl_t::on_push_2d_view( )
{
	if ( !this->m_frame_mirrored || !this->wanted( ) )
		return;

	float* engine = const_cast< float* >( &g_interfaces.m_engine_client->get_world_to_screen_matrix( ).data[ 0 ][ 0 ] );
	if ( engine != s_candidate )
		return;

	this->m_engine.m_matrix = engine;

	if ( this->m_engine.m_held && same( engine, this->m_engine.m_written ) )
		return;

	if ( !this->m_client_resolved ) {
		this->m_client_resolved = true;

		if ( unsigned char* site = g_modules[ CLIENT_DLL ].find_pattern( "0F 10 05 ? ? ? ? 8D 85 ? ? ? ? B9" ) )
			this->m_client.m_matrix = reinterpret_cast< float* >( *reinterpret_cast< std::uintptr_t* >( site + 3 ) + 176 );

		botox_dbg_log( "FLIP: engine w2s %p client copy %p", engine, this->m_client.m_matrix );
	}

	if ( this->m_client.m_matrix && !( this->m_client.m_held && same( this->m_client.m_matrix, this->m_client.m_written ) ) ) {
		if ( near_equal( this->m_client.m_matrix, engine ) )
			this->apply( this->m_client );
		else if ( !this->m_client_mismatch_logged ) {
			this->m_client_mismatch_logged = true;
			botox_dbg_log( "FLIP: client copy != engine w2s, left alone ( %.3f %.3f %.3f %.3f vs %.3f %.3f %.3f %.3f )", this->m_client.m_matrix[ 0 ],
			               this->m_client.m_matrix[ 1 ], this->m_client.m_matrix[ 2 ], this->m_client.m_matrix[ 3 ], engine[ 0 ], engine[ 1 ],
			               engine[ 2 ], engine[ 3 ] );
		}
	}

	this->apply( this->m_engine );
}

const view_matrix_t& n_flip_world::impl_t::on_world_to_screen_matrix( const view_matrix_t& matrix )
{
	// render thread only: job threads ask during the 3D pass and need the true matrix
	if ( !this->m_mirroring.load( std::memory_order_relaxed ) || GetCurrentThreadId( ) != this->m_render_thread.load( std::memory_order_relaxed ) )
		return matrix;

	const float* m = &matrix.data[ 0 ][ 0 ];
	if ( !is_projection( m ) || ( this->m_engine.m_held && same( m, this->m_engine.m_written ) ) )
		return matrix;

	// never thread_local: manual mapped image has no tls index. one thread reaches here
	static view_matrix_t copy{ };
	copy = matrix;
	for ( int c = 0; c < 4; ++c )
		copy.data[ 0 ][ c ] = -copy.data[ 0 ][ c ];

	return copy;
}

void n_flip_world::impl_t::on_mouse_input( float* x )
{
	if ( x && this->m_mirroring.load( std::memory_order_relaxed ) )
		*x = -*x;
}

void n_flip_world::impl_t::on_create_move( c_user_cmd* cmd )
{
	if ( !cmd || !this->m_mirroring.load( std::memory_order_relaxed ) )
		return;

	cmd->m_side_move = -cmd->m_side_move;

	const int lr = cmd->m_buttons & ( in_moveleft | in_moveright );
	if ( lr == in_moveleft || lr == in_moveright )
		cmd->m_buttons ^= in_moveleft | in_moveright;
}

void n_flip_world::impl_t::apply( slot_t& slot )
{
	if ( !slot.m_matrix || ( slot.m_held && same( slot.m_matrix, slot.m_written ) ) || !is_projection( slot.m_matrix ) )
		return;

	for ( int c = 0; c < 4; ++c )
		slot.m_matrix[ c ] = -slot.m_matrix[ c ];

	std::memcpy( slot.m_written, slot.m_matrix, sizeof( slot.m_written ) );
	slot.m_held = true;
}

void n_flip_world::impl_t::restore( slot_t& slot )
{
	// only a matrix we wrote goes back: a fresh engine write is already the true one
	if ( slot.m_held && slot.m_matrix && same( slot.m_matrix, slot.m_written ) )
		for ( int c = 0; c < 4; ++c )
			slot.m_matrix[ c ] = -slot.m_matrix[ c ];

	slot.m_held = false;
}

bool n_flip_world::impl_t::build( IDirect3DDevice9* device, const D3DSURFACE_DESC& description )
{
	if ( this->m_texture && this->m_description.Width == description.Width && this->m_description.Height == description.Height &&
	     this->m_description.Format == description.Format && this->m_description.MultiSampleType == description.MultiSampleType )
		return true;

	this->release_targets( );

	if ( FAILED( device->CreateTexture( description.Width, description.Height, 1, D3DUSAGE_RENDERTARGET, description.Format, D3DPOOL_DEFAULT,
	                                    &this->m_texture, nullptr ) ) ||
	     FAILED( this->m_texture->GetSurfaceLevel( 0, &this->m_surface ) ) ||
	     ( description.MultiSampleType != D3DMULTISAMPLE_NONE &&
	       FAILED( device->CreateRenderTarget( description.Width, description.Height, description.Format, D3DMULTISAMPLE_NONE, 0, FALSE,
	                                           &this->m_resolve, nullptr ) ) ) ) {
		botox_dbg_log( "FLIP: target %ux%u fmt %d msaa %d creation FAILED, flip off", description.Width, description.Height,
		               static_cast< int >( description.Format ), static_cast< int >( description.MultiSampleType ) );
		this->release_targets( );
		return false;
	}

	this->m_description = description;

	botox_dbg_log( "FLIP: target %ux%u fmt %d msaa %d", description.Width, description.Height, static_cast< int >( description.Format ),
	               static_cast< int >( description.MultiSampleType ) );
	return true;
}

void n_flip_world::impl_t::release_targets( )
{
	if ( this->m_resolve ) {
		this->m_resolve->Release( );
		this->m_resolve = nullptr;
	}

	if ( this->m_surface ) {
		this->m_surface->Release( );
		this->m_surface = nullptr;
	}

	if ( this->m_texture ) {
		this->m_texture->Release( );
		this->m_texture = nullptr;
	}

	this->m_description = { };
}

void n_flip_world::impl_t::on_device_lost( )
{
	this->release_targets( );

	if ( this->m_shader ) {
		this->m_shader->Release( );
		this->m_shader = nullptr;
	}
}

void n_flip_world::impl_t::on_release( )
{
	this->m_mirroring.store( false );
	this->restore( this->m_engine );
	this->restore( this->m_client );
	this->hand( false );
}

// render thread when queued, calling thread when not
void n_flip_world::impl_t::execute( const view_rect_t rect )
{
	IDirect3DDevice9* device = g_interfaces.m_direct_device;
	if ( !device || this->m_failed.load( std::memory_order_relaxed ) )
		return;

	if ( const unsigned long thread = GetCurrentThreadId( ); thread != this->m_game_thread.load( std::memory_order_relaxed ) )
		this->m_render_thread.store( thread, std::memory_order_relaxed );

	if ( !this->m_shader ) {
		ID3DXBuffer *code = nullptr, *errors = nullptr;
		D3DXCompileShader( k_copy_shader, sizeof( k_copy_shader ) - 1, nullptr, nullptr, "main", "ps_3_0", 0, &code, &errors, nullptr );

		if ( errors ) {
			botox_dbg_log( "FLIP: shader: %s", static_cast< const char* >( errors->GetBufferPointer( ) ) );
			errors->Release( );
		}

		if ( !code || FAILED( device->CreatePixelShader( static_cast< const DWORD* >( code->GetBufferPointer( ) ), &this->m_shader ) ) ) {
			this->m_shader = nullptr;
			this->m_failed.store( true );
			botox_dbg_log( "FLIP: shader FAILED, flip off" );
		}

		if ( code )
			code->Release( );

		if ( !this->m_shader )
			return;
	}

	IDirect3DSurface9* target = nullptr;
	if ( FAILED( device->GetRenderTarget( 0, &target ) ) || !target )
		return;

	D3DSURFACE_DESC description{ };
	if ( FAILED( target->GetDesc( &description ) ) ) {
		target->Release( );
		return;
	}

	if ( !this->build( device, description ) ) {
		this->m_failed.store( true );
		target->Release( );
		return;
	}

	HRESULT blit = device->StretchRect( target, nullptr, this->m_resolve ? this->m_resolve : this->m_surface, nullptr, D3DTEXF_NONE );
	if ( SUCCEEDED( blit ) && this->m_resolve )
		blit = device->StretchRect( this->m_resolve, nullptr, this->m_surface, nullptr, D3DTEXF_NONE );

	if ( FAILED( blit ) ) {
		static bool logged = false;
		if ( !logged ) {
			logged = true;
			botox_dbg_log( "FLIP: frame copy failed %#lx", static_cast< unsigned long >( blit ) );
		}
		target->Release( );
		return;
	}

	const int width  = static_cast< int >( description.Width );
	const int height = static_cast< int >( description.Height );

	int x = std::clamp( rect.x, 0, width ), y = std::clamp( rect.y, 0, height );
	int w = std::clamp( rect.w, 0, width - x ), h = std::clamp( rect.h, 0, height - y );
	if ( w <= 0 || h <= 0 ) {
		x = 0, y = 0, w = width, h = height;
	}

	IDirect3DPixelShader9* old_pixel_shader             = nullptr;
	IDirect3DVertexShader9* old_vertex_shader           = nullptr;
	IDirect3DVertexDeclaration9* old_vertex_declaration = nullptr;
	IDirect3DBaseTexture9* old_texture                  = nullptr;
	D3DVIEWPORT9 old_viewport{ };
	DWORD fvf = 0;

	constexpr D3DRENDERSTATETYPE k_states[] = { D3DRS_ZENABLE,       D3DRS_ZWRITEENABLE,      D3DRS_ALPHABLENDENABLE, D3DRS_ALPHATESTENABLE,
	                                            D3DRS_CULLMODE,      D3DRS_SCISSORTESTENABLE, D3DRS_SRGBWRITEENABLE,  D3DRS_COLORWRITEENABLE,
	                                            D3DRS_STENCILENABLE, D3DRS_CLIPPLANEENABLE,   D3DRS_FILLMODE };
	constexpr DWORD k_values[] = { FALSE, FALSE, FALSE, FALSE, D3DCULL_NONE, FALSE, FALSE, 0x0f, FALSE, 0, D3DFILL_SOLID };
	constexpr D3DSAMPLERSTATETYPE k_samplers[] = { D3DSAMP_ADDRESSU,  D3DSAMP_ADDRESSV,  D3DSAMP_MINFILTER,
	                                               D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER, D3DSAMP_SRGBTEXTURE };
	constexpr DWORD k_sampler_values[] = { D3DTADDRESS_CLAMP, D3DTADDRESS_CLAMP, D3DTEXF_POINT, D3DTEXF_POINT, D3DTEXF_NONE, 0 };

	DWORD old_states[ std::size( k_states ) ]{ };
	DWORD old_samplers[ std::size( k_samplers ) ]{ };

	device->GetPixelShader( &old_pixel_shader );
	device->GetVertexShader( &old_vertex_shader );
	device->GetVertexDeclaration( &old_vertex_declaration );
	device->GetFVF( &fvf );
	device->GetTexture( 0, &old_texture );
	device->GetViewport( &old_viewport );

	for ( std::size_t i = 0; i < std::size( k_states ); ++i ) {
		device->GetRenderState( k_states[ i ], &old_states[ i ] );
		device->SetRenderState( k_states[ i ], k_values[ i ] );
	}

	for ( std::size_t i = 0; i < std::size( k_samplers ); ++i ) {
		device->GetSamplerState( 0, k_samplers[ i ], &old_samplers[ i ] );
		device->SetSamplerState( 0, k_samplers[ i ], k_sampler_values[ i ] );
	}

	n_stream_guard::state_t streams{ };
	streams.capture( device );

	const D3DVIEWPORT9 viewport{ 0, 0, description.Width, description.Height, 0.f, 1.f };
	device->SetViewport( &viewport );
	device->SetVertexShader( nullptr );
	device->SetVertexDeclaration( nullptr );
	device->SetFVF( D3DFVF_XYZRHW | D3DFVF_TEX1 );
	device->SetPixelShader( this->m_shader );
	device->SetTexture( 0, this->m_texture );

	struct vertex_t {
		float m_x, m_y, m_z, m_rhw, m_u, m_v;
	};

	// texel x+w-1-(c-x) lands on pixel c, exact centres under point sampling
	const float left = static_cast< float >( x ) - 0.5f, right = static_cast< float >( x + w ) - 0.5f;
	const float top = static_cast< float >( y ) - 0.5f, bottom = static_cast< float >( y + h ) - 0.5f;
	const float u_left = static_cast< float >( x + w ) / static_cast< float >( width ), u_right = static_cast< float >( x ) / static_cast< float >( width );
	const float v_top = static_cast< float >( y ) / static_cast< float >( height ), v_bottom = static_cast< float >( y + h ) / static_cast< float >( height );

	const vertex_t quad[ 4 ] = {
		{ left, top, 0.f, 1.f, u_left, v_top },
		{ right, top, 0.f, 1.f, u_right, v_top },
		{ left, bottom, 0.f, 1.f, u_left, v_bottom },
		{ right, bottom, 0.f, 1.f, u_right, v_bottom },
	};

	n_fx_compat::draw_up( device, D3DPT_TRIANGLESTRIP, 2, quad, sizeof( vertex_t ) );

	device->SetPixelShader( old_pixel_shader );
	device->SetVertexShader( old_vertex_shader );
	if ( fvf )
		device->SetFVF( fvf );
	device->SetVertexDeclaration( old_vertex_declaration );
	device->SetTexture( 0, old_texture );
	device->SetViewport( &old_viewport );
	streams.restore( device );

	for ( std::size_t i = 0; i < std::size( k_states ); ++i )
		device->SetRenderState( k_states[ i ], old_states[ i ] );

	for ( std::size_t i = 0; i < std::size( k_samplers ); ++i )
		device->SetSamplerState( 0, k_samplers[ i ], old_samplers[ i ] );

	if ( old_texture )
		old_texture->Release( );
	if ( old_vertex_declaration )
		old_vertex_declaration->Release( );
	if ( old_vertex_shader )
		old_vertex_shader->Release( );
	if ( old_pixel_shader )
		old_pixel_shader->Release( );

	target->Release( );
}
