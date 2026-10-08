#include "movement_internal.h"
#include <algorithm>
#include <atomic>
#include <climits>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <share.h>
#include <cwchar>
#include <format>
#include <fstream>
#include <mutex>
#include <string>
#include "../../dependencies/imgui/imgui.h"
#include "../../dependencies/json/json.hpp"
#include "../chat_hud.h"
#include "texturebug.h"
#include "texturebug_common.h"
#include "edgebug.h"

float difference( float a, float b )
{
	return std::max( abs( a ), abs( b ) ) - std::min( abs( a ), abs( b ) );
}

/* flush moves = tb's ( texturebug.cpp texture_bug ), your stance, never a flip. every push past the gap is a hard hit: the hull parks
   0.03125 off the plane ( cmodel.cpp enter frac ( d1 - DIST_EPSILON ) / ( d1 - d2 ) ) where no seam pin exists, so the old contact
   push handed tb / wc their worst start. lab tb2_lab aa_run AA_MODE 2 */
namespace
{
	constexpr float k_aa_eps   = 0.03125f;
	constexpr float k_aa_touch = k_aa_eps * 1.3f;
	constexpr int k_aa_rungs   = 48;
	constexpr int k_aa_keys    = in_forward | in_back | in_moveleft | in_moveright;

	int s_park_margin = 0, s_park_cmd = -1;
	float s_park_from = 0.f, s_park_to = 0.f;
	float s_park_wx = 0.f, s_park_wy = 0.f;
	/* server pinned you stuck ( xy < 1 ) right after a park we simmed free: flush is a coin flip on this wall, park off till ground.
	   10-07 20:26 log: 26 / 41 stuck pins came the tick after a flush park ( wc's, same sim vs server split ) */
	bool s_park_off = false;

	void aa_world_wish( const c_user_cmd& c, float& x, float& y )
	{
		const float a = deg2rad( c.m_view_point.m_y );
		x             = c.m_forward_move * std::cosf( a ) + c.m_side_move * std::sinf( a );
		y             = c.m_forward_move * std::sinf( a ) - c.m_side_move * std::cosf( a );
	}

	void aa_flush_drop( )
	{
		s_park_margin = 0;
		s_park_cmd    = -1;
	}

	struct aa_flush_t {
		struct end_t {
			bool ok = false, cut = false, pin = false, head = false, stuck = false, ground = false, next_stuck = false;
			c_vector org{ }, vel{ };
		};

		c_base_entity* local = nullptr;
		const n_tick::c_sim_budget* budget = nullptr;
		c_user_cmd in{ };
		c_vector n{ }, org{ }, vel{ };
		int frame = 0, sims = 0;
		float gap = 0.f, ulp = 1e-7f, ipt = 0.f, k_acc = 0.f, pin = 0.f, xy0 = 0.f, yaw_in = 0.f, v_old = 0.f;
		bool duck = false, ducked0 = false, rising = false, user_dies = false, user_pins = false, user_stuck = false;

		bool init( const c_user_cmd* cmd, const c_vector& wall_n, const float plane_dist, const c_vector& maxs, const n_tick::c_sim_budget& b )
		{
			local  = g_ctx.m_local;
			budget = &b;
			ipt    = g_interfaces.m_global_vars_base ? g_interfaces.m_global_vars_base->m_interval_per_tick : 0.f;
			if ( !local || ipt <= 0.f )
				return false;
			in    = *cmd;
			frame = g_interfaces.m_prediction->m_commands_predicted - 1;
			g_prediction.restore_entity_to_predicted_frame( frame );
			n_tb::s_pred_dirty = true;
			org     = local->get_origin( );
			vel     = local->get_velocity( );
			ducked0 = ( local->get_flags( ) & e_flags::fl_ducking ) != 0;
			duck    = ( in.m_buttons & in_duck ) != 0;
			pin     = g_prediction.get_engine_target_predict_z_velocity( );
			rising  = vel.m_z + pin > 1e-3f;
			xy0     = vel.length_2d( );
			n       = c_vector( wall_n.m_x, wall_n.m_y, 0.f );
			gap     = org.m_x * n.m_x + org.m_y * n.m_y - plane_dist - ( std::fabsf( n.m_x ) * maxs.m_x + std::fabsf( n.m_y ) * maxs.m_y );
			yaw_in  = rad2deg( std::atan2f( -n.m_y, -n.m_x ) );
			v_old   = -( vel.m_x * n.m_x + vel.m_y * n.m_y );
			static auto sv_airaccelerate = g_interfaces.m_convar->find_var( "sv_airaccelerate" );
			k_acc = ( sv_airaccelerate ? sv_airaccelerate->get_float( ) : 12.f ) * ipt * local->get_surface_friction( );
			/* the circle ray is 1u: a gap off that = a plane that isn't this hull's face ( 10-07 log: park "from=4188" ) */
			if ( k_acc <= 0.f || gap < -0.1f || gap > 1.5f )
				return false;
			/* slanted wall: sim vs server split past ulps ( 23:40 log n 0.71 -0.70 plan 0.000036 got 0.001072 ), hull 3e-5 off = every
			   move zeroed, rise killed. old pushes park 0.03125 off there like the engine */
			if ( std::fminf( std::fabsf( n.m_x ), std::fabsf( n.m_y ) ) > 1e-3f ) {
				aa_outs[ aa_out_slant ]++;
				return false;
			}
			const float coord = std::max( std::fabsf( org.m_x * n.m_x ), std::fabsf( org.m_y * n.m_y ) );
			int ex            = 0;
			std::frexp( coord, &ex );
			if ( coord > 0.f )
				ulp = std::ldexp( 1.f, ex - 24 );

			/* a server that won't take the planned park ( offline listen server refuses 1 ulp from 0.03125 ) -> keep margin ulps.
			   any stuck arrival on a wall, not just park + 1: 10-07 23:14 log 30 TBC: as on slanted walls, poff=0 */
			if ( !s_park_off && xy0 < 1.f && std::fabsf( vel.m_z - pin ) < 1e-3f ) {
				s_park_off = true;
				aa_outs[ aa_out_park_off ]++;
			}
			if ( s_park_cmd >= 0 && cmd->m_command_number == s_park_cmd + 1 ) {
				if ( s_park_from - gap < 0.5f * ( s_park_from - s_park_to ) || ( s_park_to < 0.5f * k_aa_eps && gap > 0.9f * k_aa_eps ) ) {
					s_park_margin = s_park_margin ? s_park_margin * 2 : 2;
					if ( GET_VARIABLE( g_variables.m_debug_log, bool ) )
						botox_dbg_log( "AA: park refused from=%.6f plan=%.6f got=%.6f margin=%d", s_park_from, s_park_to, gap, s_park_margin );
				} else
					s_park_margin = 0;
			}
			s_park_cmd = -1;

			const end_t e = sim( in );
			user_dies     = e.ok && dies( e );
			user_pins     = e.pin;
			/* slanted wall, hull ulps off: your move zeroes xy + vz ( air stuck, rise killed ). keep would send it as is */
			user_stuck    = e.ok && e.stuck;
			if ( user_stuck && GET_VARIABLE( g_variables.m_debug_log, bool ) )
				botox_dbg_log( "AA: stuck gap=%.6f n=%.2f %.2f xy=%.1f vz=%.2f", gap, n.m_x, n.m_y, xy0, vel.m_z );
			return !e.cut;
		}

		bool dies( const end_t& e ) const { return xy0 > 50.f && e.vel.length_2d( ) < 0.2f * xy0; }

		/* engine order like tb pred_simulate / wc: leaf list bounds from the last move's max speed, else ulp-close sims miss brushes */
		end_t sim( c_user_cmd c, const c_user_cmd* next = nullptr )
		{
			end_t e;
			if ( sims > 0 && budget->expired( ) ) {
				e.cut = true;
				return e;
			}
			g_prediction.restore_entity_to_predicted_frame( frame );
			g_prediction.m_bounds_max_speed = g_prediction.m_real_max_speed;
			g_prediction.begin( local, &c );
			g_prediction.end( local );
			g_prediction.m_bounds_max_speed = 0.f;
			n_tb::s_pred_dirty              = true;
			++sims;
			e.ok     = local->get_move_type( ) == e_move_types::move_type_walk;
			e.ground = ( local->get_flags( ) & e_flags::fl_onground ) != 0;
			e.org    = local->get_origin( );
			e.vel    = local->get_velocity( );
			const bool pinned = e.ok && !e.ground && std::fabsf( e.vel.m_z - pin ) < 1e-3f;
			e.pin    = pinned && e.vel.length_2d( ) >= 1.f && vel.m_z + pin < 0.f;
			e.stuck  = pinned && e.vel.length_2d( ) < 1.f;
			/* touch list's first flat +z face over START feet: > 1u = head crossed a brush bottom = texture bug. no face = sloped / disp lip */
			e.head = g_prediction.m_last_face_dz != n_prediction::impl_t::k_no_face && g_prediction.m_last_face_dz > 1.f;
			/* + your move one tick on: park ends clean but the next move off a slanted wall's ulp gap zeroes ( 10-07 23:14 log ) */
			if ( next && e.ok && !e.ground && !budget->expired( ) ) {
				c_user_cmd c2                   = *next;
				g_prediction.m_bounds_max_speed = g_prediction.m_real_max_speed;
				g_prediction.begin( local, &c2 );
				g_prediction.end( local );
				g_prediction.m_bounds_max_speed = 0.f;
				++sims;
				const c_vector v2 = local->get_velocity( );
				e.next_stuck      = local->get_move_type( ) == e_move_types::move_type_walk && !( local->get_flags( ) & e_flags::fl_onground ) &&
				               std::fabsf( v2.m_z - pin ) < 1e-3f && v2.length_2d( ) < 1.f;
			}
			return e;
		}

		/* into-wall speed v_old -> v_t in one AirAccelerate, straight at the wall ( < 0 = straight out ) */
		c_user_cmd press( const float v_t ) const
		{
			const float crop = ( duck || ducked0 ) ? 0.34f : 1.f;
			float w          = v_t >= v_old ? std::max( ( v_t - v_old ) / k_acc, v_t ) : -std::max( ( v_old - v_t ) / k_acc, -v_t );
			w                = std::clamp( w / crop, -450.f, 450.f );
			c_user_cmd c     = in;
			c.m_forward_move = std::fabsf( w );
			c.m_side_move    = 0.f;
			n_tb::move_fix_to_yaw( w >= 0.f ? yaw_in : yaw_in + 180.f, c );
			c.m_buttons &= ~k_aa_keys;
			return c;
		}

		static void commit( c_user_cmd* cmd, const c_user_cmd& c )
		{
			cmd->m_forward_move = c.m_forward_move;
			cmd->m_side_move    = c.m_side_move;
			cmd->m_buttons      = c.m_buttons;
		}

		/* touching + falling: dx ladder from the smallest crossing press up to the head pin bound, ulp steps, send the pinning run's
		   middle. rising pins = head bounces off your rise, tb's call ( lab: same rides, fewer bounces, max sims 64 -> 40 ) */
		bool fine( c_user_cmd* cmd )
		{
			if ( gap > k_aa_touch || rising )
				return false;
			const float g  = std::max( gap, 1e-7f );
			const float dz = std::fabsf( ( vel.m_z + pin ) * ipt ) + 1e-4f;
			const float hp = ( duck || ducked0 ) ? 54.f : 72.f;
			const float hi = std::max( ( k_aa_eps - g ) * dz / ( hp + k_aa_eps ), g * 1.5f );
			const float step = 1.4f;
			float run[ k_aa_rungs ];
			int run_n = 0, k = 0;
			for ( float dx = g * 1.0005f + 1e-7f; dx <= hi && k < k_aa_rungs; dx = std::max( dx * step, dx + ulp ), ++k ) {
				const end_t e = sim( press( dx / ipt ) );
				if ( e.cut )
					break;
				if ( e.pin && !e.head && ( user_dies || !dies( e ) ) )
					run[ run_n++ ] = dx;
				else if ( run_n )
					break;
			}
			if ( !run_n )
				return false;
			commit( cmd, press( run[ run_n / 2 ] / ipt ) );
			return true;
		}

		/* park: tightest press that ends short of the plane with no hit ( a hit clips v_in, parks 0.03125 ), never under margin ulps */
		bool park( c_user_cmd* cmd )
		{
			if ( gap <= 0.f || s_park_margin > 16 || s_park_off )
				return false;
			static const float k_keep[] = { 0.000001f, 0.0001f, 0.002f, 0.05f, 0.3f };
			const float floor_gap       = static_cast< float >( std::max( s_park_margin, 1 ) ) * ulp;
			float last_end              = -1.f;
			for ( const float keep : k_keep ) {
				const float end = std::max( gap * keep, floor_gap );
				if ( end >= gap )
					break;
				if ( end == last_end )
					continue;
				last_end           = end;
				const c_user_cmd c = press( ( gap - end ) / ipt );
				const end_t e      = sim( c, &in );
				if ( e.cut )
					break;
				if ( !e.ok || e.ground || e.stuck || e.next_stuck )
					continue;
				const float g1     = ( e.org.m_x - org.m_x ) * n.m_x + ( e.org.m_y - org.m_y ) * n.m_y + gap;
				const bool touched = -( e.vel.m_x * n.m_x + e.vel.m_y * n.m_y ) <= 1e-3f;
				if ( g1 > 0.f && g1 < gap && !touched && g1 >= floor_gap * 0.5f && ( user_dies || !dies( e ) ) ) {
					commit( cmd, c );
					s_park_cmd  = cmd->m_command_number;
					s_park_from = gap;
					s_park_to   = g1;
					aa_world_wish( c, s_park_wx, s_park_wy );
					return true;
				}
			}
			return false;
		}

		/* your move dead-stops while flush ( next brush's lip sticks out under 0.03125 ): step off to the smallest gap that keeps the slide */
		bool unstick( c_user_cmd* cmd )
		{
			if ( ( !user_dies && !user_stuck ) || gap > k_aa_touch )
				return false;
			for ( const float to : { k_aa_eps, 0.125f, 0.5f } ) {
				const c_user_cmd c = press( -( to - gap ) / ipt );
				const end_t e      = sim( c );
				if ( e.cut )
					break;
				if ( e.ok && !e.stuck && !dies( e ) ) {
					commit( cmd, c );
					return true;
				}
			}
			return false;
		}
	};
}

/* end of create_move: a later feature ( fireman / wc ... ) rewrote our park = no server verdict, never grow the margin off it.
   10-07 20:18 log: every repeat refusal sat on a WC STOMP@fire_man, margin 2 -> 32 = park dead on that wall */
void n_movement::impl_t::auto_align_sent( const c_user_cmd* cmd )
{
	if ( g_ctx.m_local && ( g_ctx.m_local->get_flags( ) & e_flags::fl_onground ) )
		s_park_off = false;
	if ( !cmd || s_park_cmd != cmd->m_command_number )
		return;
	float x, y;
	aa_world_wish( *cmd, x, y );
	if ( std::fabsf( x - s_park_wx ) + std::fabsf( y - s_park_wy ) > 1e-3f ) {
		s_park_cmd = -1;
		aa_outs[ aa_out_stomp ]++;
	}
}

void n_movement::impl_t::auto_align( c_user_cmd* cmd )
{
	wall_detected = false;

	if ( !g_ctx.m_local || !g_ctx.m_local->is_alive( ) )
		return;

	const auto move_type = g_ctx.m_local->get_move_type( );
	if ( move_type == e_move_types::move_type_ladder || move_type == e_move_types::move_type_noclip )
		return;

	if ( m_fireman_data.is_ladder )
		return;

	if ( g_ctx.m_local->get_flags( ) & e_flags::fl_onground )
		return;

	if ( g_input.check_input( &GET_VARIABLE( g_variables.m_ladder_bug_key, key_bind_t ) ) )
		return;

	if ( as_key( ) && ( g_air_stuck_owns_cmd || g_air_stuck_holding || g_air_stuck_owned_prev_tick ) )
		return;

	const auto col = g_ctx.m_local->get_collideable( );
	if ( !col )
		return;

	const float max_radians  = 3.14159265358979323846f * 2.f;
	const float step         = max_radians / 16.f;
	const c_vector start_pos = g_ctx.m_local->get_abs_origin( );
	const auto mins          = col->get_obb_mins( );
	const auto maxs          = col->get_obb_maxs( );
	c_trace_filter_world fil;

	static float start_circle = 0.f;
	static float warm_fwd = 0.f, warm_side = 0.f;
	static int warm_tick = -1;

	int aa_sims                  = 0;
	constexpr int AA_MAX_PREDICTIONS = 24;
	n_tick::c_sim_budget aa_budget;
	aa_budget.start( 0.25f, 0.f, n_tick::search_aa );
	const auto aa_spent = [ & ]( ) { return aa_sims >= AA_MAX_PREDICTIONS || aa_budget.expired( ); };
	trace_t trace;
	c_vector save_start_pos{ };
	float saved_cos = 0.f;
	float saved_sin = 0.f;

	{
		c_vector near_mins = mins, near_maxs = maxs;
		near_mins.m_x -= 1.f; near_mins.m_y -= 1.f;
		near_maxs.m_x += 1.f; near_maxs.m_y += 1.f;
		trace_t near_trace;
		ray_t near_ray( start_pos, start_pos, near_mins, near_maxs );
		g_interfaces.m_engine_trace->trace_ray( near_ray, mask_playersolid, &fil, &near_trace );
		if ( !near_trace.did_hit( ) ) {
			start_circle = 0.f;
			warm_tick    = -1;
			aa_flush_drop( );
			aa_outs[ aa_out_near ]++;
			return;
		}
	}

	/* all 16 from last tick's wall angle, wrapping: a corner's lower angle used to cost a tick + the warm move */
	for ( int s = 0; s < 16; s++ ) {
		const float a  = std::fmod( start_circle + step * static_cast< float >( s ), max_radians );
		const float ca = std::cosf( a );
		const float sa = std::sinf( a );
		c_vector end_pos;
		end_pos.m_x = ca + start_pos.m_x;
		end_pos.m_y = sa + start_pos.m_y;
		end_pos.m_z = start_pos.m_z;
		saved_cos   = ca * 64.f;
		saved_sin   = sa * 64.f;

		ray_t ray( start_pos, end_pos, mins, maxs );
		g_interfaces.m_engine_trace->trace_ray( ray, mask_playersolid, &fil, &trace );
		if ( trace.m_fraction != 1.f && trace.m_plane.m_normal.m_z == 0.f ) {
			wall_detected  = true;
			start_circle   = a;
			save_start_pos = start_pos;
			break;
		}
	}

	if ( !wall_detected ) {
		start_circle = 0.f;
		warm_tick    = -1;
		aa_flush_drop( );
		aa_outs[ aa_out_face ]++;
		return;
	}

	/* tb key held: tb's ladder / park run after us with flips + lookahead. lab 4 maps x 64/128: our pushes under tb cost
	   inferno64 rides 749 -> 668, hands off = tb alone everywhere. wall_detected stays set for pixel_surf */
	if ( GET_VARIABLE( g_variables.m_texture_bug, bool ) && g_input.check_input( &GET_VARIABLE( g_variables.m_texture_bug_key, key_bind_t ) ) ) {
		warm_tick = -1;
		aa_flush_drop( );
		aa_outs[ aa_out_tb ]++;
		return;
	}

	if ( steering_away_from_wall( cmd, trace.m_plane.m_normal, m_user_forward_move, m_user_side_move, 0.3f ) ) {
		start_circle = 0.f;
		aa_outs[ aa_out_away ]++;
		return;
	}

	bool angle_check      = false;
	const c_vector angles = { trace.m_plane.m_normal.m_x * -0.005f, trace.m_plane.m_normal.m_y * -0.005f, 0.f };
	const c_vector end_pos2 = start_pos + angles;
	trace_t trace228;
	ray_t ray228( start_pos, end_pos2, mins, maxs );
	g_interfaces.m_engine_trace->trace_ray( ray228, mask_playersolid, &fil, &trace228 );

	if ( trace228.m_fraction == 1.f ) {
		c_angle to_wall = angles.to_angle2( );
		to_wall.normalize( );
		c_vector velo = g_ctx.m_local->get_velocity( );
		velo.m_z      = 0.f;

		if ( velo.length_2d( ) > 0.f ) {
			c_angle velo_ang = velo.to_angle2( );
			c_angle delta    = velo_ang - to_wall;
			delta.normalize( );
			if ( std::fabsf( delta.m_y ) > 92.5f )
				angle_check = true;
		}
	}

	if ( angle_check ) {
		aa_outs[ aa_out_angle ]++;
		return;
	}

	static int fail_tick = -1;
	static c_vector fail_normal{ };

	aa_flush_t flush;
	const bool flush_on = flush.init( cmd, trace.m_plane.m_normal, trace.m_plane.m_distance, maxs, aa_budget );
	/* your own move already rides ( tb act R ): keep it and its speed */
	if ( flush_on && flush.user_pins ) {
		warm_tick = fail_tick = -1;
		aa_outs[ aa_out_pin ]++;
		return;
	}
	if ( flush_on && flush.fine( cmd ) ) {
		warm_tick = fail_tick = -1;
		aa_outs[ aa_out_fine ]++;
		return;
	}
	if ( flush.sims )
		g_prediction.restore_entity_to_predicted_frame( g_interfaces.m_prediction->m_commands_predicted - 1 );

	/* one full search per 1/64 s: at 128 re-use last ACCEPTED move for one tick ( warm_tick set on
	   success only ). ticks( 1 ) == 1 at 64 = never skips there. */
	if ( const int search_gap = n_tick::ticks( 1 ); search_gap > 1 && warm_tick >= 0 &&
	     g_interfaces.m_global_vars_base->m_tick_count - warm_tick < search_gap && ( warm_fwd != 0.f || warm_side != 0.f ) ) {
		cmd->m_forward_move = warm_fwd;
		cmd->m_side_move    = warm_side;
		return;
	}

	c_angle wall_angle = angles.to_angle2( );
	wall_angle.normalize( );

	const c_vector lb_start( save_start_pos.m_x, save_start_pos.m_y, trace.m_end.m_z );
	const c_vector end_pos2_lb = lb_start + c_vector( saved_cos, saved_sin, 0.f );
	const float rotation       = deg2rad( wall_angle.m_y - cmd->m_view_point.m_y );
	const float cos_rot        = std::cosf( rotation );
	const float sin_rot        = std::sinf( rotation );
	bool detect                = false;

	trace_t trace_2;
	ray_t ray_2( lb_start, end_pos2_lb );
	g_interfaces.m_engine_trace->trace_ray( ray_2, mask_playersolid, &fil, &trace_2 );

	auto aligned_now = [ & ]( ) {
		if ( g_ctx.m_local->get_flags( ) & e_flags::fl_onground || g_ctx.m_local->get_move_type( ) == e_move_types::move_type_ladder )
			return false;
		/* xy < 1 = stuck on the wall, not a surf */
		return g_prediction.is_target_predict_z_velocity( g_ctx.m_local->get_velocity( ).m_z ) && g_ctx.m_local->get_velocity( ).length_2d( ) >= 1.f;
	};

	const float engine_ipt = g_interfaces.m_global_vars_base ? g_interfaces.m_global_vars_base->m_interval_per_tick : 0.f;
	bool vz_reachable      = true;
	if ( engine_ipt > 0.f ) {
		const c_vector vel = g_ctx.m_local->get_velocity( );
		const float move_vz = vel.m_z + g_prediction.get_engine_target_predict_z_velocity( );
		c_vector disp( vel.m_x * engine_ipt - trace.m_plane.m_normal.m_x * 0.5f,
		               vel.m_y * engine_ipt - trace.m_plane.m_normal.m_y * 0.5f,
		               move_vz * engine_ipt + ( move_vz > 0.f ? 2.f : -2.f ) );
		c_vector pos = start_pos;
		vz_reachable = false;

		for ( int bump = 0; bump < 3; bump++ ) {
			trace_t sweep;
			ray_t sweep_ray( pos, pos + disp, mins, maxs );
			g_interfaces.m_engine_trace->trace_ray( sweep_ray, mask_playersolid, &fil, &sweep );

			if ( sweep.m_start_solid || sweep.m_all_solid ) {
				vz_reachable = true;
				break;
			}
			if ( !sweep.did_hit( ) )
				break;
			if ( sweep.m_plane.m_normal.m_z != 0.f ) {
				vz_reachable = true;
				break;
			}

			pos                = sweep.m_end;
			c_vector left      = disp * ( 1.f - sweep.m_fraction );
			const float backoff = left.dot_product( sweep.m_plane.m_normal );
			left               = left - sweep.m_plane.m_normal * backoff;
			if ( left.length( ) < 0.03f )
				break;
			disp = left;
		}
	}

	/* fail cache ( 128 only ): same wall one tick later fails the same way, so reproduce the failed
	   output ( align loop leaves multiplier 90's push, contact loop writes nothing ). never while pinned. */
	bool fail_skip = false;
	if ( const int fail_gap = n_tick::ticks( 1 ); fail_gap > 1 && fail_tick >= 0 &&
	     !g_prediction.is_target_predict_z_velocity( g_prediction.backup_data.m_velocity.m_z ) &&
	     g_interfaces.m_global_vars_base->m_tick_count - fail_tick < fail_gap &&
	     fail_normal.dot_product( trace.m_plane.m_normal ) > 0.999f ) {
		fail_skip = true;
		aa_fail_skips++;
	}

	if ( !vz_reachable || fail_skip ) {
		/* = what a failed align loop leaves in cmd; skipping sims must not change the cmd. */
		cmd->m_forward_move = cos_rot * 90.f;
		cmd->m_side_move    = -sin_rot * 90.f;
		warm_tick           = -1;
		if ( !vz_reachable )
			aa_gate_skips++;
	} else if ( warm_tick >= 0 && g_interfaces.m_global_vars_base->m_tick_count - warm_tick <= 2 &&
	     ( warm_fwd != 0.f || warm_side != 0.f ) ) {
		g_prediction.restore_entity_to_predicted_frame( g_interfaces.m_prediction->m_commands_predicted - 1 );
		cmd->m_forward_move = warm_fwd;
		cmd->m_side_move    = warm_side;
		g_prediction.begin( g_ctx.m_local, cmd );
		g_prediction.end( g_ctx.m_local );
		aa_sims++;
		if ( aligned_now( ) ) {
			warm_tick = g_interfaces.m_global_vars_base->m_tick_count;
			detect    = true;
		}
	}

	for ( float multiplier = 10.f; vz_reachable && !fail_skip && !detect && multiplier < 100.f && !aa_budget.expired( ); multiplier += 10.f ) {
		g_prediction.restore_entity_to_predicted_frame( g_interfaces.m_prediction->m_commands_predicted - 1 );

		const float forwardmove = cos_rot * multiplier;
		const float sidemove    = -sin_rot * multiplier;
		cmd->m_forward_move     = forwardmove;
		cmd->m_side_move        = sidemove;

		g_prediction.begin( g_ctx.m_local, cmd );
		g_prediction.end( g_ctx.m_local );
		aa_sims++;

		if ( aligned_now( ) ) {
			cmd->m_forward_move = forwardmove;
			cmd->m_side_move    = sidemove;
			warm_fwd            = forwardmove;
			warm_side           = sidemove;
			warm_tick           = g_interfaces.m_global_vars_base->m_tick_count;
			detect              = true;
			break;
		}
	}

	/* no coarse pin: 1 ulp park ( or step off a dead stop ) instead of the hard-hit contact push; already flush and neither = your
	   move, the contact push would only slam the hull back to 0.03125. lab 4 maps in aa_check.py */
	if ( flush_on && !detect ) {
		const bool parked = flush.park( cmd );
		if ( parked || flush.unstick( cmd ) ) {
			aa_outs[ parked ? aa_out_park : aa_out_unstick ]++;
			fail_tick = -1;
			return;
		}
		g_prediction.restore_entity_to_predicted_frame( g_interfaces.m_prediction->m_commands_predicted - 1 );
		/* your move goes stuck and no step off frees it: never keep it, old path + fix_air_stucks flip */
		if ( flush.gap <= k_aa_touch && !flush.user_stuck ) {
			aa_flush_t::commit( cmd, flush.in );
			aa_outs[ aa_out_keep ]++;
			if ( !fail_skip ) {
				fail_tick   = g_interfaces.m_global_vars_base->m_tick_count;
				fail_normal = trace.m_plane.m_normal;
			}
			return;
		}
	}

	bool contact_reachable = true;
	if ( !detect && !fail_skip && engine_ipt > 0.f ) {
		const float dir_x  = saved_cos / 64.f, dir_y = saved_sin / 64.f;
		const float toward = -( dir_x * trace.m_plane.m_normal.m_x + dir_y * trace.m_plane.m_normal.m_y );
		const float gap_n  = trace.m_fraction * ( toward > 0.f ? toward : 0.f );
		const float close  = -( g_prediction.backup_data.m_velocity.m_x * trace.m_plane.m_normal.m_x +
		                        g_prediction.backup_data.m_velocity.m_y * trace.m_plane.m_normal.m_y ) * engine_ipt;
		if ( gap_n > 2.f * ( close > 0.f ? close : 0.f ) + 0.5f ) {
			contact_reachable = false;
			aa_reach_skips++;
		}
	}

	if ( !detect && contact_reachable && !fail_skip ) {
		for ( int i = 5; i >= 1 && !aa_spent( ); i-- ) {
			g_prediction.restore_entity_to_predicted_frame( g_interfaces.m_prediction->m_commands_predicted - 1 );
			const float forwardmove = cos_rot * i * 9.f;
			const float sidemove    = -sin_rot * i * 9.f;

			for ( int seph = 0; seph < 2; seph++ ) {
				c_user_cmd fakecmd     = *cmd;
				fakecmd.m_forward_move = forwardmove;
				fakecmd.m_side_move    = sidemove;
				g_prediction.begin( g_ctx.m_local, &fakecmd );
				g_prediction.end( g_ctx.m_local );
				aa_sims++;

				if ( g_ctx.m_local->get_move_type( ) == e_move_types::move_type_ladder )
					continue;

				const c_vector start_pos2 = g_ctx.m_local->get_abs_origin( );
				const c_vector end_pos3   = start_pos2 + angles;
				trace_t trace3;
				ray_t ray3( start_pos2, end_pos3, mins, maxs );
				g_interfaces.m_engine_trace->trace_ray( ray3, mask_playersolid, &fil, &trace3 );
				if ( trace3.m_fraction < 1.f ) {
					cmd->m_forward_move = forwardmove;
					cmd->m_side_move    = sidemove;
					detect              = true;
					break;
				}
			}

			if ( detect )
				break;
		}
	}

	const bool pinned = detect && g_prediction.is_target_predict_z_velocity( g_ctx.m_local->get_velocity( ).m_z );
	aa_outs[ pinned ? aa_out_pin : detect ? aa_out_touch : aa_out_miss ]++;

	/* skipped ticks don't stamp: a skip is always followed by a real search. */
	if ( !fail_skip ) {
		fail_tick   = detect ? -1 : g_interfaces.m_global_vars_base->m_tick_count;
		fail_normal = trace.m_plane.m_normal;
	}

	if ( g_prediction.is_target_predict_z_velocity( g_prediction.backup_data.m_velocity.m_z ) || ( cmd->m_buttons & in_forward ) || ( cmd->m_buttons & in_back ) ||
	     ( cmd->m_buttons & in_moveleft ) || ( cmd->m_buttons & in_moveright ) ) {
		c_vector wishdir;
		bool done = false;

		if ( ( cmd->m_buttons & in_forward ) && !( cmd->m_buttons & in_back ) && !( cmd->m_buttons & in_moveleft ) &&
		     !( cmd->m_buttons & in_moveright ) ) {
			wishdir = { std::cos( deg2rad( cmd->m_view_point.m_y ) ) * 128.f, std::sin( deg2rad( cmd->m_view_point.m_y ) ) * 128.f, 0.f };
			done    = true;
		}
		if ( ( cmd->m_buttons & in_back ) && !( cmd->m_buttons & in_forward ) && !( cmd->m_buttons & in_moveleft ) &&
		     !( cmd->m_buttons & in_moveright ) ) {
			wishdir = { std::cos( deg2rad( cmd->m_view_point.m_y + 180.f ) ) * 128.f, std::sin( deg2rad( cmd->m_view_point.m_y + 180.f ) ) * 128.f,
				        0.f };
			done    = true;
		}
		if ( ( cmd->m_buttons & in_moveleft ) && !( cmd->m_buttons & in_back ) && !( cmd->m_buttons & in_forward ) &&
		     !( cmd->m_buttons & in_moveright ) ) {
			wishdir = { std::cos( deg2rad( cmd->m_view_point.m_y + 90.f ) ) * 128.f, std::sin( deg2rad( cmd->m_view_point.m_y + 90.f ) ) * 128.f,
				        0.f };
			done    = true;
		}
		if ( ( cmd->m_buttons & in_moveright ) && !( cmd->m_buttons & in_back ) && !( cmd->m_buttons & in_moveleft ) &&
		     !( cmd->m_buttons & in_forward ) ) {
			wishdir = { std::cos( deg2rad( cmd->m_view_point.m_y - 90.f ) ) * 128.f, std::sin( deg2rad( cmd->m_view_point.m_y - 90.f ) ) * 128.f,
				        0.f };
			done    = true;
		}

		if ( done ) {
			trace_t trace_4;
			const c_vector st( save_start_pos.m_x, save_start_pos.m_y, trace.m_end.m_z );
			ray_t ray_4( st, st + wishdir );
			g_interfaces.m_engine_trace->trace_ray( ray_4, mask_playersolid, &fil, &trace_4 );

			if ( trace_4.m_fraction < 1.f ) {
				const int buttons_2 = cmd->m_buttons;
				float forwardmove_2 = cmd->m_forward_move;
				float sidemove_2    = cmd->m_side_move;
				const int i_backup_velo = static_cast< int >( g_ctx.m_local->get_velocity( ).length_2d( ) );

				if ( g_prediction.is_target_predict_z_velocity( g_prediction.backup_data.m_velocity.m_z ) ||
				     g_prediction.is_target_predict_z_velocity( g_ctx.m_local->get_velocity( ).m_z ) ) {
					if ( g_prediction.backup_data.m_velocity.length_2d( ) != 0.f && g_ctx.m_local->get_velocity( ).length_2d( ) != 0.f ) {
						static float warm_speed = 0.f;
						static int warm_speed_tick = -1;
						bool speed_warm = false;
						if ( warm_speed != 0.f && g_interfaces.m_global_vars_base->m_tick_count - warm_speed_tick <= 2 ) {
							g_prediction.restore_entity_to_predicted_frame( g_interfaces.m_prediction->m_commands_predicted - 1 );
							if ( buttons_2 & in_forward )
								cmd->m_forward_move = warm_speed;
							if ( buttons_2 & in_back )
								cmd->m_forward_move = -warm_speed;
							if ( buttons_2 & in_moveleft )
								cmd->m_side_move = -warm_speed;
							if ( buttons_2 & in_moveright )
								cmd->m_side_move = warm_speed;
							g_prediction.begin( g_ctx.m_local, cmd );
							g_prediction.end( g_ctx.m_local );
							aa_sims++;
							if ( g_prediction.is_target_predict_z_velocity( g_ctx.m_local->get_velocity( ).m_z ) ) {
								forwardmove_2   = cmd->m_forward_move;
								sidemove_2      = cmd->m_side_move;
								warm_speed_tick = g_interfaces.m_global_vars_base->m_tick_count;
								speed_warm      = true;
							}
						}
						for ( int i = speed_warm ? 0 : 450; i > 0 && !aa_spent( ); i -= 45 ) {
							g_prediction.restore_entity_to_predicted_frame( g_interfaces.m_prediction->m_commands_predicted - 1 );
							if ( buttons_2 & in_forward )
								cmd->m_forward_move = static_cast< float >( i );
							if ( buttons_2 & in_back )
								cmd->m_forward_move = static_cast< float >( -i );
							if ( buttons_2 & in_moveleft )
								cmd->m_side_move = static_cast< float >( -i );
							if ( buttons_2 & in_moveright )
								cmd->m_side_move = static_cast< float >( i );

							g_prediction.begin( g_ctx.m_local, cmd );
							g_prediction.end( g_ctx.m_local );
							aa_sims++;

							if ( g_prediction.is_target_predict_z_velocity( g_ctx.m_local->get_velocity( ).m_z ) ) {
								forwardmove_2   = cmd->m_forward_move;
								sidemove_2      = cmd->m_side_move;
								warm_speed      = static_cast< float >( i );
								warm_speed_tick = g_interfaces.m_global_vars_base->m_tick_count;
								break;
							}
						}

						const int i_preed_velo = static_cast< int >( g_ctx.m_local->get_velocity( ).length_2d( ) );

						cmd->m_forward_move = forwardmove_2;
						cmd->m_side_move    = sidemove_2;

						float dir_fwd[ 3 ]{ };
						float dir_side[ 3 ]{ };
						float max_speed[ 3 ]{ };
						int found = 0;

						if ( difference( static_cast< float >( i_backup_velo ), static_cast< float >( i_preed_velo ) ) < 5.f ) {
							for ( float angle = 15.f; angle < 30.f && !aa_spent( ); angle += 5.f ) {
								g_prediction.restore_entity_to_predicted_frame( g_interfaces.m_prediction->m_commands_predicted - 1 );
								const float mVel  = hypotf( g_prediction.backup_data.m_velocity.m_x, g_prediction.backup_data.m_velocity.m_y );
								const float ideal = rad2deg( atanf( angle / mVel ) );
								c_vector dvelo    = g_ctx.m_local->get_velocity( );
								dvelo.m_z         = 0.f;
								c_angle velo_angle = dvelo.to_angle2( );
								c_angle delta = velo_angle - c_vector( trace.m_plane.m_normal.m_x * -1.f, trace.m_plane.m_normal.m_y * -1.f, 0.f ).to_angle2( );
								delta.normalize( );
								if ( delta.m_y >= 0.f )
									wall_angle.m_y += ideal;
								else
									wall_angle.m_y -= ideal;

								const float rotation2    = deg2rad( wall_angle.m_y - cmd->m_view_point.m_y );
								const float cos_rot2     = std::cosf( rotation2 );
								const float sin_rot2     = std::sinf( rotation2 );
								const float forwardmove2 = cos_rot2 * 450.f;
								const float sidemove2    = -sin_rot2 * 450.f;
								cmd->m_forward_move      = forwardmove2;
								cmd->m_side_move         = sidemove2;

								const c_vector b_velo = g_ctx.m_local->get_velocity( );
								g_prediction.begin( g_ctx.m_local, cmd );
								g_prediction.end( g_ctx.m_local );
								aa_sims++;
								const c_vector p_velo = g_ctx.m_local->get_velocity( );

								if ( g_prediction.is_target_predict_z_velocity( b_velo.m_z ) &&
								     g_prediction.is_target_predict_z_velocity( p_velo.m_z ) ) {
									if ( p_velo.length_2d( ) > b_velo.length_2d( ) && found < 3 ) {
										dir_fwd[ found ]   = forwardmove2;
										dir_side[ found ]  = sidemove2;
										max_speed[ found ] = p_velo.length_2d( ) - b_velo.length_2d( );
										found++;
									}
								}
							}

							cmd->m_forward_move = forwardmove_2;
							cmd->m_side_move    = sidemove_2;

							float mxsp          = 0.f;
							int index_max_speed = -1;
							for ( int k = 0; k < found; k++ ) {
								if ( max_speed[ k ] > mxsp ) {
									index_max_speed = k;
									mxsp            = max_speed[ k ];
								}
							}
							if ( index_max_speed != -1 ) {
								cmd->m_forward_move = dir_fwd[ index_max_speed ];
								cmd->m_side_move    = dir_side[ index_max_speed ];
							}
						}
					}
				}

				if ( difference( trace_4.m_end.m_x, trace_2.m_end.m_x ) > 1.f && difference( trace_4.m_end.m_y, trace_2.m_end.m_y ) > 1.f ) {
					if ( cmd->m_forward_move < 0.f && cmd->m_buttons & in_forward )
						cmd->m_forward_move = 450.f;
					if ( cmd->m_forward_move > 0.f && cmd->m_buttons & in_back )
						cmd->m_forward_move = -450.f;
					if ( cmd->m_side_move < 0.f && cmd->m_buttons & in_moveright )
						cmd->m_side_move = 450.f;
					if ( cmd->m_side_move > 0.f && cmd->m_buttons & in_moveleft )
						cmd->m_side_move = -450.f;
				}
			} else {
				if ( cmd->m_forward_move < 0.f && cmd->m_buttons & in_forward )
					cmd->m_forward_move = 450.f;
				if ( cmd->m_forward_move > 0.f && cmd->m_buttons & in_back )
					cmd->m_forward_move = -450.f;
				if ( cmd->m_side_move < 0.f && cmd->m_buttons & in_moveright )
					cmd->m_side_move = 450.f;
				if ( cmd->m_side_move > 0.f && cmd->m_buttons & in_moveleft )
					cmd->m_side_move = -450.f;
			}
		}
	}

	g_prediction.restore_entity_to_predicted_frame( g_interfaces.m_prediction->m_commands_predicted - 1 );

	if ( GET_VARIABLE( g_variables.m_fix_air_stucks, bool ) && !aa_spent( ) ) {
		const float inward_fwd  = cmd->m_forward_move;
		const float inward_side = cmd->m_side_move;
		if ( std::fabs( inward_fwd ) < 0.01f && std::fabs( inward_side ) < 0.01f )
			return;

		g_prediction.restore_entity_to_predicted_frame( g_interfaces.m_prediction->m_commands_predicted - 1 );
		c_user_cmd test_cmd = *cmd;
		g_prediction.begin( g_ctx.m_local, &test_cmd );
		g_prediction.end( g_ctx.m_local );
		aa_sims++;
		const float inward_speed = g_ctx.m_local->get_velocity( ).length_2d( );
		g_prediction.restore_entity_to_predicted_frame( g_interfaces.m_prediction->m_commands_predicted - 1 );
		if ( inward_speed >= 1.f )
			return;

		test_cmd                = *cmd;
		test_cmd.m_forward_move = -inward_fwd;
		test_cmd.m_side_move    = -inward_side;
		g_prediction.begin( g_ctx.m_local, &test_cmd );
		g_prediction.end( g_ctx.m_local );
		aa_sims++;
		const float outward_speed = g_ctx.m_local->get_velocity( ).length_2d( );
		g_prediction.restore_entity_to_predicted_frame( g_interfaces.m_prediction->m_commands_predicted - 1 );
		if ( outward_speed > inward_speed + 0.01f ) {
			cmd->m_forward_move = -inward_fwd;
			cmd->m_side_move    = -inward_side;
		}
	}
}
