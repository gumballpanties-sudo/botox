#pragma once
#include <cmath>
#include <vector>

namespace n_color_curve
{
	inline constexpr int max_points = 12;
	inline constexpr float min_gap  = 1.f / 128.f;

	inline constexpr int config_size = max_points * 2;

	struct curve_t {
		int m_count = 2;
		float m_x[ max_points ]{ 0.f, 1.f };
		float m_y[ max_points ]{ 0.f, 1.f };
	};

	inline float clamp01( const float value )
	{
		// nan becomes 0
		return value > 1.f ? 1.f : ( value >= 0.f ? value : 0.f );
	}

	inline bool is_identity( const curve_t& curve )
	{
		return curve.m_count == 2 && curve.m_x[ 0 ] == 0.f && curve.m_y[ 0 ] == 0.f && curve.m_x[ 1 ] == 1.f && curve.m_y[ 1 ] == 1.f;
	}

	inline float evaluate( const curve_t& curve, float x )
	{
		const int count = curve.m_count < 2 ? 2 : ( curve.m_count > max_points ? max_points : curve.m_count );

		x = clamp01( x );
		if ( x <= curve.m_x[ 0 ] )
			return curve.m_y[ 0 ];
		if ( x >= curve.m_x[ count - 1 ] )
			return curve.m_y[ count - 1 ];

		float secant[ max_points ], tangent[ max_points ];
		for ( int i = 0; i < count - 1; i++ ) {
			const float width = curve.m_x[ i + 1 ] - curve.m_x[ i ];
			secant[ i ]       = width > 1e-6f ? ( curve.m_y[ i + 1 ] - curve.m_y[ i ] ) / width : 0.f;
		}

		tangent[ 0 ]         = secant[ 0 ];
		tangent[ count - 1 ] = secant[ count - 2 ];
		for ( int i = 1; i < count - 1; i++ )
			tangent[ i ] = secant[ i - 1 ] * secant[ i ] <= 0.f ? 0.f : ( secant[ i - 1 ] + secant[ i ] ) * 0.5f;

		for ( int i = 0; i < count - 1; i++ ) {
			if ( secant[ i ] == 0.f ) {
				tangent[ i ]     = 0.f;
				tangent[ i + 1 ] = 0.f;
				continue;
			}

			const float a = tangent[ i ] / secant[ i ], b = tangent[ i + 1 ] / secant[ i ];
			const float s = a * a + b * b;
			if ( s > 9.f ) {
				const float t    = 3.f / std::sqrt( s );
				tangent[ i ]     = t * a * secant[ i ];
				tangent[ i + 1 ] = t * b * secant[ i ];
			}
		}

		int i = 0;
		while ( i < count - 2 && x > curve.m_x[ i + 1 ] )
			i++;

		const float width = curve.m_x[ i + 1 ] - curve.m_x[ i ];
		if ( width <= 1e-6f )
			return curve.m_y[ i + 1 ];

		const float t = ( x - curve.m_x[ i ] ) / width, t2 = t * t, t3 = t2 * t;
		const float y = ( 2.f * t3 - 3.f * t2 + 1.f ) * curve.m_y[ i ] + ( t3 - 2.f * t2 + t ) * width * tangent[ i ] +
		                ( -2.f * t3 + 3.f * t2 ) * curve.m_y[ i + 1 ] + ( t3 - t2 ) * width * tangent[ i + 1 ];
		return clamp01( y );
	}

	// endpoints slide vertically only; x clamped between neighbours so a drag never reorders the list
	inline void move_point( curve_t& curve, const int index, const float x, const float y )
	{
		if ( index < 0 || index >= curve.m_count )
			return;

		curve.m_y[ index ] = clamp01( y );

		if ( index == 0 || index == curve.m_count - 1 ) {
			curve.m_x[ index ] = index == 0 ? 0.f : 1.f;
			return;
		}

		const float low  = curve.m_x[ index - 1 ] + min_gap;
		const float high = curve.m_x[ index + 1 ] - min_gap;
		if ( low < high )
			curve.m_x[ index ] = x < low ? low : ( x > high ? high : x );
	}

	inline int add_point( curve_t& curve, const float x, const float y )
	{
		if ( curve.m_count >= max_points || !( x > curve.m_x[ 0 ] && x < curve.m_x[ curve.m_count - 1 ] ) )
			return -1;

		int index = 1;
		while ( index < curve.m_count && curve.m_x[ index ] < x )
			index++;

		if ( x - curve.m_x[ index - 1 ] < min_gap || curve.m_x[ index ] - x < min_gap )
			return -1;

		for ( int k = curve.m_count; k > index; k-- ) {
			curve.m_x[ k ] = curve.m_x[ k - 1 ];
			curve.m_y[ k ] = curve.m_y[ k - 1 ];
		}

		curve.m_x[ index ] = x;
		curve.m_y[ index ] = clamp01( y );
		curve.m_count++;
		return index;
	}

	inline void remove_point( curve_t& curve, const int index )
	{
		if ( index <= 0 || index >= curve.m_count - 1 )
			return;

		for ( int k = index; k < curve.m_count - 1; k++ ) {
			curve.m_x[ k ] = curve.m_x[ k + 1 ];
			curve.m_y[ k ] = curve.m_y[ k + 1 ];
		}

		curve.m_count--;
		curve.m_x[ curve.m_count ] = 0.f;
		curve.m_y[ curve.m_count ] = 0.f;
	}

	// config into curve. first x < 0 (or nan) ends the list; a hand edited point that breaks the order is
	// dropped, endpoints re-pinned. unused slots stay zero so two loads of one config memcmp equal
	inline curve_t load( const std::vector< float >& values )
	{
		curve_t curve{ };
		curve.m_count = 0;

		for ( std::size_t i = 0; i + 1 < values.size( ) && curve.m_count < max_points; i += 2 ) {
			const float x = values[ i ];
			if ( !( x >= 0.f && x <= 1.f ) )
				break;

			if ( curve.m_count > 0 && x < curve.m_x[ curve.m_count - 1 ] + min_gap )
				continue;

			curve.m_x[ curve.m_count ] = x;
			curve.m_y[ curve.m_count ] = clamp01( values[ i + 1 ] );
			curve.m_count++;
		}

		if ( curve.m_count < 2 )
			return curve_t{ };

		curve.m_x[ 0 ]                 = 0.f;
		curve.m_x[ curve.m_count - 1 ] = 1.f;
		return curve;
	}

	inline void store( const curve_t& curve, std::vector< float >& values )
	{
		values.assign( config_size, -1.f );

		for ( int i = 0; i < curve.m_count && i < max_points; i++ ) {
			values[ i * 2 ]     = curve.m_x[ i ];
			values[ i * 2 + 1 ] = curve.m_y[ i ];
		}
	}
}
