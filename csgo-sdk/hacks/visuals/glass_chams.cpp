#include "glass_chams.h"
#include "model_shader.h"
#include "screen/frame_copy.h"
#include "screen/render_queue.h"
#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"
#include "../../globals/config/variables.h"
#include "../../globals/globals.h"

#include <array>
#include <initializer_list>
#include <iterator>
#include <vector>

namespace
{
	using namespace n_model_shader;

	/* two tone's vs ( skinned position, normal, tangent, uv ) + the tangent frame in screen space. engine constants left by the
	   model's own shader: c2 camera pos, c8 viewproj, c58 bones */
	constexpr const char k_vs[] = R"(
float4 cEyePos : register( c2 );
float4x4 cViewProj : register( c8 );
float4x3 cModel[ 53 ] : register( c58 );

struct VS_INPUT {
	float4 vPos : POSITION;
	float4 vBoneWeights : BLENDWEIGHT;
	float4 vBoneIndices : BLENDINDICES;
	float4 vNormal : NORMAL;
	float4 vTexCoord0 : TEXCOORD0;
#if HAS_TANGENT
	float4 vUserData : TANGENT;
#endif
};

struct VS_OUTPUT {
	float4 vPos : POSITION;
	float2 vUv : TEXCOORD0;
	float3 vNormal : TEXCOORD1;
	float4 vTangent : TEXCOORD2;
	float3 vView : TEXCOORD3;
	float4 vBendTB : TEXCOORD4;
	float2 vBendN : TEXCOORD5;
};

// common_vs_fxc.h _DecompressUByte4NormalTangent 1:1
void decompress( float4 inputNormal, out float3 outputNormal, out float4 outputTangent )
{
	float4 ztztSignBits = ( inputNormal - 128.0f ) < 0;
	float4 xyxyAbs      = abs( inputNormal - 128.0f ) - ztztSignBits;
	float4 xyxySignBits = ( xyxyAbs - 64.0f ) < 0;
	float4 normTan      = ( abs( xyxyAbs - 64.0f ) - xyxySignBits ) / 63.0f;
	outputNormal.xy     = normTan.xy;
	outputTangent.xy    = normTan.zw;

	float4 xyxySigns = 1 - 2 * xyxySignBits;
	float4 ztztSigns = 1 - 2 * ztztSignBits;

	outputNormal.z    = 1.0f - outputNormal.x - outputNormal.y;
	outputNormal.xyz  = normalize( outputNormal.xyz );
	outputNormal.xy  *= xyxySigns.xy;
	outputNormal.z   *= ztztSigns.x;

	outputTangent.z    = 1.0f - outputTangent.x - outputTangent.y;
	outputTangent.xyz  = normalize( outputTangent.xyz );
	outputTangent.xy  *= xyxySigns.zw;
	outputTangent.z   *= ztztSigns.z;
	outputTangent.w    = ztztSigns.w;
}

VS_OUTPUT main( const VS_INPUT i )
{
	float4 vWeights = i.vBoneWeights;
#if COMPRESSED_VERTS
	vWeights = ( vWeights + 1 ) / 32768;
#endif
	int3 boneIndices = D3DCOLORtoUBYTE4( i.vBoneIndices ).xyz;
	float3 weights   = vWeights.xyz;
	weights[ 2 ]     = 1 - ( weights[ 0 ] + weights[ 1 ] );
	float4x3 blendMatrix = cModel[ boneIndices[ 0 ] ] * weights[ 0 ] + cModel[ boneIndices[ 1 ] ] * weights[ 1 ] + cModel[ boneIndices[ 2 ] ] * weights[ 2 ];

	float3 normal;
	float4 tangent;
#if COMPRESSED_VERTS
	decompress( i.vNormal, normal, tangent );
#else
	normal = i.vNormal.xyz;
#if HAS_TANGENT
	tangent = i.vUserData;
#else
	tangent = float4( 1, 0, 0, 1 );
#endif
#endif

	float3 world = mul( i.vPos, blendMatrix );
	float3 n     = normalize( mul( normal, ( float3x3 )blendMatrix ) );
	float3 t     = normalize( mul( tangent.xyz, ( float3x3 )blendMatrix ) );
	float3 b     = cross( n, t ) * tangent.w;

	VS_OUTPUT o;
	o.vPos     = mul( float4( world, 1.0f ), cViewProj );
	o.vUv      = i.vTexCoord0.xy;
	o.vNormal  = n;
	o.vTangent = float4( t, tangent.w );
	o.vView    = cEyePos.xyz - world;

	/* clip space of each frame axis / depth -> uv ( y down ): projection is linear, so a normal mapped normal bends by the same
	   mix of these. depth floored at 100 so the viewmodel bends like a model 100 units out */
	float2 to_uv = float2( 0.5f, -0.5f ) / max( o.vPos.w, 100.0f );
	o.vBendTB    = float4( mul( float4( t, 0.0f ), cViewProj ).xy * to_uv, mul( float4( b, 0.0f ), cViewProj ).xy * to_uv );
	o.vBendN     = mul( float4( n, 0.0f ), cViewProj ).xy * to_uv;
	return o;
}
)";

	/* what's behind, pulled against the (normal mapped) normal: a lens flips it like real glass. rgb split, rim + sky glint.
	   c30 tint ( display, alpha pre-scaled ), c31 refract ( world units ) / bump / alpha test on / alpha ref, c32 xy = 1 / rt size.
	   s0 base ( alpha test ), s1 / s3 engine normal map, s15 frame copy */
	constexpr const char k_ps[] = R"(
const float4 cTint : register( c30 );
const float4 cParams : register( c31 );
const float4 cScreen : register( c32 );

sampler BaseSampler : register( s0 );
#if BUMP_SAMPLER == 1
sampler NormalSampler : register( s1 );
#elif BUMP_SAMPLER == 3
sampler NormalSampler : register( s3 );
#endif
sampler FrameSampler : register( s15 );

struct PS_INPUT {
	float2 vUv : TEXCOORD0;
	float3 vNormal : TEXCOORD1;
	float4 vTangent : TEXCOORD2;
	float3 vView : TEXCOORD3;
	float4 vBendTB : TEXCOORD4;
	float2 vBendN : TEXCOORD5;
	float2 vPixel : VPOS;
};

float4 main( PS_INPUT i ) : COLOR
{
	// material alpha test: hair, straps keep their cutout
	clip( cParams.z * ( tex2D( BaseSampler, i.vUv ).a - cParams.w ) );

	float3 tn = float3( 0.0f, 0.0f, 1.0f );
#if BUMP_SAMPLER
	tn = tex2D( NormalSampler, i.vUv ).xyz * 2.0f - 1.0f;
	tn = normalize( float3( tn.xy * cParams.y, tn.z ) );
#endif

	// phong_ps20b.fxc:502-553 frame
	float3 n    = normalize( i.vNormal );
	float3 t    = normalize( i.vTangent.xyz );
	float3 b    = cross( n, t ) * i.vTangent.w;
	float3 unit = normalize( tn.x * t + tn.y * b + tn.z * n );

	float2 uv   = ( i.vPixel + 0.5f ) * cScreen.xy;
	float2 bend = ( tn.x * i.vBendTB.xy + tn.y * i.vBendTB.zw + tn.z * i.vBendN ) * cParams.x;
	float3 seen = float3( tex2D( FrameSampler, uv - bend * 0.94f ).r, tex2D( FrameSampler, uv - bend ).g, tex2D( FrameSampler, uv - bend * 1.06f ).b );

	float3 view      = normalize( i.vView );
	float  rim       = 1.0f - saturate( dot( unit, view ) );
	float3 reflected = reflect( -view, unit );
	float  glint     = pow( rim, 4.0f ) * 0.35f + pow( saturate( reflected.z ), 32.0f ) * 0.4f;
	return float4( seen * cTint.rgb * ( 0.85f + rim * 0.3f ) + glint, cTint.a );
}
)";

	// vs: plain, plain + tangent stream, compressed. ps: no normal map, normal map in s1 (character), s3 (phong / bump)
	IDirect3DVertexShader9* g_vs[ 3 ]{ };
	IDirect3DPixelShader9* g_ps[ 3 ]{ };
	defs_t g_vs_defs{ }, g_ps_defs{ };
	bool g_gpu_failed = false;

	constexpr DWORD k_frame_stage = 15;

	void release_gpu( )
	{
		for ( IDirect3DVertexShader9*& shader : g_vs ) {
			if ( shader )
				shader->Release( );
			shader = nullptr;
		}

		for ( IDirect3DPixelShader9*& shader : g_ps ) {
			if ( shader )
				shader->Release( );
			shader = nullptr;
		}

		g_vs_defs.clear( );
		g_ps_defs.clear( );
	}

	bool gpu_ready( IDirect3DDevice9* device )
	{
		if ( g_ps[ 0 ] )
			return true;

		if ( g_gpu_failed )
			return false;

		const D3DXMACRO vs_macros[ 3 ][ 3 ] = { { { "COMPRESSED_VERTS", "0" }, { "HAS_TANGENT", "0" }, { nullptr, nullptr } },
			                                    { { "COMPRESSED_VERTS", "0" }, { "HAS_TANGENT", "1" }, { nullptr, nullptr } },
			                                    { { "COMPRESSED_VERTS", "1" }, { "HAS_TANGENT", "0" }, { nullptr, nullptr } } };
		const D3DXMACRO ps_macros[ 3 ][ 2 ] = { { { "BUMP_SAMPLER", "0" }, { nullptr, nullptr } },
			                                    { { "BUMP_SAMPLER", "1" }, { nullptr, nullptr } },
			                                    { { "BUMP_SAMPLER", "3" }, { nullptr, nullptr } } };

		bool ok = true;

		for ( int i = 0; i < 3 && ok; i++ ) {
			ID3DXBuffer* code = compile( "GLASS", k_vs, "vs_3_0", vs_macros[ i ] );
			ok = code && SUCCEEDED( device->CreateVertexShader( static_cast< const DWORD* >( code->GetBufferPointer( ) ), &g_vs[ i ] ) );
			if ( ok )
				collect_defs( static_cast< const DWORD* >( code->GetBufferPointer( ) ), g_vs_defs );
			if ( code )
				code->Release( );
		}

		for ( int i = 0; i < 3 && ok; i++ ) {
			ID3DXBuffer* code = compile( "GLASS", k_ps, "ps_3_0", ps_macros[ i ] );
			ok = code && SUCCEEDED( device->CreatePixelShader( static_cast< const DWORD* >( code->GetBufferPointer( ) ), &g_ps[ i ] ) );
			if ( ok )
				collect_defs( static_cast< const DWORD* >( code->GetBufferPointer( ) ), g_ps_defs );
			if ( code )
				code->Release( );
		}

		if ( !ok ) {
			botox_dbg_log( "GLASS: gpu FAILED" );
			release_gpu( );
			g_gpu_failed = true;
		}

		return ok;
	}

	// copy of the bound rt, keyed by its desc. ponytail: 2 entries (main view + preview / reflection rt), a 3rd rt churns creates
	struct frame_t {
		IDirect3DTexture9* m_texture = nullptr;
		IDirect3DSurface9* m_surface = nullptr;
		D3DSURFACE_DESC m_desc{ };
		unsigned int m_used = 0U;
	};

	frame_t g_frames[ 2 ]{ };
	frame_t* g_frame           = nullptr; // copied at the current layer's first dip
	unsigned int g_frame_clock = 0U;
	bool g_copy_failed         = false;

	void release_frame( frame_t& frame )
	{
		for ( IUnknown* held : std::initializer_list< IUnknown* >{ frame.m_surface, frame.m_texture } )
			if ( held )
				held->Release( );

		frame = frame_t{ };
	}

	bool same_target( const D3DSURFACE_DESC& a, const D3DSURFACE_DESC& b )
	{
		return a.Width == b.Width && a.Height == b.Height && a.Format == b.Format && a.MultiSampleType == b.MultiSampleType &&
		       a.MultiSampleQuality == b.MultiSampleQuality;
	}

	// nullptr = glass off until Reset (logged once)
	frame_t* copy_frame( IDirect3DDevice9* device )
	{
		if ( g_copy_failed )
			return nullptr;

		IDirect3DSurface9* target = nullptr;
		if ( FAILED( device->GetRenderTarget( 0, &target ) ) || !target )
			return nullptr;

		D3DSURFACE_DESC desc{ };
		frame_t* frame = nullptr;

		if ( SUCCEEDED( target->GetDesc( &desc ) ) ) {
			for ( frame_t& entry : g_frames )
				if ( entry.m_texture && same_target( entry.m_desc, desc ) )
					frame = &entry;

			if ( !frame ) {
				frame = g_frames[ 0 ].m_used <= g_frames[ 1 ].m_used ? &g_frames[ 0 ] : &g_frames[ 1 ];
				release_frame( *frame );

				if ( FAILED( device->CreateTexture( desc.Width, desc.Height, 1, D3DUSAGE_RENDERTARGET, desc.Format, D3DPOOL_DEFAULT, &frame->m_texture, nullptr ) ) ||
				     FAILED( frame->m_texture->GetSurfaceLevel( 0, &frame->m_surface ) ) ) {
					botox_dbg_log( "GLASS: copy %ux%u fmt %d msaa %d creation FAILED, glass off", desc.Width, desc.Height, static_cast< int >( desc.Format ),
					               static_cast< int >( desc.MultiSampleType ) );
					release_frame( *frame );
					frame = nullptr;
				} else
					frame->m_desc = desc;
			}

			if ( frame ) {
				if ( const HRESULT copied = n_frame_copy::copy( device, target, desc, frame->m_surface ); FAILED( copied ) ) {
					botox_dbg_log( "GLASS: copy FAILED %#lx fmt %d msaa %d, glass off", static_cast< unsigned long >( copied ), static_cast< int >( desc.Format ),
					               static_cast< int >( desc.MultiSampleType ) );
					frame = nullptr;
				} else
					frame->m_used = ++g_frame_clock;
			}
		}

		target->Release( );

		if ( !frame )
			g_copy_failed = true;

		return frame;
	}
}

void n_glass_chams::impl_t::begin( const float tint[ 4 ], const bool ignorez, const bool lay_depth )
{
	const float refract = GET_VARIABLE( g_variables.m_chams_glass_refract, float );
	const float bump    = GET_VARIABLE( g_variables.m_chams_glass_bump, float );

	auto arm = [ c = std::to_array( { tint[ 0 ], tint[ 1 ], tint[ 2 ], tint[ 3 ] } ), refract, bump, ignorez, lay_depth ] {
		for ( int i = 0; i < 4; i++ )
			g_glass_chams.m_tint[ i ] = c[ i ];

		g_glass_chams.m_refract      = refract;
		g_glass_chams.m_bump         = bump;
		g_glass_chams.m_ignorez      = ignorez;
		g_glass_chams.m_translucent  = c[ 3 ] < 1.f;
		g_glass_chams.m_lay_depth    = lay_depth;
		g_glass_chams.m_copy_pending = true;
		g_glass_chams.m_armed.store( true, std::memory_order_relaxed );
	};

	if ( !n_render_queue::submit( arm ) )
		arm( );
}

void n_glass_chams::impl_t::end( )
{
	auto disarm = [ ] { g_glass_chams.m_armed.store( false, std::memory_order_relaxed ); };

	if ( !n_render_queue::submit( disarm ) )
		disarm( );
}

long n_glass_chams::impl_t::draw( const draw_indexed_primitive_t original, IDirect3DDevice9* device, const D3DPRIMITIVETYPE type, const int base_vertex,
                                  const unsigned int min_vertex, const unsigned int vertices, const unsigned int start_index, const unsigned int primitives )
{
	// no shader / not a studio mesh = the model's own material draws as is
	const int compressed = gpu_ready( device ) ? vertex_compression( device ) : -1;
	if ( compressed < 0 )
		return original( device, type, base_vertex, min_vertex, vertices, start_index, primitives );

	/* one rt copy per layer, before its first mesh (later meshes refract the same frame, never each other). no copy = own material.
	   ponytail: full rt per copy, 10 glass ghosts = 10 copies; copy the model's screen rect if it shows in perf */
	if ( this->m_copy_pending ) {
		this->m_copy_pending = false;
		g_frame              = copy_frame( device );
	}

	if ( !g_frame )
		return original( device, type, base_vertex, min_vertex, vertices, start_index, primitives );

	IDirect3DVertexShader9* old_vs = nullptr;
	IDirect3DPixelShader9* old_ps  = nullptr;
	device->GetVertexShader( &old_vs );
	device->GetPixelShader( &old_ps );

	const int bump     = bump_sampler( old_ps );
	const int vs_index = compressed ? 2 : has_tangent_stream( device ) ? 1 : 0;
	const int ps_index = bump == 1 ? 1 : bump == 3 ? 2 : 0;

	DWORD old_srgb = 0, old_zfunc = 0, old_zwrite = 0, old_alpha_test = 0, old_alpha_ref = 0, old_blend = 0, old_src = 0, old_dest = 0, old_color_write = 0;
	float old_ps_constants[ 3 ][ 4 ]{ };

	device->GetRenderState( D3DRS_SRGBWRITEENABLE, &old_srgb );
	device->GetRenderState( D3DRS_ZFUNC, &old_zfunc );
	device->GetRenderState( D3DRS_ZWRITEENABLE, &old_zwrite );
	device->GetRenderState( D3DRS_ALPHATESTENABLE, &old_alpha_test );
	device->GetRenderState( D3DRS_ALPHAREF, &old_alpha_ref );
	device->GetRenderState( D3DRS_ALPHABLENDENABLE, &old_blend );
	device->GetRenderState( D3DRS_SRCBLEND, &old_src );
	device->GetRenderState( D3DRS_DESTBLEND, &old_dest );
	device->GetRenderState( D3DRS_COLORWRITEENABLE, &old_color_write );

	const float ps_constants[ 3 ][ 4 ] = {
		{ this->m_tint[ 0 ], this->m_tint[ 1 ], this->m_tint[ 2 ], this->m_tint[ 3 ] },
		{ this->m_refract, this->m_bump, old_alpha_test ? 1.f : 0.f, static_cast< float >( old_alpha_ref & 0xFF ) / 255.f },
		{ 1.f / static_cast< float >( g_frame->m_desc.Width ), 1.f / static_cast< float >( g_frame->m_desc.Height ), 0.f, 0.f } };

	// raw bytes in, raw bytes out (srgb read + write off) = the copy lands exactly as it was drawn
	static constexpr D3DSAMPLERSTATETYPE samplers[] = { D3DSAMP_ADDRESSU,  D3DSAMP_ADDRESSV,    D3DSAMP_MAGFILTER,  D3DSAMP_MINFILTER,
		                                                D3DSAMP_MIPFILTER, D3DSAMP_SRGBTEXTURE, D3DSAMP_MAXMIPLEVEL };
	static constexpr DWORD sampler_values[]         = { D3DTADDRESS_CLAMP, D3DTADDRESS_CLAMP, D3DTEXF_LINEAR, D3DTEXF_LINEAR, D3DTEXF_NONE, FALSE, 0 };
	DWORD old_samplers[ std::size( samplers ) ]{ };
	IDirect3DBaseTexture9* old_texture = nullptr;

	static std::vector< saved_constant_t > saved_vs{ }, saved_ps{ };
	saved_vs.clear( );
	saved_ps.clear( );

	device->GetPixelShaderConstantF( 30, old_ps_constants[ 0 ], 3 );
	save_constants( device, true, g_vs_defs, saved_vs );
	save_constants( device, false, g_ps_defs, saved_ps );

	for ( std::size_t i = 0; i < std::size( samplers ); i++ ) {
		device->GetSamplerState( k_frame_stage, samplers[ i ], &old_samplers[ i ] );
		device->SetSamplerState( k_frame_stage, samplers[ i ], sampler_values[ i ] );
	}

	device->GetTexture( k_frame_stage, &old_texture );
	device->SetTexture( k_frame_stage, g_frame->m_texture );

	// tint is a display value: no linear -> srgb on write. alpha test = clip in our ps
	device->SetRenderState( D3DRS_SRGBWRITEENABLE, FALSE );
	device->SetRenderState( D3DRS_ALPHATESTENABLE, FALSE );

	if ( this->m_ignorez )
		device->SetRenderState( D3DRS_ZFUNC, D3DCMP_ALWAYS );
	else if ( this->m_translucent && ( old_zfunc == D3DCMP_LESS || old_zfunc == D3DCMP_GREATER ) )
		device->SetRenderState( D3DRS_ZFUNC, old_zfunc == D3DCMP_LESS ? D3DCMP_LESSEQUAL : D3DCMP_GREATEREQUAL );

	device->SetVertexShader( g_vs[ vs_index ] );
	device->SetPixelShader( g_ps[ ps_index ] );
	device->SetPixelShaderConstantF( 30, ps_constants[ 0 ], 3 );

	// opaque = own blend off whatever the material asked; translucent = depth only prepass first ( two tone ), then blend
	if ( this->m_translucent ) {
		if ( !this->m_ignorez && this->m_lay_depth ) {
			device->SetRenderState( D3DRS_COLORWRITEENABLE, 0 );
			device->SetRenderState( D3DRS_ZWRITEENABLE, TRUE );
			original( device, type, base_vertex, min_vertex, vertices, start_index, primitives );
			device->SetRenderState( D3DRS_COLORWRITEENABLE, old_color_write );
		}

		device->SetRenderState( D3DRS_ALPHABLENDENABLE, TRUE );
		device->SetRenderState( D3DRS_SRCBLEND, D3DBLEND_SRCALPHA );
		device->SetRenderState( D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA );
	} else
		device->SetRenderState( D3DRS_ALPHABLENDENABLE, FALSE );

	if ( this->m_ignorez || this->m_translucent )
		device->SetRenderState( D3DRS_ZWRITEENABLE, FALSE );

	const long result = original( device, type, base_vertex, min_vertex, vertices, start_index, primitives );

	// constants before shaders: the engine shader's own defs must win their slots
	device->SetRenderState( D3DRS_SRGBWRITEENABLE, old_srgb );
	device->SetRenderState( D3DRS_ZFUNC, old_zfunc );
	device->SetRenderState( D3DRS_ZWRITEENABLE, old_zwrite );
	device->SetRenderState( D3DRS_ALPHATESTENABLE, old_alpha_test );
	device->SetRenderState( D3DRS_ALPHABLENDENABLE, old_blend );
	device->SetRenderState( D3DRS_SRCBLEND, old_src );
	device->SetRenderState( D3DRS_DESTBLEND, old_dest );
	device->SetPixelShaderConstantF( 30, old_ps_constants[ 0 ], 3 );
	restore_constants( device, true, saved_vs );
	restore_constants( device, false, saved_ps );
	device->SetVertexShader( old_vs );
	device->SetPixelShader( old_ps );

	for ( std::size_t i = 0; i < std::size( samplers ); i++ )
		device->SetSamplerState( k_frame_stage, samplers[ i ], old_samplers[ i ] );
	device->SetTexture( k_frame_stage, old_texture );

	for ( IUnknown* held : std::initializer_list< IUnknown* >{ old_vs, old_ps, old_texture } )
		if ( held )
			held->Release( );

	return result;
}

void n_glass_chams::impl_t::on_reset( )
{
	for ( frame_t& frame : g_frames )
		release_frame( frame );

	g_frame       = nullptr;
	g_frame_clock = 0U;
	g_copy_failed = false;
}

void n_glass_chams::impl_t::on_device_lost( )
{
	release_gpu( );
	g_gpu_failed = false;
	this->on_reset( );
}
