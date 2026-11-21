#include "depth_view.h"
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

static const char k_shader[] = R"(
sampler2D s_depth:register(s0);
sampler2D s_frame:register(s1);
float4 c0:register(c0);
float4 c1:register(c1);
float4 c2:register(c2);
float4 c3:register(c3);
float4 c4:register(c4);
float4 c5:register(c5);
float4 c6:register(c6);
float4 c7:register(c7);
float lin(float2 uv){float d=tex2Dlod(s_depth,float4(uv,0,0)).r;return d/(1000.0-d*999.0);}
float lin_view(float2 uv){
	if(c4.z<0.5)return lin(uv);
	if(c5.x>0.5)uv.y=1.0-uv.y;
	uv/=c6.xy;
	uv.x-=c6.z*c0.z;
	uv.y+=c6.w*c0.w;
	float d=tex2Dlod(s_depth,float4(uv,0,0)).r*c7.x;
	if(c5.z>0.5)d=(exp(d*log(1.01))-1.0)/0.01;
	if(c5.y>0.5)d=1.0-d;
	d/=c5.w-d*(c5.w-1.0);
	return d;
}
float3 normal_at(float2 uv){
	float2 pn=uv-float2(0,c0.w);
	float2 pe=uv+float2(c0.z,0);
	float3 vc=float3(uv-0.5,1)*lin_view(uv);
	float3 vn=float3(pn-0.5,1)*lin_view(pn);
	float3 ve=float3(pe-0.5,1)*lin_view(pe);
	return normalize(cross(vc-vn,vc-ve))*0.5+0.5;
}
float4 main(float2 uv:TEXCOORD0,float2 vpos:VPOS):COLOR0{
	float4 col=tex2Dlod(s_frame,float4(uv,0,0));
	if(c0.x>0.5){
		float sd=lin(uv);
		float sf=c1.x;
		float full=c1.y+c1.z;
		float dd;
		if(c1.w>0.5){
			float2 t=(uv-c2.yz)*c7.yz;
			float fd=sqrt(dot(t,t))*(c2.x/c7.y);
			dd=sqrt(sd*sd+sf*sf-2.0*sd*sf*cos(fd*(2.0*3.1415927/360.0)));
		}else{
			dd=abs(sd-sf);
		}
		float coc=saturate(dd>full?1.0:smoothstep(c1.y,full,dd));
		float g=(col.r+col.g+col.b)/3.0;
		float4 des=float4(g,g,g,coc);
		des=lerp(des,float4(c3.rgb,coc),c3.w);
		col=lerp(col,des,saturate(coc*c2.w));
	}
	if(c0.y>0.5){
		float3 depth=lin_view(uv).xxx;
		float3 n=normal_at(uv);
		float grid=frac(dot(uv,(c7.yz*float2(1.0/16.0,10.0/36.0))+0.25));
		float ds=0.25*(1.0/(pow(2.0,8.0)-1.0));
		float3 dsr=float3(ds,-ds,ds);
		dsr=lerp(2.0*dsr,-2.0*dsr,grid);
		depth+=dsr;
		float3 c=depth;
		if(c4.x>0.5&&c4.x<1.5)c=n;
		if(c4.x>1.5)c=lerp(n,depth,step(c7.y*0.5,vpos.x));
		if(c4.y>0.5){
			float3 o=col.rgb;
			c=lerp(2.0*c*o,1.0-2.0*(1.0-c)*(1.0-o),max(c.r,max(c.g,c.b))<0.5?0.0:1.0);
		}
		col.rgb=c;
	}
	return col;
}
)";

// OtisFX AdaptiveFog.fx, both passes fused: the blend reads the bloom target only at its own pixel, so bloom runs inline and
// only where fog lands. tap weight sqrt( (x*x+y*y)/8 ) = Otis's sqrt( CurDistance/MaxDistance ), bloom width cancels.
// lin = map independent reshade lin ( far 1000 over the default 7..28377.9 range ), same as botox-tf2 fx_depthview.hlsl
static const char k_fog_shader[] = R"(
sampler2D s_depth:register(s0);
sampler2D s_frame:register(s1);
float4 c0:register(c0);
float4 c1:register(c1);
float4 c2:register(c2);
float4 c3:register(c3);
float lin(float2 uv){
	float d=tex2Dlod(s_depth,float4(uv,0,0)).r;
	if(d>=1.0)return 1.0;
	float z=c3.x*c3.y/(c3.y-d*(c3.y-c3.x));
	float r=saturate(28377.9*(z-7.0)/(z*28370.9));
	return r/(1000.0-r*999.0);
}
float4 main(float2 uv:TEXCOORD0):COLOR0{
	float4 col=tex2Dlod(s_frame,float4(uv,0,0));
	float f=clamp(saturate(lin(uv)-c2.y)*c2.x,0.0,c1.w);
	[branch]if(f<=0.0)return col;
	[branch]if(f>=1.0)return float4(c1.rgb,col.a);
	float2 tap=uv*c0.xy*c0.z;
	float3 sum=0;
	[unroll]for(int i=0;i<5;i++){
		float bx=2.5-i;
		[unroll]for(int j=0;j<5;j++){
			float by=-2.5+j;
			float3 t=tex2Dlod(s_frame,float4(uv+float2(bx,by)*tap,0,0)).rgb;
			sum+=lerp(t,col.rgb,sqrt((bx*bx+by*by)/8.0));
		}
	}
	float3 bloom=saturate(lerp(col.rgb,sum*c2.z,dot(col.rgb,float3(0.299,0.587,0.114))));
	return float4(lerp(col.rgb,lerp(bloom,c1.rgb,f),f),col.a);
}
)";

static bool view_on( )
{
	return GET_VARIABLE( g_variables.m_emphasize, bool ) || GET_VARIABLE( g_variables.m_show_depth, bool );
}

// max 0 or curve 0 = f 0 on every pixel: no pass at all
static bool fog_on( )
{
	return GET_VARIABLE( g_variables.m_adaptive_fog, bool ) && GET_VARIABLE( g_variables.m_adaptive_fog_max, float ) > 0.f &&
	       GET_VARIABLE( g_variables.m_adaptive_fog_curve, float ) > 0.f;
}

void n_depth_view::impl_t::release( )
{
	const auto drop = []( auto& resource ) {
		if ( resource ) {
			resource->Release( );
			resource = nullptr;
		}
	};

	drop( this->m_shader );
	drop( this->m_fog_shader );

	this->m_width  = 0;
	this->m_height = 0;
}

void n_depth_view::impl_t::on_device_lost( )
{
	this->release( );
	this->m_active = false;
}

IDirect3DPixelShader9* n_depth_view::impl_t::ensure_shader( IDirect3DDevice9* device, const bool fog )
{
	IDirect3DPixelShader9*& shader = fog ? this->m_fog_shader : this->m_shader;
	bool& failed                   = fog ? this->m_fog_compile_failed : this->m_compile_failed;
	const char* source             = fog ? k_fog_shader : k_shader;

	if ( shader || failed )
		return shader;

	ID3DXBuffer* code   = nullptr;
	ID3DXBuffer* errors = nullptr;

	HRESULT result =
		D3DXCompileShader( source, static_cast< UINT >( std::strlen( source ) ), nullptr, nullptr, "main", "ps_3_0", 0, &code, &errors, nullptr );

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

	result = device->CreatePixelShader( static_cast< const DWORD* >( code->GetBufferPointer( ) ), &shader );
	code->Release( );

	if ( FAILED( result ) || !shader ) {
		shader = nullptr;
		failed = true;
		return nullptr;
	}

	g_console.print( fog ? "adaptive fog: shader built" : "depth view: shader built" );

	return shader;
}

void n_depth_view::impl_t::on_post_screen_space_effects( c_view_setup* setup )
{
	this->submit( setup, false );
}

void n_depth_view::impl_t::on_post_screen_space_effects_fog( c_view_setup* setup )
{
	this->submit( setup, true );
}

void n_depth_view::impl_t::submit( c_view_setup* setup, const bool fog )
{
	if ( !setup )
		return;

	const bool view = view_on( ) && !this->m_compile_failed;
	const bool mist = fog_on( ) && !this->m_fog_compile_failed;

	// targets are shared: only the view slot drops them, and only once both are off
	if ( fog ? !mist : ( !view && ( mist || !this->m_active ) ) ) {
		g_serial_render.idle( );
		return;
	}

	frame_t frame{ };
	frame.m_frame = g_interfaces.m_global_vars_base ? g_interfaces.m_global_vars_base->m_frame_count : 0;
	frame.m_znear = setup->m_znear;
	frame.m_zfar  = setup->m_zfar;
	frame.m_fog   = fog;

	// always the queue when there is one: raw d3d from here races the render thread ( see ambient_occlusion )
	g_serial_render.idle( );

	const auto pass = [ this, frame ] { n_fx_compat::run( g_interfaces.m_direct_device, frame.m_fog ? "adaptive_fog" : "depth_view", [ & ] { this->execute( frame ); } ); };

	if ( n_render_queue::submit( pass ) )
		return;

	pass( );
}

void n_depth_view::impl_t::execute( const frame_t frame )
{
	IDirect3DDevice9* device = g_interfaces.m_direct_device;

	if ( !device )
		return;

	const n_gpu_timer::scope_t gpu_timer( device, frame.m_fog ? n_gpu_timer::pass_adaptive_fog : n_gpu_timer::pass_depth_view );

	const bool emphasize  = GET_VARIABLE( g_variables.m_emphasize, bool );
	const bool show_depth = GET_VARIABLE( g_variables.m_show_depth, bool );
	const bool view       = ( emphasize || show_depth ) && !this->m_compile_failed;
	const bool mist       = fog_on( ) && !this->m_fog_compile_failed;

	if ( !view && !mist ) {
		this->release( );
		this->m_active = false;
		return;
	}

	// zfar <= znear would divide by zero in lin
	if ( !( frame.m_fog ? mist && frame.m_zfar > frame.m_znear && frame.m_znear > 0.f : view ) )
		return;

	IDirect3DSurface9* target = nullptr;

	if ( FAILED( device->GetRenderTarget( 0, &target ) ) || !target )
		return;

	D3DSURFACE_DESC description{ };

	if ( FAILED( target->GetDesc( &description ) ) ) {
		target->Release( );
		return;
	}

	this->m_active = true;
	this->m_width  = static_cast< int >( description.Width );
	this->m_height = static_cast< int >( description.Height );

	IDirect3DPixelShader9* shader = this->ensure_shader( device, frame.m_fog );
	IDirect3DTexture9* depth      = g_depth_source.ensure( device, description ) ? g_depth_source.texture( ) : nullptr;

	D3DSURFACE_DESC depth_description{ };

	// must be frame sized, else it's not the scene's depth
	if ( !shader || !depth || FAILED( depth->GetLevelDesc( 0, &depth_description ) ) || depth_description.Width != description.Width ||
	     depth_description.Height != description.Height ) {
		target->Release( );
		return;
	}

	IDirect3DTexture9* frame_copy = n_frame_copy::grab( device, target, description );

	if ( !frame_copy ) {
		target->Release( );
		return;
	}

	IDirect3DSurface9* old_depth_stencil                = nullptr;
	IDirect3DPixelShader9* old_pixel_shader             = nullptr;
	IDirect3DVertexShader9* old_vertex_shader           = nullptr;
	IDirect3DVertexDeclaration9* old_vertex_declaration = nullptr;
	constexpr unsigned long k_stages = 2;
	constexpr unsigned int k_constants = 8;

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

	for ( unsigned long stage = 0; stage < k_stages; ++stage ) {
		// fog bloom taps land between texels: s1 linear like reshade's BackBuffer
		const DWORD filter = frame.m_fog && stage == 1 ? D3DTEXF_LINEAR : D3DTEXF_POINT;

		device->SetSamplerState( stage, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP );
		device->SetSamplerState( stage, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP );
		device->SetSamplerState( stage, D3DSAMP_MIPFILTER, D3DTEXF_NONE );
		device->SetSamplerState( stage, D3DSAMP_SRGBTEXTURE, 0 );
		device->SetSamplerState( stage, D3DSAMP_MINFILTER, filter );
		device->SetSamplerState( stage, D3DSAMP_MAGFILTER, filter );
	}

	if ( frame.m_fog ) {
		const c_color fog_color = GET_VARIABLE( g_variables.m_adaptive_fog_color, c_color );
		const float power       = GET_VARIABLE( g_variables.m_adaptive_fog_bloom_power, float );

		// Otis: blur / ( 25 - ( power - threshold * 5 ) ) * ( power + 4 ), folded to one gain
		float divisor = 25.f - ( power - GET_VARIABLE( g_variables.m_adaptive_fog_bloom_threshold, float ) * 5.f );

		if ( std::fabs( divisor ) < 1e-4f )
			divisor = 1e-4f;

		const float fog_constants[ 4 ][ 4 ] = {
			{ 1.f / width, 1.f / height, GET_VARIABLE( g_variables.m_adaptive_fog_bloom_width, float ), 0.f },
			{ fog_color[ 0 ] / 255.f, fog_color[ 1 ] / 255.f, fog_color[ 2 ] / 255.f, GET_VARIABLE( g_variables.m_adaptive_fog_max, float ) },
			{ GET_VARIABLE( g_variables.m_adaptive_fog_curve, float ), GET_VARIABLE( g_variables.m_adaptive_fog_start, float ), ( power + 4.f ) / divisor,
		      0.f },
			{ frame.m_znear, frame.m_zfar, 0.f, 0.f } };

		device->SetPixelShaderConstantF( 0, fog_constants[ 0 ], 4 );
	} else {
		const c_color blend_color = GET_VARIABLE( g_variables.m_emphasize_blend_color, c_color );

		const float scale_x = std::max( GET_VARIABLE( g_variables.m_show_depth_scale_x, float ), 0.001f );
		const float scale_y = std::max( GET_VARIABLE( g_variables.m_show_depth_scale_y, float ), 0.001f );

		const float constants[ k_constants ][ 4 ] = {
			{ emphasize ? 1.f : 0.f, show_depth ? 1.f : 0.f, 1.f / width, 1.f / height },
			{ GET_VARIABLE( g_variables.m_emphasize_focus_depth, float ), GET_VARIABLE( g_variables.m_emphasize_focus_range, float ),
		      GET_VARIABLE( g_variables.m_emphasize_focus_edge, float ), GET_VARIABLE( g_variables.m_emphasize_spherical, bool ) ? 1.f : 0.f },
			{ static_cast< float >( GET_VARIABLE( g_variables.m_emphasize_sphere_fov, int ) ), GET_VARIABLE( g_variables.m_emphasize_sphere_x, float ),
		      GET_VARIABLE( g_variables.m_emphasize_sphere_y, float ), GET_VARIABLE( g_variables.m_emphasize_effect_factor, float ) },
			{ blend_color[ 0 ] / 255.f, blend_color[ 1 ] / 255.f, blend_color[ 2 ] / 255.f, GET_VARIABLE( g_variables.m_emphasize_blend_factor, float ) },
			{ static_cast< float >( GET_VARIABLE( g_variables.m_show_depth_present_type, int ) ),
		      GET_VARIABLE( g_variables.m_show_depth_blend, bool ) ? 1.f : 0.f, GET_VARIABLE( g_variables.m_show_depth_live_preview, bool ) ? 1.f : 0.f,
		      0.f },
			{ GET_VARIABLE( g_variables.m_show_depth_upside_down, bool ) ? 1.f : 0.f, GET_VARIABLE( g_variables.m_show_depth_reversed, bool ) ? 1.f : 0.f,
		      GET_VARIABLE( g_variables.m_show_depth_logarithmic, bool ) ? 1.f : 0.f, GET_VARIABLE( g_variables.m_show_depth_far_plane, float ) },
			{ scale_x, scale_y, static_cast< float >( GET_VARIABLE( g_variables.m_show_depth_offset_x, int ) ),
		      static_cast< float >( GET_VARIABLE( g_variables.m_show_depth_offset_y, int ) ) },
			{ GET_VARIABLE( g_variables.m_show_depth_multiplier, float ), width, height, 0.f } };

		device->SetPixelShaderConstantF( 0, constants[ 0 ], k_constants );
	}

	device->SetTexture( 0, depth );
	device->SetTexture( 1, frame_copy );
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
}
