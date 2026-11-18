/* style 3 hotwheels = USELESSSRCCODE/hotwheels/csgo-sdk/hacks/movement/movement.cpp edge_bug / detect_edgebug / fsn / override_mouse_input 1:1 */
#include "edgebug.h"
#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"
#include "../prediction/prediction.h"
#include "wall_climb.h"
#include <cstring>

extern bool g_air_stuck_owns_cmd;
bool ps_latched( );
bool eb_cmd_taken( );
extern void botox_dbg_log( const char* fmt, ... );

void n_edgebug::impl_t::hw_reset( )
{
	m_hw.ducking       = false;
	m_hw.last_tick     = 0;
	m_hw.ticks_to_stop = 0;
	m_hw.will_edgebug  = false;
	m_hw.will_fail     = false;
}

void n_edgebug::impl_t::hw_detect( c_user_cmd* cmd )
{
	const auto local  = g_ctx.m_local;
	const auto& start = g_prediction.backup_data;
	const int mt      = local->get_move_type( );
	if ( start.m_velocity.m_z > 0 || mt == move_type_ladder || mt == move_type_observer || mt == move_type_noclip ) {
		m_hw.will_edgebug = false;
		m_hw.will_fail    = true;
		return;
	}
	const c_vector vel = local->get_velocity( );
	if ( std::roundf( vel.m_z ) == 0 || start.m_flags & fl_onground ) {
		m_hw.will_edgebug = false;
		m_hw.will_fail    = true;
	} else if ( start.m_velocity.m_z < -6.f && vel.m_z > start.m_velocity.m_z && vel.m_z < -6.f && !( local->get_flags( ) & fl_onground ) &&
	            start.m_origin.m_z > local->get_origin( ).m_z ) {
		const float gravity = g_convars.float_or( HASH_BT( "sv_gravity" ), 800.f );
		if ( std::floor( start.m_velocity.m_z ) < -7 && std::floor( vel.m_z ) == -7 && vel.length_2d( ) >= start.m_velocity.length_2d( ) ) {
			m_hw.will_edgebug = true;
			m_hw.will_fail    = false;
		} else {
			const float previous_velocity = vel.m_z;
			g_prediction.begin( local, cmd );
			g_prediction.end( local );
			const float expected = std::roundf( ( -gravity ) * g_interfaces.m_global_vars_base->m_interval_per_tick + previous_velocity );
			m_hw.will_edgebug    = expected == std::roundf( local->get_velocity( ).m_z );
			m_hw.will_fail       = !m_hw.will_edgebug;
		}
	}
}

void n_edgebug::impl_t::hw_post( c_user_cmd* cmd )
{
	const auto local = g_ctx.m_local;
	if ( !local || !local->is_alive( ) ) {
		hw_reset( );
		m_found = false;
		return;
	}
	const auto& start = g_prediction.backup_data;
	/* donor's on_create_move_post returns before edge_bug here: latch kept, nothing applied */
	if ( const int mt = start.m_move_type; mt == move_type_ladder || mt == move_type_noclip || mt == move_type_fly || mt == move_type_observer )
		return;
	const bool key = GET_VARIABLE( g_variables.edge_bug, bool ) && g_input.check_input( &GET_VARIABLE( g_variables.edge_bug_key, key_bind_t ) );
	if ( !key || ps_latched( ) || m_backup_move_type == move_type_ladder || g_air_stuck_owns_cmd || g_wall_climb.caught( cmd ) || eb_cmd_taken( ) ) {
		hw_reset( );
		m_found = false;
		return;
	}

	const int ticks       = std::clamp( GET_VARIABLE( g_variables.m_edgebug_hw_ticks, int ), 0, k_predicted_cmd_max - 2 );
	const float yaw_delta = std::clamp( start.m_view_angles.m_y - g_ctx.m_last_tick_yaw, -( 180.f / ticks ), 180.f / ticks );
	const float original_forward_move = cmd->m_forward_move;
	const float original_side_move    = cmd->m_side_move;
	const c_angle original_view_point = cmd->m_view_point;
	const int tick_count              = g_interfaces.m_global_vars_base->m_tick_count;
	const bool was_found              = m_hw.will_edgebug;
	c_vector path[ k_predicted_cmd_max + 1 ];
	int sims = 0;

	const auto loop_through_ticks = [ & ]( const bool ducked, const bool strafe = false ) {
		if ( m_hw.will_edgebug )
			return;
		/* hw: own rounds, not the stance combo */
		/* dont unduck: duck held = ducked rounds only, standing = both */
		const bool user_duck = ( m_user_buttons & in_duck ) != 0;
		if ( GET_VARIABLE( g_variables.m_edgebug_dont_unduck, bool ) ? ( user_duck && !ducked )
		                                                             : ( GET_VARIABLE( g_variables.m_edgebug_cur_stance, bool ) && ducked != user_duck ) )
			return;
		ReStorePrediction( );
		m_hw.starting_yaw = original_view_point.m_y;
		int path_n        = 0;
		path[ path_n++ ]  = local->get_origin( );
		for ( int i = 0; i <= ticks; i++ ) {
			c_user_cmd sim = *cmd;
			sim.m_buttons |= in_bullrush;
			if ( ducked ) {
				sim.m_buttons |= in_duck;
				local->get_flags( ) |= fl_ducking;
			} else {
				sim.m_buttons &= ~in_duck;
				local->get_flags( ) &= ~fl_ducking;
			}
			if ( !strafe ) {
				m_hw.strafing      = false;
				sim.m_forward_move = 0;
				sim.m_side_move    = 0;
				sim.m_buttons &= ~( in_jump | in_forward | in_back | in_moveleft | in_moveright );
			} else {
				m_hw.strafing          = true;
				sim.m_forward_move     = original_forward_move;
				sim.m_side_move        = original_side_move;
				sim.m_view_point.m_y   = g_math.normalize_angle( original_view_point.m_y + ( yaw_delta * i ) );
			}
			g_prediction.begin( local, &sim );
			g_prediction.end( local );
			sims++;
			if ( path_n <= k_predicted_cmd_max )
				path[ path_n++ ] = local->get_origin( );
			if ( start.m_flags & fl_onground || std::round( start.m_velocity.m_z ) >= 0 || local->get_flags( ) & fl_onground ||
			     local->get_move_type( ) == move_type_ladder ) {
				m_hw.will_edgebug = false;
				break;
			}
			if ( !m_hw.will_edgebug )
				hw_detect( &sim );
			if ( m_hw.will_edgebug ) {
				m_hw.saved_mousedx = static_cast< float >( std::abs( sim.m_mouse_delta_x ) );
				m_hw.ticks_to_stop = i + 1;
				m_hw.last_tick     = tick_count;
				m_hw.ducking       = ducked;
				if ( strafe ) {
					m_hw.yaw_delta    = yaw_delta;
					m_hw.forward_move = original_forward_move;
					m_hw.side_move    = original_side_move;
				} else
					m_hw.forward_move = m_hw.side_move = 0;
				std::memcpy( m_viz_path, path, sizeof( c_vector ) * path_n );
				m_viz_count = path_n;
				m_viz_pos   = local->get_origin( );
				m_viz_base  = tick_count;
				m_viz_alts  = 0;
				m_viz_sel   = -1;
				break;
			}
			if ( m_hw.will_fail ) {
				m_hw.will_fail = false;
				break;
			}
		}
		ReStorePrediction( );
	};

	loop_through_ticks( false );
	loop_through_ticks( true );
	if ( GET_VARIABLE( g_variables.m_edgebug_hw_advanced, bool ) &&
	     yaw_delta < ( GET_VARIABLE( g_variables.m_edgebug_hw_strafe_delta_max, float ) / 10000.f ) && !g_ctx.m_low_fps ) {
		loop_through_ticks( false, true );
		loop_through_ticks( true, true );
	}
	if ( !was_found && m_hw.will_edgebug )
		botox_dbg_log( "EB: style=3 find t=%d duck=%d strafe=%d yd=%.3f fwd=%.0f side=%.0f sims=%d lim=%d vz=%.2f\n", m_hw.ticks_to_stop,
		               m_hw.ducking ? 1 : 0, m_hw.strafing ? 1 : 0, m_hw.yaw_delta, m_hw.forward_move, m_hw.side_move, sims, ticks,
		               start.m_velocity.m_z );

	if ( m_hw.will_edgebug ) {
		ReStorePrediction( );
		if ( tick_count < m_hw.ticks_to_stop + m_hw.last_tick + 1 ) {
			cmd->m_buttons &= ~( in_jump | in_forward | in_back | in_moveleft | in_moveright );
			if ( m_hw.strafing ) {
				cmd->m_side_move      = m_hw.side_move;
				cmd->m_forward_move   = m_hw.forward_move;
				cmd->m_view_point.m_y = g_math.normalize_angle( m_hw.starting_yaw + ( m_hw.yaw_delta * ( tick_count - m_hw.last_tick ) ) );
			} else {
				cmd->m_side_move    = 0.f;
				cmd->m_forward_move = 0.f;
			}
			if ( m_hw.ducking )
				cmd->m_buttons |= in_duck;
			else
				cmd->m_buttons &= ~in_duck;
		} else
			hw_reset( );
	}

	m_found = m_hw.will_edgebug;
	if ( m_found ) {
		m_ducked           = false;
		m_found_tick       = m_hw.last_tick + m_hw.ticks_to_stop;
		m_prediction_ticks = m_hw.ticks_to_stop;
		m_replay_hits      = tick_count - m_hw.last_tick + 1;
	} else
		m_replay_hits = 0;
}

void n_edgebug::impl_t::hw_frame_start( )
{
	if ( !m_hw.will_edgebug || !m_hw.strafing )
		return;
	if ( const int mt = g_prediction.backup_data.m_move_type; mt == move_type_ladder || mt == move_type_noclip || mt == move_type_fly || mt == move_type_observer )
		return;
	c_angle wish_angles( g_prediction.backup_data.m_view_angles.m_x, m_hw.starting_yaw, 0.f );
	const float hit_time_delta = g_math.ticks_to_time( m_hw.ticks_to_stop );
	const float cur_time_delta = g_interfaces.m_global_vars_base->m_current_time - g_math.ticks_to_time( m_hw.last_tick );
	wish_angles.m_y += g_math.normalize_angle( m_hw.yaw_delta * ( m_hw.ticks_to_stop * ( cur_time_delta / hit_time_delta ) ) );
	g_interfaces.m_engine_client->set_view_angles( wish_angles );
}

void n_edgebug::impl_t::hw_mouse( float* x, float* y )
{
	if ( !m_hw.will_edgebug || !x || *x == 0 )
		return;
	const float amt    = GET_VARIABLE( g_variables.m_edgebug_hw_lock_amt, float );
	const int left     = m_hw.last_tick + m_hw.ticks_to_stop - g_interfaces.m_global_vars_base->m_tick_count;
	const float factor = 100.f / ( static_cast< float >( std::pow( m_hw.ticks_to_stop, left / *x ) ) + 100.f + ( amt * m_hw.saved_mousedx ) );
	if ( !std::isnan( factor ) )
		*x *= factor;
	if ( y && *y != 0 )
		*y *= 1.f - ( 0.1f * amt * 10.f );
}
