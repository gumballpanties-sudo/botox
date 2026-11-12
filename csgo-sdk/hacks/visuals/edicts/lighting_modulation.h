#pragma once
#include <cstddef>
#include <cstring>

namespace n_lighting_modulation
{
	struct point_t {
		float x, y, z;
	};

	inline void normalize( const char* src, char* out, std::size_t size )
	{
		std::size_t n = 0;
		for ( ; src && src[ n ] && n + 1 < size; ++n ) {
			const char c = src[ n ] == '\\' ? '/' : src[ n ];
			out[ n ]     = ( c >= 'A' && c <= 'Z' ) ? static_cast< char >( c - 'A' + 'a' ) : c;
		}
		out[ n ] = '\0';
	}

	template< std::size_t n >
	inline bool any( const char* hay, const char* const ( &needles )[ n ] )
	{
		for ( const char* needle : needles )
			if ( std::strstr( hay, needle ) )
				return true;
		return false;
	}

	inline bool is_lamp_model( const char* model )
	{
		static const char* const veto[] = { "models/player", "models/weapons", "models/inventory_items", "lighthouse", "flight",
			                                "highlight",     "lightning",      "switch" };
		static const char* const lamp[] = { "lamp", "light", "bulb", "lantern", "fixture", "neon" };
		char m[ 192 ];
		normalize( model, m, sizeof( m ) );
		return m[ 0 ] && !any( m, veto ) && any( m, lamp );
	}

	/* "beam" left out: it hit tracer / laser sprites */
	inline bool is_light_glow_material( const char* name )
	{
		static const char* const veto[] = { "wall",         "concrete",       "brick",          "metal",      "wood",   "floor",
			                                "ceiling",      "roof",           "building",       "structure",  "models/player",
			                                "models/weapons", "models/inventory_items", "cs_custom_material", "particle/", "vgui/",
			                                "hud/",         "console/",       "debug/",         "dev/",       "laser",  "physbeam" };
		static const char* const glow[] = { "lighteffect", "sprites/glow", "sprites/light", "light_glow", "_glow", "spotlight", "volumetric",
			                                "lightbeam" };
		char m[ 192 ];
		normalize( name, m, sizeof( m ) );
		return m[ 0 ] && !any( m, veto ) && any( m, glow );
	}

	/* up to max_out lamps nearest eye within max_dist, nearest first, as indices */
	inline int pick_lamps( const point_t* lamps, int n, const point_t& eye, float max_dist, int max_out, int* out )
	{
		float d2s[ 64 ];
		if ( max_out > 64 )
			max_out = 64;
		if ( max_out <= 0 || !lamps || !out )
			return 0;

		int k           = 0;
		const float lim = max_dist * max_dist;
		for ( int i = 0; i < n; ++i ) {
			const float dx = lamps[ i ].x - eye.x, dy = lamps[ i ].y - eye.y, dz = lamps[ i ].z - eye.z;
			const float d2 = dx * dx + dy * dy + dz * dz;
			if ( !( d2 <= lim ) )
				continue;

			int j = k < max_out ? k++ : max_out;
			if ( j == max_out && d2 >= d2s[ max_out - 1 ] )
				continue;
			if ( j == max_out )
				j = max_out - 1;

			for ( ; j > 0 && d2s[ j - 1 ] > d2; --j ) {
				d2s[ j ] = d2s[ j - 1 ];
				out[ j ] = out[ j - 1 ];
			}
			d2s[ j ] = d2;
			out[ j ] = i;
		}
		return k;
	}
}
