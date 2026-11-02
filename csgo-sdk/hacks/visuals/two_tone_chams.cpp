#include "two_tone_chams.h"
#include "model_shader.h"
#include "screen/render_queue.h"
#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"
#include "../../globals/config/variables.h"
#include "../../globals/globals.h"

#include <algorithm>
#include <array>
#include <initializer_list>
#include <unordered_map>
#include <vector>

namespace
{
	using namespace n_model_shader;

	/* skinned position + normal + tangent. engine constants left by the model's own shader: c2 camera pos
	   (IShader.h:126 VERTEX_SHADER_CAMERA_POS), c8 viewproj, c58 bones */
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

	VS_OUTPUT o;
	o.vPos     = mul( float4( world, 1.0f ), cViewProj );
	o.vUv      = i.vTexCoord0.xy;
	o.vNormal  = mul( normal, ( float3x3 )blendMatrix );
	o.vTangent = float4( mul( tangent.xyz, ( float3x3 )blendMatrix ), tangent.w );
	o.vView    = cEyePos.xyz - world;
	return o;
}
)";

	/* light = the camera: lit where the surface (normal map included) faces the eye.
	   c30 light colour, c31 dark colour, c32 threshold / edge / alpha test on / alpha ref */
	constexpr const char k_ps[] = R"(
const float4 cLightColor : register( c30 );
const float4 cDarkColor : register( c31 );
const float4 cParams : register( c32 );

sampler BaseSampler : register( s0 );
#if BUMP_SAMPLER == 1
sampler NormalSampler : register( s1 );
#elif BUMP_SAMPLER == 3
sampler NormalSampler : register( s3 );
#endif

struct PS_INPUT {
	float2 vUv : TEXCOORD0;
	float3 vNormal : TEXCOORD1;
	float4 vTangent : TEXCOORD2;
	float3 vView : TEXCOORD3;
};

float4 main( PS_INPUT i ) : COLOR
{
	// material alpha test done here: picked alpha stays out of the cutout (hair, straps keep shape when translucent)
	clip( cParams.z * ( tex2D( BaseSampler, i.vUv ).a - cParams.w ) );

	float3 n = normalize( i.vNormal );
#if BUMP_SAMPLER
	// phong_ps20b.fxc:502-553
	float3 t  = normalize( i.vTangent.xyz );
	float3 b  = cross( n, t ) * i.vTangent.w;
	float3 tn = tex2D( NormalSampler, i.vUv ).xyz * 2.0f - 1.0f;
	n = normalize( tn.x * t + tn.y * b + tn.z * n );
#endif

	// fwidth floor = 1 px anti aliased line at edge 0
	// ^1.5 = more dark: threshold 0.5 cuts at 51 deg off view (was 60)
	float level = saturate( dot( n, normalize( i.vView ) ) );
	level *= sqrt( level );
	float width = max( max( cParams.y, fwidth( level ) ), 0.0001f );
	return lerp( cDarkColor, cLightColor, smoothstep( cParams.x - width, cParams.x + width, level ) );
}
)";

	// vs: plain, plain + tangent stream, compressed. ps: no normal map, normal map in s1 (character), s3 (phong / bump)
	IDirect3DVertexShader9* g_vs[ 3 ]{ };
	IDirect3DPixelShader9* g_ps[ 3 ]{ };
	defs_t g_vs_defs{ }, g_ps_defs{ };
	bool g_gpu_failed = false;

	std::unordered_map< IDirect3DPixelShader9*, int > g_bump_samplers{ };
	int g_logged = 0;

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
		g_bump_samplers.clear( );
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
			ID3DXBuffer* code = compile( "TWO", k_vs, "vs_3_0", vs_macros[ i ] );
			ok = code && SUCCEEDED( device->CreateVertexShader( static_cast< const DWORD* >( code->GetBufferPointer( ) ), &g_vs[ i ] ) );
			if ( ok )
				collect_defs( static_cast< const DWORD* >( code->GetBufferPointer( ) ), g_vs_defs );
			if ( code )
				code->Release( );
		}

		for ( int i = 0; i < 3 && ok; i++ ) {
			ID3DXBuffer* code = compile( "TWO", k_ps, "ps_3_0", ps_macros[ i ] );
			ok = code && SUCCEEDED( device->CreatePixelShader( static_cast< const DWORD* >( code->GetBufferPointer( ) ), &g_ps[ i ] ) );
			if ( ok )
				collect_defs( static_cast< const DWORD* >( code->GetBufferPointer( ) ), g_ps_defs );
			if ( code )
				code->Release( );
		}

		if ( !ok ) {
			botox_dbg_log( "TWO: gpu FAILED" );
			release_gpu( );
			g_gpu_failed = true;
		}

		return ok;
	}

	bool has_tangent_stream( IDirect3DDevice9* device )
	{
		IDirect3DVertexDeclaration9* declaration = nullptr;
		if ( FAILED( device->GetVertexDeclaration( &declaration ) ) || !declaration )
			return false;

		D3DVERTEXELEMENT9 elements[ MAXD3DDECLLENGTH + 1 ]{ };
		UINT count  = 0;
		bool result = false;

		if ( SUCCEEDED( declaration->GetDeclaration( nullptr, &count ) ) && count <= std::size( elements ) &&
		     SUCCEEDED( declaration->GetDeclaration( elements, &count ) ) ) {
			for ( UINT i = 0; i < count && elements[ i ].Stream != 0xFF; i++ )
				result |= elements[ i ].Usage == D3DDECLUSAGE_TANGENT && elements[ i ].UsageIndex == 0;
		}

		declaration->Release( );
		return result;
	}

	/* normal map register of the engine pixel shader, by name from its constant table (phong / character
	   "NormalMapSampler", bumped vertexlit "BumpmapSampler"). -1 none, -2 no table. cached per shader */
	int bump_sampler( IDirect3DPixelShader9* shader )
	{
		if ( !shader )
			return -1;

		if ( const auto found = g_bump_samplers.find( shader ); found != g_bump_samplers.end( ) )
			return found->second;

		int result = -2;
		UINT size  = 0;

		if ( SUCCEEDED( shader->GetFunction( nullptr, &size ) ) && size ) {
			std::vector< DWORD > code( ( size + 3 ) / 4 );

			ID3DXConstantTable* table = nullptr;
			if ( SUCCEEDED( shader->GetFunction( code.data( ), &size ) ) && SUCCEEDED( D3DXGetShaderConstantTable( code.data( ), &table ) ) && table ) {
				result = -1;

				for ( const char* name : { "NormalMapSampler", "BumpmapSampler" } ) {
					const D3DXHANDLE handle = table->GetConstantByName( nullptr, name );
					D3DXCONSTANT_DESC desc{ };
					UINT descs = 1;

					if ( handle && SUCCEEDED( table->GetConstantDesc( handle, &desc, &descs ) ) && desc.RegisterSet == D3DXRS_SAMPLER ) {
						result = static_cast< int >( desc.RegisterIndex );
						break;
					}
				}

				table->Release( );
			}
		}

		if ( g_logged < 32 ) {
			g_logged++;
			botox_dbg_log( "TWO: ps %p bump %d (-1 none, -2 no ctab)", static_cast< void* >( shader ), result );
		}

		g_bump_samplers.emplace( shader, result );
		return result;
	}
}

void n_two_tone_chams::impl_t::begin( const float light[ 4 ], const float dark[ 4 ], const bool ignorez )
{
	const float threshold = GET_VARIABLE( g_variables.m_chams_two_tone_threshold, float );
	const float edge      = GET_VARIABLE( g_variables.m_chams_two_tone_edge, float );
	const bool translucent = light[ 3 ] < 1.f || dark[ 3 ] < 1.f;

	auto arm = [ l = std::to_array( { light[ 0 ], light[ 1 ], light[ 2 ], light[ 3 ] } ), d = std::to_array( { dark[ 0 ], dark[ 1 ], dark[ 2 ], dark[ 3 ] } ),
	             threshold, edge, ignorez, translucent ] {
		for ( int i = 0; i < 4; i++ ) {
			g_two_tone_chams.m_light[ i ] = l[ i ];
			g_two_tone_chams.m_dark[ i ]  = d[ i ];
		}

		g_two_tone_chams.m_params[ 0 ] = threshold;
		g_two_tone_chams.m_params[ 1 ] = edge;
		g_two_tone_chams.m_ignorez     = ignorez;
		g_two_tone_chams.m_translucent = translucent;
		g_two_tone_chams.m_armed.store( true, std::memory_order_relaxed );
	};

	if ( !n_render_queue::submit( arm ) )
		arm( );
}

void n_two_tone_chams::impl_t::end( )
{
	auto disarm = [ ] { g_two_tone_chams.m_armed.store( false, std::memory_order_relaxed ); };

	if ( !n_render_queue::submit( disarm ) )
		disarm( );
}

long n_two_tone_chams::impl_t::draw( const draw_indexed_primitive_t original, IDirect3DDevice9* device, const D3DPRIMITIVETYPE type, const int base_vertex,
                                     const unsigned int min_vertex, const unsigned int vertices, const unsigned int start_index, const unsigned int primitives )
{
	// no shader / not a studio mesh = the model's own material draws as is
	const int compressed = gpu_ready( device ) ? vertex_compression( device ) : -1;
	if ( compressed < 0 )
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

	const float ps_constants[ 3 ][ 4 ] = { { this->m_light[ 0 ], this->m_light[ 1 ], this->m_light[ 2 ], this->m_light[ 3 ] },
		                                   { this->m_dark[ 0 ], this->m_dark[ 1 ], this->m_dark[ 2 ], this->m_dark[ 3 ] },
		                                   { this->m_params[ 0 ], this->m_params[ 1 ], old_alpha_test ? 1.f : 0.f, static_cast< float >( old_alpha_ref & 0xFF ) / 255.f } };

	static std::vector< saved_constant_t > saved_vs{ }, saved_ps{ };
	saved_vs.clear( );
	saved_ps.clear( );

	device->GetPixelShaderConstantF( 30, old_ps_constants[ 0 ], 3 );
	save_constants( device, true, g_vs_defs, saved_vs );
	save_constants( device, false, g_ps_defs, saved_ps );

	// picked colours are display values: no linear -> srgb on write. alpha test = clip in our ps
	device->SetRenderState( D3DRS_SRGBWRITEENABLE, FALSE );
	device->SetRenderState( D3DRS_ALPHATESTENABLE, FALSE );

	// prepass lays depth, colour pass then only hits each pixel's nearest surface (equal depth must pass)
	if ( this->m_ignorez )
		device->SetRenderState( D3DRS_ZFUNC, D3DCMP_ALWAYS );
	else if ( this->m_translucent && ( old_zfunc == D3DCMP_LESS || old_zfunc == D3DCMP_GREATER ) )
		device->SetRenderState( D3DRS_ZFUNC, old_zfunc == D3DCMP_LESS ? D3DCMP_LESSEQUAL : D3DCMP_GREATEREQUAL );

	device->SetVertexShader( g_vs[ vs_index ] );
	device->SetPixelShader( g_ps[ ps_index ] );
	device->SetPixelShaderConstantF( 30, ps_constants[ 0 ], 3 );

	/* translucent: own blend whatever the material asked (additive, opaque snapshot). not ignorez = depth only
	   prepass first so inner parts (far arm, legs, gun) never show through. ignorez can't: depth there is the world */
	if ( this->m_translucent ) {
		if ( !this->m_ignorez ) {
			device->SetRenderState( D3DRS_COLORWRITEENABLE, 0 );
			device->SetRenderState( D3DRS_ZWRITEENABLE, TRUE );
			original( device, type, base_vertex, min_vertex, vertices, start_index, primitives );
			device->SetRenderState( D3DRS_COLORWRITEENABLE, old_color_write );
		}

		device->SetRenderState( D3DRS_ALPHABLENDENABLE, TRUE );
		device->SetRenderState( D3DRS_SRCBLEND, D3DBLEND_SRCALPHA );
		device->SetRenderState( D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA );
	}

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

	for ( IUnknown* held : std::initializer_list< IUnknown* >{ old_vs, old_ps } )
		if ( held )
			held->Release( );

	return result;
}

void n_two_tone_chams::impl_t::on_device_lost( )
{
	release_gpu( );
	g_gpu_failed = false;
	g_logged     = 0;
}
