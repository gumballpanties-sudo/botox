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

void n_depth_view::impl_t::release( )
{
	const auto drop = []( auto& resource ) {
		if ( resource ) {
			resource->Release( );
			resource = nullptr;
		}
	};

	drop( this->m_resolve_surface );
	drop( this->m_frame_surface );
	drop( this->m_frame_texture );
	drop( this->m_shader );

	this->m_width        = 0;
	this->m_height       = 0;
	this->m_format       = D3DFMT_UNKNOWN;
	this->m_multi_sample = D3DMULTISAMPLE_NONE;
}

void n_depth_view::impl_t::on_device_lost( )
{
	this->release( );
	this->m_active = false;
}

IDirect3DPixelShader9* n_depth_view::impl_t::ensure_shader( IDirect3DDevice9* device )
{
	if ( this->m_shader || this->m_compile_failed )
		return this->m_shader;

	ID3DXBuffer* code   = nullptr;
	ID3DXBuffer* errors = nullptr;

	HRESULT result = D3DXCompileShader( k_shader, static_cast< UINT >( std::strlen( k_shader ) ), nullptr, nullptr, "main", "ps_3_0", 0, &code,
	                                    &errors, nullptr );

	if ( FAILED( result ) || !code ) {
		if ( errors ) {
			g_console.print< n_console::log_level::WARNING >( static_cast< const char* >( errors->GetBufferPointer( ) ) );
			errors->Release( );
		}

		if ( code )
			code->Release( );

		this->m_compile_failed = true;
		return nullptr;
	}

	if ( errors )
		errors->Release( );

	result = device->CreatePixelShader( static_cast< const DWORD* >( code->GetBufferPointer( ) ), &this->m_shader );
	code->Release( );

	if ( FAILED( result ) || !this->m_shader ) {
		this->m_shader         = nullptr;
		this->m_compile_failed = true;
		return nullptr;
	}

	g_console.print( "depth view: shader built" );

	return this->m_shader;
}

bool n_depth_view::impl_t::build( IDirect3DDevice9* device, const D3DSURFACE_DESC& description )
{
	const int width  = static_cast< int >( description.Width );
	const int height = static_cast< int >( description.Height );

	if ( this->m_frame_texture && this->m_width == width && this->m_height == height && this->m_format == description.Format &&
	     this->m_multi_sample == description.MultiSampleType )
		return true;

	const auto drop = []( auto& resource ) {
		if ( resource ) {
			resource->Release( );
			resource = nullptr;
		}
	};

	drop( this->m_resolve_surface );
	drop( this->m_frame_surface );
	drop( this->m_frame_texture );

	if ( FAILED( device->CreateTexture( description.Width, description.Height, 1, D3DUSAGE_RENDERTARGET, description.Format, D3DPOOL_DEFAULT,
	                                    &this->m_frame_texture, nullptr ) ) ||
	     FAILED( this->m_frame_texture->GetSurfaceLevel( 0, &this->m_frame_surface ) ) ||
	     ( description.MultiSampleType != D3DMULTISAMPLE_NONE &&
	       FAILED( device->CreateRenderTarget( description.Width, description.Height, description.Format, D3DMULTISAMPLE_NONE, 0, FALSE,
	                                           &this->m_resolve_surface, nullptr ) ) ) ) {
		g_console.print< n_console::log_level::WARNING >( "depth view: frame copy creation failed" );
		drop( this->m_resolve_surface );
		drop( this->m_frame_surface );
		drop( this->m_frame_texture );
		return false;
	}

	this->m_width        = width;
	this->m_height       = height;
	this->m_format       = description.Format;
	this->m_multi_sample = description.MultiSampleType;

	return true;
}

void n_depth_view::impl_t::on_post_screen_space_effects( c_view_setup* setup )
{
	if ( !setup )
		return;

	const bool enabled =
		( GET_VARIABLE( g_variables.m_emphasize, bool ) || GET_VARIABLE( g_variables.m_show_depth, bool ) ) && !this->m_compile_failed;

	if ( !enabled && !this->m_active ) {
		g_serial_render.idle( );
		return;
	}

	frame_t frame{ };
	frame.m_frame = g_interfaces.m_global_vars_base ? g_interfaces.m_global_vars_base->m_frame_count : 0;

	// always the queue when there is one: raw d3d from here races the render thread ( see ambient_occlusion )
	g_serial_render.idle( );

	const auto pass = [ this, frame ] { n_fx_compat::run( g_interfaces.m_direct_device, "depth_view", [ & ] { this->execute( frame ); } ); };

	if ( n_render_queue::submit( pass ) )
		return;

	pass( );
}

void n_depth_view::impl_t::execute( const frame_t frame )
{
	IDirect3DDevice9* device = g_interfaces.m_direct_device;

	if ( !device )
		return;

	const n_gpu_timer::scope_t gpu_timer( device, n_gpu_timer::pass_depth_view );

	const bool emphasize  = GET_VARIABLE( g_variables.m_emphasize, bool );
	const bool show_depth = GET_VARIABLE( g_variables.m_show_depth, bool );

	if ( ( !emphasize && !show_depth ) || this->m_compile_failed ) {
		this->release( );
		this->m_active = false;
		return;
	}

	IDirect3DSurface9* target = nullptr;

	if ( FAILED( device->GetRenderTarget( 0, &target ) ) || !target )
		return;

	D3DSURFACE_DESC description{ };

	if ( FAILED( target->GetDesc( &description ) ) || !this->build( device, description ) ) {
		target->Release( );
		return;
	}

	this->m_active = true;

	IDirect3DPixelShader9* shader = this->ensure_shader( device );
	IDirect3DTexture9* depth      = g_depth_source.ensure( device, description ) ? g_depth_source.texture( ) : nullptr;

	D3DSURFACE_DESC depth_description{ };

	// must be frame sized, else it's not the scene's depth
	if ( !shader || !depth || FAILED( depth->GetLevelDesc( 0, &depth_description ) ) || depth_description.Width != description.Width ||
	     depth_description.Height != description.Height ) {
		target->Release( );
		return;
	}

	const long blit_result = n_frame_copy::copy( device, target, this->m_resolve_surface, this->m_frame_surface );

	if ( FAILED( blit_result ) ) {
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
		device->SetSamplerState( stage, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP );
		device->SetSamplerState( stage, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP );
		device->SetSamplerState( stage, D3DSAMP_MIPFILTER, D3DTEXF_NONE );
		device->SetSamplerState( stage, D3DSAMP_SRGBTEXTURE, 0 );
		device->SetSamplerState( stage, D3DSAMP_MINFILTER, D3DTEXF_POINT );
		device->SetSamplerState( stage, D3DSAMP_MAGFILTER, D3DTEXF_POINT );
	}

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
	device->SetTexture( 0, depth );
	device->SetTexture( 1, this->m_frame_texture );
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
