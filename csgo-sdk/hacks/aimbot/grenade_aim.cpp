#include "grenade_aim.h"
#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"
#include "../../utilities/input/input.h"
#include "../entity_cache/entity_cache.h"
#include "../lagcomp/lagcomp.h"
#include "../misc/misc.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>

extern void botox_dbg_log( const char* fmt, ... );
extern void start_movement_fix( c_user_cmd* cmd );
extern void end_movement_fix( c_user_cmd* cmd );

namespace
{
	constexpr float k_gravity_modifier            = 0.4f;
	constexpr float k_underhand_dampening         = 0.3f;
	constexpr float k_underhand_lower             = 12.f;
	constexpr float k_launch_offset               = 16.f;
	constexpr unsigned int k_contents_grenadeclip = 0x80000;
	constexpr int k_path_max                      = 400;
	constexpr float k_elasticity                  = 0.45f; /* GetGrenadeElasticity, world surface 1 */
	constexpr float k_hull                        = 2.f;
	constexpr int k_wall_rays                     = 12;
	constexpr float k_wall_range                  = 512.f;
	constexpr float k_bounce_slop                 = 16.f;

	using path_t = std::array< c_vector, k_path_max >;

	/* missing netvar = offset 0: return the fallback, never read the vtable pointer */
	template< typename T >
	T read_netvar( c_base_entity* entity, const unsigned int offset, const T fallback )
	{
		return offset ? *reinterpret_cast< T* >( reinterpret_cast< std::uintptr_t >( entity ) + offset ) : fallback;
	}

	/* menu slot order = m_grenade_aim_types labels in menu.cpp */
	int grenade_slot( const short item_index )
	{
		switch ( item_index ) {
		case e_item_definition_index::weapon_flashbang:
			return 0;
		case e_item_definition_index::weapon_hegrenade:
			return 1;
		case e_item_definition_index::weapon_smokegrenade:
			return 2;
		case e_item_definition_index::weapon_molotov:
		case e_item_definition_index::weapon_incgrenade:
			return 3;
		case e_item_definition_index::weapon_decoy:
			return 4;
		case e_item_definition_index::weapon_tagrenade:
			return 5;
		default:
			return -1;
		}
	}

	float max_flight_time( const short item_index )
	{
		const int slot    = grenade_slot( item_index );
		const auto& types = GET_VARIABLE( g_variables.m_grenade_aim_types, std::vector< bool > );
		if ( slot < 0 || slot >= static_cast< int >( types.size( ) ) || !types[ slot ] )
			return 0.f;

		return slot <= 1 ? 1.5f : slot == 3 ? 2.f : 3.f;
	}

	/* CBaseCSGrenade::ThrowGrenade inputs ( weapon_basecsgrenade.cpp:555 ) */
	struct throw_t {
		c_vector m_eye     = { };
		c_vector m_inherit = { };
		float m_speed      = 0.f;
		float m_gravity    = 0.f;
		float m_max_time   = 0.f;
	};

	struct motion_t {
		c_vector m_center   = { };
		c_vector m_velocity = { };
		float m_lead        = 0.f;
		float m_gravity     = 0.f;
		float m_floor_z     = -1e9f;
		float m_wall_time   = 1e9f;
		bool m_airborne     = false;

		c_vector at( const float t ) const
		{
			const float tau = std::min( t + m_lead, m_wall_time );
			c_vector out    = m_center + m_velocity * tau;

			if ( m_airborne )
				out.m_z = std::max( out.m_z - 0.5f * m_gravity * tau * tau, m_floor_z );

			return out;
		}
	};

	/* ponytail: linear run / ballistic fall, floor under the start only; a strafing target over 1.5 s needs a movement sim */
	motion_t target_motion( c_base_entity* entity, const float lead, const float max_time, const float gravity )
	{
		const float height = 36.f - 9.f * entity->get_duck_amount( );

		motion_t out{ };
		out.m_center   = entity->get_abs_origin( ) + c_vector( 0.f, 0.f, height );
		out.m_velocity = entity->get_velocity( );
		out.m_lead     = lead;
		out.m_gravity  = gravity;
		out.m_airborne = !( entity->get_flags( ) & fl_onground );

		c_trace_filter_world filter{ };
		trace_t trace = { };

		if ( out.m_airborne ) {
			g_interfaces.m_engine_trace->trace_ray( ray_t( out.m_center, out.m_center - c_vector( 0.f, 0.f, 4096.f ) ), e_mask::mask_playersolid,
			                                        &filter, &trace );
			out.m_floor_z = trace.m_end.m_z + height;
		} else {
			out.m_velocity.m_z = 0.f;

			const float span = max_time + lead;
			g_interfaces.m_engine_trace->trace_ray( ray_t( out.m_center, out.m_center + out.m_velocity * span ), e_mask::mask_playersolid, &filter,
			                                        &trace );
			out.m_wall_time = span * trace.m_fraction;
		}

		return out;
	}

	c_vector launch_delta( const throw_t& th, const c_vector& target, const float t )
	{
		c_vector delta = target - th.m_eye - th.m_inherit * t;
		delta.m_z += 0.5f * th.m_gravity * t * t;
		return delta;
	}

	/* first 2 sign changes of miss( t ), tick scan + bisect. root 0 = low arc, root 1 = lob */
	template< typename F >
	int find_roots( const F& miss, const float max_time, float out[ 2 ] )
	{
		const float step = g_interfaces.m_global_vars_base->m_interval_per_tick;
		if ( step <= 0.f )
			return 0;

		int count    = 0;
		float last_t = step;
		float last   = miss( last_t );

		for ( float t = step * 2.f; t <= max_time && count < 2; t += step ) {
			const float now = miss( t );

			if ( ( now > 0.f ) != ( last > 0.f ) ) {
				float lo = last_t, hi = t;

				for ( int i = 0; i < 16; ++i ) {
					const float mid = ( lo + hi ) * 0.5f;

					if ( ( miss( mid ) > 0.f ) == ( last > 0.f ) )
						lo = mid;
					else
						hi = mid;
				}

				out[ count++ ] = ( lo + hi ) * 0.5f;
			}

			last_t = t;
			last   = now;
		}

		return count;
	}

	/* eye + dir * ( 16 + v t ) + u t - g t^2 / 2 = target, so |launch_delta| = 16 + v t */
	int flight_times( const throw_t& th, const motion_t& motion, float out[ 2 ] )
	{
		return find_roots( [ & ]( const float t ) { return launch_delta( th, motion.at( t ), t ).length( ) - ( k_launch_offset + th.m_speed * t ); },
		                   th.m_max_time, out );
	}

	/* vertical wall, plane for the grenade center: n . x = dist ( surface + box hull offset ) */
	struct wall_t {
		c_vector m_normal = { };
		float m_dist      = 0.f;
	};

	/* ponytail: 12 flat rays from the target only, walls it can't see are missed; trace from the eye too if lobs over cover matter */
	int find_walls( const c_vector& center, wall_t out[ k_wall_rays ] )
	{
		c_trace_filter_world filter{ };
		int count = 0;

		for ( int i = 0; i < k_wall_rays; ++i ) {
			const float yaw = 2.f * std::numbers::pi_v< float > * static_cast< float >( i ) / k_wall_rays;
			const c_vector dir( std::cos( yaw ), std::sin( yaw ), 0.f );

			trace_t trace = { };
			g_interfaces.m_engine_trace->trace_ray( ray_t( center, center + dir * k_wall_range ), e_mask::mask_solid | k_contents_grenadeclip, &filter,
			                                        &trace );

			if ( trace.m_fraction >= 1.f || trace.m_start_solid || std::fabsf( trace.m_plane.m_normal.m_z ) > 0.05f )
				continue;

			const c_vector normal = c_vector( trace.m_plane.m_normal.m_x, trace.m_plane.m_normal.m_y, 0.f ).normalized( );
			const float dist      = trace.m_end.dot_product( normal ) + k_hull * ( std::fabsf( normal.m_x ) + std::fabsf( normal.m_y ) );

			const bool seen = std::any_of( out, out + count, [ & ]( const wall_t& wall ) {
				return wall.m_normal.dot_product( normal ) > 0.99f && std::fabsf( wall.m_dist - dist ) < 4.f;
			} );

			if ( !seen )
				out[ count++ ] = { normal, dist };
		}

		return count;
	}

	/* total launch velocity from spawn that reaches target at t after one reflect * 0.45 off the wall:
	   normal speed s in, 0.45 s out; tangent and z scaled 0.45 at the hit ( ResolveFlyCollisionCustom, basecsgrenade_projectile.cpp:461 ) */
	bool bounce_velocity( const throw_t& th, const wall_t& wall, const c_vector& spawn, const c_vector& target, const float t, c_vector& velocity,
	                      float& hit_time )
	{
		const float e   = spawn.dot_product( wall.m_normal ) - wall.m_dist;
		const float tau = target.dot_product( wall.m_normal ) - wall.m_dist;
		if ( e <= 0.f || tau <= 0.f )
			return false;

		const float s    = ( e + tau / k_elasticity ) / t;
		const float t1   = e / s;
		const float t2   = t - t1;
		const float span = t1 + k_elasticity * t2;
		const float g    = th.m_gravity;

		const c_vector side( -wall.m_normal.m_y, wall.m_normal.m_x, 0.f );
		const float vt = ( target - spawn ).dot_product( side ) / span;
		const float vz = ( target.m_z - spawn.m_z + 0.5f * g * t1 * t1 + k_elasticity * g * t1 * t2 + 0.5f * g * t2 * t2 ) / span;

		velocity       = wall.m_normal * -s + side * vt;
		velocity.m_z   = vz;
		hit_time       = t1;
		return true;
	}

	/* the 16 u spawn offset follows dir: re-solve from the moved spawn */
	bool bounce_launch( const throw_t& th, const wall_t& wall, const c_vector& target, const float t, c_vector& velocity, float& hit_time )
	{
		c_vector spawn = th.m_eye;

		for ( int i = 0; i < 3; ++i ) {
			if ( !bounce_velocity( th, wall, spawn, target, t, velocity, hit_time ) )
				return false;

			const c_vector launch = velocity - th.m_inherit;
			if ( launch.length( ) < 1.f )
				return false;

			spawn = th.m_eye + launch.normalized( ) * k_launch_offset;
		}

		return true;
	}

	int bounce_times( const throw_t& th, const wall_t& wall, const motion_t& motion, float out[ 2 ] )
	{
		return find_roots(
			[ & ]( const float t ) {
				c_vector velocity = { };
				float hit_time    = 0.f;
				return bounce_launch( th, wall, motion.at( t ), t, velocity, hit_time ) ? ( velocity - th.m_inherit ).length( ) - th.m_speed : 1e9f;
			},
			th.m_max_time, out );
	}

	/* PhysicsClipVelocity backoff 2 + STOP_EPSILON */
	c_vector reflect( const c_vector& in, const c_vector& normal )
	{
		c_vector out = in - normal * ( in.dot_product( normal ) * 2.f );

		if ( std::fabsf( out.m_x ) < 0.1f )
			out.m_x = 0.f;
		if ( std::fabsf( out.m_y ) < 0.1f )
			out.m_y = 0.f;
		if ( std::fabsf( out.m_z ) < 0.1f )
			out.m_z = 0.f;

		return out;
	}

	/* inverse of ThrowGrenade's pitch -= 10 * ( 90 - |pitch| ) / 90 */
	bool view_pitch( const float thrown, float& view )
	{
		view = thrown >= -10.f ? ( thrown + 10.f ) * 0.9f : ( thrown + 10.f ) * 1.125f;
		return view >= -89.f && view <= 89.f;
	}

	/* n_grenade_path::simulate steps; false = launch blocked or world / other player before the target.
	   wall = exactly one bounce off that plane, then the last tick must land within k_bounce_slop of end */
	bool trace_path( const throw_t& th, const c_vector& dir, const float time, const c_vector& end, c_base_entity* target, const wall_t* wall,
	                 path_t& path, int& count )
	{
		const c_vector mins( -2.f, -2.f, -2.f ), maxs( 2.f, 2.f, 2.f );
		const unsigned int mask = e_mask::mask_solid | k_contents_grenadeclip;
		const float step        = g_interfaces.m_global_vars_base->m_interval_per_tick;

		c_trace_filter filter( [ target ]( c_base_entity* hit, int ) { return hit != g_ctx.m_local && hit != target; } );
		trace_t trace = { };

		g_interfaces.m_engine_trace->trace_ray( ray_t( th.m_eye, th.m_eye + dir * 22.f, mins, maxs ), mask, &filter, &trace );
		if ( trace.m_fraction < 1.f )
			return false;

		c_vector position = trace.m_end - dir * 6.f;
		c_vector velocity = dir * th.m_speed + th.m_inherit;

		count           = 0;
		path[ count++ ] = position;

		bool bounced = false;

		/* bounce tick writes 2, end writes 1 */
		for ( float t = step; t <= time && count < k_path_max - 3; t += step ) {
			const float vz = velocity.m_z - th.m_gravity * step;
			const c_vector move( velocity.m_x * step, velocity.m_y * step, ( velocity.m_z + vz ) * 0.5f * step );
			velocity.m_z = vz;

			g_interfaces.m_engine_trace->trace_ray( ray_t( position, position + move, mins, maxs ), mask, &filter, &trace );

			if ( trace.m_fraction < 1.f ) {
				if ( !wall || bounced || trace.m_start_solid || trace.m_plane.m_normal.dot_product( wall->m_normal ) < 0.99f )
					return false;

				bounced         = true;
				position        = trace.m_end;
				path[ count++ ] = position;
				velocity        = reflect( velocity, trace.m_plane.m_normal ) * k_elasticity;

				g_interfaces.m_engine_trace->trace_ray( ray_t( position, position + velocity * ( ( 1.f - trace.m_fraction ) * step ), mins, maxs ), mask,
				                                        &filter, &trace );
				if ( trace.m_fraction < 1.f )
					return false;
			}

			position        = trace.m_end;
			path[ count++ ] = position;
		}

		if ( wall && ( !bounced || ( position - end ).length( ) > k_bounce_slop ) )
			return false;

		g_interfaces.m_engine_trace->trace_ray( ray_t( position, end, mins, maxs ), mask, &filter, &trace );
		if ( trace.m_fraction < 1.f )
			return false;

		path[ count++ ] = end;
		return true;
	}

	float fov_to( const c_angle& view, const c_angle& aim )
	{
		c_vector a = { }, b = { };
		g_math.angle_vectors( view, &a );
		g_math.angle_vectors( aim, &b );
		return rad2deg( std::acos( std::clamp( a.dot_product( b ), -1.f, 1.f ) ) );
	}
}

void n_grenade_aim::impl_t::on_create_move( )
{
	m_throw      = false;
	m_path_count = 0;

	const auto weapon      = g_interfaces.m_client_entity_list->get< c_base_entity >( g_ctx.m_local->get_active_weapon_handle( ) );
	const short item_index = weapon ? weapon->get_item_definition_index( ) : 0;
	const float max_time   = max_flight_time( item_index );

	if ( !GET_VARIABLE( g_variables.m_grenade_aim, bool ) || max_time <= 0.f ) {
		m_auto_released = false;
		m_release_time  = 0.f;
		return;
	}

	static const unsigned int pin_offset = g_netvars[ HASH_BT( "CBaseCSGrenade->m_bPinPulled" ) ].m_offset;

	const bool pin_pulled = read_netvar( weapon, pin_offset, false );
	const float curtime   = g_interfaces.m_global_vars_base->m_current_time;

	/* stale across death / tick base correction */
	if ( m_release_time > 0.f && curtime - m_release_time > 0.5f ) {
		m_release_time  = 0.f;
		m_auto_released = false;
	}

	if ( !pin_pulled && m_release_time <= 0.f ) {
		m_auto_released = false;
		return;
	}

	/* m_fThrowTime is no pred field ( weapon_basecsgrenade.cpp:66 ), the client copy lags: mirror StartGrenadeThrow off the predicted pin.
	   ItemPostFrame throws once throw time < curtime; inside prediction begin curtime = this cmd's tick base */
	const bool throw_tick = m_release_time > 0.f && m_release_time < curtime;

	aim( weapon, item_index, max_time, pin_pulled, throw_tick );

	if ( throw_tick )
		m_release_time = 0.f;
	else if ( pin_pulled && m_release_time <= 0.f && !( g_ctx.m_cmd->m_buttons & ( in_attack | in_second_attack ) ) )
		m_release_time = curtime + 0.1f;
}

void n_grenade_aim::impl_t::aim( c_base_entity* weapon, const short item_index, const float max_time, const bool pin_pulled, const bool throw_tick )
{
	const bool key = g_input.check_input( &GET_VARIABLE( g_variables.m_grenade_aim_key, key_bind_t ) );
	if ( !key && !m_auto_released ) {
		m_lock = 0;
		return;
	}

	const auto data = g_interfaces.m_weapon_system->get_weapon_data( item_index );
	if ( !data )
		return;

	static const unsigned int strength_offset = g_netvars[ HASH_BT( "CBaseCSGrenade->m_flThrowStrength" ) ].m_offset;

	const float strength = std::clamp( read_netvar( weapon, strength_offset, 1.f ), 0.f, 1.f );
	const float gravity  = g_convars.float_or( HASH_BT( "sv_gravity" ), 800.f );

	throw_t th{ };
	th.m_eye = g_ctx.m_local->get_origin( ) + g_ctx.m_local->get_view_offset( );
	th.m_eye.m_z -= k_underhand_lower * ( 1.f - strength );
	th.m_inherit  = g_ctx.m_local->get_velocity( ) * 1.25f;
	th.m_speed    = std::clamp( data->m_throw_velocity * 0.9f, 15.f, 750.f ) * ( k_underhand_dampening + ( 1.f - k_underhand_dampening ) * strength );
	th.m_gravity  = gravity * k_gravity_modifier;
	th.m_max_time = max_time;

	const float lead      = g_lagcomp.lerp_time( ) + g_lagcomp.real_latency( );
	const float fov_limit = GET_VARIABLE( g_variables.m_grenade_aim_fov, float );
	const int arc         = GET_VARIABLE( g_variables.m_grenade_aim_arc, int );

	struct best_t {
		c_base_entity* m_entity = nullptr;
		float m_fov             = 1e9f;
		float m_time            = 0.f;
		int m_root              = 0;
		c_angle m_final         = { };
		bool m_bounce           = false;
	} best{ };

	path_t scratch    = { };
	const bool bounce = GET_VARIABLE( g_variables.m_grenade_aim_bounce, bool );

	g_entity_cache.enumerate( e_enumeration_type::type_players, [ & ]( c_base_entity* entity ) {
		if ( !entity || !entity->is_valid_aim_target( ) || entity->has_immunity( ) || !player_list_aim_allowed( entity->get_index( ) ) )
			return;

		const motion_t motion = target_motion( entity, lead, max_time, gravity );

		/* lock skips the fov gate: a non-silent lob turns the camera far off the target */
		const float fov = fov_to( g_ctx.old_view_point, g_math.calculate_angle( th.m_eye, motion.m_center ) );
		if ( ( fov > fov_limit && entity->get_index( ) != m_lock ) || fov >= best.m_fov )
			return;

		const auto take = [ & ]( c_vector dir, const float time, const int root, const wall_t* wall ) {
			if ( ( arc == 1 && root != 0 ) || ( arc == 2 && root != 1 ) || dir.length( ) < 1.f )
				return false;

			dir = dir.normalized( );

			const c_angle thrown = g_math.calculate_angle( c_vector( ), dir );

			float pitch = 0.f;
			if ( !view_pitch( thrown.m_x, pitch ) )
				return false;

			int count = 0;
			if ( !trace_path( th, dir, time, motion.at( time ), entity, wall, scratch, count ) )
				return false;

			best         = { entity, fov, time, root, c_angle( pitch, thrown.m_y, 0.f ), wall != nullptr };
			m_path       = scratch;
			m_path_count = count;
			return true;
		};

		float times[ 2 ] = { };
		const int roots  = flight_times( th, motion, times );

		for ( int root = 0; root < roots; ++root )
			if ( take( launch_delta( th, motion.at( times[ root ] ), times[ root ] ), times[ root ], root, nullptr ) )
				return;

		if ( !bounce )
			return;

		wall_t walls[ k_wall_rays ] = { };
		const int wall_count        = find_walls( motion.m_center, walls );
		const float step            = g_interfaces.m_global_vars_base->m_interval_per_tick;

		for ( int i = 0; i < wall_count; ++i ) {
			const int wall_roots = bounce_times( th, walls[ i ], motion, times );

			for ( int root = 0; root < wall_roots; ++root ) {
				c_vector velocity = { };
				float hit_time    = 0.f;

				if ( !bounce_launch( th, walls[ i ], motion.at( times[ root ] ), times[ root ], velocity, hit_time ) || hit_time < step ||
				     times[ root ] - hit_time < step )
					continue;

				if ( take( velocity - th.m_inherit, times[ root ], root, &walls[ i ] ) )
					return;
			}
		}
	} );

	m_lock = best.m_entity ? best.m_entity->get_index( ) : 0;

	if ( !best.m_entity ) {
		if ( throw_tick )
			botox_dbg_log( "GAIM: throw no solution item=%d str=%.2f", item_index, strength );
		return;
	}

	/* GetFinalAimAngle = view + punch * weapon_recoil_scale */
	c_angle view = best.m_final - g_ctx.m_local->get_punch( ) * g_convars.float_or( HASH_BT( "weapon_recoil_scale" ), 2.f );
	view.normalize( );
	view.clamp( );

	if ( !std::isfinite( view.m_x ) || !std::isfinite( view.m_y ) ) {
		m_path_count = 0;
		return;
	}

	m_view = view;

	if ( !GET_VARIABLE( g_variables.m_grenade_aim_silent, bool ) )
		g_interfaces.m_engine_client->set_view_angles( view );

	if ( GET_VARIABLE( g_variables.m_grenade_aim_auto_throw, bool ) && key && pin_pulled &&
	     ( g_ctx.m_cmd->m_buttons & ( in_attack | in_second_attack ) ) ) {
		g_ctx.m_cmd->m_buttons &= ~( in_attack | in_second_attack );

		if ( !m_auto_released )
			botox_dbg_log( "GAIM: auto release ent=%d t=%.2f arc=%d bnc=%d str=%.2f", best.m_entity->get_index( ), best.m_time, best.m_root,
			               best.m_bounce, strength );

		m_auto_released = true;
	}

	if ( throw_tick ) {
		m_throw = true;
		botox_dbg_log( "GAIM: throw ent=%d t=%.2f arc=%d bnc=%d view=%.2f/%.2f str=%.2f lead=%.0fms", best.m_entity->get_index( ), best.m_time,
		               best.m_root, best.m_bounce, view.m_x, view.m_y, strength, lead * 1000.f );
	}
}

void n_grenade_aim::impl_t::apply( c_user_cmd* cmd )
{
	if ( !m_throw )
		return;

	m_throw = false;

	start_movement_fix( cmd );
	cmd->m_view_point = c_angle( m_view.m_x, m_view.m_y, cmd->m_view_point.m_z );
	end_movement_fix( cmd );
}

void n_grenade_aim::impl_t::on_paint_traverse( )
{
	if ( !GET_VARIABLE( g_variables.m_grenade_aim_draw, bool ) || m_path_count < 2 || !g_ctx.m_local || !g_ctx.m_local->is_alive( ) )
		return;

	const auto color = GET_VARIABLE( g_variables.m_grenade_aim_color, c_color ).get_u32( );

	c_vector_2d previous = { };
	bool has_previous    = false;

	for ( int i = 0; i < m_path_count; ++i ) {
		c_vector_2d screen = { };

		if ( !g_render.world_to_screen( m_path[ i ], screen ) ) {
			has_previous = false;
			continue;
		}

		if ( has_previous )
			g_render.m_draw_data.emplace_back( e_draw_type::draw_type_line,
			                                   std::make_any< line_draw_object_t >( line_draw_object_t{ previous, screen, color, 1.5f } ) );

		previous     = screen;
		has_previous = true;
	}

	c_vector_2d end = { };
	if ( g_render.world_to_screen( m_path[ m_path_count - 1 ], end ) )
		g_render.m_draw_data.emplace_back( e_draw_type::draw_type_filled_circle, std::make_any< filled_circle_draw_object_t >( end, 4.f, color ) );
}
