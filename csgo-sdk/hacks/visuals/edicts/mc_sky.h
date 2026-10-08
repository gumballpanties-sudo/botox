#pragma once

#include "mc_sky_assets.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <execution>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <string>
#include <system_error>
#include <vector>

/* minecraft 1.21.11 overworld sky baked into a source skybox ( skybox/botox_minecraft_v2{rt,bk,lf,ft,up,dn} ).
   /time set day ( tick 1000 ), plains, clear weather, default options ( render distance 12, cloud range 128,
   fancy clouds ), eye y 65.62 ( standing on y 64 ). numbers from decompiled SkyRenderer hpk, FogRenderer igq,
   AtmosphericFogEnvironment igr, CloudRenderer hnv, EnvironmentAttributes ceg + core sky / clouds shaders.
   clouds are not baked: drawn live on top ( mc_clouds.h ). cloud_hit = cpu reference of that shader.
   gate: tools/mc_sky_check.py */
namespace n_mc_sky
{
	inline constexpr const char* k_name     = "botox_minecraft_v2";
	inline constexpr const char* k_old_name = "botox_minecraft"; // clouds baked in, deleted by ensure
	inline constexpr const char* k_faces[ 6 ] = { "rt", "bk", "lf", "ft", "up", "dn" };
	inline constexpr int k_size             = 1024;

	struct rgb_t {
		float r, g, b;
	};

	// ARGB.lerp -> Mth.lerpInt: a + floor( t * ( b - a ) )
	inline int lerp_int( const float t, const int a, const int b )
	{
		return a + static_cast< int >( std::floor( t * static_cast< float >( b - a ) ) );
	}

	inline rgb_t sky_color( )
	{
		return { 0x78 / 255.f, 0xa7 / 255.f, 0xff / 255.f };
	}

	// fog_color #c0d8ff pulled toward sky_color by 1 - clampedLerp( min( 512 / 16, 12 ) / 32, .25, 1 ) ^ .25 ( igr )
	inline rgb_t fog_color( )
	{
		const float t = 1.f - static_cast< float >( std::pow( 0.25f + ( 12.f / 32.f ) * 0.75f, 0.25 ) );
		return { lerp_int( t, 0xc0, 0x78 ) / 255.f, lerp_int( t, 0xd8, 0xa7 ) / 255.f, lerp_int( t, 0xff, 0xff ) / 255.f };
	}

	inline constexpr float k_sky_fog_end   = 192.f;  // min( 12 * 16, sky_fog_end_distance 512 )
	inline constexpr float k_cloud_fog_end = 2048.f; // min( 128 * 16, cloud_fog_end_distance 2048 )

	// visual/sun_angle at tick 1000: 360 * cubic_bezier( .362, .241, .638, .759 ) over ( tick - 6000 ) / 24000
	inline float sun_angle( )
	{
		const auto bz = []( const double a, const double b, const double t ) {
			return 3.0 * ( 1.0 - t ) * ( 1.0 - t ) * t * a + 3.0 * ( 1.0 - t ) * t * t * b + t * t * t;
		};
		const double p = ( 1000.0 - 6000.0 + 24000.0 ) / 24000.0;
		double lo = 0.0, hi = 1.0;
		for ( int i = 0; i < 60; i++ )
			( bz( 0.362, 0.638, ( lo + hi ) * 0.5 ) < p ? lo : hi ) = ( lo + hi ) * 0.5;
		return static_cast< float >( bz( 0.241, 0.759, ( lo + hi ) * 0.5 ) * 360.0 * 3.14159265358979 / 180.0 );
	}

	inline rgb_t mix( const rgb_t& a, const rgb_t& b, const float t )
	{
		return { a.r + ( b.r - a.r ) * t, a.g + ( b.g - a.g ) * t, a.b + ( b.b - a.b ) * t };
	}

	// source face pixel -> source direction. engine gl_warp.cpp: st_to_vec + skytexorder, t flipped
	inline void face_dir( const int face, const float s, const float t, float out[ 3 ] )
	{
		const float dirs[ 6 ][ 3 ] = { { 1.f, -s, t }, { s, 1.f, t }, { -1.f, s, t }, { -s, -1.f, t }, { -t, -s, 1.f }, { t, -s, -1.f } };
		std::memcpy( out, dirs[ face ], sizeof( dirs[ face ] ) );
	}

	inline bool cloud_cell( const int x, const int z )
	{
		const int cx = ( ( x % 256 ) + 256 ) % 256, cz = ( ( z % 256 ) + 256 ) % 256;
		return ( k_clouds[ cz * 32 + cx / 8 ] >> ( cx % 8 ) ) & 1;
	}

	inline constexpr float k_cloud_h     = 192.33f - 65.62f; // CloudOffset.y = cloud_height - eye y
	inline constexpr int k_cloud_radius  = 171;              // ceil( 2048 / 12 ), hnv mesh radius in cells
	inline constexpr float k_cloud_alpha = 0.8f;             // cloud_color #ccffffff

	// camera position in cloud texture blocks ( hnv: cam x + ticks * 0.03, cam z + 3.96 ). tick 1000 at cell 35, 14:
	// sun clear of clouds, ~30% cover overhead
	inline constexpr float k_reference_offset[ 2 ] = { 35.f * 12.f + 6.f, 14.f * 12.f + 3.96f };

	// first cloud face the ray hits ( depth tested 12x4x12 cells, camera below ). shade = face color.
	// mc_clouds.h k_shader runs the same walk on the gpu: change both
	inline bool cloud_hit( const float d[ 3 ], const float offset[ 2 ], float& shade, float& dist )
	{
		constexpr float k_inf = 1e30f;

		if ( d[ 1 ] <= 0.f )
			return false;

		float t           = k_cloud_h / d[ 1 ];
		const float t_top = ( k_cloud_h + 4.f ) / d[ 1 ];
		const float x     = offset[ 0 ] + d[ 0 ] * t, z = offset[ 1 ] + d[ 2 ] * t;
		const int home_x  = static_cast< int >( std::floor( offset[ 0 ] / 12.f ) ), home_z = static_cast< int >( std::floor( offset[ 1 ] / 12.f ) );
		int cx = static_cast< int >( std::floor( x / 12.f ) ), cz = static_cast< int >( std::floor( z / 12.f ) );

		const int step_x = d[ 0 ] > 0.f ? 1 : -1, step_z = d[ 2 ] > 0.f ? 1 : -1;
		const float dt_x = d[ 0 ] != 0.f ? 12.f / std::fabs( d[ 0 ] ) : k_inf;
		const float dt_z = d[ 2 ] != 0.f ? 12.f / std::fabs( d[ 2 ] ) : k_inf;
		float next_x = d[ 0 ] != 0.f ? t + ( ( cx + ( step_x > 0 ) ) * 12.f - x ) / d[ 0 ] : k_inf;
		float next_z = d[ 2 ] != 0.f ? t + ( ( cz + ( step_z > 0 ) ) * 12.f - z ) / d[ 2 ] : k_inf;

		// bottom 0.7, west/east 0.9, north/south 0.8 ( rendertype_clouds.vsh faceColors )
		float face = 0.7f;
		while ( t < t_top && t < k_cloud_fog_end ) {
			const int rx = cx - home_x, rz = cz - home_z;
			if ( rx * rx + rz * rz <= k_cloud_radius * k_cloud_radius && cloud_cell( cx, cz ) ) {
				shade = face;
				dist  = t;
				return true;
			}

			if ( next_x < next_z ) {
				t = next_x;
				next_x += dt_x;
				cx += step_x;
				face = 0.9f;
			} else {
				t = next_z;
				next_z += dt_z;
				cz += step_z;
				face = 0.8f;
			}
		}

		return false;
	}

	// per bake constants. not function statics: the dll builds with /Zc:threadSafeInit- and render_face runs parallel
	struct frame_t {
		rgb_t sky = sky_color( ), fog = fog_color( );
		float angle = sun_angle( ), sin_a = std::sin( angle ), cos_a = std::cos( angle );
	};

	// minecraft frame color for a source direction
	inline rgb_t shade( const frame_t& frame, const float src[ 3 ] )
	{
		const rgb_t& sky = frame.sky;
		const rgb_t& fog = frame.fog;
		const float sin_a = frame.sin_a, cos_a = frame.cos_a;

		// source ( x fwd, y left, z up ) -> minecraft ( x east, y up, z south )
		const float len  = std::sqrt( src[ 0 ] * src[ 0 ] + src[ 1 ] * src[ 1 ] + src[ 2 ] * src[ 2 ] );
		const float d[ 3 ] = { src[ 0 ] / len, src[ 2 ] / len, -src[ 1 ] / len };

		// clear = fog color. sky disc: fan, centre ( 0, 16, 0 ), octagon r 512 at y 16 ( corners every 45 deg from +x ),
		// per vertex fog distance interpolated. dark disc only below y 63
		rgb_t c = fog;
		if ( d[ 1 ] > 0.f ) {
			const float px = d[ 0 ] * 16.f / d[ 1 ], pz = d[ 2 ] * 16.f / d[ 1 ];
			constexpr float k_pi = 3.14159265358979f, k_wedge = k_pi / 4.f;
			const float a        = std::atan2( pz, px );
			const float mid      = std::floor( ( a + k_pi ) / k_wedge ) * k_wedge - k_pi + k_wedge * 0.5f;
			const float ring     = ( px * std::cos( mid ) + pz * std::sin( mid ) ) / ( 512.f * std::cos( k_wedge * 0.5f ) );
			if ( ring <= 1.f ) {
				const float spherical   = 16.f + ring * ( std::sqrt( 512.f * 512.f + 16.f * 16.f ) - 16.f );
				const float cylindrical = 16.f + ring * ( 512.f - 16.f );
				const float f           = std::max( std::min( spherical / k_sky_fog_end, 1.f ), cylindrical >= k_sky_fog_end ? 1.f : 0.f );
				c                       = mix( sky, fog, f );
			}
		}

		// sun: quad +-30 at 100 up, posed YP( -90 ) * XP( angle ), additive ( OVERLAY ), nearest texel
		const float sun[ 3 ] = { -sin_a, cos_a, 0.f };
		const float facing   = d[ 0 ] * sun[ 0 ] + d[ 1 ] * sun[ 1 ];
		if ( facing > 0.f ) {
			const float t = 100.f / facing;
			const float u = ( d[ 2 ] * t ) / 30.f;                                    // local x -> +z
			const float v = ( -d[ 0 ] * cos_a - d[ 1 ] * sin_a ) * t / 30.f;         // local z -> ( -cos, -sin, 0 )
			if ( std::fabs( u ) < 1.f && std::fabs( v ) < 1.f ) {
				const int tx = std::min( 31, static_cast< int >( ( u + 1.f ) * 16.f ) ), ty = std::min( 31, static_cast< int >( ( v + 1.f ) * 16.f ) );
				const std::uint32_t texel = k_sun[ ty * 32 + tx ];
				const float alpha         = ( ( texel >> 24 ) & 0xff ) / 255.f;
				c.r                       = std::min( 1.f, c.r + ( ( texel >> 16 ) & 0xff ) / 255.f * alpha );
				c.g                       = std::min( 1.f, c.g + ( ( texel >> 8 ) & 0xff ) / 255.f * alpha );
				c.b                       = std::min( 1.f, c.b + ( texel & 0xff ) / 255.f * alpha );
			}
		}

		return c;
	}

	// cloud_color #ccffffff * face shade, alpha *= 1 - dist / cloud fog end, translucent blend over the sky
	inline rgb_t with_clouds( const rgb_t& sky, const float d[ 3 ], const float offset[ 2 ] )
	{
		float face = 0.f, dist = 0.f;
		if ( !cloud_hit( d, offset, face, dist ) )
			return sky;

		return mix( sky, { face, face, face }, k_cloud_alpha * ( 1.f - std::min( dist / k_cloud_fog_end, 1.f ) ) );
	}

	// one face, BGR888 rows top to bottom
	inline void render_face( const int face, const int size, std::uint8_t* bgr )
	{
		const frame_t frame;
		std::vector< int > rows( size );
		std::iota( rows.begin( ), rows.end( ), 0 );
		std::for_each( std::execution::par, rows.begin( ), rows.end( ), [ & ]( const int y ) {
			for ( int x = 0; x < size; x++ ) {
				float dir[ 3 ];
				face_dir( face, ( x + 0.5f ) * 2.f / size - 1.f, 1.f - ( y + 0.5f ) * 2.f / size, dir );
				const rgb_t c   = shade( frame, dir );
				std::uint8_t* p = bgr + ( static_cast< std::size_t >( y ) * size + x ) * 3;
				p[ 0 ]          = static_cast< std::uint8_t >( std::lround( std::clamp( c.b, 0.f, 1.f ) * 255.f ) );
				p[ 1 ]          = static_cast< std::uint8_t >( std::lround( std::clamp( c.g, 0.f, 1.f ) * 255.f ) );
				p[ 2 ]          = static_cast< std::uint8_t >( std::lround( std::clamp( c.r, 0.f, 1.f ) * 255.f ) );
			}
		} );
	}

	// vtf 7.2, one BGR888 mip, clamp + nomip + nolod. 80 byte header ( VectorAligned reflectivity pads it, vtf.h )
	inline bool write_vtf( const std::filesystem::path& file, const int size, const std::vector< std::uint8_t >& bgr )
	{
		std::uint8_t header[ 80 ]{ };
		const auto put     = [ & ]( const int at, const auto value ) { std::memcpy( header + at, &value, sizeof( value ) ); };
		std::memcpy( header, "VTF", 4 );
		put( 4, std::uint32_t( 7 ) );
		put( 8, std::uint32_t( 2 ) );
		put( 12, std::uint32_t( 80 ) );
		put( 16, std::uint16_t( size ) );
		put( 18, std::uint16_t( size ) );
		put( 20, std::uint32_t( 0x4 | 0x8 | 0x100 | 0x200 ) );
		put( 24, std::uint16_t( 1 ) );
		put( 48, 1.f );
		put( 52, std::uint32_t( 3 ) ); // IMAGE_FORMAT_BGR888
		header[ 56 ] = 1;
		put( 57, std::uint32_t( 0xffffffff ) );
		put( 63, std::uint16_t( 1 ) );

		std::ofstream out( file, std::ios::binary | std::ios::trunc );
		out.write( reinterpret_cast< const char* >( header ), sizeof( header ) );
		out.write( reinterpret_cast< const char* >( bgr.data( ) ), static_cast< std::streamsize >( bgr.size( ) ) );
		return out.good( );
	}

	inline std::uintmax_t vtf_bytes( const int size )
	{
		return 80u + static_cast< std::uintmax_t >( size ) * size * 3u;
	}

	// writes <csgo>\materials\skybox\botox_minecraft*.vtf/.vmt unless all 6 are already there. vmt last so a half
	// written set never loads
	inline bool ensure( const std::filesystem::path& csgo_dir, const int size = k_size )
	{
		std::error_code ec;
		const std::filesystem::path dir = csgo_dir / "materials" / "skybox";

		for ( const char* face : k_faces )
			for ( const char* ext : { ".vtf", ".vmt" } )
				std::filesystem::remove( dir / ( std::string( k_old_name ) + face + ext ), ec );

		bool ready = true;
		for ( const char* face : k_faces ) {
			const std::string base = std::string( k_name ) + face;
			ready &= std::filesystem::file_size( dir / ( base + ".vtf" ), ec ) == vtf_bytes( size ) && !ec &&
			         std::filesystem::exists( dir / ( base + ".vmt" ), ec );
		}
		if ( ready )
			return true;

		std::filesystem::create_directories( dir, ec );
		std::vector< std::uint8_t > bgr( static_cast< std::size_t >( size ) * size * 3 );
		for ( int face = 0; face < 6; face++ ) {
			render_face( face, size, bgr.data( ) );
			if ( !write_vtf( dir / ( std::string( k_name ) + k_faces[ face ] + ".vtf" ), size, bgr ) )
				return false;
		}

		for ( const char* face : k_faces ) {
			std::ofstream vmt( dir / ( std::string( k_name ) + face + ".vmt" ), std::ios::trunc );
			vmt << "\"UnlitGeneric\"\n{\n\t\"$basetexture\" \"skybox/" << k_name << face << "\"\n\t\"$nofog\" \"1\"\n\t\"$ignorez\" \"1\"\n}\n";
			if ( !vmt.good( ) )
				return false;
		}

		return true;
	}
}
