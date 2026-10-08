#include "depth_of_field.h"
#include "../../../game/sdk/includes/includes.h"
#include "../../../globals/includes/includes.h"
#include "depth_source.h"
#include "fx_compat.h"
#include "frame_copy.h"
#include "gpu_timer.h"
#include "render_queue.h"
#include "serial_render.h"
#include "stream_guard.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <d3dx9.h>
#include <string>

static constexpr int k_style_bokeh = 2;

static const char k_prepare_body[] =
	"sampler2D s0:register(s0);"
	"sampler2D s1:register(s1);"
	"float4 c0:register(c0);"
	"float4 c1:register(c1);"
	"float4 c2:register(c2);"
	"float4 c3:register(c3);"
	"float linear_z(float d){return (c0.x*c0.y)/(c0.y-d*(c0.y-c0.x));}"
	"float4 main(float2 uv:TEXCOORD0):COLOR0{"
	"float3 c=tex2Dlod(s0,float4(uv,0,0)).rgb;"
	"float raw=tex2Dlod(s1,float4(uv,0,0)).r;"
	"float z=linear_z(raw);"
	"float f=saturate((z-c0.z-c1.x)*c0.w);"
	"float n=saturate((c0.z-c1.x-z)*c3.w)*c2.y;"
	"float k=(raw<c2.x)?0.0:max(f,n);"
	"float s=(n>f)?-1.0:1.0;"
	"return float4(c,k*s*0.5+0.5);"
	"}";

static const char k_shader_body[] =
	"sampler2D s0:register(s0);"
	"sampler2D s1:register(s1);"
	"float4 c0:register(c0);"
	"float4 c1:register(c1);"
	"float4 c2:register(c2);"
	"float4 c3:register(c3);"
	"float4 c5:register(c5);"
	"float3 unpack_hdr(float3 c){return c*rcp(1.2-saturate(c));}"
	"float3 pack_hdr(float3 c){return 1.2*c*rcp(c+1.0);}"
	"float linear_z(float d){return (c0.x*c0.y)/(c0.y-d*(c0.y-c0.x));}"
	"float4 main(float2 uv:TEXCOORD0):COLOR0{"
	// s0 = prepared frame, alpha = signed coc: the tap loop never reads depth
	"float4 mid=tex2Dlod(s0,float4(uv,0,0));"
	"float3 centre=mid.rgb;"
	"float cs=mid.a*2.0-1.0;\n"
	/* debug reads = two depth fetches per pixel and a tex2D can't sit in dynamic flow, so debug is its
	   own compiled variant; the play shader never touches depth. */
	"#if DEBUG\n"
	"float dc=tex2Dlod(s1,float4(uv,0,0)).r;\n"
	"float dg=tex2D(s1,uv).r;"
	"float zc=linear_z(dc);"
	"if(c2.z>1.5)return float4(dc,dg,zc,tex2Dlod(s0,float4(uv,0,0)).a);"
	"if(c2.z>0.5)return float4(saturate(zc/c2.w).xxx,1);\n"
	"#endif\n"
	"if(c5.x>0.5&&cs<0.0)return float4(centre,mid.a);"
	"float radius=abs(cs)*c1.y;"
	"if(radius<0.5)return float4(centre,mid.a);"
	"float n=clamp(ceil(radius*2.5),8.0,(float)TAPS);\n"
	"float2 px=uv/c1.zw;\n"
	"float jit=frac(52.9829189*frac(dot(px,float2(0.06711056,0.00583715))))*6.2831853;\n"
	// golden angle step as a rotation, not a sincos per tap
	"float2 dir;sincos(1.19998162+jit,dir.y,dir.x);\n"
	"float2 disc=radius*c1.zw;\n"
	"float3 base=centre;\n"
	"#if STYLE==2\n"
	"base=unpack_hdr(base);\n"
	"#endif\n"
	"float4 sum=float4(base,1.0);\n"
	"[loop] for(int i=0;i<TAPS;i++){"
	"if((float)i>=n)break;"
	"float fi=(float)i+0.5;"
	"float rr=fi/n;\n"
	"#if STYLE==2\n"
	"rr=pow(rr,0.35);\n"
	"#else\n"
	"rr=sqrt(rr);\n"
	"#endif\n"
	"float2 off=dir*rr*disc;"
	"dir=float2(dir.x*-0.73736888-dir.y*0.67549029,dir.x*0.67549029+dir.y*-0.73736888);"
	"float4 s=tex2Dlod(s0,float4(uv+off,0,0));"
	"float3 t=s.rgb;"
	"float ts=s.a*2.0-1.0;"
	"float rpx=rr*radius;\n"
	"float trad=abs(ts)*c1.y;\n"
	"float relax=(ts>0.0&&cs>0.0)?c3.z:0.0;\n"
	"float reach=(ts<cs)?lerp(trad,radius,relax):radius;\n"
	"float w=saturate((reach-rpx)/max(1.0,radius*0.35)+1.0);\n"
	// near taps are unblurred in this buffer: weight 0 so they don't smear over the background
	"w*=(c5.x>0.5&&ts<0.0)?0.0:1.0;\n"
	"#if STYLE==1\n"
	"w*=exp(-2.0*rr*rr);\n"
	"#endif\n"
	"#if STYLE==2\n"
	"w*=1.0+c3.x*saturate(dot(t,float3(0.2126,0.7152,0.0722))-c3.y);"
	"t=unpack_hdr(t);\n"
	"#endif\n"
	"sum+=float4(t*w,w);"
	"}"
	"float3 outc=sum.rgb/sum.a;\n"
	"#if STYLE==2\n"
	"outc=pack_hdr(outc);\n"
	"#endif\n"
	"return float4(outc,mid.a);"
	"}";

static const char k_coc_body[] =
	"sampler2D s0:register(s0);"
	"float4 c1:register(c1);"
	"float4 c4:register(c4);"
	"static const float k_off[18]={0.0,1.4953705027,3.4891992113,5.4830312105,7.4768683759,9.4707125766,11.4645656736,"
	"13.4584295168,15.4523059431,17.4461967743,19.4661974725,21.4627427973,23.4592916956,25.455844494,27.4524015179,"
	"29.4489630909,31.445529535,33.4421011704};"
	"static const float k_wgt[18]={0.033245,0.0659162217,0.0636705814,0.0598194658,0.0546642566,0.0485871646,0.0420045997,"
	"0.0353207015,0.0288880982,0.0229808311,0.0177815511,0.013382297,0.0097960001,0.0069746748,0.0048301008,0.0032534598,"
	"0.0021315311,0.0013582974};"
	"float read(float2 t){"
	"float4 s=tex2Dlod(s0,float4(t,0,0));"
	"return (c4.z>0.5)?max(-(s.a*2.0-1.0),0.0):s.r;"
	"}"
	"float4 main(float2 uv:TEXCOORD0):COLOR0{"
	"float2 axis=c4.xy*c1.zw*max(c1.y,1.0)*0.1645;"
	"float coc=read(uv)*k_wgt[0];"
	"[unroll] for(int i=1;i<18;i++){"
	"float2 o=axis*k_off[i];"
	"coc+=(read(uv+o)+read(uv-o))*k_wgt[i];"
	"}"
	"return float4(saturate(coc),0.0,0.0,1.0);"
	"}";

static const char k_near_body[] =
	"sampler2D s0:register(s0);"
	"sampler2D s1:register(s1);"
	"sampler2D s2:register(s2);"
	"float4 c1:register(c1);"
	"float4 c2:register(c2);"
	"float4 c3:register(c3);"
	"float4 c5:register(c5);"
	"float3 unpack_hdr(float3 c){return c*rcp(1.2-saturate(c));}"
	"float3 pack_hdr(float3 c){return 1.2*c*rcp(c+1.0);}"
	"float4 main(float2 uv:TEXCOORD0):COLOR0{"
	"float4 mid=tex2Dlod(s0,float4(uv,0,0));"
	"float cs=mid.a*2.0-1.0;"
	"float own=max(-cs,0.0);"
	"float nb=tex2Dlod(s2,float4(uv,0,0)).r;"
	"float alpha=saturate(1.4*((nb>0.1)?((own>0.0)?2.0:1.0)*nb:max(nb,own)));"
	"if(c5.y>0.5)return float4(nb,alpha,saturate(c1.y*0.0166667),1.0);"
	"if(tex2Dlod(s1,float4(uv,0,0)).r<c2.x)return mid;"
	"if(nb<=0.0&&own<=0.0)return mid;"
	"float radius=max(nb*c1.y,1.0);"
	"float n=clamp(ceil(radius*2.5),8.0,(float)TAPS);\n"
	"float2 px=uv/c1.zw;\n"
	"float jit=frac(52.9829189*frac(dot(px,float2(0.06711056,0.00583715))))*6.2831853;\n"
	"float2 dir;sincos(1.19998162+jit,dir.y,dir.x);\n"
	"float2 disc=radius*c1.zw;\n"
	"float3 base=mid.rgb;\n"
	"#if STYLE==2\n"
	"base=unpack_hdr(base);\n"
	"#endif\n"
	"float4 sum=float4(base*nb*0.5,0.5);"
	"[loop] for(int i=0;i<TAPS;i++){"
	"if((float)i>=n)break;"
	"float fi=(float)i+0.5;"
	"float rr=fi/n;\n"
	"#if STYLE==2\n"
	"rr=pow(rr,0.35);\n"
	"#else\n"
	"rr=sqrt(rr);\n"
	"#endif\n"
	"float2 off=dir*rr*disc;"
	"dir=float2(dir.x*-0.73736888-dir.y*0.67549029,dir.x*0.67549029+dir.y*-0.73736888);"
	"float4 s=tex2Dlod(s0,float4(uv+off,0,0));"
	"float3 t=s.rgb;"
	"float w=lerp(rr,1.0,0.5);\n"
	"#if STYLE==1\n"
	"w*=exp(-2.0*rr*rr);\n"
	"#endif\n"
	"#if STYLE==2\n"
	"w*=1.0+c3.x*saturate(dot(t,float3(0.2126,0.7152,0.0722))-c3.y);"
	"t=unpack_hdr(t);\n"
	"#endif\n"
	"sum+=float4(t*w,w);"
	"}"
	"float3 outc=sum.rgb/max(sum.a,0.0001);\n"
	"#if STYLE==2\n"
	"outc=pack_hdr(outc);\n"
	"#endif\n"
	"float sc=lerp(cs,-max(nb,own),alpha);"
	"return float4(lerp(mid.rgb,outc,alpha),sc*0.5+0.5);"
	"}";

static const char k_smooth_body[] =
	"sampler2D s0:register(s0);"
	"float4 c1:register(c1);"
	"float4 c4:register(c4);"
	"float4 main(float2 uv:TEXCOORD0):COLOR0{"
	"float4 mid=tex2Dlod(s0,float4(uv,0,0));"
	"float sgn=mid.a*2.0-1.0;"
	"float cs=saturate(abs(sgn)*4.0);"
	"float nearpx=(sgn<0.0)?1.0:0.0;"
	"float steps=floor(cs*c4.w);"
	"if(steps<1.0)return mid;"
	"float expc=-2.0*rcp(steps*steps+1e-3);"
	"float2 axis=c4.xy*c1.zw;"
	"float w0=max(nearpx,saturate(cs-cs*c4.z));"
	"float3 sum=mid.rgb*w0;"
	"float wsum=w0+1e-3;"
	"[loop] for(int i=1;i<=8;i++){"
	"if((float)i>steps)break;"
	"float fi=(float)i;"
	"float w=exp(fi*fi*expc);"
	"float4 a=tex2Dlod(s0,float4(uv+axis*(2.0*fi-0.5),0,0));"
	"float4 b=tex2Dlod(s0,float4(uv-axis*(2.0*fi+0.5),0,0));"
	"float wa=w*max(nearpx,saturate(saturate(abs(a.a*2.0-1.0)*4.0)-cs*c4.z));"
	"float wb=w*max(nearpx,saturate(saturate(abs(b.a*2.0-1.0)*4.0)-cs*c4.z));"
	"sum+=a.rgb*wa+b.rgb*wb;"
	"wsum+=wa+wb;"
	"}"
	"sum/=wsum;"
	"return float4(lerp(mid.rgb,sum,saturate(wsum)),mid.a);"
	"}";

void n_depth_of_field::impl_t::selftest_intz( IDirect3DDevice9* device, IDirect3DSurface9* target )
{
	IDirect3DTexture9* own_depth = g_depth_source.own_depth( );

	if ( !device || !own_depth || !this->m_probe_surface )
		return;

	IDirect3DSurface9* depth_surface = nullptr;

	if ( FAILED( own_depth->GetSurfaceLevel( 0, &depth_surface ) ) || !depth_surface )
		return;

	device->SetRenderTarget( 0, this->m_probe_surface );
	device->SetDepthStencilSurface( depth_surface );
	device->Clear( 0, nullptr, D3DCLEAR_ZBUFFER, 0, 0.25f, 0 );
	device->SetDepthStencilSurface( nullptr );
	device->SetRenderTarget( 0, target );

	depth_surface->Release( );
}

void n_depth_of_field::impl_t::release( )
{
	if ( this->m_probe_sysmem ) {
		this->m_probe_sysmem->Release( );
		this->m_probe_sysmem = nullptr;
	}

	if ( this->m_probe_surface ) {
		this->m_probe_surface->Release( );
		this->m_probe_surface = nullptr;
	}

	if ( this->m_probe_texture ) {
		this->m_probe_texture->Release( );
		this->m_probe_texture = nullptr;
	}

	if ( this->m_resolve_surface ) {
		this->m_resolve_surface->Release( );
		this->m_resolve_surface = nullptr;
	}

	if ( this->m_prepare_shader ) {
		this->m_prepare_shader->Release( );
		this->m_prepare_shader = nullptr;
	}

	if ( this->m_smooth_shader ) {
		this->m_smooth_shader->Release( );
		this->m_smooth_shader = nullptr;
	}

	if ( this->m_coc_shader ) {
		this->m_coc_shader->Release( );
		this->m_coc_shader = nullptr;
	}

	for ( IDirect3DPixelShader9*& shader : this->m_near_shaders ) {
		if ( shader ) {
			shader->Release( );
			shader = nullptr;
		}
	}

	if ( this->m_coc_surface ) {
		this->m_coc_surface->Release( );
		this->m_coc_surface = nullptr;
	}

	if ( this->m_coc_texture ) {
		this->m_coc_texture->Release( );
		this->m_coc_texture = nullptr;
	}

	if ( this->m_coc_tmp_surface ) {
		this->m_coc_tmp_surface->Release( );
		this->m_coc_tmp_surface = nullptr;
	}

	if ( this->m_coc_tmp_texture ) {
		this->m_coc_tmp_texture->Release( );
		this->m_coc_tmp_texture = nullptr;
	}

	if ( this->m_blur_surface ) {
		this->m_blur_surface->Release( );
		this->m_blur_surface = nullptr;
	}

	if ( this->m_blur_texture ) {
		this->m_blur_texture->Release( );
		this->m_blur_texture = nullptr;
	}

	if ( this->m_stage_surface ) {
		this->m_stage_surface->Release( );
		this->m_stage_surface = nullptr;
	}

	if ( this->m_stage_texture ) {
		this->m_stage_texture->Release( );
		this->m_stage_texture = nullptr;
	}

	if ( this->m_colour_surface ) {
		this->m_colour_surface->Release( );
		this->m_colour_surface = nullptr;
	}

	if ( this->m_colour_texture ) {
		this->m_colour_texture->Release( );
		this->m_colour_texture = nullptr;
	}

	for ( IDirect3DPixelShader9*& shader : this->m_shaders ) {
		if ( shader ) {
			shader->Release( );
			shader = nullptr;
		}
	}

	this->m_width        = 0;
	this->m_height       = 0;
	this->m_coc_width    = 0;
	this->m_coc_height   = 0;
	this->m_format       = D3DFMT_UNKNOWN;
	this->m_multi_sample = D3DMULTISAMPLE_NONE;
}

void n_depth_of_field::impl_t::on_device_lost( )
{
	this->release( );
}

float n_depth_of_field::impl_t::focus_distance( c_view_setup* setup )
{
	float target = GET_VARIABLE( g_variables.m_depth_of_field_distance, float );

	if ( GET_VARIABLE( g_variables.m_depth_of_field_auto_focus, bool ) && g_ctx.m_local ) {
		c_vector forward{ }, right{ }, up{ };
		g_math.angle_vectors( setup->m_angles, &forward, &right, &up );

		const float point_x = std::clamp( GET_VARIABLE( g_variables.m_depth_of_field_focus_x, float ), 0.f, 1.f );
		const float point_y = std::clamp( GET_VARIABLE( g_variables.m_depth_of_field_focus_y, float ), 0.f, 1.f );

		c_vector direction = forward;

		if ( point_x != 0.5f || point_y != 0.5f ) {
			const float aspect = setup->m_aspect_ratio > 0.01f
			                       ? setup->m_aspect_ratio
			                       : ( setup->m_height > 0 ? static_cast< float >( setup->m_width ) / static_cast< float >( setup->m_height ) : 1.f );

			const float tan_x = std::tan( deg2rad( std::clamp( setup->m_fov, 1.f, 170.f ) * 0.5f ) );
			const float tan_y = tan_x / std::max( aspect, 0.01f );

			direction = ( forward + right * ( ( point_x - 0.5f ) * 2.f * tan_x ) + up * ( ( 0.5f - point_y ) * 2.f * tan_y ) ).normalized( );
		}

		const float limit = std::clamp( GET_VARIABLE( g_variables.m_depth_of_field_focus_limit, float ), 64.f, 8192.f );

		const c_vector start = setup->m_origin;
		const c_vector end   = start + direction * 8192.f;

		ray_t ray( start, end );
		c_trace_filter filter( g_ctx.m_local );
		trace_t trace;

		const unsigned int mask = GET_VARIABLE( g_variables.m_depth_of_field_focus_see_through, bool )
		                            ? static_cast< unsigned int >( e_contents::contents_solid | e_contents::contents_moveable | e_contents::contents_monster |
		                                                           e_contents::contents_hitbox )
		                            : static_cast< unsigned int >( e_mask::mask_shot_hull | e_contents::contents_hitbox );

		g_interfaces.m_engine_trace->trace_ray( ray, mask, &filter, &trace );

		target = std::clamp( ( trace.m_end - start ).dot_product( forward ), 16.f, limit );
	}

	if ( this->m_focus <= 0.f )
		this->m_focus = target;

	const float speed      = std::clamp( GET_VARIABLE( g_variables.m_depth_of_field_focus_speed, float ), 0.1f, 40.f );
	const float frame_time = g_interfaces.m_global_vars_base ? g_interfaces.m_global_vars_base->m_frame_time : 0.f;
	const float rate       = frame_time > 0.f ? 1.f - std::exp( -speed * std::min( frame_time, 0.25f ) ) : 1.f;

	this->m_focus += ( target - this->m_focus ) * rate;

	return this->m_focus;
}

IDirect3DPixelShader9* n_depth_of_field::impl_t::ensure_shader( IDirect3DDevice9* device, const int style, const int quality, const bool debug )
{
	const int shape = std::clamp( style, 0, 2 );
	const int tier  = std::clamp( quality, 0, 3 );

	const int index = debug ? 12 : shape * 4 + tier;

	if ( this->m_shaders[ index ] )
		return this->m_shaders[ index ];

	if ( this->m_shader_failed[ index ] )
		return nullptr;

	const int taps = 16 << tier;

	const int debug_flag = debug ? 1 : 0;

	const std::string source =
		std::vformat( "#define STYLE {:d}\n#define TAPS {:d}\n#define DEBUG {:d}\n", std::make_format_args( shape, taps, debug_flag ) ) +
		k_shader_body;

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

		this->m_shader_failed[ index ] = true;
		return nullptr;
	}

	if ( errors )
		errors->Release( );

	result = device->CreatePixelShader( static_cast< const DWORD* >( code->GetBufferPointer( ) ), &this->m_shaders[ index ] );
	code->Release( );

	if ( FAILED( result ) || !this->m_shaders[ index ] ) {
		this->m_shaders[ index ] = nullptr;
		this->m_compile_failed = true;
		return nullptr;
	}

	g_console.print(
		std::vformat( "depth of field: blur shader built, style {:d} taps {:d} debug {:d}", std::make_format_args( shape, taps, debug_flag ) )
			.c_str( ) );

	return this->m_shaders[ index ];
}

IDirect3DPixelShader9* n_depth_of_field::impl_t::ensure_prepare_shader( IDirect3DDevice9* device )
{
	if ( this->m_prepare_shader || this->m_prepare_failed )
		return this->m_prepare_shader;

	ID3DXBuffer* code   = nullptr;
	ID3DXBuffer* errors = nullptr;

	HRESULT result = D3DXCompileShader( k_prepare_body, static_cast< UINT >( std::strlen( k_prepare_body ) ), nullptr, nullptr, "main", "ps_3_0", 0,
	                                    &code, &errors, nullptr );

	if ( FAILED( result ) || !code ) {
		if ( errors ) {
			g_console.print< n_console::log_level::WARNING >( static_cast< const char* >( errors->GetBufferPointer( ) ) );
			errors->Release( );
		}

		if ( code )
			code->Release( );

		this->m_prepare_failed = true;
		return nullptr;
	}

	if ( errors )
		errors->Release( );

	result = device->CreatePixelShader( static_cast< const DWORD* >( code->GetBufferPointer( ) ), &this->m_prepare_shader );
	code->Release( );

	if ( FAILED( result ) || !this->m_prepare_shader ) {
		this->m_prepare_shader = nullptr;
		this->m_prepare_failed = true;
		return nullptr;
	}

	return this->m_prepare_shader;
}

IDirect3DPixelShader9* n_depth_of_field::impl_t::ensure_coc_shader( IDirect3DDevice9* device )
{
	if ( this->m_coc_shader || this->m_coc_failed )
		return this->m_coc_shader;

	ID3DXBuffer* code   = nullptr;
	ID3DXBuffer* errors = nullptr;

	HRESULT result = D3DXCompileShader( k_coc_body, static_cast< UINT >( std::strlen( k_coc_body ) ), nullptr, nullptr, "main", "ps_3_0", 0, &code,
	                                    &errors, nullptr );

	if ( FAILED( result ) || !code ) {
		if ( errors ) {
			g_console.print< n_console::log_level::WARNING >( static_cast< const char* >( errors->GetBufferPointer( ) ) );
			errors->Release( );
		}

		if ( code )
			code->Release( );

		this->m_coc_failed = true;
		return nullptr;
	}

	if ( errors )
		errors->Release( );

	result = device->CreatePixelShader( static_cast< const DWORD* >( code->GetBufferPointer( ) ), &this->m_coc_shader );
	code->Release( );

	if ( FAILED( result ) || !this->m_coc_shader ) {
		this->m_coc_shader = nullptr;
		this->m_coc_failed = true;
		return nullptr;
	}

	return this->m_coc_shader;
}

IDirect3DPixelShader9* n_depth_of_field::impl_t::ensure_near_shader( IDirect3DDevice9* device, const int style, const int quality )
{
	const int shape = std::clamp( style, 0, 2 );
	const int tier  = std::clamp( quality, 0, 3 );
	const int index = shape * 4 + tier;

	if ( this->m_near_shaders[ index ] )
		return this->m_near_shaders[ index ];

	if ( this->m_near_failed[ index ] )
		return nullptr;

	const int taps = 16 << tier;

	const std::string source =
		std::vformat( "#define STYLE {:d}\n#define TAPS {:d}\n", std::make_format_args( shape, taps ) ) + k_near_body;

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

		this->m_near_failed[ index ] = true;
		return nullptr;
	}

	if ( errors )
		errors->Release( );

	result = device->CreatePixelShader( static_cast< const DWORD* >( code->GetBufferPointer( ) ), &this->m_near_shaders[ index ] );
	code->Release( );

	if ( FAILED( result ) || !this->m_near_shaders[ index ] ) {
		this->m_near_shaders[ index ] = nullptr;
		this->m_near_failed[ index ]  = true;
		return nullptr;
	}

	g_console.print(
		std::vformat( "depth of field: near plane shader built, style {:d} taps {:d}", std::make_format_args( shape, taps ) ).c_str( ) );

	return this->m_near_shaders[ index ];
}

IDirect3DPixelShader9* n_depth_of_field::impl_t::ensure_smooth_shader( IDirect3DDevice9* device )
{
	if ( this->m_smooth_shader || this->m_smooth_failed )
		return this->m_smooth_shader;

	ID3DXBuffer* code   = nullptr;
	ID3DXBuffer* errors = nullptr;

	HRESULT result = D3DXCompileShader( k_smooth_body, static_cast< UINT >( std::strlen( k_smooth_body ) ), nullptr, nullptr, "main", "ps_3_0", 0,
	                                    &code, &errors, nullptr );

	if ( FAILED( result ) || !code ) {
		if ( errors ) {
			g_console.print< n_console::log_level::WARNING >( static_cast< const char* >( errors->GetBufferPointer( ) ) );
			errors->Release( );
		}

		if ( code )
			code->Release( );

		this->m_smooth_failed = true;
		return nullptr;
	}

	if ( errors )
		errors->Release( );

	result = device->CreatePixelShader( static_cast< const DWORD* >( code->GetBufferPointer( ) ), &this->m_smooth_shader );
	code->Release( );

	if ( FAILED( result ) || !this->m_smooth_shader ) {
		this->m_smooth_shader = nullptr;
		this->m_smooth_failed = true;
		return nullptr;
	}

	return this->m_smooth_shader;
}

bool n_depth_of_field::impl_t::build( IDirect3DDevice9* device, const D3DSURFACE_DESC& description )
{
	const int width  = static_cast< int >( description.Width );
	const int height = static_cast< int >( description.Height );

	if ( this->m_colour_texture && this->m_width == width && this->m_height == height && this->m_format == description.Format &&
	     this->m_multi_sample == description.MultiSampleType )
		return true;

	this->release( );

	if ( FAILED( device->CreateTexture( description.Width, description.Height, 1, D3DUSAGE_RENDERTARGET, description.Format, D3DPOOL_DEFAULT,
	                                    &this->m_stage_texture, nullptr ) ) ||
	     FAILED( this->m_stage_texture->GetSurfaceLevel( 0, &this->m_stage_surface ) ) ) {
		g_console.print< n_console::log_level::WARNING >( "depth of field: stage target creation failed" );
		this->release( );
		return false;
	}

	if ( FAILED( device->CreateTexture( description.Width, description.Height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
	                                    &this->m_colour_texture, nullptr ) ) ||
	     FAILED( this->m_colour_texture->GetSurfaceLevel( 0, &this->m_colour_surface ) ) ) {
		g_console.print< n_console::log_level::WARNING >( "depth of field: render target creation failed" );
		this->release( );
		return false;
	}

	// gather output when smoothing is on. A8R8G8B8: the coc must survive for the two gaussian draws
	if ( FAILED( device->CreateTexture( description.Width, description.Height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
	                                    &this->m_blur_texture, nullptr ) ) ||
	     FAILED( this->m_blur_texture->GetSurfaceLevel( 0, &this->m_blur_surface ) ) ) {
		g_console.print< n_console::log_level::WARNING >( "depth of field: blur target creation failed" );
		this->release( );
		return false;
	}

	const unsigned int coc_width  = std::max< unsigned int >( description.Width / 2u, 1u );
	const unsigned int coc_height = std::max< unsigned int >( description.Height / 2u, 1u );

	for ( const D3DFORMAT format : { D3DFMT_A16B16G16R16F, D3DFMT_A8R8G8B8 } ) {
		if ( SUCCEEDED( device->CreateTexture( coc_width, coc_height, 1, D3DUSAGE_RENDERTARGET, format, D3DPOOL_DEFAULT, &this->m_coc_texture,
		                                       nullptr ) ) &&
		     SUCCEEDED( this->m_coc_texture->GetSurfaceLevel( 0, &this->m_coc_surface ) ) &&
		     SUCCEEDED( device->CreateTexture( coc_width, coc_height, 1, D3DUSAGE_RENDERTARGET, format, D3DPOOL_DEFAULT, &this->m_coc_tmp_texture,
		                                       nullptr ) ) &&
		     SUCCEEDED( this->m_coc_tmp_texture->GetSurfaceLevel( 0, &this->m_coc_tmp_surface ) ) )
			break;

		if ( this->m_coc_tmp_surface ) {
			this->m_coc_tmp_surface->Release( );
			this->m_coc_tmp_surface = nullptr;
		}

		if ( this->m_coc_tmp_texture ) {
			this->m_coc_tmp_texture->Release( );
			this->m_coc_tmp_texture = nullptr;
		}

		if ( this->m_coc_surface ) {
			this->m_coc_surface->Release( );
			this->m_coc_surface = nullptr;
		}

		if ( this->m_coc_texture ) {
			this->m_coc_texture->Release( );
			this->m_coc_texture = nullptr;
		}
	}

	if ( !this->m_coc_surface || !this->m_coc_tmp_surface )
		g_console.print< n_console::log_level::WARNING >( "depth of field: near plane coc target unavailable — no near bleed" );

	this->m_coc_width  = static_cast< int >( coc_width );
	this->m_coc_height = static_cast< int >( coc_height );

	if ( description.MultiSampleType != D3DMULTISAMPLE_NONE &&
	     FAILED( device->CreateRenderTarget( description.Width, description.Height, description.Format, D3DMULTISAMPLE_NONE, 0, FALSE,
	                                         &this->m_resolve_surface, nullptr ) ) ) {
		g_console.print< n_console::log_level::WARNING >( "depth of field: msaa resolve target creation failed" );
		this->release( );
		return false;
	}

	this->m_selftest_state = 0;

	if ( FAILED( device->CreateTexture( 1, 1, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A32B32G32R32F, D3DPOOL_DEFAULT, &this->m_probe_texture, nullptr ) ) ||
	     FAILED( this->m_probe_texture->GetSurfaceLevel( 0, &this->m_probe_surface ) ) ||
	     FAILED( device->CreateOffscreenPlainSurface( 1, 1, D3DFMT_A32B32G32R32F, D3DPOOL_SYSTEMMEM, &this->m_probe_sysmem, nullptr ) ) )
		g_console.print< n_console::log_level::WARNING >( "depth of field: debug probe target unavailable" );

	this->m_width        = width;
	this->m_height       = height;
	this->m_format       = description.Format;
	this->m_multi_sample = description.MultiSampleType;

	const int samples = static_cast< int >( description.MultiSampleType );

	g_console.print( std::vformat( "depth of field: built {:d}x{:d} msaa {:d}", std::make_format_args( width, height, samples ) ).c_str( ) );

	return true;
}

void n_depth_of_field::impl_t::on_post_screen_space_effects( c_view_setup* setup )
{
	if ( !setup )
		return;

	const bool enabled = GET_VARIABLE( g_variables.m_depth_of_field, bool ) && !this->m_compile_failed;

	if ( !enabled && !this->m_active ) {
		g_serial_render.idle( );

		this->m_focus = 0.f;
		return;
	}

	frame_t frame{ };

	frame.m_znear = setup->m_znear;
	frame.m_zfar  = setup->m_zfar;
	frame.m_frame = g_interfaces.m_global_vars_base ? g_interfaces.m_global_vars_base->m_frame_count : 0;
	frame.m_time  = g_interfaces.m_global_vars_base ? g_interfaces.m_global_vars_base->m_current_time : 0.f;

	// focus trace stays on the main thread ( engine trace + local player )
	if ( enabled )
		frame.m_focus = this->focus_distance( setup );

	// always the queue when there is one: raw d3d from here races the render thread ( see ambient_occlusion )
	g_serial_render.idle( );

	const auto pass = [ this, frame ] { n_fx_compat::run( g_interfaces.m_direct_device, "dof", [ & ] { this->execute( frame ); } ); };

	if ( n_render_queue::submit( pass ) )
		return;

	pass( );
}

void n_depth_of_field::impl_t::execute( const frame_t frame )
{
	IDirect3DDevice9* device = g_interfaces.m_direct_device;

	if ( !device )
		return;

	const n_gpu_timer::scope_t gpu_timer( device, n_gpu_timer::pass_depth_of_field );

	if ( !GET_VARIABLE( g_variables.m_depth_of_field, bool ) || this->m_compile_failed ) {
		if ( this->m_colour_texture )
			this->release( );

		this->m_active = false;
		this->m_focus  = 0.f;
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
		target->Release( );
		return;
	}

	this->m_active = true;

	const int quality = std::clamp( GET_VARIABLE( g_variables.m_depth_of_field_quality, int ), 0, 3 );

	const bool debug_depth = GET_VARIABLE( g_variables.m_depth_of_field_debug_depth, bool );

	IDirect3DPixelShader9* shader = this->ensure_shader( device, k_style_bokeh, quality, debug_depth );

	IDirect3DPixelShader9* prepare = this->ensure_prepare_shader( device );

	if ( !shader || !prepare ) {
		target->Release( );
		return;
	}

	IDirect3DPixelShader9* smooth = GET_VARIABLE( g_variables.m_depth_of_field_smooth, bool ) ? this->ensure_smooth_shader( device ) : nullptr;

	if ( !this->m_blur_surface )
		smooth = nullptr;

	if ( debug_depth )
		smooth = nullptr;

	// the bleed field view is data, don't smooth it
	const bool debug_near = GET_VARIABLE( g_variables.m_depth_of_field_debug_near, bool ) && !debug_depth;

	if ( debug_near )
		smooth = nullptr;

	IDirect3DPixelShader9* near_shader = nullptr;
	IDirect3DPixelShader9* coc_shader  = nullptr;

	if ( GET_VARIABLE( g_variables.m_depth_of_field_near_blur, bool ) && !debug_depth && this->m_coc_surface && this->m_coc_tmp_surface ) {
		near_shader = this->ensure_near_shader( device, k_style_bokeh, quality );
		coc_shader  = this->ensure_coc_shader( device );

		if ( !coc_shader )
			near_shader = nullptr;
	}

	IDirect3DTexture9* depth_source = g_depth_source.ensure( device, description ) ? g_depth_source.texture( ) : nullptr;

	if ( !depth_source ) {
		static bool logged_depth = false;

		if ( !logged_depth ) {
			logged_depth = true;
			g_console.print( "depth of field: no depth target yet — load a map ( or change resolution ) once with this on" );
		}

		target->Release( );
		return;
	}

	// full frame sized, or we are sampling something that was never the scene's depth
	D3DSURFACE_DESC depth_description{ };

	if ( FAILED( depth_source->GetLevelDesc( 0, &depth_description ) ) || depth_description.Width != description.Width ||
	     depth_description.Height != description.Height ) {
		static bool logged_size = false;

		if ( !logged_size ) {
			logged_size = true;
			g_console.print< n_console::log_level::WARNING >( "depth of field: depth target size does not match the frame" );
		}

		target->Release( );
		return;
	}

	static bool logged_blit = false;

	const long blit_result = n_frame_copy::copy( device, target, this->m_resolve_surface, this->m_stage_surface );

	if ( FAILED( blit_result ) ) {
		if ( !logged_blit ) {
			logged_blit = true;

			const unsigned long code = static_cast< unsigned long >( blit_result );

			g_console.print< n_console::log_level::WARNING >(
				std::vformat( "depth of field: frame blit failed {:#x}", std::make_format_args( code ) ).c_str( ) );
		}

		target->Release( );
		return;
	}

	const float width  = static_cast< float >( this->m_width );
	const float height = static_cast< float >( this->m_height );

	struct vertex_t {
		float m_x, m_y, m_z, m_rhw, m_u, m_v;
	};

	const vertex_t quad[ 4 ] = {
		{ -0.5f, -0.5f, 0.f, 1.f, 0.f, 0.f },
		{ width - 0.5f, -0.5f, 0.f, 1.f, 1.f, 0.f },
		{ -0.5f, height - 0.5f, 0.f, 1.f, 0.f, 1.f },
		{ width - 0.5f, height - 0.5f, 0.f, 1.f, 1.f, 1.f },
	};

	const float coc_width  = static_cast< float >( this->m_coc_width );
	const float coc_height = static_cast< float >( this->m_coc_height );

	const vertex_t coc_quad[ 4 ] = {
		{ -0.5f, -0.5f, 0.f, 1.f, 0.f, 0.f },
		{ coc_width - 0.5f, -0.5f, 0.f, 1.f, 1.f, 0.f },
		{ -0.5f, coc_height - 0.5f, 0.f, 1.f, 0.f, 1.f },
		{ coc_width - 0.5f, coc_height - 0.5f, 0.f, 1.f, 1.f, 1.f },
	};

	IDirect3DSurface9* old_depth_stencil                = nullptr;
	IDirect3DPixelShader9* old_pixel_shader             = nullptr;
	IDirect3DVertexShader9* old_vertex_shader           = nullptr;
	IDirect3DBaseTexture9* old_texture_0                = nullptr;
	IDirect3DBaseTexture9* old_texture_1                = nullptr;
	IDirect3DBaseTexture9* old_texture_2                = nullptr;
	IDirect3DVertexDeclaration9* old_vertex_declaration = nullptr;
	D3DVIEWPORT9 old_viewport{ };

	DWORD z_enable, z_write, alpha_blend, alpha_test, cull, scissor, srgb_write, fvf;
	DWORD stencil, colour_write, clip_planes, fill_mode, point_size;

	DWORD address_u_0, address_v_0, min_filter_0, mag_filter_0, mip_filter_0, srgb_texture_0;
	DWORD address_u_1, address_v_1, min_filter_1, mag_filter_1, mip_filter_1, srgb_texture_1;
	DWORD address_u_2, address_v_2, min_filter_2, mag_filter_2, mip_filter_2, srgb_texture_2;

	float old_constants[ 11 ][ 4 ]{ };

	if ( FAILED( device->GetDepthStencilSurface( &old_depth_stencil ) ) )
		old_depth_stencil = nullptr;

	device->GetPixelShader( &old_pixel_shader );
	device->GetVertexShader( &old_vertex_shader );
	device->GetTexture( 0, &old_texture_0 );
	device->GetTexture( 1, &old_texture_1 );
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
	device->GetSamplerState( 0, D3DSAMP_ADDRESSU, &address_u_0 );
	device->GetSamplerState( 0, D3DSAMP_ADDRESSV, &address_v_0 );
	device->GetSamplerState( 0, D3DSAMP_MINFILTER, &min_filter_0 );
	device->GetSamplerState( 0, D3DSAMP_MAGFILTER, &mag_filter_0 );
	device->GetSamplerState( 0, D3DSAMP_MIPFILTER, &mip_filter_0 );
	device->GetSamplerState( 0, D3DSAMP_SRGBTEXTURE, &srgb_texture_0 );
	device->GetSamplerState( 1, D3DSAMP_ADDRESSU, &address_u_1 );
	device->GetSamplerState( 1, D3DSAMP_ADDRESSV, &address_v_1 );
	device->GetSamplerState( 1, D3DSAMP_MINFILTER, &min_filter_1 );
	device->GetSamplerState( 1, D3DSAMP_MAGFILTER, &mag_filter_1 );
	device->GetSamplerState( 1, D3DSAMP_MIPFILTER, &mip_filter_1 );
	device->GetSamplerState( 1, D3DSAMP_SRGBTEXTURE, &srgb_texture_1 );
	device->GetTexture( 2, &old_texture_2 );
	device->GetSamplerState( 2, D3DSAMP_ADDRESSU, &address_u_2 );
	device->GetSamplerState( 2, D3DSAMP_ADDRESSV, &address_v_2 );
	device->GetSamplerState( 2, D3DSAMP_MINFILTER, &min_filter_2 );
	device->GetSamplerState( 2, D3DSAMP_MAGFILTER, &mag_filter_2 );
	device->GetSamplerState( 2, D3DSAMP_MIPFILTER, &mip_filter_2 );
	device->GetSamplerState( 2, D3DSAMP_SRGBTEXTURE, &srgb_texture_2 );
	device->GetFVF( &fvf );
	device->GetVertexDeclaration( &old_vertex_declaration );
	device->GetPixelShaderConstantF( 0, old_constants[ 0 ], 11 );

	n_stream_guard::state_t streams{ };
	streams.capture( device );

	// log the bound depth surface once: wrong size / none = we never sample the scene's depth
	static bool logged_depth_surface = false;

	if ( !logged_depth_surface ) {
		logged_depth_surface = true;

		D3DSURFACE_DESC bound{ };

		if ( old_depth_stencil && SUCCEEDED( old_depth_stencil->GetDesc( &bound ) ) ) {
			const unsigned int bound_width  = bound.Width;
			const unsigned int bound_height = bound.Height;
			const int bound_format          = static_cast< int >( bound.Format );
			const int bound_samples         = static_cast< int >( bound.MultiSampleType );

			g_console.print( std::vformat( "depth of field: bound depth surface {:d}x{:d} format {:d} msaa {:d}",
			                               std::make_format_args( bound_width, bound_height, bound_format, bound_samples ) )
			                     .c_str( ) );
		} else {
			g_console.print< n_console::log_level::WARNING >( "depth of field: no depth surface bound at this point in the frame" );
		}
	}

	if ( g_depth_source.own_route( ) && !g_depth_source.live_route( ) && this->m_selftest_state == 0 && debug_depth ) {
		this->m_selftest_state = 1;
		g_depth_source.skip_next_resolve( );
		this->selftest_intz( device, target );
	}

	g_depth_source.resolve( device, old_depth_stencil, frame.m_frame );

	D3DVIEWPORT9 viewport{ 0, 0, static_cast< DWORD >( this->m_width ), static_cast< DWORD >( this->m_height ), 0.f, 1.f };

	device->SetDepthStencilSurface( nullptr );
	device->SetViewport( &viewport );
	device->SetVertexShader( nullptr );
	device->SetVertexDeclaration( nullptr );
	device->SetFVF( D3DFVF_XYZRHW | D3DFVF_TEX1 );
	device->SetRenderState( D3DRS_STENCILENABLE, FALSE );
	device->SetRenderState( D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE );
	device->SetRenderState( D3DRS_CLIPPLANEENABLE, 0 );
	device->SetRenderState( D3DRS_FILLMODE, D3DFILL_SOLID );
	device->SetRenderState( D3DRS_ZENABLE, FALSE );
	device->SetRenderState( D3DRS_ZWRITEENABLE, FALSE );
	device->SetRenderState( D3DRS_ALPHABLENDENABLE, FALSE );
	device->SetRenderState( D3DRS_ALPHATESTENABLE, FALSE );
	device->SetRenderState( D3DRS_CULLMODE, D3DCULL_NONE );
	device->SetRenderState( D3DRS_SCISSORTESTENABLE, FALSE );
	device->SetRenderState( D3DRS_SRGBWRITEENABLE, FALSE );
	device->SetSamplerState( 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP );
	device->SetSamplerState( 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP );
	device->SetSamplerState( 0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR );
	device->SetSamplerState( 0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR );
	device->SetSamplerState( 0, D3DSAMP_MIPFILTER, D3DTEXF_NONE );
	device->SetSamplerState( 0, D3DSAMP_SRGBTEXTURE, 0 );
	device->SetSamplerState( 1, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP );
	device->SetSamplerState( 1, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP );
	device->SetSamplerState( 1, D3DSAMP_MINFILTER, D3DTEXF_POINT );
	device->SetSamplerState( 1, D3DSAMP_MAGFILTER, D3DTEXF_POINT );
	device->SetSamplerState( 1, D3DSAMP_MIPFILTER, D3DTEXF_NONE );
	device->SetSamplerState( 1, D3DSAMP_SRGBTEXTURE, 0 );
	device->SetSamplerState( 2, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP );
	device->SetSamplerState( 2, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP );
	device->SetSamplerState( 2, D3DSAMP_MINFILTER, D3DTEXF_LINEAR );
	device->SetSamplerState( 2, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR );
	device->SetSamplerState( 2, D3DSAMP_MIPFILTER, D3DTEXF_NONE );
	device->SetSamplerState( 2, D3DSAMP_SRGBTEXTURE, 0 );
	device->SetTexture( 1, depth_source );

	const float focus = frame.m_focus;

	const float ramp = std::max( GET_VARIABLE( g_variables.m_depth_of_field_ramp, float ), 1.f );

	const float constant_0[ 4 ] = { frame.m_znear, frame.m_zfar, focus, 1.f / ramp };

	const float half_band = GET_VARIABLE( g_variables.m_depth_of_field_range, float ) * 0.5f;

	const float constant_1[ 4 ] = { half_band, GET_VARIABLE( g_variables.m_depth_of_field_max_blur, float ), 1.f / width, 1.f / height };

	const float near_ramp = std::max( std::min( ramp, focus - half_band ), 1.f );

	const float constant_2[ 4 ] = { GET_VARIABLE( g_variables.m_depth_of_field_viewmodel_sharp, bool ) ? 0.1f : 0.f,
		                            GET_VARIABLE( g_variables.m_depth_of_field_near_blur, bool ) ? 1.f : 0.f, debug_depth ? 1.f : 0.f,
		                            std::max( GET_VARIABLE( g_variables.m_depth_of_field_debug_white, float ), 1.f ) };

	if ( debug_depth ) {
		static float last_print = 0.f;
		const float now         = frame.m_time;

		if ( now - last_print > 1.f ) {
			last_print = now;

			g_console.print(
				std::vformat( "depth of field: focus {:.1f} znear {:.2f} zfar {:.1f}", std::make_format_args( focus, frame.m_znear, frame.m_zfar ) )
					.c_str( ) );
		}
	}

	const float edge_softness = 1.f;

	const float constant_3[ 4 ] = { GET_VARIABLE( g_variables.m_depth_of_field_bokeh_boost, float ),
		                            GET_VARIABLE( g_variables.m_depth_of_field_bokeh_threshold, float ), edge_softness, 1.f / near_ramp };

	const float block[ 4 ][ 4 ] = { { constant_0[ 0 ], constant_0[ 1 ], constant_0[ 2 ], constant_0[ 3 ] },
		                            { constant_1[ 0 ], constant_1[ 1 ], constant_1[ 2 ], constant_1[ 3 ] },
		                            { constant_2[ 0 ], constant_2[ 1 ], constant_2[ 2 ], constant_2[ 3 ] },
		                            { constant_3[ 0 ], constant_3[ 1 ], constant_3[ 2 ], constant_3[ 3 ] } };

	device->SetPixelShaderConstantF( 0, block[ 0 ], 4 );

	const float constant_5[ 4 ] = { near_shader ? 1.f : 0.f, debug_near ? 1.f : 0.f, 0.f, 0.f };

	device->SetPixelShaderConstantF( 5, constant_5, 1 );

	device->SetRenderState( D3DRS_COLORWRITEENABLE, 0x0f );
	device->SetRenderTarget( 0, this->m_colour_surface );
	device->SetTexture( 0, this->m_stage_texture );
	device->SetPixelShader( prepare );
	n_fx_compat::draw_up( device, D3DPT_TRIANGLESTRIP, 2, quad, sizeof( vertex_t ) );

	if ( debug_depth && this->m_probe_surface && this->m_probe_sysmem ) {
		const float now = frame.m_time;

		if ( now - this->m_probe_time > 1.f ) {
			this->m_probe_time = now;

			const float probe_constant[ 4 ] = { constant_2[ 0 ], constant_2[ 1 ], 2.f, constant_2[ 3 ] };

			device->SetTexture( 0, this->m_colour_texture );
			device->SetPixelShader( shader );
			device->SetPixelShaderConstantF( 2, probe_constant, 1 );

			const vertex_t probe_quad[ 4 ] = {
				{ -0.5f, -0.5f, 0.f, 1.f, 0.5f, 0.5f },
				{ 0.5f, -0.5f, 0.f, 1.f, 0.5f, 0.5f },
				{ -0.5f, 0.5f, 0.f, 1.f, 0.5f, 0.5f },
				{ 0.5f, 0.5f, 0.f, 1.f, 0.5f, 0.5f },
			};

			D3DVIEWPORT9 probe_viewport{ 0, 0, 1, 1, 0.f, 1.f };

			device->SetRenderState( D3DRS_COLORWRITEENABLE, 0x0f );
			device->SetRenderTarget( 0, this->m_probe_surface );
			device->SetViewport( &probe_viewport );
			n_fx_compat::draw_up( device, D3DPT_TRIANGLESTRIP, 2, probe_quad, sizeof( vertex_t ) );
			device->SetRenderTarget( 0, target );
			device->SetViewport( &viewport );

			D3DLOCKED_RECT locked{ };

			if ( SUCCEEDED( device->GetRenderTargetData( this->m_probe_surface, this->m_probe_sysmem ) ) &&
			     SUCCEEDED( this->m_probe_sysmem->LockRect( &locked, nullptr, D3DLOCK_READONLY ) ) ) {
				const float* pixel       = static_cast< const float* >( locked.pBits );
				const float raw_lod      = pixel[ 0 ];
				const float raw_gradient = pixel[ 1 ];
				const float linear       = pixel[ 2 ];
				const float frame_alpha  = pixel[ 3 ];

				this->m_probe_sysmem->UnlockRect( );

				if ( this->m_selftest_state == 1 ) {
					this->m_selftest_state = 2;

					g_console.print( std::vformat( "depth of field: INTZ selftest expected 0.250000 got {:.6f}", std::make_format_args( raw_lod ) )
					                     .c_str( ) );
				} else {
					g_console.print( std::vformat( "depth of field: centre lod {:.6f} grad {:.6f} -> {:.1f} units, coc {:.6f}",
					                               std::make_format_args( raw_lod, raw_gradient, linear, frame_alpha ) )
					                     .c_str( ) );
				}
			}
		}
	}

	const DWORD colour_only = D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE;

	// near coc field, two half res draws. must run while the colour target still holds the prepared frame
	if ( near_shader ) {
		D3DVIEWPORT9 coc_viewport{ 0, 0, static_cast< DWORD >( this->m_coc_width ), static_cast< DWORD >( this->m_coc_height ), 0.f, 1.f };

		const float coc_horizontal[ 4 ] = { 1.f, 0.f, 1.f, 0.f };
		const float coc_vertical[ 4 ]   = { 0.f, 1.f, 0.f, 0.f };

		device->SetPixelShader( coc_shader );
		device->SetRenderState( D3DRS_COLORWRITEENABLE, 0x0f );
		device->SetViewport( &coc_viewport );
		device->SetRenderTarget( 0, this->m_coc_tmp_surface );
		device->SetTexture( 0, this->m_colour_texture );
		device->SetPixelShaderConstantF( 4, coc_horizontal, 1 );
		n_fx_compat::draw_up( device, D3DPT_TRIANGLESTRIP, 2, coc_quad, sizeof( vertex_t ) );

		device->SetRenderTarget( 0, this->m_coc_surface );
		device->SetViewport( &coc_viewport );
		device->SetTexture( 0, this->m_coc_tmp_texture );
		device->SetPixelShaderConstantF( 4, coc_vertical, 1 );
		n_fx_compat::draw_up( device, D3DPT_TRIANGLESTRIP, 2, coc_quad, sizeof( vertex_t ) );
	}

	const bool gather_to_frame = !near_shader && !smooth;

	device->SetRenderTarget( 0, gather_to_frame ? target : this->m_blur_surface );
	device->SetViewport( &viewport );
	device->SetRenderState( D3DRS_COLORWRITEENABLE, gather_to_frame ? colour_only : 0x0f );
	device->SetTexture( 0, this->m_colour_texture );
	device->SetPixelShader( shader );

	if ( debug_depth )
		device->SetPixelShaderConstantF( 2, constant_2, 1 );

	n_fx_compat::draw_up( device, D3DPT_TRIANGLESTRIP, 2, quad, sizeof( vertex_t ) );

	IDirect3DTexture9* smooth_source          = this->m_blur_texture;
	IDirect3DSurface9* smooth_scratch         = this->m_colour_surface;
	IDirect3DTexture9* smooth_scratch_texture = this->m_colour_texture;

	if ( near_shader ) {
		const bool near_to_frame = !smooth;

		device->SetTexture( 0, this->m_blur_texture );
		device->SetTexture( 2, this->m_coc_texture );
		device->SetRenderTarget( 0, near_to_frame ? target : this->m_colour_surface );
		device->SetViewport( &viewport );
		device->SetRenderState( D3DRS_COLORWRITEENABLE, near_to_frame ? colour_only : 0x0f );
		device->SetPixelShader( near_shader );
		n_fx_compat::draw_up( device, D3DPT_TRIANGLESTRIP, 2, quad, sizeof( vertex_t ) );

		smooth_source          = this->m_colour_texture;
		smooth_scratch         = this->m_blur_surface;
		smooth_scratch_texture = this->m_blur_texture;
	}

	if ( smooth ) {
		// ADoF bleed factor: share of own blur a neighbour must beat. 1 = only blurrier, 0 = anything bleeds
		const float bleed = 1.f - edge_softness;

		const float smoothening = std::clamp( GET_VARIABLE( g_variables.m_depth_of_field_max_blur, float ) * 0.5f, 0.f, 4.f );

		const float horizontal[ 4 ] = { 1.f, 0.f, bleed, smoothening };
		const float vertical[ 4 ]   = { 0.f, 1.f, bleed, smoothening };

		device->SetPixelShader( smooth );
		device->SetTexture( 0, smooth_source );
		device->SetRenderTarget( 0, smooth_scratch );
		device->SetViewport( &viewport );
		device->SetRenderState( D3DRS_COLORWRITEENABLE, 0x0f );
		device->SetPixelShaderConstantF( 4, horizontal, 1 );
		n_fx_compat::draw_up( device, D3DPT_TRIANGLESTRIP, 2, quad, sizeof( vertex_t ) );

		device->SetTexture( 0, smooth_scratch_texture );
		device->SetRenderTarget( 0, target );
		device->SetViewport( &viewport );
		device->SetRenderState( D3DRS_COLORWRITEENABLE, colour_only );
		device->SetPixelShaderConstantF( 4, vertical, 1 );
		n_fx_compat::draw_up( device, D3DPT_TRIANGLESTRIP, 2, quad, sizeof( vertex_t ) );
	}

	device->SetPixelShaderConstantF( 0, old_constants[ 0 ], 11 );
	device->SetPixelShader( old_pixel_shader );
	device->SetVertexShader( old_vertex_shader );

	if ( fvf )
		device->SetFVF( fvf );

	device->SetVertexDeclaration( old_vertex_declaration );

	streams.restore( device );

	device->SetTexture( 0, old_texture_0 );
	device->SetTexture( 1, old_texture_1 );
	device->SetTexture( 2, old_texture_2 );
	device->SetSamplerState( 2, D3DSAMP_ADDRESSU, address_u_2 );
	device->SetSamplerState( 2, D3DSAMP_ADDRESSV, address_v_2 );
	device->SetSamplerState( 2, D3DSAMP_MINFILTER, min_filter_2 );
	device->SetSamplerState( 2, D3DSAMP_MAGFILTER, mag_filter_2 );
	device->SetSamplerState( 2, D3DSAMP_MIPFILTER, mip_filter_2 );
	device->SetSamplerState( 2, D3DSAMP_SRGBTEXTURE, srgb_texture_2 );
	device->SetSamplerState( 0, D3DSAMP_ADDRESSU, address_u_0 );
	device->SetSamplerState( 0, D3DSAMP_ADDRESSV, address_v_0 );
	device->SetSamplerState( 0, D3DSAMP_MINFILTER, min_filter_0 );
	device->SetSamplerState( 0, D3DSAMP_MAGFILTER, mag_filter_0 );
	device->SetSamplerState( 0, D3DSAMP_MIPFILTER, mip_filter_0 );
	device->SetSamplerState( 0, D3DSAMP_SRGBTEXTURE, srgb_texture_0 );
	device->SetSamplerState( 1, D3DSAMP_ADDRESSU, address_u_1 );
	device->SetSamplerState( 1, D3DSAMP_ADDRESSV, address_v_1 );
	device->SetSamplerState( 1, D3DSAMP_MINFILTER, min_filter_1 );
	device->SetSamplerState( 1, D3DSAMP_MAGFILTER, mag_filter_1 );
	device->SetSamplerState( 1, D3DSAMP_MIPFILTER, mip_filter_1 );
	device->SetSamplerState( 1, D3DSAMP_SRGBTEXTURE, srgb_texture_1 );
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

	if ( old_texture_2 )
		old_texture_2->Release( );

	if ( old_texture_1 )
		old_texture_1->Release( );

	if ( old_texture_0 )
		old_texture_0->Release( );

	if ( old_vertex_shader )
		old_vertex_shader->Release( );

	if ( old_pixel_shader )
		old_pixel_shader->Release( );

	target->Release( );
}
