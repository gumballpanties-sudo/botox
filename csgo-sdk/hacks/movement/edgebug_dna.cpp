/* style 2 dna = USELESSSRCCODE/DNA-SOURCECODE-main/features/movement/moveutil.h EdgebugAssist pre / run + fsn camera 1:1 */
#include "edgebug.h"
#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"
#include "../prediction/prediction.h"
#include "tick_scale.h"
#include "wall_climb.h"
#include <cstring>

extern bool g_air_stuck_owns_cmd;
bool ps_latched( );
bool eb_cmd_taken( );
extern void botox_dbg_log( const char* fmt, ... );
int eb_donor_reach( const c_vector& origin, const c_vector& vel, int window_ticks, int segs );

namespace {
	float donor_yaw( const float y ) { return std::remainderf( y, 360.f ); }

	float donor_mouse_yaw( const c_user_cmd* cmd )
	{
		static auto m_yaw = g_interfaces.m_convar->find_var( "m_yaw" );
		static auto sens  = g_interfaces.m_convar->find_var( "sensitivity" );
		return std::clamp( static_cast< float >( cmd->m_mouse_delta_x ) * ( m_yaw ? m_yaw->get_float( ) : 0.022f ) *
		                       ( sens ? sens->get_float( ) : 1.f ),
		                   -30.f, 30.f );
	}

	void donor_fix_move( c_user_cmd* cmd, const c_angle& angle )
	{
		if ( !( g_ctx.m_local->get_flags( ) & fl_onground ) && cmd->m_view_point.m_z != 0.f && ( cmd->m_buttons & in_attack ) )
			cmd->m_side_move = 0.f;
		const float len = sqrtf( cmd->m_forward_move * cmd->m_forward_move + cmd->m_side_move * cmd->m_side_move );
		if ( len == 0.f )
			return;
		const float yaw = deg2rad( rad2deg( atan2f( cmd->m_side_move, cmd->m_forward_move ) ) + cmd->m_view_point.m_y - angle.m_y );
		float fwd       = cosf( yaw ) * len;
		if ( cmd->m_view_point.m_x < -90.f || cmd->m_view_point.m_x > 90.f )
			fwd = -fwd;
		cmd->m_forward_move = std::clamp( fwd, -450.f, 450.f );
		cmd->m_side_move    = std::clamp( sinf( yaw ) * len, -450.f, 450.f );
		cmd->m_up_move      = std::clamp( cmd->m_up_move, -320.f, 320.f );
		cmd->m_buttons &= ~( in_forward | in_back | in_moveright | in_moveleft );
	}
}

bool n_edgebug::impl_t::dna_check( c_user_cmd* cmd, bool& brk )
{
	const auto local = g_ctx.m_local;
	if ( const int mt = local->get_move_type( ); mt == move_type_ladder || mt == move_type_noclip ) {
		brk = true;
		return false;
	}
	const auto& start      = g_prediction.backup_data;
	const c_vector vel     = local->get_velocity( );
	static auto sv_gravity = g_interfaces.m_convar->find_var( "sv_gravity" );
	const float gravity    = sv_gravity ? sv_gravity->get_float( ) : 800.f;
	const float interval   = g_interfaces.m_global_vars_base->m_interval_per_tick;
	const float tick_rate  = interval > 0.f ? 1.f / interval : 0.f;
	const float grav_velo  = ( ( gravity / 2.f ) / tick_rate ) * -1.f;
	if ( std::roundf( vel.m_z ) >= 0.f || ( local->get_flags( ) & fl_onground ) || std::roundf( vel.length_2d( ) ) == 0.f ) {
		brk = true;
		return false;
	}
	if ( start.m_velocity.m_z < 0.f && vel.m_z > start.m_velocity.m_z && vel.m_z < 0.f ) {
		if ( start.m_origin.m_z < local->get_origin( ).m_z )
			return false;
		const int z_vel = static_cast< int >( vel.m_z );
		++local->get_tick_base( );
		g_prediction.begin( local, cmd, false, true );
		g_prediction.end( local );
		const float rounded = std::roundf( -gravity * interval ) + static_cast< float >( z_vel );
		const float next_vz = std::roundf( local->get_velocity( ).m_z );
		if ( rounded == next_vz || ( next_vz == 0.f && ( local->get_flags( ) & fl_onground ) ) )
			return true;
		brk = true;
		return false;
	}
	return start.m_velocity.m_z < grav_velo && std::roundf( vel.m_z ) == std::roundf( grav_velo ) && local->get_move_type( ) != move_type_ladder;
}

void n_edgebug::impl_t::dna_pre( c_user_cmd* cmd )
{
	if ( m_found && m_donor.crouched && m_donor.ticks_left )
		cmd->m_buttons |= in_duck;
	if ( m_found && m_donor.ticks_left && m_donor.strafing ) {
		cmd->m_forward_move = m_donor.forwardmove;
		cmd->m_side_move    = m_donor.sidemove;
	}
}

void n_edgebug::impl_t::dna_post( c_user_cmd* cmd )
{
	const auto local = g_ctx.m_local;
	if ( !local || !local->is_alive( ) || !g_interfaces.m_engine_client->is_connected( ) ) {
		donor_reset( );
		return;
	}
	const auto& start   = g_prediction.backup_data;
	const int move_type = local->get_move_type( );
	const bool key      = GET_VARIABLE( g_variables.edge_bug, bool ) && g_input.check_input( &GET_VARIABLE( g_variables.edge_bug_key, key_bind_t ) );
	if ( !key || ( start.m_flags & fl_onground ) || move_type != move_type_walk ) {
		donor_reset( );
		return;
	}
	if ( m_backup_move_type == move_type_ladder || g_air_stuck_owns_cmd || g_wall_climb.caught( cmd ) || ps_latched( ) || eb_cmd_taken( ) ) {
		donor_reset( );
		return;
	}

	const c_angle view_backup = cmd->m_view_point;
	if ( !m_donor.ticks_left ) {
		float yawdelta        = donor_mouse_yaw( cmd );
		const float interval  = g_interfaces.m_global_vars_base->m_interval_per_tick;
		const int ticklimit   = std::clamp( static_cast< int >( 0.5f + GET_VARIABLE( g_variables.m_edgebug_dna_scan, float ) / interval ), 0, k_predicted_cmd_max );
		const bool advanced   = GET_VARIABLE( g_variables.m_edgebug_dna_advanced, bool );
		const int pred_rounds = ( advanced && yawdelta != 0.f ) ? 4 : 2;
		const int search      = std::max( GET_VARIABLE( g_variables.m_edgebug_dna_range, int ), 1 );
		const int divisor     = advanced ? search : 2;
		const bool dna_custom = GET_VARIABLE( g_variables.m_edgebug_dna_custom_angle, bool );
		const bool cur_stance = GET_VARIABLE( g_variables.m_edgebug_cur_stance, bool );
		const bool user_duck  = ( m_user_buttons & in_duck ) != 0;
		const float dna_limit = GET_VARIABLE( g_variables.m_edgebug_dna_angle_limit, float );
		const c_user_cmd base = *cmd;
		const c_angle originalangles = base.m_view_point;
		const c_vector end_vel = local->get_velocity( );
		m_donor.startingyaw    = originalangles.m_y;
		const int tick_count   = g_interfaces.m_global_vars_base->m_tick_count;
		c_vector path[ k_predicted_cmd_max + 1 ];
		int sims = 0;
		/* rising: every run breaks after its 1st sim. strafe rounds break before any sim at limit != 0 */
		const bool dna_rising  = end_vel.m_z > 0.f || start.m_velocity.m_z > 0.f;
		const bool strafe_runs = !dna_rising && divisor != 0 && dna_limit == 0.f;
		/* rounds 0/1 have no wish = ballistic: nothing solid in the swept start hull = no contact = no find */
		bool still_runs = !dna_rising && ticklimit > 0;
		int reach       = -1;
		if ( still_runs ) {
			ReStorePrediction( );
			const int window = ticklimit + 1;
			reach            = eb_donor_reach( local->get_origin( ), local->get_velocity( ), window,
			                                   std::clamp( static_cast< int >( std::ceil( window * interval / 0.1f ) ), 3, 16 ) );
			still_runs       = reach >= 0;
		}
		/* contact far ( or none ): search every 2nd tick, a far bug found 1 tick later is the same bug */
		static int s_last_search = -1;
		const bool far_skip = !dna_rising && ticklimit > 0 && ( reach < 0 || reach > n_tick::ticks( 16 ) ) && s_last_search >= 0 && tick_count > s_last_search &&
		                      tick_count - s_last_search < n_tick::ticks( 2 );
		if ( far_skip )
			still_runs = false;
		else
			s_last_search = tick_count;
		/* exact donor fan lists ( round 3 fans from round 2's overshoot ), walked under a clock below */
		float fan[ 2 ][ 64 ];
		int fan_n[ 2 ] = { };
		if ( strafe_runs && pred_rounds > 2 && !far_skip ) {
			float yd = yawdelta;
			for ( int r = 0; r < 2; ++r ) {
				const float max_delta = yd;
				for ( yd = max_delta / divisor; fabsf( yd ) <= fabsf( max_delta ) && fan_n[ r ] < 64; yd += max_delta / divisor )
					fan[ r ][ fan_n[ r ]++ ] = yd;
			}
		}
		const long long t0 = n_tick::qpc_now( );
		const auto used_us = [ t0 ]( ) { return ( n_tick::qpc_now( ) - t0 ) * 1000000ll / n_tick::qpc_freq( ); };

		const auto run = [ & ]( const int round, const float yd ) {
			const bool crouched = round == 0 || round == 2, strafing = round > 1;
			*cmd = base;
			if ( crouched )
				cmd->m_buttons |= in_duck;
			else
				cmd->m_buttons &= ~in_duck;
			if ( !strafing ) {
				cmd->m_forward_move = 0.f;
				cmd->m_side_move    = 0.f;
				cmd->m_up_move      = 0.f;
			} else {
				cmd->m_side_move = round == 2 && originalangles.m_y <= 0.f ? base.m_side_move * -450.f : base.m_side_move * 450.f;
			}
			ReStorePrediction( );
			const int tick_base0 = local->get_tick_base( );
			int path_n      = 0;
			path[ path_n++ ] = local->get_origin( );
			for ( int t = 1; t <= ticklimit; ++t ) {
				if ( strafing ) {
					cmd->m_view_point.m_y = donor_yaw( originalangles.m_y + yd * static_cast< float >( t ) );
					const float turned    = fabsf( cmd->m_view_point.m_y - m_donor.startingyaw );
					if ( ( ( turned > ( dna_custom ? 1.f : 0.f ) ) ? dna_limit : 1.f ) != 0.f )
						break;
				}
				/* donors' end( ) steps the tickbase per sim: curtime must move ( duck gate ), restore puts it back */
				local->get_tick_base( ) = tick_base0 + t - 1;
				g_prediction.begin( local, cmd, false, true );
				g_prediction.end( local );
				sims++;
				const c_vector vel = local->get_velocity( );
				if ( local->get_move_type( ) == move_type_ladder || end_vel.m_z > 0.f || start.m_velocity.m_z > 0.f )
					break;
				if ( path_n <= k_predicted_cmd_max )
					path[ path_n++ ] = local->get_origin( );
				bool br = false;
				const bool ducked_now = ( local->get_flags( ) & fl_ducking ) != 0;
				if ( dna_check( cmd, br ) ) {
					if ( cur_stance && ducked_now != user_duck )
						break;
					m_donor.ticks_left  = t;
					m_donor.eblength    = t;
					m_donor.edgebugtick = tick_count + t;
					m_donor.detecttick  = tick_count;
					m_donor.forwardmove = cmd->m_forward_move;
					m_donor.sidemove    = cmd->m_side_move;
					m_donor.yawdelta    = yd;
					m_donor.crouched    = crouched;
					m_donor.strafing    = strafing;
					m_donor.mousedx     = base.m_mouse_delta_x;
					std::memcpy( m_viz_path, path, sizeof( c_vector ) * path_n );
					m_viz_count = path_n;
					m_viz_pos   = local->get_origin( );
					m_viz_base  = tick_count;
					m_viz_alts  = 0;
					m_viz_sel   = -1;
					break;
				}
				if ( br )
					break;
			}
		};

		int found_round = -1;
		for ( int round = 0; round < 2 && still_runs && found_round < 0; ++round ) {
			run( round, yawdelta );
			if ( m_donor.ticks_left )
				found_round = round;
		}
		/* strafe fan over 0.2 tick of clock ( rounds 0/1 never cut ): a cut fan resumes where it stopped next search */
		static int s_fan_resume = 0;
		const int fan_total     = fan_n[ 0 ] + fan_n[ 1 ];
		bool fan_cut            = false;
		if ( found_round < 0 && fan_total > 0 ) {
			const long long cap_us = static_cast< long long >( n_tick::engine_interval( ) * 200000.f );
			const int first        = s_fan_resume % fan_total;
			int i                  = 0;
			for ( ; i < fan_total; ++i ) {
				if ( used_us( ) >= cap_us )
					break;
				const int k = ( first + i ) % fan_total, r = k < fan_n[ 0 ] ? 0 : 1;
				run( 2 + r, fan[ r ][ r ? k - fan_n[ 0 ] : k ] );
				if ( m_donor.ticks_left ) {
					found_round = 2 + r;
					break;
				}
			}
			fan_cut      = found_round < 0 && i < fan_total;
			s_fan_resume = fan_cut ? ( first + i ) % fan_total : 0;
		}
		static int n_search = 0, n_skip = 0, n_far = 0, n_cut = 0, n_sims = 0, n_peak = 0;
		static long long n_us = 0, n_peak_us = 0;
		const long long us = used_us( );
		n_search++;
		n_skip += !still_runs && !far_skip;
		n_far += far_skip;
		n_cut += fan_cut;
		n_sims += sims;
		n_peak    = std::max( n_peak, sims );
		n_us += us;
		n_peak_us = std::max( n_peak_us, us );
		if ( static unsigned long long next_report = 0ull; GetTickCount64( ) >= next_report ) {
			next_report = GetTickCount64( ) + 1000ull;
			botox_dbg_log( "EB: style=%d gate searches=%d skip=%d far=%d cut=%d sims=%d peak=%d us=%lld peak_us=%lld\n", 2, n_search, n_skip,
			               n_far, n_cut, n_sims, n_peak, n_us, n_peak_us );
			n_search = n_skip = n_far = n_cut = n_sims = n_peak = 0;
			n_us = n_peak_us = 0;
		}
		*cmd = base;
		ReStorePrediction( );
		if ( m_donor.ticks_left )
			botox_dbg_log( "EB: style=%d find round=%d t=%d duck=%d strafe=%d yd=%.2f fwd=%.0f side=%.0f sims=%d lim=%d vz=%.2f\n", 2,
			               found_round, m_donor.eblength, m_donor.crouched ? 1 : 0, m_donor.strafing ? 1 : 0, m_donor.yawdelta,
			               m_donor.forwardmove, m_donor.sidemove, sims, ticklimit, start.m_velocity.m_z );
	}

	if ( !m_donor.ticks_left ) {
		m_found = false;
		return;
	}
	m_found            = true;
	m_ducked           = false;
	m_found_tick       = m_donor.edgebugtick;
	m_prediction_ticks = m_donor.eblength;
	const float yaw    = donor_yaw( m_donor.startingyaw + m_donor.yawdelta * static_cast< float >( m_donor.eblength - ( m_donor.ticks_left - 1 ) ) );
	if ( m_donor.crouched )
		cmd->m_buttons |= in_duck | in_bullrush;
	else
		cmd->m_buttons &= ~in_duck;
	if ( m_donor.strafing ) {
		cmd->m_forward_move  = m_donor.forwardmove;
		cmd->m_side_move     = m_donor.sidemove;
		cmd->m_mouse_delta_x = m_donor.mousedx;
		const c_angle ang( view_backup.m_x + 18.f, yaw, 0.f );
		cmd->m_view_point.m_y = yaw;
		donor_fix_move( cmd, ang );
	} else {
		cmd->m_forward_move = 0.f;
		cmd->m_side_move    = 0.f;
	}
	m_donor.ticks_left--;
	m_replay_hits = m_donor.eblength - m_donor.ticks_left;
}

void n_edgebug::impl_t::dna_frame_start( )
{
	if ( !m_donor.ticks_left || !m_donor.strafing || !g_ctx.m_local || !g_ctx.m_local->is_alive( ) )
		return;
	const float ipt       = g_interfaces.m_global_vars_base->m_interval_per_tick;
	const float to_eb     = static_cast< float >( m_donor.edgebugtick - m_donor.detecttick ) * ipt;
	const float from_find = g_interfaces.m_global_vars_base->m_current_time - static_cast< float >( m_donor.detecttick ) * ipt;
	c_angle view( g_ctx.m_first_view_angles.m_x, m_donor.startingyaw, g_ctx.m_first_view_angles.m_z );
	view.m_y += donor_yaw( m_donor.yawdelta * ( static_cast< float >( m_donor.eblength ) * ( from_find / to_eb ) ) );
	g_interfaces.m_engine_client->set_view_angles( view );
}
