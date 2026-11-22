/* style 5 grimoire = USELESSSRCCODE/grimv5/features/movement/movement.cpp EdgebugAssist ( 2808-3124 ) + frame_stage_notify
   advanced_edgebug_scan + override_mouse_input 1:1 */
#include "edgebug.h"
#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"
#include "../prediction/prediction.h"
#include "wall_climb.h"
#include "movement.h"
#include <cstring>

extern bool g_air_stuck_owns_cmd;
bool ps_latched( );
bool eb_cmd_taken( );
extern void botox_dbg_log( const char* fmt, ... );

namespace {
	/* math::normalize_yaw */
	float grim_yaw( const float yaw )
	{
		if ( !std::isfinite( yaw ) )
			return 0.f;
		if ( yaw >= -180.f && yaw <= 180.f )
			return yaw;
		const float rot = std::round( std::abs( yaw / 360.f ) );
		return yaw < 0.f ? yaw + 360.f * rot : yaw - 360.f * rot;
	}
}

bool n_edgebug::impl_t::grim_check( c_user_cmd* cmd, bool& brk )
{
	const auto local  = g_ctx.m_local;
	const auto& start = g_prediction.backup_data;
	if ( start.m_flags & fl_onground )
		return false;
	const c_vector vel     = local->get_velocity( );
	static auto sv_gravity = g_interfaces.m_convar->find_var( "sv_gravity" );
	const float gravity    = sv_gravity ? sv_gravity->get_float( ) : 800.f;
	if ( std::round( vel.m_z ) >= 0.f || ( local->get_flags( ) & fl_onground ) ) {
		brk = true;
		return false;
	}
	if ( start.m_velocity.m_z < 0.f && vel.m_z > start.m_velocity.m_z && vel.m_z < 0.f ) {
		if ( start.m_origin.m_z < local->get_origin( ).m_z )
			return false;
		const int z_vel = static_cast< int >( vel.m_z );
		/* donor end( ) stepped the tickbase after the sim this checks */
		++local->get_tick_base( );
		g_prediction.begin( local, cmd );
		g_prediction.end( local );
		const float rounded = std::round( -gravity * g_interfaces.m_global_vars_base->m_interval_per_tick ) + static_cast< float >( z_vel );
		const float next_vz = std::round( local->get_velocity( ).m_z );
		const bool on       = ( local->get_flags( ) & fl_onground ) != 0;
		/* -6.25 / -6.2 / -3.125 vs a rounded value never match: donor's, kept */
		if ( rounded == next_vz || ( next_vz == 0.f && on ) || ( next_vz == -6.25f && on ) || ( next_vz == -6.2f && on ) ||
		     ( next_vz == -3.125f && on ) )
			return true;
		brk = true;
		return false;
	}
	return false;
}

void n_edgebug::impl_t::grim_post( c_user_cmd* cmd )
{
	const auto local = g_ctx.m_local;
	const bool key   = GET_VARIABLE( g_variables.edge_bug, bool ) && g_input.check_input( &GET_VARIABLE( g_variables.edge_bug_key, key_bind_t ) );
	if ( !key || !local || !local->is_alive( ) ) {
		grim_reset( );
		return;
	}
	const auto& start   = g_prediction.backup_data;
	const int move_type = local->get_move_type( );
	if ( ( start.m_flags & fl_onground ) || move_type == move_type_noclip || move_type == move_type_observer || move_type == move_type_ladder ) {
		grim_reset( );
		return;
	}
	if ( m_backup_move_type == move_type_ladder || g_air_stuck_owns_cmd || g_wall_climb.caught( cmd ) || ps_latched( ) || eb_cmd_taken( ) ) {
		grim_reset( );
		return;
	}

	auto& g = m_grim;
	/* published before this tick's search / step: fsn camera + mouse lock run one cmd behind, like the donor */
	g.pub_startingyaw = g.startingyaw;
	g.pub_ticks_left  = g.ticks_left;
	g.pub_tick        = g.edgebugtick;
	g.pub_detecttick  = g.detecttick;
	g.pub_yawdelta    = g.yawdelta;
	g.pub_length      = g.eblength;
	g.pub_strafing    = g.strafing;

	if ( !g.ticks_left ) {
		static auto m_yaw            = g_interfaces.m_convar->find_var( "m_yaw" );
		static auto sens             = g_interfaces.m_convar->find_var( "sensitivity" );
		const float interval         = g_interfaces.m_global_vars_base->m_interval_per_tick;
		/* time_to_ticks */
		const int ticklimit          = std::min( static_cast< int >( 0.5f + GET_VARIABLE( g_variables.m_edgebug_grim_scan, float ) / interval ), k_predicted_cmd_max );
		float yawdelta               = std::clamp( static_cast< float >( cmd->m_mouse_delta_x ) * ( m_yaw ? m_yaw->get_float( ) : 0.022f ) *
		                                               ( sens ? sens->get_float( ) : 1.f ),
		                                           -30.f, 30.f );
		const int pred_rounds        = ( GET_VARIABLE( g_variables.m_edgebug_grim_strafe, bool ) && yawdelta != 0.f ) ? 4 : 2;
		const int search             = GET_VARIABLE( g_variables.m_edgebug_grim_search, int );
		const float limit            = GET_VARIABLE( g_variables.m_edgebug_grim_angle_limit, float );
		const c_angle originalangles = cmd->m_view_point;
		g.startingyaw                = originalangles.m_y;
		g.first_pitch                = originalangles.m_x;
		const int tick_count         = g_interfaces.m_global_vars_base->m_tick_count;
		const bool dont_unduck       = GET_VARIABLE( g_variables.m_edgebug_dont_unduck, bool );
		const bool cur_stance        = GET_VARIABLE( g_variables.m_edgebug_cur_stance, bool );
		const bool user_duck         = ( m_user_buttons & in_duck ) != 0;
		/* botox stance rows ( off = donor ) */
		const auto stance_skip       = [ & ]( const bool crouched ) {
			return dont_unduck ? ( user_duck && !crouched ) : ( cur_stance && crouched != user_duck );
		};
		c_vector path[ k_predicted_cmd_max + 1 ];
		int sims = 0, found_round = -1;

		const auto run = [ & ]( c_user_cmd& predictcmd, const int round, const bool crouched, const float yd ) {
			ReStorePrediction( );
			const int tick_base0 = local->get_tick_base( );
			int path_n           = 0;
			path[ path_n++ ]     = local->get_origin( );
			for ( int t = 1; t <= ticklimit; ++t ) {
				if ( round > 1 ) {
					predictcmd.m_view_point.m_y = grim_yaw( originalangles.m_y + yd * static_cast< float >( t ) );
					if ( std::abs( predictcmd.m_view_point.m_y - g.startingyaw ) > limit )
						break;
				}
				local->get_tick_base( ) = tick_base0 + t - 1;
				g_prediction.begin( local, &predictcmd );
				g_prediction.end( local );
				sims++;
				const c_vector vel = local->get_velocity( );
				if ( vel.m_z > 0.f || vel.length_2d( ) == 0.f || local->get_move_type( ) == move_type_ladder )
					break;
				if ( path_n <= k_predicted_cmd_max )
					path[ path_n++ ] = local->get_origin( );
				bool br = false;
				if ( grim_check( &predictcmd, br ) ) {
					g.ticks_left  = t;
					g.eblength    = t;
					g.edgebugtick = tick_count + t;
					g.detecttick  = tick_count;
					g.forwardmove = predictcmd.m_forward_move;
					g.sidemove    = predictcmd.m_side_move;
					g.yawdelta    = yd;
					g.crouched    = crouched;
					g.strafing    = round > 1;
					found_round   = round;
					std::memcpy( m_viz_path, path, sizeof( c_vector ) * path_n );
					m_viz_count = path_n;
					m_viz_pos   = local->get_origin( );
					m_viz_base  = tick_count;
					m_viz_alts  = 0;
					m_viz_sel   = -1;
					break;
				}
				/* a find this search: later fan variants sim once, a tick 1 find still overwrites ( donor ). real tick, not the cmd's:
				   botox lagcomp lowers cmd tick before eb ( donor backtracks after ), a stale edgebugtick would cut every variant */
				if ( tick_count < g.edgebugtick )
					break;
				if ( br )
					break;
			}
		};

		for ( int round = 0; round < pred_rounds; ++round ) {
			if ( round > 1 ) {
				/* shared var: round 3 fans from round 2's overshoot ( donor ) */
				const float max_delta = yawdelta;
				const bool crouched   = round == 2;
				for ( yawdelta = max_delta / search; std::abs( yawdelta ) <= std::abs( max_delta ); yawdelta += max_delta / search ) {
					if ( stance_skip( crouched ) )
						continue;
					c_user_cmd predictcmd = *cmd;
					if ( crouched )
						predictcmd.m_buttons |= in_duck;
					else
						predictcmd.m_buttons &= ~in_duck;
					run( predictcmd, round, crouched, yawdelta );
				}
			} else {
				const bool crouched = round == 0;
				if ( !stance_skip( crouched ) ) {
					c_user_cmd predictcmd = *cmd;
					if ( crouched )
						predictcmd.m_buttons |= in_duck;
					else
						predictcmd.m_buttons &= ~in_duck;
					predictcmd.m_forward_move = 0.f;
					predictcmd.m_side_move    = 0.f;
					run( predictcmd, round, crouched, yawdelta );
				}
			}
			if ( g.ticks_left )
				break;
		}
		ReStorePrediction( );
		if ( g.ticks_left )
			botox_dbg_log( "EB: style=%d find round=%d t=%d duck=%d strafe=%d yd=%.2f fwd=%.0f side=%.0f sims=%d lim=%d vz=%.2f\n", 5, found_round,
			               g.eblength, g.crouched ? 1 : 0, g.strafing ? 1 : 0, g.yawdelta, g.forwardmove, g.sidemove, sims, ticklimit,
			               start.m_velocity.m_z );
	}

	if ( !g.ticks_left ) {
		g.should      = false;
		m_found       = false;
		m_replay_hits = 0;
		return;
	}
	/* donor's stamina write sits behind an on ground START that returned above: dead, skipped */
	g.should           = true;
	m_found            = true;
	m_ducked           = false;
	m_found_tick       = g.edgebugtick;
	m_prediction_ticks = g.eblength;
	if ( g.crouched )
		cmd->m_buttons |= in_duck;
	else
		cmd->m_buttons &= ~in_duck;
	if ( GET_VARIABLE( g_variables.m_edgebug_grim_lock_movement, bool ) ) {
		if ( g.strafing ) {
			cmd->m_forward_move   = g.forwardmove;
			cmd->m_side_move      = g.sidemove;
			cmd->m_view_point.m_y = grim_yaw( g.startingyaw + g.yawdelta * static_cast< float >( g.eblength - ( g.ticks_left - 1 ) ) );
		} else {
			cmd->m_forward_move = 0.f;
			cmd->m_side_move    = 0.f;
		}
	}
	g.ticks_left--;
	m_replay_hits = g.eblength - g.ticks_left;
}

bool n_edgebug::impl_t::grim_lock_live( ) const
{
	return GET_VARIABLE( g_variables.m_edgebug_grim_lock_amount, float ) > 0.f && m_grim.should && m_grim.pub_ticks_left &&
	       !g_movement.m_pixelsurf_data.should_pixel_surf;
}

void n_edgebug::impl_t::grim_frame_start( )
{
	const auto& g = m_grim;
	if ( !( GET_VARIABLE( g_variables.m_edgebug_grim_lock_amount, float ) > 0.f && g.should && g.pub_ticks_left && g.pub_strafing ) )
		return;
	if ( !g_ctx.m_local || !g_ctx.m_local->is_alive( ) || GET_VARIABLE( g_variables.m_edgebug_grim_silent, bool ) )
		return;
	const float ipt       = g_interfaces.m_global_vars_base->m_interval_per_tick;
	const float to_eb     = ipt * static_cast< float >( g.pub_tick ) - ipt * static_cast< float >( g.pub_detecttick );
	const float from_find = g_interfaces.m_global_vars_base->m_current_time - ipt * static_cast< float >( g.pub_detecttick );
	c_angle view( g.first_pitch, g.pub_startingyaw, 0.f );
	view.m_y += grim_yaw( g.pub_yawdelta * ( static_cast< float >( g.pub_length ) * ( from_find / to_eb ) ) );
	g_interfaces.m_engine_client->set_view_angles( view );
}

/* lock type 0 static ( donor has no type row, default 0 ) */
void n_edgebug::impl_t::grim_mouse( float* x )
{
	*x *= 1.f - GET_VARIABLE( g_variables.m_edgebug_grim_lock_amount, float ) / 100.f;
}
