#include "resolution_spoof.h"
#include "../../../game/sdk/includes/includes.h"
#include "../../../globals/includes/includes.h"
#include "../../../utilities/console/console.h"
// render scale draws raw d3d from a main thread hook, shares the mat_queue_mode hold
#include "serial_render.h"
#include "frame_dump.h"

#include <algorithm>
#include <cmath>

void n_resolution_spoof::impl_t::viewport_scale( const bool enabled )
{
	static c_cconvar* viewport_scale_var = nullptr;
	static float last_written            = 0.f;

	if ( !viewport_scale_var && !( viewport_scale_var = g_convars[ HASH_BT( "mat_viewportscale" ) ] ) )
		return;

	float wanted = 1.f;

	if ( enabled && this->m_screen_width > 0 && this->m_screen_height > 0 ) {
		int target_width  = 0;
		int target_height = 0;

		this->target_size( this->m_screen_width, this->m_screen_height, target_width, target_height );

		wanted = std::min( static_cast< float >( target_width ) / this->m_screen_width,
		                   static_cast< float >( target_height ) / this->m_screen_height );

		wanted = std::clamp( wanted, 1.f / 640.f, 1.f );
	}

	if ( fabsf( wanted - last_written ) < 0.0005f )
		return;

	viewport_scale_var->set_value( wanted );

	if ( fabsf( viewport_scale_var->get_float( ) - wanted ) > 0.005f )
		viewport_scale_var->force_value( wanted );

	last_written = wanted;

	float log_wanted = wanted;
	float log_read   = viewport_scale_var->get_float( );

	g_console.print( std::vformat( "resolution spoofer: mat_viewportscale {:.4f} ( reads back {:.4f} )",
	                               std::make_format_args( log_wanted, log_read ) )
	                     .c_str( ) );
}

bool n_resolution_spoof::impl_t::render_scale_active( )
{
	return this->enabled( ) && GET_VARIABLE( g_variables.m_resolution_spoof_render_scale, bool );
}

void n_resolution_spoof::impl_t::on_post_screen_space_effects( c_view_setup* setup )
{
	IDirect3DDevice9* device = g_interfaces.m_direct_device;

	const int view_width  = setup ? setup->m_width : 0;
	const int view_height = setup ? setup->m_height : 0;
	const int full_width  = setup && setup->m_unscaled_width > 0 ? setup->m_unscaled_width : this->m_screen_width;
	const int full_height = setup && setup->m_unscaled_height > 0 ? setup->m_unscaled_height : this->m_screen_height;

	if ( !device || !this->render_scale_active( ) || view_width <= 0 || view_height <= 0 || full_width <= 0 || full_height <= 0 ||
	     ( view_width >= full_width && view_height >= full_height ) ) {
		this->m_view_width = this->m_view_height = 0;
		g_serial_render.idle( );
		return;
	}

	this->m_view_width  = view_width;
	this->m_view_height = view_height;

	// main thread, mid queue: no draws until the material system runs serial
	if ( !g_serial_render.request( ) )
		return;

	IDirect3DSurface9* target = nullptr;
	if ( FAILED( device->GetRenderTarget( 0, &target ) ) || !target )
		return;

	D3DSURFACE_DESC description{ };

	if ( FAILED( target->GetDesc( &description ) ) || !this->build_scale_target( device, view_width, view_height, description.Format ) ) {
		target->Release( );
		return;
	}

	const LONG rendered_x = setup ? setup->m_x : 0;
	const LONG rendered_y = setup ? setup->m_y : 0;

	const RECT rendered{ rendered_x, rendered_y, rendered_x + view_width, rendered_y + view_height };

	if ( FAILED( device->StretchRect( target, &rendered, this->m_scale_surface, nullptr, D3DTEXF_NONE ) ) ) {
		static bool warned = false;
		if ( !warned ) {
			g_console.print( "resolution spoofer: render scale copy out refused" );
			warned = true;
		}

		target->Release( );
		return;
	}

	if ( GET_VARIABLE( g_variables.m_resolution_spoof_dump, bool ) ) {
		GET_VARIABLE( g_variables.m_resolution_spoof_dump, bool ) = false;

		n_frame_dump::save( device, this->m_scale_surface, "spoof_render_scale.png" );
	}

	D3DVIEWPORT9 previous_viewport{ };
	const bool had_viewport = SUCCEEDED( device->GetViewport( &previous_viewport ) );

	const D3DVIEWPORT9 full{ static_cast< DWORD >( setup ? setup->m_unscaled_x : 0 ), static_cast< DWORD >( setup ? setup->m_unscaled_y : 0 ),
	                         static_cast< DWORD >( full_width ), static_cast< DWORD >( full_height ), 0.f, 1.f };
	device->SetViewport( &full );

	IDirect3DStateBlock9* state_block = nullptr;
	bool drawn                        = false;

	if ( this->begin_quad_state( device, &state_block ) ) {
		drawn = this->draw_quad( device, this->m_scale_texture, full_width, full_height );

		this->end_quad_state( device, state_block );
	}

	if ( had_viewport )
		device->SetViewport( &previous_viewport );

	static bool warned = false;
	if ( !drawn && !warned ) {
		g_console.print( "resolution spoofer: render scale quad draw failed" );
		warned = true;
	}

	target->Release( );
}

bool n_resolution_spoof::impl_t::build_scale_target( IDirect3DDevice9* device, const int width, const int height, const D3DFORMAT format )
{
	if ( this->m_scale_surface && this->m_scale_width == width && this->m_scale_height == height && this->m_scale_format == format )
		return true;

	this->release_scale( );

	if ( FAILED( device->CreateTexture( width, height, 1, D3DUSAGE_RENDERTARGET, format, D3DPOOL_DEFAULT, &this->m_scale_texture, nullptr ) ) ||
	     FAILED( this->m_scale_texture->GetSurfaceLevel( 0, &this->m_scale_surface ) ) ) {
		this->release_scale( );

		static bool warned = false;
		if ( !warned ) {
			g_console.print( "resolution spoofer: render scale target creation failed" );
			warned = true;
		}

		return false;
	}

	this->m_scale_width  = width;
	this->m_scale_height = height;
	this->m_scale_format = format;

	int log_width  = width;
	int log_height = height;

	g_console.print( std::vformat( "resolution spoofer: render scale target {:d}x{:d}", std::make_format_args( log_width, log_height ) ).c_str( ) );

	return true;
}

void n_resolution_spoof::impl_t::release_scale( )
{
	if ( this->m_scale_surface ) {
		this->m_scale_surface->Release( );
		this->m_scale_surface = nullptr;
	}

	if ( this->m_scale_texture ) {
		this->m_scale_texture->Release( );
		this->m_scale_texture = nullptr;
	}

	this->m_scale_width  = 0;
	this->m_scale_height = 0;
	this->m_scale_format = D3DFMT_UNKNOWN;
}

void n_resolution_spoof::impl_t::release( )
{
	if ( this->m_surface ) {
		this->m_surface->Release( );
		this->m_surface = nullptr;
	}

	if ( this->m_texture ) {
		this->m_texture->Release( );
		this->m_texture = nullptr;
	}

	if ( this->m_capture_surface ) {
		this->m_capture_surface->Release( );
		this->m_capture_surface = nullptr;
	}

	if ( this->m_capture ) {
		this->m_capture->Release( );
		this->m_capture = nullptr;
	}

	this->m_capture_has_mips = false;

	for ( auto& level : this->m_chain ) {
		if ( level.m_surface )
			level.m_surface->Release( );

		if ( level.m_texture )
			level.m_texture->Release( );
	}

	this->m_chain.clear( );
	this->m_chain_screen_width  = 0;
	this->m_chain_screen_height = 0;

	this->m_width  = 0;
	this->m_height = 0;
	this->m_format = D3DFMT_UNKNOWN;
}

bool n_resolution_spoof::impl_t::build_chain( IDirect3DDevice9* device, const int screen_width, const int screen_height, const int width,
                                              const int height, const D3DFORMAT format )
{
	this->m_chain_screen_width  = screen_width;
	this->m_chain_screen_height = screen_height;

	int level_width  = screen_width;
	int level_height = screen_height;

	for ( int i = 0; i < 8; ++i ) {
		if ( level_width / 2 < width || level_height / 2 < height )
			break;

		level_width /= 2;
		level_height /= 2;

		chain_level_t level{ };
		level.m_width  = level_width;
		level.m_height = level_height;

		if ( FAILED( device->CreateTexture( level_width, level_height, 1, D3DUSAGE_RENDERTARGET, format, D3DPOOL_DEFAULT, &level.m_texture,
		                                    nullptr ) ) ||
		     FAILED( level.m_texture->GetSurfaceLevel( 0, &level.m_surface ) ) ) {
			if ( level.m_texture )
				level.m_texture->Release( );

			return false;
		}

		this->m_chain.push_back( level );
	}

	return true;
}

/* one state block per pass, not per draw (D3DSBT_ALL is costly). no block = don't draw, or every
   state below leaks into the device. */
bool n_resolution_spoof::impl_t::begin_quad_state( IDirect3DDevice9* device, IDirect3DStateBlock9** state_block )
{
	*state_block = nullptr;

	if ( FAILED( device->CreateStateBlock( D3DSBT_ALL, state_block ) ) || !*state_block ) {
		static bool warned = false;
		if ( !warned ) {
			g_console.print( "resolution spoofer: CreateStateBlock failed, skipping the pass" );
			warned = true;
		}

		return false;
	}

	this->m_quad_streams.capture( device );

	const DWORD filter = D3DTEXF_LINEAR;

	device->SetVertexShader( nullptr );
	device->SetPixelShader( nullptr );
	device->SetFVF( D3DFVF_XYZRHW | D3DFVF_TEX1 );

	device->SetRenderState( D3DRS_ZENABLE, FALSE );
	device->SetRenderState( D3DRS_ZWRITEENABLE, FALSE );
	device->SetRenderState( D3DRS_ALPHABLENDENABLE, FALSE );
	device->SetRenderState( D3DRS_ALPHATESTENABLE, FALSE );
	device->SetRenderState( D3DRS_CULLMODE, D3DCULL_NONE );
	device->SetRenderState( D3DRS_LIGHTING, FALSE );
	device->SetRenderState( D3DRS_FOGENABLE, FALSE );
	device->SetRenderState( D3DRS_SCISSORTESTENABLE, FALSE );
	device->SetRenderState( D3DRS_STENCILENABLE, FALSE );
	device->SetRenderState( D3DRS_CLIPPING, FALSE );
	device->SetRenderState( D3DRS_SRGBWRITEENABLE, FALSE );
	device->SetRenderState( D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE |
	                                                    D3DCOLORWRITEENABLE_ALPHA );

	device->SetTextureStageState( 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1 );
	device->SetTextureStageState( 0, D3DTSS_COLORARG1, D3DTA_TEXTURE );
	device->SetTextureStageState( 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1 );
	device->SetTextureStageState( 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE );
	device->SetTextureStageState( 1, D3DTSS_COLOROP, D3DTOP_DISABLE );
	device->SetTextureStageState( 1, D3DTSS_ALPHAOP, D3DTOP_DISABLE );

	device->SetSamplerState( 0, D3DSAMP_MINFILTER, filter );
	device->SetSamplerState( 0, D3DSAMP_MAGFILTER, filter );

	device->SetSamplerState( 0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR );
	device->SetSamplerState( 0, D3DSAMP_MAXMIPLEVEL, 0 );
	device->SetSamplerState( 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP );
	device->SetSamplerState( 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP );
	device->SetSamplerState( 0, D3DSAMP_SRGBTEXTURE, FALSE );

	return true;
}

bool n_resolution_spoof::impl_t::draw_quad( IDirect3DDevice9* device, IDirect3DTexture9* texture, const int quad_width, const int quad_height )
{
	if ( !texture )
		return false;

	struct vertex_t {
		float m_x, m_y, m_z, m_rhw;
		float m_u, m_v;
	};

	const float left   = -0.5f;
	const float top    = -0.5f;
	const float right  = static_cast< float >( quad_width ) - 0.5f;
	const float bottom = static_cast< float >( quad_height ) - 0.5f;

	const vertex_t vertices[ 4 ] = {
		{ left, top, 0.f, 1.f, 0.f, 0.f },
		{ right, top, 0.f, 1.f, 1.f, 0.f },
		{ left, bottom, 0.f, 1.f, 0.f, 1.f },
		{ right, bottom, 0.f, 1.f, 1.f, 1.f },
	};

	device->SetTexture( 0, texture );

	return SUCCEEDED( device->DrawPrimitiveUP( D3DPT_TRIANGLESTRIP, 2, vertices, sizeof( vertex_t ) ) );
}

void n_resolution_spoof::impl_t::end_quad_state( IDirect3DDevice9* device, IDirect3DStateBlock9* state_block )
{
	if ( !state_block )
		return;

	// DrawPrimitiveUP nulls stream 0 + indices, state blocks don't carry them, and source skips the
	// re-bind when its cache matches. restore exactly.
	this->m_quad_streams.restore( device );

	state_block->Apply( );
	state_block->Release( );
}

void n_resolution_spoof::impl_t::on_device_lost( )
{
	this->release( );
	this->release_scale( );
}

void n_resolution_spoof::impl_t::on_release( )
{
	this->net_graph_scale( false );

	this->viewport_scale( false );

	this->release( );
	this->release_scale( );
}

bool n_resolution_spoof::impl_t::enabled( )
{
	constexpr bool shelved = true;

	if ( shelved )
		return false;

	return GET_VARIABLE( g_variables.m_resolution_spoof, bool );
}

bool n_resolution_spoof::impl_t::wants_overlay_pass( )
{
	// never in a debug mode: they skip draw or resize, so the menu would land where it can't be clicked
	if ( GET_VARIABLE( g_variables.m_resolution_spoof_debug, int ) != 0 )
		return false;

	if ( this->render_scale_active( ) )
		return false;

	return this->enabled( ) && GET_VARIABLE( g_variables.m_resolution_spoof_overlay, bool ) && this->pass_is_running( );
}

bool n_resolution_spoof::impl_t::pass_is_running( )
{
	return this->m_frames_without_pass <= 2;
}

void n_resolution_spoof::impl_t::on_end_scene( )
{
	if ( this->m_frames_without_pass < 1000 )
		++this->m_frames_without_pass;
}

float n_resolution_spoof::impl_t::aspect_scale( )
{
	if ( !GET_VARIABLE( g_variables.m_aspect_ratio_enable, bool ) )
		return 1.f;

	const float wanted = GET_VARIABLE( g_variables.m_aspect_ratio, float );
	if ( wanted <= 0.f || this->m_screen_width <= 0 || this->m_screen_height <= 0 )
		return 1.f;

	const float screen = static_cast< float >( this->m_screen_width ) / static_cast< float >( this->m_screen_height );

	return std::clamp( screen / wanted, 0.25f, 4.f );
}

float n_resolution_spoof::impl_t::overlay_aspect_scale( )
{
	return GET_VARIABLE( g_variables.m_aspect_ratio_overlay, bool ) || GET_VARIABLE( g_variables.m_aspect_ratio_hud, bool )
	           ? this->aspect_scale( )
	           : 1.f;
}

void n_resolution_spoof::impl_t::target_size( const int screen_width, const int screen_height, int& width, int& height )
{
	width  = std::clamp( GET_VARIABLE( g_variables.m_resolution_spoof_width, int ), 32, std::max( screen_width, 32 ) );
	height = std::clamp( GET_VARIABLE( g_variables.m_resolution_spoof_height, int ), 32, std::max( screen_height, 32 ) );
}

bool n_resolution_spoof::impl_t::low_res_grid( float& pixels_x, float& pixels_y )
{
	if ( this->render_scale_active( ) ) {
		if ( this->m_view_width <= 0 || this->m_screen_width <= 0 )
			return false;

		pixels_x = pixels_y = static_cast< float >( this->m_screen_width ) / this->m_view_width;
		return pixels_x > 1.001f;
	}

	if ( !this->enabled( ) )
		return false;

	if ( this->m_width <= 0 || this->m_height <= 0 || this->m_chain_screen_width <= 0 || this->m_chain_screen_height <= 0 )
		return false;

	pixels_x = static_cast< float >( this->m_chain_screen_width ) / static_cast< float >( this->m_width );
	pixels_y = static_cast< float >( this->m_chain_screen_height ) / static_cast< float >( this->m_height );

	return pixels_x > 1.001f || pixels_y > 1.001f;
}

void n_resolution_spoof::impl_t::on_imgui_new_frame( )
{
	auto& io = ImGui::GetIO( );
	if ( io.DisplaySize.x <= 0.f || io.DisplaySize.y <= 0.f )
		return;

	// -FLT_MAX is imgui's "no cursor" and must stay that way
	const bool cursor = io.MousePos.x > -FLT_MAX && io.MousePos.y > -FLT_MAX;

	if ( this->wants_overlay_pass( ) && this->m_width > 0 && this->m_height > 0 ) {
		const float scale_x = static_cast< float >( this->m_width ) / io.DisplaySize.x;
		const float scale_y = static_cast< float >( this->m_height ) / io.DisplaySize.y;

		io.DisplaySize = ImVec2( static_cast< float >( this->m_width ), static_cast< float >( this->m_height ) );

		if ( cursor ) {
			io.MousePos.x *= scale_x;
			io.MousePos.y *= scale_y;
		}
	}

}

void n_resolution_spoof::impl_t::net_graph_scale( const bool enabled )
{
	static c_cconvar* net_graph_height = nullptr;
	static int stored_default          = 0;
	static int last_written            = 0;

	if ( !net_graph_height && !( net_graph_height = g_convars[ HASH_BT( "net_graphheight" ) ] ) )
		return;

	if ( !enabled ) {
		if ( stored_default != 0 ) {
			net_graph_height->set_value( stored_default );
			stored_default = 0;
			last_written   = 0;
		}

		return;
	}

	if ( this->m_height <= 0 || this->m_screen_height <= 0 )
		return;

	if ( stored_default == 0 ) {
		const int current = net_graph_height->get_int( );
		stored_default    = current > 0 ? current : 64;
	}

	const int wanted = static_cast< int >( stored_default * ( static_cast< float >( this->m_screen_height ) / this->m_height ) );

	if ( wanted != last_written && wanted > 0 ) {
		net_graph_height->set_value( wanted );
		last_written = wanted;
	}
}

void n_resolution_spoof::impl_t::on_frame_stage_notify( )
{
	// convar writes, game thread only
	this->net_graph_scale( this->enabled( ) && GET_VARIABLE( g_variables.m_resolution_spoof_net_graph, bool ) );

	this->viewport_scale( this->render_scale_active( ) );

}

void n_resolution_spoof::impl_t::on_present( IDirect3DDevice9* device, const std::function< void( ) >& draw )
{
	if ( !device )
		return;

	this->m_frames_without_pass = 0;

	const auto draw_fallback = [ & ]( ) {
		if ( !draw )
			return;

		if ( SUCCEEDED( device->BeginScene( ) ) ) {
			draw( );
			device->EndScene( );
		}
	};

	IDirect3DSurface9* back_buffer = nullptr;
	if ( FAILED( device->GetBackBuffer( 0, 0, D3DBACKBUFFER_TYPE_MONO, &back_buffer ) ) || !back_buffer ) {
		draw_fallback( );
		return;
	}

	D3DSURFACE_DESC description{ };
	if ( FAILED( back_buffer->GetDesc( &description ) ) ) {
		back_buffer->Release( );
		draw_fallback( );
		return;
	}

	this->m_screen_width  = static_cast< int >( description.Width );
	this->m_screen_height = static_cast< int >( description.Height );

	if ( !this->enabled( ) || this->render_scale_active( ) ) {
		this->release( );
		back_buffer->Release( );
		draw_fallback( );
		return;
	}

	static bool announced = false;
	if ( !announced ) {
		g_console.print( "resolution spoofer: pass reached" );
		announced = true;
	}

	const int debug_mode = GET_VARIABLE( g_variables.m_resolution_spoof_debug, int );

	if ( debug_mode == 3 ) {
		this->release( );
		back_buffer->Release( );
		draw_fallback( );
		return;
	}

	int width  = 0;
	int height = 0;

	this->target_size( static_cast< int >( description.Width ), static_cast< int >( description.Height ), width, height );

	if ( width >= static_cast< int >( description.Width ) && height >= static_cast< int >( description.Height ) ) {
		this->release( );
		back_buffer->Release( );
		draw_fallback( );

		static bool warned = false;
		if ( !warned ) {
			g_console.print( "resolution spoofer: target is not smaller than the screen" );
			warned = true;
		}

		return;
	}

	if ( !this->m_surface || this->m_width != width || this->m_height != height || this->m_format != description.Format ||
	     this->m_chain_screen_width != static_cast< int >( description.Width ) ||
	     this->m_chain_screen_height != static_cast< int >( description.Height ) ) {
		this->release( );

		bool built = SUCCEEDED( device->CreateTexture( description.Width, description.Height, 0,
		                                               D3DUSAGE_RENDERTARGET | D3DUSAGE_AUTOGENMIPMAP, description.Format, D3DPOOL_DEFAULT,
		                                               &this->m_capture, nullptr ) );

		this->m_capture_has_mips = built;

		if ( !built ) {
			built = SUCCEEDED( device->CreateTexture( description.Width, description.Height, 1, D3DUSAGE_RENDERTARGET, description.Format,
			                                          D3DPOOL_DEFAULT, &this->m_capture, nullptr ) );

			static bool warned = false;
			if ( !warned ) {
				g_console.print( "resolution spoofer: no autogen mips on the capture, falling back to the halving chain" );
				warned = true;
			}
		}

		built = built && SUCCEEDED( this->m_capture->GetSurfaceLevel( 0, &this->m_capture_surface ) ) &&
		        SUCCEEDED( device->CreateTexture( width, height, 1, D3DUSAGE_RENDERTARGET, description.Format, D3DPOOL_DEFAULT,
		                                          &this->m_texture, nullptr ) ) &&
		        SUCCEEDED( this->m_texture->GetSurfaceLevel( 0, &this->m_surface ) );

		if ( !built ) {
			this->release( );
			back_buffer->Release( );
			draw_fallback( );

			static bool warned = false;
			if ( !warned ) {
				g_console.print( "resolution spoofer: CreateTexture( RENDERTARGET ) failed" );
				warned = true;
			}

			return;
		}

		if ( !this->m_capture_has_mips &&
		     !this->build_chain( device, static_cast< int >( description.Width ), static_cast< int >( description.Height ), width, height,
		                         description.Format ) ) {
			static bool warned = false;
			if ( !warned ) {
				g_console.print( "resolution spoofer: downsample chain incomplete" );
				warned = true;
			}
		}

		// set here too: with mips build_chain never runs, and a mismatch would rebuild every frame
		this->m_chain_screen_width  = static_cast< int >( description.Width );
		this->m_chain_screen_height = static_cast< int >( description.Height );

		this->m_width  = width;
		this->m_height = height;
		this->m_format = description.Format;

		int log_width         = width;
		int log_height        = height;
		int log_screen_width  = static_cast< int >( description.Width );
		int log_screen_height = static_cast< int >( description.Height );
		int log_multisample   = static_cast< int >( description.MultiSampleType );
		int log_chain         = static_cast< int >( this->m_chain.size( ) );

		int log_mips = this->m_capture_has_mips ? 1 : 0;

		g_console.print( std::vformat( "resolution spoofer: {:d}x{:d} -> {:d}x{:d}, msaa {:d}, mips {:d}, chain {:d}",
		                               std::make_format_args( log_width, log_height, log_screen_width, log_screen_height, log_multisample,
		                                                      log_mips, log_chain ) )
		                     .c_str( ) );
	}

	if ( FAILED( device->StretchRect( back_buffer, nullptr, this->m_capture_surface, nullptr, D3DTEXF_NONE ) ) ) {
		back_buffer->Release( );
		draw_fallback( );

		static bool warned = false;
		if ( !warned ) {
			g_console.print( "resolution spoofer: back buffer copy refused" );
			warned = true;
		}

		return;
	}

	if ( this->m_capture_has_mips )
		this->m_capture->GenerateMipSubLevels( );

	const bool dump = GET_VARIABLE( g_variables.m_resolution_spoof_dump, bool );

	if ( dump )
		n_frame_dump::save( device, this->m_capture_surface, "spoof_capture.png" );

	if ( debug_mode == 1 ) {
		back_buffer->Release( );
		draw_fallback( );
		return;
	}

	IDirect3DStateBlock9* state_block = nullptr;

	if ( !this->begin_quad_state( device, &state_block ) ) {
		back_buffer->Release( );
		draw_fallback( );
		return;
	}

	const bool scene = SUCCEEDED( device->BeginScene( ) );

	if ( !scene ) {
		this->end_quad_state( device, state_block );
		back_buffer->Release( );
		draw_fallback( );

		static bool warned = false;
		if ( !warned ) {
			g_console.print( "resolution spoofer: BeginScene refused, no downscale" );
			warned = true;
		}

		return;
	}

	IDirect3DSurface9* previous_depth = nullptr;

	if ( FAILED( device->GetDepthStencilSurface( &previous_depth ) ) )
		previous_depth = nullptr;

	IDirect3DSurface9* engine_target = nullptr;

	if ( FAILED( device->GetRenderTarget( 0, &engine_target ) ) )
		engine_target = nullptr;

	device->SetDepthStencilSurface( nullptr );

	IDirect3DTexture9* source = this->m_capture;
	bool stepped              = true;

	for ( auto& level : this->m_chain ) {
		if ( debug_mode == 2 )
			break;

		if ( FAILED( device->SetRenderTarget( 0, level.m_surface ) ) || !this->draw_quad( device, source, level.m_width, level.m_height ) ) {
			static bool warned = false;
			if ( !warned ) {
				g_console.print( "resolution spoofer: chain step draw failed" );
				warned = true;
			}

			stepped = false;
			break;
		}

		source = level.m_texture;
	}

	if ( stepped && debug_mode != 2 &&
	     ( FAILED( device->SetRenderTarget( 0, this->m_surface ) ) || !this->draw_quad( device, source, width, height ) ) ) {
		static bool warned = false;
		if ( !warned ) {
			g_console.print( "resolution spoofer: target draw failed" );
			warned = true;
		}

		stepped = false;
	}

	device->EndScene( );

	if ( dump ) {
		GET_VARIABLE( g_variables.m_resolution_spoof_dump, bool ) = false;

		n_frame_dump::save( device, this->m_surface, "spoof_target.png" );
	}

	if ( engine_target )
		device->SetRenderTarget( 0, engine_target );

	device->SetDepthStencilSurface( previous_depth );

	if ( engine_target )
		engine_target->Release( );

	if ( previous_depth )
		previous_depth->Release( );

	this->end_quad_state( device, state_block );

	if ( !stepped ) {
		back_buffer->Release( );
		draw_fallback( );
		return;
	}

	if ( draw ) {
		IDirect3DSurface9* old_render_target = nullptr;
		IDirect3DSurface9* old_depth_stencil = nullptr;

		device->GetRenderTarget( 0, &old_render_target );

		if ( FAILED( device->GetDepthStencilSurface( &old_depth_stencil ) ) )
			old_depth_stencil = nullptr;

		device->SetDepthStencilSurface( nullptr );

		if ( SUCCEEDED( device->SetRenderTarget( 0, this->m_surface ) ) ) {
			if ( SUCCEEDED( device->BeginScene( ) ) ) {
				draw( );
				device->EndScene( );
			}

			if ( old_render_target )
				device->SetRenderTarget( 0, old_render_target );
		} else
			draw_fallback( );

		device->SetDepthStencilSurface( old_depth_stencil );

		if ( old_render_target )
			old_render_target->Release( );

		if ( old_depth_stencil )
			old_depth_stencil->Release( );
	}

	IDirect3DSurface9* previous_target = nullptr;
	D3DVIEWPORT9 previous_viewport{ };

	const bool had_target   = SUCCEEDED( device->GetRenderTarget( 0, &previous_target ) ) && previous_target;
	const bool had_viewport = SUCCEEDED( device->GetViewport( &previous_viewport ) );

	static bool reported = false;
	if ( !reported && had_target ) {
		reported = true;

		g_console.print( previous_target == back_buffer
		                     ? "resolution spoofer: render target at Present IS the back buffer"
		                     : "resolution spoofer: render target at Present is NOT the back buffer — the quad was landing in an engine target" );
	}

	const bool bound = SUCCEEDED( device->SetRenderTarget( 0, back_buffer ) );

	if ( bound ) {
		const D3DVIEWPORT9 full{ 0, 0, description.Width, description.Height, 0.f, 1.f };
		device->SetViewport( &full );
	} else {
		static bool warned = false;
		if ( !warned ) {
			g_console.print( "resolution spoofer: could not bind the back buffer, drawing into whatever was bound" );
			warned = true;
		}
	}

	if ( SUCCEEDED( device->BeginScene( ) ) ) {
		IDirect3DStateBlock9* up_state = nullptr;
		bool drawn                     = false;

		if ( this->begin_quad_state( device, &up_state ) ) {
			IDirect3DTexture9* up_source = debug_mode == 2 ? this->m_capture : this->m_texture;

			drawn = this->draw_quad( device, up_source, static_cast< int >( description.Width ), static_cast< int >( description.Height ) );

			this->end_quad_state( device, up_state );
		}

		device->EndScene( );

		static bool warned = false;
		if ( !drawn && !warned ) {
			g_console.print( "resolution spoofer: quad draw failed" );
			warned = true;
		}
	}

	if ( bound && had_target )
		device->SetRenderTarget( 0, previous_target );

	if ( had_viewport )
		device->SetViewport( &previous_viewport );

	if ( previous_target )
		previous_target->Release( );

	back_buffer->Release( );
}
