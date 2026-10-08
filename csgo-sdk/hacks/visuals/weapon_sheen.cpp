#include "weapon_sheen.h"
#include "../misc/scaleform/image_cache.h"
#include "screen/render_queue.h"
#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"
#include "../../globals/config/variables.h"
#include "../../globals/globals.h"
#include "../../globals/logger/logger.h"

#include <windows.h>
#include <bcrypt.h>
#include <d3dx9.h>

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#pragma comment( lib, "bcrypt.lib" )

namespace
{
	constexpr const char* k_base_url  = "https://raw.githubusercontent.com/gumballpanties-sudo/botox-assets/main/sheen/";
	constexpr const char* k_cache_dir = "C:\\botox\\sheen\\";
	constexpr float k_frame_rate      = 25.f;

	// bright core u per frame of the sha-pinned animatedsheen0.vtf (box-smoothed column peak, red channel)
	constexpr float k_mask_core[ 60 ] = { 0.000f, 0.000f, 0.020f, 0.020f, 0.020f, 0.020f, 0.020f, 0.035f, 0.043f, 0.059f, 0.066f, 0.090f,
	                                      0.105f, 0.121f, 0.145f, 0.168f, 0.191f, 0.215f, 0.238f, 0.270f, 0.301f, 0.332f, 0.355f, 0.395f,
	                                      0.418f, 0.457f, 0.488f, 0.520f, 0.551f, 0.582f, 0.613f, 0.645f, 0.684f, 0.715f, 0.746f, 0.777f,
	                                      0.816f, 0.848f, 0.879f, 0.902f, 0.949f, 0.973f, 0.980f, 0.980f, 0.980f, 0.980f, 0.980f, 0.980f,
	                                      0.980f, 0.980f, 0.980f, 0.980f, 0.980f, 0.980f, 0.980f, 0.980f, 0.980f, 0.980f, 0.980f, 0.980f };

	enum e_asset : int { asset_mask, asset_cube, asset_cube_hdr, asset_count };

	struct asset_t {
		const char* m_name;
		const char* m_sha256;
	};

	constexpr asset_t k_assets[ asset_count ] = {
		{ "animatedsheen0.vtf", "2cdd1054c6ce25a8973c34cf00b0e004746eaaa6438d6a2c466f04d54de1a00c" },
		{ "cubemap_sheen001.vtf", "7bed7b6d9308932b7c5d8e74857836786bb5486c17a289607d5e3ad75afa7a0b" },
		{ "cubemap_sheen001.hdr.vtf", "4fd062162bcdc7d0564495404ceb4255e9c8295435398c40a7ceaf0f16f84f3d" },
	};

	constexpr const char k_vs[] = R"(
float4 cEyePos : register( c2 );
float4x4 cViewProj : register( c8 );
float4x3 cModel[ 53 ] : register( c58 );

struct VS_INPUT {
	float4 vPos : POSITION;
	float4 vNormal : NORMAL;
	float4 vBoneWeights : BLENDWEIGHT;
	float4 vBoneIndices : BLENDINDICES;
};

struct VS_OUTPUT {
	float4 vProjPosition : POSITION;
	float3 vWorldNormal : TEXCOORD0;
	float3 vWorldViewVector : TEXCOORD1;
	float3 vModelSpacePos : TEXCOORD2;
};

float3 mul4x3( float4 v, float4x3 m ) { return float3( dot( v, transpose( m )[ 0 ] ), dot( v, transpose( m )[ 1 ] ), dot( v, transpose( m )[ 2 ] ) ); }
float3 mul3x3( float3 v, float3x3 m ) { return float3( dot( v, transpose( m )[ 0 ] ), dot( v, transpose( m )[ 1 ] ), dot( v, transpose( m )[ 2 ] ) ); }

VS_OUTPUT main( const VS_INPUT i )
{
	VS_OUTPUT o;
	float3 vObjNormal;
	float4 vWeights = i.vBoneWeights;
#if COMPRESSED_VERTS
	float2 ztSigns = ( i.vNormal.xy - 128.0f ) < 0;
	float2 xyAbs   = abs( i.vNormal.xy - 128.0f ) - ztSigns;
	float2 xySigns = ( xyAbs - 64.0f ) < 0;
	vObjNormal.xy  = ( abs( xyAbs - 64.0f ) - xySigns ) / 63.0f;
	vObjNormal.z   = 1.0f - vObjNormal.x - vObjNormal.y;
	vObjNormal     = normalize( vObjNormal );
	vObjNormal.xy *= lerp( float2( 1.0f, 1.0f ), float2( -1.0f, -1.0f ), xySigns );
	vObjNormal.z  *= lerp( 1.0f, -1.0f, ztSigns.x );
	vWeights       = ( vWeights + 1 ) / 32768;
#else
	vObjNormal = i.vNormal.xyz;
#endif
	int3 boneIndices = D3DCOLORtoUBYTE4( i.vBoneIndices ).xyz;
	float3 weights   = vWeights.xyz;
	weights[ 2 ]     = 1 - ( weights[ 0 ] + weights[ 1 ] );
	float4x3 blendMatrix = cModel[ boneIndices[ 0 ] ] * weights[ 0 ] + cModel[ boneIndices[ 1 ] ] * weights[ 1 ] + cModel[ boneIndices[ 2 ] ] * weights[ 2 ];

	float3 vWorldPosition = mul4x3( i.vPos, blendMatrix );
	o.vWorldNormal        = mul3x3( vObjNormal, ( float3x3 )blendMatrix );
	o.vProjPosition       = mul( float4( vWorldPosition, 1.0f ), cViewProj );
	o.vWorldViewVector    = normalize( vWorldPosition - cEyePos.xyz );
	o.vModelSpacePos      = i.vPos.xyz;
	return o;
}
)";

	constexpr const char k_ps[] = R"(
samplerCUBE EnvmapSampler : register( s0 );
sampler2D EnvmapMaskSampler : register( s1 );

float4 cLightScale : register( c30 );
float4 g_vSheenU : register( c6 );
float4 g_vSheenV : register( c7 );
float4 g_cCloakColorTint : register( c8 );
float4 g_vSheenBand : register( c9 );

struct PS_INPUT {
	float3 vWorldNormal : TEXCOORD0;
	float3 vWorldViewVector : TEXCOORD1;
	float3 vModelSpacePos : TEXCOORD2;
};

float4 main( PS_INPUT i ) : COLOR
{
	float3 vEyeDir          = -normalize( i.vWorldViewVector );
	float3 worldSpaceNormal = normalize( i.vWorldNormal );
	float3 vReflect         = 2 * worldSpaceNormal * dot( worldSpaceNormal, vEyeDir ) - vEyeDir;

	float3 envMapColor = cLightScale.z * texCUBE( EnvmapSampler, vReflect ).xyz * g_cCloakColorTint.xyz;
	envMapColor *= 10.0f;

	float4 vPos            = float4( i.vModelSpacePos, 1.0f );
	float u                = g_vSheenBand.x + ( dot( vPos, g_vSheenU ) - g_vSheenBand.x ) * g_vSheenBand.y;
	float4 envmapMaskTexel = tex2D( EnvmapMaskSampler, float2( u, 1.0f - dot( vPos, g_vSheenV ) ) );

	float alpha = max( max( envMapColor.x, envMapColor.y ), envMapColor.z );
	return float4( envMapColor.xyz * envmapMaskTexel.xyz, alpha * envmapMaskTexel.x * g_cCloakColorTint.w );
}
)";

	struct vtf_t {
		std::vector< unsigned char > m_bytes{ };
		int m_width = 0, m_height = 0, m_mips = 0, m_frames = 0, m_faces = 0;
		D3DFORMAT m_format = D3DFMT_UNKNOWN;
		std::size_t m_data = 0;

		std::size_t level_size( const int level ) const
		{
			const int w = ( std::max )( 1, this->m_width >> level ), h = ( std::max )( 1, this->m_height >> level );
			if ( this->m_format == D3DFMT_DXT1 )
				return static_cast< std::size_t >( ( std::max )( 1, ( w + 3 ) / 4 ) ) * ( std::max )( 1, ( h + 3 ) / 4 ) * 8;

			return static_cast< std::size_t >( w ) * h * 8;
		}

		// vtf high res block: smallest mip first, then frame, then face
		const unsigned char* image( const int level, const int frame, const int face ) const
		{
			std::size_t offset = this->m_data;
			for ( int l = this->m_mips - 1; l > level; l-- )
				offset += this->level_size( l ) * this->m_frames * this->m_faces;

			return this->m_bytes.data( ) + offset + ( static_cast< std::size_t >( frame ) * this->m_faces + face ) * this->level_size( level );
		}
	};

	struct planes_t {
		float m_u[ 4 ];
		float m_v[ 4 ];
	};

	struct vvd_header_t {
		int m_id, m_version, m_checksum, m_lods;
		int m_lod_vertexes[ 8 ];
		int m_fixups, m_fixup_table_start, m_vertex_data_start, m_tangent_data_start;
	};

	// 0 idle, 1 loading, 2 ready, 3 failed
	std::atomic< int > g_state{ 0 };
	vtf_t g_vtf[ asset_count ]{ };

	c_material* g_carrier = nullptr;
	std::unordered_map< std::string, planes_t > g_planes{ };

	IDirect3DVertexShader9* g_vs[ 2 ]{ };
	IDirect3DPixelShader9* g_ps = nullptr;
	std::vector< IDirect3DTexture9* > g_masks{ };
	IDirect3DCubeTexture9* g_cubes[ 2 ]{ };
	std::vector< std::pair< int, int > > g_vs_defs{ }, g_ps_defs{ };
	bool g_gpu_failed = false;

	template< typename value_t >
	value_t read_at( const std::vector< unsigned char >& bytes, const std::size_t offset )
	{
		value_t value{ };
		std::memcpy( &value, bytes.data( ) + offset, sizeof( value ) );
		return value;
	}

	bool parse_vtf( vtf_t& vtf )
	{
		const auto& bytes = vtf.m_bytes;
		if ( bytes.size( ) < 80 || std::memcmp( bytes.data( ), "VTF", 4 ) != 0 || read_at< int >( bytes, 4 ) != 7 )
			return false;

		const int minor       = read_at< int >( bytes, 8 );
		const int header_size = read_at< int >( bytes, 12 );
		const int flags       = read_at< int >( bytes, 20 );
		const int first_frame = read_at< unsigned short >( bytes, 26 );
		const int format      = read_at< int >( bytes, 52 );

		vtf.m_width  = read_at< unsigned short >( bytes, 16 );
		vtf.m_height = read_at< unsigned short >( bytes, 18 );
		vtf.m_frames = read_at< unsigned short >( bytes, 24 );
		vtf.m_mips   = bytes[ 56 ];
		vtf.m_format = format == 13 ? D3DFMT_DXT1 : format == 24 ? D3DFMT_A16B16G16R16F : D3DFMT_UNKNOWN;
		// pre 7.5 envmaps carry a 7th spheremap face
		vtf.m_faces = ( flags & 0x4000 ) ? ( ( minor < 5 && first_frame != 0xffff ) ? 7 : 6 ) : 1;

		if ( vtf.m_format == D3DFMT_UNKNOWN || vtf.m_width <= 0 || vtf.m_height <= 0 || vtf.m_frames <= 0 || vtf.m_mips <= 0 || vtf.m_mips > 16 )
			return false;

		std::size_t total = 0;
		for ( int level = 0; level < vtf.m_mips; level++ )
			total += vtf.level_size( level ) * vtf.m_frames * vtf.m_faces;

		if ( header_size < 80 || total + header_size > bytes.size( ) )
			return false;

		vtf.m_data = bytes.size( ) - total;
		return true;
	}

	std::string sha256_hex( const std::vector< unsigned char >& data )
	{
		BCRYPT_ALG_HANDLE algorithm = nullptr;
		BCRYPT_HASH_HANDLE hash     = nullptr;
		unsigned char digest[ 32 ]{ };

		const bool ok = BCRYPT_SUCCESS( BCryptOpenAlgorithmProvider( &algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0 ) ) &&
		                BCRYPT_SUCCESS( BCryptCreateHash( algorithm, &hash, nullptr, 0, nullptr, 0, 0 ) ) &&
		                BCRYPT_SUCCESS( BCryptHashData( hash, const_cast< PUCHAR >( data.data( ) ), static_cast< ULONG >( data.size( ) ), 0 ) ) &&
		                BCRYPT_SUCCESS( BCryptFinishHash( hash, digest, sizeof( digest ), 0 ) );

		if ( hash )
			BCryptDestroyHash( hash );
		if ( algorithm )
			BCryptCloseAlgorithmProvider( algorithm, 0 );

		if ( !ok )
			return { };

		char hex[ 65 ]{ };
		for ( int i = 0; i < 32; i++ )
			sprintf_s( hex + i * 2, 3, "%02x", digest[ i ] );

		return hex;
	}

	std::vector< unsigned char > read_file( const std::string& path )
	{
		std::ifstream file( path, std::ios::binary );
		if ( !file )
			return { };

		return { std::istreambuf_iterator< char >( file ), std::istreambuf_iterator< char >( ) };
	}

	void load_assets( )
	{
		CreateDirectoryA( "C:\\botox", nullptr );
		CreateDirectoryA( "C:\\botox\\sheen", nullptr );

		for ( int i = 0; i < asset_count; i++ ) {
			const std::string path = std::string( k_cache_dir ) + k_assets[ i ].m_name;
			vtf_t& vtf             = g_vtf[ i ];

			for ( int attempt = 0; attempt < 2; attempt++ ) {
				vtf.m_bytes = read_file( path );
				if ( !vtf.m_bytes.empty( ) && sha256_hex( vtf.m_bytes ) == k_assets[ i ].m_sha256 )
					break;

				vtf.m_bytes.clear( );
				if ( attempt == 0 && !n_image_cache::download_file( std::string( k_base_url ) + k_assets[ i ].m_name, path ) )
					break;
			}

			if ( vtf.m_bytes.empty( ) || !parse_vtf( vtf ) ) {
				botox_dbg_log( "SHEEN: %s missing or bad hash, expected at %s%s", k_assets[ i ].m_name, k_base_url, k_assets[ i ].m_name );
				g_state.store( 3, std::memory_order_release );
				return;
			}
		}

		botox_dbg_log( "SHEEN: assets ready, mask %d frames", g_vtf[ asset_mask ].m_frames );
		g_state.store( 2, std::memory_order_release );
	}

	// vvd vertex frame, not bbox: csgo v_ verts don't line up with the bbox like tf2 c_models do
	bool model_planes( const char* name, planes_t& out )
	{
		if ( const auto it = g_planes.find( name ); it != g_planes.end( ) ) {
			out = it->second;
			return true;
		}

		c_model_cache* cache = g_interfaces.m_model_cache;
		if ( !cache )
			return false;

		const unsigned short handle = cache->find_mdl( name );
		if ( handle == 0xffffu )
			return false;

		float mins[ 3 ] = { FLT_MAX, FLT_MAX, FLT_MAX }, maxs[ 3 ] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
		bool measured = false;

		cache->begin_lock( );
		if ( const auto vvd = static_cast< const vvd_header_t* >( cache->get_vertex_data( handle ) );
		     vvd && vvd->m_id == 0x56534449 && vvd->m_lod_vertexes[ 0 ] > 0 && vvd->m_vertex_data_start > 0 ) {
			const int count = vvd->m_lod_vertexes[ 0 ];

			if ( vvd->m_tangent_data_start == 0 || vvd->m_tangent_data_start >= vvd->m_vertex_data_start + count * 48 ) {
				const auto* vertex = reinterpret_cast< const unsigned char* >( vvd ) + vvd->m_vertex_data_start;

				for ( int i = 0; i < count; i++, vertex += 48 ) {
					float position[ 3 ];
					std::memcpy( position, vertex + 16, sizeof( position ) );

					for ( int axis = 0; axis < 3; axis++ ) {
						mins[ axis ] = ( std::min )( mins[ axis ], position[ axis ] );
						maxs[ axis ] = ( std::max )( maxs[ axis ], position[ axis ] );
					}
				}

				measured = true;
			}
		}
		cache->end_lock( );
		cache->release( handle );

		if ( !measured )
			return false;

		int axes[ 3 ] = { 0, 1, 2 };
		std::sort( axes, axes + 3, [ & ]( const int a, const int b ) { return maxs[ a ] - mins[ a ] > maxs[ b ] - mins[ b ]; } );

		const int along = axes[ 0 ], across = axes[ 1 ];
		const float length = maxs[ along ] - mins[ along ], height = maxs[ across ] - mins[ across ];
		if ( length <= 0.01f || height <= 0.01f )
			return false;

		planes_t planes{ };

		// sweep runs toward the far end from the grip (the muzzle / blade tip)
		if ( maxs[ along ] >= -mins[ along ] ) {
			planes.m_u[ along ] = 1.f / length;
			planes.m_u[ 3 ]     = -mins[ along ] / length;
		} else {
			planes.m_u[ along ] = -1.f / length;
			planes.m_u[ 3 ]     = maxs[ along ] / length;
		}

		planes.m_v[ across ] = 1.f / height;
		planes.m_v[ 3 ]      = -mins[ across ] / height;

		botox_dbg_log( "SHEEN: %s along %d (%.1f..%.1f) across %d (%.1f..%.1f)", name, along, mins[ along ], maxs[ along ], across, mins[ across ],
		               maxs[ across ] );

		g_planes.emplace( name, planes );
		out = planes;
		return true;
	}

	c_material* carrier_material( )
	{
		if ( g_carrier || !g_interfaces.m_material_system || !c_key_values::usable( ) )
			return g_carrier;

		// additive black: invisible if any carrier draw ever slips past the armed window
		auto* key_values = new c_key_values( "UnlitGeneric" );
		key_values->load_from_buffer( "botox_sheen_carrier", R"("UnlitGeneric"
{
	"$basetexture" "vgui/white"
	"$color"       "[0 0 0]"
	"$color2"      "[0 0 0]"
	"$additive"    "1"
	"$model"       "1"
	"$nofog"       "1"
})" );

		g_carrier = g_interfaces.m_material_system->create_material( "botox_sheen_carrier", key_values );

		if ( g_carrier )
			g_carrier->increment_reference_count( );
		else
			delete key_values;

		return g_carrier;
	}

	ID3DXBuffer* compile( const char* source, const char* profile, const D3DXMACRO* macros )
	{
		ID3DXBuffer *code = nullptr, *errors = nullptr;
		D3DXCompileShader( source, static_cast< UINT >( std::strlen( source ) ), macros, nullptr, "main", profile, 0, &code, &errors, nullptr );

		if ( errors ) {
			botox_dbg_log( "SHEEN: %s compile: %s", profile, static_cast< const char* >( errors->GetBufferPointer( ) ) );
			errors->Release( );
		}

		return code;
	}

	// def literals can leak into the hw register file under the engine's state cache: save + restore each
	void collect_defs( const DWORD* code, std::vector< std::pair< int, int > >& out )
	{
		for ( std::size_t i = 1; code[ i ] != 0x0000FFFF; ) {
			const DWORD token  = code[ i ];
			const DWORD opcode = token & 0xFFFF;

			if ( opcode == D3DSIO_COMMENT ) {
				i += 1 + ( ( token & D3DSI_COMMENTSIZE_MASK ) >> D3DSI_COMMENTSIZE_SHIFT );
				continue;
			}

			if ( opcode == D3DSIO_DEF || opcode == D3DSIO_DEFI || opcode == D3DSIO_DEFB )
				out.emplace_back( opcode == D3DSIO_DEF ? 0 : opcode == D3DSIO_DEFI ? 1 : 2, static_cast< int >( code[ i + 1 ] & D3DSP_REGNUM_MASK ) );

			i += 1 + ( ( token & D3DSI_INSTLENGTH_MASK ) >> D3DSI_INSTLENGTH_SHIFT );
		}
	}

	void fill_level( const vtf_t& vtf, const int level, const int frame, const int face, const D3DLOCKED_RECT& rect )
	{
		const int w = ( std::max )( 1, vtf.m_width >> level ), h = ( std::max )( 1, vtf.m_height >> level );
		const bool dxt               = vtf.m_format == D3DFMT_DXT1;
		const int rows               = dxt ? ( std::max )( 1, ( h + 3 ) / 4 ) : h;
		const std::size_t row_bytes  = dxt ? static_cast< std::size_t >( ( std::max )( 1, ( w + 3 ) / 4 ) ) * 8 : static_cast< std::size_t >( w ) * 8;
		const unsigned char* source  = vtf.image( level, frame, face );

		for ( int row = 0; row < rows; row++ )
			std::memcpy( static_cast< unsigned char* >( rect.pBits ) + row * rect.Pitch, source + row * row_bytes, row_bytes );
	}

	IDirect3DTexture9* make_texture( IDirect3DDevice9* device, const vtf_t& vtf, const int frame )
	{
		IDirect3DTexture9 *staging = nullptr, *texture = nullptr;
		if ( FAILED( device->CreateTexture( vtf.m_width, vtf.m_height, vtf.m_mips, 0, vtf.m_format, D3DPOOL_SYSTEMMEM, &staging, nullptr ) ) )
			return nullptr;

		bool ok = true;
		for ( int level = 0; level < vtf.m_mips && ok; level++ ) {
			D3DLOCKED_RECT rect{ };
			ok = SUCCEEDED( staging->LockRect( level, &rect, nullptr, 0 ) );
			if ( ok ) {
				fill_level( vtf, level, frame, 0, rect );
				staging->UnlockRect( level );
			}
		}

		if ( !ok || FAILED( device->CreateTexture( vtf.m_width, vtf.m_height, vtf.m_mips, 0, vtf.m_format, D3DPOOL_DEFAULT, &texture, nullptr ) ) ||
		     FAILED( device->UpdateTexture( staging, texture ) ) ) {
			if ( texture )
				texture->Release( );
			texture = nullptr;
		}

		staging->Release( );
		return texture;
	}

	IDirect3DCubeTexture9* make_cube( IDirect3DDevice9* device, const vtf_t& vtf )
	{
		IDirect3DCubeTexture9 *staging = nullptr, *texture = nullptr;
		if ( vtf.m_faces < 6 ||
		     FAILED( device->CreateCubeTexture( vtf.m_width, vtf.m_mips, 0, vtf.m_format, D3DPOOL_SYSTEMMEM, &staging, nullptr ) ) )
			return nullptr;

		bool ok = true;
		for ( int face = 0; face < 6 && ok; face++ ) {
			for ( int level = 0; level < vtf.m_mips && ok; level++ ) {
				D3DLOCKED_RECT rect{ };
				ok = SUCCEEDED( staging->LockRect( static_cast< D3DCUBEMAP_FACES >( face ), level, &rect, nullptr, 0 ) );
				if ( ok ) {
					fill_level( vtf, level, 0, face, rect );
					staging->UnlockRect( static_cast< D3DCUBEMAP_FACES >( face ), level );
				}
			}
		}

		if ( !ok || FAILED( device->CreateCubeTexture( vtf.m_width, vtf.m_mips, 0, vtf.m_format, D3DPOOL_DEFAULT, &texture, nullptr ) ) ||
		     FAILED( device->UpdateTexture( staging, texture ) ) ) {
			if ( texture )
				texture->Release( );
			texture = nullptr;
		}

		staging->Release( );
		return texture;
	}

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

		for ( IDirect3DTexture9* texture : g_masks )
			if ( texture )
				texture->Release( );
		g_masks.clear( );

		for ( IDirect3DCubeTexture9*& texture : g_cubes ) {
			if ( texture )
				texture->Release( );
			texture = nullptr;
		}

		g_vs_defs.clear( );
		g_ps_defs.clear( );
	}

	// render thread
	bool gpu_ready( IDirect3DDevice9* device )
	{
		if ( g_ps )
			return true;

		if ( g_gpu_failed || g_state.load( std::memory_order_acquire ) != 2 )
			return false;

		const D3DXMACRO plain[]      = { { "COMPRESSED_VERTS", "0" }, { nullptr, nullptr } };
		const D3DXMACRO compressed[] = { { "COMPRESSED_VERTS", "1" }, { nullptr, nullptr } };

		ID3DXBuffer* code[ 3 ] = { compile( k_vs, "vs_3_0", plain ), compile( k_vs, "vs_3_0", compressed ), compile( k_ps, "ps_3_0", nullptr ) };

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

		for ( int frame = 0; frame < g_vtf[ asset_mask ].m_frames && ok; frame++ ) {
			g_masks.push_back( make_texture( device, g_vtf[ asset_mask ], frame ) );
			ok = g_masks.back( ) != nullptr;
		}

		if ( ok ) {
			g_cubes[ 0 ] = make_cube( device, g_vtf[ asset_cube ] );
			g_cubes[ 1 ] = make_cube( device, g_vtf[ asset_cube_hdr ] );
			ok           = g_cubes[ 0 ] && g_cubes[ 1 ];
		}

		botox_dbg_log( "SHEEN: gpu %s, defs vs %d ps %d", ok ? "built" : "FAILED", static_cast< int >( g_vs_defs.size( ) ),
		               static_cast< int >( g_ps_defs.size( ) ) );

		if ( !ok ) {
			release_gpu( );
			g_gpu_failed = true;
			return false;
		}

		return true;
	}

	// -1 = not a studio decl we can read, 0 = float normals, 1 = csgo UBYTE4 compressed (vertexdecl.cpp:142)
	int vertex_compression( IDirect3DDevice9* device )
	{
		static IDirect3DVertexDeclaration9* last = nullptr;
		static int result                        = -1;

		IDirect3DVertexDeclaration9* declaration = nullptr;
		if ( FAILED( device->GetVertexDeclaration( &declaration ) ) || !declaration )
			return -1;

		if ( declaration != last ) {
			D3DVERTEXELEMENT9 elements[ MAXD3DDECLLENGTH + 1 ]{ };
			UINT count = 0;
			result     = -1;

			if ( SUCCEEDED( declaration->GetDeclaration( nullptr, &count ) ) && count <= std::size( elements ) &&
			     SUCCEEDED( declaration->GetDeclaration( elements, &count ) ) ) {
				for ( UINT i = 0; i < count && elements[ i ].Stream != 0xFF; i++ ) {
					if ( elements[ i ].Usage == D3DDECLUSAGE_NORMAL && elements[ i ].UsageIndex == 0 )
						result = elements[ i ].Type == D3DDECLTYPE_UBYTE4 ? 1 : elements[ i ].Type == D3DDECLTYPE_FLOAT3 ? 0 : -1;
				}
			}

			last = declaration;
		}

		declaration->Release( );
		return result;
	}

	struct saved_constant_t {
		int m_kind;
		int m_register;
		float m_value[ 4 ];
	};

	void save_constants( IDirect3DDevice9* device, const bool vertex, const std::vector< std::pair< int, int > >& defs, std::vector< saved_constant_t >& out )
	{
		for ( const auto& [ kind, reg ] : defs ) {
			saved_constant_t saved{ kind, reg, { } };

			if ( kind == 0 )
				vertex ? device->GetVertexShaderConstantF( reg, saved.m_value, 1 ) : device->GetPixelShaderConstantF( reg, saved.m_value, 1 );
			else if ( kind == 1 )
				vertex ? device->GetVertexShaderConstantI( reg, reinterpret_cast< int* >( saved.m_value ), 1 )
				       : device->GetPixelShaderConstantI( reg, reinterpret_cast< int* >( saved.m_value ), 1 );
			else
				vertex ? device->GetVertexShaderConstantB( reg, reinterpret_cast< BOOL* >( saved.m_value ), 1 )
				       : device->GetPixelShaderConstantB( reg, reinterpret_cast< BOOL* >( saved.m_value ), 1 );

			out.push_back( saved );
		}
	}

	void restore_constants( IDirect3DDevice9* device, const bool vertex, const std::vector< saved_constant_t >& saved )
	{
		for ( const saved_constant_t& entry : saved ) {
			if ( entry.m_kind == 0 )
				vertex ? device->SetVertexShaderConstantF( entry.m_register, entry.m_value, 1 ) : device->SetPixelShaderConstantF( entry.m_register, entry.m_value, 1 );
			else if ( entry.m_kind == 1 )
				vertex ? device->SetVertexShaderConstantI( entry.m_register, reinterpret_cast< const int* >( entry.m_value ), 1 )
				       : device->SetPixelShaderConstantI( entry.m_register, reinterpret_cast< const int* >( entry.m_value ), 1 );
			else
				vertex ? device->SetVertexShaderConstantB( entry.m_register, reinterpret_cast< const BOOL* >( entry.m_value ), 1 )
				       : device->SetPixelShaderConstantB( entry.m_register, reinterpret_cast< const BOOL* >( entry.m_value ), 1 );
		}
	}
}

bool n_weapon_sheen::impl_t::begin( const char* model_name )
{
	if ( !GET_VARIABLE( g_variables.m_weapon_sheen, bool ) ) {
		int failed = 3;
		g_state.compare_exchange_strong( failed, 0 );
		return false;
	}

	if ( g_ctx.m_unloading.load( std::memory_order_relaxed ) || !g_interfaces.m_global_vars_base || !g_interfaces.m_model_render )
		return false;

	if ( int idle = 0; g_state.compare_exchange_strong( idle, 1 ) )
		std::thread( load_assets ).detach( );

	if ( g_state.load( std::memory_order_acquire ) != 2 )
		return false;

	// CProxyAnimatedWeaponSheen::OnBind: same frame for every call in one frame, wrap -> wait
	const float curtime = g_interfaces.m_global_vars_base->m_current_time;
	const float delay   = std::clamp( GET_VARIABLE( g_variables.m_weapon_sheen_delay, float ), 0.f, 10.f );

	if ( this->m_next_start > curtime + delay + 1.f )
		this->m_next_start = curtime;

	if ( this->m_next_start > curtime )
		return false;

	const float delta    = curtime - this->m_next_start;
	const float previous = ( std::max )( delta - g_interfaces.m_global_vars_base->m_frame_time, 0.f );
	const int frames     = g_vtf[ asset_mask ].m_frames;

	params_t params{ };
	params.m_frame = static_cast< int >( k_frame_rate * delta ) % frames;

	if ( static_cast< int >( k_frame_rate * previous ) % frames > params.m_frame ) {
		this->m_next_start = curtime + delay;
		return false;
	}

	if ( params.m_frame <= 0 )
		return false;

	planes_t planes{ };
	if ( !model_planes( model_name, planes ) )
		return false;

	std::memcpy( params.m_u, planes.m_u, sizeof( params.m_u ) );
	std::memcpy( params.m_v, planes.m_v, sizeof( params.m_v ) );

	params.m_band[ 0 ] = k_mask_core[ ( std::min )( params.m_frame, 59 ) ];
	params.m_band[ 1 ] = 1.f / std::clamp( GET_VARIABLE( g_variables.m_weapon_sheen_width, float ), 0.1f, 1.f );

	const c_color& color = GET_VARIABLE( g_variables.m_weapon_sheen_color, c_color );
	for ( int i = 0; i < 4; i++ )
		params.m_tint[ i ] = color[ i ] / 255.f;

	c_material* carrier = carrier_material( );
	if ( !carrier )
		return false;

	auto arm = [ params ] {
		g_weapon_sheen.m_params = params;
		g_weapon_sheen.m_armed.store( true, std::memory_order_relaxed );
	};

	if ( !n_render_queue::submit( arm ) )
		arm( );

	g_interfaces.m_model_render->forced_material_override( carrier );
	return true;
}

void n_weapon_sheen::impl_t::end( )
{
	g_interfaces.m_model_render->forced_material_override( nullptr );

	auto disarm = [ ] { g_weapon_sheen.m_armed.store( false, std::memory_order_relaxed ); };

	if ( !n_render_queue::submit( disarm ) )
		disarm( );
}

long n_weapon_sheen::impl_t::draw( const draw_indexed_primitive_t original, IDirect3DDevice9* device, const D3DPRIMITIVETYPE type, const int base_vertex,
                                   const unsigned int min_vertex, const unsigned int vertices, const unsigned int start_index, const unsigned int primitives )
{
	// skipping is safe: the carrier itself is additive black
	if ( !gpu_ready( device ) )
		return D3D_OK;

	const int compressed = vertex_compression( device );
	if ( compressed < 0 )
		return D3D_OK;

	const params_t& params = this->m_params;
	if ( params.m_frame <= 0 || params.m_frame >= static_cast< int >( g_masks.size( ) ) )
		return D3D_OK;

	// c30.z = ENV_MAP_SCALE: 16 under integer hdr, 1 without (shaderapidx8.cpp:16622)
	float light_scale[ 4 ]{ };
	device->GetPixelShaderConstantF( 30, light_scale, 1 );
	const bool hdr = light_scale[ 2 ] > 1.5f;

	static constexpr D3DRENDERSTATETYPE states[] = { D3DRS_ALPHABLENDENABLE,   D3DRS_SRCBLEND,         D3DRS_DESTBLEND,      D3DRS_BLENDOP,
	                                                 D3DRS_SEPARATEALPHABLENDENABLE, D3DRS_ALPHATESTENABLE, D3DRS_ZWRITEENABLE, D3DRS_COLORWRITEENABLE,
	                                                 D3DRS_SRGBWRITEENABLE, D3DRS_FOGENABLE };
	const DWORD state_values[]                   = { TRUE,  D3DBLEND_SRCALPHA, D3DBLEND_INVSRCALPHA, D3DBLENDOP_ADD, FALSE, FALSE, TRUE,
	                                                 D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE, TRUE, FALSE };

	static constexpr D3DSAMPLERSTATETYPE samplers[] = { D3DSAMP_ADDRESSU,  D3DSAMP_ADDRESSV,    D3DSAMP_ADDRESSW,      D3DSAMP_MAGFILTER,   D3DSAMP_MINFILTER,
	                                                    D3DSAMP_MIPFILTER, D3DSAMP_SRGBTEXTURE, D3DSAMP_MIPMAPLODBIAS, D3DSAMP_MAXMIPLEVEL, D3DSAMP_BORDERCOLOR };
	const DWORD srgb_read                          = hdr ? FALSE : TRUE;
	// mask u: black border, a narrowed band maps the gun ends past the mask edges
	const DWORD sampler_values[ 2 ][ std::size( samplers ) ] = {
		{ D3DTADDRESS_CLAMP, D3DTADDRESS_CLAMP, D3DTADDRESS_CLAMP, D3DTEXF_LINEAR, D3DTEXF_LINEAR, D3DTEXF_LINEAR, srgb_read, 0, 0, 0 },
		{ D3DTADDRESS_BORDER, D3DTADDRESS_WRAP, D3DTADDRESS_WRAP, D3DTEXF_LINEAR, D3DTEXF_LINEAR, D3DTEXF_LINEAR, srgb_read, 0, 0, 0 } };

	DWORD old_states[ std::size( states ) ]{ }, old_samplers[ 2 ][ std::size( samplers ) ]{ };
	IDirect3DVertexShader9* old_vs   = nullptr;
	IDirect3DPixelShader9* old_ps    = nullptr;
	IDirect3DBaseTexture9* old_textures[ 2 ]{ };
	float old_ps_constants[ 4 ][ 4 ]{ };

	static std::vector< saved_constant_t > saved_vs{ }, saved_ps{ };
	saved_vs.clear( );
	saved_ps.clear( );

	for ( std::size_t i = 0; i < std::size( states ); i++ )
		device->GetRenderState( states[ i ], &old_states[ i ] );
	for ( DWORD stage = 0; stage < 2; stage++ ) {
		for ( std::size_t i = 0; i < std::size( samplers ); i++ )
			device->GetSamplerState( stage, samplers[ i ], &old_samplers[ stage ][ i ] );
		device->GetTexture( stage, &old_textures[ stage ] );
	}
	device->GetVertexShader( &old_vs );
	device->GetPixelShader( &old_ps );
	device->GetPixelShaderConstantF( 6, old_ps_constants[ 0 ], 4 );
	save_constants( device, true, g_vs_defs, saved_vs );
	save_constants( device, false, g_ps_defs, saved_ps );

	for ( std::size_t i = 0; i < std::size( states ); i++ )
		device->SetRenderState( states[ i ], state_values[ i ] );
	for ( DWORD stage = 0; stage < 2; stage++ )
		for ( std::size_t i = 0; i < std::size( samplers ); i++ )
			device->SetSamplerState( stage, samplers[ i ], sampler_values[ stage ][ i ] );

	device->SetTexture( 0, g_cubes[ hdr ? 1 : 0 ] );
	device->SetTexture( 1, g_masks[ params.m_frame ] );
	device->SetVertexShader( g_vs[ compressed ] );
	device->SetPixelShader( g_ps );
	device->SetPixelShaderConstantF( 6, params.m_u, 1 );
	device->SetPixelShaderConstantF( 7, params.m_v, 1 );
	device->SetPixelShaderConstantF( 8, params.m_tint, 1 );
	device->SetPixelShaderConstantF( 9, params.m_band, 1 );

	const long result = original( device, type, base_vertex, min_vertex, vertices, start_index, primitives );

	static bool logged = false;
	if ( !logged ) {
		logged = true;
		botox_dbg_log( "SHEEN: first draw hr %08lx hdr %d compressed %d frame %d", static_cast< unsigned long >( result ), hdr ? 1 : 0, compressed,
		               params.m_frame );
	}

	// constants before shaders: the engine shader's own defs must win their slots
	for ( std::size_t i = 0; i < std::size( states ); i++ )
		device->SetRenderState( states[ i ], old_states[ i ] );
	for ( DWORD stage = 0; stage < 2; stage++ )
		for ( std::size_t i = 0; i < std::size( samplers ); i++ )
			device->SetSamplerState( stage, samplers[ i ], old_samplers[ stage ][ i ] );
	device->SetPixelShaderConstantF( 6, old_ps_constants[ 0 ], 4 );
	restore_constants( device, true, saved_vs );
	restore_constants( device, false, saved_ps );
	device->SetVertexShader( old_vs );
	device->SetPixelShader( old_ps );
	for ( DWORD stage = 0; stage < 2; stage++ )
		device->SetTexture( stage, old_textures[ stage ] );

	for ( IUnknown* held : std::initializer_list< IUnknown* >{ old_vs, old_ps, old_textures[ 0 ], old_textures[ 1 ] } )
		if ( held )
			held->Release( );

	return result;
}

void n_weapon_sheen::impl_t::on_device_lost( )
{
	release_gpu( );
	g_gpu_failed = false;
}

void n_weapon_sheen::impl_t::release_material( )
{
	for ( int waited = 0; g_state.load( ) == 1 && waited < 500; ++waited )
		Sleep( 10 );

	if ( g_carrier )
		g_carrier->decrement_reference_count( );
	g_carrier = nullptr;
}
