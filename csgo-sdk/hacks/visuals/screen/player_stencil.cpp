#include "player_stencil.h"
#include "../../../game/sdk/includes/includes.h"
#include "../../../globals/includes/includes.h"
#include "../../entity_cache/entity_cache.h"
#include "../players/sound_esp/sound_esp.h"
#include "fx_compat.h"
#include "render_queue.h"
#include "stream_guard.h"

#include <algorithm>
#include <cmath>
#include <d3dx9.h>
#include <initializer_list>
#include <iterator>

static constexpr float k_max_thickness = 8.f;

static constexpr float k_min_glow_radius = 2.f;
static constexpr float k_max_glow_radius = 30.f;

static constexpr float k_engine_glow_radius = 15.f;

static const char k_fill_shader[] = "float4 main():COLOR0{return 1;}";

#define STENCIL_CLASS_FN "float2 cls(float2 m){float v=step(m.g,m.r);return step(0.5,m.r+m.g)*float2(v,1.0-v);}"

static const char k_rows_shader[] =
	"sampler2D s0:register(s0);"
	"float4 c0:register(c0);" STENCIL_CLASS_FN
	"float4 main(float2 uv:TEXCOORD0):COLOR0{"
	"float2 m=tex2Dlod(s0,float4(uv,0,0)).rg;"
	"float far=c0.z+2.0;"
	"float2 h=far*(1.0-cls(m));"
	"[loop] for(int i=1;i<=32;i++){"
	"float fi=(float)i;"
	"if(fi>=max(h.x,h.y))break;"
	"float2 o=float2(c0.x*fi,0);"
	"float2 a=max(cls(tex2Dlod(s0,float4(uv+o,0,0)).rg),cls(tex2Dlod(s0,float4(uv-o,0,0)).rg));"
	"h=min(h,lerp(far.xx,fi.xx,a));}"
	"return float4(h/255.0,saturate(m.r+m.g),1);}";

#define COLUMN_DISTANCE_FN                                                                                                       \
	"float2 row(float2 uv){return floor(tex2Dlod(s0,float4(uv,0,0)).rg*255.0+0.5);}"                                             \
	"float3 dist(float2 uv){"                                                                                                    \
	"float3 s=tex2Dlod(s0,float4(uv,0,0)).rgb;"                                                                                  \
	"float2 h=floor(s.rg*255.0+0.5);"                                                                                            \
	"float2 d2=h*h;"                                                                                                             \
	"[loop] for(int i=1;i<=32;i++){"                                                                                             \
	"float fi=(float)i;"                                                                                                         \
	"if(fi*fi>=max(d2.x,d2.y))break;"                                                                                            \
	"float2 o=float2(0,c0.y*fi);"                                                                                                \
	"float2 a=min(row(uv+o),row(uv-o));"                                                                                         \
	"d2=min(d2,fi*fi+a*a);}"                                                                                                     \
	"return float3(sqrt(d2),s.b);}"

#define GAUSS_FN                                                                                                                 \
	"float2 gauss(float2 uv,float2 o,float2 m){"                                                                                 \
	"float k=-6.125/(c7.z*c7.z);"                                                                                                \
	"float2 s=m;"                                                                                                                \
	"float n=1.0;"                                                                                                               \
	"[loop] for(int i=1;i<=32;i++){"                                                                                             \
	"float fi=(float)i;"                                                                                                         \
	"if(fi>=c7.z+1.0)break;"                                                                                                     \
	"float w=exp(fi*fi*k);"                                                                                                      \
	"s+=w*(tex2Dlod(s0,float4(uv+o*fi,0,0)).rg+tex2Dlod(s0,float4(uv-o*fi,0,0)).rg);"                                            \
	"n+=2.0*w;}"                                                                                                                 \
	"return s/n;}"

static const char k_blur_shader[] =
	"sampler2D s0:register(s0);"
	"float4 c0:register(c0);"
	"float4 c7:register(c7);" GAUSS_FN
	"float4 main(float2 uv:TEXCOORD0):COLOR0{"
	"float2 m=tex2Dlod(s0,float4(uv,0,0)).rg;"
	"return float4(gauss(uv,float2(c0.x,0),m),saturate(m.r+m.g),1);}";

static const char k_glow_shader[] =
	"sampler2D s0:register(s0);"
	"float4 c0:register(c0);"
	"float4 c3:register(c3);"
	"float4 c4:register(c4);"
	"float4 c5:register(c5);"
	"float4 c6:register(c6);"
	"float4 c7:register(c7);"
	"float4 c8:register(c8);"
	"float4 c9:register(c9);" GAUSS_FN
	"float3 soft(float3 x){return x*pow(1.0+pow(x,2.625),-0.381);}"
	"float3 bright(float3 p){float3 e=p*dot(p,1.0);return min(e,pow(max(e,1.0),0.35));}"
	"float4 main(float2 uv:TEXCOORD0):COLOR0{"
	"float3 c=tex2Dlod(s0,float4(uv,0,0)).rgb;"
	"float2 b=gauss(uv,float2(0,c0.y),c.rg);"
	"float a=pow(soft((b.x+b.y)*6.75).x,0.8);"
	"float g=saturate((a-c7.x)*c7.y);"
	"float4 v=lerp(float4(c5.rgb*c5.a,c5.a),float4(c3.rgb*c3.a,c3.a),g);"
	"float4 h=lerp(float4(c6.rgb*c6.a,c6.a),float4(c4.rgb*c4.a,c4.a),g);"
	"float w=0.5+0.5*cos(6.2831853*((uv.x*c0.y/c0.x+uv.y)*c8.z-c8.y));"
	"float k=w*w*w*c8.w*c8.x;"
	"v.rgb=lerp(v.rgb,c9.rgb*v.a,k);"
	"h.rgb=lerp(h.rgb,c9.rgb*h.a,k);"
	"if(c7.w>0.5){"
	"float3 e=soft(6.75*(bright(v.rgb)*b.x+bright(h.rgb)*b.y));"
	"return float4(e,pow(max(e.r,max(e.g,e.b)),0.8))*(1.0-c.b);}"
	"return lerp(h,v,b.x/max(b.x+b.y,1e-5))*(a*(1.0-c.b));}";

static const char k_inner_shader[] =
	"sampler2D s0:register(s0);"
	"float4 c0:register(c0);"
	"float4 c7:register(c7);"
	"float4 c10:register(c10);"
	"float4 c11:register(c11);" GAUSS_FN
	"float4 main(float2 uv:TEXCOORD0):COLOR0{"
	"float3 c=tex2Dlod(s0,float4(uv,0,0)).rgb;"
	"float2 b=gauss(uv,float2(0,c0.y),c.rg);"
	"float e=smoothstep(0.5,1.0,b.x+b.y)*c.b*c7.x/max(b.x+b.y,1e-5);"
	"return float4((c10.rgb*c10.a*b.x+c11.rgb*c11.a*b.y)*e,0);}";

/* internal glow, bones: one quad per hitbox capsule, gaussian of the distance to the segment ( sigma lerped along it ) x mask
   coverage, so it never leaves the model. BLENDOP_MAX: overlapping capsules union. VPOS + 0.5 = pixel centre */
static const char k_capsule_shader[] =
	"sampler2D s0:register(s0);"
	"float4 c0:register(c0);"
	"float4 main(float2 v:VPOS,float4 s:TEXCOORD0,float2 r:TEXCOORD1):COLOR0{"
	"float2 p=v+0.5;"
	"float2 e=s.zw-s.xy;"
	"float t=saturate(dot(p-s.xy,e)/max(dot(e,e),1e-4));"
	"float q=length(p-s.xy-e*t)/lerp(r.x,r.y,t);"
	"return float4(exp(-0.5*q*q)*tex2Dlod(s0,float4(p*c0.xy,0,0)).rg,0,0);}";

static const char k_bone_shader[] =
	"sampler2D s0:register(s0);"
	"float4 c7:register(c7);"
	"float4 c10:register(c10);"
	"float4 c11:register(c11);"
	"float4 main(float2 uv:TEXCOORD0):COLOR0{"
	"float2 b=tex2Dlod(s0,float4(uv,0,0)).rg*c7.x;"
	"return float4(c10.rgb*c10.a*b.x+c11.rgb*c11.a*b.y,0);}";

static const char k_columns_shader[] =
	"sampler2D s0:register(s0);"
	"float4 c0:register(c0);"
	"float4 c1:register(c1);"
	"float4 c2:register(c2);" COLUMN_DISTANCE_FN
	"float4 main(float2 uv:TEXCOORD0):COLOR0{"
	"float3 k=dist(uv);"
	"float4 col=lerp(c2,c1,saturate(0.5+0.5*(k.y-k.x)));"
	"return float4(col.rgb,col.a*saturate(c0.w+1.0-min(k.x,k.y))*(1.0-k.z));}";

bool n_player_stencil::impl_t::enabled( ) const
{
	return !this->m_failed && GET_VARIABLE( g_variables.m_players, bool ) && GET_VARIABLE( g_variables.m_players_stencil, bool );
}

bool n_player_stencil::impl_t::glow_wanted( ) const
{
	return !this->m_failed && GET_VARIABLE( g_variables.m_players, bool ) && GET_VARIABLE( g_variables.m_glow_enable, bool ) &&
	       !GET_VARIABLE( g_variables.m_glow_legacy, bool );
}

bool n_player_stencil::impl_t::inner_enabled( ) const
{
	return !this->m_failed && GET_VARIABLE( g_variables.m_players, bool ) && GET_VARIABLE( g_variables.m_inner_glow, bool );
}

bool n_player_stencil::impl_t::glow_enabled( ) const
{
	return this->glow_wanted( ) && this->m_glow_live;
}

bool n_player_stencil::impl_t::ensure_shaders( IDirect3DDevice9* device )
{
	if ( this->m_columns_shader || this->m_failed )
		return !this->m_failed;

	const auto compile = [ & ]( const char* source, const size_t size, IDirect3DPixelShader9*& shader ) {
		ID3DXBuffer* code   = nullptr;
		ID3DXBuffer* errors = nullptr;

		if ( SUCCEEDED( D3DXCompileShader( source, static_cast< UINT >( size ), nullptr, nullptr, "main", "ps_3_0", 0, &code, &errors, nullptr ) ) && code )
			device->CreatePixelShader( static_cast< const DWORD* >( code->GetBufferPointer( ) ), &shader );

		if ( errors ) {
			g_console.print< n_console::log_level::WARNING >( static_cast< const char* >( errors->GetBufferPointer( ) ) );
			errors->Release( );
		}

		if ( code )
			code->Release( );

		return shader != nullptr;
	};

	if ( !compile( k_fill_shader, sizeof( k_fill_shader ) - 1, this->m_fill_shader ) ||
	     !compile( k_rows_shader, sizeof( k_rows_shader ) - 1, this->m_rows_shader ) ||
	     !compile( k_blur_shader, sizeof( k_blur_shader ) - 1, this->m_blur_shader ) ||
	     !compile( k_glow_shader, sizeof( k_glow_shader ) - 1, this->m_glow_shader ) ||
	     !compile( k_columns_shader, sizeof( k_columns_shader ) - 1, this->m_columns_shader ) ||
	     !compile( k_inner_shader, sizeof( k_inner_shader ) - 1, this->m_inner_shader ) ||
	     !compile( k_capsule_shader, sizeof( k_capsule_shader ) - 1, this->m_capsule_shader ) ||
	     !compile( k_bone_shader, sizeof( k_bone_shader ) - 1, this->m_bone_shader ) ) {
		g_console.print< n_console::log_level::WARNING >( "player stencil: shader failed — outline off, glow back to engine halo" );
		this->m_failed = true;
	}

	return !this->m_failed;
}

void n_player_stencil::impl_t::release_targets( )
{
	for ( IDirect3DSurface9** surface : { &this->m_msaa_surface, &this->m_mask_surface, &this->m_distance_surface, &this->m_blur_surface } ) {
		if ( *surface ) {
			( *surface )->Release( );
			*surface = nullptr;
		}
	}

	for ( IDirect3DTexture9** texture : { &this->m_mask_texture, &this->m_distance_texture, &this->m_blur_texture } ) {
		if ( *texture ) {
			( *texture )->Release( );
			*texture = nullptr;
		}
	}

	this->m_width                = 0;
	this->m_height               = 0;
	this->m_format               = D3DFMT_UNKNOWN;
	this->m_multi_sample         = D3DMULTISAMPLE_NONE;
	this->m_multi_sample_quality = 0;
}

void n_player_stencil::impl_t::on_device_lost( )
{
	this->release_targets( );

	for ( IDirect3DPixelShader9** shader :
	      { &this->m_fill_shader, &this->m_rows_shader, &this->m_blur_shader, &this->m_glow_shader, &this->m_columns_shader, &this->m_inner_shader,
	        &this->m_capsule_shader, &this->m_bone_shader } ) {
		if ( *shader ) {
			( *shader )->Release( );
			*shader = nullptr;
		}
	}
}

bool n_player_stencil::impl_t::build( IDirect3DDevice9* device, const D3DSURFACE_DESC& description )
{
	if ( this->m_mask_texture && this->m_width == description.Width && this->m_height == description.Height && this->m_format == description.Format &&
	     this->m_multi_sample == description.MultiSampleType && this->m_multi_sample_quality == description.MultiSampleQuality )
		return true;

	this->release_targets( );

	const auto make = [ & ]( IDirect3DTexture9*& texture, IDirect3DSurface9*& surface, const D3DFORMAT format ) {
		return SUCCEEDED( device->CreateTexture( description.Width, description.Height, 1, D3DUSAGE_RENDERTARGET, format, D3DPOOL_DEFAULT, &texture,
		                                         nullptr ) ) &&
		       SUCCEEDED( texture->GetSurfaceLevel( 0, &surface ) );
	};

	bool built = make( this->m_mask_texture, this->m_mask_surface, description.Format ) &&
	             make( this->m_distance_texture, this->m_distance_surface, description.Format ) &&
	             ( make( this->m_blur_texture, this->m_blur_surface, D3DFMT_A16B16G16R16F ) ||
	               make( this->m_blur_texture, this->m_blur_surface, description.Format ) );

	if ( built && description.MultiSampleType != D3DMULTISAMPLE_NONE )
		built = SUCCEEDED( device->CreateRenderTarget( description.Width, description.Height, description.Format, description.MultiSampleType,
		                                               description.MultiSampleQuality, FALSE, &this->m_msaa_surface, nullptr ) );

	if ( !built ) {
		g_console.print< n_console::log_level::WARNING >( "player stencil: target creation failed" );
		this->release_targets( );
		return false;
	}

	this->m_width                = description.Width;
	this->m_height               = description.Height;
	this->m_format               = description.Format;
	this->m_multi_sample         = description.MultiSampleType;
	this->m_multi_sample_quality = description.MultiSampleQuality;
	return true;
}

void n_player_stencil::impl_t::after_glow( )
{
	settings_t settings{ };
	settings.m_outline = this->enabled( );
	settings.m_glow    = this->glow_wanted( );
	settings.m_inner   = this->inner_enabled( );

	if ( !settings.m_outline && !settings.m_glow && !settings.m_inner )
		return;

	const auto rgba = [ ]( float( &out )[ 4 ], const c_color& color ) {
		out[ 0 ] = color.base< color_type_r >( );
		out[ 1 ] = color.base< color_type_g >( );
		out[ 2 ] = color.base< color_type_b >( );
		out[ 3 ] = color.base< color_type_a >( );
	};

	rgba( settings.m_inner_visible, GET_VARIABLE( g_variables.m_inner_glow_vis_color, c_color ) );
	rgba( settings.m_inner_invisible, GET_VARIABLE( g_variables.m_inner_glow_invis_color, c_color ) );
	settings.m_inner_reach[ 0 ] = std::clamp( GET_VARIABLE( g_variables.m_inner_glow_intensity, float ), 0.25f, 4.f );
	settings.m_inner_reach[ 2 ] = std::clamp( GET_VARIABLE( g_variables.m_inner_glow_size, float ), k_min_glow_radius, k_max_glow_radius );
	settings.m_inner_bones      = settings.m_inner && GET_VARIABLE( g_variables.m_inner_glow_bones, bool );

	if ( settings.m_inner_bones && g_ctx.m_local ) {
		const view_matrix_t& m = g_interfaces.m_engine_client->get_world_to_screen_matrix( );
		const float focal      = std::sqrt( m[ 1 ][ 0 ] * m[ 1 ][ 0 ] + m[ 1 ][ 1 ] * m[ 1 ][ 1 ] + m[ 1 ][ 2 ] * m[ 1 ][ 2 ] );
		const float width      = std::clamp( GET_VARIABLE( g_variables.m_inner_glow_bone_width, float ), 10.f, 150.f ) / 100.f;

		const auto project = [ & ]( const c_vector& p, float( &ndc )[ 2 ], float& w ) {
			w = m[ 3 ][ 0 ] * p.m_x + m[ 3 ][ 1 ] * p.m_y + m[ 3 ][ 2 ] * p.m_z + m[ 3 ][ 3 ];
			if ( w < 1.f )
				return false;
			ndc[ 0 ] = ( m[ 0 ][ 0 ] * p.m_x + m[ 0 ][ 1 ] * p.m_y + m[ 0 ][ 2 ] * p.m_z + m[ 0 ][ 3 ] ) / w;
			ndc[ 1 ] = ( m[ 1 ][ 0 ] * p.m_x + m[ 1 ][ 1 ] * p.m_y + m[ 1 ][ 2 ] * p.m_z + m[ 1 ][ 3 ] ) / w;
			return true;
		};

		g_entity_cache.enumerate( e_enumeration_type::type_players, [ & ]( c_base_entity* entity ) {
			if ( !entity || entity == g_ctx.m_local || !entity->is_alive( ) || entity->is_dormant( ) || !g_ctx.m_local->is_enemy( entity ) ||
			     g_sound_esp.player_gate( entity ) <= 0.f )
				return;

			hitbox_resolver_t resolver{ };
			auto& bones            = entity->get_cached_bone_data( );
			const int bone_count   = bones.count( );
			matrix3x4_t* matrices  = bones.get_elements( );

			if ( !resolver.setup( entity ) || !matrices )
				return;

			for ( int i = 0; i < resolver.m_set->m_hit_boxes; ++i ) {
				const mstudiobbox_t* box = resolver.m_set->get_hitbox( i );

				if ( !box || box->m_radius <= 0.f || box->m_bone < 0 || box->m_bone >= bone_count )
					continue;

				settings_t::capsule_t capsule{ };
				float wa = 0.f, wb = 0.f;

				if ( !project( g_math.vector_transform( box->m_bb_min, matrices[ box->m_bone ] ), capsule.m_a, wa ) ||
				     !project( g_math.vector_transform( box->m_bb_max, matrices[ box->m_bone ] ), capsule.m_b, wb ) )
					continue;

				capsule.m_sigma[ 0 ] = box->m_radius * width * focal / wa;
				capsule.m_sigma[ 1 ] = box->m_radius * width * focal / wb;
				settings.m_capsules.push_back( capsule );
			}
		} );
	}

	rgba( settings.m_visible, GET_VARIABLE( g_variables.m_players_stencil_vis_color, c_color ) );
	rgba( settings.m_invisible, GET_VARIABLE( g_variables.m_players_stencil_invis_color, c_color ) );
	settings.m_thickness = std::clamp( GET_VARIABLE( g_variables.m_players_stencil_thickness, float ), 0.5f, k_max_thickness );

	const bool gradient = GET_VARIABLE( g_variables.m_glow_gradient, bool );
	rgba( settings.m_glow_visible, GET_VARIABLE( g_variables.m_glow_vis_color, c_color ) );
	rgba( settings.m_glow_invisible, GET_VARIABLE( g_variables.m_glow_invis_color, c_color ) );
	rgba( settings.m_glow_visible_outer, gradient ? GET_VARIABLE( g_variables.m_glow_vis_outer_color, c_color )
	                                              : GET_VARIABLE( g_variables.m_glow_vis_color, c_color ) );
	rgba( settings.m_glow_invisible_outer, gradient ? GET_VARIABLE( g_variables.m_glow_invis_outer_color, c_color )
	                                                : GET_VARIABLE( g_variables.m_glow_invis_color, c_color ) );

	const float start        = 1.f - std::clamp( GET_VARIABLE( g_variables.m_glow_gradient_inner, float ), 0.f, 100.f ) / 100.f;
	settings.m_gradient[ 0 ] = start;
	settings.m_gradient[ 1 ] = 1.f / std::max( 1.f - start, 1e-3f );

	const bool additive      = !GET_VARIABLE( g_variables.m_glow_normal_blend, bool );
	settings.m_gradient[ 2 ] = additive ? k_engine_glow_radius
	                                    : std::clamp( GET_VARIABLE( g_variables.m_glow_thickness, float ), k_min_glow_radius, k_max_glow_radius );
	settings.m_gradient[ 3 ] = additive ? 1.f : 0.f;

	// real_time: keeps moving in pause / freezetime. phase kept in 0..1 so float precision never runs out
	const float now = g_interfaces.m_global_vars_base ? g_interfaces.m_global_vars_base->m_real_time : 0.f;

	if ( GET_VARIABLE( g_variables.m_glow_wave, bool ) ) {
		const float speed     = GET_VARIABLE( g_variables.m_glow_wave_speed, float );
		const float phase     = std::fmod( now * speed, 1.f );
		settings.m_wave[ 0 ]  = 1.f;
		settings.m_wave[ 1 ]  = phase < 0.f ? phase + 1.f : phase;
		settings.m_wave[ 2 ]  = 1.f / std::clamp( GET_VARIABLE( g_variables.m_glow_wave_size, float ), 0.05f, 4.f );
		settings.m_wave[ 3 ]  = std::clamp( GET_VARIABLE( g_variables.m_glow_wave_strength, float ), 0.f, 1.f );
		rgba( settings.m_wave_color, GET_VARIABLE( g_variables.m_glow_wave_color, c_color ) );
	}

	if ( GET_VARIABLE( g_variables.m_glow_pulse, bool ) ) {
		const float floor = std::clamp( GET_VARIABLE( g_variables.m_glow_pulse_min, float ), 0.f, 100.f ) / 100.f;
		const float wave  = 0.5f + 0.5f * std::sin( 6.2831853f * std::fmod( now * GET_VARIABLE( g_variables.m_glow_pulse_speed, float ), 1.f ) );
		const float scale = floor + ( 1.f - floor ) * wave;

		for ( float* color : { settings.m_glow_visible, settings.m_glow_invisible, settings.m_glow_visible_outer, settings.m_glow_invisible_outer } )
			color[ 3 ] *= scale;

		if ( !additive )
			settings.m_gradient[ 2 ] = ( std::max )( k_min_glow_radius, settings.m_gradient[ 2 ] * ( 0.6f + 0.4f * scale ) );
	}

	const auto pass = [ this, settings ] { n_fx_compat::run( g_interfaces.m_direct_device, "player_stencil", [ & ] { this->execute( settings ); } ); };

	if ( !n_render_queue::submit( pass ) )
		pass( );
}

void n_player_stencil::impl_t::execute( const settings_t& settings )
{
	IDirect3DDevice9* device  = g_interfaces.m_direct_device;
	IDirect3DSurface9* target = nullptr;
	bool drawn                = false;

	if ( device && this->ensure_shaders( device ) && SUCCEEDED( device->GetRenderTarget( 0, &target ) ) && target ) {
		IDirect3DSurface9* depth_stencil = nullptr;

		if ( FAILED( device->GetDepthStencilSurface( &depth_stencil ) ) )
			depth_stencil = nullptr;

		D3DSURFACE_DESC description{ };
		D3DSURFACE_DESC depth_description{ };

		// stencil test reads the engine's depth-stencil: mask target must match its samples and fit inside
		if ( depth_stencil && SUCCEEDED( target->GetDesc( &description ) ) && SUCCEEDED( depth_stencil->GetDesc( &depth_description ) ) &&
		     depth_description.MultiSampleType == description.MultiSampleType && depth_description.MultiSampleQuality == description.MultiSampleQuality &&
		     depth_description.Width >= description.Width && depth_description.Height >= description.Height && this->build( device, description ) )
			drawn = this->draw( device, target, depth_stencil, settings );

		if ( depth_stencil )
			depth_stencil->Release( );

		target->Release( );
	}

	if ( settings.m_glow )
		this->m_glow_live = drawn;
}

bool n_player_stencil::impl_t::draw( IDirect3DDevice9* device, IDirect3DSurface9* target, IDirect3DSurface9* depth_stencil, const settings_t& settings )
{
	IDirect3DPixelShader9* old_pixel_shader             = nullptr;
	IDirect3DVertexShader9* old_vertex_shader           = nullptr;
	IDirect3DVertexDeclaration9* old_vertex_declaration = nullptr;
	IDirect3DBaseTexture9* old_texture                  = nullptr;
	D3DVIEWPORT9 old_viewport{ };
	RECT old_scissor{ };
	DWORD fvf = 0;
	float old_constants[ 12 ][ 4 ]{ };

	constexpr D3DRENDERSTATETYPE k_states[ ] = { D3DRS_STENCILENABLE,   D3DRS_TWOSIDEDSTENCILMODE, D3DRS_STENCILFUNC,      D3DRS_STENCILREF,
		                                         D3DRS_STENCILMASK,     D3DRS_STENCILWRITEMASK,    D3DRS_STENCILFAIL,      D3DRS_STENCILZFAIL,
		                                         D3DRS_STENCILPASS,     D3DRS_COLORWRITEENABLE,    D3DRS_CLIPPLANEENABLE,  D3DRS_FILLMODE,
		                                         D3DRS_ZENABLE,         D3DRS_ZWRITEENABLE,        D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND,
		                                         D3DRS_DESTBLEND,       D3DRS_BLENDOP,             D3DRS_ALPHATESTENABLE,  D3DRS_CULLMODE,
		                                         D3DRS_SCISSORTESTENABLE, D3DRS_SRGBWRITEENABLE };
	constexpr DWORD k_state_values[ ] = { FALSE,
		                                  FALSE,
		                                  D3DCMP_LESS,
		                                  1,
		                                  0xffffffff,
		                                  0,
		                                  D3DSTENCILOP_KEEP,
		                                  D3DSTENCILOP_KEEP,
		                                  D3DSTENCILOP_KEEP,
		                                  0x0f,
		                                  0,
		                                  D3DFILL_SOLID,
		                                  FALSE,
		                                  FALSE,
		                                  FALSE,
		                                  D3DBLEND_SRCALPHA,
		                                  D3DBLEND_INVSRCALPHA,
		                                  D3DBLENDOP_ADD,
		                                  FALSE,
		                                  D3DCULL_NONE,
		                                  FALSE,
		                                  FALSE };
	static_assert( std::size( k_states ) == std::size( k_state_values ) );
	DWORD old_states[ std::size( k_states ) ]{ };

	constexpr D3DSAMPLERSTATETYPE k_samplers[ ] = { D3DSAMP_ADDRESSU,  D3DSAMP_ADDRESSV,  D3DSAMP_MINFILTER,
		                                            D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER, D3DSAMP_SRGBTEXTURE };
	constexpr DWORD k_sampler_values[ ] = { D3DTADDRESS_CLAMP, D3DTADDRESS_CLAMP, D3DTEXF_POINT, D3DTEXF_POINT, D3DTEXF_NONE, 0 };
	DWORD old_samplers[ std::size( k_samplers ) ]{ };

	device->GetPixelShader( &old_pixel_shader );
	device->GetVertexShader( &old_vertex_shader );
	device->GetVertexDeclaration( &old_vertex_declaration );
	device->GetFVF( &fvf );
	device->GetViewport( &old_viewport );
	device->GetScissorRect( &old_scissor );
	device->GetPixelShaderConstantF( 0, old_constants[ 0 ], 12 );
	device->GetTexture( 0, &old_texture );

	for ( size_t i = 0; i < std::size( k_states ); ++i ) {
		device->GetRenderState( k_states[ i ], &old_states[ i ] );
		device->SetRenderState( k_states[ i ], k_state_values[ i ] );
	}

	for ( size_t i = 0; i < std::size( k_samplers ); ++i ) {
		device->GetSamplerState( 0, k_samplers[ i ], &old_samplers[ i ] );
		device->SetSamplerState( 0, k_samplers[ i ], k_sampler_values[ i ] );
	}

	n_stream_guard::state_t streams{ };
	streams.capture( device );

	const float width  = static_cast< float >( this->m_width );
	const float height = static_cast< float >( this->m_height );
	const D3DVIEWPORT9 full_viewport{ 0, 0, this->m_width, this->m_height, 0.f, 1.f };

	struct vertex_t {
		float m_x, m_y, m_z, m_rhw, m_u, m_v;
	};

	const auto draw_rect = [ & ]( const float left, const float top, const float right, const float bottom ) {
		const vertex_t quad[ 4 ] = {
			{ left - 0.5f, top - 0.5f, 0.f, 1.f, left / width, top / height },
			{ right - 0.5f, top - 0.5f, 0.f, 1.f, right / width, top / height },
			{ left - 0.5f, bottom - 0.5f, 0.f, 1.f, left / width, bottom / height },
			{ right - 0.5f, bottom - 0.5f, 0.f, 1.f, right / width, bottom / height },
		};

		device->SetViewport( &full_viewport );
		n_fx_compat::draw_up( device, D3DPT_TRIANGLESTRIP, 2, quad, sizeof( vertex_t ) );
	};

	const float view_left   = static_cast< float >( old_viewport.X );
	const float view_top    = static_cast< float >( old_viewport.Y );
	const float view_right  = view_left + static_cast< float >( old_viewport.Width );
	const float view_bottom = view_top + static_cast< float >( old_viewport.Height );

	const float radius          = std::ceil( settings.m_thickness );
	const float constant_0[ 4 ] = { 1.f / width, 1.f / height, radius, settings.m_thickness };

	device->SetVertexShader( nullptr );
	device->SetVertexDeclaration( nullptr );
	device->SetFVF( D3DFVF_XYZRHW | D3DFVF_TEX1 );
	device->SetPixelShaderConstantF( 0, constant_0, 1 );
	device->SetPixelShaderConstantF( 1, settings.m_visible, 1 );
	device->SetPixelShaderConstantF( 2, settings.m_invisible, 1 );
	device->SetPixelShaderConstantF( 3, settings.m_glow_visible, 1 );
	device->SetPixelShaderConstantF( 4, settings.m_glow_invisible, 1 );
	device->SetPixelShaderConstantF( 5, settings.m_glow_visible_outer, 1 );
	device->SetPixelShaderConstantF( 6, settings.m_glow_invisible_outer, 1 );
	device->SetPixelShaderConstantF( 7, settings.m_gradient, 1 );
	device->SetPixelShaderConstantF( 8, settings.m_wave, 1 );
	device->SetPixelShaderConstantF( 9, settings.m_wave_color, 1 );
	device->SetPixelShaderConstantF( 10, settings.m_inner_visible, 1 );
	device->SetPixelShaderConstantF( 11, settings.m_inner_invisible, 1 );

	device->SetRenderTarget( 0, this->m_msaa_surface ? this->m_msaa_surface : this->m_mask_surface );
	device->Clear( 0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_ARGB( 0, 0, 0, 0 ), 1.f, 0 );
	device->SetRenderState( D3DRS_STENCILENABLE, TRUE );
	device->SetPixelShader( this->m_fill_shader );
	device->SetRenderState( D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED );
	draw_rect( view_left, view_top, view_right, view_bottom );
	// vgui popups ( console ) leave stencil 254 down and the main view never clears it.
	// glow refs stay < 128: zero red where 128 <= stencil
	device->SetRenderState( D3DRS_STENCILFUNC, D3DCMP_LESSEQUAL );
	device->SetRenderState( D3DRS_STENCILREF, 128 );
	device->SetRenderState( D3DRS_ALPHABLENDENABLE, TRUE );
	device->SetRenderState( D3DRS_SRCBLEND, D3DBLEND_ZERO );
	device->SetRenderState( D3DRS_DESTBLEND, D3DBLEND_ZERO );
	draw_rect( view_left, view_top, view_right, view_bottom );
	device->SetRenderState( D3DRS_ALPHABLENDENABLE, FALSE );
	device->SetRenderState( D3DRS_SRCBLEND, D3DBLEND_SRCALPHA );
	device->SetRenderState( D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA );
	device->SetRenderState( D3DRS_STENCILREF, 1 );
	device->SetRenderState( D3DRS_STENCILFUNC, D3DCMP_EQUAL );
	device->SetRenderState( D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_GREEN );
	draw_rect( view_left, view_top, view_right, view_bottom );
	device->SetRenderState( D3DRS_STENCILENABLE, FALSE );
	device->SetRenderState( D3DRS_COLORWRITEENABLE, 0x0f );

	const bool masked =
		!this->m_msaa_surface || SUCCEEDED( device->StretchRect( this->m_msaa_surface, nullptr, this->m_mask_surface, nullptr, D3DTEXF_NONE ) );

	device->SetDepthStencilSurface( nullptr );

	if ( masked ) {
		/* offscreen passes run over the whole target so the frame passes never read a stale texel.
		   ponytail: full screen passes, empty px run every tap; scissor to player boxes if it shows in frame time */
		device->SetTexture( 0, this->m_mask_texture );

		if ( settings.m_outline ) {
			device->SetRenderTarget( 0, this->m_distance_surface );
			device->SetPixelShader( this->m_rows_shader );
			draw_rect( 0.f, 0.f, width, height );
		}

		if ( settings.m_inner && !( settings.m_inner_bones && settings.m_capsules.empty( ) ) ) {
			device->SetPixelShaderConstantF( 7, settings.m_inner_reach, 1 );
			device->SetRenderTarget( 0, this->m_blur_surface );

			if ( settings.m_inner_bones ) {
				struct capsule_vertex_t {
					float m_x, m_y, m_z, m_rhw, m_a[ 2 ], m_b[ 2 ], m_sigma[ 2 ], m_pad[ 2 ];
				};

				// ndc to view rect px ( pixel centres at i + 0.5 ), sigma floored so far players don't shimmer
				const float half_width  = 0.5f * static_cast< float >( old_viewport.Width );
				const float half_height = 0.5f * static_cast< float >( old_viewport.Height );
				std::vector< capsule_vertex_t > vertices;
				vertices.reserve( settings.m_capsules.size( ) * 6 );

				for ( const auto& capsule : settings.m_capsules ) {
					const float ax = view_left + ( capsule.m_a[ 0 ] + 1.f ) * half_width, ay = view_top + ( 1.f - capsule.m_a[ 1 ] ) * half_height;
					const float bx = view_left + ( capsule.m_b[ 0 ] + 1.f ) * half_width, by = view_top + ( 1.f - capsule.m_b[ 1 ] ) * half_height;
					const float sa = ( std::max )( capsule.m_sigma[ 0 ] * half_height, 0.75f );
					const float sb = ( std::max )( capsule.m_sigma[ 1 ] * half_height, 0.75f );
					const float reach = 3.f * ( std::max )( sa, sb );
					const float left = ( std::max )( ( std::min )( ax, bx ) - reach, view_left ), right = ( std::min )( ( std::max )( ax, bx ) + reach, view_right );
					const float top = ( std::max )( ( std::min )( ay, by ) - reach, view_top ), bottom = ( std::min )( ( std::max )( ay, by ) + reach, view_bottom );

					if ( left >= right || top >= bottom )
						continue;

					const auto corner = [ & ]( const float x, const float y ) {
						return capsule_vertex_t{ x - 0.5f, y - 0.5f, 0.f, 1.f, { ax, ay }, { bx, by }, { sa, sb }, { 0.f, 0.f } };
					};

					for ( const auto& v : { corner( left, top ), corner( right, top ), corner( left, bottom ), corner( right, top ), corner( right, bottom ),
					                        corner( left, bottom ) } )
						vertices.push_back( v );
				}

				device->Clear( 0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_ARGB( 0, 0, 0, 0 ), 1.f, 0 );

				if ( !vertices.empty( ) ) {
					device->SetRenderState( D3DRS_ALPHABLENDENABLE, TRUE );
					device->SetRenderState( D3DRS_BLENDOP, D3DBLENDOP_MAX );
					device->SetPixelShader( this->m_capsule_shader );
					device->SetFVF( D3DFVF_XYZRHW | D3DFVF_TEX2 | D3DFVF_TEXCOORDSIZE4( 0 ) | D3DFVF_TEXCOORDSIZE4( 1 ) );
					device->SetViewport( &full_viewport );
					n_fx_compat::draw_up( device, D3DPT_TRIANGLELIST, static_cast< UINT >( vertices.size( ) / 3 ), vertices.data( ), sizeof( capsule_vertex_t ) );
					device->SetFVF( D3DFVF_XYZRHW | D3DFVF_TEX1 );
					device->SetRenderState( D3DRS_BLENDOP, D3DBLENDOP_ADD );
					device->SetRenderState( D3DRS_ALPHABLENDENABLE, FALSE );
				}
			} else {
				device->SetPixelShader( this->m_blur_shader );
				draw_rect( 0.f, 0.f, width, height );
			}

			device->SetRenderTarget( 0, target );
			device->SetRenderState( D3DRS_ALPHABLENDENABLE, TRUE );
			device->SetRenderState( D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE );
			device->SetRenderState( D3DRS_SRCBLEND, D3DBLEND_ONE );
			device->SetPixelShader( settings.m_inner_bones ? this->m_bone_shader : this->m_inner_shader );
			device->SetTexture( 0, this->m_blur_texture );
			draw_rect( view_left, view_top, view_right, view_bottom );

			device->SetRenderState( D3DRS_ALPHABLENDENABLE, FALSE );
			device->SetRenderState( D3DRS_COLORWRITEENABLE, 0x0f );
			device->SetRenderState( D3DRS_SRCBLEND, D3DBLEND_SRCALPHA );
			device->SetTexture( 0, this->m_mask_texture );
			device->SetPixelShaderConstantF( 7, settings.m_gradient, 1 );
		}

		if ( settings.m_glow ) {
			device->SetRenderTarget( 0, this->m_blur_surface );
			device->SetPixelShader( this->m_blur_shader );
			draw_rect( 0.f, 0.f, width, height );
		}

		device->SetRenderTarget( 0, target );
		device->SetRenderState( D3DRS_ALPHABLENDENABLE, TRUE );
		device->SetRenderState( D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE );

		if ( settings.m_glow ) {
			device->SetRenderState( D3DRS_SRCBLEND, D3DBLEND_ONE );
			device->SetPixelShader( this->m_glow_shader );
			device->SetTexture( 0, this->m_blur_texture );
			draw_rect( view_left, view_top, view_right, view_bottom );
			device->SetRenderState( D3DRS_SRCBLEND, D3DBLEND_SRCALPHA );
		}

		if ( settings.m_outline ) {
			device->SetPixelShader( this->m_columns_shader );
			device->SetTexture( 0, this->m_distance_texture );
			draw_rect( view_left, view_top, view_right, view_bottom );
		}
	}

	device->SetRenderTarget( 0, target );
	device->SetDepthStencilSurface( depth_stencil );
	device->SetViewport( &old_viewport );
	device->SetScissorRect( &old_scissor );

	device->SetPixelShaderConstantF( 0, old_constants[ 0 ], 12 );
	device->SetPixelShader( old_pixel_shader );
	device->SetVertexShader( old_vertex_shader );

	if ( fvf )
		device->SetFVF( fvf );

	device->SetVertexDeclaration( old_vertex_declaration );
	streams.restore( device );

	device->SetTexture( 0, old_texture );

	for ( size_t i = 0; i < std::size( k_samplers ); ++i )
		device->SetSamplerState( 0, k_samplers[ i ], old_samplers[ i ] );

	for ( size_t i = 0; i < std::size( k_states ); ++i )
		device->SetRenderState( k_states[ i ], old_states[ i ] );

	if ( old_texture )
		old_texture->Release( );

	if ( old_vertex_declaration )
		old_vertex_declaration->Release( );

	if ( old_vertex_shader )
		old_vertex_shader->Release( );

	if ( old_pixel_shader )
		old_pixel_shader->Release( );

	return masked;
}
