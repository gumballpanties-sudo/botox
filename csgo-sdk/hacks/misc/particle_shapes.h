#pragma once

#include <algorithm>
#include <cmath>
#include "../../dependencies/imgui/imgui.h"
#include "../../dependencies/imgui/imgui_internal.h"

/* soul death particle shapes ( kill_effects k_kfx_ps kind 4..9 ), shared with detection particles. index = combo, 0 = circle.
   outline( ) = same shapes as the shader sdf, checked by tools/soul_shape_check.py */
namespace n_particle_shape
{
	inline constexpr const char* k_names = "circle\0" "4 star\0" "5 star\0" "triangle\0" "square\0" "cross\0" "butterfly\0";
	inline constexpr int k_last          = 6;
	inline constexpr float k_scale       = 1.6f; // shape tip radius per circle radius
	inline constexpr int k_max_points    = 50;

	// == k_kfx_ps k_bfly: right half, bottom notch -> top notch
	inline constexpr float k_bfly[ 26 ][ 2 ] = {
		{ 0.0000f, -0.4267f }, { 0.0145f, -0.5783f }, { 0.0548f, -0.7177f }, { 0.1164f, -0.8324f }, { 0.1944f, -0.9103f }, { 0.2843f, -0.9390f },
		{ 0.3950f, -0.9165f }, { 0.4854f, -0.8552f }, { 0.5463f, -0.7643f }, { 0.5686f, -0.6530f }, { 0.5431f, -0.5356f }, { 0.4747f, -0.4412f },
		{ 0.6261f, -0.3698f }, { 0.7462f, -0.2555f }, { 0.8253f, -0.1079f }, { 0.8538f, 0.0634f }, { 0.8538f, 0.4450f }, { 0.8215f, 0.5652f },
		{ 0.7364f, 0.6507f }, { 0.6164f, 0.6832f }, { 0.5839f, 0.6801f }, { 0.4987f, 0.6585f }, { 0.3793f, 0.5997f }, { 0.2441f, 0.4854f },
		{ 0.1115f, 0.2968f }, { 0.0000f, 0.0155f } };

	/* unit outline, tips at r 1, +y up, clockwise ( imgui winding once y flips to screen ), star shaped from 0, 0 */
	inline int outline( const int shape, float ( &out )[ k_max_points ][ 2 ] )
	{
		int n          = 0;
		const auto put = [ & ]( const float x, const float y ) {
			out[ n ][ 0 ] = x;
			out[ n ][ 1 ] = y;
			++n;
		};

		if ( shape >= 1 && shape <= 4 ) {
			// shader fold: tip, notch at rf, tip ... ( rf = cos( pi / tips ) is the plain polygon )
			constexpr int k_tips[ 4 ]    = { 4, 5, 3, 4 };
			constexpr float k_notch[ 4 ] = { 0.431f, 0.454f, 0.5f, 0.7071068f };
			const int tips               = k_tips[ shape - 1 ];
			for ( int k = 0; k < tips * 2; ++k ) {
				const float a = 3.14159265f * static_cast< float >( k ) / static_cast< float >( tips );
				const float r = k & 1 ? k_notch[ shape - 1 ] : 1.f;
				put( r * std::sin( a ), r * std::cos( a ) );
			}
		} else if ( shape == 5 ) {
			constexpr float w = 0.23f, l = 0.973f;
			constexpr float k_cross[ 12 ][ 2 ] = { { -w, l }, { w, l }, { w, w }, { l, w }, { l, -w }, { w, -w }, { w, -l }, { -w, -l }, { -w, -w }, { -l, -w }, { -l, w }, { -w, w } };
			for ( const auto& p : k_cross )
				put( p[ 0 ], p[ 1 ] );
		} else if ( shape == 6 ) {
			for ( int i = 25; i >= 0; --i )
				put( k_bfly[ i ][ 0 ], k_bfly[ i ][ 1 ] );
			for ( int i = 1; i <= 24; ++i )
				put( -k_bfly[ i ][ 0 ], k_bfly[ i ][ 1 ] );
		}
		return n;
	}

	/* imgui AddConvexPolyFilled fringe over screen points, fanned from center ( stars / cross / butterfly are concave,
	   star shaped from center ). any winding: the area sign turns the fringe outward ( a 3d plane seen from behind flips it ) */
	inline void fill_screen( ImDrawList* dl, const ImVec2& center, const ImVec2* pts, const int n, const ImU32 col )
	{
		if ( n < 3 || n > k_max_points )
			return;

		float area = 0.f;
		for ( int i = 0; i < n; ++i )
			area += pts[ i ].x * pts[ ( i + 1 ) % n ].y - pts[ ( i + 1 ) % n ].x * pts[ i ].y;
		const float side = area < 0.f ? -1.f : 1.f;

		ImVec2 nrm[ k_max_points ];
		for ( int i = 0; i < n; ++i ) {
			const ImVec2& a = pts[ i ];
			const ImVec2& b = pts[ ( i + 1 ) % n ];
			const float len = std::sqrt( ( b.x - a.x ) * ( b.x - a.x ) + ( b.y - a.y ) * ( b.y - a.y ) );
			nrm[ i ]        = len > 0.f ? ImVec2( side * ( b.y - a.y ) / len, side * ( a.x - b.x ) / len ) : ImVec2( 0.f, 0.f );
		}

		const ImVec2 uv   = dl->_Data->TexUvWhitePixel;
		const float half  = dl->_FringeScale * 0.5f;
		const ImU32 clear = col & ~IM_COL32_A_MASK;
		dl->PrimReserve( n * 9, n * 2 + 1 );
		const unsigned int base = dl->_VtxCurrentIdx;
		const unsigned int mid  = base + static_cast< unsigned int >( n ) * 2u;
		for ( int i0 = n - 1, i1 = 0; i1 < n; i0 = i1++ ) {
			float mx = ( nrm[ i0 ].x + nrm[ i1 ].x ) * 0.5f, my = ( nrm[ i0 ].y + nrm[ i1 ].y ) * 0.5f;
			if ( const float d2 = mx * mx + my * my; d2 > 1e-6f ) {
				const float inv = ( std::min )( 1.f / d2, 100.f );
				mx *= inv;
				my *= inv;
			}
			dl->PrimWriteVtx( ImVec2( pts[ i1 ].x - mx * half, pts[ i1 ].y - my * half ), uv, col );
			dl->PrimWriteVtx( ImVec2( pts[ i1 ].x + mx * half, pts[ i1 ].y + my * half ), uv, clear );

			const unsigned int in0 = base + static_cast< unsigned int >( i0 ) * 2u, in1 = base + static_cast< unsigned int >( i1 ) * 2u;
			const unsigned int idx[ 9 ] = { mid, in0, in1, in1, in0, in0 + 1u, in0 + 1u, in1 + 1u, in1 };
			for ( const unsigned int i : idx )
				dl->PrimWriteIdx( static_cast< ImDrawIdx >( i ) );
		}
		dl->PrimWriteVtx( center, uv, col );
	}

	/* 2d glyph: outline at radius, spin 0..1 */
	inline void fill( ImDrawList* dl, const ImVec2& center, const float radius, const float spin, const ImU32 col, const int shape )
	{
		float unit[ k_max_points ][ 2 ];
		const int n = outline( shape, unit );
		if ( n < 3 )
			return;

		const float sn = std::sin( spin * 6.2831853f ), cs = std::cos( spin * 6.2831853f );
		ImVec2 pts[ k_max_points ];
		for ( int i = 0; i < n; ++i )
			pts[ i ] = ImVec2( center.x + ( unit[ i ][ 0 ] * cs - unit[ i ][ 1 ] * sn ) * radius,
			                   center.y - ( unit[ i ][ 0 ] * sn + unit[ i ][ 1 ] * cs ) * radius );
		fill_screen( dl, center, pts, n, col );
	}

	/* 3d planar rotation ( clipart "3d rotation" ), float[ 3 ] so every game's vector type can use it.
	   tumble = rodrigues about unit axis k from the unit start frame u0 / v0. w 1 = tumbling, 0 = flat on the unit landing
	   normal n with the in-plane turn the tumble had at angle_land, so the blend over the settle has no pop.
	   out u / v orthonormal ( checked by tools/tumble_frame_test.cpp ) */
	inline void rotate3( const float* p, const float* k, const float c, const float s, float* out )
	{
		const float kp = k[ 0 ] * p[ 0 ] + k[ 1 ] * p[ 1 ] + k[ 2 ] * p[ 2 ];
		const float x[ 3 ] = { k[ 1 ] * p[ 2 ] - k[ 2 ] * p[ 1 ], k[ 2 ] * p[ 0 ] - k[ 0 ] * p[ 2 ], k[ 0 ] * p[ 1 ] - k[ 1 ] * p[ 0 ] };
		for ( int i = 0; i < 3; ++i )
			out[ i ] = p[ i ] * c + x[ i ] * s + k[ i ] * kp * ( 1.f - c );
	}

	inline bool unit3( float* v )
	{
		const float len = std::sqrt( v[ 0 ] * v[ 0 ] + v[ 1 ] * v[ 1 ] + v[ 2 ] * v[ 2 ] );
		if ( len < 1e-4f )
			return false;
		for ( int i = 0; i < 3; ++i )
			v[ i ] /= len;
		return true;
	}

	// orthonormal u / v ( columns u, v, u x v ) -> unit quaternion x y z w
	inline void frame_quat( const float* u, const float* v, float* q )
	{
		const float m00 = u[ 0 ], m10 = u[ 1 ], m20 = u[ 2 ], m01 = v[ 0 ], m11 = v[ 1 ], m21 = v[ 2 ];
		const float m02 = u[ 1 ] * v[ 2 ] - u[ 2 ] * v[ 1 ], m12 = u[ 2 ] * v[ 0 ] - u[ 0 ] * v[ 2 ], m22 = u[ 0 ] * v[ 1 ] - u[ 1 ] * v[ 0 ];
		const float trace = m00 + m11 + m22;
		if ( trace > 0.f ) {
			const float s = std::sqrt( trace + 1.f ) * 2.f;
			q[ 3 ] = 0.25f * s, q[ 0 ] = ( m21 - m12 ) / s, q[ 1 ] = ( m02 - m20 ) / s, q[ 2 ] = ( m10 - m01 ) / s;
		} else if ( m00 > m11 && m00 > m22 ) {
			const float s = std::sqrt( 1.f + m00 - m11 - m22 ) * 2.f;
			q[ 3 ] = ( m21 - m12 ) / s, q[ 0 ] = 0.25f * s, q[ 1 ] = ( m01 + m10 ) / s, q[ 2 ] = ( m02 + m20 ) / s;
		} else if ( m11 > m22 ) {
			const float s = std::sqrt( 1.f + m11 - m00 - m22 ) * 2.f;
			q[ 3 ] = ( m02 - m20 ) / s, q[ 0 ] = ( m01 + m10 ) / s, q[ 1 ] = 0.25f * s, q[ 2 ] = ( m12 + m21 ) / s;
		} else {
			const float s = std::sqrt( 1.f + m22 - m00 - m11 ) * 2.f;
			q[ 3 ] = ( m10 - m01 ) / s, q[ 0 ] = ( m02 + m20 ) / s, q[ 1 ] = ( m12 + m21 ) / s, q[ 2 ] = 0.25f * s;
		}
	}

	inline void tumble_frame( const float* k, const float* u0, const float* v0, const float angle, const float angle_land, const float* n, float w,
	                          float* u, float* v )
	{
		rotate3( u0, k, std::cos( angle ), std::sin( angle ), u );
		rotate3( v0, k, std::cos( angle ), std::sin( angle ), v );
		w = std::clamp( w, 0.f, 1.f );
		if ( w >= 1.f )
			return;

		float ul[ 3 ], vl[ 3 ];
		rotate3( u0, k, std::cos( angle_land ), std::sin( angle_land ), ul );
		rotate3( v0, k, std::cos( angle_land ), std::sin( angle_land ), vl );

		// flat frame on n: the landing u pressed into the plane ( v when u stands on n ), v keeps the landing side
		float uf[ 3 ], vf[ 3 ];
		const float* src = ul;
		for ( int pass = 0; pass < 3; ++pass ) {
			const float axis[ 3 ] = { 1.f, 0.f, 0.f };
			src            = pass == 0 ? ul : pass == 1 ? vl : axis;
			const float d  = n[ 0 ] * src[ 0 ] + n[ 1 ] * src[ 1 ] + n[ 2 ] * src[ 2 ];
			for ( int i = 0; i < 3; ++i )
				uf[ i ] = src[ i ] - n[ i ] * d;
			if ( unit3( uf ) )
				break;
			if ( pass == 2 ) {
				uf[ 0 ] = 0.f, uf[ 1 ] = 1.f, uf[ 2 ] = 0.f; // n along x
			}
		}
		vf[ 0 ] = n[ 1 ] * uf[ 2 ] - n[ 2 ] * uf[ 1 ], vf[ 1 ] = n[ 2 ] * uf[ 0 ] - n[ 0 ] * uf[ 2 ], vf[ 2 ] = n[ 0 ] * uf[ 1 ] - n[ 1 ] * uf[ 0 ];
		if ( vf[ 0 ] * vl[ 0 ] + vf[ 1 ] * vl[ 1 ] + vf[ 2 ] * vl[ 2 ] < 0.f )
			for ( float& x : vf )
				x = -x;

		/* correction c = flat * land^-1 is fixed per flake; apply ( 1 - w ) of it on top of the live tumble. blending toward the
		   moving tumble instead pops every time the tumble passes 180 deg from flat ( the shortest-path sign flips ) */
		float ql[ 4 ], qf[ 4 ], qt[ 4 ], c[ 4 ], p[ 4 ], q[ 4 ];
		frame_quat( ul, vl, ql );
		frame_quat( uf, vf, qf );
		frame_quat( u, v, qt );
		const auto mul = []( const float* a, const float* b, float* o ) {
			o[ 0 ] = a[ 3 ] * b[ 0 ] + a[ 0 ] * b[ 3 ] + a[ 1 ] * b[ 2 ] - a[ 2 ] * b[ 1 ];
			o[ 1 ] = a[ 3 ] * b[ 1 ] - a[ 0 ] * b[ 2 ] + a[ 1 ] * b[ 3 ] + a[ 2 ] * b[ 0 ];
			o[ 2 ] = a[ 3 ] * b[ 2 ] + a[ 0 ] * b[ 1 ] - a[ 1 ] * b[ 0 ] + a[ 2 ] * b[ 3 ];
			o[ 3 ] = a[ 3 ] * b[ 3 ] - a[ 0 ] * b[ 0 ] - a[ 1 ] * b[ 1 ] - a[ 2 ] * b[ 2 ];
		};
		const float land_inv[ 4 ] = { -ql[ 0 ], -ql[ 1 ], -ql[ 2 ], ql[ 3 ] };
		mul( qf, land_inv, c );
		if ( c[ 3 ] < 0.f ) // shortest way, decided once from fixed inputs, so it never flips mid settle
			for ( float& x : c )
				x = -x;
		// nlerp identity -> c: same hemisphere ( c w >= 0 ), never degenerate
		const float s = 1.f - w;
		float len     = 0.f;
		for ( int i = 0; i < 4; ++i ) {
			p[ i ] = c[ i ] * s + ( i == 3 ? 1.f - s : 0.f );
			len += p[ i ] * p[ i ];
		}
		len = std::sqrt( len );
		for ( float& x : p )
			x /= len;
		mul( p, qt, q );
		const float x = q[ 0 ], y = q[ 1 ], z = q[ 2 ], r = q[ 3 ];
		u[ 0 ] = 1.f - 2.f * ( y * y + z * z ), u[ 1 ] = 2.f * ( x * y + r * z ), u[ 2 ] = 2.f * ( x * z - r * y );
		v[ 0 ] = 2.f * ( x * y - r * z ), v[ 1 ] = 1.f - 2.f * ( x * x + z * z ), v[ 2 ] = 2.f * ( y * z + r * x );
	}
} // namespace n_particle_shape
