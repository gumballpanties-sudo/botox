#include "misc.h"
#include "particle_shapes.h"

#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"

#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

/* precipitation type clipart: soul shapes falling like snow around the view, main thread ( kill_effects_paint ).
   a column needs sky straight over eye height ( roof = no flake, like real snow ), fall path + landing traced once at spawn.
   area = cylinder sheared along the wind, so at eye height it stays centred on the eye at any wind */
namespace
{
	constexpr float k_top     = 512.f;   // spawn at most this far over the eye
	constexpr float k_sky     = 16384.f; // sky search
	constexpr float k_depth   = 2048.f;  // lowest fall under eye
	constexpr float k_fade_in = 0.4f;
	constexpr float k_settle  = 0.5f;    // sway fades out over the last s before landing, glyph lands on the traced point
	constexpr float k_tau     = 6.2831853f;

	struct flake_t {
		c_vector m_from{ }, m_vel{ };
		float m_fall = -1.f; // s to landing, < 0 = falls past k_depth
		float m_life = 0.f, m_age = 0.f, m_shown = 0.f;
		float m_size = 1.f, m_spin = 0.f, m_spin_rate = 1.f, m_phase = 0.f, m_hue = 0.f;
		// 3d rotation: tumble axis + start plane ( unit, u0 . v0 = 0 ), landing surface normal
		c_vector m_axis{ 0.f, 0.f, 1.f }, m_u0{ 1.f, 0.f, 0.f }, m_v0{ 0.f, 1.f, 0.f }, m_normal{ 0.f, 0.f, 1.f };
	};

	std::vector< flake_t > g_flakes{ };
	std::minstd_rand g_rng{ 0x5eedu };
	int g_warm   = 0;    // spawn attempts that start mid life ( first fill, refill after leaving the area )
	bool g_fresh = true; // arm the first fill once, not every frame an indoor map leaves the list empty
	int g_fails  = 0;    // failed spawns in a row: indoors every column hits a roof, back off to a trickle

	float rnd( const float lo, const float hi )
	{
		return std::uniform_real_distribution< float >( lo, hi )( g_rng );
	}

	c_vector rand_unit( )
	{
		const float z = rnd( -1.f, 1.f ), a = rnd( 0.f, k_tau ), r = std::sqrt( 1.f - z * z );
		return c_vector( r * std::cos( a ), r * std::sin( a ), z );
	}

	// area centre at height z: upwind above the eye, downwind below
	c_vector area_centre( const c_vector& eye, const c_vector& wind, const float speed, const float z )
	{
		const float lead = ( z - eye.m_z ) / speed;
		return c_vector( eye.m_x - wind.m_x * lead, eye.m_y - wind.m_y * lead, z );
	}

	bool spawn( flake_t& f, const c_vector& eye, const float radius, const float speed, const c_vector& wind, const float ground_time, const bool warm )
	{
		const float angle    = rnd( 0.f, k_tau ), dist = radius * std::sqrt( rnd( 0.f, 1.f ) );
		const c_vector top   = area_centre( eye, wind, speed, eye.m_z + k_top );
		const c_vector base( top.m_x + std::cos( angle ) * dist, top.m_y + std::sin( angle ) * dist, eye.m_z );

		static c_trace_filter_world world{ };
		trace_t up{ };
		g_interfaces.m_engine_trace->trace_ray( ray_t( base, base + c_vector( 0.f, 0.f, k_sky ) ), e_mask::mask_solid_brushonly, &world, &up );
		if ( up.m_start_solid || up.m_fraction >= 1.f || !( up.surface.m_flags & e_surf_type::surf_sky ) )
			return false;

		f.m_from = c_vector( base.m_x, base.m_y, ( std::min )( up.m_end.m_z - 4.f, eye.m_z + k_top ) );
		f.m_vel  = c_vector( wind.m_x, wind.m_y, -speed * rnd( 0.8f, 1.2f ) );

		// world + static props only: trace_everything never hands static props to the filter, every entity is skipped
		// ( players, nades, doors move after the landing is fixed )
		const float span = ( f.m_from.m_z - ( eye.m_z - k_depth ) ) / -f.m_vel.m_z;
		static c_trace_filter solid( [ ]( c_base_entity*, int ) { return false; } ); // main thread only, built once
		trace_t down{ };
		g_interfaces.m_engine_trace->trace_ray( ray_t( f.m_from, f.m_from + f.m_vel * span ), e_mask::mask_solid | e_contents::contents_water, &solid, &down );

		f.m_fall      = down.m_fraction < 1.f && !down.m_start_solid ? span * down.m_fraction : -1.f;
		f.m_life      = f.m_fall >= 0.f ? f.m_fall + ground_time : span;
		f.m_size      = rnd( 0.75f, 1.25f );
		f.m_spin      = rnd( 0.f, 1.f );
		f.m_spin_rate = rnd( 0.5f, 1.5f ) * ( rnd( 0.f, 1.f ) < 0.5f ? -1.f : 1.f );
		f.m_phase     = rnd( 0.f, k_tau );
		f.m_hue       = rnd( 0.f, 1.f );
		f.m_normal    = f.m_fall >= 0.f ? down.m_plane.m_normal : c_vector( 0.f, 0.f, 1.f );
		f.m_axis      = rand_unit( );
		f.m_u0        = rand_unit( );
		for ( int tries = 0; tries < 8; tries++ ) {
			const c_vector r = rand_unit( );
			f.m_v0           = r - f.m_u0 * f.m_u0.dot_product( r );
			if ( f.m_v0.length( ) > 0.1f )
				break;
		}
		f.m_v0 = f.m_v0.normalized( );
		f.m_age       = warm ? rnd( 0.f, f.m_life ) : 0.f;
		f.m_shown     = 0.f;
		return true;
	}
} // namespace

void precip_clipart_paint( const float dt, const c_vector& eye, const c_vector& fwd )
{
	if ( !g_interfaces.m_engine_client->is_in_game( ) || !GET_VARIABLE( g_variables.m_precipitation, bool ) ||
	     GET_VARIABLE( g_variables.m_precipitation_type, int ) != k_precip_clipart ) {
		g_flakes.clear( );
		g_warm  = 0;
		g_fresh = true;
		g_fails = 0;
		return;
	}

	const int target          = std::clamp( GET_VARIABLE( g_variables.m_precipitation_clipart_density, int ), 10, 3000 );
	const float radius        = std::clamp( GET_VARIABLE( g_variables.m_precipitation_clipart_radius, float ), 100.f, 4000.f );
	const float size          = std::clamp( GET_VARIABLE( g_variables.m_precipitation_clipart_size, float ), 0.1f, 32.f );
	const float spin          = GET_VARIABLE( g_variables.m_precipitation_clipart_spin, float );
	const float sway          = GET_VARIABLE( g_variables.m_precipitation_clipart_sway, float );
	const int shape           = std::clamp( GET_VARIABLE( g_variables.m_precipitation_clipart_shape, int ), 0, n_particle_shape::k_last );
	const int color_mode      = GET_VARIABLE( g_variables.m_precipitation_clipart_color_mode, int );
	const int glow_mode       = GET_VARIABLE( g_variables.m_precipitation_clipart_glow, int );
	const c_color& picked     = GET_VARIABLE( g_variables.m_precipitation_clipart_color, c_color );
	const c_color& glow_color = GET_VARIABLE( g_variables.m_precipitation_clipart_glow_color, c_color );
	const float saturation    = std::clamp( GET_VARIABLE( g_variables.m_precipitation_clipart_saturation, float ), 0.f, 1.f );
	const float hue_shift     = color_mode == 2 ? GET_VARIABLE( g_variables.m_precipitation_clipart_rainbow_speed, float ) * g_interfaces.m_global_vars_base->m_real_time : 0.f;
	const float lift          = shape ? n_particle_shape::k_scale : 1.f; // glyph bottom sits on the floor
	const bool planar         = GET_VARIABLE( g_variables.m_precipitation_clipart_3d, bool );

	const float wind_speed  = GET_VARIABLE( g_variables.m_precipitation_clipart_wind, float );
	const float wind_yaw    = GET_VARIABLE( g_variables.m_precipitation_clipart_wind_yaw, float ) * ( k_tau / 360.f );
	const c_vector wind( std::cos( wind_yaw ) * wind_speed, std::sin( wind_yaw ) * wind_speed, 0.f );
	const float speed       = std::clamp( GET_VARIABLE( g_variables.m_precipitation_clipart_speed, float ), 1.f, 2000.f );
	const float ground_time = ( std::max )( GET_VARIABLE( g_variables.m_precipitation_clipart_ground_time, float ), 0.f );

	if ( g_fresh ) {
		g_warm  = target * 3;
		g_fresh = false;
	}

	std::size_t live = 0;
	for ( flake_t& f : g_flakes ) {
		f.m_age += dt;
		f.m_shown += dt;
		if ( f.m_age >= f.m_life )
			continue;

		const bool landed  = f.m_fall >= 0.f && f.m_age >= f.m_fall;
		const float t      = landed ? f.m_fall : f.m_age;
		const float s      = f.m_size * size;
		const float settle = f.m_fall >= 0.f ? std::clamp( ( f.m_fall - t ) / k_settle, 0.f, 1.f ) : 1.f; // 1 falling, 0 landed
		const float wobble = sway * settle;
		c_vector at( f.m_from.m_x + f.m_vel.m_x * t + std::sin( t * 1.7f + f.m_phase ) * wobble,
		             f.m_from.m_y + f.m_vel.m_y * t + std::cos( t * 1.1f + f.m_phase * 1.7f ) * wobble, f.m_from.m_z + f.m_vel.m_z * t );
		if ( planar )
			at = at + f.m_normal * ( 0.3f + s * lift * settle ); // lies flat 0.3 u off the surface, tumbles clear of it
		else
			at.m_z += s * lift;

		const c_vector centre = area_centre( eye, wind, speed, at.m_z );
		const float dx = at.m_x - centre.m_x, dy = at.m_y - centre.m_y;
		const float dist = std::sqrt( dx * dx + dy * dy );
		if ( dist > radius ) {
			g_warm += 3;
			continue;
		}
		g_flakes[ live++ ] = f;

		/* more than 120 deg off the view ( about a third of the area ) = no quads, the sim above still ran. fwd is last
		   frame's: the wide cone keeps a fast flick from showing edge flakes a frame late; margin = glow reach */
		const c_vector rel = at - eye;
		if ( rel.dot_product( fwd ) < -0.5f * rel.length( ) - s * 2.8f )
			continue;

		float alpha = ( std::min )( f.m_shown / k_fade_in, 1.f ) * std::clamp( ( radius - dist ) / ( radius * 0.15f ), 0.f, 1.f );
		if ( landed )
			alpha *= 1.f - ( f.m_age - f.m_fall ) / ( f.m_life - f.m_fall );
		if ( alpha <= 0.01f )
			continue;

		float spin_at = f.m_spin + f.m_spin_rate * spin * t;
		spin_at -= std::floor( spin_at );

		c_color color = picked;
		if ( color_mode ) {
			// multicolor = own hue per glyph, rainbow = shared hue cycling, glyphs spread 0.15 around it
			float hue = f.m_hue * ( color_mode == 2 ? 0.15f : 1.f ) + hue_shift;
			hue -= std::floor( hue );
			color = c_color::from_hsb( hue, saturation, 1.f, picked.base< e_color_type::color_type_a >( ) );
		}
		const c_color* glow = glow_mode == 1 ? &color : glow_mode == 2 ? &glow_color : nullptr;
		if ( planar ) {
			// spin = tumble speed; the halo stays facing the view ( a glow edge on would vanish )
			const float angle = ( f.m_spin + f.m_spin_rate * spin * t ) * k_tau, land = ( f.m_spin + f.m_spin_rate * spin * f.m_fall ) * k_tau;
			c_vector u{ }, v{ };
			n_particle_shape::tumble_frame( &f.m_axis.m_x, &f.m_u0.m_x, &f.m_v0.m_x, angle, land, &f.m_normal.m_x, settle, &u.m_x, &v.m_x );
			if ( glow )
				kill_effects_shape( at, s, 0, color, glow, alpha * 0.5f, 0.f );
			kill_effects_shape_plane( at, u, v, s, shape, color, alpha );
			continue;
		}
		// soul's circle + glow is the glow alone; here the disc stays on top so the glyph colour shows
		if ( !shape && glow )
			kill_effects_shape( at, s, 0, color, glow, alpha * 0.5f, 0.f );
		kill_effects_shape( at, s, shape, color, shape ? glow : nullptr, alpha, spin_at );
	}
	g_flakes.resize( live );
	g_warm = ( std::min )( g_warm, target * 3 );

	// ponytail: fixed attempt cap; 256 misses in a row ( indoors, every column hits a roof ) = 4 a frame until one lands
	int attempts = ( std::min )( target - static_cast< int >( g_flakes.size( ) ), g_warm > 0 ? 64 : 32 );
	if ( g_fails >= 256 )
		attempts = ( std::min )( attempts, 4 );
	for ( int i = 0; i < attempts; i++ ) {
		const bool warm = g_warm > 0;
		if ( warm )
			--g_warm;
		flake_t f{ };
		if ( spawn( f, eye, radius, speed, wind, ground_time, warm ) ) {
			g_flakes.push_back( f );
			g_fails = 0;
		} else
			g_fails = ( std::min )( g_fails + 1, 1 << 20 );
	}
}
