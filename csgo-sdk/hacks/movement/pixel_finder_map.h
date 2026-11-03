#pragma once
/* engine-free, gate: tools/pf_map_test.cpp ( tools/pf_map_check.py ). planes = GetBrushInfo: solid where dot( n, p ) < d */
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace n_pm
{
	struct plane_t {
		float nx, ny, nz, d;
	};

	/* one wall face's top edge: n = unit xy out of the brush, a..b on the wall at z */
	struct seam_t {
		float nx = 0.f, ny = 0.f;
		float z  = 0.f;
		float ax = 0.f, ay = 0.f;
		float bx = 0.f, by = 0.f;
		float len( ) const
		{
			const float dx = bx - ax, dy = by - ay;
			return std::sqrt( dx * dx + dy * dy );
		}
	};

	constexpr float k_max_nz  = 0.2f;
	constexpr float k_min_len = 12.f;
	constexpr float k_max_len = 8192.f;

	/* lowest up plane = the real top ( bevels duplicate it ); none = wedge / ramp */
	inline bool brush_top( const plane_t* p, int count, float& z_top )
	{
		bool found = false;
		for ( int i = 0; i < count; ++i ) {
			if ( p[ i ].nz < 0.999f )
				continue;
			const float z = p[ i ].d / p[ i ].nz;
			if ( !found || z < z_top ) {
				z_top = z;
				found = true;
			}
		}
		return found;
	}

	inline bool face_seam( const plane_t* p, int count, int face, float z_top, seam_t& out )
	{
		if ( face < 0 || face >= count )
			return false;
		const plane_t& w = p[ face ];
		if ( std::fabs( w.nz ) > k_max_nz )
			return false;
		const float len2 = std::sqrt( w.nx * w.nx + w.ny * w.ny );
		if ( len2 < 0.1f )
			return false;

		const float nx = w.nx / len2, ny = w.ny / len2;
		const float d2 = ( w.d - w.nz * z_top ) / len2;
		const float px = nx * d2, py = ny * d2;
		const float tx = ny, ty = -nx;

		float smin = -k_max_len, smax = k_max_len;
		for ( int i = 0; i < count; ++i ) {
			if ( i == face )
				continue;
			const float a = p[ i ].nx * tx + p[ i ].ny * ty;
			const float c = p[ i ].d - p[ i ].nz * z_top - ( p[ i ].nx * px + p[ i ].ny * py );
			if ( a > 1e-4f ) {
				const float s = c / a;
				if ( s < smax )
					smax = s;
			} else if ( a < -1e-4f ) {
				const float s = c / a;
				if ( s > smin )
					smin = s;
			} else if ( c < -1e-3f )
				return false;
			if ( smax - smin < k_min_len )
				return false;
		}
		if ( smax - smin < k_min_len || smax - smin > k_max_len )
			return false;

		out.nx = nx;
		out.ny = ny;
		out.z  = z_top;
		out.ax = px + tx * smin;
		out.ay = py + ty * smin;
		out.bx = px + tx * smax;
		out.by = py + ty * smax;
		return true;
	}

	/* appends, never clears */
	inline void brush_seams( const plane_t* p, int count, std::vector< seam_t >& out )
	{
		float z_top = 0.f;
		if ( !brush_top( p, count, z_top ) )
			return;
		seam_t s;
		for ( int i = 0; i < count; ++i )
			if ( face_seam( p, count, i, z_top, s ) )
				out.push_back( s );
	}

	/* a row of brushes = one seam. depth 0.5u tolerant, height 1/64u: the probe owns the 1u unit z falls in */
	inline void merge( std::vector< seam_t >& v, float gap = 2.f )
	{
		auto key = []( const seam_t& s ) {
			const float d = s.nx * s.ax + s.ny * s.ay;
			return std::array< long, 4 >{ std::lround( s.nx * 100.f ), std::lround( s.ny * 100.f ), std::lround( d * 2.f ), std::lround( s.z * 64.f ) };
		};
		auto proj = []( const seam_t& s, float x, float y ) { return s.ny * x - s.nx * y; };

		std::sort( v.begin( ), v.end( ), [ & ]( const seam_t& a, const seam_t& b ) {
			const auto ka = key( a ), kb = key( b );
			if ( ka != kb )
				return ka < kb;
			return ( std::min )( proj( a, a.ax, a.ay ), proj( a, a.bx, a.by ) ) < ( std::min )( proj( b, b.ax, b.ay ), proj( b, b.bx, b.by ) );
		} );

		std::vector< seam_t > outv;
		for ( const seam_t& s : v ) {
			if ( !outv.empty( ) && key( outv.back( ) ) == key( s ) ) {
				seam_t& m      = outv.back( );
				const float m0 = proj( m, m.ax, m.ay ), m1 = proj( m, m.bx, m.by );
				const float s0 = proj( m, s.ax, s.ay ), s1 = proj( m, s.bx, s.by );
				const float mlo = ( std::min )( m0, m1 ), mhi = ( std::max )( m0, m1 );
				const float slo = ( std::min )( s0, s1 ), shi = ( std::max )( s0, s1 );
				if ( slo <= mhi + gap && shi >= mlo - gap ) {
					if ( slo < mlo ) {
						m.ax = ( s0 < s1 ) ? s.ax : s.bx;
						m.ay = ( s0 < s1 ) ? s.ay : s.by;
					}
					if ( shi > mhi ) {
						m.bx = ( s0 < s1 ) ? s.bx : s.ax;
						m.by = ( s0 < s1 ) ? s.by : s.ay;
					}
					continue;
				}
			}
			outv.push_back( s );
		}
		v.swap( outv );
	}

	/* no gap between samples wider than stride, ends inset */
	inline int sample_count( float len, float stride, int cap )
	{
		if ( len < k_min_len )
			return 0;
		const float usable = len - 2.f * ( std::min )( 8.f, len * 0.25f );
		const float step   = ( std::max )( stride, 1.f );
		int n              = 1 + static_cast< int >( std::floor( usable / step ) );
		if ( n > 1 && usable / static_cast< float >( n - 1 ) > step )
			n++;
		return n < 1 ? 1 : ( n > cap ? cap : n );
	}

	inline float sample_t( int i, int n, float len )
	{
		if ( n <= 1 )
			return 0.5f;
		const float inset = ( std::min )( 8.f, len * 0.25f ) / ( std::max )( len, 1e-3f );
		return inset + ( 1.f - 2.f * inset ) * ( static_cast< float >( i ) / static_cast< float >( n - 1 ) );
	}

	/* confirmed run lo..hi -> segment, half a stride wider each end, clamped to the seam */
	inline void run_segment( const seam_t& s, int lo, int hi, int n, float stride, float& ax, float& ay, float& bx, float& by )
	{
		const float len  = s.len( );
		const float half = ( stride * 0.5f ) / ( len > 1.f ? len : 1.f );
		float t0         = sample_t( lo, n, len ) - half;
		float t1         = sample_t( hi, n, len ) + half;
		if ( t0 < 0.f )
			t0 = 0.f;
		if ( t1 > 1.f )
			t1 = 1.f;
		ax = s.ax + ( s.bx - s.ax ) * t0;
		ay = s.ay + ( s.by - s.ay ) * t0;
		bx = s.ax + ( s.bx - s.ax ) * t1;
		by = s.ay + ( s.by - s.ay ) * t1;
	}
}
