#pragma once
#include <numbers>

class c_angle;
class c_vector;
struct matrix3x4_t;

constexpr float rad2deg( const float x )
{
	return x * ( 180.f / std::numbers::pi_v< float > );
}

constexpr float deg2rad( const float x )
{
	return x * ( std::numbers::pi_v< float > / 180.f );
}

namespace n_math
{
	struct impl_t {
		float normalize_angle( float angle, float start = -180.f, float end = 180.f ) const;

		void sin_cos( float radians, float* sine, float* cosine ) const;

		c_vector interpolate_vector( const c_vector from, const c_vector to, const float percentage );

		void angle_vectors( const c_angle& angle, c_vector* forward, c_vector* right = nullptr, c_vector* up = nullptr );

		void angle_matrix( const c_angle& angles, matrix3x4_t& matrix );

		c_angle calculate_angle( const c_vector& start, const c_vector& end );

		float calculate_fov( const c_angle& view_point, const c_angle& aim_angle );

		c_vector vector_angle( const c_vector vector );

		void vector_angles( const c_vector& forward, c_angle& view );

		c_vector vector_transform( const c_vector& transform, const matrix3x4_t& matrix );

		std::int32_t time_to_ticks( float time );

		float ticks_to_time( std::int32_t ticks );

		template< typename T >
		constexpr T divide_if_less( T num, T div )
		{
			return ( num >= div ? div : num ) / div;
		};

	};
}

inline n_math::impl_t g_math{ };
