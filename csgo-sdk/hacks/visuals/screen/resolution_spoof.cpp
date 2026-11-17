#include "resolution_spoof.h"
#include "../../../game/sdk/includes/includes.h"
#include "../../../globals/includes/includes.h"
#include "../../../utilities/console/console.h"
#include "frame_dump.h"
#include "fx_compat.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <utility>

static constexpr D3DRENDERSTATETYPE k_render_states[] = { D3DRS_ZENABLE,           D3DRS_ZWRITEENABLE,      D3DRS_ALPHABLENDENABLE,
	                                                      D3DRS_ALPHATESTENABLE,   D3DRS_CULLMODE,          D3DRS_LIGHTING,
	                                                      D3DRS_FOGENABLE,         D3DRS_SCISSORTESTENABLE, D3DRS_STENCILENABLE,
	                                                      D3DRS_CLIPPING,          D3DRS_SRGBWRITEENABLE,   D3DRS_COLORWRITEENABLE,
	                                                      D3DRS_CLIPPLANEENABLE,   D3DRS_FILLMODE };
static constexpr DWORD k_render_values[] = { FALSE, FALSE, FALSE, FALSE, D3DCULL_NONE, FALSE, FALSE, FALSE, FALSE, FALSE, FALSE, 0x0f, 0, D3DFILL_SOLID };

struct stage_state_t {
	DWORD m_stage;
	D3DTEXTURESTAGESTATETYPE m_type;
	DWORD m_value;
};

static constexpr stage_state_t k_stage_states[] = {
	{ 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1 },   { 0, D3DTSS_COLORARG1, D3DTA_TEXTURE },
	{ 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1 },   { 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE },
	{ 0, D3DTSS_TEXCOORDINDEX, 0 },             { 0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE },
	{ 1, D3DTSS_COLOROP, D3DTOP_DISABLE },      { 1, D3DTSS_ALPHAOP, D3DTOP_DISABLE },
};

static constexpr D3DSAMPLERSTATETYPE k_sampler_states[] = { D3DSAMP_ADDRESSU,  D3DSAMP_ADDRESSV,    D3DSAMP_MINFILTER,   D3DSAMP_MAGFILTER,
	                                                        D3DSAMP_MIPFILTER, D3DSAMP_MAXMIPLEVEL, D3DSAMP_SRGBTEXTURE, D3DSAMP_MIPMAPLODBIAS };
static constexpr DWORD k_sampler_values[] = { D3DTADDRESS_CLAMP, D3DTADDRESS_CLAMP, D3DTEXF_LINEAR, D3DTEXF_LINEAR, D3DTEXF_LINEAR, 0, FALSE, 0 };

static_assert( std::size( k_render_states ) == std::size( k_render_values ) );
static_assert( std::size( k_sampler_states ) == std::size( k_sampler_values ) );

/* one axis. c0 = source px along it, source px per target px ( r ), 1 / source w, 1 / source h. c1.xy = axis.
   i0.x = ceil( r ) + 1 taps. footprint [ lo, lo + r ) in source px, every px it touches weighted by overlap ( point
   taps, lod 0: gradients are undefined in a loop ). port in tools/res_spoof_check.py */
static constexpr char k_area_shader[] = "sampler2D s0:register(s0);float4 c0:register(c0);float4 c1:register(c1);int n:register(i0);"
                                        "float4 main(float2 uv:TEXCOORD0):COLOR0{"
                                        "float lo=dot(uv,c1.xy)*c0.x-c0.y*0.5;float hi=lo+c0.y;float t=floor(lo);"
                                        "float4 sum=0.0;float total=0.0;"
                                        "[loop]for(int i=0;i<n;i++){"
                                        "float w=max(min(t+1.0,hi)-max(t,lo),0.0);"
                                        "sum+=tex2Dlod(s0,float4(lerp(uv,(t+0.5)*c0.zw,c1.xy),0.0,0.0))*w;total+=w;t+=1.0;}"
                                        "return sum/max(total,1e-6);}";

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

	if ( this->m_columns_surface ) {
		this->m_columns_surface->Release( );
		this->m_columns_surface = nullptr;
	}

	if ( this->m_columns ) {
		this->m_columns->Release( );
		this->m_columns = nullptr;
	}

	this->m_built_screen_width  = 0;
	this->m_built_screen_height = 0;

	this->m_width  = 0;
	this->m_height = 0;
	this->m_format = D3DFMT_UNKNOWN;
}

bool n_resolution_spoof::impl_t::area_shader( IDirect3DDevice9* device )
{
	if ( this->m_area_shader )
		return true;

	if ( this->m_area_failed )
		return false;

	ID3DXBuffer *code = nullptr, *errors = nullptr;
	D3DXCompileShader( k_area_shader, sizeof( k_area_shader ) - 1, nullptr, nullptr, "main", "ps_3_0", 0, &code, &errors, nullptr );

	if ( errors ) {
		g_console.print( ( std::string( "resolution spoofer: area shader: " ) + static_cast< const char* >( errors->GetBufferPointer( ) ) ).c_str( ) );
		errors->Release( );
	}

	if ( !code || FAILED( device->CreatePixelShader( static_cast< const DWORD* >( code->GetBufferPointer( ) ), &this->m_area_shader ) ) ) {
		this->m_area_shader = nullptr;
		this->m_area_failed = true;
		g_console.print( "resolution spoofer: area shader FAILED, bilinear downscale" );
	}

	if ( code )
		code->Release( );

	return this->m_area_shader != nullptr;
}

void n_resolution_spoof::impl_t::begin_quad_state( IDirect3DDevice9* device )
{
	auto& saved = this->m_quad;

	static_assert( std::size( k_render_states ) == sizeof( m_quad.m_render ) / sizeof( DWORD ) );
	static_assert( std::size( k_stage_states ) == sizeof( m_quad.m_stage ) / sizeof( DWORD ) );
	static_assert( std::size( k_sampler_states ) == sizeof( m_quad.m_sampler ) / sizeof( DWORD ) );

	device->GetVertexShader( &saved.m_vertex_shader );
	device->GetPixelShader( &saved.m_pixel_shader );
	device->GetVertexDeclaration( &saved.m_declaration );
	device->GetFVF( &saved.m_fvf );
	device->GetTexture( 0, &saved.m_texture );
	device->GetViewport( &saved.m_viewport );
	device->GetPixelShaderConstantF( 0, saved.m_constants[ 0 ], 2 );
	device->GetPixelShaderConstantI( 0, saved.m_int_constant, 1 );

	for ( std::size_t i = 0; i < std::size( k_render_states ); ++i ) {
		device->GetRenderState( k_render_states[ i ], &saved.m_render[ i ] );
		device->SetRenderState( k_render_states[ i ], k_render_values[ i ] );
	}

	for ( std::size_t i = 0; i < std::size( k_stage_states ); ++i ) {
		device->GetTextureStageState( k_stage_states[ i ].m_stage, k_stage_states[ i ].m_type, &saved.m_stage[ i ] );
		device->SetTextureStageState( k_stage_states[ i ].m_stage, k_stage_states[ i ].m_type, k_stage_states[ i ].m_value );
	}

	for ( std::size_t i = 0; i < std::size( k_sampler_states ); ++i ) {
		device->GetSamplerState( 0, k_sampler_states[ i ], &saved.m_sampler[ i ] );
		device->SetSamplerState( 0, k_sampler_states[ i ], k_sampler_values[ i ] );
	}

	saved.m_streams.capture( device );
	saved.m_captured = true;

	device->SetVertexShader( nullptr );
	device->SetPixelShader( nullptr );
	device->SetVertexDeclaration( nullptr );
	device->SetFVF( D3DFVF_XYZRHW | D3DFVF_TEX1 );
}

bool n_resolution_spoof::impl_t::draw_quad( IDirect3DDevice9* device, IDirect3DTexture9* texture, const int quad_width, const int quad_height,
                                            const bool shader )
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

	if ( shader )
		return SUCCEEDED( n_fx_compat::draw_up( device, D3DPT_TRIANGLESTRIP, 2, vertices, sizeof( vertex_t ) ) );

	// fixed function vertex AND pixel ( no ps ): legal on amd too. draw_up's vs_3_0 may still be bound
	device->SetVertexShader( nullptr );
	device->SetFVF( D3DFVF_XYZRHW | D3DFVF_TEX1 );

	return SUCCEEDED( device->DrawPrimitiveUP( D3DPT_TRIANGLESTRIP, 2, vertices, sizeof( vertex_t ) ) );
}

void n_resolution_spoof::impl_t::end_quad_state( IDirect3DDevice9* device )
{
	auto& saved = this->m_quad;
	if ( !saved.m_captured )
		return;

	// DrawPrimitiveUP nulls stream 0 + indices and source skips the re-bind when its cache matches
	saved.m_streams.restore( device );

	device->SetVertexShader( saved.m_vertex_shader );
	device->SetPixelShader( saved.m_pixel_shader );
	if ( saved.m_fvf )
		device->SetFVF( saved.m_fvf );
	device->SetVertexDeclaration( saved.m_declaration );
	device->SetTexture( 0, saved.m_texture );
	device->SetViewport( &saved.m_viewport );
	device->SetPixelShaderConstantF( 0, saved.m_constants[ 0 ], 2 );
	device->SetPixelShaderConstantI( 0, saved.m_int_constant, 1 );

	for ( std::size_t i = 0; i < std::size( k_render_states ); ++i )
		device->SetRenderState( k_render_states[ i ], saved.m_render[ i ] );

	for ( std::size_t i = 0; i < std::size( k_stage_states ); ++i )
		device->SetTextureStageState( k_stage_states[ i ].m_stage, k_stage_states[ i ].m_type, saved.m_stage[ i ] );

	for ( std::size_t i = 0; i < std::size( k_sampler_states ); ++i )
		device->SetSamplerState( 0, k_sampler_states[ i ], saved.m_sampler[ i ] );

	for ( IUnknown* held : { static_cast< IUnknown* >( saved.m_vertex_shader ), static_cast< IUnknown* >( saved.m_pixel_shader ),
	                         static_cast< IUnknown* >( saved.m_declaration ), static_cast< IUnknown* >( saved.m_texture ) } )
		if ( held )
			held->Release( );

	saved = { };
}

void n_resolution_spoof::impl_t::on_device_lost( )
{
	this->release( );

	if ( this->m_area_shader ) {
		this->m_area_shader->Release( );
		this->m_area_shader = nullptr;
	}
}

void n_resolution_spoof::impl_t::on_release( )
{
	this->net_graph_scale( false );

	this->on_device_lost( );
}

bool n_resolution_spoof::impl_t::enabled( )
{
	return GET_VARIABLE( g_variables.m_resolution_spoof, bool ) && !g_ctx.m_eject_restoring;
}

// eject = every aspect half off: r_aspectratio 0, hud / crosshair / scope widen undone by their own off branches
static bool aspect_var( const bool value )
{
	return value && GET_VARIABLE( g_variables.m_aspect_ratio_enable, bool ) && !g_ctx.m_eject_restoring;
}

bool n_resolution_spoof::impl_t::wants_overlay_pass( )
{
	// never in a debug mode: they skip draw or resize, so the menu would land where it can't be clicked
	if ( GET_VARIABLE( g_variables.m_resolution_spoof_debug, int ) != 0 || n_fx_compat::dead( "res spoof" ) )
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

bool n_resolution_spoof::impl_t::stretch_res( )
{
	return this->enabled( ) && GET_VARIABLE( g_variables.m_resolution_spoof_stretch, bool );
}

bool n_resolution_spoof::impl_t::upscale_stretches( )
{
	return this->stretch_res( ) && this->wants_overlay_pass( );
}

bool n_resolution_spoof::impl_t::aspect_on( )
{
	return this->stretch_res( ) || aspect_var( true );
}

float n_resolution_spoof::impl_t::aspect_wanted( )
{
	if ( !this->stretch_res( ) )
		return GET_VARIABLE( g_variables.m_aspect_ratio, float );

	return static_cast< float >( std::max( GET_VARIABLE( g_variables.m_resolution_spoof_width, int ), 32 ) ) /
	       static_cast< float >( std::max( GET_VARIABLE( g_variables.m_resolution_spoof_height, int ), 32 ) );
}

bool n_resolution_spoof::impl_t::aspect_hud( )
{
	return this->stretch_res( ) || aspect_var( GET_VARIABLE( g_variables.m_aspect_ratio_hud, bool ) );
}

bool n_resolution_spoof::impl_t::aspect_crosshair( )
{
	return this->stretch_res( ) || aspect_var( GET_VARIABLE( g_variables.m_aspect_ratio_overlay, bool ) );
}

bool n_resolution_spoof::impl_t::aspect_cheat_overlay( )
{
	// stretched res: the low res -> screen upscale already widens the overlay. pass not running = widen it here instead
	if ( this->stretch_res( ) )
		return !this->wants_overlay_pass( );

	return aspect_var( GET_VARIABLE( g_variables.m_aspect_ratio_cheat_overlay, bool ) );
}

float n_resolution_spoof::impl_t::aspect_scale( )
{
	if ( !this->aspect_on( ) )
		return 1.f;

	const float wanted = this->aspect_wanted( );
	if ( wanted <= 0.f || this->m_screen_width <= 0 || this->m_screen_height <= 0 )
		return 1.f;

	const float screen = static_cast< float >( this->m_screen_width ) / static_cast< float >( this->m_screen_height );

	return std::clamp( screen / wanted, 0.25f, 4.f );
}

float n_resolution_spoof::impl_t::overlay_aspect_scale( )
{
	return this->aspect_crosshair( ) || this->aspect_hud( ) ? this->aspect_scale( ) : 1.f;
}

void n_resolution_spoof::impl_t::target_size( const int screen_width, const int screen_height, int& width, int& height )
{
	height = std::clamp( GET_VARIABLE( g_variables.m_resolution_spoof_height, int ), 32, std::max( screen_height, 32 ) );

	// no stretch: the res's line count over the screen's own shape, square pixels ( a 4:3 res unstretched is black bars )
	const int wanted_width = GET_VARIABLE( g_variables.m_resolution_spoof_stretch, bool ) || screen_height <= 0
	                             ? GET_VARIABLE( g_variables.m_resolution_spoof_width, int )
	                             : static_cast< int >( lroundf( static_cast< float >( height ) * screen_width / screen_height ) );

	width = std::clamp( wanted_width, 32, std::max( screen_width, 32 ) );
}

bool n_resolution_spoof::impl_t::low_res_grid( float& pixels_x, float& pixels_y )
{
	if ( !this->enabled( ) )
		return false;

	if ( this->m_width <= 0 || this->m_height <= 0 || this->m_built_screen_width <= 0 || this->m_built_screen_height <= 0 )
		return false;

	pixels_x = static_cast< float >( this->m_built_screen_width ) / static_cast< float >( this->m_width );
	pixels_y = static_cast< float >( this->m_built_screen_height ) / static_cast< float >( this->m_height );

	return pixels_x > 1.001f || pixels_y > 1.001f;
}

void n_resolution_spoof::impl_t::crosshair_color( const int r, const int g, const int b, const int a, const bool outline )
{
	const auto byte = []( const int value ) { return static_cast< unsigned int >( std::clamp( value, 0, 255 ) ); };

	this->m_crosshair_rgba       = IM_COL32( byte( r ), byte( g ), byte( b ), byte( a ) );
	this->m_crosshair_is_outline = outline;
}

bool n_resolution_spoof::impl_t::crosshair_capture( const int x0, const int y0, const int x1, const int y1 )
{
	if ( !g_ctx.m_is_hud_weapon_being_drawn || !this->wants_overlay_pass( ) || this->m_screen_height <= 0 )
		return false;

	if ( this->m_crosshair_count < static_cast< int >( std::size( this->m_crosshair ) ) )
		this->m_crosshair[ this->m_crosshair_count++ ] = { std::min( x0, x1 ), std::min( y0, y1 ), std::max( x0, x1 ), std::max( y0, y1 ),
		                                                   this->m_crosshair_rgba, this->m_crosshair_is_outline };

	return true;
}

/* CWeaponCSBase::DrawCrosshair ( weapon_csbase.cpp:2132-2317 ) at the spoofed height. size / thickness are YRES
   ( height / 480, rounded per res ), the gap is raw px ( style 4: the whole 4 + gap; else cl_crosshairgap on top of
   a height scaled spread ), outline = x0 - cl_crosshair_outlinethickness truncated. each captured rect is read back
   as an arm ( distance from the centre band + length ) and laid out again exactly as that res would */
void n_resolution_spoof::impl_t::crosshair_flush( )
{
	const int count = std::exchange( this->m_crosshair_count, 0 );

	const int screen_width  = this->m_screen_width;
	const int screen_height = this->m_screen_height;
	const float low_width   = g_ctx.m_width;
	const float low_height  = g_ctx.m_height;

	if ( !count || screen_width <= 0 || screen_height <= 0 || low_width <= 0.f || low_height <= 0.f )
		return;

	const auto csgo_round = []( const float value ) { return static_cast< int >( std::floor( value + 0.5f ) ); };

	const float k            = low_height / static_cast< float >( screen_height );
	const int style          = g_convars.int_or( HASH_BT( "cl_crosshairstyle" ), 4 );
	const float thickness    = g_convars.float_or( HASH_BT( "cl_crosshairthickness" ), 0.5f );
	const float size         = g_convars.float_or( HASH_BT( "cl_crosshairsize" ), 5.f );
	const int raw_gap        = static_cast< int >( g_convars.float_or( HASH_BT( "cl_crosshairgap" ), 0.f ) );
	const float outline_size = g_convars.float_or( HASH_BT( "cl_crosshair_outlinethickness" ), 1.f );

	const int thick_full = std::max( 1, csgo_round( thickness * screen_height / 480.f ) );
	const int thick_low  = std::max( 1, csgo_round( thickness * low_height / 480.f ) );
	const int bar_full   = csgo_round( size * screen_height / 480.f );
	const int bar_low    = csgo_round( size * low_height / 480.f );

	const int band_full_x = screen_width / 2 - thick_full / 2;
	const int band_full_y = screen_height / 2 - thick_full / 2;
	const int band_low_x  = static_cast< int >( low_width ) / 2 - thick_low / 2;
	const int band_low_y  = static_cast< int >( low_height ) / 2 - thick_low / 2;

	const auto low_distance = [ & ]( const int distance ) {
		return style == 4 ? distance : csgo_round( static_cast< float >( distance - raw_gap ) * k ) + raw_gap;
	};

	// style 2 split bars are not bar_size long: plain scale
	const auto low_length = [ & ]( const int length ) { return length == bar_full ? bar_low : std::max( 1, csgo_round( length * k ) ); };
	const auto low_thin   = [ & ]( const int span ) { return span == thick_full ? thick_low : std::max( 1, csgo_round( span * k ) ); };

	// band shift off centre ( cl_crosshair_recoil, cam driver ) is height scaled
	const auto low_band = [ & ]( const int low, const int band_full, const int band_low ) {
		return band_low + csgo_round( static_cast< float >( low - band_full ) * k );
	};

	/* role by draw order ( left, right, top unless cl_crosshair_t, bottom, then dot; style 2 split = the arm set twice ).
	   geometry can't tell: a negative gap pulls every arm across the centre. odd counts ( weapon_debug_spread_show ) = geometry */
	enum e_role { role_left, role_right, role_top, role_bottom, role_dot, role_shape };

	const bool t_style = g_convars.int_or( HASH_BT( "cl_crosshair_t" ), 0 ) != 0;
	const int dots     = g_convars.int_or( HASH_BT( "cl_crosshairdot" ), 0 ) != 0 ? 1 : 0;
	const int arms     = t_style ? 3 : 4;

	int lines = 0;
	for ( int i = 0; i < count; ++i )
		lines += this->m_crosshair[ i ].m_outline ? 0 : 1;

	const bool by_order = lines > dots && ( lines - dots ) % arms == 0;

	const auto role_of = [ & ]( const int line ) {
		if ( !by_order || line < 0 )
			return role_shape;

		if ( dots && line == lines - 1 )
			return role_dot;

		const int arm = line % arms;
		return arm < 2 ? static_cast< e_role >( arm ) : ( arm == 2 && !t_style ? role_top : role_bottom );
	};

	const auto rebuild = [ & ]( const crosshair_rect_t& rect, const int line ) {
		crosshair_rect_t out = rect;

		const int width  = rect.m_x1 - rect.m_x0;
		const int height = rect.m_y1 - rect.m_y0;
		const float dx   = ( rect.m_x0 + rect.m_x1 ) * 0.5f - screen_width * 0.5f;
		const float dy   = ( rect.m_y0 + rect.m_y1 ) * 0.5f - screen_height * 0.5f;

		e_role role = role_of( line );

		if ( role == role_shape )
			role = fabsf( dx ) <= 1.f && fabsf( dy ) <= 1.f ? role_dot
			       : fabsf( dx ) > fabsf( dy )               ? ( dx < 0.f ? role_left : role_right )
			                                                 : ( dy < 0.f ? role_top : role_bottom );

		if ( role == role_dot ) {
			out.m_x0 = low_band( rect.m_x0, band_full_x, band_low_x );
			out.m_y0 = low_band( rect.m_y0, band_full_y, band_low_y );
			out.m_x1 = out.m_x0 + low_thin( width );
			out.m_y1 = out.m_y0 + low_thin( height );
		} else if ( role == role_left || role == role_right ) {
			out.m_y0 = low_band( rect.m_y0, band_full_y, band_low_y );
			out.m_y1 = out.m_y0 + low_thin( height );

			if ( role == role_left ) {
				out.m_x1 = band_low_x - low_distance( band_full_x - rect.m_x1 );
				out.m_x0 = out.m_x1 - low_length( width );
			} else {
				out.m_x0 = band_low_x + thick_low + low_distance( rect.m_x0 - ( band_full_x + thick_full ) );
				out.m_x1 = out.m_x0 + low_length( width );
			}
		} else {
			out.m_x0 = low_band( rect.m_x0, band_full_x, band_low_x );
			out.m_x1 = out.m_x0 + low_thin( width );

			if ( role == role_top ) {
				out.m_y1 = band_low_y - low_distance( band_full_y - rect.m_y1 );
				out.m_y0 = out.m_y1 - low_length( height );
			} else {
				out.m_y0 = band_low_y + thick_low + low_distance( rect.m_y0 - ( band_full_y + thick_full ) );
				out.m_y1 = out.m_y0 + low_length( height );
			}
		}

		return out;
	};

	const auto push = []( const crosshair_rect_t& rect ) {
		if ( rect.m_x1 <= rect.m_x0 || rect.m_y1 <= rect.m_y0 )
			return;

		g_render.m_draw_data.emplace_back(
			e_draw_type::draw_type_rect,
			std::make_any< rect_draw_object_t >( c_vector_2d( static_cast< float >( rect.m_x0 ), static_cast< float >( rect.m_y0 ) ),
		                                        c_vector_2d( static_cast< float >( rect.m_x1 ), static_cast< float >( rect.m_y1 ) ), rect.m_color,
		                                        ImColor( 0.f, 0.f, 0.f, 0.f ), true, 0.f, ImDrawFlags_::ImDrawFlags_None, 1.f,
		                                        e_rect_flags::rect_flag_none ) );
	};

	for ( int i = 0, line_index = 0; i < count; ++i ) {
		const crosshair_rect_t& rect = this->m_crosshair[ i ];

		// DrawCrosshairRect: outline right before its line, sized off the line, never scaled itself
		if ( rect.m_outline && i + 1 < count && !this->m_crosshair[ i + 1 ].m_outline ) {
			const crosshair_rect_t line = rebuild( this->m_crosshair[ i + 1 ], line_index++ );

			crosshair_rect_t outline = rect;
			outline.m_x0             = static_cast< int >( line.m_x0 - outline_size );
			outline.m_y0             = static_cast< int >( line.m_y0 - outline_size );
			outline.m_x1             = static_cast< int >( line.m_x1 + outline_size );
			outline.m_y1             = static_cast< int >( line.m_y1 + outline_size );

			push( outline );
			push( line );
			++i;
			continue;
		}

		// lone outline ( never followed by its line ) keeps role order intact by not counting as a line
		push( rebuild( rect, rect.m_outline ? -1 : line_index++ ) );
	}
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

	if ( !this->enabled( ) ) {
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
	     this->m_built_screen_width != static_cast< int >( description.Width ) ||
	     this->m_built_screen_height != static_cast< int >( description.Height ) ) {
		this->release( );

		const bool built = SUCCEEDED( device->CreateTexture( description.Width, description.Height, 1, D3DUSAGE_RENDERTARGET, description.Format,
		                                                     D3DPOOL_DEFAULT, &this->m_capture, nullptr ) ) &&
		                   SUCCEEDED( this->m_capture->GetSurfaceLevel( 0, &this->m_capture_surface ) ) &&
		                   SUCCEEDED( device->CreateTexture( width, height, 1, D3DUSAGE_RENDERTARGET, description.Format, D3DPOOL_DEFAULT,
		                                                     &this->m_texture, nullptr ) ) &&
		                   SUCCEEDED( this->m_texture->GetSurfaceLevel( 0, &this->m_surface ) );

		// no column target = no area pass, the bilinear fallback still runs
		for ( const D3DFORMAT format : { D3DFMT_A16B16G16R16F, description.Format } ) {
			if ( !built || this->m_columns_surface )
				break;

			if ( SUCCEEDED( device->CreateTexture( width, description.Height, 1, D3DUSAGE_RENDERTARGET, format, D3DPOOL_DEFAULT, &this->m_columns,
			                                       nullptr ) ) &&
			     FAILED( this->m_columns->GetSurfaceLevel( 0, &this->m_columns_surface ) ) ) {
				this->m_columns->Release( );
				this->m_columns = nullptr;
			}
		}

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

		this->m_built_screen_width  = static_cast< int >( description.Width );
		this->m_built_screen_height = static_cast< int >( description.Height );

		this->m_width  = width;
		this->m_height = height;
		this->m_format = description.Format;

		int log_width         = width;
		int log_height        = height;
		int log_screen_width  = static_cast< int >( description.Width );
		int log_screen_height = static_cast< int >( description.Height );
		int log_multisample   = static_cast< int >( description.MultiSampleType );
		D3DSURFACE_DESC columns{ };
		int log_area = this->area_shader( device ) && this->m_columns_surface && SUCCEEDED( this->m_columns_surface->GetDesc( &columns ) )
		                   ? ( columns.Format == D3DFMT_A16B16G16R16F ? 16 : 8 )
		                   : 0;

		// area 16 / 8 = exact box ( column target bits ), 0 = bilinear fallback
		g_console.print( std::vformat( "resolution spoofer: {:d}x{:d} -> {:d}x{:d}, msaa {:d}, area {:d}",
		                               std::make_format_args( log_width, log_height, log_screen_width, log_screen_height, log_multisample,
		                                                      log_area ) )
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

	const bool dump = GET_VARIABLE( g_variables.m_resolution_spoof_dump, bool );

	if ( dump )
		n_frame_dump::save( device, this->m_capture_surface, "spoof_capture.png" );

	if ( debug_mode == 1 ) {
		back_buffer->Release( );
		draw_fallback( );
		return;
	}

	this->begin_quad_state( device );

	const bool scene = SUCCEEDED( device->BeginScene( ) );

	if ( !scene ) {
		this->end_quad_state( device );
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

	bool stepped = true;

	if ( debug_mode != 2 ) {
		const int screen_width  = static_cast< int >( description.Width );
		const int screen_height = static_cast< int >( description.Height );

		// SetRenderTarget resets the viewport to the target, which draw_up maps against
		const auto area_pass = [ & ]( IDirect3DTexture9* source, const int source_width, const int source_height, IDirect3DSurface9* target,
		                              const int target_width, const int target_height, const bool horizontal ) {
			const float along = static_cast< float >( horizontal ? source_width : source_height );
			const float ratio = along / static_cast< float >( horizontal ? target_width : target_height );

			const float constants[ 2 ][ 4 ] = { { along, ratio, 1.f / source_width, 1.f / source_height },
				                                { horizontal ? 1.f : 0.f, horizontal ? 0.f : 1.f, 0.f, 0.f } };
			const int taps[ 4 ]             = { std::min( static_cast< int >( std::ceil( ratio ) ) + 1, 255 ), 0, 1, 0 };

			device->SetPixelShaderConstantF( 0, constants[ 0 ], 2 );
			device->SetPixelShaderConstantI( 0, taps, 1 );

			return SUCCEEDED( device->SetRenderTarget( 0, target ) ) && this->draw_quad( device, source, target_width, target_height, true );
		};

		// no vs_3_0 = draw_up would pair ps_3_0 with fixed function vertex ( amd garbage ): bilinear instead
		if ( this->m_columns_surface && n_fx_compat::ready( device ) && this->area_shader( device ) ) {
			// taps land on texel centres: point, no mips. sampler states are in k_sampler_states, end_quad_state puts them back
			device->SetSamplerState( 0, D3DSAMP_MINFILTER, D3DTEXF_POINT );
			device->SetSamplerState( 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT );
			device->SetSamplerState( 0, D3DSAMP_MIPFILTER, D3DTEXF_NONE );
			device->SetPixelShader( this->m_area_shader );

			stepped = area_pass( this->m_capture, screen_width, screen_height, this->m_columns_surface, width, screen_height, true ) &&
			          area_pass( this->m_columns, width, screen_height, this->m_surface, width, height, false );

			device->SetPixelShader( nullptr );
		} else
			stepped = SUCCEEDED( device->SetRenderTarget( 0, this->m_surface ) ) && this->draw_quad( device, this->m_capture, width, height );

		static bool warned = false;
		if ( !stepped && !warned ) {
			g_console.print( "resolution spoofer: target draw failed" );
			warned = true;
		}
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

	this->end_quad_state( device );

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
		this->begin_quad_state( device );

		IDirect3DTexture9* up_source = debug_mode == 2 ? this->m_capture : this->m_texture;

		const bool drawn = this->draw_quad( device, up_source, static_cast< int >( description.Width ), static_cast< int >( description.Height ) );

		this->end_quad_state( device );

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
