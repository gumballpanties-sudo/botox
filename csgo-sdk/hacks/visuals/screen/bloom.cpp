#include "bloom.h"
#include "../../../game/sdk/includes/includes.h"
#include "../../../globals/includes/includes.h"
#include "fx_compat.h"
#include "frame_copy.h"
#include "gpu_timer.h"
#include "render_queue.h"
#include "stream_guard.h"

#include <algorithm>
#include <cmath>
#include <d3dx9.h>
#include <string>

static constexpr D3DFORMAT k_bloom_format = D3DFMT_A16B16G16R16F;

static const char k_downsample_body[] =
	"float4 box(sampler2D t,float2 texel,float2 uv){"
	"float2 o=texel*2.0;"
	"float4 c=tex2D(t,uv);"
	"float4 k=tex2Dlod(t,float4(uv.x+o.x,uv.y+o.y,0,0));"
	"k+=tex2Dlod(t,float4(uv.x-o.x,uv.y+o.y,0,0));"
	"k+=tex2Dlod(t,float4(uv.x-o.x,uv.y-o.y,0,0));"
	"k+=tex2Dlod(t,float4(uv.x+o.x,uv.y-o.y,0,0));"
	"return c/5.0+k/5.0;}";

static const char k_upsample_body[] =
	"float3 tent(sampler2D t,float2 texel,float2 uv){"
	"float2 o=texel*1.5;"
	"float3 c=tex2D(t,uv).rgb;"
	"float3 k=tex2Dlod(t,float4(uv.x-o.x,uv.y-o.y,0,0)).rgb;"
	"k+=tex2Dlod(t,float4(uv.x+o.x,uv.y-o.y,0,0)).rgb;"
	"k+=tex2Dlod(t,float4(uv.x+o.x,uv.y+o.y,0,0)).rgb;"
	"k+=tex2Dlod(t,float4(uv.x-o.x,uv.y+o.y,0,0)).rgb;"
	"return c/5.0+k/5.0;}";

/* c0.zw = per axis tap scale ( aspect ), 1 = round, only shrinks. c0.xy = frame texel, c1 = curve / saturation / intensity^3.
   dividing by brightness makes the curve pick how much glows, never the colour */
static const char k_prepass_shader[] =
	"sampler2D s0:register(s0);"
	"float4 c0:register(c0);"
	"float4 c1:register(c1);";

static const char k_prepass_main[] =
	"float4 main(float2 uv:TEXCOORD0):COLOR0{"
	"float4 d=box(s0,c0.xy*c0.zw,uv);"
	"float w=saturate(dot(d.rgb,float3(0.333,0.333,0.333)));"
	"float3 rgb=lerp(float3(w,w,w),d.rgb,c1.y);"
	"rgb*=(pow(w,c1.x)*c1.z)/(w+1e-3);"
	"return float4(rgb,w);}";

static const char k_downsample_shader[] =
	"sampler2D s0:register(s0);"
	"sampler2D s1:register(s1);"
	"float4 c0:register(c0);"
	"float4 c1:register(c1);";

static const char k_downsample_main[] =
	"float4 main(float2 uv:TEXCOORD0):COLOR0{"
	"float4 d=box(s0,c0.xy*c0.zw,uv);"
	"d.w=lerp(tex2D(s1,float2(0.5,0.5)).x,d.w,c1.x);"
	"return d;}";

static const char k_adapt_shader[] =
	"sampler2D s0:register(s0);"
	"float4 main(float2 uv:TEXCOORD0):COLOR0{"
	"float a=0.0;"
	"[unroll]for(int y=0;y<4;y++){"
	"[unroll]for(int x=0;x<4;x++){"
	"a+=tex2Dlod(s0,float4((x+0.5)/4.0,(y+0.5)/4.0,0,0)).w;}}"
	"a/=16.0;"
	"return float4(a,a,a,a);}";

static const char k_upsample_shader[] =
	"sampler2D s0:register(s0);"
	"float4 c0:register(c0);"
	"float4 c1:register(c1);"
	"float4 main(float2 uv:TEXCOORD0):COLOR0{"
	"return float4(tent(s0,c0.xy*c0.zw,uv)*c1.x,c1.y);}";

static const char k_combine_shader[] =
	"sampler2D s0:register(s0);"
	"sampler2D s1:register(s1);"
	"sampler2D s2:register(s2);"
	"float4 c0:register(c0);"
	"float4 c1:register(c1);"
	"float4 main(float2 uv:TEXCOORD0):COLOR0{"
	"float3 bloom=tent(s0,c0.xy*c0.zw,uv)*c1.x;"
	"float3 t=min(pow(saturate(tex2D(s1,uv).rgb),c1.w),0.999);"
	"float3 col=pow(t/(1.0-t),1.0/c1.w);"
	"float adapt=(tex2D(s2,float2(0.5,0.5)).x+1e-3)*8.0;"
	"col+=bloom*lerp(1.0,1.0/adapt,c1.y)*c1.z;"
	"col=pow(max(0,col),c1.w);"
	"col=col/(1.0+col);"
	"col=pow(col,1.0/c1.w);"
	"return float4(col,1);}";

IDirect3DPixelShader9* n_bloom::impl_t::ensure_shader( IDirect3DDevice9* device, const int shader )
{
	if ( this->m_shaders[ shader ] )
		return this->m_shaders[ shader ];

	if ( this->m_shader_failed[ shader ] || this->m_compile_failed )
		return nullptr;

	std::string source;

	switch ( shader ) {
		case shader_prepass: source = std::string( k_prepass_shader ) + k_downsample_body + k_prepass_main; break;
		case shader_downsample: source = std::string( k_downsample_shader ) + k_downsample_body + k_downsample_main; break;
		case shader_adapt: source = k_adapt_shader; break;
		case shader_upsample: source = std::string( k_upsample_body ) + k_upsample_shader; break;
		case shader_combine: source = std::string( k_upsample_body ) + k_combine_shader; break;
		default: return nullptr;
	}

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

		this->m_shader_failed[ shader ] = true;
		return nullptr;
	}

	if ( errors )
		errors->Release( );

	result = device->CreatePixelShader( static_cast< const DWORD* >( code->GetBufferPointer( ) ), &this->m_shaders[ shader ] );
	code->Release( );

	if ( FAILED( result ) || !this->m_shaders[ shader ] ) {
		this->m_shaders[ shader ] = nullptr;
		this->m_compile_failed = true;
		return nullptr;
	}

	g_console.print( std::vformat( "bloom: shader {:d} built", std::make_format_args( shader ) ).c_str( ) );

	return this->m_shaders[ shader ];
}

bool n_bloom::impl_t::check_format( IDirect3DDevice9* device )
{
	IDirect3D9* d3d = nullptr;

	if ( FAILED( device->GetDirect3D( &d3d ) ) || !d3d )
		return false;

	D3DDEVICE_CREATION_PARAMETERS parameters{ };
	D3DDISPLAYMODE mode{ };

	if ( FAILED( device->GetCreationParameters( &parameters ) ) || FAILED( device->GetDisplayMode( 0, &mode ) ) ) {
		d3d->Release( );
		return false;
	}

	const bool blending = SUCCEEDED( d3d->CheckDeviceFormat( parameters.AdapterOrdinal, parameters.DeviceType, mode.Format,
	                                                         D3DUSAGE_RENDERTARGET | D3DUSAGE_QUERY_POSTPIXELSHADER_BLENDING, D3DRTYPE_TEXTURE,
	                                                         k_bloom_format ) );

	const bool filtering = SUCCEEDED( d3d->CheckDeviceFormat( parameters.AdapterOrdinal, parameters.DeviceType, mode.Format,
	                                                          D3DUSAGE_RENDERTARGET | D3DUSAGE_QUERY_FILTER, D3DRTYPE_TEXTURE, k_bloom_format ) );

	d3d->Release( );

	if ( !blending || !filtering ) {
		g_console.print< n_console::log_level::WARNING >( "bloom: this card cannot blend or filter fp16 render targets — the pass is off" );
		return false;
	}

	return true;
}

void n_bloom::impl_t::release_targets( )
{
	if ( this->m_adapt_surface ) {
		this->m_adapt_surface->Release( );
		this->m_adapt_surface = nullptr;
	}

	if ( this->m_adapt_texture ) {
		this->m_adapt_texture->Release( );
		this->m_adapt_texture = nullptr;
	}

	for ( int level = 0; level < k_levels; ++level ) {
		if ( this->m_surfaces[ level ] ) {
			this->m_surfaces[ level ]->Release( );
			this->m_surfaces[ level ] = nullptr;
		}

		if ( this->m_textures[ level ] ) {
			this->m_textures[ level ]->Release( );
			this->m_textures[ level ] = nullptr;
		}

		this->m_sizes[ level ][ 0 ] = 0;
		this->m_sizes[ level ][ 1 ] = 0;
	}

	if ( this->m_resolve_surface ) {
		this->m_resolve_surface->Release( );
		this->m_resolve_surface = nullptr;
	}

	if ( this->m_frame_surface ) {
		this->m_frame_surface->Release( );
		this->m_frame_surface = nullptr;
	}

	if ( this->m_frame_texture ) {
		this->m_frame_texture->Release( );
		this->m_frame_texture = nullptr;
	}

	this->m_width        = 0;
	this->m_height       = 0;
	this->m_format       = D3DFMT_UNKNOWN;
	this->m_multi_sample = D3DMULTISAMPLE_NONE;
}

void n_bloom::impl_t::release( )
{
	this->release_targets( );

	for ( IDirect3DPixelShader9*& shader : this->m_shaders ) {
		if ( shader ) {
			shader->Release( );
			shader = nullptr;
		}
	}
}

void n_bloom::impl_t::on_device_lost( )
{
	this->release( );
	this->m_active = false;
}

bool n_bloom::impl_t::build( IDirect3DDevice9* device, const D3DSURFACE_DESC& description )
{
	const int width  = static_cast< int >( description.Width );
	const int height = static_cast< int >( description.Height );

	if ( this->m_frame_texture && this->m_width == width && this->m_height == height && this->m_format == description.Format &&
	     this->m_multi_sample == description.MultiSampleType )
		return true;

	this->release_targets( );

	if ( FAILED( device->CreateTexture( description.Width, description.Height, 1, D3DUSAGE_RENDERTARGET, description.Format, D3DPOOL_DEFAULT,
	                                    &this->m_frame_texture, nullptr ) ) ||
	     FAILED( this->m_frame_texture->GetSurfaceLevel( 0, &this->m_frame_surface ) ) ) {
		g_console.print< n_console::log_level::WARNING >( "bloom: frame copy creation failed" );
		this->release_targets( );
		return false;
	}

	if ( description.MultiSampleType != D3DMULTISAMPLE_NONE &&
	     FAILED( device->CreateRenderTarget( description.Width, description.Height, description.Format, D3DMULTISAMPLE_NONE, 0, FALSE,
	                                         &this->m_resolve_surface, nullptr ) ) ) {
		g_console.print< n_console::log_level::WARNING >( "bloom: msaa resolve target creation failed" );
		this->release_targets( );
		return false;
	}

	for ( int level = 0; level < k_levels; ++level ) {
		const int shift = level < 1 ? 1 : level;

		const int level_width  = std::max( width >> shift, 1 );
		const int level_height = std::max( height >> shift, 1 );

		if ( FAILED( device->CreateTexture( static_cast< UINT >( level_width ), static_cast< UINT >( level_height ), 1, D3DUSAGE_RENDERTARGET,
		                                    k_bloom_format, D3DPOOL_DEFAULT, &this->m_textures[ level ], nullptr ) ) ||
		     FAILED( this->m_textures[ level ]->GetSurfaceLevel( 0, &this->m_surfaces[ level ] ) ) ) {
			g_console.print< n_console::log_level::WARNING >( "bloom: pyramid level creation failed" );
			this->release_targets( );
			return false;
		}

		this->m_sizes[ level ][ 0 ] = level_width;
		this->m_sizes[ level ][ 1 ] = level_height;
	}

	if ( FAILED( device->CreateTexture( 1, 1, 1, D3DUSAGE_RENDERTARGET, k_bloom_format, D3DPOOL_DEFAULT, &this->m_adapt_texture, nullptr ) ) ||
	     FAILED( this->m_adapt_texture->GetSurfaceLevel( 0, &this->m_adapt_surface ) ) ) {
		g_console.print< n_console::log_level::WARNING >( "bloom: adaptation target creation failed" );
		this->release_targets( );
		return false;
	}

	IDirect3DSurface9* old_target = nullptr;

	if ( SUCCEEDED( device->GetRenderTarget( 0, &old_target ) ) ) {
		D3DVIEWPORT9 old_viewport{ };
		device->GetViewport( &old_viewport );

		IDirect3DSurface9* old_depth_stencil = nullptr;

		if ( FAILED( device->GetDepthStencilSurface( &old_depth_stencil ) ) )
			old_depth_stencil = nullptr;

		device->SetDepthStencilSurface( nullptr );

		if ( SUCCEEDED( device->SetRenderTarget( 0, this->m_adapt_surface ) ) )
			device->Clear( 0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_ARGB( 255, 128, 128, 128 ), 1.f, 0 );

		device->SetRenderTarget( 0, old_target );
		device->SetDepthStencilSurface( old_depth_stencil );
		device->SetViewport( &old_viewport );

		if ( old_depth_stencil )
			old_depth_stencil->Release( );

		old_target->Release( );
	}

	this->m_width        = width;
	this->m_height       = height;
	this->m_format       = description.Format;
	this->m_multi_sample = description.MultiSampleType;

	const int samples = static_cast< int >( description.MultiSampleType );

	g_console.print( std::vformat( "bloom: built {:d}x{:d} msaa {:d}", std::make_format_args( width, height, samples ) ).c_str( ) );

	return true;
}

void n_bloom::impl_t::on_post_screen_space_effects( c_view_setup* setup )
{
	if ( !setup )
		return;

	const bool enabled = GET_VARIABLE( g_variables.m_bloom, bool ) && !this->m_compile_failed && !this->m_format_failed;

	if ( !enabled && !this->m_active )
		return;

	frame_t frame{ };

	frame.m_frame_time = g_interfaces.m_global_vars_base ? g_interfaces.m_global_vars_base->m_frame_time : 0.f;

	const auto pass = [ this, frame ] { n_fx_compat::run( g_interfaces.m_direct_device, "bloom", [ & ] { this->execute( frame ); } ); };

	if ( n_render_queue::submit( pass ) )
		return;

	pass( );
}

void n_bloom::impl_t::execute( const frame_t frame )
{
	IDirect3DDevice9* device = g_interfaces.m_direct_device;

	if ( !device )
		return;

	const n_gpu_timer::scope_t gpu_timer( device, n_gpu_timer::pass_bloom );

	if ( !GET_VARIABLE( g_variables.m_bloom, bool ) || this->m_compile_failed || this->m_format_failed ) {
		if ( this->m_frame_texture )
			this->release_targets( );

		this->m_active = false;
		return;
	}

	static bool format_checked = false;

	if ( !format_checked ) {
		format_checked = true;

		if ( !this->check_format( device ) ) {
			this->m_format_failed = true;
			return;
		}
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

	IDirect3DPixelShader9* prepass    = this->ensure_shader( device, shader_prepass );
	IDirect3DPixelShader9* downsample = this->ensure_shader( device, shader_downsample );
	IDirect3DPixelShader9* adapt      = this->ensure_shader( device, shader_adapt );
	IDirect3DPixelShader9* upsample   = this->ensure_shader( device, shader_upsample );
	IDirect3DPixelShader9* combine    = this->ensure_shader( device, shader_combine );

	if ( !prepass || !downsample || !adapt || !upsample || !combine ) {
		target->Release( );
		return;
	}

	static bool logged_blit = false;

	const long blit_result = n_frame_copy::copy( device, target, this->m_resolve_surface, this->m_frame_surface );

	if ( FAILED( blit_result ) ) {
		if ( !logged_blit ) {
			logged_blit = true;

			const unsigned long code = static_cast< unsigned long >( blit_result );

			g_console.print< n_console::log_level::WARNING >( std::vformat( "bloom: frame copy failed {:#x}", std::make_format_args( code ) ).c_str( ) );
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
	DWORD stencil, colour_write, clip_planes, fill_mode, source_blend, dest_blend, blend_operation;

	DWORD address_u[ 3 ], address_v[ 3 ], min_filter[ 3 ], mag_filter[ 3 ], mip_filter[ 3 ], srgb_texture[ 3 ];

	float old_constants[ 3 ][ 4 ]{ };

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
	device->GetPixelShaderConstantF( 0, old_constants[ 0 ], 3 );

	n_stream_guard::state_t streams{ };
	streams.capture( device );

	struct vertex_t {
		float m_x, m_y, m_z, m_rhw, m_u, m_v;
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
		device->SetSamplerState( stage, D3DSAMP_MINFILTER, D3DTEXF_LINEAR );
		device->SetSamplerState( stage, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR );
	}

	/* aspect: + = wide ( y taps shrink ), - = tall ( x taps shrink ). shrinking one axis, never growing the other, so no tap
	   lands 2+ texels past its neighbour = no shimmer. ponytail: ~3x stretch max at 1, anisotropic pyramid sizes if more is wanted */
	const float aspect  = std::clamp( GET_VARIABLE( g_variables.m_bloom_aspect, float ), -1.f, 1.f );
	const float scale_x = 1.f - std::max( -aspect, 0.f );
	const float scale_y = 1.f - std::max( aspect, 0.f );

	const auto draw_level = [ & ]( IDirect3DSurface9* destination, const int destination_width, const int destination_height, const int source_width,
	                               const int source_height ) {
		const float width  = static_cast< float >( destination_width );
		const float height = static_cast< float >( destination_height );

		const vertex_t quad[ 4 ] = {
			{ -0.5f, -0.5f, 0.f, 1.f, 0.f, 0.f },
			{ width - 0.5f, -0.5f, 0.f, 1.f, 1.f, 0.f },
			{ -0.5f, height - 0.5f, 0.f, 1.f, 0.f, 1.f },
			{ width - 0.5f, height - 0.5f, 0.f, 1.f, 1.f, 1.f },
		};

		const D3DVIEWPORT9 viewport{ 0, 0, static_cast< DWORD >( destination_width ), static_cast< DWORD >( destination_height ), 0.f, 1.f };

		const float texel[ 4 ] = { source_width > 0 ? 1.f / static_cast< float >( source_width ) : 0.f,
			                       source_height > 0 ? 1.f / static_cast< float >( source_height ) : 0.f, scale_x, scale_y };

		device->SetPixelShaderConstantF( 0, texel, 1 );
		device->SetRenderTarget( 0, destination );
		device->SetViewport( &viewport );
		n_fx_compat::draw_up( device, D3DPT_TRIANGLESTRIP, 2, quad, sizeof( vertex_t ) );
	};

	float layers[ k_levels ]{ };
	{
		const float centre = 1.f + std::clamp( GET_VARIABLE( g_variables.m_bloom_radius, float ), 0.f, 1.f ) * 6.f;

		for ( int level = 1; level < k_levels; ++level ) {
			const float distance = ( static_cast< float >( level ) - centre ) / 0.6f;
			layers[ level ]      = std::exp( -0.5f * distance * distance ) + 0.05f;
		}
	}

	constexpr float k_adapt_strength = 0.5f;
	constexpr float k_adapt_speed    = 2.f;
	constexpr float k_tonemap        = 4.f;

	{
		const float intensity = std::max( GET_VARIABLE( g_variables.m_bloom_intensity, float ), 0.f );

		const float constant_1[ 4 ] = { GET_VARIABLE( g_variables.m_bloom_curve, float ), GET_VARIABLE( g_variables.m_bloom_saturation, float ),
			                            intensity * intensity * intensity, 0.f };

		device->SetPixelShaderConstantF( 1, constant_1, 1 );
		device->SetPixelShader( prepass );
		device->SetTexture( 0, this->m_frame_texture );

		draw_level( this->m_surfaces[ 0 ], this->m_sizes[ 0 ][ 0 ], this->m_sizes[ 0 ][ 1 ], this->m_width, this->m_height );
	}

	{
		const float blend = std::clamp( frame.m_frame_time * k_adapt_speed, 0.f, 1.f );

		device->SetPixelShader( downsample );
		device->SetTexture( 1, this->m_adapt_texture );

		for ( int level = 1; level < k_levels; ++level ) {
			const float constant_1[ 4 ] = { level == k_levels - 1 ? blend : 1.f, 0.f, 0.f, 0.f };

			device->SetPixelShaderConstantF( 1, constant_1, 1 );
			device->SetTexture( 0, this->m_textures[ level - 1 ] );

			draw_level( this->m_surfaces[ level ], this->m_sizes[ level ][ 0 ], this->m_sizes[ level ][ 1 ], this->m_sizes[ level - 1 ][ 0 ],
			            this->m_sizes[ level - 1 ][ 1 ] );
		}
	}

	{
		device->SetPixelShader( adapt );
		device->SetTexture( 0, this->m_textures[ k_levels - 1 ] );
		device->SetTexture( 1, nullptr );

		draw_level( this->m_adapt_surface, 1, 1, this->m_sizes[ k_levels - 1 ][ 0 ], this->m_sizes[ k_levels - 1 ][ 1 ] );
	}

	{
		device->SetRenderState( D3DRS_ALPHABLENDENABLE, TRUE );
		device->SetRenderState( D3DRS_BLENDOP, D3DBLENDOP_ADD );
		device->SetRenderState( D3DRS_SRCBLEND, D3DBLEND_ONE );
		device->SetRenderState( D3DRS_DESTBLEND, D3DBLEND_SRCALPHA );
		device->SetPixelShader( upsample );

		for ( int level = k_levels - 1; level > 1; --level ) {
			const float constant_1[ 4 ] = { level == k_levels - 1 ? layers[ level ] : 1.f, layers[ level - 1 ], 0.f, 0.f };

			device->SetPixelShaderConstantF( 1, constant_1, 1 );
			device->SetTexture( 0, this->m_textures[ level ] );

			draw_level( this->m_surfaces[ level - 1 ], this->m_sizes[ level - 1 ][ 0 ], this->m_sizes[ level - 1 ][ 1 ], this->m_sizes[ level ][ 0 ],
			            this->m_sizes[ level ][ 1 ] );
		}

		device->SetRenderState( D3DRS_ALPHABLENDENABLE, FALSE );
	}

	{
		float sum = 0.f;

		for ( int level = 1; level < k_levels; ++level )
			sum += layers[ level ];

		const float constant_1[ 4 ] = { 1.f / sum, k_adapt_strength, 1.f, k_tonemap };

		device->SetPixelShaderConstantF( 1, constant_1, 1 );
		device->SetPixelShader( combine );
		device->SetTexture( 0, this->m_textures[ 1 ] );
		device->SetTexture( 1, this->m_frame_texture );
		device->SetTexture( 2, this->m_adapt_texture );

		device->SetRenderState( D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE );

		draw_level( target, this->m_width, this->m_height, this->m_sizes[ 1 ][ 0 ], this->m_sizes[ 1 ][ 1 ] );
	}

	static bool logged_draw = false;

	if ( !logged_draw ) {
		logged_draw = true;
		g_console.print( "bloom: first pass drawn" );
	}

	device->SetPixelShaderConstantF( 0, old_constants[ 0 ], 3 );
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
	device->SetRenderTarget( 0, target );
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
