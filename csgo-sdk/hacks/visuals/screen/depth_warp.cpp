#include "depth_warp.h"
#include "../../../game/sdk/includes/includes.h"
#include "../../../globals/includes/includes.h"
#include "depth_source.h"
#include "depth_warp_shader.h"
#include "fx_compat.h"
#include "frame_copy.h"
#include "render_queue.h"
#include "serial_render.h"
#include "sim_clock.h"
#include "stream_guard.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <d3dx9.h>
#include <format>
#include <string>

namespace
{
	bool shift_depth( )
	{
		return GET_VARIABLE( g_variables.m_channel_shift, bool ) && GET_VARIABLE( g_variables.m_channel_shift_depth, bool );
	}

	bool wanted( )
	{
		return GET_VARIABLE( g_variables.m_depth_distort, bool ) || shift_depth( );
	}

	int blend_group( const int mode )
	{
		return mode <= 0 ? 0 : ( mode <= 10 ? 1 : ( mode <= 19 ? 2 : 3 ) );
	}

	float ping_pong( const double t )
	{
		const double p = std::fmod( t, 2.0 );
		return static_cast< float >( p < 1.0 ? p : 2.0 - p );
	}
}

void n_depth_warp::impl_t::release( )
{
	const auto drop = []( auto& resource ) {
		if ( resource ) {
			resource->Release( );
			resource = nullptr;
		}
	};

	for ( auto& row : this->m_shaders )
		for ( auto& shader : row )
			drop( shader );

	for ( auto& shader : this->m_write )
		drop( shader );

	drop( this->m_history_surface );
	drop( this->m_history_texture );
	drop( this->m_resolve_surface );
	drop( this->m_frame_surface );
	drop( this->m_frame_texture );

	this->m_history_effect = -1;
	this->m_width          = 0;
	this->m_height         = 0;
	this->m_format         = D3DFMT_UNKNOWN;
	this->m_multi_sample   = D3DMULTISAMPLE_NONE;
}

void n_depth_warp::impl_t::on_device_lost( )
{
	this->release( );
	this->m_active = false;
}

IDirect3DPixelShader9* n_depth_warp::impl_t::ensure_shader( IDirect3DDevice9* device, const int effect, const int write, const int blend )
{
	IDirect3DPixelShader9*& slot = write ? this->m_write[ effect - 9 ] : this->m_shaders[ effect ][ blend ];
	bool& failed                 = write ? this->m_write_failed[ effect - 9 ] : this->m_failed[ effect ][ blend ];

	if ( slot || failed )
		return slot;

	const std::string source =
		std::vformat( "#define EFFECT {:d}\n#define WRITE {:d}\n#define BLEND {:d}\n", std::make_format_args( effect, write, blend ) ) +
		k_depth_warp_body;

	ID3DXBuffer* code   = nullptr;
	ID3DXBuffer* errors = nullptr;

	HRESULT result = D3DXCompileShader( source.c_str( ), static_cast< UINT >( source.size( ) ), nullptr, nullptr, "main", "ps_3_0", 0, &code, &errors,
	                                    nullptr );

	if ( FAILED( result ) || !code ) {
		if ( errors ) {
			g_console.print< n_console::log_level::WARNING >( static_cast< const char* >( errors->GetBufferPointer( ) ) );
			errors->Release( );
		}

		if ( code )
			code->Release( );

		failed = true;
		return nullptr;
	}

	if ( errors )
		errors->Release( );

	result = device->CreatePixelShader( static_cast< const DWORD* >( code->GetBufferPointer( ) ), &slot );
	code->Release( );

	if ( FAILED( result ) || !slot ) {
		slot   = nullptr;
		failed = true;
		return nullptr;
	}

	g_console.print( std::vformat( "depth distort: shader built, effect {:d} write {:d} blend {:d}", std::make_format_args( effect, write, blend ) ).c_str( ) );

	return slot;
}

bool n_depth_warp::impl_t::build( IDirect3DDevice9* device, const D3DSURFACE_DESC& description, const bool history )
{
	const int width  = static_cast< int >( description.Width );
	const int height = static_cast< int >( description.Height );

	const auto drop = []( auto& resource ) {
		if ( resource ) {
			resource->Release( );
			resource = nullptr;
		}
	};

	if ( !this->m_frame_texture || this->m_width != width || this->m_height != height || this->m_format != description.Format ||
	     this->m_multi_sample != description.MultiSampleType ) {
		drop( this->m_history_surface );
		drop( this->m_history_texture );
		drop( this->m_resolve_surface );
		drop( this->m_frame_surface );
		drop( this->m_frame_texture );
		this->m_history_effect = -1;

		if ( FAILED( device->CreateTexture( description.Width, description.Height, 1, D3DUSAGE_RENDERTARGET, description.Format, D3DPOOL_DEFAULT,
		                                    &this->m_frame_texture, nullptr ) ) ||
		     FAILED( this->m_frame_texture->GetSurfaceLevel( 0, &this->m_frame_surface ) ) ||
		     ( description.MultiSampleType != D3DMULTISAMPLE_NONE &&
		       FAILED( device->CreateRenderTarget( description.Width, description.Height, description.Format, D3DMULTISAMPLE_NONE, 0, FALSE,
		                                           &this->m_resolve_surface, nullptr ) ) ) ) {
			g_console.print< n_console::log_level::WARNING >( "depth distort: frame copy creation failed" );
			drop( this->m_resolve_surface );
			drop( this->m_frame_surface );
			drop( this->m_frame_texture );
			return false;
		}

		this->m_width        = width;
		this->m_height       = height;
		this->m_format       = description.Format;
		this->m_multi_sample = description.MultiSampleType;
	}

	if ( history && !this->m_history_texture ) {
		if ( FAILED( device->CreateTexture( description.Width, description.Height, 1, D3DUSAGE_RENDERTARGET, description.Format, D3DPOOL_DEFAULT,
		                                    &this->m_history_texture, nullptr ) ) ||
		     FAILED( this->m_history_texture->GetSurfaceLevel( 0, &this->m_history_surface ) ) ) {
			g_console.print< n_console::log_level::WARNING >( "depth distort: slit scan history creation failed" );
			drop( this->m_history_surface );
			drop( this->m_history_texture );
			return false;
		}

		this->m_history_effect = -1;
	}

	return true;
}

void n_depth_warp::impl_t::on_post_screen_space_effects( c_view_setup* setup )
{
	if ( !setup )
		return;

	const float clock = n_sim_clock::now( g_interfaces.m_global_vars_base );

	if ( !wanted( ) && !this->m_active ) {
		g_serial_render.idle( );
		return;
	}

	frame_t frame{ };
	frame.m_znear = setup->m_znear;
	frame.m_zfar  = setup->m_zfar;
	frame.m_clock = clock;
	frame.m_frame = g_interfaces.m_global_vars_base ? g_interfaces.m_global_vars_base->m_frame_count : 0;

	// always the queue when there is one: raw d3d from here races the render thread ( see ambient_occlusion )
	g_serial_render.idle( );

	const auto pass = [ this, frame ] { n_fx_compat::run( g_interfaces.m_direct_device, "depth_warp", [ & ] { this->execute( frame ); } ); };

	if ( n_render_queue::submit( pass ) )
		return;

	pass( );
}

void n_depth_warp::impl_t::execute( const frame_t frame )
{
	IDirect3DDevice9* device = g_interfaces.m_direct_device;

	if ( !device )
		return;

	if ( !wanted( ) ) {
		this->release( );
		this->m_active = false;
		return;
	}

	const bool distort = GET_VARIABLE( g_variables.m_depth_distort, bool );
	const int effect   = distort ? std::clamp( GET_VARIABLE( g_variables.m_depth_distort_effect, int ), 0, 9 ) + 1 : 0;
	const int blend    = std::clamp( GET_VARIABLE( g_variables.m_depth_distort_blend, int ), 0, 30 );
	const bool scan    = effect == 9 || effect == 10;

	IDirect3DSurface9* target = nullptr;

	if ( FAILED( device->GetRenderTarget( 0, &target ) ) || !target )
		return;

	D3DSURFACE_DESC description{ };

	if ( FAILED( target->GetDesc( &description ) ) || !this->build( device, description, scan ) ) {
		target->Release( );
		return;
	}

	this->m_active = true;

	IDirect3DPixelShader9* shader = this->ensure_shader( device, effect, 0, effect ? blend_group( blend ) : 0 );
	IDirect3DPixelShader9* writer = scan ? this->ensure_shader( device, effect, 1, 0 ) : nullptr;
	IDirect3DTexture9* depth      = g_depth_source.ensure( device, description ) ? g_depth_source.texture( ) : nullptr;

	D3DSURFACE_DESC depth_description{ };

	// must be frame sized, else it's not the scene's depth
	if ( !shader || ( scan && !writer ) || !depth || FAILED( depth->GetLevelDesc( 0, &depth_description ) ) ||
	     depth_description.Width != description.Width || depth_description.Height != description.Height ) {
		target->Release( );
		return;
	}

	if ( FAILED( n_frame_copy::copy( device, target, this->m_resolve_surface, this->m_frame_surface ) ) ) {
		target->Release( );
		return;
	}

	if ( scan && this->m_history_effect != effect ) {
		device->ColorFill( this->m_history_surface, nullptr, D3DCOLOR_XRGB( 0, 0, 0 ) );
		this->m_history_effect = effect;
	}

	IDirect3DSurface9* old_depth_stencil                = nullptr;
	IDirect3DPixelShader9* old_pixel_shader             = nullptr;
	IDirect3DVertexShader9* old_vertex_shader           = nullptr;
	IDirect3DVertexDeclaration9* old_vertex_declaration = nullptr;
	constexpr unsigned long k_stages = 3;
	constexpr unsigned int k_constants = 18;

	IDirect3DBaseTexture9* old_texture[ k_stages ]{ };
	D3DVIEWPORT9 old_viewport{ };

	DWORD z_enable, z_write, alpha_blend, alpha_test, cull, scissor, srgb_write, fvf, stencil, colour_write, clip_planes, fill_mode, point_size;

	DWORD address_u[ k_stages ], address_v[ k_stages ], min_filter[ k_stages ], mag_filter[ k_stages ], mip_filter[ k_stages ],
		srgb_texture[ k_stages ];

	float old_constants[ k_constants ][ 4 ]{ };

	if ( FAILED( device->GetDepthStencilSurface( &old_depth_stencil ) ) )
		old_depth_stencil = nullptr;

	device->GetPixelShader( &old_pixel_shader );
	device->GetVertexShader( &old_vertex_shader );
	device->GetViewport( &old_viewport );
	device->GetRenderState( D3DRS_STENCILENABLE, &stencil );
	device->GetRenderState( D3DRS_COLORWRITEENABLE, &colour_write );
	device->GetRenderState( D3DRS_CLIPPLANEENABLE, &clip_planes );
	device->GetRenderState( D3DRS_FILLMODE, &fill_mode );
	device->GetRenderState( D3DRS_ZENABLE, &z_enable );
	device->GetRenderState( D3DRS_ZWRITEENABLE, &z_write );
	device->GetRenderState( D3DRS_ALPHABLENDENABLE, &alpha_blend );
	device->GetRenderState( D3DRS_ALPHATESTENABLE, &alpha_test );
	device->GetRenderState( D3DRS_CULLMODE, &cull );
	device->GetRenderState( D3DRS_SCISSORTESTENABLE, &scissor );
	device->GetRenderState( D3DRS_SRGBWRITEENABLE, &srgb_write );
	device->GetRenderState( D3DRS_POINTSIZE, &point_size );

	for ( unsigned long stage = 0; stage < k_stages; ++stage ) {
		device->GetTexture( stage, &old_texture[ stage ] );
		device->GetSamplerState( stage, D3DSAMP_ADDRESSU, &address_u[ stage ] );
		device->GetSamplerState( stage, D3DSAMP_ADDRESSV, &address_v[ stage ] );
		device->GetSamplerState( stage, D3DSAMP_MINFILTER, &min_filter[ stage ] );
		device->GetSamplerState( stage, D3DSAMP_MAGFILTER, &mag_filter[ stage ] );
		device->GetSamplerState( stage, D3DSAMP_MIPFILTER, &mip_filter[ stage ] );
		device->GetSamplerState( stage, D3DSAMP_SRGBTEXTURE, &srgb_texture[ stage ] );
	}

	device->GetFVF( &fvf );
	device->GetVertexDeclaration( &old_vertex_declaration );
	device->GetPixelShaderConstantF( 0, old_constants[ 0 ], k_constants );

	n_stream_guard::state_t streams{ };
	streams.capture( device );

	g_depth_source.resolve( device, old_depth_stencil, frame.m_frame );

	struct vertex_t {
		float m_x, m_y, m_z, m_rhw, m_u, m_v;
	};

	const float width  = static_cast< float >( this->m_width );
	const float height = static_cast< float >( this->m_height );

	const vertex_t quad[ 4 ] = { { -0.5f, -0.5f, 0.f, 1.f, 0.f, 0.f },
		                         { width - 0.5f, -0.5f, 0.f, 1.f, 1.f, 0.f },
		                         { -0.5f, height - 0.5f, 0.f, 1.f, 0.f, 1.f },
		                         { width - 0.5f, height - 0.5f, 0.f, 1.f, 1.f, 1.f } };

	const D3DVIEWPORT9 viewport{ 0, 0, static_cast< DWORD >( this->m_width ), static_cast< DWORD >( this->m_height ), 0.f, 1.f };

	device->SetDepthStencilSurface( nullptr );
	device->SetVertexShader( nullptr );
	device->SetVertexDeclaration( nullptr );
	device->SetFVF( D3DFVF_XYZRHW | D3DFVF_TEX1 );
	device->SetRenderState( D3DRS_STENCILENABLE, FALSE );
	device->SetRenderState( D3DRS_CLIPPLANEENABLE, 0 );
	device->SetRenderState( D3DRS_FILLMODE, D3DFILL_SOLID );
	device->SetRenderState( D3DRS_ZENABLE, FALSE );
	device->SetRenderState( D3DRS_ZWRITEENABLE, FALSE );
	device->SetRenderState( D3DRS_ALPHABLENDENABLE, FALSE );
	device->SetRenderState( D3DRS_ALPHATESTENABLE, FALSE );
	device->SetRenderState( D3DRS_CULLMODE, D3DCULL_NONE );
	device->SetRenderState( D3DRS_SCISSORTESTENABLE, FALSE );
	device->SetRenderState( D3DRS_SRGBWRITEENABLE, FALSE );
	device->SetRenderState( D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE );

	// s0 depth point clamp, s1 frame linear mirror, s2 history linear wrap
	constexpr DWORD k_address[ k_stages ] = { D3DTADDRESS_CLAMP, D3DTADDRESS_MIRROR, D3DTADDRESS_WRAP };

	for ( unsigned long stage = 0; stage < k_stages; ++stage ) {
		const DWORD filter = stage ? D3DTEXF_LINEAR : D3DTEXF_POINT;
		device->SetSamplerState( stage, D3DSAMP_ADDRESSU, k_address[ stage ] );
		device->SetSamplerState( stage, D3DSAMP_ADDRESSV, k_address[ stage ] );
		device->SetSamplerState( stage, D3DSAMP_MIPFILTER, D3DTEXF_NONE );
		device->SetSamplerState( stage, D3DSAMP_SRGBTEXTURE, 0 );
		device->SetSamplerState( stage, D3DSAMP_MINFILTER, filter );
		device->SetSamplerState( stage, D3DSAMP_MAGFILTER, filter );
	}

	float amount = 0.f, angle = 0.f, period = 0.f, option = 0.f;

	switch ( effect ) {
	case 1:
		amount = GET_VARIABLE( g_variables.m_depth_distort_magnitude, float );
		break;
	case 2:
		angle  = GET_VARIABLE( g_variables.m_depth_distort_angle, float );
		option = GET_VARIABLE( g_variables.m_depth_distort_inverse, bool ) ? 1.f : 0.f;
		break;
	case 3:
		angle = GET_VARIABLE( g_variables.m_depth_distort_angle, float );
		break;
	case 4:
	case 5:
		amount = GET_VARIABLE( g_variables.m_depth_distort_amplitude, float );
		angle  = GET_VARIABLE( g_variables.m_depth_distort_angle, float );
		period = GET_VARIABLE( g_variables.m_depth_distort_period, float );
		break;
	case 6:
		amount = GET_VARIABLE( g_variables.m_depth_distort_wave_amplitude, float );
		angle  = GET_VARIABLE( g_variables.m_depth_distort_wave_angle, float );
		period = GET_VARIABLE( g_variables.m_depth_distort_wave_period, float );
		option = static_cast< float >( GET_VARIABLE( g_variables.m_depth_distort_wave_type, int ) );
		break;
	case 9:
		option = static_cast< float >( std::clamp( GET_VARIABLE( g_variables.m_depth_distort_slit_direction, int ), 0, 3 ) );
		break;
	default:
		break;
	}

	const int animate      = GET_VARIABLE( g_variables.m_depth_distort_animate, int );
	const float anim_speed = GET_VARIABLE( g_variables.m_depth_distort_anim_speed, float );
	const float position   = GET_VARIABLE( g_variables.m_depth_distort_slit_position, float );

	++this->m_scan_frame;

	float slit[ 4 ]{ };

	if ( effect == 9 ) {
		const int direction = static_cast< int >( option );
		const double pixel  = std::max( GET_VARIABLE( g_variables.m_depth_distort_slit_speed, float ), 0.01f ) / ( direction < 2 ? width : height );
		const double scroll = std::fmod( static_cast< double >( this->m_scan_frame ) * pixel, 1.0 );
		const float column  = animate ? ping_pong( frame.m_clock * anim_speed ) : position;
		const bool forward  = direction == 0 || direction == 2;

		slit[ 0 ] = column;
		slit[ 1 ] = static_cast< float >( forward ? scroll : std::fabs( 1.0 - scroll ) );
		slit[ 2 ] = static_cast< float >( forward ? scroll - column : scroll - std::fmod( 1.0 - column, 1.0 ) );
		slit[ 3 ] = static_cast< float >( pixel );
	}

	const float fill = animate ? ping_pong( frame.m_clock * 0.1 * anim_speed ) : position;

	const bool shift = shift_depth( );
	const auto shift_x = [ & ]( const auto& variable ) { return shift ? GET_VARIABLE( variable, float ) / width : 0.f; };
	const auto shift_y = [ & ]( const auto& variable ) { return shift ? GET_VARIABLE( variable, float ) / height : 0.f; };

	const bool planet  = effect == 8;
	const c_color border = GET_VARIABLE( g_variables.m_depth_distort_border_color, c_color );

	const float constants[ k_constants ][ 4 ] = {
		{ 1.f / width, 1.f / height, width, height },
		{ static_cast< float >( frame.m_clock * 1000.0 ), static_cast< float >( animate ), anim_speed, height / width },
		{ planet ? GET_VARIABLE( g_variables.m_depth_distort_planet_rot_x, float ) : GET_VARIABLE( g_variables.m_depth_distort_center_x, float ),
	      planet ? GET_VARIABLE( g_variables.m_depth_distort_planet_rot_y, float ) : GET_VARIABLE( g_variables.m_depth_distort_center_y, float ),
	      GET_VARIABLE( g_variables.m_depth_distort_offset_x, float ), GET_VARIABLE( g_variables.m_depth_distort_offset_y, float ) },
		{ GET_VARIABLE( g_variables.m_depth_distort_radius, float ), GET_VARIABLE( g_variables.m_depth_distort_tension, float ), amount, angle },
		{ period, GET_VARIABLE( g_variables.m_depth_distort_phase, float ), GET_VARIABLE( g_variables.m_depth_distort_inner_radius, float ),
	      GET_VARIABLE( g_variables.m_depth_distort_aspect, float ) },
		{ GET_VARIABLE( g_variables.m_depth_distort_aspect_angle, float ), GET_VARIABLE( g_variables.m_depth_distort_use_offset, bool ) ? 1.f : 0.f,
	      option, static_cast< float >( std::max( GET_VARIABLE( g_variables.m_depth_distort_splices, int ), 1 ) ) },
		{ GET_VARIABLE( g_variables.m_depth_distort_depth_distance, float ), GET_VARIABLE( g_variables.m_depth_distort_depth_range, float ),
	      1.f / std::max( GET_VARIABLE( g_variables.m_depth_distort_depth_ramp, float ), 1.f ),
	      static_cast< float >( GET_VARIABLE( g_variables.m_depth_distort_depth_mode, int ) ) },
		{ static_cast< float >( blend ), distort ? GET_VARIABLE( g_variables.m_depth_distort_opacity, float ) : 0.f,
	      GET_VARIABLE( g_variables.m_depth_distort_fade, bool ) ? 1.f : 0.f, GET_VARIABLE( g_variables.m_depth_distort_protect, bool ) ? 1.f : 0.f },
		{ frame.m_znear, frame.m_zfar, 0.f, 0.f },
		{ shift_x( g_variables.m_channel_shift_red_x ), shift_y( g_variables.m_channel_shift_red_y ), shift_x( g_variables.m_channel_shift_green_x ),
	      shift_y( g_variables.m_channel_shift_green_y ) },
		{ shift_x( g_variables.m_channel_shift_blue_x ), shift_y( g_variables.m_channel_shift_blue_y ), shift ? 1.f : 0.f, 0.f },
		{ GET_VARIABLE( g_variables.m_channel_shift_depth_distance, float ), GET_VARIABLE( g_variables.m_channel_shift_depth_range, float ),
	      1.f / std::max( GET_VARIABLE( g_variables.m_channel_shift_depth_ramp, float ), 1.f ),
	      static_cast< float >( GET_VARIABLE( g_variables.m_channel_shift_depth_mode, int ) ) },
		{ GET_VARIABLE( g_variables.m_depth_distort_drunk_strength_x, float ), GET_VARIABLE( g_variables.m_depth_distort_drunk_strength_y, float ),
	      GET_VARIABLE( g_variables.m_depth_distort_drunk_speed, float ), 0.f },
		{ GET_VARIABLE( g_variables.m_depth_distort_drunk_angle, float ), GET_VARIABLE( g_variables.m_depth_distort_drunk_angle_speed, float ),
	      GET_VARIABLE( g_variables.m_depth_distort_planet_scale, float ), GET_VARIABLE( g_variables.m_depth_distort_planet_rot_z, float ) },
		{ GET_VARIABLE( g_variables.m_depth_distort_planet_offset_x, float ), GET_VARIABLE( g_variables.m_depth_distort_planet_offset_y, float ),
	      std::clamp( GET_VARIABLE( g_variables.m_depth_distort_planet_seam, float ), 0.5f, 1.f ), 0.f },
		{ slit[ 0 ], slit[ 1 ], slit[ 2 ], slit[ 3 ] },
		{ border[ 0 ] / 255.f, border[ 1 ] / 255.f, border[ 2 ] / 255.f, border[ 3 ] / 255.f },
		{ fill, 0.f, 0.f, 0.f } };

	device->SetPixelShaderConstantF( 0, constants[ 0 ], k_constants );
	device->SetTexture( 0, depth );
	device->SetTexture( 1, this->m_frame_texture );
	device->SetTexture( 2, nullptr );
	device->SetViewport( &viewport );

	if ( scan ) {
		device->SetRenderTarget( 0, this->m_history_surface );
		device->SetViewport( &viewport );
		device->SetPixelShader( writer );
		n_fx_compat::draw_up( device, D3DPT_TRIANGLESTRIP, 2, quad, sizeof( vertex_t ) );
		device->SetTexture( 2, this->m_history_texture );
	}

	device->SetRenderTarget( 0, target );
	device->SetViewport( &viewport );
	device->SetPixelShader( shader );
	n_fx_compat::draw_up( device, D3DPT_TRIANGLESTRIP, 2, quad, sizeof( vertex_t ) );

	device->SetPixelShaderConstantF( 0, old_constants[ 0 ], k_constants );
	device->SetPixelShader( old_pixel_shader );
	device->SetVertexShader( old_vertex_shader );

	if ( fvf )
		device->SetFVF( fvf );

	device->SetVertexDeclaration( old_vertex_declaration );

	streams.restore( device );

	for ( unsigned long stage = 0; stage < k_stages; ++stage ) {
		device->SetTexture( stage, old_texture[ stage ] );
		device->SetSamplerState( stage, D3DSAMP_ADDRESSU, address_u[ stage ] );
		device->SetSamplerState( stage, D3DSAMP_ADDRESSV, address_v[ stage ] );
		device->SetSamplerState( stage, D3DSAMP_MINFILTER, min_filter[ stage ] );
		device->SetSamplerState( stage, D3DSAMP_MAGFILTER, mag_filter[ stage ] );
		device->SetSamplerState( stage, D3DSAMP_MIPFILTER, mip_filter[ stage ] );
		device->SetSamplerState( stage, D3DSAMP_SRGBTEXTURE, srgb_texture[ stage ] );
	}

	device->SetRenderState( D3DRS_POINTSIZE, point_size );
	device->SetRenderState( D3DRS_SRGBWRITEENABLE, srgb_write );
	device->SetRenderState( D3DRS_SCISSORTESTENABLE, scissor );
	device->SetRenderState( D3DRS_CULLMODE, cull );
	device->SetRenderState( D3DRS_ALPHATESTENABLE, alpha_test );
	device->SetRenderState( D3DRS_ALPHABLENDENABLE, alpha_blend );
	device->SetRenderState( D3DRS_ZWRITEENABLE, z_write );
	device->SetRenderState( D3DRS_ZENABLE, z_enable );
	device->SetRenderState( D3DRS_FILLMODE, fill_mode );
	device->SetRenderState( D3DRS_CLIPPLANEENABLE, clip_planes );
	device->SetRenderState( D3DRS_COLORWRITEENABLE, colour_write );
	device->SetRenderState( D3DRS_STENCILENABLE, stencil );
	device->SetViewport( &old_viewport );
	device->SetDepthStencilSurface( old_depth_stencil );

	if ( old_depth_stencil )
		old_depth_stencil->Release( );

	if ( old_vertex_declaration )
		old_vertex_declaration->Release( );

	for ( IDirect3DBaseTexture9* texture : old_texture ) {
		if ( texture )
			texture->Release( );
	}

	if ( old_vertex_shader )
		old_vertex_shader->Release( );

	if ( old_pixel_shader )
		old_pixel_shader->Release( );

	target->Release( );

	if ( this->m_logged_effect != effect ) {
		this->m_logged_effect = effect;
		const int shifted     = shift ? 1 : 0;
		g_console.print( std::vformat( "depth distort: effect {:d} blend {:d} shift {:d}", std::make_format_args( effect, blend, shifted ) ).c_str( ) );
	}
}
