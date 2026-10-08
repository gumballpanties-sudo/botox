#pragma once
#include <algorithm>
#include <cmath>
#include <vector>

namespace n_pf
{
	constexpr float k_lat       = 16.f;
	constexpr float k_slack     = 2.f;
	constexpr float k_lat_surf  = 2.f;
	constexpr float k_lat_hb    = 2.f;
	constexpr float k_lat_tb    = 4.f;
	constexpr float k_tb_reach  = 4.f;
	constexpr float k_lat_edge  = 4.f;
	constexpr float k_edge_off  = 0.01f;
	constexpr float k_edge_tol  = 0.005f;
	constexpr float k_dna_above = 0.0287018f;
	constexpr float k_dna_below = 0.027908f;

	inline float marker( float k ) { return k + ( k < 0.f ? k_dna_below : k_dna_above ); }
	inline bool is_marker( float z ) { return std::fabs( z - marker( std::floor( z ) ) ) < 1e-4f; }

	inline float seam_top( float a, float b ) { return std::floor( ( a > b ? a : b ) + k_slack ); }
	inline float seam_bottom( float a, float b ) { return std::ceil( ( a < b ? a : b ) - k_slack ); }

	inline int lat_count( float step, float reach = k_lat ) { return 2 * static_cast< int >( reach / step + 0.5f ) + 1; }
	inline float lat( int i, float step ) { return static_cast< float >( ( i + 1 ) / 2 ) * step * ( i & 1 ? 1.f : -1.f ); }

	/* move along (nx, ny) so the hull's deepest corner sits gap off plane (p, dist); e = any point on that line */
	inline float wall_shift( float ex, float ey, float ez, float px, float py, float pz, float dist, float nx, float ny, float gap, float hull_h )
	{
		const float support = 16.f * ( std::fabs( px ) + std::fabs( py ) ) - ( pz < 0.f ? hull_h * pz : 0.f );
		return ( dist + gap + support - ( ex * px + ey * py + ez * pz ) ) / ( nx * px + ny * py );
	}

	/* lo reads unlike hi; returns the z where it flips, to ( hi - lo ) / 2^iters */
	template < class F >
	float bisect( float lo, float hi, F like_hi, int iters = 14 )
	{
		for ( int i = 0; i < iters; ++i ) {
			const float mid = 0.5f * ( lo + hi );
			( like_hi( mid ) ? hi : lo ) = mid;
		}
		return 0.5f * ( lo + hi );
	}

	struct height_t {
		float z    = 0.f;
		bool seam  = false; /* integer: flush seams no trace can see */
		bool top   = false; /* something below sticks out: surf / pj / rise */
		bool under = false; /* something above sticks out: hb */
	};

	/* integers lo..hi ( both seam + top + under ) and traced edges, top down, one entry per height ( edges within k_edge_tol merge,
	   an edge on an integer is that integer ) */
	inline std::vector< height_t > heights( float lo, float hi, std::vector< height_t > edges )
	{
		std::vector< height_t > out;
		for ( float k = hi; k >= lo; k -= 1.f )
			out.push_back( { k, true, true, true } );
		std::sort( edges.begin( ), edges.end( ), [ ]( const height_t& a, const height_t& b ) { return a.z > b.z; } );
		for ( const height_t& e : edges ) {
			if ( e.z < lo - 0.5f || e.z > hi + 0.5f || std::fabs( e.z - std::round( e.z ) ) < 0.001f )
				continue;
			const auto same = std::find_if( out.begin( ), out.end( ), [ & ]( const height_t& h ) { return !h.seam && std::fabs( h.z - e.z ) < k_edge_tol; } );
			if ( same != out.end( ) ) {
				same->top |= e.top;
				same->under |= e.under;
			} else
				out.push_back( e );
		}
		std::stable_sort( out.begin( ), out.end( ), [ ]( const height_t& a, const height_t& b ) { return a.z > b.z; } );
		return out;
	}
}
