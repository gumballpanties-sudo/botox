#include "ambient_occlusion.h"
#include "../../../game/sdk/includes/includes.h"
#include "../../../globals/includes/includes.h"
#include "../../entity_cache/entity_cache.h"
#include "depth_source.h"
#include "gpu_timer.h"
#include "fx_compat.h"
#include "render_queue.h"
#include "serial_render.h"
#include "stream_guard.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <d3dx9.h>
#include <string>

static const char k_prepare_body[] =
	"sampler2D s0:register(s0);"
	"float4 c0:register(c0);"
	"float4 c1:register(c1);"
	"float linear_z(float d){return (c0.x*c0.y)/(c0.y-d*(c0.y-c0.x));}"
	"float3 view_pos(float2 uv,float z){return float3((uv.x*2.0-1.0)*c0.z,(1.0-uv.y*2.0)*c0.w,1.0)*z;}"
	"float3 at(float2 uv){return view_pos(uv,linear_z(tex2Dlod(s0,float4(uv,0,0)).r));}"
	"float4 main(float2 uv:TEXCOORD0):COLOR0{"
	"float3 p=at(uv);"
	"if(p.z>=c0.y*0.98)return float4(0.5,0.5,0.5,1);"
	"float3 xr=at(uv+float2(c1.x,0))-p;"
	"float3 xl=p-at(uv-float2(c1.x,0));"
	"float3 yd=at(uv+float2(0,c1.y))-p;"
	"float3 yu=p-at(uv-float2(0,c1.y));"
	"float3 dx=(abs(xr.z)<abs(xl.z))?xr:xl;"
	"float3 dy=(abs(yd.z)<abs(yu.z))?yd:yu;"
	"float3 n=normalize(cross(dy,dx));"
	"n*=(dot(n,p)>0.0)?-1.0:1.0;"
	"return float4(n*0.5+0.5,1);"
	"}";

static const char k_occlusion_body[] =
	"sampler2D s0:register(s0);"
	"sampler2D s1:register(s1);"
	"float4 c0:register(c0);"
	"float4 c1:register(c1);"
	"float4 c2:register(c2);"
	"float4 c3:register(c3);"
	"float linear_z(float d){return (c0.x*c0.y)/(c0.y-d*(c0.y-c0.x));}"
	"float3 view_pos(float2 uv,float z){return float3((uv.x*2.0-1.0)*c0.z,(1.0-uv.y*2.0)*c0.w,1.0)*z;}"
	"float4 main(float2 uv:TEXCOORD0):COLOR0{"
	"float raw=tex2Dlod(s1,float4(uv,0,0)).r;"
	"float z=linear_z(raw);"
	"if(z>=c0.y*0.98||raw<c2.y||z>=c3.z)return 0.0;"
	"float3 p=view_pos(uv,z);"
	"float3 n=normalize(tex2Dlod(s0,float4(uv,0,0)).xyz*2.0-1.0);"
	"p+=n*(z*c2.w);"
	"float rad=min(c1.z/(2.0*c0.z*z),c2.z);"
	"float jit=frac(52.9829189*frac(dot(uv*c3.xy,float2(0.06711056,0.00583715))))*6.2831853;"
	"float ao=0.0;"
	"float2 dir;sincos(1.19998162+jit,dir.y,dir.x);"
	"int count=(int)c1.y;\n"
	"[loop] for(int s=0;s<count;s++){"
	"float i=(float)s+0.5;"
	"float rr=sqrt(i/c1.y)*rad;"
	"float2 off=dir*rr*float2(1.0,c1.x);"
	"dir=float2(dir.x*-0.73736888-dir.y*0.67549029,dir.x*0.67549029+dir.y*-0.73736888);"
	"float2 t=uv+off;"
	"float traw=tex2Dlod(s1,float4(t,0,0)).r;"
	"float tz=linear_z(traw);"
	"float3 d=view_pos(t,tz)-p;"
	"float v2=dot(d,d);"
	"float vn=dot(d,n)*rsqrt(v2);"
	// taps must be world too: viewmodel taps unproject to a huge occluder at the eye (dark cloud around the gun)
	"ao+=saturate(1.0+c1.w*v2)*saturate(vn-c2.x)*step(c2.y,traw);"
	"}"
	"ao=saturate(ao/((1.0-c2.x)*c1.y)*2.0);"
	"return sqrt(ao).xxxx;"
	"}";

static const char k_composite_body[] =
	"sampler2D s0:register(s0);"
	"sampler2D s1:register(s1);"
	"sampler2D s2:register(s2);"
	"float4 c0:register(c0);"
	"float4 c1:register(c1);"
	"float4 c2:register(c2);"
	"float4 c3:register(c3);"
	"float4 c4:register(c4);"
	"float4 smokes[6]:register(c5);"
	"float linear_z(float d){return (c0.x*c0.y)/(c0.y-d*(c0.y-c0.x));}"
	"float3 view_pos(float2 uv,float z){return float3((uv.x*2.0-1.0)*c1.z,(1.0-uv.y*2.0)*c1.w,1.0)*z;}"
	"static const float2 k_taps[8]={"
	"float2(1.5,0.5),float2(-1.5,-0.5),float2(-0.5,1.5),float2(0.5,-1.5),"
	"float2(1.5,2.5),float2(-1.5,-2.5),float2(-2.5,1.5),float2(2.5,-1.5)};"
	"float4 main(float2 uv:TEXCOORD0):COLOR0{"
	"float raw=tex2Dlod(s1,float4(uv,0,0)).r;"
	"float z=linear_z(raw);"
	"if(z>=c0.y*0.98||raw<c2.w||z>=c1.y)return float4(1,1,1,1);"
	"float3 n=tex2Dlod(s2,float4(uv,0,0)).xyz*2.0-1.0;"
	"float ao=tex2Dlod(s0,float4(uv,0,0)).r;"
	"float sum=1.0;\n"
	"[unroll] for(int i=0;i<8;i++){"
	"float2 t=uv+k_taps[i]*c0.zw*c1.x;"
	"float tz=linear_z(tex2Dlod(s1,float4(t,0,0)).r);"
	"float3 tn=tex2Dlod(s2,float4(t,0,0)).xyz*2.0-1.0;"
	"float dw=saturate(1.0-abs(tz-z)/(z*0.05+1.0));"
	"float nw=saturate(dot(tn,n)*16.0-15.0);"
	"float w=dw*nw;"
	"ao+=tex2Dlod(s0,float4(t,0,0)).r*w;"
	"sum+=w;"
	"}"
	"ao/=sum;"
	"ao*=ao;"
	"ao=1.0-pow(saturate(1.0-ao),c2.x);"
	"ao*=1.0-saturate((z-c2.y)*c2.z);"
	"ao*=1.0-saturate((z-c3.x)*c3.y)*c3.z;"
	"if(c4.x>0.5){"
	"float3 p=view_pos(uv,z);"
	"float dd=dot(p,p);"
	"float len=sqrt(dd);"
	"float smoked=0.0;\n"
	// must unroll: ps can't index constants by loop counter. radius 0 slot fails the chord test
	"[unroll] for(int j=0;j<6;j++){"
	"float4 sm=smokes[j];"
	"float3 cc=view_pos(sm.xy,sm.z);"
	"float t=dot(cc,p)/dd;"
	"float3 q=p*t-cc;"
	"float h2=sm.w*sm.w-dot(q,q);"
	"if(h2>0.0){"
	"float h=sqrt(h2/dd);"
	"smoked+=max(min(t+h,1.0)-max(t-h,0.0),0.0)*len;"
	"}"
	"}"
	"ao*=1.0-saturate(smoked*c4.y);"
	"}"
	"ao*=1.0-c4.z;"
	"return float4(1.0-ao,1.0-ao,1.0-ao,1.0);"
	"}";

void n_ambient_occlusion::impl_t::release( )
{
	if ( this->m_occlusion_surface ) {
		this->m_occlusion_surface->Release( );
		this->m_occlusion_surface = nullptr;
	}

	if ( this->m_occlusion_texture ) {
		this->m_occlusion_texture->Release( );
		this->m_occlusion_texture = nullptr;
	}

	if ( this->m_normal_surface ) {
		this->m_normal_surface->Release( );
		this->m_normal_surface = nullptr;
	}

	if ( this->m_normal_texture ) {
		this->m_normal_texture->Release( );
		this->m_normal_texture = nullptr;
	}

	for ( IDirect3DPixelShader9*& shader : this->m_shaders ) {
		if ( shader ) {
			shader->Release( );
			shader = nullptr;
		}
	}

	this->m_width            = 0;
	this->m_height           = 0;
	this->m_occlusion_width  = 0;
	this->m_occlusion_height = 0;
	this->m_scale            = 0.f;
}

void n_ambient_occlusion::impl_t::on_device_lost( )
{
	this->release( );
}

IDirect3DPixelShader9* n_ambient_occlusion::impl_t::ensure_shader( IDirect3DDevice9* device, const e_shader which )
{
	if ( this->m_shaders[ which ] )
		return this->m_shaders[ which ];

	if ( this->m_compile_failed )
		return nullptr;

	const char* source = which == shader_prepare ? k_prepare_body : ( which == shader_occlusion ? k_occlusion_body : k_composite_body );

	ID3DXBuffer* code   = nullptr;
	ID3DXBuffer* errors = nullptr;

	HRESULT result = D3DXCompileShader( source, static_cast< UINT >( std::strlen( source ) ), nullptr, nullptr, "main", "ps_3_0", 0, &code, &errors,
	                                    nullptr );

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

	result = device->CreatePixelShader( static_cast< const DWORD* >( code->GetBufferPointer( ) ), &this->m_shaders[ which ] );
	code->Release( );

	if ( FAILED( result ) || !this->m_shaders[ which ] ) {
		this->m_shaders[ which ] = nullptr;
		this->m_compile_failed   = true;
		return nullptr;
	}

	const int index = static_cast< int >( which );

	g_console.print( std::vformat( "ambient occlusion: shader {:d} built", std::make_format_args( index ) ).c_str( ) );

	return this->m_shaders[ which ];
}

bool n_ambient_occlusion::impl_t::build( IDirect3DDevice9* device, const D3DSURFACE_DESC& description )
{
	const int width  = static_cast< int >( description.Width );
	const int height = static_cast< int >( description.Height );

	const float scale = std::clamp( GET_VARIABLE( g_variables.m_ambient_occlusion_scale, float ), 0.25f, 1.f );

	if ( this->m_normal_texture && this->m_width == width && this->m_height == height && this->m_scale == scale )
		return true;

	IDirect3DPixelShader9* kept[ shader_max ]{ };

	for ( int i = 0; i < shader_max; ++i ) {
		kept[ i ]           = this->m_shaders[ i ];
		this->m_shaders[ i ] = nullptr;
	}

	this->release( );

	for ( int i = 0; i < shader_max; ++i )
		this->m_shaders[ i ] = kept[ i ];

	if ( FAILED( device->CreateTexture( description.Width, description.Height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
	                                    &this->m_normal_texture, nullptr ) ) ||
	     FAILED( this->m_normal_texture->GetSurfaceLevel( 0, &this->m_normal_surface ) ) ) {
		g_console.print< n_console::log_level::WARNING >( "ambient occlusion: normal target creation failed" );
		this->release( );
		return false;
	}

	const int occlusion_width  = std::max( static_cast< int >( width * scale ), 16 );
	const int occlusion_height = std::max( static_cast< int >( height * scale ), 16 );

	if ( FAILED( device->CreateTexture( occlusion_width, occlusion_height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
	                                    &this->m_occlusion_texture, nullptr ) ) ||
	     FAILED( this->m_occlusion_texture->GetSurfaceLevel( 0, &this->m_occlusion_surface ) ) ) {
		g_console.print< n_console::log_level::WARNING >( "ambient occlusion: occlusion target creation failed" );
		this->release( );
		return false;
	}

	this->m_width            = width;
	this->m_height           = height;
	this->m_occlusion_width  = occlusion_width;
	this->m_occlusion_height = occlusion_height;
	this->m_scale            = scale;

	g_console.print(
		std::vformat( "ambient occlusion: built {:d}x{:d}, occlusion {:d}x{:d}", std::make_format_args( width, height, occlusion_width, occlusion_height ) )
			.c_str( ) );

	return true;
}

bool n_ambient_occlusion::impl_t::fog_from_entity( c_base_entity* entity )
{
	if ( !entity )
		return false;

	const auto client_renderable = entity->get_client_renderable( );
	if ( !client_renderable )
		return false;

	const auto client_unknown = client_renderable->get_client_unknown( );
	if ( !client_unknown )
		return false;

	const auto client_networkable = client_unknown->get_client_networkable( );
	if ( !client_networkable )
		return false;

	const auto client_class = client_networkable->get_client_class( );
	if ( !client_class || client_class->m_class_id != e_class_ids::c_fog_controller )
		return false;

	this->m_fog_density = 0.f;

	if ( !entity->get_fog_enable( ) )
		return true;

	const float start = entity->get_fog_start( );
	const float end   = entity->get_fog_end( );

	if ( end <= start )
		return true;

	this->m_fog_start   = start;
	this->m_fog_end     = end;
	this->m_fog_density = std::clamp( entity->get_fog_density( ), 0.f, 1.f );

	return true;
}

void n_ambient_occlusion::impl_t::read_fog( )
{
	const int frame = g_interfaces.m_global_vars_base ? g_interfaces.m_global_vars_base->m_frame_count : 0;

	if ( frame == this->m_fog_frame )
		return;

	this->m_fog_frame = frame;

	if ( this->m_fog_index > 0 && g_interfaces.m_client_entity_list &&
	     this->fog_from_entity( g_interfaces.m_client_entity_list->get< c_base_entity >( this->m_fog_index ) ) )
		return;

	this->m_fog_index   = 0;
	this->m_fog_density = 0.f;

	if ( frame - this->m_fog_scan_frame < 64 )
		return;

	this->m_fog_scan_frame = frame;

	g_entity_cache.enumerate( type_edicts, [ & ]( c_base_entity* entity ) {
		if ( this->m_fog_index > 0 || !this->fog_from_entity( entity ) )
			return;

		this->m_fog_index = entity->get_index( );
	} );
}

bool n_ambient_occlusion::impl_t::smoke_from_entity( c_base_entity* entity, c_vector& centre, float& radius )
{
	if ( !entity )
		return false;

	const auto client_renderable = entity->get_client_renderable( );
	if ( !client_renderable )
		return false;

	const auto client_unknown = client_renderable->get_client_unknown( );
	if ( !client_unknown )
		return false;

	const auto client_networkable = client_unknown->get_client_networkable( );
	if ( !client_networkable )
		return false;

	const auto client_class = client_networkable->get_client_class( );
	if ( !client_class || client_class->m_class_id != e_class_ids::c_smoke_grenade_projectile )
		return false;

	const int begin = entity->get_smoke_effect_tick_begin( );

	if ( !entity->did_smoke_effect( ) || begin <= 0 )
		return true;

	const float interval = g_interfaces.m_global_vars_base ? g_interfaces.m_global_vars_base->m_interval_per_tick : 0.015625f;
	const float elapsed  = static_cast< float >( ( g_interfaces.m_global_vars_base ? g_interfaces.m_global_vars_base->m_tick_count : 0 ) - begin ) *
	                      interval;

	if ( elapsed < 0.f || elapsed > 17.5f )
		return true;

	centre = entity->get_abs_origin( ) + c_vector( 0.f, 0.f, 68.f );

	radius = 140.f * std::clamp( elapsed, 0.f, 1.f ) * std::clamp( ( 17.5f - elapsed ) / 3.f, 0.f, 1.f );

	return true;
}

void n_ambient_occlusion::impl_t::read_smokes( const c_view_setup* setup, frame_t& frame )
{
	if ( !setup || !g_interfaces.m_client_entity_list || !g_interfaces.m_engine_client )
		return;

	const int frame_count = g_interfaces.m_global_vars_base ? g_interfaces.m_global_vars_base->m_frame_count : 0;

	if ( frame_count - this->m_smoke_scan_frame >= 32 ) {
		this->m_smoke_scan_frame  = frame_count;
		this->m_smoke_index_count = 0;

		g_entity_cache.enumerate( type_edicts, [ & ]( c_base_entity* entity ) {
			if ( this->m_smoke_index_count >= 6 )
				return;

			c_vector centre{ };
			float radius = 0.f;

			if ( !this->smoke_from_entity( entity, centre, radius ) )
				return;

			this->m_smoke_indices[ this->m_smoke_index_count++ ] = entity->get_index( );
		} );
	}

	const auto& matrix = g_interfaces.m_engine_client->get_world_to_screen_matrix( );
	const c_vector eye = setup->m_origin;

	for ( int i = 0; i < this->m_smoke_index_count && frame.m_smoke_count < 6; ++i ) {
		c_vector centre{ };
		float radius = 0.f;

		if ( !this->smoke_from_entity( g_interfaces.m_client_entity_list->get< c_base_entity >( this->m_smoke_indices[ i ] ), centre, radius ) ||
		     radius <= 0.f )
			continue;

		const c_vector offset = centre - eye;

		if ( offset.length( ) <= radius ) {
			frame.m_smoke_inside = true;
			continue;
		}

		const float w = matrix[ 3 ][ 0 ] * centre.m_x + matrix[ 3 ][ 1 ] * centre.m_y + matrix[ 3 ][ 2 ] * centre.m_z + matrix[ 3 ][ 3 ];

		if ( w < 1.f )
			continue;

		const float inverse = 1.f / w;

		const float x = ( matrix[ 0 ][ 0 ] * centre.m_x + matrix[ 0 ][ 1 ] * centre.m_y + matrix[ 0 ][ 2 ] * centre.m_z + matrix[ 0 ][ 3 ] ) * inverse;
		const float y = ( matrix[ 1 ][ 0 ] * centre.m_x + matrix[ 1 ][ 1 ] * centre.m_y + matrix[ 1 ][ 2 ] * centre.m_z + matrix[ 1 ][ 3 ] ) * inverse;

		float* slot = frame.m_smoke[ frame.m_smoke_count++ ];

		slot[ 0 ] = 0.5f + x * 0.5f;
		slot[ 1 ] = 0.5f - y * 0.5f;
		slot[ 2 ] = w;
		slot[ 3 ] = radius;
	}
}

void n_ambient_occlusion::impl_t::on_post_screen_space_effects( c_view_setup* setup )
{
	if ( !setup )
		return;

	const bool enabled = GET_VARIABLE( g_variables.m_ambient_occlusion, bool ) && !this->m_compile_failed;

	if ( !enabled && !this->m_active ) {
		g_serial_render.idle( );
		return;
	}

	frame_t frame{ };

	frame.m_znear = setup->m_znear;
	frame.m_zfar  = setup->m_zfar;
	frame.m_fov   = setup->m_fov;
	frame.m_frame = g_interfaces.m_global_vars_base ? g_interfaces.m_global_vars_base->m_frame_count : 0;

	if ( enabled && GET_VARIABLE( g_variables.m_ambient_occlusion_fog_fade, bool ) ) {
		this->read_fog( );

		frame.m_fog_start   = this->m_fog_start;
		frame.m_fog_end     = this->m_fog_end;
		frame.m_fog_density = this->m_fog_density;
	}

	if ( enabled && GET_VARIABLE( g_variables.m_ambient_occlusion_smoke_fade, bool ) )
		this->read_smokes( setup, frame );

	g_serial_render.idle( );

	const auto pass = [ this, frame ] { n_fx_compat::run( g_interfaces.m_direct_device, "ao", [ & ] { this->execute( frame ); } ); };

	if ( n_render_queue::submit( pass ) )
		return;

	pass( );
}

void n_ambient_occlusion::impl_t::execute( const frame_t frame )
{
	IDirect3DDevice9* device = g_interfaces.m_direct_device;

	if ( !device )
		return;

	const n_gpu_timer::scope_t gpu_timer( device, n_gpu_timer::pass_ambient_occlusion );

	if ( !GET_VARIABLE( g_variables.m_ambient_occlusion, bool ) || this->m_compile_failed ) {
		if ( this->m_normal_texture )
			this->release( );

		this->m_active = false;
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

	IDirect3DPixelShader9* prepare   = this->ensure_shader( device, shader_prepare );
	IDirect3DPixelShader9* occlusion = this->ensure_shader( device, shader_occlusion );
	IDirect3DPixelShader9* composite = this->ensure_shader( device, shader_composite );

	if ( !prepare || !occlusion || !composite ) {
		target->Release( );
		return;
	}

	IDirect3DTexture9* depth = g_depth_source.ensure( device, description ) ? g_depth_source.texture( ) : nullptr;

	if ( !depth ) {
		static bool logged_depth = false;

		if ( !logged_depth ) {
			logged_depth = true;
			g_console.print( "ambient occlusion: no depth target yet — load a map ( or change resolution ) once with this on" );
		}

		target->Release( );
		return;
	}

	// must be frame sized, else it's not the scene's depth
	D3DSURFACE_DESC depth_description{ };

	if ( FAILED( depth->GetLevelDesc( 0, &depth_description ) ) || depth_description.Width != description.Width ||
	     depth_description.Height != description.Height ) {
		static bool logged_size = false;

		if ( !logged_size ) {
			logged_size = true;
			g_console.print< n_console::log_level::WARNING >( "ambient occlusion: depth target size does not match the frame" );
		}

		target->Release( );
		return;
	}

	IDirect3DSurface9* old_depth_stencil                = nullptr;
	IDirect3DPixelShader9* old_pixel_shader             = nullptr;
	IDirect3DVertexShader9* old_vertex_shader           = nullptr;
	IDirect3DBaseTexture9* old_texture[ 3 ]{ };
	IDirect3DVertexDeclaration9* old_vertex_declaration = nullptr;
	D3DVIEWPORT9 old_viewport{ };

	DWORD z_enable, z_write, alpha_blend, alpha_test, cull, scissor, srgb_write, fvf;
	DWORD stencil, colour_write, clip_planes, fill_mode, point_size, source_blend, dest_blend, blend_operation;

	DWORD address_u[ 3 ], address_v[ 3 ], min_filter[ 3 ], mag_filter[ 3 ], mip_filter[ 3 ], srgb_texture[ 3 ];

	float old_constants[ 11 ][ 4 ]{ };

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
	device->GetRenderState( D3DRS_SRCBLEND, &source_blend );
	device->GetRenderState( D3DRS_DESTBLEND, &dest_blend );
	device->GetRenderState( D3DRS_BLENDOP, &blend_operation );
	device->GetRenderState( D3DRS_CULLMODE, &cull );
	device->GetRenderState( D3DRS_SCISSORTESTENABLE, &scissor );
	device->GetRenderState( D3DRS_SRGBWRITEENABLE, &srgb_write );
	device->GetRenderState( D3DRS_POINTSIZE, &point_size );

	for ( unsigned long stage = 0; stage < 3; ++stage ) {
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
	device->GetPixelShaderConstantF( 0, old_constants[ 0 ], 11 );

	n_stream_guard::state_t streams{ };
	streams.capture( device );

	g_depth_source.resolve( device, old_depth_stencil, frame.m_frame );

	struct vertex_t {
		float m_x, m_y, m_z, m_rhw, m_u, m_v;
	};

	const auto make_quad = []( const float width, const float height, vertex_t( &quad )[ 4 ] ) {
		quad[ 0 ] = { -0.5f, -0.5f, 0.f, 1.f, 0.f, 0.f };
		quad[ 1 ] = { width - 0.5f, -0.5f, 0.f, 1.f, 1.f, 0.f };
		quad[ 2 ] = { -0.5f, height - 0.5f, 0.f, 1.f, 0.f, 1.f };
		quad[ 3 ] = { width - 0.5f, height - 0.5f, 0.f, 1.f, 1.f, 1.f };
	};

	const float width  = static_cast< float >( this->m_width );
	const float height = static_cast< float >( this->m_height );

	vertex_t full_quad[ 4 ], occlusion_quad[ 4 ];

	make_quad( width, height, full_quad );
	make_quad( static_cast< float >( this->m_occlusion_width ), static_cast< float >( this->m_occlusion_height ), occlusion_quad );

	D3DVIEWPORT9 full_viewport{ 0, 0, static_cast< DWORD >( this->m_width ), static_cast< DWORD >( this->m_height ), 0.f, 1.f };
	D3DVIEWPORT9 occlusion_viewport{
		0, 0, static_cast< DWORD >( this->m_occlusion_width ), static_cast< DWORD >( this->m_occlusion_height ), 0.f, 1.f
	};

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
	device->SetRenderState( D3DRS_COLORWRITEENABLE, 0x0f );

	for ( unsigned long stage = 0; stage < 3; ++stage ) {
		device->SetSamplerState( stage, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP );
		device->SetSamplerState( stage, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP );
		device->SetSamplerState( stage, D3DSAMP_MIPFILTER, D3DTEXF_NONE );
		device->SetSamplerState( stage, D3DSAMP_SRGBTEXTURE, 0 );
		device->SetSamplerState( stage, D3DSAMP_MINFILTER, D3DTEXF_POINT );
		device->SetSamplerState( stage, D3DSAMP_MAGFILTER, D3DTEXF_POINT );
	}

	const float aspect        = height > 0.f ? width / height : 1.777778f;
	const float tan_half_x    = std::tan( std::clamp( frame.m_fov, 1.f, 170.f ) * 0.5f * 3.14159265f / 180.f );
	const float tan_half_y    = tan_half_x / aspect;

	const float constant_0[ 4 ] = { frame.m_znear, frame.m_zfar, tan_half_x, tan_half_y };

	device->SetPixelShaderConstantF( 0, constant_0, 1 );

	const float fade_start = GET_VARIABLE( g_variables.m_ambient_occlusion_fade_start, float );
	const float fade_end   = std::max( GET_VARIABLE( g_variables.m_ambient_occlusion_fade_end, float ), fade_start + 1.f );

	{
		const float constant_1[ 4 ] = { 1.f / width, 1.f / height, 0.f, 0.f };

		device->SetPixelShaderConstantF( 1, constant_1, 1 );
		device->SetRenderTarget( 0, this->m_normal_surface );
		device->SetViewport( &full_viewport );
		device->SetTexture( 0, depth );
		device->SetPixelShader( prepare );
		n_fx_compat::draw_up( device, D3DPT_TRIANGLESTRIP, 2, full_quad, sizeof( vertex_t ) );
	}

	{
		const float radius  = std::max( GET_VARIABLE( g_variables.m_ambient_occlusion_radius, float ), 1.f );
		const float samples = static_cast< float >( std::clamp( GET_VARIABLE( g_variables.m_ambient_occlusion_samples, int ), 4, 64 ) );
		const float bias    = std::clamp( GET_VARIABLE( g_variables.m_ambient_occlusion_bias, float ), 0.f, 0.8f );

		const float constant_1[ 4 ] = { aspect, samples, radius, -1.f / ( radius * radius ) };

		const float constant_2[ 4 ] = { bias, GET_VARIABLE( g_variables.m_ambient_occlusion_viewmodel, bool ) ? 0.1f : 0.f, 0.1f, 0.0015f };

		const float constant_3[ 4 ] = { static_cast< float >( this->m_occlusion_width ), static_cast< float >( this->m_occlusion_height ), fade_end,
			                            0.f };

		const float block[ 3 ][ 4 ] = { { constant_1[ 0 ], constant_1[ 1 ], constant_1[ 2 ], constant_1[ 3 ] },
			                            { constant_2[ 0 ], constant_2[ 1 ], constant_2[ 2 ], constant_2[ 3 ] },
			                            { constant_3[ 0 ], constant_3[ 1 ], constant_3[ 2 ], constant_3[ 3 ] } };

		device->SetPixelShaderConstantF( 1, block[ 0 ], 3 );
		device->SetRenderTarget( 0, this->m_occlusion_surface );
		device->SetViewport( &occlusion_viewport );
		device->SetTexture( 0, this->m_normal_texture );
		device->SetTexture( 1, depth );
		device->SetPixelShader( occlusion );
		n_fx_compat::draw_up( device, D3DPT_TRIANGLESTRIP, 2, occlusion_quad, sizeof( vertex_t ) );
	}

	{
		const bool debug = GET_VARIABLE( g_variables.m_ambient_occlusion_debug, bool );

		const float constant_0_composite[ 4 ] = { frame.m_znear, frame.m_zfar, 1.f / static_cast< float >( this->m_occlusion_width ),
			                                      1.f / static_cast< float >( this->m_occlusion_height ) };

		const float constant_1[ 4 ] = { std::max( GET_VARIABLE( g_variables.m_ambient_occlusion_blur, float ), 0.f ), fade_end, tan_half_x,
			                            tan_half_y };

		const float constant_2[ 4 ] = { std::max( GET_VARIABLE( g_variables.m_ambient_occlusion_intensity, float ), 0.f ), fade_start,
			                            1.f / ( fade_end - fade_start ),
			                            GET_VARIABLE( g_variables.m_ambient_occlusion_viewmodel, bool ) ? 0.1f : 0.f };

		const float constant_3[ 4 ] = { frame.m_fog_start, frame.m_fog_density > 0.f ? 1.f / ( frame.m_fog_end - frame.m_fog_start ) : 0.f,
			                            frame.m_fog_density, 0.f };

		const float block[ 4 ][ 4 ] = {
			{ constant_0_composite[ 0 ], constant_0_composite[ 1 ], constant_0_composite[ 2 ], constant_0_composite[ 3 ] },
			{ constant_1[ 0 ], constant_1[ 1 ], constant_1[ 2 ], constant_1[ 3 ] },
			{ constant_2[ 0 ], constant_2[ 1 ], constant_2[ 2 ], constant_2[ 3 ] },
			{ constant_3[ 0 ], constant_3[ 1 ], constant_3[ 2 ], constant_3[ 3 ] }
		};

		device->SetPixelShaderConstantF( 0, block[ 0 ], 4 );

		float smoke_block[ 7 ][ 4 ]{ };

		smoke_block[ 0 ][ 0 ] = frame.m_smoke_count > 0 ? 1.f : 0.f;
		smoke_block[ 0 ][ 1 ] = 1.f / ( 0.7f * 166.f );
		smoke_block[ 0 ][ 2 ] = frame.m_smoke_inside ? 1.f : 0.f;

		for ( int i = 0; i < frame.m_smoke_count && i < 6; ++i )
			std::memcpy( smoke_block[ i + 1 ], frame.m_smoke[ i ], sizeof( float ) * 4 );

		device->SetPixelShaderConstantF( 4, smoke_block[ 0 ], 7 );

		device->SetRenderTarget( 0, target );
		device->SetViewport( &full_viewport );
		device->SetTexture( 0, this->m_occlusion_texture );
		device->SetTexture( 1, depth );
		device->SetTexture( 2, this->m_normal_texture );

		device->SetSamplerState( 0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR );
		device->SetSamplerState( 0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR );

		if ( !debug ) {
			device->SetRenderState( D3DRS_ALPHABLENDENABLE, TRUE );
			device->SetRenderState( D3DRS_BLENDOP, D3DBLENDOP_ADD );
			device->SetRenderState( D3DRS_SRCBLEND, D3DBLEND_ZERO );
			device->SetRenderState( D3DRS_DESTBLEND, D3DBLEND_SRCCOLOR );
		}

		device->SetRenderState( D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE );
		device->SetPixelShader( composite );
		n_fx_compat::draw_up( device, D3DPT_TRIANGLESTRIP, 2, full_quad, sizeof( vertex_t ) );
	}

	device->SetPixelShaderConstantF( 0, old_constants[ 0 ], 11 );
	device->SetPixelShader( old_pixel_shader );
	device->SetVertexShader( old_vertex_shader );

	if ( fvf )
		device->SetFVF( fvf );

	device->SetVertexDeclaration( old_vertex_declaration );

	streams.restore( device );

	for ( unsigned long stage = 0; stage < 3; ++stage ) {
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
	device->SetRenderState( D3DRS_BLENDOP, blend_operation );
	device->SetRenderState( D3DRS_DESTBLEND, dest_blend );
	device->SetRenderState( D3DRS_SRCBLEND, source_blend );
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
