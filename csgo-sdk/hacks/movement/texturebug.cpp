#include "texturebug.h"
#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"
#include "movement.h"
#include "../prediction/prediction.h"
#include "texturebug_common.h"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <limits>
#include "wall_climb.h"
#include "movement_recorder.h"

using namespace n_tb;

extern void CorrectMovement( c_user_cmd* cmd, c_angle wish_angle, c_angle old_angles );
extern void botox_dbg_log( const char* fmt, ... );
extern bool botox_is_ladder_trace( const trace_t& tr );
extern bool g_air_stuck_owned_prev_tick;

/* head bounce: a bounce must keep this share of entry xy */
constexpr float k_xy_creep_frac = 0.5f;

constexpr int k_hb_chain = 3;
constexpr int k_hb_chain_max  = 6;
/* rise overhead must eat to count as a bounce. wish can't change vz, so free fall is known:
   1 u/s is well under a crease clip (~2.9) and above float noise (contact-free reads 0.000). */
constexpr float k_hb_cut_min = 1.f;
constexpr int k_hb_refine = 4;
constexpr float k_hb_side_speed = 30.f;
constexpr float k_hb_unduck_dz = 9.f;
constexpr float k_hb_stand_h   = 72.f;

inline bool tb_jump_trick_key( )
{
	const auto on = [ ]( bool enabled, key_bind_t& key ) { return enabled && g_input.check_input( &key ); };
	return on( GET_VARIABLE( g_variables.m_mini_jump, bool ), GET_VARIABLE( g_variables.m_mini_jump_key, key_bind_t ) ) ||
	       on( GET_VARIABLE( g_variables.m_long_jump, bool ), GET_VARIABLE( g_variables.m_long_jump_key, key_bind_t ) ) ||
	       on( GET_VARIABLE( g_variables.m_edge_jump, bool ), GET_VARIABLE( g_variables.m_edge_jump_key, key_bind_t ) ) ||
	       on( GET_VARIABLE( g_variables.m_jump_bug, bool ), GET_VARIABLE( g_variables.m_jump_bug_key, key_bind_t ) );
}

inline bool s_tb_owned_prev = false;

/* the sent cmd's simmed END ( resim at the bottom of on_create_move ). next cmd's START must equal it: TB line div= / dvz= is how far
   the server ( or a feature after tb rewriting the cmd ) moved off our sim. -1 = no sent sim last cmd */
static int s_sent_cmd = -1;
static c_vector s_sent_org{ }, s_sent_vel{ };

// Auto-align also uses this yaw helper. Texturebug itself uses predicted inputs.
float tb_wallstrafe_yaw( const c_vector& n, const c_vector& vel, const float cap )
{
	if ( vel.length_2d( ) < 1.f )
		return FLT_MAX;
	const float nl = std::sqrt( n.m_x * n.m_x + n.m_y * n.m_y );
	const float nx = n.m_x / nl, ny = n.m_y / nl;
	float tx = -ny, ty = nx;
	if ( tx * vel.m_x + ty * vel.m_y < 0.f ) {
		tx = -tx;
		ty = -ty;
	}
	const float s = tx * vel.m_x + ty * vel.m_y;
	float c = 1.f, sn = 0.f;
	if ( s >= cap * 0.5f ) {
		c  = std::clamp( cap / ( s * 2.f ), -1.f, 1.f );
		sn = std::sqrt( 1.f - c * c );
	}
	return rad2deg( std::atan2f( c * ty - sn * ny, c * tx - sn * nx ) );
}

inline void tb_steer_into( c_user_cmd* cmd, const c_vector& wall_normal )
{
	constexpr float k_steer_forward = 450.f;

	c_angle angle_wall = ( wall_normal * -1.f ).to_angle( );
	angle_wall.m_y     = g_math.normalize_angle( angle_wall.m_y );

	const c_angle original_view = cmd->m_view_point;
	const float raw_forward     = cmd->m_forward_move;
	const float raw_side        = cmd->m_side_move;
	cmd->m_forward_move         = k_steer_forward;
	cmd->m_side_move            = 0.f;
	c_angle wish                = original_view;
	wish.m_y                    = angle_wall.m_y;
	CorrectMovement( cmd, wish, original_view );
	cmd->m_view_point = original_view;
	if ( tb_move_latches_ladder( cmd, raw_forward == 0.f && raw_side == 0.f ) ) {
		cmd->m_forward_move = raw_forward;
		cmd->m_side_move    = raw_side;
	}
}

void n_texturebug::impl_t::on_create_move( c_user_cmd* cmd )
{
	s_tb_owned_prev   = m_hit || m_hs_hit || m_hb_hit || m_acted || m_owns_cmd;
	m_acted           = false;
	m_assist          = false;
	reset_command( );
	m_hb_hit          = false;
	s_sims_this_cmd   = 0;
	s_traces_this_cmd = 0;
	s_trace_qpc_this_cmd = 0ll;
	s_trace_timing = GET_VARIABLE( g_variables.m_debug_log, bool );
	s_pred_dirty        = true;
	s_restores_this_cmd = 0;
	s_restores_skipped  = 0;
	s_bounds_next       = g_prediction.m_real_max_speed;
	s_budget.start( k_cmd_time_share,
	                g_interfaces.m_global_vars_base ? g_interfaces.m_global_vars_base->m_interval_per_tick : 0.f, n_tick::search_tb );
	s_budget_live = true;
	s_target_predict_z_vel = g_prediction.get_engine_target_predict_z_velocity( );
	s_sim_reserve = ( GET_VARIABLE( g_variables.m_air_stuck, bool ) &&
	                  g_input.check_input( &GET_VARIABLE( g_variables.m_air_stuck_key, key_bind_t ) ) )
	                    ? k_as_sim_reserve
	                    : 0;

	if ( !cmd )
		return;

	static const char* s_trick_pending = nullptr;
	static bool s_caught_prev          = false;
	if ( s_trick_pending && g_ctx.m_local ) {
		pf_check_trick( s_trick_pending, g_prediction.backup_data.m_origin, ( g_prediction.backup_data.m_flags & fl_ducking ) != 0 );
		s_trick_pending = nullptr;
	}

	/* a wall climb CATCH (landing / seam jump fix, PRE, simmed as sent): a pin rewrite and it never lands.
	   A tb ride already running made WC yield, so this is a fresh contact only */
	if ( g_wall_climb.caught( cmd ) ) {
		m_hit = m_hs_hit = m_owns_cmd = false;
		s_budget_live = false;
		return;
	}
	if ( ( g_prediction.backup_data.m_flags & fl_onground ) && tb_jump_trick_key( ) ) {
		m_hit = m_hs_hit = m_owns_cmd = false;
		s_budget_live = false;
		return;
	}

	const float pre_forward = cmd->m_forward_move;
	const float pre_side    = cmd->m_side_move;
	const int pre_buttons   = cmd->m_buttons;
	m_input_forward = pre_forward;
	m_input_side = pre_side;
	m_input_yaw = cmd->m_view_point.m_y;

	this->tb_auto_align( cmd );
	const float align_forward = cmd->m_forward_move;
	const float align_side    = cmd->m_side_move;
	const int align_buttons   = cmd->m_buttons;
	this->texture_bug( cmd );
	this->head_bounce( cmd );
	const bool caught = m_hit || m_hb_hit;
	if ( caught && !s_caught_prev && GET_VARIABLE( g_variables.m_debug_log, bool ) )
		s_trick_pending = m_hb_hit ? "head_bounce" : "texture_bug";
	s_caught_prev = caught;

	s_budget_us_last          = s_budget.used_us( );
	const bool was_time_bound = sims_left( ) <= 0;
	s_budget_live             = false;
	if ( GET_VARIABLE( g_variables.m_debug_log, bool ) ) {
		static long long worst_us = 0ll, sum_us = 0ll, worst_tr_us = 0ll, sum_tr_us = 0ll;
		static int rolls = 0, worst_sims = 0, worst_tr = 0, capped = 0, last_sec = -1, res_sum = 0, skip_sum = 0;
		res_sum += s_restores_this_cmd;
		skip_sum += s_restores_skipped;
		worst_us   = std::max( worst_us, s_budget_us_last );
		worst_sims = std::max( worst_sims, s_sims_this_cmd );
		worst_tr   = std::max( worst_tr, s_traces_this_cmd );
		const long long tr_us_this_cmd = tb_trace_us( );
		worst_tr_us = std::max( worst_tr_us, tr_us_this_cmd );
		sum_tr_us += tr_us_this_cmd;
		capped += was_time_bound;
		sum_us += s_budget_us_last;
		++rolls;
		const int sec = static_cast< int >( g_interfaces.m_global_vars_base->m_current_time );
		if ( sec != last_sec ) {
			last_sec = sec;
			botox_dbg_log( "TBP: worst=%lldus avg=%lldus sims_worst=%d tr_worst=%d tr_us_worst=%lld "
			               "tr_us_avg=%lld capped=%d/%d res=%d skip=%d",
			               worst_us, rolls > 0 ? sum_us / rolls : 0ll, worst_sims, worst_tr, worst_tr_us,
			               rolls > 0 ? sum_tr_us / rolls : 0ll, capped, rolls, res_sum, skip_sum );
			worst_us = sum_us = worst_tr_us = sum_tr_us = 0ll;
			rolls = worst_sims = worst_tr = capped = res_sum = skip_sum = 0;
		}
	}

	m_resimmed = false;
	s_sent_cmd = -1;
	if ( g_ctx.m_local && ( s_sims_this_cmd > 0 || s_restores_this_cmd > 0 ) ) {
		tb_restore( g_interfaces.m_prediction->m_commands_predicted - 1 );
		pred_simulate( cmd );
		m_resimmed     = true;
		m_sent_face_dz = g_prediction.m_last_face_dz;
		s_sent_cmd     = cmd->m_command_number;
		s_sent_org     = g_ctx.m_local->get_origin( );
		s_sent_vel     = g_ctx.m_local->get_velocity( );
	}

	m_acted = cmd->m_forward_move != pre_forward || cmd->m_side_move != pre_side || cmd->m_buttons != pre_buttons;
	/* Only auto-align's run-up steer is an assist. Proven wallstrafe owns movement. */
	m_assist = m_acted && !m_hit && !m_hb_hit && !m_owns_cmd &&
	           cmd->m_forward_move == align_forward && cmd->m_side_move == align_side && cmd->m_buttons == align_buttons;
	if ( GET_VARIABLE( g_variables.m_texture_bug_wallstrafe, bool ) && ( caught || m_owns_cmd ) )
		m_owned_move.capture( *cmd );
}

void n_texturebug::impl_t::preserve_move( c_user_cmd* cmd )
{
	if ( !cmd || !g_ctx.m_local || !g_ctx.m_local->is_alive( ) ||
	     !GET_VARIABLE( g_variables.m_texture_bug, bool ) ||
	     !GET_VARIABLE( g_variables.m_texture_bug_wallstrafe, bool ) ||
	     !g_input.check_input( &GET_VARIABLE( g_variables.m_texture_bug_key, key_bind_t ) ) )
		return;
	if ( g_movement_recorder.owns_cmd( ) || g_movement.m_fireman_data.owns_cmd ||
	     ( GET_VARIABLE( g_variables.m_air_stuck, bool ) &&
	       g_input.check_input( &GET_VARIABLE( g_variables.m_air_stuck_key, key_bind_t ) ) ) ||
	     ( GET_VARIABLE( g_variables.m_air_freeze, bool ) &&
	       g_input.check_input( &GET_VARIABLE( g_variables.m_air_freeze_key, key_bind_t ) ) ) || g_wall_climb.caught( cmd ) ) {
		reset_command( );
		return;
	}
	constexpr int movement_mask = in_forward | in_back | in_moveleft | in_moveright | in_jump | in_duck | in_run | in_speed | in_walk | in_bullrush;
	m_owned_move.apply( *cmd, movement_mask );
}

// After a confirmed surf, assistance needs a wall at head height rather than
// just beside the body. Initial approaches keep their original inward push.
// Native prediction still decides actual catches, including valid feet pins.
static bool tb_head_wall_reachable( c_base_entity* local, const c_vector& origin, const c_vector& end,
                                    const c_vector& mins, const c_vector& maxs, const c_vector& normal )
{
	constexpr float epsilon = 0.03125f;
	c_vector head_mins = mins, head_maxs = maxs;
	head_mins.m_z = maxs.m_z - epsilon;
	head_maxs.m_z = maxs.m_z + epsilon;
	trace_t wall{ };
	c_trace_filter_tb_world_props filter( local );
	// Start just outside the face so a flush hull is not reported start-solid.
	ray_t ray( origin + normal * ( 2.f * epsilon ), end, head_mins, head_maxs );
	tb_trace( ray, mask_playersolid, &filter, &wall );
	const float same_face = wall.m_plane.m_normal.m_x * normal.m_x + wall.m_plane.m_normal.m_y * normal.m_y +
	                        wall.m_plane.m_normal.m_z * normal.m_z;
	return !wall.m_start_solid && !wall.m_all_solid && wall.m_fraction < 1.f &&
	       std::fabs( wall.m_plane.m_normal.m_z ) < 0.1f && same_face >= 0.98f && !botox_is_ladder_trace( wall );
}

/* texture bug = box-brush pin ( IntersectRayWithBoxBrush ): the head crosses a wall brush's bottom ( or feet a flush lower brush's top )
   as the move crosses the wall plane by dx: engine picks the +z face, vz clipped to -g*dt/2. needs gap ~1 ulp, a hard hit parks at 0.03125 */
void n_texturebug::impl_t::texture_bug( c_user_cmd* cmd )
{
	constexpr float k_eps        = 0.03125f;
	constexpr float k_touch      = k_eps * 1.3f;
	constexpr float k_contact    = 0.05f;
	constexpr float k_ratio      = 1.4f;
	/* 10-06 log: a wall top bounce window was dx 1.08e-3..1.25e-3, x1.4 rungs 9.0e-4 -> 1.26e-3 stepped over it */
	constexpr float k_rise_ratio = 1.1f;
	constexpr int k_ladder_cap   = 48;
	constexpr float k_duck_crop  = 0.34f;
	constexpr float k_wish_max   = 450.f;
	constexpr int k_move_keys    = in_forward | in_back | in_moveleft | in_moveright;
	static const float k_keep[]  = { 0.000001f, 0.0001f, 0.002f, 0.05f, 0.3f };
	constexpr int k_keep_max     = 4;

	static int s_ride_duck = -1;
	static int s_ride_user = -1;
	static int s_miss      = 0;
	static int s_run       = 0;
	static int s_pin_cmd   = -1;
	static int s_park_margin = 0, s_park_cmd = -1;
	static float s_park_from = 0.f, s_park_to = 0.f;
	static bool s_rode       = false;
	const auto drop = [ & ]( ) {
		s_rode        = false;
		s_ride_duck   = -1;
		s_miss        = 0;
		s_run         = 0;
		s_park_margin = 0;
		s_park_cmd    = -1;
	};
	m_hit = m_hs_hit = m_owns_cmd = false;

	c_base_entity* const local = g_ctx.m_local;
	if ( !GET_VARIABLE( g_variables.m_texture_bug, bool ) || !g_input.check_input( &GET_VARIABLE( g_variables.m_texture_bug_key, key_bind_t ) ) ||
	     !local || !local->is_alive( ) || !local->get_collideable( ) ) {
		drop( );
		return;
	}
	if ( ( g_prediction.backup_data.m_flags & fl_onground ) || g_prediction.backup_data.m_move_type != e_move_types::move_type_walk ) {
		drop( );
		return;
	}

	const int frame = g_interfaces.m_prediction->m_commands_predicted - 1;
	tb_restore( frame );
	const c_vector org    = local->get_origin( );
	const c_vector vel    = local->get_velocity( );
	const bool ducked0    = ( local->get_flags( ) & fl_ducking ) != 0;
	const float friction  = local->get_surface_friction( );
	const c_vector mins   = local->get_collideable( )->get_obb_mins( );
	const c_vector maxs   = local->get_collideable( )->get_obb_maxs( );
	const float ipt       = g_interfaces.m_global_vars_base->m_interval_per_tick;
	const float pin       = s_target_predict_z_vel;
	static auto sv_airaccelerate = g_interfaces.m_convar->find_var( "sv_airaccelerate" );
	const float k_acc            = ( sv_airaccelerate ? sv_airaccelerate->get_float( ) : 12.f ) * ipt * friction;
	const bool debug             = GET_VARIABLE( g_variables.m_debug_log, bool );
	if ( debug ) {
		const auto nci        = g_interfaces.m_engine_client->get_net_channel_info( );
		const bool loopback   = nci && nci->is_loopback( );
		const char* map       = g_interfaces.m_engine_client->get_level_name_short( );
		static auto sv_gravity = g_interfaces.m_convar->find_var( "sv_gravity" );
		static char s_env[ 160 ] = { };
		char env[ 160 ];
		std::snprintf( env, sizeof( env ), "map=%s tick=%.0f loopback=%d airaccel=%.1f gravity=%.0f", map ? map : "?",
		               ipt > 0.f ? 1.f / ipt : 0.f, loopback ? 1 : 0, sv_airaccelerate ? sv_airaccelerate->get_float( ) : -1.f,
		               sv_gravity ? sv_gravity->get_float( ) : -1.f );
		if ( std::strcmp( env, s_env ) != 0 ) {
			std::memcpy( s_env, env, sizeof( env ) );
			botox_dbg_log( "TBENV: %s", env );
		}
	}
	const bool div_ok            = s_sent_cmd >= 0 && cmd->m_command_number == s_sent_cmd + 1;
	const float div_org          = div_ok ? ( org - s_sent_org ).length( ) : -1.f;
	const float div_vz           = div_ok ? vel.m_z - s_sent_vel.m_z : 0.f;

	const c_user_cmd in = *cmd;
	const bool wallstrafe_enabled = GET_VARIABLE( g_variables.m_texture_bug_wallstrafe, bool );
	int sims            = 0;
	/* rising START: a pin here is a head bounce ( seam / ceiling face clipped the rise ), one tick, never a ride.
	   START vz == -pin exactly lands on the pin with no contact */
	const bool rising = vel.m_z + pin > 1e-3f;
	struct end_t {
		bool ok = false;
		c_vector org{ }, vel{ };
		bool ride = false, bounce = false, head = false, ground = false;
	};
	const auto sim = [ & ]( c_user_cmd c ) {
		end_t e;
		if ( sims_left( ) <= 0 )
			return e;
		tb_restore( frame );
		pred_simulate( &c );
		++sims;
		e.ok   = true;
		e.org  = local->get_origin( );
		e.ground = ( local->get_flags( ) & fl_onground ) != 0;
		e.vel  = local->get_velocity( );
		const bool pinned = !( local->get_flags( ) & fl_onground ) && local->get_move_type( ) == e_move_types::move_type_walk &&
		                    std::fabsf( e.vel.m_z - pin ) < 1e-3f && e.vel.length_2d( ) >= 1.f;
		e.ride   = pinned && vel.m_z + pin < 0.f;
		e.bounce = pinned && rising && ( !wallstrafe_enabled || n_tb::wallstrafe::retains_speed( vel.length_2d( ), e.vel.length_2d( ) ) );
		e.head = g_prediction.m_last_face_dz != n_prediction::impl_t::k_no_face && g_prediction.m_last_face_dz > 1.f;
		e.ok   = local->get_move_type( ) == e_move_types::move_type_walk; /* a ladder latch is never ours */
		return e;
	};
	const auto with_duck = [ ]( c_user_cmd c, const int duck ) {
		c.m_buttons = duck ? ( c.m_buttons | in_duck ) : ( c.m_buttons & ~in_duck );
		return c;
	};
	char act        = 'N';
	float gap       = -1.f, dx_hit = 0.f, v_in = 0.f;
	bool flipped    = false, head = false;
	int lk = -1, look_a = -1, look_b = -1;
	static char s_last_act = 'N';
	static int s_touch_cmd = -1;
	const auto log  = [ & ]( ) {
		s_last_act = act;
		if ( debug )
			botox_dbg_log( "TB: act=%c gap=%.6f vz=%.2f xy=%.1f duck=%d lock=%d run=%d flip=%d head=%d dx=%.3e sims=%d us=%lld div=%.3e dvz=%.4f mg=%d vin=%.3f ud=%d lk=%d la=%d lb=%d org=%.5f,%.5f,%.4f vel=%.3f,%.3f",
			               act, gap, vel.m_z, vel.length_2d( ), ( cmd->m_buttons & in_duck ) ? 1 : 0, s_ride_duck, s_run, ( int )flipped, ( int )head,
			               dx_hit, sims, s_budget.used_us( ), div_org, div_vz, s_park_margin, v_in, ( in.m_buttons & in_duck ) ? 1 : 0, lk, look_a, look_b,
			               org.m_x, org.m_y, org.m_z, vel.m_x, vel.m_y );
	};

	const int user_duck = ( in.m_buttons & in_duck ) ? 1 : 0;
	if ( s_ride_duck >= 0 && user_duck != s_ride_user )
		s_ride_duck = -1;
	const int held      = s_ride_duck >= 0 ? s_ride_duck : user_duck;
	const auto& tb_types   = GET_VARIABLE( g_variables.m_texture_bug_types, std::vector< bool > );
	bool allow_duck        = tb_types.size( ) > 0 && tb_types[ 0 ];
	bool allow_stand       = tb_types.size( ) > 1 && tb_types[ 1 ];
	if ( !allow_duck && !allow_stand )
		allow_duck = allow_stand = true;
	const auto allowed    = [ & ]( const int duck ) { return duck ? allow_duck : allow_stand; };
	/* rising = a bounce: your stance, never a flip, never kept for the fall */
	const int stance      = rising ? held : allowed( held ) ? held : !held;
	const c_user_cmd user = with_duck( in, stance );
	const auto commit = [ & ]( const c_user_cmd& c ) {
		cmd->m_forward_move = c.m_forward_move;
		cmd->m_side_move    = c.m_side_move;
		cmd->m_buttons      = c.m_buttons;
	};

	/* nearest vertical face along 8 world yaws, within reach of the hull ( ladders never: a grab kills the pin ) */
	const float reach = std::clamp( GET_VARIABLE( g_variables.m_texture_bug_reach, float ), 0.f, 48.f );
	c_vector n{ };
	float yaw_in = 0.f;
	{
		c_trace_filter_tb_world_props filter( local );
		float best = FLT_MAX;
		for ( int i = 0; i < 8; ++i ) {
			const float a = deg2rad( static_cast< float >( i < 4 ? i * 90 : ( i - 4 ) * 90 + 45 ) );
			trace_t tr{ };
			ray_t ray( org, org + c_vector( std::cosf( a ), std::sinf( a ), 0.f ) * ( reach + 0.05f ), mins, maxs );
			tb_trace( ray, mask_playersolid, &filter, &tr );
			if ( tr.m_start_solid || tr.m_fraction >= 1.f || std::fabsf( tr.m_plane.m_normal.m_z ) > 0.05f || botox_is_ladder_trace( tr ) )
				continue;
			const c_vector& tn = tr.m_plane.m_normal;
			/* nearest hull corner to the plane: a face leaning over you ( n.z < 0 ) meets the head, xy-only read it |n.z|*72 too far */
			const float g = org.m_x * tn.m_x + org.m_y * tn.m_y + org.m_z * tn.m_z - tr.m_plane.m_distance +
			                std::min( tn.m_x * mins.m_x, tn.m_x * maxs.m_x ) + std::min( tn.m_y * mins.m_y, tn.m_y * maxs.m_y ) +
			                std::min( tn.m_z * mins.m_z, tn.m_z * maxs.m_z );
			if ( g < best ) {
				best = g;
				n    = tn;
			}
		}
		if ( best == FLT_MAX ) {
			/* contact last cmd, no wall now = cleared the top ( rising ) or fell past the bottom */
			if ( debug && s_touch_cmd == cmd->m_command_number - 1 )
				botox_dbg_log( "TB: off wall last=%c vz=%.2f org=%.5f,%.5f,%.4f", s_last_act, vel.m_z, org.m_x, org.m_y, org.m_z );
			drop( );
			return;
		}
		gap    = best;
		if ( gap <= k_contact )
			s_touch_cmd = cmd->m_command_number;
		yaw_in = rad2deg( std::atan2f( -n.m_y, -n.m_x ) );
	}
	v_in = -( vel.m_x * n.m_x + vel.m_y * n.m_y );
	/* one ulp of the coordinate the wall normal moves: rungs / park margins finer than that move the hull the same */
	float ulp = 1e-7f;
	{
		const float coord = std::max( std::fabsf( org.m_x * n.m_x ), std::fabsf( org.m_y * n.m_y ) );
		int ex            = 0;
		std::frexp( coord, &ex );
		if ( coord > 0.f )
			ulp = std::ldexp( 1.f, ex - 24 );
	}
	if ( s_park_cmd >= 0 && cmd->m_command_number == s_park_cmd + 1 ) {
		if ( s_park_from - gap < 0.5f * ( s_park_from - s_park_to ) || ( s_park_to < 0.5f * k_eps && gap > 0.9f * k_eps ) ) {
			s_park_margin = s_park_margin ? s_park_margin * 2 : 2;
			if ( debug )
				botox_dbg_log( "TB: park refused from=%.6f plan=%.6f got=%.6f vin=%.3f margin=%d", s_park_from, s_park_to, gap, v_in, s_park_margin );
		} else
			s_park_margin = 0;
	}
	s_park_cmd = -1;
	const float xy0 = vel.length_2d( );
	bool user_dies  = false;
	c_user_cmd raw = in;
	raw.m_view_point.m_y = m_input_yaw;
	raw.m_forward_move = m_input_forward;
	raw.m_side_move = m_input_side;
	if ( wallstrafe_enabled && tb_steering_away_from_wall( &raw, n, raw.m_forward_move, raw.m_side_move ) ) {
		drop( );
		cmd->m_forward_move = raw.m_forward_move;
		cmd->m_side_move = raw.m_side_move;
		act = 'S';
		log( );
		return;
	}
	float user_along_speed = -FLT_MAX;
	if ( const end_t e = sim( user ); e.ok && ( e.ride || e.bounce ) ) {
		commit( user );
		if ( e.bounce ) {
			m_hb_hit = true;
			act      = 'B';
			log( );
			return;
		}
		m_hit = true;
		m_hs_hit = head = e.head;
		s_miss = 0;
		s_rode = true;
		s_pin_cmd = cmd->m_command_number;
		++s_run;
		act = 'R';
		log( );
		return;
	} else {
		user_dies = e.ok && xy0 > 50.f && e.vel.length_2d( ) < 0.2f * xy0;
		if ( e.ok && !e.ground )
			user_along_speed = ( e.vel.m_x * -n.m_y + e.vel.m_y * n.m_x ) / std::sqrt( n.m_x * n.m_x + n.m_y * n.m_y );
	}

	static int s_away = 0;
	s_away            = tb_steering_away_from_wall( &in, n, in.m_forward_move, in.m_side_move ) ? s_away + 1 : 0;
	if ( s_away > ( s_run > 0 ? 1 : 0 ) ) {
		drop( );
		act = 'S';
		log( );
		return;
	}

	/* v_plane = plane closing speed. a tilted face also closes by the fall ( vz + pin ) * n.z, the xy press makes that up.
	   v_old_n / v_plane are along n, AirAccelerate along yaw_in: / |n.xy| */
	const float nxy       = std::sqrt( n.m_x * n.m_x + n.m_y * n.m_y );
	const c_vector tangent( -n.m_y / nxy, n.m_x / nxy, 0.f );
	const float along = vel.m_x * tangent.m_x + vel.m_y * tangent.m_y;
	const float direction = n_tb::wallstrafe::direction( along, along );
	const auto press_from = [ & ]( const float v_old_n, const bool dk0, const float v_plane, const int duck, const float vz, bool keep_speed = true ) {
		const float v_old = v_old_n / nxy;
		const float v_t   = ( v_plane + ( vz + pin ) * n.m_z ) / nxy;
		const float crop  = ( duck || dk0 ) ? k_duck_crop : 1.f;
		float w          = v_t >= v_old ? std::max( ( v_t - v_old ) / k_acc, v_t ) : -std::max( ( v_old - v_t ) / k_acc, -v_t );
		w                = std::clamp( w / crop, -k_wish_max, k_wish_max );
		c_user_cmd c     = with_duck( in, duck );
		c.m_forward_move = std::fabsf( w );
		c.m_side_move    = 0.f;
		if ( wallstrafe_enabled && rising && keep_speed ) {
			const float speed = std::min( std::fabsf( w ) * crop, std::max( 0.f, local->get_max_speed( ) ) );
			const float sign = w < 0.f ? -1.f : 1.f;
			const float delta = sign * std::min( k_acc * speed, std::max( 0.f, std::min( speed, 30.f ) - v_old * sign ) );
			const float max_wish = std::min( k_wish_max * crop, std::max( 0.f, local->get_max_speed( ) ) );
			const auto wish = n_tb::wallstrafe::aligned_wish( v_old, along * direction, delta, max_wish, k_acc );
			if ( wish.gain > 1e-4f ) {
				c.m_forward_move = wish.into / crop;
				c.m_side_move = wish.along * direction / crop;
				move_fix_to_yaw( yaw_in, c );
				c.m_buttons &= ~k_move_keys;
				return c;
			}
		}
		move_fix_to_yaw( w >= 0.f ? yaw_in : yaw_in + 180.f, c );
		c.m_buttons &= ~k_move_keys;
		return c;
	};
	const float v_old = -( vel.m_x * n.m_x + vel.m_y * n.m_y );
	const auto press  = [ & ]( const float v_t, const int duck ) { return press_from( v_old, ducked0, v_t, duck, vel.m_z ); };

	// Probe a small shortlist once, from the same restored frame as the catch
	// search. Keep it pending until catch/precision corrections have had priority.
	c_user_cmd strafe_cmd{ };
	bool have_strafe = false;
	bool head_assist_blocked = false;
	if ( wallstrafe_enabled && sims_left( ) > 3 ) {
		const float yaw = deg2rad( raw.m_view_point.m_y );
		const c_vector wish( std::cosf( yaw ) * raw.m_forward_move + std::sinf( yaw ) * raw.m_side_move,
		                     std::sinf( yaw ) * raw.m_forward_move - std::cosf( yaw ) * raw.m_side_move, 0.f );
		const float direction = n_tb::wallstrafe::direction( wish.m_x * tangent.m_x + wish.m_y * tangent.m_y, along );
		const float crop = ( stance || ducked0 ) ? k_duck_crop : 1.f;
		const float wishspeed = std::min( k_wish_max * crop, std::max( 0.f, local->get_max_speed( ) ) );
		const auto wishes = n_tb::wallstrafe::candidates( v_old / nxy, along * direction, wishspeed, k_acc );
		float best_speed = std::max( along * direction, user_along_speed == -FLT_MAX ? along * direction : user_along_speed * direction );
		for ( const auto& w : wishes ) {
			if ( w.gain <= 1e-4f || sims_left( ) <= 0 )
				continue;
			c_user_cmd c = with_duck( in, stance );
			c.m_forward_move = w.into * k_wish_max;
			c.m_side_move = w.along * direction * k_wish_max;
			move_fix_to_yaw( yaw_in, c );
			c.m_buttons &= ~k_move_keys;
			const end_t e = sim( c );
			if ( !e.ok || e.ground || !n_tb::wallstrafe::retains_speed( xy0, e.vel.length_2d( ) ) )
				continue;
			// Check the wall still exists at the predicted position, including
			// corners and ladders; an old infinite plane is insufficient here.
			trace_t wall{ };
			c_trace_filter_tb_world_props filter( local );
			auto* col = local->get_collideable( );
			if ( !col )
				continue;
			ray_t ray( e.org, e.org - n * ( reach + k_touch ), col->get_obb_mins( ), col->get_obb_maxs( ) );
			tb_trace( ray, mask_playersolid, &filter, &wall );
			const float same_face = wall.m_plane.m_normal.m_x * n.m_x + wall.m_plane.m_normal.m_y * n.m_y + wall.m_plane.m_normal.m_z * n.m_z;
			const float end_gap = gap + ( e.org.m_x - org.m_x ) * n.m_x + ( e.org.m_y - org.m_y ) * n.m_y + ( e.org.m_z - org.m_z ) * n.m_z;
			if ( wall.m_start_solid || wall.m_all_solid || wall.m_fraction >= 1.f || same_face < 0.98f || botox_is_ladder_trace( wall ) ||
			     end_gap > std::max( gap, k_touch ) + k_touch )
				continue;
			if ( e.ride || e.bounce ) {
				commit( c );
				m_hit = e.ride;
				s_rode = s_rode || e.ride;
				m_hb_hit = e.bounce;
				m_hs_hit = e.ride && e.head;
				s_miss = 0;
				s_ride_duck = stance;
				s_ride_user = user_duck;
				s_pin_cmd = cmd->m_command_number;
				++s_run;
				act = 'P';
				log( );
				return;
			}
			if ( s_rode && !rising && !tb_head_wall_reachable( local, e.org, e.org - n * ( reach + k_touch ),
			                                                   col->get_obb_mins( ), col->get_obb_maxs( ), n ) ) {
				head_assist_blocked = true;
				continue;
			}
			const float speed = ( e.vel.m_x * tangent.m_x + e.vel.m_y * tangent.m_y ) * direction;
			if ( speed > best_speed + 0.005f ) {
				best_speed = speed;
				strafe_cmd = c;
				have_strafe = true;
			}
		}
	}
	const auto send_strafe = [ & ]( ) {
		commit( strafe_cmd );
		m_wallstrafed = m_owns_cmd = true;
		act = 'W';
		log( );
	};

	/* touching: dx ladder from the smallest crossing press ( feet seam ) up to the head pin bound at Hbrush = 0.
	   rungs closer than one ulp of the coordinate move the hull the same, so steps are at least one ulp */
	if ( gap <= k_touch ) {
		const float g   = std::max( gap, 1e-7f );
		const float dz  = std::fabsf( ( vel.m_z + pin ) * ipt ) + 1e-4f;
		const auto ladder = [ & ]( const int duck, c_user_cmd& out, bool& out_head ) {
			const float hp = ( duck || ducked0 ) ? 54.f : 72.f;
			const float hi = std::max( ( k_eps - g ) * dz / ( hp + k_eps ), g * 1.5f );
			/* climb past the first pin while rungs still pin, send the run's middle: the lowest pinning rung sits on the window edge
			   and a server 1 ulp off misses it */
			float run_dx[ k_ladder_cap ];
			bool run_head[ k_ladder_cap ], run_plain[ k_ladder_cap ];
			int run_n = 0, k = 0;
			const float step = rising ? k_rise_ratio : k_ratio;
			for ( float dx = g * 1.0005f + 1e-7f; dx <= hi && k < k_ladder_cap; dx = std::max( dx * step, dx + ulp ), ++k ) {
				c_user_cmd candidate = press( dx / ipt, duck );
				end_t e = sim( candidate );
				bool plain_used = false;
				if ( wallstrafe_enabled && rising && e.ok && !e.bounce && sims_left( ) > 0 ) {
					const c_user_cmd plain = press_from( v_old, ducked0, dx / ipt, duck, vel.m_z, false );
					if ( plain.m_forward_move != candidate.m_forward_move || plain.m_side_move != candidate.m_side_move ) {
						e = sim( plain );
						plain_used = true;
					}
				}
				if ( !e.ok && sims_left( ) <= 0 )
					break;
				if ( e.ok && ( e.ride || e.bounce ) ) {
					run_dx[ run_n ]     = dx;
					run_plain[ run_n ] = plain_used;
					run_head[ run_n++ ] = e.head;
				} else if ( run_n )
					break;
			}
			if ( !run_n )
				return false;
			dx_hit   = run_dx[ run_n / 2 ];
			out      = press_from( v_old, ducked0, dx_hit / ipt, duck, vel.m_z, !run_plain[ run_n / 2 ] );
			out_head = run_head[ run_n / 2 ];
			return true;
		};
		c_user_cmd hit{ };
		bool got = ladder( stance, hit, head );
		if ( !got && !rising && allowed( !stance ) && ladder( !stance, hit, head ) )
			got = flipped = true;
		if ( got && !rising && ipt > 0.01f && cmd->m_command_number != s_pin_cmd + 1 ) {
			constexpr int k_look         = 10;
			constexpr int k_look_cap     = 100;
			constexpr float k_look_ratio = 2.f;
			const auto pinned_now = [ & ]( const float vz_start ) {
				const c_vector v = local->get_velocity( );
				return !( local->get_flags( ) & fl_onground ) && local->get_move_type( ) == e_move_types::move_type_walk &&
				       std::fabsf( v.m_z - pin ) < 1e-3f && v.length_2d( ) >= 1.f && vz_start + pin < 0.f;
			};
			const auto rollout = [ & ]( c_user_cmd first, int st ) -> int {
				const int s0 = sims;
				if ( sims_left( ) <= 0 )
					return -1;
				tb_restore( frame );
				pred_simulate( &first );
				++sims;
				int r = pinned_now( vel.m_z ) ? 1 : 0, last = 0; /* the hold must show inside the rollout ( lab twin ) */
				const auto from_here = [ & ]( c_user_cmd c ) {
					tb_restore( frame );
					g_prediction.snapshot_load( 0 );
					pred_simulate( &c );
					++sims;
				};
				for ( int i = 0; i < k_look && sims - s0 < k_look_cap; ++i ) {
					if ( ( local->get_flags( ) & fl_onground ) || local->get_move_type( ) != e_move_types::move_type_walk )
						break;
					if ( sims_left( ) <= 0 || !g_prediction.snapshot_save( 0 ) )
						return -1;
					const c_vector o = local->get_origin( ), v = local->get_velocity( );
					const bool dk    = ( local->get_flags( ) & fl_ducking ) != 0;
					const float gk   = std::max( gap + ( o.m_x - org.m_x ) * n.m_x + ( o.m_y - org.m_y ) * n.m_y + ( o.m_z - org.m_z ) * n.m_z, 1e-7f );
					const float vo   = -( v.m_x * n.m_x + v.m_y * n.m_y );
					const float dzk  = std::fabsf( ( v.m_z + pin ) * ipt ) + 1e-4f;
					bool pinned      = false;
					for ( int s = 0; s < 2 && !pinned && gk <= k_touch; ++s ) {
						const int d = s ? !st : st;
						if ( s && !allowed( d ) )
							break;
						const float hi = std::max( ( k_eps - gk ) * dzk / ( ( ( d || dk ) ? 54.f : 72.f ) + k_eps ), gk * 1.5f );
						for ( float dx = gk * 1.0005f + 1e-7f; dx <= hi && sims - s0 < k_look_cap; dx = std::max( dx * k_look_ratio, dx + ulp ) ) {
							if ( sims_left( ) <= 0 )
								return -1;
							from_here( press_from( vo, dk, dx / ipt, d, v.m_z ) );
							if ( pinned_now( v.m_z ) ) {
								pinned = true;
								st     = d;
								break;
							}
						}
					}
					if ( pinned ) {
						++r;
						if ( last )
							return r + ( k_look - 1 - i );
						last = 1;
						continue;
					}
					if ( sims_left( ) <= 0 )
						return -1;
					from_here( with_duck( in, st ) );
					last = 0;
				}
				return r;
			};
			look_a = rollout( hit, ( hit.m_buttons & in_duck ) ? 1 : 0 );
			look_b = look_a >= 0 ? rollout( with_duck( in, held ), held ) : -1;
			lk     = look_a < 0 || look_b < 0 ? 2 : look_b > look_a ? 1 : 0;
			if ( lk == 1 ) {
				if ( ++s_miss > 1 ) {
					if ( s_miss > k_keep_max )
						s_ride_duck = -1;
					s_run = 0;
				}
				const c_user_cmd skip = with_duck( in, held );
				commit( skip );
				m_owns_cmd = skip.m_buttons != in.m_buttons && s_miss <= 1;
				act        = 'L';
				log( );
				return;
			}
		}
		if ( got ) {
			commit( hit );
			s_miss = 0;
			if ( rising ) {
				m_hb_hit = true;
				act      = 'B';
				log( );
				return;
			}
			s_ride_duck = ( hit.m_buttons & in_duck ) ? 1 : 0;
			s_ride_user = user_duck;
			++s_run;
			s_rode      = true;
			s_pin_cmd   = cmd->m_command_number;
			m_hit    = true;
			m_hs_hit = head;
			act      = 'P';
			log( );
			return;
		}
	}

	if ( ++s_miss > 1 ) {
		if ( gap > k_touch || s_miss > k_keep_max )
			s_ride_duck = -1;
		s_run = 0;
	}
	// A capped search is unknown, not a lost catch. Never resume assistance
	// because the remaining catch candidates could not be evaluated.
	if ( sims_left( ) <= 0 )
		have_strafe = false;
	if ( have_strafe && gap > k_touch ) {
		send_strafe( );
		return;
	}
	if ( ( reach > 0.f || gap <= k_touch ) && gap > 0.f && s_park_margin <= 16 ) {
		/* never under 1 ulp: a sub-ulp end is the plane or a hard hit on the server */
		const float floor_gap = static_cast< float >( std::max( s_park_margin, 1 ) ) * ulp;
		float last_end        = -1.f;
		for ( const float keep : k_keep ) {
			const float end = std::max( gap * keep, floor_gap );
			if ( end >= gap )
				break;
			if ( end == last_end )
				continue;
			last_end           = end;
			const c_user_cmd c = press( ( gap - end ) / ipt, stance );
			const end_t e      = sim( c );
			if ( !e.ok || e.ground || ( wallstrafe_enabled && rising && !n_tb::wallstrafe::retains_speed( xy0, e.vel.length_2d( ) ) ) )
				continue;
			const auto col = local->get_collideable( );
			if ( wallstrafe_enabled && s_rode && !rising && !e.ride && ( !col || !tb_head_wall_reachable( local, e.org, e.org - n * ( reach + k_touch ),
			                                                               col->get_obb_mins( ), col->get_obb_maxs( ), n ) ) ) {
				head_assist_blocked = true;
				continue;
			}
			const float g1     = e.org.m_x * n.m_x + e.org.m_y * n.m_y + e.org.m_z * n.m_z - ( org.m_x * n.m_x + org.m_y * n.m_y + org.m_z * n.m_z ) + gap;
			const bool touched = -( e.vel.m_x * n.m_x + e.vel.m_y * n.m_y ) <= 1e-3f;
			if ( g1 > 0.f && g1 < gap && !touched && g1 >= floor_gap * 0.5f ) {
				commit( c );
				m_owns_cmd  = true;
				s_park_cmd  = cmd->m_command_number;
				s_park_from = gap;
				s_park_to   = g1;
				act         = 'K';
				log( );
				return;
			}
		}
	}
	/* unstick: your move dead-stops while flush = the next brush's lip sticks out under 0.03125. a plain hard hit slides past it,
	   the 1 ulp park doesn't: step off the plane to the smallest gap that keeps the slide */
	if ( user_dies && gap <= k_touch ) {
		for ( const float to : { k_eps, 0.125f, 0.5f } ) {
			const c_user_cmd c = press( -( to - gap ) / ipt, stance );
			const end_t e      = sim( c );
			if ( e.ok && e.vel.length_2d( ) >= 0.2f * xy0 &&
			     ( !wallstrafe_enabled || !rising || n_tb::wallstrafe::retains_speed( xy0, e.vel.length_2d( ) ) ) ) {
				commit( c );
				m_owns_cmd = true;
				act        = 'U';
				log( );
				return;
			}
		}
	}
	if ( have_strafe && sims_left( ) > 0 ) {
		send_strafe( );
		return;
	}
	const bool fell = s_rode && s_miss > 1;
	const c_user_cmd mine = with_duck( head_assist_blocked ? raw : in, held );
	commit( mine );
	m_owns_cmd = cmd->m_buttons != in.m_buttons && s_miss <= 1;
	if ( fell )
		act = 'F';
	log( );
}

void n_texturebug::impl_t::head_bounce( c_user_cmd* cmd )
{

	static int s_plan_cmd = -1, s_plan_left = 0;
	static float s_plan_yaw = 0.f, s_plan_fwd = 0.f;
	static bool s_plan_unduck = false, s_plan_own = false;
	const bool plan_live = cmd && s_plan_left > 0 && cmd->m_command_number == s_plan_cmd + 1;
	const int plan_left  = s_plan_left;
	const float p_yaw = s_plan_yaw, p_fwd = s_plan_fwd;
	const bool p_unduck = s_plan_unduck, p_own = s_plan_own;
	s_plan_left = 0;

	if ( !GET_VARIABLE( g_variables.m_texture_bug, bool ) ||
	     !g_input.check_input( &GET_VARIABLE( g_variables.m_texture_bug_key, key_bind_t ) ) )
		return;
	if ( !cmd || !g_ctx.m_local || !g_ctx.m_local->is_alive( ) )
		return;
	const bool wallstrafe_enabled = GET_VARIABLE( g_variables.m_texture_bug_wallstrafe, bool );
	if ( m_hit || m_hs_hit || m_hb_hit )
		return;
	if ( ( g_prediction.backup_data.m_flags & fl_onground ) || g_prediction.backup_data.m_move_type != e_move_types::move_type_walk )
		return;

	const float half = s_target_predict_z_vel;

	const auto& pre          = g_prediction.backup_data;
	const c_vector start_pos = pre.m_origin;
	const c_vector base_vel  = pre.m_velocity;
	const float base_vz      = base_vel.m_z;
	if ( base_vz <= 0.f )
		return;

	const c_vector mins = g_ctx.m_local->get_collideable( )->get_obb_mins( );
	c_vector maxs       = g_ctx.m_local->get_collideable( )->get_obb_maxs( );
	maxs.m_z            = ( pre.m_flags & fl_ducking ) ? 54.f : k_hb_stand_h;
	const float dt      = g_interfaces.m_global_vars_base->m_interval_per_tick;

	const bool debug = GET_VARIABLE( g_variables.m_debug_log, bool );

	const c_angle original_view  = cmd->m_view_point;
	const float original_forward = cmd->m_forward_move;
	const float original_side    = cmd->m_side_move;
	const int original_buttons   = cmd->m_buttons;

	const int n_base            = g_interfaces.m_prediction->m_commands_predicted;
	const auto restore_original = [ & ]( ) { tb_restore( n_base - 1 ); };

	int budget = sims_left( );
	if ( budget > 40 )
		budget = 40;
	if ( budget <= 0 && !plan_live )
		return;
	int sims_used = 0;

	const float xy_base      = base_vel.length_2d( );
	/* a bounce that costs half the speed is not a catch, it is a crash into the ceiling */
	const float xy_floor    = xy_base >= 1.f ? ( wallstrafe_enabled ? xy_base - n_tb::wallstrafe::k_speed_tolerance : xy_base * k_xy_creep_frac ) : 0.f;
	const float vz_free     = base_vz + 2.f * half;

	const float rise = std::fmax( 0.f, base_vz + half ) * dt;
	const auto hb_ticks_to_gap = [ & ]( float need ) -> int {
		float climbed = 0.f, vz_i = base_vz;
		for ( int i = 1; i <= k_hb_chain_max; ++i ) {
			const float step = ( vz_i + half ) * dt;
			if ( step <= 0.f )
				return 0;
			climbed += step;
			if ( climbed >= need )
				return i;
			vz_i += 2.f * half;
		}
		return 0;
	};
	const float reach = std::fmin( 4.f + base_vz * dt * k_hb_chain_max, 48.f );
	trace_t over{ };
	const bool over_hit = tb_detect_overhead( start_pos, mins, maxs, reach, over );
	const float gap     = over_hit ? over.m_fraction * reach : 0.f;

	trace_t arc{ };
	int arc_tick   = 0;
	bool arc_clear = true;
	float arc_gap  = 0.f;
	{
		c_trace_filter_tb_world_props filter( g_ctx.m_local );
		c_vector p    = start_pos;
		float vz_i    = base_vz;
		float climbed = 0.f;
		for ( int i = 1; i <= k_hb_chain_max; ++i ) {
			const float step = ( vz_i + half ) * dt;
			if ( step <= 0.f )
				break;
			const c_vector q( p.m_x + base_vel.m_x * dt, p.m_y + base_vel.m_y * dt, p.m_z + step );
			ray_t ray( p, q, mins, maxs );
			tb_trace( ray, mask_playersolid, &filter, &arc );
			if ( arc.m_start_solid || arc.m_all_solid ) {
				arc_clear = false;
				break;
			}
			if ( arc.m_fraction < 1.f ) {
				if ( arc.m_plane.m_normal.m_z < -0.1f ) {
					arc_tick = i;
					arc_gap  = climbed + step * arc.m_fraction;
				} else {
					arc_clear = false;
				}
				break;
			}
			climbed += step;
			p    = q;
			vz_i += 2.f * half;
		}
	}
	int side_hit = 0;
	if ( arc_tick == 0 && !over_hit && xy_base > 1.f ) {
		c_trace_filter_tb_world_props filter( g_ctx.m_local );
		const c_vector lat( -base_vel.m_y / xy_base, base_vel.m_x / xy_base, 0.f );
		for ( int s = 1; s >= -1 && side_hit == 0; s -= 2 ) {
			const float vx = base_vel.m_x + lat.m_x * k_hb_side_speed * static_cast< float >( s );
			const float vy = base_vel.m_y + lat.m_y * k_hb_side_speed * static_cast< float >( s );
			c_vector p     = start_pos;
			float vz_i     = base_vz;
			float climbed  = 0.f;
			for ( int i = 1; i <= k_hb_chain_max; ++i ) {
				const float step = ( vz_i + half ) * dt;
				if ( step <= 0.f )
					break;
				const c_vector q( p.m_x + vx * dt, p.m_y + vy * dt, p.m_z + step );
				trace_t tr{ };
				ray_t ray( p, q, mins, maxs );
				tb_trace( ray, mask_playersolid, &filter, &tr );
				if ( tr.m_start_solid || tr.m_all_solid || ( tr.m_fraction < 1.f && tr.m_plane.m_normal.m_z >= -0.1f ) )
					break;
				if ( tr.m_fraction < 1.f ) {
					side_hit = s;
					arc      = tr;
					arc_tick = i;
					arc_gap  = climbed + step * tr.m_fraction;
					break;
				}
				climbed += step;
				p = q;
				vz_i += 2.f * half;
			}
		}
	}

	int unduck_need = 0;
	if ( ( pre.m_flags & fl_ducking ) && ( original_buttons & in_duck ) && !m_owns_cmd ) {
		const c_vector org_u( start_pos.m_x, start_pos.m_y, start_pos.m_z - k_hb_unduck_dz );
		const c_vector maxs_u( maxs.m_x, maxs.m_y, mins.m_z + k_hb_stand_h );
		trace_t over_u{ };
		if ( tb_detect_overhead( org_u, mins, maxs_u, reach, over_u ) )
			unduck_need = hb_ticks_to_gap( over_u.m_fraction * reach );
	}

	if ( !over_hit && arc_tick == 0 && unduck_need <= 0 )
		return;

	const int need_ticks = arc_tick > 0 ? arc_tick : ( over_hit ? hb_ticks_to_gap( gap ) : 0 );
	const bool brake     = arc_tick == 0 && arc_clear && over_hit;
	if ( need_ticks <= 0 && unduck_need <= 0 ) {
		if ( debug ) {
			static int s_far_tick = -1000;
			const int now = g_interfaces.m_global_vars_base->m_tick_count;
			if ( now - s_far_tick > 64 ) {
				s_far_tick = now;
				botox_dbg_log( "HB: far gap=%.2f rise=%.2f nz=%.2f vz=%.1f", gap, rise, over.m_plane.m_normal.m_z, base_vz );
			}
		}
		return;
	}
	const int chain   = std::clamp( need_ticks + 1, k_hb_chain, k_hb_chain_max );
	const int chain_u = std::clamp( unduck_need + 1, k_hb_chain, k_hb_chain_max );

	float best_dz = 1e9f, best_vz = 0.f, best_xy = 0.f, best_cut = 0.f;
	struct hb_best_t {
		bool m_have = false;
		int m_tick = 0, m_buttons = 0, m_refine = 0;
		float m_fwd = 0.f, m_side = 0.f, m_yaw = 0.f, m_wish = 0.f, m_xy = 0.f;
	};
	hb_best_t hb_best{ };
	int dbg_bounces = 0;

	const auto step_result = [ & ]( float vz_in ) -> int {
		const c_vector v      = g_ctx.m_local->get_velocity( );
		const float free_vz   = vz_in + 2.f * half;
		const float cut       = free_vz - v.m_z;
		const float dz        = std::fabs( v.m_z - half );
		if ( cut > best_cut ) {
			best_cut = cut;
			best_vz  = v.m_z;
			best_xy  = v.length_2d( );
		}
		if ( dz < best_dz )
			best_dz = dz;
		if ( ( g_ctx.m_local->get_flags( ) & fl_onground ) ||
		     g_ctx.m_local->get_move_type( ) != e_move_types::move_type_walk )
			return -1;
		if ( wallstrafe_enabled && v.length_2d( ) < xy_floor )
			return -1;
		if ( cut > k_hb_cut_min )
			return v.length_2d( ) >= xy_floor ? 1 : -1;
		return v.m_z > 0.f ? 0 : -1;
	};

	const trace_t& hit    = arc_tick > 0 ? arc : over;
	const char* src       = side_hit != 0 ? "side" : arc_tick > 0 ? "arc" : over_hit ? ( brake ? "brake" : "up" ) : "none";
	const float gap_log   = arc_tick > 0 ? arc_gap : gap;
	const auto log_result = [ & ]( const char* how ) {
		if ( !debug )
			return;
		botox_dbg_log( "HB: %s src=%s nz=%.2f disp=%d gap=%.2f vz=%.1f->%.1f free=%.1f tgt=%.2f cut=%.2f dz=%.2f xy=%.0f->%.0f/%.0f chain=%d/%d sims=%d side=%d udn=%d plan=%d bn=%d bxy=%.0f",
		                   how, src, hit.m_plane.m_normal.m_z, ( int )( ( hit.m_disp_flags & k_dispsurf_flag_surface ) != 0 ),
		                   gap_log, base_vz, best_vz, vz_free, half, best_cut, best_dz, xy_base, best_xy,
		                   xy_floor, need_ticks, chain, sims_used, side_hit, unduck_need, plan_live ? plan_left : 0, dbg_bounces,
		                   hb_best.m_xy );
	};

	const auto run_chain = [ & ]( c_user_cmd* c, int n ) -> int {
		for ( int i = 0; i < n; ++i ) {
			if ( sims_used >= budget || ( wallstrafe_enabled && sims_left( ) <= 0 ) )
				return -1;
			const float vz_in = g_ctx.m_local->get_velocity( ).m_z;
			++sims_used;
			pred_simulate( c );
			const int r = step_result( vz_in );
			if ( r != 0 )
				return r > 0 ? i + 1 : 0;
		}
		return 0;
	};

	const auto set_plan = [ & ]( int left, float yaw, float fwd, bool unduck, bool own ) {
		s_plan_left   = left;
		s_plan_cmd    = cmd->m_command_number;
		s_plan_yaw    = yaw;
		s_plan_fwd    = fwd;
		s_plan_unduck = unduck;
		s_plan_own    = own;
	};
	const auto write_plan = [ & ]( ) {
		cmd->m_view_point = original_view;
		cmd->m_buttons    = p_unduck ? original_buttons & ~in_duck : original_buttons;
		if ( p_own ) {
			cmd->m_forward_move = original_forward;
			cmd->m_side_move    = original_side;
			return;
		}
		cmd->m_forward_move = p_fwd;
		cmd->m_side_move    = 0.f;
		c_angle wish        = original_view;
		wish.m_y            = p_yaw;
		CorrectMovement( cmd, wish, original_view );
		cmd->m_view_point = original_view;
	};
	const auto keep_blind = [ & ]( ) {
		restore_original( );
		if ( wallstrafe_enabled ) {
			// Budget exhaustion is not evidence that a stored braking plan is
			// still safe for speed. Fall back to this tick's original movement.
			cmd->m_forward_move = original_forward;
			cmd->m_side_move = original_side;
			cmd->m_buttons = original_buttons;
			cmd->m_view_point = original_view;
			return;
		}
		write_plan( );
		m_hb_hit = true;
		set_plan( plan_left - 1, p_yaw, p_fwd, p_unduck, p_own );
		log_result( "keep-blind" );
	};
	if ( plan_live && sims_used >= budget ) {
		keep_blind( );
		return;
	}

	if ( need_ticks > 0 ) {
		restore_original( );
		c_user_cmd natural = *cmd;
		const bool nat     = run_chain( &natural, chain ) > 0;
		restore_original( );
		cmd->m_view_point   = original_view;
		cmd->m_forward_move = original_forward;
		cmd->m_side_move    = original_side;
		cmd->m_buttons      = original_buttons;
		if ( nat ) {
			m_hb_hit = true;
			log_result( "natural" );
			return;
		}
	}

	if ( plan_live ) {
		restore_original( );
		write_plan( );
		const int r = run_chain( cmd, need_ticks > 0 ? chain : chain_u );
		if ( r > 0 ) {
			restore_original( );
			cmd->m_view_point = original_view;
			m_hb_hit          = true;
			set_plan( r - 1, p_yaw, p_fwd, p_unduck, p_own );
			log_result( "keep" );
			return;
		}
		if ( r < 0 ) {
			keep_blind( );
			return;
		}
	}

	const float base_yaw = ( xy_base > 1.f ) ? rad2deg( atan2f( base_vel.m_y, base_vel.m_x ) ) : original_view.m_y;
	static constexpr float k_hb_yaw[]  = { 0.f, 15.f, -15.f, 30.f, -30.f, 60.f, -60.f, 90.f, -90.f, 120.f, -120.f, 150.f, -150.f, 180.f };
	static constexpr float k_hb_yaw_brake[] = { 180.f, 150.f, -150.f, 120.f, -120.f, 90.f, -90.f, 60.f, -60.f, 30.f, -30.f, 15.f, -15.f, 0.f };
	static_assert( std::size( k_hb_yaw ) == std::size( k_hb_yaw_brake ) );
	const float* yaws  = brake ? k_hb_yaw_brake : k_hb_yaw;
	const int n_yaws   = ( int )std::size( k_hb_yaw );
	static constexpr float k_hb_fwd[]  = { 450.f, 60.f, 15.f };

	float side_yaws[ std::size( k_hb_yaw ) ];
	if ( side_hit != 0 ) {
		std::copy( std::begin( k_hb_yaw ), std::end( k_hb_yaw ), side_yaws );
		const float aim = 90.f * static_cast< float >( side_hit );
		std::stable_sort( std::begin( side_yaws ), std::end( side_yaws ), [ aim ]( float a, float b ) {
			return std::fabs( g_math.normalize_angle( a - aim ) ) < std::fabs( g_math.normalize_angle( b - aim ) );
		} );
		yaws = side_yaws;
	}

	float speed_yaws[ std::size( k_hb_yaw ) ];
	if ( wallstrafe_enabled ) {
		std::copy( yaws, yaws + n_yaws, speed_yaws );
		std::stable_sort( std::begin( speed_yaws ), std::end( speed_yaws ), [ & ]( float a, float b ) {
			return ( base_vel + air_accel_delta( base_vel, base_yaw + a, 450.f ) ).length_2d( ) >
			       ( base_vel + air_accel_delta( base_vel, base_yaw + b, 450.f ) ).length_2d( );
		} );
		yaws = speed_yaws;
	}

	tb_dedup_t dedup{ }, dedup_u{ };
	if ( plan_live && !p_own )
		( p_unduck ? dedup_u : dedup ).seen_or_add( air_accel_delta( base_vel, p_yaw, p_fwd ) );
	float tried_yaw = 0.f, tried_fwd = 0.f;
	const auto try_variant = [ & ]( float fwd, float yaw_off, int n, int buttons, tb_dedup_t& pool ) -> int {
		if ( sims_used >= budget )
			return 0;
		const float world_yaw = g_math.normalize_angle( base_yaw + yaw_off );
		const c_vector delta  = air_accel_delta( base_vel, world_yaw, fwd );
		if ( delta.length_2d( ) < k_dup_vel_eps )
			return 0;
		if ( pool.seen_or_add( delta ) )
			return 0;
		tried_yaw = world_yaw;
		tried_fwd = fwd;
		restore_original( );
		cmd->m_view_point   = original_view;
		cmd->m_buttons      = buttons;
		cmd->m_forward_move = fwd;
		cmd->m_side_move    = 0.f;
		c_angle wish        = original_view;
		wish.m_y            = world_yaw;
		CorrectMovement( cmd, wish, original_view );
		cmd->m_view_point = original_view;
		return run_chain( cmd, n );
	};
	const auto commit = [ & ]( const char* how, int hit_tick, bool unduck, bool own ) {
		restore_original( );
		cmd->m_view_point = original_view;
		m_hb_hit          = true;
		set_plan( hit_tick - 1, tried_yaw, tried_fwd, unduck, own );
		log_result( how );
	};
	const auto refine_done = [ & ]( ) { return hb_best.m_have && hb_best.m_refine <= 0; };
	const auto keep_best   = [ & ]( int r, int sims_before ) {
		if ( hb_best.m_have && sims_used > sims_before )
			--hb_best.m_refine;
		if ( r <= 0 )
			return;
		++dbg_bounces;
		const float xy = g_ctx.m_local->get_velocity( ).length_2d( );
		if ( hb_best.m_have && xy <= hb_best.m_xy + 0.5f )
			return;
		hb_best.m_refine  = hb_best.m_have ? hb_best.m_refine : k_hb_refine;
		hb_best.m_have    = true;
		hb_best.m_tick    = r;
		hb_best.m_buttons = cmd->m_buttons;
		hb_best.m_fwd     = cmd->m_forward_move;
		hb_best.m_side    = cmd->m_side_move;
		hb_best.m_yaw     = tried_yaw;
		hb_best.m_wish    = tried_fwd;
		hb_best.m_xy      = xy;
	};
	const auto take_best = [ & ]( ) {
		cmd->m_forward_move = hb_best.m_fwd;
		cmd->m_side_move    = hb_best.m_side;
		cmd->m_buttons      = hb_best.m_buttons;
		tried_yaw           = hb_best.m_yaw;
		tried_fwd           = hb_best.m_wish;
	};

	if ( need_ticks > 0 ) {
		if ( wallstrafe_enabled && xy_base > 1.f ) {
			restore_original( );
			static auto sv_airaccelerate = g_interfaces.m_convar->find_var( "sv_airaccelerate" );
			const float acceleration = ( sv_airaccelerate ? sv_airaccelerate->get_float( ) : 12.f ) * dt * g_ctx.m_local->get_surface_friction( );
			const float crop = ( ( pre.m_flags & fl_ducking ) || ( original_buttons & in_duck ) ) ? 0.34f : 1.f;
			const float speed = std::min( 450.f * crop, std::max( 0.f, g_ctx.m_local->get_max_speed( ) ) );
			const auto wishes = n_tb::wallstrafe::candidates( 0.f, xy_base, speed, acceleration );
			// Both lateral directions can reach a ledge. Try true acceleration
			// angles before the coarse legacy sweep, within its existing budget.
			for ( const auto& wish : wishes ) {
				if ( wish.gain <= 1e-4f )
					continue;
				const float offset = rad2deg( std::atan2f( wish.into, wish.along ) );
				for ( const float sign : { 1.f, -1.f } ) {
					if ( sims_used >= budget || refine_done( ) || sims_left( ) <= 0 )
						break;
					const int before = sims_used;
					keep_best( try_variant( 450.f, sign * offset, chain, original_buttons, dedup ), before );
				}
			}
		}
		for ( float fwd : k_hb_fwd ) {
			for ( int y = 0; y < n_yaws; ++y ) {
				if ( sims_used >= budget || refine_done( ) )
					break;
				const int before = sims_used;
				keep_best( try_variant( fwd, yaws[ y ], chain, original_buttons, dedup ), before );
			}
		}
		if ( hb_best.m_have ) {
			take_best( );
			commit( "sweep", hb_best.m_tick, false, false );
			return;
		}
	}

	if ( unduck_need > 0 && sims_used < budget ) {
		const int unduck_buttons = original_buttons & ~in_duck;
		restore_original( );
		cmd->m_view_point   = original_view;
		cmd->m_forward_move = original_forward;
		cmd->m_side_move    = original_side;
		cmd->m_buttons      = unduck_buttons;
		const int r_own     = run_chain( cmd, chain_u );
		if ( r_own > 0 ) {
			commit( "unduck", r_own, true, true );
			return;
		}
		for ( float fwd : k_hb_fwd ) {
			for ( int y = 0; y < n_yaws; ++y ) {
				if ( sims_used >= budget || refine_done( ) )
					break;
				const int before = sims_used;
				keep_best( try_variant( fwd, k_hb_yaw[ y ], chain_u, unduck_buttons, dedup_u ), before );
			}
		}
		if ( hb_best.m_have ) {
			take_best( );
			commit( "unduck", hb_best.m_tick, true, false );
			return;
		}
	}

	restore_original( );
	cmd->m_view_point   = original_view;
	cmd->m_forward_move = original_forward;
	cmd->m_side_move    = original_side;
	cmd->m_buttons      = original_buttons;
	log_result( "miss" );
}

void n_texturebug::impl_t::tb_auto_align( c_user_cmd* cmd )
{
	constexpr float k_align_min_speed = 60.f;
	constexpr float k_align_face_dot  = 0.75f;
	constexpr float k_align_aimed_dot = 0.985f;
	constexpr int k_align_hold_max    = 24;
	constexpr float k_align_min_dist  = 2.f;
	constexpr float k_align_scan_cap  = 192.f;
	constexpr float k_align_step_up   = 18.f;
	constexpr float k_align_band_z    = 36.f;

	constexpr int k_align_min_ground = 5;

	constexpr int k_align_air_look  = 24;
	constexpr float k_align_air_min = 16.f;
	constexpr int k_align_face_mem  = 32;

	static int hold_run     = 0;
	static int want_tick    = -1000;
	static int ground_ticks = 0;
	static c_vector face_normal{ };
	static int face_tick        = -1000;
	static int ground_tick_last = -1000;
	static int hop_period       = 0;

	const bool dbg     = GET_VARIABLE( g_variables.m_debug_log, bool );
	const int tick_now = g_interfaces.m_global_vars_base ? g_interfaces.m_global_vars_base->m_tick_count : 0;

	const bool on_ground = g_ctx.m_local && ( g_prediction.backup_data.m_flags & fl_onground ) != 0;
	ground_ticks         = on_ground ? ground_ticks + 1 : 0;
	if ( on_ground ) {
		const int gap = tick_now - ground_tick_last;
		if ( ground_tick_last > -1000 && gap > 1 && gap <= n_tick::ticks( 64 ) )
			hop_period = gap;
		ground_tick_last = tick_now;
	}

	static int last_bail_id   = -1;
	static int last_bail_tick = -1000;
	auto bail = [ & ]( const int id, const char* why, const float a = 0.f, const float b = 0.f ) {
		if ( hold_run > 0 && on_ground )
			cmd->m_buttons |= in_jump;
		hold_run  = 0;
		want_tick = -1000;
		if ( dbg && ( id != last_bail_id || tick_now - last_bail_tick >= 64 ) ) {
			last_bail_id   = id;
			last_bail_tick = tick_now;
			botox_dbg_log( "TB ALIGN: bail=%s a=%.2f b=%.2f gt=%d", why, a, b, ground_ticks );
		}
	};

	if ( !GET_VARIABLE( g_variables.m_texture_bug, bool ) ||
	     !g_input.check_input( &GET_VARIABLE( g_variables.m_texture_bug_key, key_bind_t ) ) ) {
		bail( 0, "off" );
		return;
	}
	if ( tb_jump_trick_key( ) ) {
		bail( 18, "trick_key" );
		return;
	}
	if ( !g_ctx.m_local || !g_ctx.m_local->is_alive( ) ||
	     g_ctx.m_local->get_move_type( ) != e_move_types::move_type_walk ||
	     g_prediction.backup_data.m_move_type != e_move_types::move_type_walk ) {
		bail( 1, "movetype" );
		return;
	}

	if ( g_movement.m_user_buttons_raw & in_jump )
		want_tick = tick_now;
	const bool wants_jump = ( tick_now - want_tick ) <= n_tick::ticks( k_align_hold_max );

	const auto col = g_ctx.m_local->get_collideable( );
	if ( !col ) {
		bail( 2, "no_hull" );
		return;
	}
	const bool may_hold = on_ground && ground_ticks >= n_tick::ticks( k_align_min_ground );

	if ( on_ground && !wants_jump ) {
		hold_run = 0;
		if ( dbg && ( last_bail_id != 4 || tick_now - last_bail_tick >= 64 ) ) {
			last_bail_id   = 4;
			last_bail_tick = tick_now;
			botox_dbg_log( "TB ALIGN: bail=no_ask a=0.00 b=0.00 gt=%d", ground_ticks );
		}
		return;
	}
	const bool wc_key = GET_VARIABLE( g_variables.m_wall_climb, bool ) &&
	                    g_input.check_input( &GET_VARIABLE( g_variables.m_wall_climb_key, key_bind_t ) );
	const int wc_since = tick_now - g_wall_climb.m_act_tick;
	if ( wc_key && wc_since >= 0 && wc_since <= n_tick::ticks( 8 ) ) {
		bail( 5, "wc_owns" );
		return;
	}

	c_vector vel      = g_ctx.m_local->get_velocity( );
	vel.m_z           = 0.f;
	const float speed = vel.length_2d( );
	if ( speed < k_align_min_speed ) {
		bail( 6, "slow", speed );
		return;
	}
	const c_vector dir( vel.m_x / speed, vel.m_y / speed, 0.f );
	const float ipt = g_interfaces.m_global_vars_base->m_interval_per_tick;

	if ( !on_ground ) {
		const float look = std::min( k_align_scan_cap, speed * ipt * ( float )n_tick::ticks( k_align_air_look ) + 16.f );
		const c_vector air_start = g_prediction.backup_data.m_origin;
		const c_vector air_end( air_start.m_x + dir.m_x * look, air_start.m_y + dir.m_y * look, air_start.m_z );
		trace_t air_tr{ };
		c_trace_filter_tb_world_props air_fil( g_ctx.m_local );
		ray_t air_ray( air_start, air_end, col->get_obb_mins( ), col->get_obb_maxs( ) );
		tb_trace( air_ray, mask_playersolid, &air_fil, &air_tr );

		const c_vector an = air_tr.m_plane.m_normal;
		if ( air_tr.m_start_solid || air_tr.m_all_solid || air_tr.m_fraction >= 1.f || std::fabs( an.m_z ) >= 0.1f ) {
			bail( 13, "air_no_wall", look, an.m_z );
			return;
		}
		if ( botox_is_ladder_trace( air_tr ) ) {
			bail( 16, "ladder", air_tr.m_fraction * look );
			return;
		}
		face_normal = an;
		face_tick   = tick_now;

		const float air_dist = air_tr.m_fraction * look;
		const float air_into = -( dir.m_x * an.m_x + dir.m_y * an.m_y );
		if ( air_into < k_align_face_dot ) {
			bail( 9, "past_it", air_into );
			return;
		}
		if ( air_dist <= k_align_air_min ) {
			bail( 14, "air_close", air_dist );
			return;
		}
		if ( s_tb_owned_prev || g_air_stuck_owned_prev_tick ) {
			bail( 15, "air_busy", air_dist );
			return;
		}
		if ( tb_steering_away_from_wall( cmd, an, cmd->m_forward_move, cmd->m_side_move ) ) {
			bail( 10, "steer_away" );
			return;
		}
		if ( air_into < k_align_aimed_dot )
			tb_steer_into( cmd, an );
		if ( dbg ) {
			last_bail_id = -1;
			botox_dbg_log( "TB ALIGN AIR: d=%.1f into=%.2f look=%.0f vz=%.1f aim=%d", air_dist, air_into, look,
			                   g_ctx.m_local->get_velocity( ).m_z, ( int )( air_into < k_align_aimed_dot ) );
		}
		return;
	}

	static auto sv_jump_impulse = g_interfaces.m_convar->find_var( "sv_jump_impulse" );
	static auto sv_gravity      = g_interfaces.m_convar->find_var( "sv_gravity" );
	const float impulse = sv_jump_impulse ? sv_jump_impulse->get_float( ) : 301.993f;
	const float gravity = ( sv_gravity && sv_gravity->get_float( ) > 1.f ) ? sv_gravity->get_float( ) : 800.f;
	const float stam_raw = g_ctx.m_local->get_stamina( );
	const float stam     = ( stam_raw >= 0.f && stam_raw <= 100.f ) ? stam_raw : 0.f;
	const float jump_vz  = impulse * std::max( 0.f, 1.f - stam * 0.01f );
	if ( jump_vz < 1.f ) {
		bail( 7, "no_arc", stam_raw );
		return;
	}
	const float apex_h   = jump_vz * jump_vz / ( 2.f * gravity );
	const float want_h   = std::max( 1.f, ( jump_vz - gravity * ipt * 0.5f ) * ipt );
	const float root     = std::sqrt( std::max( 0.f, jump_vz * jump_vz - 2.f * gravity * want_h ) );
	const float t_want   = ( jump_vz - root ) / gravity;

	const float max_speed  = g_ctx.m_local->get_max_speed( );
	const float crop       = ( max_speed > 1.f && speed > 1.1f * max_speed ) ? ( 1.1f * max_speed / speed ) : 1.f;
	const int hold_max     = n_tick::ticks( k_align_hold_max );
	const float along       = speed * crop;
	const float scan        = std::min( k_align_scan_cap, along * t_want + along * ipt * ( float )hold_max + 16.f );

	const c_vector start_pos = g_prediction.backup_data.m_origin;
	const c_vector end_pos( start_pos.m_x + dir.m_x * scan, start_pos.m_y + dir.m_y * scan, start_pos.m_z );
	c_vector sweep_mins = col->get_obb_mins( );
	sweep_mins.m_z += k_align_step_up;
	trace_t tr{ };
	c_trace_filter_tb_world_props fil( g_ctx.m_local );
	ray_t ray( start_pos, end_pos, sweep_mins, col->get_obb_maxs( ) );
	tb_trace( ray, mask_playersolid, &fil, &tr );

	const c_vector n = tr.m_plane.m_normal;
	if ( tr.m_start_solid || tr.m_all_solid || tr.m_fraction >= 1.f || std::fabs( n.m_z ) >= 0.1f ) {
		bail( 8, "no_wall", tr.m_fraction * scan, n.m_z );
		if ( ( tick_now - face_tick ) <= n_tick::ticks( k_align_face_mem ) && face_normal.length_2d( ) > 0.5f ) {
			const float mem_into = -( dir.m_x * face_normal.m_x + dir.m_y * face_normal.m_y );
			if ( mem_into >= k_align_face_dot && mem_into < k_align_aimed_dot &&
			     !tb_steering_away_from_wall( cmd, face_normal, cmd->m_forward_move, cmd->m_side_move ) ) {
				tb_steer_into( cmd, face_normal );
				if ( dbg )
					botox_dbg_log( "TB ALIGN: mem=1 into=%.2f age=%d gt=%d", mem_into, tick_now - face_tick,
					                   ground_ticks );
			}
		}
		return;
	}
	if ( botox_is_ladder_trace( tr ) ) {
		bail( 17, "ladder", tr.m_fraction * scan );
		return;
	}
	face_normal      = n;
	face_tick        = tick_now;
	const float into = -( dir.m_x * n.m_x + dir.m_y * n.m_y );
	if ( into < k_align_face_dot ) {
		bail( 9, "past_it", into );
		return;
	}
	if ( tb_steering_away_from_wall( cmd, n, cmd->m_forward_move, cmd->m_side_move ) ) {
		bail( 10, "steer_away" );
		return;
	}
	const float dist   = tr.m_fraction * scan;
	const float d_want = along * t_want;
	const float travel = std::max( 1.f, along * ipt );

	const float need = ( dist - d_want ) / travel;
	if ( need > ( float )hold_max ) {
		hold_run = 0;
		if ( dbg && ( last_bail_id != 11 || tick_now - last_bail_tick >= 64 ) ) {
			last_bail_id   = 11;
			last_bail_tick = tick_now;
			botox_dbg_log( "TB ALIGN: bail=too_far a=%.1f b=%.1f gt=%d", dist, need, ground_ticks );
		}
		return;
	}

	/* face must still exist at contact height, else it's a ledge we land on. one ray, mid-hull, arc height */
	trace_t band{ };
	const c_vector band_start( start_pos.m_x, start_pos.m_y, start_pos.m_z + want_h + k_align_band_z );
	const c_vector band_end( band_start.m_x + dir.m_x * ( dist + 32.f ), band_start.m_y + dir.m_y * ( dist + 32.f ),
	                         band_start.m_z );
	ray_t band_ray( band_start, band_end );
	tb_trace( band_ray, mask_playersolid, &fil, &band );
	if ( band.m_fraction >= 1.f || std::fabs( band.m_plane.m_normal.m_z ) >= 0.1f ) {
		bail( 12, "ledge", want_h + k_align_band_z, band.m_fraction );
		return;
	}

	/* hull already on the face: hand the cmd back WITH the latched ask (else an eaten tap vanishes).
	   threshold must stay under the comparator's press window (~1.5 travel) or it swallows it. */
	if ( dist < std::max( k_align_min_dist, travel * 0.5f ) ) {
		cmd->m_buttons |= in_jump;
		hold_run  = 0;
		want_tick = -1000;
		if ( dbg )
			botox_dbg_log( "TB ALIGN: spend=on_face d=%.1f into=%.2f gt=%d", dist, into, ground_ticks );
		return;
	}

	const int step_ticks  = may_hold ? 1 : std::max( 1, hop_period );
	const float err_now   = std::fabs( dist - d_want );
	const float err_next  = std::fabs( dist - travel * ( float )step_ticks - d_want );
	bool hold             = false;
	if ( err_next < err_now && hold_run < hold_max && may_hold ) {
		cmd->m_buttons &= ~in_jump;
		hold = true;
		++hold_run;
	} else if ( err_next < err_now ) {
		hold_run = 0;
	} else {
		cmd->m_buttons |= in_jump;
		want_tick = -1000;
		hold_run  = 0;
	}

	if ( into < k_align_aimed_dot )
		tb_steer_into( cmd, n );

	if ( dbg ) {
		last_bail_id = -1;
		botox_dbg_log( "TB ALIGN: d=%.1f want=%.1f need=%.1f close=%.0f into=%.2f h=%.1f apex=%.1f vz0=%.0f "
		                   "stam=%.1f scan=%.0f k=%.1f gt=%d mh=%d hold=%d run=%d hp=%d step=%.1f",
		                   dist, d_want, need, along, into, want_h, apex_h, jump_vz, stam, scan,
		                   ipt > 0.f ? t_want / ipt : 0.f, ground_ticks, ( int )may_hold, ( int )hold, hold_run,
		                   hop_period, travel * ( float )step_ticks );
	}
}
