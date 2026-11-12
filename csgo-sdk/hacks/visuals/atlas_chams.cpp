#include "atlas_chams.h"
#include "model_shader.h"
#include "screen/render_queue.h"
#include "../chams/chams.h"
#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"
#include "../../globals/globals.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <vector>

namespace
{
	using namespace n_model_shader;

	/* skinned position + normal (two tone's vs). c2 camera pos, c8 viewproj, c58 bones. out: world normal,
	   eye - world, bind pose position (Atlas "model") */
	constexpr const char k_vs[] = R"(
float4 cEyePos : register( c2 );
float4x4 cViewProj : register( c8 );
float4x3 cModel[ 53 ] : register( c58 );

struct VS_INPUT {
	float4 vPos : POSITION;
	float4 vBoneWeights : BLENDWEIGHT;
	float4 vBoneIndices : BLENDINDICES;
	float4 vNormal : NORMAL;
};

struct VS_OUTPUT {
	float4 vPos : POSITION;
	float3 vNormal : TEXCOORD1;
	float3 vEye : TEXCOORD2;
	float3 vModel : TEXCOORD3;
};

// common_vs_fxc.h _DecompressUByte4Normal
float3 decompress( float4 inputNormal )
{
	float2 ztSigns = ( inputNormal.xy - 128.0f ) < 0;
	float2 xyAbs   = abs( inputNormal.xy - 128.0f ) - ztSigns;
	float2 xySigns = ( xyAbs - 64.0f ) < 0;
	float3 n;
	n.xy  = ( abs( xyAbs - 64.0f ) - xySigns ) / 63.0f;
	n.z   = 1.0f - n.x - n.y;
	n     = normalize( n );
	n.xy *= 1 - 2 * xySigns;
	n.z  *= 1 - 2 * ztSigns.x;
	return n;
}

VS_OUTPUT main( const VS_INPUT i )
{
	float4 vWeights = i.vBoneWeights;
#if COMPRESSED_VERTS
	vWeights = ( vWeights + 1 ) / 32768;
	float3 normal = decompress( i.vNormal );
#else
	float3 normal = i.vNormal.xyz;
#endif
	int3 boneIndices = D3DCOLORtoUBYTE4( i.vBoneIndices ).xyz;
	float3 weights   = vWeights.xyz;
	weights[ 2 ]     = 1 - ( weights[ 0 ] + weights[ 1 ] );
	float4x3 blendMatrix = cModel[ boneIndices[ 0 ] ] * weights[ 0 ] + cModel[ boneIndices[ 1 ] ] * weights[ 1 ] + cModel[ boneIndices[ 2 ] ] * weights[ 2 ];

	float3 world = mul( i.vPos, blendMatrix );

	VS_OUTPUT o;
	o.vPos    = mul( float4( world, 1.0f ), cViewProj );
	o.vNormal = mul( normal, ( float3x3 )blendMatrix );
	o.vEye    = cEyePos.xyz - world;
	o.vModel  = i.vPos.xyz;
	return o;
}
)";

	/* Atlas-TF2 model.hlsl pixel shaders 1:1 + botox grid (24), MODE = n_chams slot (same numbers as botox-tf2 fx_cham_ps.hlsl).
	   c40 colour (display, alpha pre-scaled), c41 x = time */
	constexpr const char k_ps[] = R"(
float4 cColor : register( c40 );
float4 cTime : register( c41 );

struct PS_INPUT {
	float3 vNormal : TEXCOORD1;
	float3 vEye : TEXCOORD2;
	float3 vModel : TEXCOORD3;
};

// ---- Atlas helpers ----
float light( float3 normal, float facing )
{
	const float sky = lerp( 0.35f, 1.0f, normal.z * 0.5f + 0.5f );
	return saturate( sky * 0.75f + facing * 0.25f );
}

float hash( float3 position )
{
	position = frac( position * 0.1031f );
	position += dot( position, position.zyx + 31.32f );
	return frac( ( position.x + position.y ) * position.z );
}

float value_noise( float3 position )
{
	const float3 cell = floor( position );
	const float3 part = frac( position );
	const float3 ease = part * part * ( 3.0f - 2.0f * part );

	const float x00 = lerp( hash( cell ), hash( cell + float3( 1, 0, 0 ) ), ease.x );
	const float x10 = lerp( hash( cell + float3( 0, 1, 0 ) ), hash( cell + float3( 1, 1, 0 ) ), ease.x );
	const float x01 = lerp( hash( cell + float3( 0, 0, 1 ) ), hash( cell + float3( 1, 0, 1 ) ), ease.x );
	const float x11 = lerp( hash( cell + float3( 0, 1, 1 ) ), hash( cell + float3( 1, 1, 1 ) ), ease.x );

	return lerp( lerp( x00, x10, ease.y ), lerp( x01, x11, ease.y ), ease.z );
}

float3 hue( float value )
{
	return saturate( abs( frac( value + float3( 0.0f, 2.0f / 3.0f, 1.0f / 3.0f ) ) * 6.0f - 3.0f ) - 1.0f );
}

float3 ramp( float value )
{
	const float  position = saturate( value * 4.0f - 0.5f ) * 3.0f;
	const float3 black    = float3( 0.0f, 0.0f, 0.0f );
	const float3 cyan     = float3( 0.0f, 230.0f, 255.0f ) / 255.0f;
	const float3 yellow   = float3( 255.0f, 230.0f, 0.0f ) / 255.0f;
	const float3 magenta  = float3( 255.0f, 60.0f, 230.0f ) / 255.0f;

	return position < 1.0f ? lerp( black, cyan, position ) : position < 2.0f ? lerp( cyan, yellow, position - 1.0f ) : lerp( yellow, magenta, position - 2.0f );
}

void surface( float3 normal, float3 eye, out float3 unit, out float facing )
{
	unit   = normalize( normal );
	facing = saturate( dot( unit, normalize( eye ) ) );
}

float4 aero( float3 body, float3 glow, float rim )
{
	const float inner   = lerp( 0.15f, 1.0f, pow( rim, 1.6f ) );
	const float outline = smoothstep( 0.80f, 0.90f, rim );

	return float4( body * inner + glow + outline, cColor.a );
}

float4 main( PS_INPUT i ) : COLOR
{
	float3 unit;
	float  facing;
	surface( i.vNormal, i.vEye, unit, facing );
	const float3 model = i.vModel;
	const float3 eye   = i.vEye;
	const float  time  = cTime.x;

#if MODE == 14   // fresnel
	return float4( cColor.rgb * 5.5f * light( unit, facing ) * ( 1.0f - pow( facing, 0.75f ) ), cColor.a );
#elif MODE == 15   // lava
	const float  scale  = 0.75f + 0.22f * sin( time * ( 6.2831853f / 12.7f ) );
	const float3 flow   = model * ( 0.09f / scale ) - float3( 0.0f, 0.0f, time * 0.6f );
	const float  heat   = saturate( value_noise( flow ) * 0.65f + value_noise( flow * 2.3f + 17.0f ) * 0.35f );
	const float  blend  = 0.78f + 0.17f * sin( time * ( 6.2831853f / 6.1f ) ) + 0.05f * sin( time * ( 6.2831853f / 2.3f ) );
	const float3 albedo = cColor.rgb * lerp( 1.0f, heat * 2.0f, blend ) + pow( heat, 6.0f ) * 0.5f;
	const float  mask   = saturate( facing * 1.25f - 0.25f );
	return float4( lerp( albedo * light( unit, facing ), albedo, mask ), cColor.a );
#elif MODE == 16   // pearl
	const float  shift      = ( 1.0f - facing ) * 0.8f + value_noise( model * 0.05f ) * 0.6f + time * 0.05f;
	const float3 iridescent = lerp( float3( 1.0f, 1.0f, 1.0f ), hue( shift ), 0.55f );
	const float3 reflected  = reflect( -normalize( eye ), unit );
	const float  sheen      = 0.2f + pow( saturate( reflected.z ), 24.0f ) * 0.8f + pow( facing, 48.0f ) * 0.5f;
	return float4( iridescent * cColor.rgb + sheen * 0.6f, cColor.a );
#elif MODE == 17   // bubble
	const float3 key = normalize( normalize( eye ) + float3( 0.0f, 0.0f, 0.5f ) );
	return float4( cColor.rgb * 2.0f * ramp( saturate( dot( unit, key ) ) ), cColor.a );
#elif MODE == 18   // neon (Atlas "lsd")
	const float  rim  = 1.0f - facing;
	const float  edge = rim * rim * rim;
	const float3 neon = hue( rim * 0.6f + ( model.z + model.x * 0.5f ) * ( 1.0f / 64.0f ) - time * 0.2f );
	return float4( cColor.rgb * ( 0.2f + rim * 0.3f ) + neon * ( 0.1f + edge * 1.5f ) + edge * edge * 0.4f, cColor.a );
#elif MODE == 19   // network
	const float  cell_size = 18.0f;
	const float3 position  = model / cell_size;
	const float3 corner    = floor( position - 0.5f );
	float nearest = 2.0f;
	[unroll] for ( int x = 0; x < 2; ++x ) {
		[unroll] for ( int y = 0; y < 2; ++y ) {
			[unroll] for ( int z = 0; z < 2; ++z ) {
				const float3 cell  = corner + float3( x, y, z );
				const float  seed  = hash( cell );
				const float3 phase = frac( seed * float3( 1.0f, 7.31f, 13.7f ) ) * 6.2831853f;
				const float3 node  = cell + 0.5f + 0.4f * sin( time * ( 0.6f + seed * 0.5f ) + phase );
				nearest = min( nearest, length( position - node ) );
			}
		}
	}
	const float link = pow( saturate( 1.0f - nearest / 0.6f ), 2.0f );
	return float4( cColor.rgb * ( 0.45f + link * 1.2f ), cColor.a * saturate( 0.2f + link ) );
#elif MODE == 20   // soap
	const float  rim       = 1.0f - facing;
	const float3 view      = normalize( eye );
	const float3 marble    = model * 0.035f;
	const float3 warp      = float3( value_noise( marble + float3( 0.0f, time * 0.12f, 0.0f ) ), value_noise( marble + float3( 5.2f, 1.3f, time * 0.1f ) ), 0.0f );
	const float  swirl     = value_noise( marble * 1.7f + warp * 2.5f + float3( 0.0f, time * 0.18f, 0.0f ) );
	const float  thickness = swirl * 1.4f + model.y * 0.012f + rim * 0.5f - time * 0.05f;
	const float3 film      = 0.5f + 0.5f * cos( 6.2831853f * ( thickness * float3( 1.0f, 1.15f, 1.3f ) + float3( 0.0f, 0.3f, 0.6f ) ) );
	const float  window    = smoothstep( 0.90f, 0.92f, dot( unit, normalize( view + float3( 0.0f, 0.0f, 1.1f ) ) ) );
	const float  bounce    = smoothstep( 0.95f, 0.97f, dot( unit, normalize( view - float3( 0.0f, 0.0f, 0.9f ) ) ) ) * 0.6f;
	return aero( lerp( float3( 0.85f, 0.92f, 1.0f ), film, 0.75f ) * cColor.rgb * 1.2f, window + bounce, rim );
#elif MODE == 21   // aurora
	const float  warp  = value_noise( model * 0.03f + float3( 0.0f, time * 0.08f, 0.0f ) ) * 3.0f;
	const float  green = pow( 1.0f - abs( sin( dot( model, float3( 0.04f, 0.02f, 0.07f ) ) + warp - time * 0.7f ) ), 24.0f );
	const float  blue  = pow( 1.0f - abs( sin( dot( model, float3( -0.03f, 0.05f, 0.06f ) ) + warp * 1.3f - time * 0.5f + 1.7f ) ), 30.0f );
	const float3 glow  = ( green * float3( 0.35f, 1.0f, 0.55f ) + blue * float3( 0.30f, 0.70f, 1.0f ) ) * cColor.rgb;
	return aero( float3( 0.05f, 0.35f, 0.75f ) * cColor.rgb, glow, 1.0f - facing );
#elif MODE == 22   // aquarium
	const float3 water   = model * 0.06f + float3( time * 0.2f, 0.0f, time * 0.15f );
	const float  caustic = pow( saturate( 1.0f - abs( value_noise( water ) - value_noise( water * 1.4f + float3( 5.2f, 1.3f, -time * 0.25f ) ) ) * 3.0f ), 10.0f );
	const float  shaft   = pow( 0.5f + 0.5f * sin( model.x * 0.18f + model.z * 0.11f + value_noise( model * 0.02f + float3( 0.0f, 0.0f, time * 0.1f ) ) * 4.0f + time * 0.6f ), 6.0f ) *
	                       smoothstep( 10.0f, 80.0f, model.y );
	const float3 body    = lerp( float3( 0.0f, 0.10f, 0.45f ), float3( 0.05f, 0.45f, 1.0f ), unit.z * 0.5f + 0.5f ) * cColor.rgb * 1.3f;
	const float3 glow    = ( caustic * float3( 0.44f, 0.68f, 0.8f ) + shaft * float3( 0.075f, 0.225f, 0.5f ) ) * cColor.rgb;
	return aero( body, glow, 1.0f - facing );
#elif MODE == 23   // sky
	const float  height = model.y + ( value_noise( model * 0.08f ) - 0.5f ) * 6.0f;
	const float  above  = smoothstep( 30.0f, 34.0f, height );
	const float  cloud  = value_noise( model * 0.07f + float3( time * 0.8f, 0.0f, 0.0f ) ) * 0.65f +
	                      value_noise( model * 0.16f + float3( time * 1.3f, 3.0f, 0.0f ) ) * 0.35f;
	const float3 air    = lerp( float3( 0.45f, 0.78f, 1.0f ), float3( 0.04f, 0.32f, 0.92f ), smoothstep( 30.0f, 85.0f, height ) );
	const float3 grass  = lerp( float3( 0.10f, 0.45f, 0.05f ), float3( 0.35f, 0.80f, 0.15f ), value_noise( model * 0.35f ) );
	const float  gloss  = smoothstep( 0.80f, 0.88f, dot( unit, normalize( normalize( eye ) + float3( 0.0f, 0.0f, 1.1f ) ) ) ) * 0.3f;
	const float  glow   = smoothstep( 0.55f, 0.72f, cloud ) * above * 0.45f + gloss;
	return aero( lerp( grass, air, above ) * cColor.rgb * 1.2f, glow, 1.0f - facing );
#elif MODE == 24   // grid: 4 unit bind pose cells (position, not uv: ride the bones), 3 plane families = rings + stripes on every limb
	const float3 p     = model / 4.0f;
	const float3 dist  = abs( frac( p - 0.5f ) - 0.5f ) / max( fwidth( p ) * 1.5f, 1e-5f );   // 1.5 px lines at any angle / range
	const float3 flat_ = pow( abs( unit ), 16.0f ) * 4.0f;                                     // only a face lying IN a plane is muted (would light whole)
	const float  line_ = saturate( 1.0f - min( min( dist.x + flat_.x, dist.y + flat_.y ), dist.z + flat_.z ) );
	return float4( cColor.rgb, cColor.a * line_ );
#else
	return cColor;
#endif
}
)";

	constexpr int k_first = n_chams::chams_material_fresnel;
	constexpr int k_count = n_chams::chams_material_max - k_first;

	IDirect3DVertexShader9* g_vs[ 2 ]{ };
	IDirect3DPixelShader9* g_ps[ k_count ]{ };
	defs_t g_vs_defs{ }, g_ps_defs{ };
	bool g_gpu_failed = false;

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

		const D3DXMACRO vs_macros[ 2 ][ 2 ] = { { { "COMPRESSED_VERTS", "0" }, { nullptr, nullptr } }, { { "COMPRESSED_VERTS", "1" }, { nullptr, nullptr } } };

		bool ok = true;

		for ( int i = 0; i < 2 && ok; i++ ) {
			ID3DXBuffer* code = compile( "ATL", k_vs, "vs_3_0", vs_macros[ i ] );
			ok = code && SUCCEEDED( device->CreateVertexShader( static_cast< const DWORD* >( code->GetBufferPointer( ) ), &g_vs[ i ] ) );
			if ( ok )
				collect_defs( static_cast< const DWORD* >( code->GetBufferPointer( ) ), g_vs_defs );
			if ( code )
				code->Release( );
		}

		// ps defs: union of every mode's
		for ( int i = 0; i < k_count && ok; i++ ) {
			char mode[ 8 ]{ };
			std::snprintf( mode, sizeof( mode ), "%d", k_first + i );
			const D3DXMACRO ps_macros[ 2 ] = { { "MODE", mode }, { nullptr, nullptr } };

			ID3DXBuffer* code = compile( "ATL", k_ps, "ps_3_0", ps_macros );
			ok = code && SUCCEEDED( device->CreatePixelShader( static_cast< const DWORD* >( code->GetBufferPointer( ) ), &g_ps[ i ] ) );
			if ( ok )
				collect_defs( static_cast< const DWORD* >( code->GetBufferPointer( ) ), g_ps_defs );
			if ( code )
				code->Release( );
		}

		if ( !ok ) {
			botox_dbg_log( "ATL: gpu FAILED" );
			release_gpu( );
			g_gpu_failed = true;
		}

		return ok;
	}

	// Atlas model_render.hpp surface_t::additive
	bool additive( const int slot )
	{
		using namespace n_chams;
		return slot == chams_material_fresnel || slot == chams_material_neon || slot == chams_material_soap || slot == chams_material_aurora ||
		       slot == chams_material_aquarium || slot == chams_material_sky;
	}
}

void n_atlas_chams::impl_t::begin( const int slot, const float color[ 4 ], const bool ignorez )
{
	// Atlas TIME_WRAP 1024: float precision stays fine
	const float time = g_interfaces.m_global_vars_base ? std::fmod( g_interfaces.m_global_vars_base->m_real_time, 1024.f ) : 0.f;

	auto arm = [ slot, c = std::to_array( { color[ 0 ], color[ 1 ], color[ 2 ], color[ 3 ] } ), time, ignorez ] {
		for ( int i = 0; i < 4; i++ )
			g_atlas_chams.m_color[ i ] = c[ i ];

		g_atlas_chams.m_slot    = slot;
		g_atlas_chams.m_time    = time;
		g_atlas_chams.m_ignorez = ignorez;
		g_atlas_chams.m_armed.store( true, std::memory_order_relaxed );
	};

	if ( !n_render_queue::submit( arm ) )
		arm( );
}

void n_atlas_chams::impl_t::end( )
{
	auto disarm = [ ] { g_atlas_chams.m_armed.store( false, std::memory_order_relaxed ); };

	if ( !n_render_queue::submit( disarm ) )
		disarm( );
}

long n_atlas_chams::impl_t::draw( const draw_indexed_primitive_t original, IDirect3DDevice9* device, const D3DPRIMITIVETYPE type, const int base_vertex,
                                  const unsigned int min_vertex, const unsigned int vertices, const unsigned int start_index, const unsigned int primitives )
{
	// no shader / not a studio mesh = the carrier draws as plain white vertexlit
	const int compressed = gpu_ready( device ) ? vertex_compression( device ) : -1;
	const int mode       = this->m_slot - k_first;
	if ( compressed < 0 || mode < 0 || mode >= k_count )
		return original( device, type, base_vertex, min_vertex, vertices, start_index, primitives );

	const bool add    = additive( this->m_slot );
	const bool blends = add || this->m_color[ 3 ] < 1.f || this->m_slot == n_chams::chams_material_network || this->m_slot == n_chams::chams_material_grid;

	IDirect3DVertexShader9* old_vs = nullptr;
	IDirect3DPixelShader9* old_ps  = nullptr;
	device->GetVertexShader( &old_vs );
	device->GetPixelShader( &old_ps );

	DWORD old_srgb = 0, old_zfunc = 0, old_zwrite = 0, old_alpha_test = 0, old_blend = 0, old_src = 0, old_dest = 0, old_color_write = 0;
	device->GetRenderState( D3DRS_SRGBWRITEENABLE, &old_srgb );
	device->GetRenderState( D3DRS_ZFUNC, &old_zfunc );
	device->GetRenderState( D3DRS_ZWRITEENABLE, &old_zwrite );
	device->GetRenderState( D3DRS_ALPHATESTENABLE, &old_alpha_test );
	device->GetRenderState( D3DRS_ALPHABLENDENABLE, &old_blend );
	device->GetRenderState( D3DRS_SRCBLEND, &old_src );
	device->GetRenderState( D3DRS_DESTBLEND, &old_dest );
	device->GetRenderState( D3DRS_COLORWRITEENABLE, &old_color_write );

	const float ps_constants[ 2 ][ 4 ] = { { this->m_color[ 0 ], this->m_color[ 1 ], this->m_color[ 2 ], this->m_color[ 3 ] }, { this->m_time, 0.f, 0.f, 0.f } };
	float old_ps_constants[ 2 ][ 4 ]{ };

	static std::vector< saved_constant_t > saved_vs{ }, saved_ps{ };
	saved_vs.clear( );
	saved_ps.clear( );

	device->GetPixelShaderConstantF( 40, old_ps_constants[ 0 ], 2 );
	save_constants( device, true, g_vs_defs, saved_vs );
	save_constants( device, false, g_ps_defs, saved_ps );

	// picked colours are display values (Atlas: srgb write off)
	device->SetRenderState( D3DRS_SRGBWRITEENABLE, FALSE );
	device->SetRenderState( D3DRS_ALPHATESTENABLE, FALSE );

	if ( this->m_ignorez )
		device->SetRenderState( D3DRS_ZFUNC, D3DCMP_ALWAYS );
	else if ( blends && ( old_zfunc == D3DCMP_LESS || old_zfunc == D3DCMP_GREATER ) )
		device->SetRenderState( D3DRS_ZFUNC, old_zfunc == D3DCMP_LESS ? D3DCMP_LESSEQUAL : D3DCMP_GREATEREQUAL );

	device->SetVertexShader( g_vs[ compressed ] );
	device->SetPixelShader( g_ps[ mode ] );
	device->SetPixelShaderConstantF( 40, ps_constants[ 0 ], 2 );

	// Atlas draw_depth_first: blended = depth only prepass, then colour on the nearest surface only
	if ( blends ) {
		if ( !this->m_ignorez ) {
			device->SetRenderState( D3DRS_COLORWRITEENABLE, 0 );
			device->SetRenderState( D3DRS_ZWRITEENABLE, TRUE );
			original( device, type, base_vertex, min_vertex, vertices, start_index, primitives );
			device->SetRenderState( D3DRS_COLORWRITEENABLE, old_color_write );
		}

		device->SetRenderState( D3DRS_ALPHABLENDENABLE, TRUE );
		device->SetRenderState( D3DRS_SRCBLEND, D3DBLEND_SRCALPHA );
		device->SetRenderState( D3DRS_DESTBLEND, add ? D3DBLEND_ONE : D3DBLEND_INVSRCALPHA );
	} else
		device->SetRenderState( D3DRS_ALPHABLENDENABLE, FALSE );

	if ( this->m_ignorez || blends )
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
	device->SetPixelShaderConstantF( 40, old_ps_constants[ 0 ], 2 );
	restore_constants( device, true, saved_vs );
	restore_constants( device, false, saved_ps );
	device->SetVertexShader( old_vs );
	device->SetPixelShader( old_ps );

	for ( IUnknown* held : std::initializer_list< IUnknown* >{ old_vs, old_ps } )
		if ( held )
			held->Release( );

	return result;
}

void n_atlas_chams::impl_t::on_device_lost( )
{
	release_gpu( );
	g_gpu_failed = false;
}
