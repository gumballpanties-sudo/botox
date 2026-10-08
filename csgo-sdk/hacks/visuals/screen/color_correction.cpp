#include "color_correction.h"
#include "../../../game/sdk/includes/includes.h"
#include "../../../globals/includes/includes.h"
#include "fx_compat.h"
#include "frame_copy.h"
#include "gpu_timer.h"
#include "stream_guard.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <d3dx9.h>
#include <string>
#include <vector>

static const char k_shader[] =
	"sampler2D s0:register(s0);"
	"sampler2D s1:register(s1);"
	"float4 c0:register(c0);"
	"float4 c1:register(c1);"
	"float4 c2:register(c2);"
	"float4 c3:register(c3);"
	"float4 c4:register(c4);"
	"float4 c5:register(c5);"
	"float4 c6:register(c6);"
	"float4 c7:register(c7);"
	"float4 c8:register(c8);"
	"float4 c9:register(c9);"
	"float4 c10:register(c10);"
	"float4 c11:register(c11);"
	"float4 c12:register(c12);"
	"float4 c13:register(c13);"
	"float4 c14:register(c14);"
	"float4 c15:register(c15);"
	"float4 c16:register(c16);"
	"float4 c17:register(c17);"
	"float4 c18:register(c18);\n"
	/* src = shifted read for sharpen / blur taps. deband taps read raw: where shift shows ( edges ) diff > threshold keeps c,
	   where deband acts ( flat areas ) shift barely changes the avg, so 2 fewer fetches per tap */
	"float3 raw(float2 t){return tex2Dlod(s0,float4(t,0,0)).rgb;}"
	"float3 src(float2 t){\n"
	"#if SHIFT\n"
	"float4 q=t.xyxy+c10;"
	"return float3(tex2Dlod(s0,float4(q.xy,0,0)).r,tex2Dlod(s0,float4(q.zw,0,0)).g,tex2Dlod(s0,float4(t+c11.xy,0,0)).b);\n"
	"#else\n"
	"return raw(t);\n"
	"#endif\n"
	"}\n"
	"#if GRAIN\n"
	"float4 rnm(float2 tc){"
	"float n=sin(dot(tc,float2(12.9898,78.233)))*43758.5453;"
	"return float4(frac(n),frac(n*1.2154),frac(n*1.3453),frac(n*1.3647))*2.0-1.0;}"
	"float2 grad(float perm,float z){return rnm(float2(perm,z)).rg*4.0-1.0;}"
	"float pnoise(float2 p,float z){"
	"float2 pi=0.00390625*floor(p)+0.001953125;"
	"float2 pf=frac(p);"
	"float n00=dot(grad(rnm(pi).a,z),pf);"
	"float n01=dot(grad(rnm(pi+float2(0.0,0.00390625)).a,z),pf-float2(0.0,1.0));"
	"float n10=dot(grad(rnm(pi+float2(0.00390625,0.0)).a,z),pf-float2(1.0,0.0));"
	"float n11=dot(grad(rnm(pi+0.00390625).a,z),pf-1.0);"
	"float2 f=pf*pf*pf*(pf*(pf*6.0-15.0)+10.0);"
	"return lerp(lerp(n00,n01,f.y),lerp(n10,n11,f.y),f.x);}\n"
	"#endif\n"
	"float4 main(float2 uv:TEXCOORD0):COLOR0{"
	"float3 c=src(uv);\n"
	/* deband = edge keeping smooth, BrutPitt smartDeNoise bilateral: vogel disk taps, weight = gaussian( dist, sigma radius / 2 )
	   * gaussian( color diff, sigma threshold ), so flat / noisy areas average and edges ( diff >> threshold ) keep. exp2 with
	   log2e folded in c16.x, then grain */
	"#if DEBAND\n"
	"float dn=frac(52.9829189*frac(dot(uv/c13.xy+c16.w,float2(0.06711056,0.00583715))));"
	"float2 dd;sincos(6.2831853*dn,dd.y,dd.x);"
	"float3 da=c;float dw=1.0;"
	"[loop]for(float di=0.5;di<64.0;di+=1.0){"
	"if(di>c17.x)break;"
	"float dt=di/c17.x;"
	"float3 ds=raw(uv+dd*sqrt(dt)*c16.y*c13.xy);"
	"float3 dc=ds-c;"
	"float w=exp2(-2.8853901*dt-dot(dc,dc)*c16.x);"
	"da+=ds*w;dw+=w;"
	"dd=float2(dd.x*-0.73736888-dd.y*0.67549029,dd.x*0.67549029+dd.y*-0.73736888);}"
	"c=da/dw+c16.z*(frac(dn+float3(0.0,0.381966,0.618034))-0.5);\n"
	"#endif\n"
	"#if VIGNETTE\n"
	"float2 vt=uv-0.5;"
	"vt.x*=c7.z;"
	"float vm=smoothstep(c7.x,c7.y,length(vt));"
	"vm=lerp(vm,1.0-vm,c7.w);"
	/* blur: 24 tap vogel disk, radius = mask * blur px, lod taps ( gradients undefined in a branch ), under half a px skipped.
	   [loop] not unroll: unrolled taps push the all-stages variant past ps_3_0's 512 slots */
	"if(vm*c9.x>0.5){"
	"float2 vp=uv/c13.xy;"
	"float2 vd;"
	"sincos(6.2831853*frac(52.9829189*frac(dot(vp,float2(0.06711056,0.00583715)))),vd.y,vd.x);"
	"float2 vs=vm*c9.x*c13.xy;"
	"float3 vb=c;"
	"[loop]for(float i=0.5;i<24.0;i+=1.0){"
	"vb+=src(uv+vd*sqrt(i/24.0)*vs);"
	"vd=float2(vd.x*-0.73736888-vd.y*0.67549029,vd.x*0.67549029+vd.y*-0.73736888);}"
	"c=vb/25.0;}\n"
	"#endif\n"
	"#if SHARPEN\n"
	"float3 b=src(uv+float2(c13.x,-c13.y)*0.5);"
	"b+=src(uv-c13.xy*0.5);"
	"b+=src(uv+c13.xy*0.5);"
	"b+=src(uv-float2(c13.x,-c13.y)*0.5);"
	"float d=dot(c-b*0.25,float3(0.2126,0.7152,0.0722));"
	"d=sign(d)*max(abs(d)-c13.w,0.0);\n"
	"#if VIGNETTE\n"
	"d*=1.0-vm*c9.y;\n"
	"#endif\n"
	"float hc=0.1*max(c13.z,1.0);"
	"c=saturate(c+clamp(d*c13.z,-hc,hc));\n"
	"#endif\n"
	"#if BASIC\n"
	"c*=c0.rgb*c1.z;\n"
	"#endif\n"
	"#if TEMPERATURE\n"
	"c.r=saturate(c.r+c1.y*0.15);"
	"c.g=saturate(c.g+c1.y*0.05);"
	"c.b=saturate(c.b-c1.y*0.15);\n"
	"#else\n"
	"c=saturate(c);\n"
	"#endif\n"
	"#if LEVELS\n"
	"float3 li=saturate((c-c5.x)/(c5.y-c5.x));"
	"c=pow(li,c5.z);\n"
	"#endif\n"
	"#if BASIC\n"
	"float luma=dot(c,float3(0.2126,0.7152,0.0722));"
	"c=luma+(c-luma)*c1.x;\n"
	"#endif\n"
	"#if HUE\n"
	"float3 h;"
	"h.r=dot(c,c2.xyz);"
	"h.g=dot(c,c3.xyz);"
	"h.b=dot(c,c4.xyz);"
	"c=saturate(h);\n"
	"#else\n"
	"c=saturate(c);\n"
	"#endif\n"
	"#if CURVES\n"
	"float3 cu=c*(255.0/256.0)+(0.5/256.0);"
	"c=float3(tex2Dlod(s1,float4(cu.r,0.5,0,0)).r,tex2Dlod(s1,float4(cu.g,0.5,0,0)).g,tex2Dlod(s1,float4(cu.b,0.5,0,0)).b);\n"
	"#endif\n"
	"#if BASIC\n"
	"c=saturate(c6.x+c*c6.y);\n"
	"#endif\n"
	"#if TONE\n"
	"float tl=dot(c,float3(0.2126,0.7152,0.0722));"
	"float hw=smoothstep(0.5,1.0,tl);"
	"float sw=1.0-smoothstep(0.0,0.5,tl);"
	"c=saturate(c+c6.z*hw+c6.w*sw);\n"
	"#endif\n"
	"#if BASIC\n"
	"c=saturate((c-0.5)*c0.w+0.5);\n"
	"#endif\n"
	/* posterize: n levels, optional ign dither of one step before rounding ( prod80 PD80_06 style, round not floor so white stays white ) */
	"#if POSTERIZE\n"
	"float pn=frac(52.9829189*frac(dot(uv/c13.xy,float2(0.06711056,0.00583715))))-0.5;"
	"float3 pq=floor(saturate(c)*c17.y+0.5+pn*c17.z)/c17.y;"
	"c=lerp(c,saturate(pq),c17.w);\n"
	"#endif\n"
	/* invert: rgb = 1 - c, luma = flip brightness keep hue ( c + 1 - 2 * luma ) */
	"#if INVERT\n"
	"float3 iv=c18.y>0.5?c+1.0-2.0*dot(c,float3(0.2126,0.7152,0.0722)):1.0-c;"
	"c=lerp(c,saturate(iv),c18.x);\n"
	"#endif\n"
	"#if VIGNETTE\n"
	"c=lerp(c,c8.rgb,vm*c8.a);\n"
	"#endif\n"
	"#if GRAIN\n"
	"float2 q=uv*2.0-1.0;"
	"q.x*=c15.z;"
	"float2 r=float2(q.x*c14.x-q.y*c14.y,q.y*c14.x+q.x*c14.y);"
	"r.x/=c15.z;"
	"float n=pnoise((r*0.5+0.5)*c15.xy,c14.z);"
	"float gl=saturate(abs(dot(c,float3(0.299,0.587,0.114))-c12.x)*c12.y);"
	"gl*=gl;"
	"c=saturate(c+n*lerp(1.0-gl*gl,1.0,c12.z)*c14.w);\n"
	"#endif\n"
	"return float4(c,1);"
	"}";

static void compute_hue_matrix( const float theta, float matrix[ 3 ][ 3 ] )
{
	const float cosine = std::cos( theta ), sine = std::sin( theta );
	const float lx = 0.213f, ly = 0.715f, lz = 0.072f;
	const float inverse = 1.f - cosine;

	matrix[ 0 ][ 0 ] = cosine + inverse * lx * lx;
	matrix[ 0 ][ 1 ] = inverse * lx * ly - sine * lz;
	matrix[ 0 ][ 2 ] = inverse * lx * lz + sine * ly;
	matrix[ 1 ][ 0 ] = inverse * ly * lx + sine * lz;
	matrix[ 1 ][ 1 ] = cosine + inverse * ly * ly;
	matrix[ 1 ][ 2 ] = inverse * ly * lz - sine * lx;
	matrix[ 2 ][ 0 ] = inverse * lz * lx - sine * ly;
	matrix[ 2 ][ 1 ] = inverse * lz * ly + sine * lx;
	matrix[ 2 ][ 2 ] = cosine + inverse * lz * lz;
}

static void load_curves( n_color_curve::curve_t ( &curves )[ 4 ] )
{
	curves[ 0 ] = n_color_curve::load( GET_VARIABLE( g_variables.m_color_correction_curve_points, std::vector< float > ) );
	curves[ 1 ] = n_color_curve::load( GET_VARIABLE( g_variables.m_color_correction_curve_red_points, std::vector< float > ) );
	curves[ 2 ] = n_color_curve::load( GET_VARIABLE( g_variables.m_color_correction_curve_green_points, std::vector< float > ) );
	curves[ 3 ] = n_color_curve::load( GET_VARIABLE( g_variables.m_color_correction_curve_blue_points, std::vector< float > ) );
}

bool n_color_correction::impl_t::update_lut( IDirect3DDevice9* device, const n_color_curve::curve_t ( &curves )[ 4 ] )
{
	if ( this->m_lut_valid && std::memcmp( curves, this->m_lut_key, sizeof( this->m_lut_key ) ) == 0 )
		return true;

	if ( !this->m_lut_texture &&
	     FAILED( device->CreateTexture( 256, 1, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &this->m_lut_texture, nullptr ) ) ) {
		this->m_lut_texture = nullptr;
		return false;
	}

	D3DLOCKED_RECT locked{ };
	if ( FAILED( this->m_lut_texture->LockRect( 0, &locked, nullptr, D3DLOCK_DISCARD ) ) )
		return this->m_lut_valid;

	auto* row = static_cast< unsigned char* >( locked.pBits );
	for ( int i = 0; i < 256; i++ ) {
		const float master = n_color_curve::evaluate( curves[ 0 ], i / 255.f );

		for ( int channel = 0; channel < 3; channel++ )
			row[ i * 4 + 2 - channel ] = static_cast< unsigned char >( n_color_curve::evaluate( curves[ 1 + channel ], master ) * 255.f + 0.5f );

		row[ i * 4 + 3 ] = 255;
	}

	this->m_lut_texture->UnlockRect( 0 );

	std::memcpy( this->m_lut_key, curves, sizeof( this->m_lut_key ) );
	this->m_lut_valid = true;
	return true;
}

unsigned int n_color_correction::impl_t::grade_flags( )
{
	unsigned int flags = 0;

	if ( GET_VARIABLE( g_variables.m_sharpen, bool ) && GET_VARIABLE( g_variables.m_sharpen_strength, float ) > 0.f )
		flags |= grade_sharpen;

	if ( GET_VARIABLE( g_variables.m_film_grain, bool ) && GET_VARIABLE( g_variables.m_film_grain_intensity, float ) > 0.f )
		flags |= grade_grain;

	if ( GET_VARIABLE( g_variables.m_vignette, bool ) &&
	     ( GET_VARIABLE( g_variables.m_vignette_color, c_color )[ 3 ] > 0 || GET_VARIABLE( g_variables.m_vignette_blur, float ) > 0.f ) )
		flags |= grade_vignette;

	if ( GET_VARIABLE( g_variables.m_invert_filter, bool ) && GET_VARIABLE( g_variables.m_invert_filter_strength, float ) > 0.f )
		flags |= grade_invert;

	if ( GET_VARIABLE( g_variables.m_posterize, bool ) && GET_VARIABLE( g_variables.m_posterize_strength, float ) > 0.f )
		flags |= grade_posterize;

	if ( GET_VARIABLE( g_variables.m_deband, bool ) )
		flags |= grade_deband;

	if ( GET_VARIABLE( g_variables.m_channel_shift, bool ) ) {
		const float offsets[ 6 ] = { GET_VARIABLE( g_variables.m_channel_shift_red_x, float ),   GET_VARIABLE( g_variables.m_channel_shift_red_y, float ),
			                         GET_VARIABLE( g_variables.m_channel_shift_green_x, float ), GET_VARIABLE( g_variables.m_channel_shift_green_y, float ),
			                         GET_VARIABLE( g_variables.m_channel_shift_blue_x, float ),  GET_VARIABLE( g_variables.m_channel_shift_blue_y, float ) };

		if ( std::any_of( std::begin( offsets ), std::end( offsets ), []( const float offset ) { return offset != 0.f; } ) )
			flags |= grade_shift;
	}

	if ( !GET_VARIABLE( g_variables.m_color_correction, bool ) )
		return flags;

	if ( GET_VARIABLE( g_variables.m_color_correction_temperature, float ) != 0.f )
		flags |= grade_temperature;

	if ( GET_VARIABLE( g_variables.m_color_correction_levels_in_black, float ) != 0.f ||
	     GET_VARIABLE( g_variables.m_color_correction_levels_in_white, float ) != 1.f ||
	     GET_VARIABLE( g_variables.m_color_correction_levels_gamma, float ) != 1.f )
		flags |= grade_levels;

	if ( GET_VARIABLE( g_variables.m_color_correction_hue, float ) != 0.f )
		flags |= grade_hue;

	n_color_curve::curve_t curves[ 4 ];
	load_curves( curves );

	for ( const n_color_curve::curve_t& curve : curves ) {
		if ( !n_color_curve::is_identity( curve ) )
			flags |= grade_curves;
	}

	if ( GET_VARIABLE( g_variables.m_color_correction_highlights, float ) != 0.f ||
	     GET_VARIABLE( g_variables.m_color_correction_shadows, float ) != 0.f )
		flags |= grade_tone;

	if ( GET_VARIABLE( g_variables.m_color_correction_red, float ) != 1.f || GET_VARIABLE( g_variables.m_color_correction_green, float ) != 1.f ||
	     GET_VARIABLE( g_variables.m_color_correction_blue, float ) != 1.f || GET_VARIABLE( g_variables.m_color_correction_exposure, float ) != 0.f ||
	     GET_VARIABLE( g_variables.m_color_correction_saturation, float ) != 1.f ||
	     GET_VARIABLE( g_variables.m_color_correction_contrast, float ) != 1.f ||
	     GET_VARIABLE( g_variables.m_color_correction_levels_out_black, float ) != 0.f ||
	     GET_VARIABLE( g_variables.m_color_correction_levels_out_white, float ) != 1.f )
		flags |= grade_basic;

	return flags;
}

IDirect3DPixelShader9* n_color_correction::impl_t::ensure_shader( IDirect3DDevice9* device, const unsigned int flags )
{
	const unsigned int index = flags & grade_shader_mask;

	if ( this->m_shaders[ index ] )
		return this->m_shaders[ index ];

	if ( this->m_shader_failed[ index ] || this->m_compile_failed )
		return nullptr;

	const int temperature = ( index & grade_temperature ) ? 1 : 0;
	const int levels      = ( index & grade_levels ) ? 1 : 0;
	const int hue         = ( index & grade_hue ) ? 1 : 0;
	const int curves      = ( index & grade_curves ) ? 1 : 0;
	const int tone        = ( index & grade_tone ) ? 1 : 0;
	const int basic       = ( index & grade_basic ) ? 1 : 0;
	const int sharpen     = ( index & grade_sharpen ) ? 1 : 0;
	const int grain       = ( index & grade_grain ) ? 1 : 0;
	const int vignette    = ( index & grade_vignette ) ? 1 : 0;
	const int invert      = ( index & grade_invert ) ? 1 : 0;
	const int posterize   = ( index & grade_posterize ) ? 1 : 0;
	const int deband      = ( index & grade_deband ) ? 1 : 0;
	const int shift       = ( index & grade_shift ) ? 1 : 0;

	const std::string source = std::vformat( "#define TEMPERATURE {:d}\n#define LEVELS {:d}\n#define HUE {:d}\n#define CURVES {:d}\n"
	                                         "#define TONE {:d}\n#define BASIC {:d}\n#define SHARPEN {:d}\n#define GRAIN {:d}\n#define VIGNETTE {:d}\n"
	                                         "#define INVERT {:d}\n#define POSTERIZE {:d}\n#define DEBAND {:d}\n#define SHIFT {:d}\n",
	                                         std::make_format_args( temperature, levels, hue, curves, tone, basic, sharpen, grain, vignette, invert,
	                                                                posterize, deband, shift ) ) +
	                           k_shader;

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

	g_console.print( std::vformat( "color correction: grade shader built, stages {:#x}", std::make_format_args( index ) ).c_str( ) );

	return this->m_shaders[ index ];
}

void n_color_correction::impl_t::release_targets( )
{
	if ( this->m_resolve_surface ) {
		this->m_resolve_surface->Release( );
		this->m_resolve_surface = nullptr;
	}

	if ( this->m_input_surface ) {
		this->m_input_surface->Release( );
		this->m_input_surface = nullptr;
	}

	if ( this->m_input_texture ) {
		this->m_input_texture->Release( );
		this->m_input_texture = nullptr;
	}

	this->m_width        = 0;
	this->m_height       = 0;
	this->m_format       = D3DFMT_UNKNOWN;
	this->m_multi_sample = D3DMULTISAMPLE_NONE;
}

void n_color_correction::impl_t::release( )
{
	this->release_targets( );

	if ( this->m_lut_texture ) {
		this->m_lut_texture->Release( );
		this->m_lut_texture = nullptr;
	}

	this->m_lut_valid = false;

	for ( IDirect3DPixelShader9*& shader : this->m_shaders ) {
		if ( shader ) {
			shader->Release( );
			shader = nullptr;
		}
	}
}

void n_color_correction::impl_t::on_device_lost( )
{
	this->release( );
}

bool n_color_correction::impl_t::wants_pass( )
{
	if ( this->m_compile_failed )
		return false;

	if ( this->grade_flags( ) == 0 )
		return false;

	auto* engine = g_interfaces.m_engine_client;

	if ( !engine || !engine->is_in_game( ) ) {
		static bool logged_menu = false;

		if ( !logged_menu ) {
			logged_menu = true;
			g_console.print( "color correction: grade / sharpen / grain / vignette / filters on, but not in a game yet — the pass only runs on a live map" );
		}

		return false;
	}

	return true;
}

bool n_color_correction::impl_t::build( IDirect3DDevice9* device, const D3DSURFACE_DESC& description )
{
	const int width  = static_cast< int >( description.Width );
	const int height = static_cast< int >( description.Height );

	if ( this->m_input_texture && this->m_width == width && this->m_height == height && this->m_format == description.Format &&
	     this->m_multi_sample == description.MultiSampleType )
		return true;

	this->release_targets( );

	if ( FAILED( device->CreateTexture( description.Width, description.Height, 1, D3DUSAGE_RENDERTARGET, description.Format, D3DPOOL_DEFAULT,
	                                    &this->m_input_texture, nullptr ) ) ||
	     FAILED( this->m_input_texture->GetSurfaceLevel( 0, &this->m_input_surface ) ) ) {
		g_console.print< n_console::log_level::WARNING >( "color correction: render target creation failed" );
		this->release( );
		return false;
	}

	if ( description.MultiSampleType != D3DMULTISAMPLE_NONE &&
	     FAILED( device->CreateRenderTarget( description.Width, description.Height, description.Format, D3DMULTISAMPLE_NONE, 0, FALSE,
	                                         &this->m_resolve_surface, nullptr ) ) ) {
		g_console.print< n_console::log_level::WARNING >( "color correction: msaa resolve target creation failed" );
		this->release( );
		return false;
	}

	this->m_width        = width;
	this->m_height       = height;
	this->m_format       = description.Format;
	this->m_multi_sample = description.MultiSampleType;

	const int samples = static_cast< int >( description.MultiSampleType );

	g_console.print( std::vformat( "color correction: built {:d}x{:d} msaa {:d}", std::make_format_args( width, height, samples ) ).c_str( ) );

	return true;
}

bool n_color_correction::impl_t::on_end_scene_post( IDirect3DDevice9* device )
{
	if ( !device )
		return false;

	if ( !this->wants_pass( ) ) {
		if ( this->m_input_texture )
			this->release_targets( );

		return false;
	}

	const n_gpu_timer::scope_t gpu_timer( device, n_gpu_timer::pass_color_correction );

	IDirect3DSurface9* back_buffer = nullptr;
	if ( FAILED( device->GetBackBuffer( 0, 0, D3DBACKBUFFER_TYPE_MONO, &back_buffer ) ) || !back_buffer )
		return false;

	D3DSURFACE_DESC description{ };
	if ( FAILED( back_buffer->GetDesc( &description ) ) ) {
		back_buffer->Release( );
		return false;
	}

	if ( !this->build( device, description ) ) {
		back_buffer->Release( );
		return false;
	}

	const unsigned int flags = this->grade_flags( );

	IDirect3DPixelShader9* shader = this->ensure_shader( device, flags );

	if ( !shader ) {
		back_buffer->Release( );
		return false;
	}

	const bool curves_on = ( flags & grade_curves ) != 0;

	if ( curves_on ) {
		n_color_curve::curve_t curves[ 4 ];
		load_curves( curves );

		if ( !this->update_lut( device, curves ) ) {
			static bool logged_lut = false;

			if ( !logged_lut ) {
				logged_lut = true;
				g_console.print< n_console::log_level::WARNING >( "color correction: curves table creation failed" );
			}

			back_buffer->Release( );
			return false;
		}
	}

	static bool logged_blit = false;

	const long blit_result = n_frame_copy::copy( device, back_buffer, this->m_resolve_surface, this->m_input_surface );

	if ( FAILED( blit_result ) ) {
		if ( !logged_blit ) {
			logged_blit = true;

			const unsigned long code = static_cast< unsigned long >( blit_result );

			g_console.print< n_console::log_level::WARNING >(
				std::vformat( "color correction: back buffer blit failed {:#x}", std::make_format_args( code ) ).c_str( ) );
		}

		back_buffer->Release( );
		return false;
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

	IDirect3DSurface9* old_render_target                = nullptr;
	IDirect3DSurface9* old_depth_stencil                = nullptr;
	IDirect3DPixelShader9* old_pixel_shader             = nullptr;
	IDirect3DVertexShader9* old_vertex_shader           = nullptr;
	IDirect3DBaseTexture9* old_texture                  = nullptr;
	IDirect3DBaseTexture9* old_lut_texture              = nullptr;
	IDirect3DVertexDeclaration9* old_vertex_declaration = nullptr;
	D3DVIEWPORT9 old_viewport{ };

	DWORD z_enable, z_write, alpha_blend, alpha_test, cull, scissor, srgb_write, address_u, address_v, min_filter, mag_filter, srgb_texture, fvf;

	DWORD stencil, color_write, clip_planes, fill_mode;

	DWORD lut_address_u, lut_address_v, lut_min_filter, lut_mag_filter, lut_srgb_texture;

	float old_constants[ 19 ][ 4 ]{ };

	device->GetRenderTarget( 0, &old_render_target );

	if ( FAILED( device->GetDepthStencilSurface( &old_depth_stencil ) ) )
		old_depth_stencil = nullptr;

	device->GetPixelShader( &old_pixel_shader );
	device->GetVertexShader( &old_vertex_shader );
	device->GetTexture( 0, &old_texture );
	device->GetViewport( &old_viewport );
	device->GetRenderState( D3DRS_STENCILENABLE, &stencil );
	device->GetRenderState( D3DRS_COLORWRITEENABLE, &color_write );
	device->GetRenderState( D3DRS_CLIPPLANEENABLE, &clip_planes );
	device->GetRenderState( D3DRS_FILLMODE, &fill_mode );
	device->GetRenderState( D3DRS_ZENABLE, &z_enable );
	device->GetRenderState( D3DRS_ZWRITEENABLE, &z_write );
	device->GetRenderState( D3DRS_ALPHABLENDENABLE, &alpha_blend );
	device->GetRenderState( D3DRS_ALPHATESTENABLE, &alpha_test );
	device->GetRenderState( D3DRS_CULLMODE, &cull );
	device->GetRenderState( D3DRS_SCISSORTESTENABLE, &scissor );
	device->GetRenderState( D3DRS_SRGBWRITEENABLE, &srgb_write );
	device->GetSamplerState( 0, D3DSAMP_ADDRESSU, &address_u );
	device->GetSamplerState( 0, D3DSAMP_ADDRESSV, &address_v );
	device->GetSamplerState( 0, D3DSAMP_MINFILTER, &min_filter );
	device->GetSamplerState( 0, D3DSAMP_MAGFILTER, &mag_filter );
	device->GetSamplerState( 0, D3DSAMP_SRGBTEXTURE, &srgb_texture );
	device->GetTexture( 1, &old_lut_texture );
	device->GetSamplerState( 1, D3DSAMP_ADDRESSU, &lut_address_u );
	device->GetSamplerState( 1, D3DSAMP_ADDRESSV, &lut_address_v );
	device->GetSamplerState( 1, D3DSAMP_MINFILTER, &lut_min_filter );
	device->GetSamplerState( 1, D3DSAMP_MAGFILTER, &lut_mag_filter );
	device->GetSamplerState( 1, D3DSAMP_SRGBTEXTURE, &lut_srgb_texture );
	device->GetFVF( &fvf );
	device->GetVertexDeclaration( &old_vertex_declaration );
	device->GetPixelShaderConstantF( 0, old_constants[ 0 ], 19 );

	n_stream_guard::state_t streams{ };
	streams.capture( device );

	D3DVIEWPORT9 viewport{ 0, 0, static_cast< DWORD >( this->m_width ), static_cast< DWORD >( this->m_height ), 0.f, 1.f };

	device->SetDepthStencilSurface( nullptr );
	device->SetViewport( &viewport );
	device->SetVertexShader( nullptr );
	device->SetVertexDeclaration( nullptr );
	device->SetFVF( D3DFVF_XYZRHW | D3DFVF_TEX1 );
	device->SetRenderState( D3DRS_STENCILENABLE, FALSE );
	device->SetRenderState( D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE |
	                                                    D3DCOLORWRITEENABLE_ALPHA );
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
	device->SetSamplerState( 0, D3DSAMP_SRGBTEXTURE, 0 );
	device->SetSamplerState( 1, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP );
	device->SetSamplerState( 1, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP );
	device->SetSamplerState( 1, D3DSAMP_MINFILTER, D3DTEXF_LINEAR );
	device->SetSamplerState( 1, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR );
	device->SetSamplerState( 1, D3DSAMP_SRGBTEXTURE, 0 );
	device->SetPixelShader( shader );

	const float exposure = std::pow( 2.f, GET_VARIABLE( g_variables.m_color_correction_exposure, float ) );

	const float constant_0[ 4 ] = { GET_VARIABLE( g_variables.m_color_correction_red, float ),
		                            GET_VARIABLE( g_variables.m_color_correction_green, float ),
		                            GET_VARIABLE( g_variables.m_color_correction_blue, float ),
		                            GET_VARIABLE( g_variables.m_color_correction_contrast, float ) };

	const float constant_1[ 4 ] = { GET_VARIABLE( g_variables.m_color_correction_saturation, float ),
		                            GET_VARIABLE( g_variables.m_color_correction_temperature, float ), exposure, 0.f };

	float hue_matrix[ 3 ][ 3 ];
	compute_hue_matrix( GET_VARIABLE( g_variables.m_color_correction_hue, float ) * ( 3.14159265f / 180.f ), hue_matrix );

	const float constant_2[ 4 ] = { hue_matrix[ 0 ][ 0 ], hue_matrix[ 0 ][ 1 ], hue_matrix[ 0 ][ 2 ], 0.f };
	const float constant_3[ 4 ] = { hue_matrix[ 1 ][ 0 ], hue_matrix[ 1 ][ 1 ], hue_matrix[ 1 ][ 2 ], 0.f };
	const float constant_4[ 4 ] = { hue_matrix[ 2 ][ 0 ], hue_matrix[ 2 ][ 1 ], hue_matrix[ 2 ][ 2 ], 0.f };

	const float in_black = GET_VARIABLE( g_variables.m_color_correction_levels_in_black, float );
	const float in_white = std::max( GET_VARIABLE( g_variables.m_color_correction_levels_in_white, float ), in_black + 0.001f );
	const float gamma    = GET_VARIABLE( g_variables.m_color_correction_levels_gamma, float );

	const float constant_5[ 4 ] = { in_black, in_white, gamma > 0.001f ? 1.f / gamma : 1.f, 0.f };

	const float out_black = GET_VARIABLE( g_variables.m_color_correction_levels_out_black, float );

	const float constant_6[ 4 ] = { out_black, GET_VARIABLE( g_variables.m_color_correction_levels_out_white, float ) - out_black,
		                            GET_VARIABLE( g_variables.m_color_correction_highlights, float ),
		                            GET_VARIABLE( g_variables.m_color_correction_shadows, float ) };

	const float constant_13[ 4 ] = { 1.f / width, 1.f / height, GET_VARIABLE( g_variables.m_sharpen_strength, float ),
		                             GET_VARIABLE( g_variables.m_sharpen_threshold, float ) };

	const unsigned int grain_frame = this->m_grain_frame++;
	const float grain_angle        = std::fmod( static_cast< float >( grain_frame % 4096u ) * 2.39996323f, 6.28318531f );
	const float grain_size         = std::max( GET_VARIABLE( g_variables.m_film_grain_size, float ), 0.5f );

	const float constant_14[ 4 ] = { std::cos( grain_angle ), std::sin( grain_angle ),
		                             static_cast< float >( grain_frame % 256u ) / 256.f + 0.5f / 256.f,
		                             GET_VARIABLE( g_variables.m_film_grain_intensity, float ) };

	const float constant_15[ 4 ] = { width / grain_size, height / grain_size, width / height, 0.f };

	// range floor: 1 / 0 would be inf * 0 = nan at the position
	const float constant_12[ 4 ] = { GET_VARIABLE( g_variables.m_film_grain_response_position, float ),
		                             1.f / std::max( GET_VARIABLE( g_variables.m_film_grain_response_range, float ), 0.01f ),
		                             GET_VARIABLE( g_variables.m_film_grain_response_minimum, float ), 0.f };

	const float vignette_radius = GET_VARIABLE( g_variables.m_vignette_radius, float );
	const float vignette_aspect = std::max( GET_VARIABLE( g_variables.m_vignette_aspect, float ), 0.1f );
	const float vignette_blur   = std::max( GET_VARIABLE( g_variables.m_vignette_blur, float ), 0.f );
	const c_color vignette_color = GET_VARIABLE( g_variables.m_vignette_color, c_color );

	const float constant_7[ 4 ] = { vignette_radius, vignette_radius + std::max( GET_VARIABLE( g_variables.m_vignette_feather, float ), 0.001f ),
		                            ( width / height ) / vignette_aspect, GET_VARIABLE( g_variables.m_vignette_invert, bool ) ? 1.f : 0.f };

	const float constant_8[ 4 ] = { vignette_color[ 0 ] / 255.f, vignette_color[ 1 ] / 255.f, vignette_color[ 2 ] / 255.f, vignette_color[ 3 ] / 255.f };

	const float constant_9[ 4 ] = { vignette_blur, vignette_blur > 0.f ? 1.f : 0.f, 0.f, 0.f };

	const float constant_10[ 4 ] = { GET_VARIABLE( g_variables.m_channel_shift_red_x, float ) / width,
		                             GET_VARIABLE( g_variables.m_channel_shift_red_y, float ) / height,
		                             GET_VARIABLE( g_variables.m_channel_shift_green_x, float ) / width,
		                             GET_VARIABLE( g_variables.m_channel_shift_green_y, float ) / height };

	const float constant_11[ 4 ] = { GET_VARIABLE( g_variables.m_channel_shift_blue_x, float ) / width,
		                             GET_VARIABLE( g_variables.m_channel_shift_blue_y, float ) / height, 0.f, 0.f };

	// x = log2e / ( 2 sigma² ), sigma = threshold in 8 bit steps; grain / 8192; w = ign temporal offset ( 5.588238 px per frame )
	const float deband_sigma     = std::max( GET_VARIABLE( g_variables.m_deband_threshold, float ), 1.f ) / 255.f;
	const float constant_16[ 4 ] = { 1.4426950f * 0.5f / ( deband_sigma * deband_sigma ),
		                             std::clamp( GET_VARIABLE( g_variables.m_deband_range, float ), 1.f, 16.f ),
		                             GET_VARIABLE( g_variables.m_deband_grain, float ) / 8192.f,
		                             static_cast< float >( grain_frame % 64u ) * 5.588238f };

	const float posterize_steps = static_cast< float >( std::clamp( GET_VARIABLE( g_variables.m_posterize_levels, int ), 2, 64 ) - 1 );

	const float constant_17[ 4 ] = { static_cast< float >( std::clamp( GET_VARIABLE( g_variables.m_deband_iterations, int ), 1, 4 ) * 16 ), posterize_steps,
		                             GET_VARIABLE( g_variables.m_posterize_dither, float ), GET_VARIABLE( g_variables.m_posterize_strength, float ) };

	const float constant_18[ 4 ] = { GET_VARIABLE( g_variables.m_invert_filter_strength, float ),
		                             GET_VARIABLE( g_variables.m_invert_filter_mode, int ) == 1 ? 1.f : 0.f, 0.f, 0.f };

	const float block[ 19 ][ 4 ] = { { constant_0[ 0 ], constant_0[ 1 ], constant_0[ 2 ], constant_0[ 3 ] },
		                             { constant_1[ 0 ], constant_1[ 1 ], constant_1[ 2 ], constant_1[ 3 ] },
		                             { constant_2[ 0 ], constant_2[ 1 ], constant_2[ 2 ], constant_2[ 3 ] },
		                             { constant_3[ 0 ], constant_3[ 1 ], constant_3[ 2 ], constant_3[ 3 ] },
		                             { constant_4[ 0 ], constant_4[ 1 ], constant_4[ 2 ], constant_4[ 3 ] },
		                             { constant_5[ 0 ], constant_5[ 1 ], constant_5[ 2 ], constant_5[ 3 ] },
		                             { constant_6[ 0 ], constant_6[ 1 ], constant_6[ 2 ], constant_6[ 3 ] },
		                             { constant_7[ 0 ], constant_7[ 1 ], constant_7[ 2 ], constant_7[ 3 ] },
		                             { constant_8[ 0 ], constant_8[ 1 ], constant_8[ 2 ], constant_8[ 3 ] },
		                             { constant_9[ 0 ], constant_9[ 1 ], constant_9[ 2 ], constant_9[ 3 ] },
		                             { constant_10[ 0 ], constant_10[ 1 ], constant_10[ 2 ], constant_10[ 3 ] },
		                             { constant_11[ 0 ], constant_11[ 1 ], constant_11[ 2 ], constant_11[ 3 ] },
		                             { constant_12[ 0 ], constant_12[ 1 ], constant_12[ 2 ], constant_12[ 3 ] },
		                             { constant_13[ 0 ], constant_13[ 1 ], constant_13[ 2 ], constant_13[ 3 ] },
		                             { constant_14[ 0 ], constant_14[ 1 ], constant_14[ 2 ], constant_14[ 3 ] },
		                             { constant_15[ 0 ], constant_15[ 1 ], constant_15[ 2 ], constant_15[ 3 ] },
		                             { constant_16[ 0 ], constant_16[ 1 ], constant_16[ 2 ], constant_16[ 3 ] },
		                             { constant_17[ 0 ], constant_17[ 1 ], constant_17[ 2 ], constant_17[ 3 ] },
		                             { constant_18[ 0 ], constant_18[ 1 ], constant_18[ 2 ], constant_18[ 3 ] } };

	device->SetPixelShaderConstantF( 0, block[ 0 ], 19 );

	static bool logged_draw = false;
	long draw_result        = -1;

	if ( SUCCEEDED( device->SetRenderTarget( 0, back_buffer ) ) ) {
		device->SetTexture( 0, this->m_input_texture );

		if ( curves_on )
			device->SetTexture( 1, this->m_lut_texture );

		const bool own_scene = SUCCEEDED( device->BeginScene( ) );

		draw_result = n_fx_compat::draw_up( device, D3DPT_TRIANGLESTRIP, 2, quad, sizeof( vertex_t ) );

		if ( own_scene )
			device->EndScene( );
	}

	if ( !logged_draw ) {
		logged_draw = true;

		if ( FAILED( draw_result ) ) {
			const unsigned long code = static_cast< unsigned long >( draw_result );

			g_console.print< n_console::log_level::WARNING >(
				std::vformat( "color correction: draw failed {:#x}", std::make_format_args( code ) ).c_str( ) );
		}
		else
			g_console.print( "color correction: first pass drawn" );
	}

	if ( old_render_target )
		device->SetRenderTarget( 0, old_render_target );

	device->SetDepthStencilSurface( old_depth_stencil );
	device->SetPixelShader( old_pixel_shader );
	device->SetVertexShader( old_vertex_shader );
	device->SetTexture( 0, old_texture );
	device->SetViewport( &old_viewport );
	device->SetRenderState( D3DRS_STENCILENABLE, stencil );
	device->SetRenderState( D3DRS_COLORWRITEENABLE, color_write );
	device->SetRenderState( D3DRS_CLIPPLANEENABLE, clip_planes );
	device->SetRenderState( D3DRS_FILLMODE, fill_mode );
	device->SetRenderState( D3DRS_ZENABLE, z_enable );
	device->SetRenderState( D3DRS_ZWRITEENABLE, z_write );
	device->SetRenderState( D3DRS_ALPHABLENDENABLE, alpha_blend );
	device->SetRenderState( D3DRS_ALPHATESTENABLE, alpha_test );
	device->SetRenderState( D3DRS_CULLMODE, cull );
	device->SetRenderState( D3DRS_SCISSORTESTENABLE, scissor );
	device->SetRenderState( D3DRS_SRGBWRITEENABLE, srgb_write );
	device->SetSamplerState( 0, D3DSAMP_ADDRESSU, address_u );
	device->SetSamplerState( 0, D3DSAMP_ADDRESSV, address_v );
	device->SetSamplerState( 0, D3DSAMP_MINFILTER, min_filter );
	device->SetSamplerState( 0, D3DSAMP_MAGFILTER, mag_filter );
	device->SetSamplerState( 0, D3DSAMP_SRGBTEXTURE, srgb_texture );
	device->SetTexture( 1, old_lut_texture );
	device->SetSamplerState( 1, D3DSAMP_ADDRESSU, lut_address_u );
	device->SetSamplerState( 1, D3DSAMP_ADDRESSV, lut_address_v );
	device->SetSamplerState( 1, D3DSAMP_MINFILTER, lut_min_filter );
	device->SetSamplerState( 1, D3DSAMP_MAGFILTER, lut_mag_filter );
	device->SetSamplerState( 1, D3DSAMP_SRGBTEXTURE, lut_srgb_texture );

	if ( old_vertex_declaration ) {
		device->SetVertexDeclaration( old_vertex_declaration );
		old_vertex_declaration->Release( );
	} else if ( fvf )
		device->SetFVF( fvf );

	streams.restore( device );

	device->SetPixelShaderConstantF( 0, old_constants[ 0 ], 19 );

	if ( old_render_target )
		old_render_target->Release( );

	if ( old_depth_stencil )
		old_depth_stencil->Release( );

	if ( old_vertex_shader )
		old_vertex_shader->Release( );

	if ( old_pixel_shader )
		old_pixel_shader->Release( );

	if ( old_texture )
		old_texture->Release( );

	if ( old_lut_texture )
		old_lut_texture->Release( );

	back_buffer->Release( );

	return SUCCEEDED( draw_result );
}
