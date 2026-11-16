#pragma once
#include <algorithm>
#include <cmath>
#include <vector>

/* CRadar port (gta_sa.exe 1.0 US, exe-verified): radar space = unit circle, +y up, map + blips through
   TransformRealWorldPointToRadarSpace 0x583530 / LimitRadarPoint 0x5832F0 / TransformRadarPointToScreenSpace 0x583480,
   map shape = DrawRadarMask's 24-gon (15 deg steps), blips ShowRadarTraceWithHeight 0x584070, arrow
   DrawRotatingRadarSprite 0x584850. pure: no d3d / imgui (tools/gta_hud_test.cpp) */
namespace n_gta_radar
{
	struct vec_t {
		float x = 0.f, y = 0.f;
	};

	inline constexpr float k_pi          = 3.14159265f;
	// csgo inch units per sa metre for the +2 / -4 m blip shapes. deliberately true scale, not the radar zoom option:
	// at 11 u/m a jump or a stair flips the shape, at 39.37 it tracks floors like sa
	inline constexpr float k_units_per_m = 39.37f;

	struct radar_t {
		float m_cx = 0.f, m_cy = 0.f, m_rx = 1.f, m_ry = 1.f; // screen centre (SX(87), SFB(66)), radii (SX(47), SY(38))
		float m_ox = 0.f, m_oy = 0.f;                         // vec2DRadarOrigin (world)
		float m_range = 1.f;                                  // m_radarRange (world units)
		float m_sin = 0.f, m_cos = 1.f;                       // cached orientation

		// csgo view yaw (deg, 0 = +x) -> sa heading (0 = +y, ccw)
		void set_yaw( float yaw_deg )
		{
			const float h = yaw_deg * k_pi / 180.f - k_pi / 2.f;
			m_sin = std::sin( h ), m_cos = std::cos( h );
		}
		float heading( ) const { return std::atan2( m_sin, m_cos ); }

		vec_t to_radar( float wx, float wy ) const
		{
			const float dx = ( wx - m_ox ) / m_range, dy = ( wy - m_oy ) / m_range;
			return { m_cos * dx + m_sin * dy, -m_sin * dx + m_cos * dy };
		}
		// inverse of to_radar (DrawRadarSection uv path)
		vec_t to_world( vec_t r ) const { return { m_ox + ( m_cos * r.x - m_sin * r.y ) * m_range, m_oy + ( m_sin * r.x + m_cos * r.y ) * m_range }; }
		vec_t to_screen( vec_t r ) const { return { m_cx + m_rx * r.x, m_cy - m_ry * r.y }; }
	};

	// clamps to the unit circle, returns the pre-clamp length (1 = radar edge)
	inline float limit( vec_t& r )
	{
		const float mag = std::sqrt( r.x * r.x + r.y * r.y );
		if ( mag > 1.f )
			r.x /= mag, r.y /= mag;
		return mag;
	}

	// DrawRadarMask: visible map = 24-gon on the unit circle
	inline std::vector< vec_t > mask_polygon( )
	{
		std::vector< vec_t > p( 24 );
		for ( int k = 0; k < 24; ++k )
			p[ k ] = { std::cos( k * 0.261799f ), std::sin( k * 0.261799f ) };
		return p;
	}

	// sutherland-hodgman: subject polygon clipped by a convex ccw clip polygon (ClipRadarPoly + the depth mask)
	inline std::vector< vec_t > clip( std::vector< vec_t > subject, const std::vector< vec_t >& convex )
	{
		for ( size_t e = 0; e < convex.size( ) && !subject.empty( ); ++e ) {
			const vec_t a = convex[ e ], b = convex[ ( e + 1 ) % convex.size( ) ];
			const auto inside = [ & ]( vec_t p ) { return ( b.x - a.x ) * ( p.y - a.y ) - ( b.y - a.y ) * ( p.x - a.x ) >= 0.f; };
			const auto cross  = [ & ]( vec_t p, vec_t q ) {
                const float d1 = ( b.x - a.x ) * ( p.y - a.y ) - ( b.y - a.y ) * ( p.x - a.x );
                const float d2 = ( b.x - a.x ) * ( q.y - a.y ) - ( b.y - a.y ) * ( q.x - a.x );
                const float t  = d1 / ( d1 - d2 );
                return vec_t{ p.x + ( q.x - p.x ) * t, p.y + ( q.y - p.y ) * t };
			};
			std::vector< vec_t > out{ };
			for ( size_t i = 0; i < subject.size( ); ++i ) {
				const vec_t p = subject[ i ], q = subject[ ( i + 1 ) % subject.size( ) ];
				const bool pi = inside( p ), qi = inside( q );
				if ( pi )
					out.push_back( p );
				if ( pi != qi )
					out.push_back( cross( p, q ) );
			}
			subject = std::move( out );
		}
		return subject;
	}

	// ShowRadarTraceWithHeight: 2 = box, 0 = target above (up triangle), 1 = target below (down triangle)
	enum e_height : int { height_low, height_high, height_normal };

	inline int height_of( float dz_units )
	{
		const float dz = dz_units / k_units_per_m;
		return dz > 2.f ? height_low : dz < -4.f ? height_high : height_normal;
	}

	// shape in screen px: n = 3 (triangle) or 4 (box), border = true for the black outline drawn first
	struct shape_t {
		vec_t m_p[ 4 ];
		int m_n = 0;
	};

	inline shape_t trace_shape( float x, float y, int size, int height, bool border, float sx_unit, float sy_unit )
	{
		const auto SX = [ & ]( float v ) { return v * sx_unit; };
		const auto SY = [ & ]( float v ) { return v * sy_unit; };
		const float s = static_cast< float >( size );
		shape_t o{ };
		if ( height == height_normal ) {
			const float k = border ? s + 1.f : s;
			o.m_n = 4;
			o.m_p[ 0 ] = { x - SX( k ), y - SY( k ) }, o.m_p[ 1 ] = { x + SX( k ), y - SY( k ) };
			o.m_p[ 2 ] = { x + SX( k ), y + SY( k ) }, o.m_p[ 3 ] = { x - SX( k ), y + SY( k ) };
			return o;
		}
		o.m_n = 3;
		const float apex = border ? s + 3.f : s + 1.f, side = border ? s + 3.f : s + 1.f, base = border ? s + 2.f : s + 1.f;
		if ( height == height_high ) // down: apex below
			o.m_p[ 0 ] = { x, y + SY( apex ) }, o.m_p[ 1 ] = { x - SX( side ), y - SY( base ) }, o.m_p[ 2 ] = { x + SX( side ), y - SY( base ) };
		else // up: apex above
			o.m_p[ 0 ] = { x, y - SY( apex ) }, o.m_p[ 1 ] = { x + SX( side ), y + SY( base ) }, o.m_p[ 2 ] = { x - SX( side ), y + SY( base ) };
		return o;
	}

	// DrawRotatingRadarSprite: corners at half-diagonal w / h, uv (0,0) v0, (1,0) v1, (1,1) v2, (0,1) v3
	inline void rotating_quad( float x, float y, float angle, float w, float h, vec_t out[ 4 ] )
	{
		for ( int i = 0; i < 4; ++i ) {
			const float th = angle - 0.785398f + static_cast< float >( i ) * 1.5708f;
			out[ i ]       = { x + w * std::sin( th ), y + h * std::cos( th ) };
		}
	}

	// arrow: angle = playerHeading - (orientation + pi), headings in sa convention
	inline float arrow_angle( float body_yaw_deg, const radar_t& r ) { return ( body_yaw_deg * k_pi / 180.f - k_pi / 2.f ) - ( r.heading( ) + k_pi ); }
}
