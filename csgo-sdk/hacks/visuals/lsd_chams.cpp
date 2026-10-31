#include "lsd_chams.h"
#include "model_shader.h"
#include "screen/render_queue.h"
#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"
#include "../../globals/globals.h"

#include <initializer_list>
#include <vector>

namespace
{
	using namespace n_model_shader;

	// skinned studio position only, same engine constants as weapon_sheen's vs
	constexpr const char k_vs[] = R"(
float4x4 cViewProj : register( c8 );
float4x3 cModel[ 53 ] : register( c58 );

struct VS_INPUT {
	float4 vPos : POSITION;
	float4 vBoneWeights : BLENDWEIGHT;
	float4 vBoneIndices : BLENDINDICES;
};

float4 main( const VS_INPUT i ) : POSITION
{
	float4 vWeights = i.vBoneWeights;
#if COMPRESSED_VERTS
	vWeights = ( vWeights + 1 ) / 32768;
#endif
	int3 boneIndices = D3DCOLORtoUBYTE4( i.vBoneIndices ).xyz;
	float3 weights   = vWeights.xyz;
	weights[ 2 ]     = 1 - ( weights[ 0 ] + weights[ 1 ] );
	float4x3 blendMatrix = cModel[ boneIndices[ 0 ] ] * weights[ 0 ] + cModel[ boneIndices[ 1 ] ] * weights[ 1 ] + cModel[ boneIndices[ 2 ] ] * weights[ 2 ];

	return mul( float4( mul( i.vPos, blendMatrix ), 1.0f ), cViewProj );
}
)";

	// shadertoy "70s Melt" 1:1, fragCoord from VPOS (gl origin = bottom left of the viewport)
	constexpr const char k_ps[] = R"(
float4 cLsd : register( c20 );
float4 cViewport : register( c21 );

float cosRange( float amt, float range, float minimum )
{
	return ( ( ( 1.0 + cos( radians( amt ) ) ) * 0.5 ) * range ) + minimum;
}

float4 main( float2 vpos : VPOS ) : COLOR
{
	const float brightness = 0.975;
	float time   = cLsd.x * 1.25;
	float2 res   = cViewport.zw;
	float2 frag  = float2( vpos.x - cViewport.x + 0.5, res.y - ( vpos.y - cViewport.y + 0.5 ) );
	float2 p     = ( 2.0 * frag - res ) / max( res.x, res.y );
	float ct     = cosRange( time * 5.0, 3.0, 1.1 );
	float xBoost = cosRange( time * 0.2, 5.0, 5.0 );
	float yBoost = cosRange( time * 0.1, 10.0, 5.0 );
	float fScale = cosRange( time * 15.5, 1.25, 0.5 );

	[loop] for ( int i = 1; i < 40; i++ ) {
		float _i    = float( i );
		float2 newp = p;
		newp.x += 0.25 / _i * sin( _i * p.y + time * cos( ct ) * 0.5 / 20.0 + 0.005 * _i ) * fScale + xBoost;
		newp.y += 0.25 / _i * sin( _i * p.x + time * ct * 0.3 / 40.0 + 0.03 * float( i + 15 ) ) * fScale + yBoost;
		p = newp;
	}

	float3 col = float3( 0.5 * sin( 3.0 * p.x ) + 0.5, 0.5 * sin( 3.0 * p.y ) + 0.5, sin( p.x + p.y ) ) * brightness;
	return float4( saturate( col ), cLsd.y );
}
)";

	IDirect3DVertexShader9* g_vs[ 2 ]{ };
	IDirect3DPixelShader9* g_ps = nullptr;
	defs_t g_vs_defs{ }, g_ps_defs{ };
	bool g_gpu_failed = false;

	void release_gpu( )
	{
		for ( IDirect3DVertexShader9*& shader : g_vs ) {
			if ( shader )
				shader->Release( );
			shader = nullptr;
		}

		if ( g_ps )
			g_ps->Release( );
		g_ps = nullptr;

		g_vs_defs.clear( );
		g_ps_defs.clear( );
	}

	bool gpu_ready( IDirect3DDevice9* device )
	{
		if ( g_ps )
			return true;

		if ( g_gpu_failed )
			return false;

		const D3DXMACRO plain[]      = { { "COMPRESSED_VERTS", "0" }, { nullptr, nullptr } };
		const D3DXMACRO compressed[] = { { "COMPRESSED_VERTS", "1" }, { nullptr, nullptr } };

		ID3DXBuffer* code[ 3 ] = { compile( "LSD", k_vs, "vs_3_0", plain ), compile( "LSD", k_vs, "vs_3_0", compressed ), compile( "LSD", k_ps, "ps_3_0", nullptr ) };

		bool ok = code[ 0 ] && code[ 1 ] && code[ 2 ];
		for ( int i = 0; i < 2 && ok; i++ ) {
			ok = SUCCEEDED( device->CreateVertexShader( static_cast< const DWORD* >( code[ i ]->GetBufferPointer( ) ), &g_vs[ i ] ) );
			if ( ok )
				collect_defs( static_cast< const DWORD* >( code[ i ]->GetBufferPointer( ) ), g_vs_defs );
		}

		if ( ok ) {
			ok = SUCCEEDED( device->CreatePixelShader( static_cast< const DWORD* >( code[ 2 ]->GetBufferPointer( ) ), &g_ps ) );
			if ( ok )
				collect_defs( static_cast< const DWORD* >( code[ 2 ]->GetBufferPointer( ) ), g_ps_defs );
		}

		for ( ID3DXBuffer* buffer : code )
			if ( buffer )
				buffer->Release( );

		if ( !ok ) {
			botox_dbg_log( "LSD: gpu FAILED" );
			release_gpu( );
			g_gpu_failed = true;
		}

		return ok;
	}
}

void n_lsd_chams::impl_t::begin( const float alpha )
{
	if ( !g_interfaces.m_global_vars_base )
		return;

	const float time = g_interfaces.m_global_vars_base->m_real_time;

	auto arm = [ time, alpha ] {
		g_lsd_chams.m_params[ 0 ] = time;
		g_lsd_chams.m_params[ 1 ] = alpha;
		g_lsd_chams.m_armed.store( true, std::memory_order_relaxed );
	};

	if ( !n_render_queue::submit( arm ) )
		arm( );
}

void n_lsd_chams::impl_t::end( )
{
	auto disarm = [ ] { g_lsd_chams.m_armed.store( false, std::memory_order_relaxed ); };

	if ( !n_render_queue::submit( disarm ) )
		disarm( );
}

long n_lsd_chams::impl_t::draw( const draw_indexed_primitive_t original, IDirect3DDevice9* device, const D3DPRIMITIVETYPE type, const int base_vertex,
                                const unsigned int min_vertex, const unsigned int vertices, const unsigned int start_index, const unsigned int primitives )
{
	// no shader = the carrier draws as plain flat chams
	const int compressed = gpu_ready( device ) ? vertex_compression( device ) : -1;
	if ( compressed < 0 )
		return original( device, type, base_vertex, min_vertex, vertices, start_index, primitives );

	D3DVIEWPORT9 viewport{ };
	device->GetViewport( &viewport );

	const float constants[ 2 ][ 4 ] = { { this->m_params[ 0 ], this->m_params[ 1 ], 0.f, 0.f },
		                                { float( viewport.X ), float( viewport.Y ), float( viewport.Width ? viewport.Width : 1 ), float( viewport.Height ? viewport.Height : 1 ) } };

	DWORD old_srgb                = 0;
	IDirect3DVertexShader9* old_vs = nullptr;
	IDirect3DPixelShader9* old_ps  = nullptr;
	float old_ps_constants[ 2 ][ 4 ]{ };

	static std::vector< saved_constant_t > saved_vs{ }, saved_ps{ };
	saved_vs.clear( );
	saved_ps.clear( );

	device->GetRenderState( D3DRS_SRGBWRITEENABLE, &old_srgb );
	device->GetVertexShader( &old_vs );
	device->GetPixelShader( &old_ps );
	device->GetPixelShaderConstantF( 20, old_ps_constants[ 0 ], 2 );
	save_constants( device, true, g_vs_defs, saved_vs );
	save_constants( device, false, g_ps_defs, saved_ps );

	// shadertoy colours are display values: no linear -> srgb on write
	device->SetRenderState( D3DRS_SRGBWRITEENABLE, FALSE );
	device->SetVertexShader( g_vs[ compressed ] );
	device->SetPixelShader( g_ps );
	device->SetPixelShaderConstantF( 20, constants[ 0 ], 2 );

	const long result = original( device, type, base_vertex, min_vertex, vertices, start_index, primitives );

	// constants before shaders: the engine shader's own defs must win their slots
	device->SetRenderState( D3DRS_SRGBWRITEENABLE, old_srgb );
	device->SetPixelShaderConstantF( 20, old_ps_constants[ 0 ], 2 );
	restore_constants( device, true, saved_vs );
	restore_constants( device, false, saved_ps );
	device->SetVertexShader( old_vs );
	device->SetPixelShader( old_ps );

	for ( IUnknown* held : std::initializer_list< IUnknown* >{ old_vs, old_ps } )
		if ( held )
			held->Release( );

	return result;
}

void n_lsd_chams::impl_t::on_device_lost( )
{
	release_gpu( );
	g_gpu_failed = false;
}
