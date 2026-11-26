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

/* mask r g b = class 1 2 3 ( players: visible, hidden, unused / viewmodel: weapon, gloves, sleeves ). class = strongest channel,
   body = sum >= 0.5. coverage ( "M" ) is read straight off the mask on s1 by the passes that need it */
#define STENCIL_CLASS_FN                                                                                                         \
	"float3 cls(float3 m){float a=step(max(m.g,m.b),m.r);float b=(1.0-a)*step(m.b,m.g);"                                         \
	"return step(0.5,dot(m,1.0))*float3(a,b,1.0-a-b);}"

#define COVER_FN "float cover(float2 uv){return saturate(dot(tex2Dlod(s1,float4(uv,0,0)).rgb,1.0));}"

static const char k_rows_shader[] =
	"sampler2D s0:register(s0);"
	"float4 c0:register(c0);" STENCIL_CLASS_FN
	"float4 main(float2 uv:TEXCOORD0):COLOR0{"
	"float3 m=tex2Dlod(s0,float4(uv,0,0)).rgb;"
	"float far=c0.z+2.0;"
	"float3 h=far*(1.0-cls(m));"
	"[loop] for(int i=1;i<=32;i++){"
	"float fi=(float)i;"
	"if(fi>=max(h.x,max(h.y,h.z)))break;"
	"float2 o=float2(c0.x*fi,0);"
	"float3 a=max(cls(tex2Dlod(s0,float4(uv+o,0,0)).rgb),cls(tex2Dlod(s0,float4(uv-o,0,0)).rgb));"
	"h=min(h,lerp(far.xxx,fi.xxx,a));}"
	"return float4(h/255.0,1);}";

#define COLUMN_DISTANCE_FN                                                                                                       \
	"float3 row(float2 uv){return floor(tex2Dlod(s0,float4(uv,0,0)).rgb*255.0+0.5);}"                                            \
	"float4 dist(float2 uv){"                                                                                                    \
	"float3 h=row(uv);"                                                                                                          \
	"float3 d2=h*h;"                                                                                                             \
	"[loop] for(int i=1;i<=32;i++){"                                                                                             \
	"float fi=(float)i;"                                                                                                         \
	"if(fi*fi>=max(d2.x,max(d2.y,d2.z)))break;"                                                                                  \
	"float2 o=float2(0,c0.y*fi);"                                                                                                \
	"float3 a=min(row(uv+o),row(uv-o));"                                                                                         \
	"d2=min(d2,fi*fi+a*a);}"                                                                                                     \
	"return float4(sqrt(d2),cover(uv));}"

#define GAUSS_FN                                                                                                                 \
	"float3 gauss(float2 uv,float2 o,float3 m){"                                                                                 \
	"float k=-6.125/(c7.z*c7.z);"                                                                                                \
	"float3 s=m;"                                                                                                                \
	"float n=1.0;"                                                                                                               \
	"[loop] for(int i=1;i<=32;i++){"                                                                                             \
	"float fi=(float)i;"                                                                                                         \
	"if(fi>=c7.z+1.0)break;"                                                                                                     \
	"float w=exp(fi*fi*k);"                                                                                                      \
	"s+=w*(tex2Dlod(s0,float4(uv+o*fi,0,0)).rgb+tex2Dlod(s0,float4(uv-o*fi,0,0)).rgb);"                                          \
	"n+=2.0*w;}"                                                                                                                 \
	"return s/n;}"

static const char k_blur_shader[] =
	"sampler2D s0:register(s0);"
	"float4 c0:register(c0);"
	"float4 c7:register(c7);" GAUSS_FN
	"float4 main(float2 uv:TEXCOORD0):COLOR0{"
	"return float4(gauss(uv,float2(c0.x,0),tex2Dlod(s0,float4(uv,0,0)).rgb),1);}";

/* wave coordinate: screen diagonal, or ( on ) height along the nearest player's box ( 0 head .. 1 feet ) plus that
   player's phase offset, so the sweep rides the body and turning the view never slides it. c15.y boxes in c16.., c48..
   r = cos sin of the angle slider ( c14 ), turns that direction; x in screen-height / body-height units so bands stay square */
#define WAVE_POS_FN                                                                                                              \
	"float4 c14:register(c14);"                                                                                                  \
	"float4 c15:register(c15);"                                                                                                  \
	"float4 pb[32]:register(c16);"                                                                                               \
	"float4 po[32]:register(c48);"                                                                                               \
	"float wpos(float2 uv,float on,float2 r){"                                                                                   \
	"float a=c0.y/c0.x;"                                                                                                         \
	"float t=dot(float2(uv.x*a,uv.y),float2(r.x-r.y,r.y+r.x));"                                                                  \
	"if(on<0.5||c15.y<0.5)return t;"                                                                                             \
	"float best=1e9;"                                                                                                            \
	"[loop] for(int i=0;i<32;i++){"                                                                                              \
	"if(i>=c15.y)break;"                                                                                                         \
	"float4 b=pb[i];"                                                                                                            \
	"float2 e=max(b.zw-b.xy,1e-4);"                                                                                              \
	"float2 d=uv-(b.xy+b.zw)*0.5;"                                                                                               \
	"float m=max(abs(d.x)/e.x,abs(d.y)/e.y);"                                                                                    \
	"if(m<best){best=m;t=dot(float2(d.x*a,d.y)/e.y,float2(-r.y,r.x))+0.5+po[i].x;}}"                                             \
	"return t;}"

/* normal blend: colour = blurred share of each class. 2 classes ( b.z = 0 ) = lerp( h, v, b.x / ( b.x + b.y ) ) */
static const char k_glow_shader[] =
	"sampler2D s0:register(s0);"
	"sampler2D s1:register(s1);"
	"float4 c0:register(c0);"
	"float4 c3:register(c3);"
	"float4 c4:register(c4);"
	"float4 c5:register(c5);"
	"float4 c6:register(c6);"
	"float4 c7:register(c7);"
	"float4 c8:register(c8);"
	"float4 c9:register(c9);"
	"float4 c81:register(c81);"
	"float4 c82:register(c82);" GAUSS_FN WAVE_POS_FN COVER_FN
	"float3 soft(float3 x){return x*pow(1.0+pow(x,2.625),-0.381);}"
	"float3 bright(float3 p){float3 e=p*dot(p,1.0);return min(e,pow(max(e,1.0),0.35));}"
	"float4 main(float2 uv:TEXCOORD0):COLOR0{"
	"float3 b=gauss(uv,float2(0,c0.y),tex2Dlod(s0,float4(uv,0,0)).rgb);"
	"float m=cover(uv);"
	"float a=pow(soft(dot(b,1.0)*6.75).x,0.8);"
	"float g=saturate((a-c7.x)*c7.y);"
	"float4 v=lerp(float4(c5.rgb*c5.a,c5.a),float4(c3.rgb*c3.a,c3.a),g);"
	"float4 h=lerp(float4(c6.rgb*c6.a,c6.a),float4(c4.rgb*c4.a,c4.a),g);"
	"float4 t=lerp(float4(c82.rgb*c82.a,c82.a),float4(c81.rgb*c81.a,c81.a),g);"
	"float w=0.5+0.5*cos(6.2831853*(wpos(uv,c15.x,c14.xy)*c8.z-c8.y));"
	"float k=w*w*w*c8.w*c8.x;"
	"v.rgb=lerp(v.rgb,c9.rgb*v.a,k);"
	"h.rgb=lerp(h.rgb,c9.rgb*h.a,k);"
	"t.rgb=lerp(t.rgb,c9.rgb*t.a,k);"
	"if(c7.w>0.5){"
	"float3 e=soft(6.75*(bright(v.rgb)*b.x+bright(h.rgb)*b.y+bright(t.rgb)*b.z));"
	"return float4(e,pow(max(e.r,max(e.g,e.b)),0.8))*(1.0-m);}"
	"return (v*b.x+h*b.y+t*b.z)*(a*(1.0-m)/max(dot(b,1.0),1e-5));}";

static const char k_inner_shader[] =
	"sampler2D s0:register(s0);"
	"sampler2D s1:register(s1);"
	"float4 c0:register(c0);"
	"float4 c7:register(c7);"
	"float4 c10:register(c10);"
	"float4 c11:register(c11);" GAUSS_FN COVER_FN
	"float4 main(float2 uv:TEXCOORD0):COLOR0{"
	"float3 b=gauss(uv,float2(0,c0.y),tex2Dlod(s0,float4(uv,0,0)).rgb);"
	"float e=smoothstep(0.5,1.0,b.x+b.y)*cover(uv)*c7.x/max(b.x+b.y,1e-5);"
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

/* nearer class colour, 1 px blend: weight = sat( 0.5 + 0.5 ( nearest other - own ) ). 2 classes = lerp( c2, c1, sat( 0.5 + 0.5 ( dh - dv ) ) );
   an absent class sits at far >= T + 2, weight 0 wherever alpha > 0 */
static const char k_columns_shader[] =
	"sampler2D s0:register(s0);"
	"sampler2D s1:register(s1);"
	"float4 c0:register(c0);"
	"float4 c1:register(c1);"
	"float4 c2:register(c2);"
	"float4 c12:register(c12);"
	"float4 c13:register(c13);"
	"float4 c80:register(c80);" COVER_FN COLUMN_DISTANCE_FN WAVE_POS_FN
	"float4 main(float2 uv:TEXCOORD0):COLOR0{"
	"float4 k=dist(uv);"
	"float3 q=saturate(0.5+0.5*(min(k.yzx,k.zxy)-k.xyz));"
	"float4 col=(c1*q.x+c2*q.y+c80*q.z)/max(dot(q,1.0),1e-5);"
	"float w=0.5+0.5*cos(6.2831853*(wpos(uv,c15.z,c14.zw)*c12.z-c12.y));"
	"col.rgb=lerp(col.rgb,c13.rgb,w*w*w*c12.w*c12.x);"
	"return float4(col.rgb,col.a*saturate(c0.w+1.0-min(k.x,min(k.y,k.z)))*(1.0-k.w));}";

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

bool n_player_stencil::impl_t::wanted( ) const
{
	return this->enabled( ) || this->glow_wanted( ) || this->inner_enabled( );
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

	/* one compile per call ( ~frame ): eight at once hitched the frame glow first turned on. columns last = the done flag above */
	const struct {
		const char* m_source;
		size_t m_size;
		IDirect3DPixelShader9*& m_shader;
	} shaders[ ] = {
		{ k_fill_shader, sizeof( k_fill_shader ) - 1, this->m_fill_shader },          { k_rows_shader, sizeof( k_rows_shader ) - 1, this->m_rows_shader },
		{ k_blur_shader, sizeof( k_blur_shader ) - 1, this->m_blur_shader },          { k_glow_shader, sizeof( k_glow_shader ) - 1, this->m_glow_shader },
		{ k_inner_shader, sizeof( k_inner_shader ) - 1, this->m_inner_shader },       { k_capsule_shader, sizeof( k_capsule_shader ) - 1, this->m_capsule_shader },
		{ k_bone_shader, sizeof( k_bone_shader ) - 1, this->m_bone_shader },          { k_columns_shader, sizeof( k_columns_shader ) - 1, this->m_columns_shader },
	};

	for ( const auto& shader : shaders ) {
		if ( shader.m_shader )
			continue;

		if ( !compile( shader.m_source, shader.m_size, shader.m_shader ) ) {
			g_console.print< n_console::log_level::WARNING >( "player stencil: shader failed — outline off, glow back to engine halo" );
			this->m_failed = true;
		}

		break;
	}

	this->m_ready = !this->m_failed && this->m_columns_shader;
	return this->m_ready;
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
	this->m_captured_frame       = -1;
}

void n_player_stencil::impl_t::on_device_lost( )
{
	this->release_targets( );

	this->m_ready = false;

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
	this->run( nullptr, nullptr, 1.f );
}

void n_player_stencil::impl_t::after_glow( const view_matrix_t& world_to_screen, const std::vector< n_players::hitbox_capsule_t >& bones,
                                           const float scale )
{
	this->run( &world_to_screen, &bones, scale );
}

static void rgba( float ( &out )[ 4 ], const c_color& color )
{
	out[ 0 ] = color.base< color_type_r >( );
	out[ 1 ] = color.base< color_type_g >( );
	out[ 2 ] = color.base< color_type_b >( );
	out[ 3 ] = color.base< color_type_a >( );
}

void n_player_stencil::impl_t::submit( const look_t& look, const fill_t* fills, const int fill_count, const clear_t& clear )
{
	if ( this->m_failed || !fills || fill_count <= 0 )
		return;

	settings_t settings{ };
	settings.m_name       = "model_glow";
	settings.m_glow       = look.m_glow;
	settings.m_outline    = look.m_outline;
	settings.m_fill_count = std::min( fill_count, settings_t::k_max_fills );
	settings.m_clear      = clear;
	std::copy_n( fills, settings.m_fill_count, settings.m_fills );

	apply_look( settings, look, 1.f );
	this->queue( settings );
}

void n_player_stencil::impl_t::capture( const fill_t* fills, const int fill_count, const clear_t& clear, const int frame )
{
	if ( this->m_failed || !fills || fill_count <= 0 )
		return;

	settings_t settings{ };
	settings.m_name          = "model_glow";
	settings.m_phase         = settings_t::phase_capture;
	settings.m_capture_frame = frame;
	settings.m_fill_count    = std::min( fill_count, settings_t::k_max_fills );
	settings.m_clear         = clear;
	std::copy_n( fills, settings.m_fill_count, settings.m_fills );

	this->queue( settings );
}

void n_player_stencil::impl_t::submit_captured( const look_t& look, const int frame )
{
	if ( this->m_failed || ( !look.m_glow && !look.m_outline ) )
		return;

	settings_t settings{ };
	settings.m_name          = "model_glow";
	settings.m_phase         = settings_t::phase_captured;
	settings.m_capture_frame = frame;
	settings.m_glow          = look.m_glow;
	settings.m_outline       = look.m_outline;

	apply_look( settings, look, 1.f );
	this->queue( settings );
}

void n_player_stencil::impl_t::run( const view_matrix_t* world_to_screen, const std::vector< n_players::hitbox_capsule_t >* given, const float scale )
{
	settings_t settings{ };
	settings.m_outline    = this->enabled( );
	settings.m_glow       = this->glow_wanted( );
	settings.m_inner      = this->inner_enabled( );
	settings.m_track_live = true;

	if ( !settings.m_outline && !settings.m_glow && !settings.m_inner )
		return;

	/* engine glow refs: 2..127 visible ( health index 4+ ), 1 hidden. vgui popups leave 254 down and the main view never clears
	   stencil: zero red where 128 <= stencil. and where model_glow's band survived a skipped pass ( health index 76+ never ) */
	settings.m_fills[ 0 ]  = { D3DCMP_LESS, 1, D3DCOLORWRITEENABLE_RED, false };
	settings.m_fills[ 1 ]  = { D3DCMP_LESSEQUAL, 128, D3DCOLORWRITEENABLE_RED, true };
	settings.m_fills[ 2 ]  = { D3DCMP_EQUAL, k_model_band_ref, D3DCOLORWRITEENABLE_RED, true, k_model_band_test };
	settings.m_fills[ 3 ]  = { D3DCMP_EQUAL, 1, D3DCOLORWRITEENABLE_GREEN, false };
	settings.m_fill_count = 4;

	rgba( settings.m_inner_visible, GET_VARIABLE( g_variables.m_inner_glow_vis_color, c_color ) );
	rgba( settings.m_inner_invisible, GET_VARIABLE( g_variables.m_inner_glow_invis_color, c_color ) );
	settings.m_inner_reach[ 0 ] = std::clamp( GET_VARIABLE( g_variables.m_inner_glow_intensity, float ), 0.25f, 4.f );
	/* blur loops stop at 32 taps */
	settings.m_inner_reach[ 2 ] =
		std::min( std::clamp( GET_VARIABLE( g_variables.m_inner_glow_size, float ), k_min_glow_radius, k_max_glow_radius ) * scale, 31.f );
	settings.m_inner_bones = settings.m_inner && GET_VARIABLE( g_variables.m_inner_glow_bones, bool );

	const bool anchor_glow    = GET_VARIABLE( g_variables.m_glow_wave, bool ) && GET_VARIABLE( g_variables.m_glow_wave_player, bool );
	const bool anchor_outline = GET_VARIABLE( g_variables.m_players_stencil_wave, bool ) && GET_VARIABLE( g_variables.m_players_stencil_wave_player, bool );
	const bool have_view      = given || g_ctx.m_local;

	const view_matrix_t m = !have_view ? view_matrix_t{ } : world_to_screen ? *world_to_screen : g_interfaces.m_engine_client->get_world_to_screen_matrix( );

	const auto project = [ & ]( const c_vector& p, float( &ndc )[ 2 ], float& w ) {
		w = m[ 3 ][ 0 ] * p.m_x + m[ 3 ][ 1 ] * p.m_y + m[ 3 ][ 2 ] * p.m_z + m[ 3 ][ 3 ];
		if ( w < 1.f )
			return false;
		ndc[ 0 ] = ( m[ 0 ][ 0 ] * p.m_x + m[ 0 ][ 1 ] * p.m_y + m[ 0 ][ 2 ] * p.m_z + m[ 0 ][ 3 ] ) / w;
		ndc[ 1 ] = ( m[ 1 ][ 0 ] * p.m_x + m[ 1 ][ 1 ] * p.m_y + m[ 1 ][ 2 ] * p.m_z + m[ 1 ][ 3 ] ) / w;
		return true;
	};

	/* wave anchor boxes: ndc bounds of each player, preview = the given capsules, live = the collision hull corners */
	if ( ( anchor_glow || anchor_outline ) && have_view ) {
		int n = 0;

		const auto add_box = [ & ]( const float( &lo )[ 2 ], const float( &hi )[ 2 ], const float phase ) {
			if ( n >= settings_t::k_max_boxes )
				return;
			settings.m_boxes[ n ][ 0 ]    = lo[ 0 ];
			settings.m_boxes[ n ][ 1 ]    = lo[ 1 ];
			settings.m_boxes[ n ][ 2 ]    = hi[ 0 ];
			settings.m_boxes[ n ][ 3 ]    = hi[ 1 ];
			settings.m_box_phase[ n ][ 0 ] = phase;
			++n;
		};

		if ( given ) {
			float lo[ 2 ] = { 1e9f, 1e9f }, hi[ 2 ] = { -1e9f, -1e9f };
			bool any      = false;

			for ( const auto& capsule : *given ) {
				for ( const c_vector* p : { &capsule.m_a, &capsule.m_b } ) {
					float ndc[ 2 ], w;
					if ( !project( *p, ndc, w ) )
						continue;
					lo[ 0 ] = std::min( lo[ 0 ], ndc[ 0 ] );
					lo[ 1 ] = std::min( lo[ 1 ], ndc[ 1 ] );
					hi[ 0 ] = std::max( hi[ 0 ], ndc[ 0 ] );
					hi[ 1 ] = std::max( hi[ 1 ], ndc[ 1 ] );
					any     = true;
				}
			}

			if ( any )
				add_box( lo, hi, 0.f );
		} else
			g_entity_cache.enumerate( e_enumeration_type::type_players, [ & ]( c_base_entity* entity ) {
			if ( !entity || entity == g_ctx.m_local || !entity->is_alive( ) || entity->is_dormant( ) )
				return;

			const auto renderable = entity->get_client_renderable( );
			const auto unknown    = renderable ? renderable->get_client_unknown( ) : nullptr;
			const auto collideable = unknown ? unknown->get_collideable( ) : nullptr;
			if ( !collideable )
				return;

			const c_vector mins       = collideable->get_obb_mins( );
			const c_vector maxs       = collideable->get_obb_maxs( );
			const matrix3x4_t& frame  = entity->get_coordinate_frame( );
			float lo[ 2 ] = { 1e9f, 1e9f }, hi[ 2 ] = { -1e9f, -1e9f };

			for ( int i = 0; i < 8; ++i ) {
				const c_vector corner( ( i & 1 ) ? maxs.m_x : mins.m_x, ( i & 2 ) ? maxs.m_y : mins.m_y, ( i & 4 ) ? maxs.m_z : mins.m_z );
				float ndc[ 2 ], w;
				if ( !project( g_math.vector_transform( corner, frame ), ndc, w ) )
					return; /* a corner behind the camera: no box, the pixel falls to its neighbour or the screen sweep */
				lo[ 0 ] = std::min( lo[ 0 ], ndc[ 0 ] );
				lo[ 1 ] = std::min( lo[ 1 ], ndc[ 1 ] );
				hi[ 0 ] = std::max( hi[ 0 ], ndc[ 0 ] );
				hi[ 1 ] = std::max( hi[ 1 ], ndc[ 1 ] );
			}

			add_box( lo, hi, static_cast< float >( entity->get_index( ) ) * 0.17f );
		} );

		settings.m_wave_anchor[ 0 ] = anchor_glow ? 1.f : 0.f;
		settings.m_wave_anchor[ 1 ] = static_cast< float >( n );
		settings.m_wave_anchor[ 2 ] = anchor_outline ? 1.f : 0.f;
	}

	if ( settings.m_inner_bones && have_view ) {
		const float focal      = std::sqrt( m[ 1 ][ 0 ] * m[ 1 ][ 0 ] + m[ 1 ][ 1 ] * m[ 1 ][ 1 ] + m[ 1 ][ 2 ] * m[ 1 ][ 2 ] );
		const float width      = std::clamp( GET_VARIABLE( g_variables.m_inner_glow_bone_width, float ), 10.f, 150.f ) / 100.f;

		const auto add_capsule = [ & ]( const c_vector& a, const c_vector& b, const float radius ) {
			settings_t::capsule_t capsule{ };
			float wa = 0.f, wb = 0.f;

			if ( !project( a, capsule.m_a, wa ) || !project( b, capsule.m_b, wb ) )
				return;

			capsule.m_sigma[ 0 ] = radius * width * focal / wa;
			capsule.m_sigma[ 1 ] = radius * width * focal / wb;
			settings.m_capsules.push_back( capsule );
		};

		if ( given ) {
			for ( const auto& capsule : *given )
				add_capsule( capsule.m_a, capsule.m_b, capsule.m_radius );
		} else
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

				add_capsule( g_math.vector_transform( box->m_bb_min, matrices[ box->m_bone ] ),
				             g_math.vector_transform( box->m_bb_max, matrices[ box->m_bone ] ), box->m_radius );
			}
		} );
	}

	look_t look{ };
	look.m_outline = settings.m_outline;
	look.m_glow    = settings.m_glow;

	look.m_outline_color[ 0 ] = GET_VARIABLE( g_variables.m_players_stencil_vis_color, c_color );
	look.m_outline_color[ 1 ] = GET_VARIABLE( g_variables.m_players_stencil_invis_color, c_color );
	look.m_outline_thickness  = GET_VARIABLE( g_variables.m_players_stencil_thickness, float );

	look.m_gradient       = GET_VARIABLE( g_variables.m_glow_gradient, bool );
	look.m_glow_inner[ 0 ] = GET_VARIABLE( g_variables.m_glow_vis_color, c_color );
	look.m_glow_inner[ 1 ] = GET_VARIABLE( g_variables.m_glow_invis_color, c_color );
	look.m_glow_outer[ 0 ] = GET_VARIABLE( g_variables.m_glow_vis_outer_color, c_color );
	look.m_glow_outer[ 1 ] = GET_VARIABLE( g_variables.m_glow_invis_outer_color, c_color );
	look.m_gradient_inner = GET_VARIABLE( g_variables.m_glow_gradient_inner, float );
	look.m_normal_blend   = GET_VARIABLE( g_variables.m_glow_normal_blend, bool );
	look.m_glow_thickness = GET_VARIABLE( g_variables.m_glow_thickness, float );

	look.m_wave          = GET_VARIABLE( g_variables.m_glow_wave, bool );
	look.m_wave_color    = GET_VARIABLE( g_variables.m_glow_wave_color, c_color );
	look.m_wave_speed    = GET_VARIABLE( g_variables.m_glow_wave_speed, float );
	look.m_wave_size     = GET_VARIABLE( g_variables.m_glow_wave_size, float );
	look.m_wave_strength = GET_VARIABLE( g_variables.m_glow_wave_strength, float );
	look.m_wave_angle    = GET_VARIABLE( g_variables.m_glow_wave_angle, float );

	look.m_pulse       = GET_VARIABLE( g_variables.m_glow_pulse, bool );
	look.m_pulse_speed = GET_VARIABLE( g_variables.m_glow_pulse_speed, float );
	look.m_pulse_min   = GET_VARIABLE( g_variables.m_glow_pulse_min, float );

	look.m_outline_wave          = GET_VARIABLE( g_variables.m_players_stencil_wave, bool );
	look.m_outline_wave_color    = GET_VARIABLE( g_variables.m_players_stencil_wave_color, c_color );
	look.m_outline_wave_speed    = GET_VARIABLE( g_variables.m_players_stencil_wave_speed, float );
	look.m_outline_wave_size     = GET_VARIABLE( g_variables.m_players_stencil_wave_size, float );
	look.m_outline_wave_strength = GET_VARIABLE( g_variables.m_players_stencil_wave_strength, float );
	look.m_outline_wave_angle    = GET_VARIABLE( g_variables.m_players_stencil_wave_angle, float );

	apply_look( settings, look, scale );
	this->queue( settings );
}

void n_player_stencil::impl_t::apply_look( settings_t& settings, const look_t& look, const float scale )
{
	rgba( settings.m_visible, look.m_outline_color[ 0 ] );
	rgba( settings.m_invisible, look.m_outline_color[ 1 ] );
	rgba( settings.m_third, look.m_outline_color[ 2 ] );
	settings.m_thickness = std::min( std::clamp( look.m_outline_thickness, 0.5f, k_max_thickness ) * scale, 31.f );

	const bool gradient = look.m_gradient;
	rgba( settings.m_glow_visible, look.m_glow_inner[ 0 ] );
	rgba( settings.m_glow_invisible, look.m_glow_inner[ 1 ] );
	rgba( settings.m_glow_third, look.m_glow_inner[ 2 ] );
	rgba( settings.m_glow_visible_outer, gradient ? look.m_glow_outer[ 0 ] : look.m_glow_inner[ 0 ] );
	rgba( settings.m_glow_invisible_outer, gradient ? look.m_glow_outer[ 1 ] : look.m_glow_inner[ 1 ] );
	rgba( settings.m_glow_third_outer, gradient ? look.m_glow_outer[ 2 ] : look.m_glow_inner[ 2 ] );

	const float start        = 1.f - std::clamp( look.m_gradient_inner, 0.f, 100.f ) / 100.f;
	settings.m_gradient[ 0 ] = start;
	settings.m_gradient[ 1 ] = 1.f / std::max( 1.f - start, 1e-3f );

	const bool additive      = !look.m_normal_blend;
	settings.m_gradient[ 2 ] =
		std::min( ( additive ? k_engine_glow_radius : std::clamp( look.m_glow_thickness, k_min_glow_radius, k_max_glow_radius ) ) * scale, 31.f );
	settings.m_gradient[ 3 ] = additive ? 1.f : 0.f;

	// real_time: keeps moving in pause / freezetime. phase kept in 0..1 so float precision never runs out
	const float now = g_interfaces.m_global_vars_base ? g_interfaces.m_global_vars_base->m_real_time : 0.f;

	if ( look.m_wave ) {
		const float phase          = std::fmod( now * look.m_wave_speed, 1.f );
		settings.m_wave[ 0 ]       = 1.f;
		settings.m_wave[ 1 ]       = phase < 0.f ? phase + 1.f : phase;
		settings.m_wave[ 2 ]       = 1.f / std::clamp( look.m_wave_size, 0.05f, 4.f );
		settings.m_wave[ 3 ]       = std::clamp( look.m_wave_strength, 0.f, 1.f );
		rgba( settings.m_wave_color, look.m_wave_color );
		const float angle          = deg2rad( look.m_wave_angle );
		settings.m_wave_angle[ 0 ] = std::cos( angle );
		settings.m_wave_angle[ 1 ] = std::sin( angle );
	}

	if ( look.m_outline_wave ) {
		const float phase             = std::fmod( now * look.m_outline_wave_speed, 1.f );
		settings.m_outline_wave[ 0 ]  = 1.f;
		settings.m_outline_wave[ 1 ]  = phase < 0.f ? phase + 1.f : phase;
		settings.m_outline_wave[ 2 ]  = 1.f / std::clamp( look.m_outline_wave_size, 0.05f, 4.f );
		settings.m_outline_wave[ 3 ]  = std::clamp( look.m_outline_wave_strength, 0.f, 1.f );
		rgba( settings.m_outline_wave_color, look.m_outline_wave_color );
		const float angle          = deg2rad( look.m_outline_wave_angle );
		settings.m_wave_angle[ 2 ] = std::cos( angle );
		settings.m_wave_angle[ 3 ] = std::sin( angle );
	}

	if ( look.m_pulse ) {
		const float floor = std::clamp( look.m_pulse_min, 0.f, 100.f ) / 100.f;
		const float wave  = 0.5f + 0.5f * std::sin( 6.2831853f * std::fmod( now * look.m_pulse_speed, 1.f ) );
		const float pulse = floor + ( 1.f - floor ) * wave;

		for ( float* color : { settings.m_glow_visible, settings.m_glow_invisible, settings.m_glow_third, settings.m_glow_visible_outer,
		                       settings.m_glow_invisible_outer, settings.m_glow_third_outer } )
			color[ 3 ] *= pulse;

		if ( !additive )
			settings.m_gradient[ 2 ] = ( std::max )( k_min_glow_radius, settings.m_gradient[ 2 ] * ( 0.6f + 0.4f * pulse ) );
	}
}

void n_player_stencil::impl_t::warm( )
{
	if ( this->m_failed || this->m_ready )
		return;

	const auto pass = [ this ] {
		if ( !n_fx_compat::run( g_interfaces.m_direct_device, "model_glow", [ & ] {
			     if ( g_interfaces.m_direct_device )
				     this->ensure_shaders( g_interfaces.m_direct_device );
		     } ) )
			this->m_model_dead = true;
	};

	if ( !n_render_queue::submit( pass ) )
		pass( );
}

void n_player_stencil::impl_t::queue( const settings_t& settings )
{
	const auto pass = [ this, settings ] {
		if ( !n_fx_compat::run( g_interfaces.m_direct_device, settings.m_name, [ & ] { this->execute( settings ); } ) && !settings.m_track_live )
			this->m_model_dead = true;
	};

	if ( !n_render_queue::submit( pass ) )
		pass( );
}

bool n_player_stencil::impl_t::drawable( IDirect3DDevice9* device, IDirect3DSurface9*& target, IDirect3DSurface9*& depth_stencil )
{
	if ( FAILED( device->GetRenderTarget( 0, &target ) ) )
		target = nullptr;

	if ( FAILED( device->GetDepthStencilSurface( &depth_stencil ) ) )
		depth_stencil = nullptr;

	D3DSURFACE_DESC description{ };
	D3DSURFACE_DESC depth_description{ };

	// stencil test reads the engine's depth-stencil: mask target must match its samples and fit inside
	return target && depth_stencil && SUCCEEDED( target->GetDesc( &description ) ) && SUCCEEDED( depth_stencil->GetDesc( &depth_description ) ) &&
	       depth_description.MultiSampleType == description.MultiSampleType && depth_description.MultiSampleQuality == description.MultiSampleQuality &&
	       depth_description.Width >= description.Width && depth_description.Height >= description.Height && this->build( device, description );
}

void n_player_stencil::impl_t::execute( const settings_t& settings )
{
	IDirect3DDevice9* device         = g_interfaces.m_direct_device;
	IDirect3DSurface9* target        = nullptr;
	IDirect3DSurface9* depth_stencil = nullptr;
	bool drawn                       = false;

	const bool reached = device && this->ensure_shaders( device ) && this->drawable( device, target, depth_stencil );

	if ( reached )
		drawn = this->draw( device, target, depth_stencil, settings );

	if ( depth_stencil )
		depth_stencil->Release( );

	if ( target )
		target->Release( );

	if ( settings.m_glow && settings.m_track_live )
		this->m_glow_live = drawn;

	// model_glow: a pass that never reached draw left its refs ( no clear ): stamps stop until probe( ) sees the checks pass
	if ( !settings.m_track_live )
		this->m_model_live = reached;
}

void n_player_stencil::impl_t::probe( )
{
	if ( this->m_failed || this->m_model_live )
		return;

	const auto pass = [ this ] {
		n_fx_compat::run( g_interfaces.m_direct_device, "model_glow", [ & ] {
			IDirect3DDevice9* device         = g_interfaces.m_direct_device;
			IDirect3DSurface9* target        = nullptr;
			IDirect3DSurface9* depth_stencil = nullptr;

			this->m_model_live = device && this->ensure_shaders( device ) && this->drawable( device, target, depth_stencil );

			if ( depth_stencil )
				depth_stencil->Release( );

			if ( target )
				target->Release( );
		} );
	};

	if ( !n_render_queue::submit( pass ) )
		pass( );
}

bool n_player_stencil::impl_t::draw( IDirect3DDevice9* device, IDirect3DSurface9* target, IDirect3DSurface9* depth_stencil, const settings_t& settings )
{
	IDirect3DPixelShader9* old_pixel_shader             = nullptr;
	IDirect3DVertexShader9* old_vertex_shader           = nullptr;
	IDirect3DVertexDeclaration9* old_vertex_declaration = nullptr;
	IDirect3DBaseTexture9* old_texture                  = nullptr;
	IDirect3DBaseTexture9* old_cover_texture            = nullptr;
	D3DVIEWPORT9 old_viewport{ };
	RECT old_scissor{ };
	DWORD fvf = 0;
	/* c0..c13 settings, c15 wave anchor, c16..c47 player boxes, c48..c79 their phases, c80..c82 class 3 colours */
	constexpr UINT k_constant_count = 83;
	float old_constants[ k_constant_count ][ 4 ]{ };

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
	DWORD old_cover_samplers[ std::size( k_samplers ) ]{ };

	device->GetPixelShader( &old_pixel_shader );
	device->GetVertexShader( &old_vertex_shader );
	device->GetVertexDeclaration( &old_vertex_declaration );
	device->GetFVF( &fvf );
	device->GetViewport( &old_viewport );
	device->GetScissorRect( &old_scissor );
	device->GetPixelShaderConstantF( 0, old_constants[ 0 ], k_constant_count );
	device->GetTexture( 0, &old_texture );
	device->GetTexture( 1, &old_cover_texture );

	for ( size_t i = 0; i < std::size( k_states ); ++i ) {
		device->GetRenderState( k_states[ i ], &old_states[ i ] );
		device->SetRenderState( k_states[ i ], k_state_values[ i ] );
	}

	// s1 = the mask: body coverage for the glow / inner / column passes
	for ( size_t i = 0; i < std::size( k_samplers ); ++i ) {
		device->GetSamplerState( 0, k_samplers[ i ], &old_samplers[ i ] );
		device->SetSamplerState( 0, k_samplers[ i ], k_sampler_values[ i ] );
		device->GetSamplerState( 1, k_samplers[ i ], &old_cover_samplers[ i ] );
		device->SetSamplerState( 1, k_samplers[ i ], k_sampler_values[ i ] );
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
	device->SetPixelShaderConstantF( 12, settings.m_outline_wave, 1 );
	device->SetPixelShaderConstantF( 13, settings.m_outline_wave_color, 1 );
	device->SetPixelShaderConstantF( 14, settings.m_wave_angle, 1 );
	device->SetPixelShaderConstantF( 15, settings.m_wave_anchor, 1 );
	device->SetPixelShaderConstantF( 80, settings.m_third, 1 );
	device->SetPixelShaderConstantF( 81, settings.m_glow_third, 1 );
	device->SetPixelShaderConstantF( 82, settings.m_glow_third_outer, 1 );

	/* player boxes: ndc -> this target's uv through the view rect, like the capsules */
	if ( const int boxes = static_cast< int >( settings.m_wave_anchor[ 1 ] ); boxes > 0 ) {
		const float half_width  = 0.5f * static_cast< float >( old_viewport.Width );
		const float half_height = 0.5f * static_cast< float >( old_viewport.Height );
		float uv_boxes[ settings_t::k_max_boxes ][ 4 ]{ };

		for ( int i = 0; i < boxes && i < settings_t::k_max_boxes; ++i ) {
			const float* b     = settings.m_boxes[ i ];
			uv_boxes[ i ][ 0 ] = ( view_left + ( b[ 0 ] + 1.f ) * half_width ) / width;
			uv_boxes[ i ][ 1 ] = ( view_top + ( 1.f - b[ 3 ] ) * half_height ) / height;
			uv_boxes[ i ][ 2 ] = ( view_left + ( b[ 2 ] + 1.f ) * half_width ) / width;
			uv_boxes[ i ][ 3 ] = ( view_top + ( 1.f - b[ 1 ] ) * half_height ) / height;
		}

		device->SetPixelShaderConstantF( 16, uv_boxes[ 0 ], boxes );
		device->SetPixelShaderConstantF( 48, settings.m_box_phase[ 0 ], boxes );
	}

	bool masked = false;

	if ( settings.m_phase == settings_t::phase_captured ) {
		// mask built earlier this frame ( capture ), the stencil it came from may be gone
		masked                 = this->m_captured_frame == settings.m_capture_frame;
		this->m_captured_frame = -1;
	} else {
		device->SetRenderTarget( 0, this->m_msaa_surface ? this->m_msaa_surface : this->m_mask_surface );
		device->Clear( 0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_ARGB( 0, 0, 0, 0 ), 1.f, 0 );
		device->SetRenderState( D3DRS_STENCILENABLE, TRUE );
		device->SetPixelShader( this->m_fill_shader );

		for ( int i = 0; i < settings.m_fill_count; ++i ) {
			const fill_t& fill = settings.m_fills[ i ];

			device->SetRenderState( D3DRS_STENCILFUNC, fill.m_func );
			device->SetRenderState( D3DRS_STENCILREF, fill.m_ref );
			device->SetRenderState( D3DRS_STENCILMASK, fill.m_mask );
			device->SetRenderState( D3DRS_COLORWRITEENABLE, fill.m_channels );
			device->SetRenderState( D3DRS_ALPHABLENDENABLE, fill.m_zero ? TRUE : FALSE );
			device->SetRenderState( D3DRS_SRCBLEND, fill.m_zero ? D3DBLEND_ZERO : D3DBLEND_SRCALPHA );
			device->SetRenderState( D3DRS_DESTBLEND, fill.m_zero ? D3DBLEND_ZERO : D3DBLEND_INVSRCALPHA );
			draw_rect( view_left, view_top, view_right, view_bottom );
		}

		device->SetRenderState( D3DRS_ALPHABLENDENABLE, FALSE );
		device->SetRenderState( D3DRS_SRCBLEND, D3DBLEND_SRCALPHA );
		device->SetRenderState( D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA );

		// read done: zero the source's ref bits in the frame stencil, colour untouched
		if ( settings.m_clear.m_write ) {
			device->SetRenderState( D3DRS_COLORWRITEENABLE, 0 );
			device->SetRenderState( D3DRS_STENCILFUNC, D3DCMP_EQUAL );
			device->SetRenderState( D3DRS_STENCILREF, settings.m_clear.m_ref );
			device->SetRenderState( D3DRS_STENCILMASK, settings.m_clear.m_test );
			device->SetRenderState( D3DRS_STENCILWRITEMASK, settings.m_clear.m_write );
			device->SetRenderState( D3DRS_STENCILPASS, D3DSTENCILOP_ZERO );
			draw_rect( view_left, view_top, view_right, view_bottom );
			device->SetRenderState( D3DRS_STENCILPASS, D3DSTENCILOP_KEEP );
			device->SetRenderState( D3DRS_STENCILWRITEMASK, 0 );
		}

		device->SetRenderState( D3DRS_STENCILMASK, 0xffffffff );

		device->SetRenderState( D3DRS_STENCILENABLE, FALSE );
		device->SetRenderState( D3DRS_COLORWRITEENABLE, 0x0f );

		masked = !this->m_msaa_surface || SUCCEEDED( device->StretchRect( this->m_msaa_surface, nullptr, this->m_mask_surface, nullptr, D3DTEXF_NONE ) );

		// any other mask built over it spends the capture
		this->m_captured_frame = settings.m_phase == settings_t::phase_capture && masked ? settings.m_capture_frame : -1;
	}

	device->SetDepthStencilSurface( nullptr );

	if ( masked && settings.m_phase != settings_t::phase_capture ) {
		/* offscreen passes run over the whole target so the frame passes never read a stale texel.
		   ponytail: full screen passes, empty px run every tap; scissor to player boxes if it shows in frame time */
		device->SetTexture( 0, this->m_mask_texture );
		device->SetTexture( 1, this->m_mask_texture );

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

	device->SetPixelShaderConstantF( 0, old_constants[ 0 ], k_constant_count );
	device->SetPixelShader( old_pixel_shader );
	device->SetVertexShader( old_vertex_shader );

	if ( fvf )
		device->SetFVF( fvf );

	device->SetVertexDeclaration( old_vertex_declaration );
	streams.restore( device );

	device->SetTexture( 0, old_texture );
	device->SetTexture( 1, old_cover_texture );

	for ( size_t i = 0; i < std::size( k_samplers ); ++i ) {
		device->SetSamplerState( 0, k_samplers[ i ], old_samplers[ i ] );
		device->SetSamplerState( 1, k_samplers[ i ], old_cover_samplers[ i ] );
	}

	for ( size_t i = 0; i < std::size( k_states ); ++i )
		device->SetRenderState( k_states[ i ], old_states[ i ] );

	if ( old_texture )
		old_texture->Release( );

	if ( old_cover_texture )
		old_cover_texture->Release( );

	if ( old_vertex_declaration )
		old_vertex_declaration->Release( );

	if ( old_vertex_shader )
		old_vertex_shader->Release( );

	if ( old_pixel_shader )
		old_pixel_shader->Release( );

	return masked;
}
