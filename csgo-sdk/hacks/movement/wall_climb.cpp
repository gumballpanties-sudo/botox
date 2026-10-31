#include "wall_climb.h"
#include "movement_internal.h"
#include "texturebug.h"
#include "texturebug_common.h"
#include "edgebug.h"
#include <cfloat>
#include <format>
#include <vector>

extern bool HITGODA;
extern bool HITGODA2;

// wc2 ( tools/wc_engine/fl_lab.cpp P57 ): a disp lip lands only from the first 1-2 float positions off the face, so the hull is
// parked flush every air tick ( tb style press ladder ) and caught by the same ladder, or by a pin just over the lip.
// 10-31 back from the 10-10 ebanat port: lab 5016 hops 64t, first catches ebanat 21, keys alone 186, this 2915, beam oracle 3633
namespace
{
	constexpr float k_contact      = 0.05f;
	constexpr float k_pin_reach    = 1.f;
	constexpr float k_wall_nz      = 0.35f;
	constexpr float k_hull_step    = 0.03125f;
	constexpr float k_fine_lo      = 5e-4f; /* ladder rungs in F = the wish that moves the hull 1/32u in one tick */
	constexpr float k_fine_hi      = 0.05f;
	constexpr float k_fine_ratio   = 1.3f;
	constexpr float k_away_lo      = 3e-3f; /* away rungs from here: smaller ones move the hull under 1 float */
	constexpr float k_coarse[ ]    = { 0.1f, 0.2f, 0.4f, 0.84f, 1.2f, 2.f };
	constexpr float k_along_fwd[ ] = { 5.f, 30.f, 0.875f, 15.f };
	constexpr float k_along_off[ ] = { 85.999f, 86.999f, 87.999f }; /* 111 antoha offsets, side opposite velocity's tangent */
	constexpr float k_rise_vz      = 150.f; /* CategorizePosition never grounds zvel > 140 */
	constexpr int k_hold_ticks     = 8;
	constexpr int k_pin_tries      = 3;
	constexpr int k_exit_look      = 3;
	constexpr float k_keys_ulp     = 1.3e-4f;
	constexpr float k_park_keep    = 2.f; /* park ends this much slower in xy than your keys = your keys ( lab P42: park cost -91%, catches -2.6% ) */
	constexpr float k_land_above   = 2.f;
	constexpr float k_low_ledge    = 10.f; /* a land this far under your jump floor = a ledge you fall past, not a climb */
	constexpr int k_reserve_sims   = 24;
	constexpr int k_commit_sims    = 3;
	constexpr float k_time_share   = 0.6f;
	constexpr float k_slow_xy      = 200.f;
	constexpr float k_slow_ratio   = 0.5f;
	constexpr float k_pin_sign_xy  = 5.f;
	constexpr float k_jump_share   = 0.8f; /* wc_air_jump_step: jump must reach 80% of ideal vz */
	constexpr int k_back_steps     = 16;
	constexpr float k_seam_off     = 0.03125f;
	constexpr float k_seam_below   = 64.f;
	constexpr float k_seam_floor   = 2.f;
	constexpr float k_leave_cos    = 0.5f;
	constexpr int k_robust_parks   = 3; /* lab P62 */
	constexpr float k_robust_vel   = 0.002f;
	constexpr float k_spot_step    = 2.f;
	constexpr float k_spot_rise    = 10.f;
	constexpr int k_spot_ticks     = 64;
	constexpr int k_spot_tick_sims = 20;
	constexpr float k_spot_snap    = 0.25f;
	constexpr float k_spot_look    = 4.f; /* ticks of along speed counted when walking to the spot */
	constexpr float k_spot_along[ ] = { 0.f, 10.f, -10.f, 25.f, -25.f, 60.f, -60.f, 150.f, -150.f, 250.f, -250.f };
	constexpr int k_spot_slot_base = 62, k_spot_slot_roll = 63;
	constexpr int k_viz_ticks = 16;

	enum e_wc_count {
		wc_land, wc_flip, wc_pin_catch, wc_hold, wc_park, wc_keys, wc_along, wc_own, wc_leave, wc_aa_leave, wc_exit, wc_exit_none, wc_far, wc_rise,
		wc_scan_cut, wc_cut, wc_div, wc_div_stomp, wc_stomp, wc_yield, wc_ramp, wc_skip, wc_jump_back, wc_jump_strip, wc_as_take, wc_pin_miss, wc_no_park,
		wc_park_keep, wc_away, wc_robust, wc_spot, wc_spot_sims, wc_hold_land, wc_low, wc_leave_hang, wc_ticks, wc_sims, wc_max
	};
	int s_count[ wc_max ]{ };
	long long s_us = 0ll, s_us_worst = 0ll;
	unsigned long long s_log_next = 0ull;

	struct sim_out_t {
		c_vector m_origin{ }, m_vel{ };
		int m_flags = 0;
	};

	using n_tb::move_fix_to_yaw;

	const std::vector< float >& fine_rungs( )
	{
		static const std::vector< float > v = [ ] {
			std::vector< float > r{ 0.f };
			for ( float m = k_fine_lo; m <= k_fine_hi; m *= k_fine_ratio )
				r.push_back( m );
			return r;
		}( );
		return v;
	}

	bool wish_leaves_wall( const c_user_cmd& c, const c_vector& n )
	{
		const float p = deg2rad( c.m_view_point.m_x ), y = deg2rad( c.m_view_point.m_y ), r = deg2rad( c.m_view_point.m_z );
		const float sp = std::sinf( p ), cp = std::cosf( p ), sy = std::sinf( y ), cy = std::cosf( y ), sr = std::sinf( r ), cr = std::cosf( r );
		float fx = cp * cy, fy = cp * sy, rx = -sr * sp * cy + cr * sy, ry = -sr * sp * sy - cr * cy;
		if ( const float l = std::sqrtf( fx * fx + fy * fy ); l > 0.f )
			fx /= l, fy /= l;
		if ( const float l = std::sqrtf( rx * rx + ry * ry ); l > 0.f )
			rx /= l, ry /= l;
		const float wx = fx * c.m_forward_move + rx * c.m_side_move;
		const float wy = fy * c.m_forward_move + ry * c.m_side_move;
		const float l  = std::sqrtf( wx * wx + wy * wy );
		return l > 0.f && ( wx * n.m_x + wy * n.m_y ) > k_leave_cos * l;
	}

	float face_d( const c_vector& o, const c_vector& n ) { return o.m_x * n.m_x + o.m_y * n.m_y + o.m_z * n.m_z; }

	void world_wish( const float yaw, const float f, const float s, float& x, float& y )
	{
		const float a = deg2rad( yaw );
		x = f * std::cosf( a ) + s * std::sinf( a );
		y = f * std::sinf( a ) - s * std::cosf( a );
	}
}

bool n_wall_climb::impl_t::caught( const c_user_cmd* cmd ) const
{
	return cmd && m_catch_cmd == cmd->m_command_number;
}

bool n_wall_climb::impl_t::active( const c_user_cmd* cmd ) const
{
	return cmd && m_active_cmd == cmd->m_command_number;
}

void n_wall_climb::impl_t::check_stomp( const c_user_cmd* cmd )
{
	if ( !cmd )
		return;
	constexpr int k_move_buttons = in_jump | in_duck;
	const bool stomped = cmd->m_forward_move != m_sent_fwd || cmd->m_side_move != m_sent_side ||
	                     ( cmd->m_buttons & k_move_buttons ) != ( m_sent_buttons & k_move_buttons );
	if ( m_pred_cmd == cmd->m_command_number )
		m_pred_stomped = stomped;
	if ( caught( cmd ) && stomped )
		botox_dbg_log( "WC STOMP: fwd %.4f->%.4f side %.4f->%.4f j/d %d%d->%d%d", m_sent_fwd, cmd->m_forward_move, m_sent_side, cmd->m_side_move,
		               ( m_sent_buttons & in_jump ) != 0, ( m_sent_buttons & in_duck ) != 0, ( cmd->m_buttons & in_jump ) != 0,
		               ( cmd->m_buttons & in_duck ) != 0 );
	trace_stomp( cmd, "end" );
}

// world wish, not raw fwd / side: silent view rotates the cmd every tick and keeps the wish
void n_wall_climb::impl_t::trace_stomp( const c_user_cmd* cmd, const char* stage )
{
	if ( !cmd || m_stomp_cmd == cmd->m_command_number || ( !active( cmd ) && !caught( cmd ) ) )
		return;
	constexpr int k_move_buttons = in_jump | in_duck;
	float sx, sy, nx, ny;
	world_wish( m_sent_yaw, m_sent_fwd, m_sent_side, sx, sy );
	world_wish( cmd->m_view_point.m_y, cmd->m_forward_move, cmd->m_side_move, nx, ny );
	if ( std::fabsf( nx - sx ) + std::fabsf( ny - sy ) <= 1e-3f && ( cmd->m_buttons & k_move_buttons ) == ( m_sent_buttons & k_move_buttons ) )
		return;
	m_stomp_cmd = cmd->m_command_number;
	++s_count[ wc_stomp ];
	botox_dbg_log( "WC STOMP@%s: wish %.3f,%.3f->%.3f,%.3f fwd %.3f->%.3f side %.3f->%.3f yaw %.2f->%.2f j/d %d%d->%d%d catch=%d", stage, sx, sy, nx, ny,
	               m_sent_fwd, cmd->m_forward_move, m_sent_side, cmd->m_side_move, m_sent_yaw, cmd->m_view_point.m_y, ( m_sent_buttons & in_jump ) != 0,
	               ( m_sent_buttons & in_duck ) != 0, ( cmd->m_buttons & in_jump ) != 0, ( cmd->m_buttons & in_duck ) != 0, caught( cmd ) ? 1 : 0 );
}

// ebanat 0x1054b3: on press, slope of the surface you look at
void n_wall_climb::impl_t::show_wall_slope( )
{
	const bool down = GET_VARIABLE( g_variables.m_show_wall_slope, bool ) &&
	                  g_input.check_input( &GET_VARIABLE( g_variables.m_show_wall_slope_key, key_bind_t ) );
	const bool press = down && !m_slope_down;
	m_slope_down     = down;
	c_base_entity* const local = g_ctx.m_local;
	if ( !press || !local || !local->is_alive( ) )
		return;
	c_angle view{ };
	g_interfaces.m_engine_client->get_view_angles( view );
	const float p = deg2rad( view.m_x ), y = deg2rad( view.m_y );
	const c_vector eye = local->get_eye_position( );
	const c_vector dir( std::cosf( p ) * std::cosf( y ), std::cosf( p ) * std::sinf( y ), -std::sinf( p ) );
	c_trace_filter filter( local );
	trace_t tr{ };
	ray_t ray( eye, eye + dir * 500.f );
	g_interfaces.m_engine_trace->trace_ray( ray, mask_playersolid, &filter, &tr );
	if ( tr.m_fraction >= 1.f || tr.m_start_solid )
		return;
	movement_add_window( 5, std::format( "wall slope: {:.7f}", rad2deg( std::acosf( std::min( 1.f, std::fabsf( tr.m_plane.m_normal.m_z ) ) ) ) ) );
}

void n_wall_climb::impl_t::on_create_move( c_user_cmd* cmd )
{
	show_wall_slope( );
	m_viz.valid = false;

	if ( const unsigned long long now = GetTickCount64( ); now >= s_log_next ) {
		s_log_next = now + 1000ull;
		if ( s_count[ wc_ticks ] || s_count[ wc_yield ] || s_count[ wc_ramp ] || s_count[ wc_skip ] )
			botox_dbg_log( "WC: land=%d flip=%d pinc=%d hold=%d park=%d keys=%d along=%d own=%d leave=%d aa=%d exit=%d/%d far=%d rise=%d scut=%d cut=%d div=%d/%d stomp=%d yield=%d ramp=%d skip=%d jump back=%d strip=%d as=%d miss=%d np=%d pk=%d away=%d robust=%d spot=%d/%d hland=%d low=%d lhang=%d ticks=%d sims=%d us=%lld worst=%lld",
			               s_count[ wc_land ], s_count[ wc_flip ], s_count[ wc_pin_catch ], s_count[ wc_hold ], s_count[ wc_park ], s_count[ wc_keys ],
			               s_count[ wc_along ], s_count[ wc_own ], s_count[ wc_leave ], s_count[ wc_aa_leave ], s_count[ wc_exit ], s_count[ wc_exit_none ], s_count[ wc_far ],
			               s_count[ wc_rise ], s_count[ wc_scan_cut ], s_count[ wc_cut ], s_count[ wc_div ], s_count[ wc_div_stomp ], s_count[ wc_stomp ], s_count[ wc_yield ], s_count[ wc_ramp ], s_count[ wc_skip ],
			               s_count[ wc_jump_back ], s_count[ wc_jump_strip ], s_count[ wc_as_take ], s_count[ wc_pin_miss ], s_count[ wc_no_park ], s_count[ wc_park_keep ], s_count[ wc_away ], s_count[ wc_robust ], s_count[ wc_spot ], s_count[ wc_spot_sims ], s_count[ wc_hold_land ], s_count[ wc_low ], s_count[ wc_leave_hang ], s_count[ wc_ticks ], s_count[ wc_sims ], s_us,
			               s_us_worst );
		for ( int& c : s_count )
			c = 0;
		s_us = s_us_worst = 0ll;
	}

	c_base_entity* const local = g_ctx.m_local;
	// online the client keeps its own START while the server END is < 0.0625u off ( cl_pred_optimize 2 ): flush parks / lip windows are
	// sub-ulp, so key held = full re-predict off every snapshot ( air stuck / tb do the same )
	g_prediction.sync_arm( n_prediction::impl_t::k_sync_wc, cmd && local && local->is_alive( ) && GET_VARIABLE( g_variables.m_wall_climb, bool ) &&
	                                                            g_input.check_input( &GET_VARIABLE( g_variables.m_wall_climb_key, key_bind_t ) ) );
	if ( !cmd || !local || !local->is_alive( ) || !GET_VARIABLE( g_variables.m_wall_climb, bool ) || local->get_move_type( ) != move_type_walk ) {
		m_holding = false;
		return;
	}
	m_park_off = m_park_off && !( local->get_flags( ) & fl_onground );
	if ( local->get_flags( ) & fl_onground )
		m_ground_z = local->get_abs_origin( ).m_z;
	if ( !g_input.check_input( &GET_VARIABLE( g_variables.m_wall_climb_key, key_bind_t ) ) ) {
		m_holding = false;
		return;
	}
	auto* const collideable = local->get_collideable( );
	if ( !collideable ) {
		m_holding = false;
		return;
	}

	const int frame = g_interfaces.m_prediction->m_commands_predicted - 1;
	g_prediction.restore_entity_to_predicted_frame( frame );
	const c_vector origin    = local->get_abs_origin( );
	const c_vector start_vel = local->get_velocity( );
	const bool on_ground     = ( local->get_flags( ) & fl_onground ) != 0;
	const float friction     = local->get_surface_friction( );
	const float stamina      = local->get_stamina( );
	const c_vector mins = collideable->get_obb_mins( ), maxs = collideable->get_obb_maxs( );
	if ( on_ground )
		m_ground_z = origin.m_z;
	// div: this START vs the END wc simmed for the cmd it sent last tick. stomped = a later feature rewrote that cmd
	if ( m_pred_cmd >= 0 && m_pred_cmd == cmd->m_command_number - 1 ) {
		const c_vector dp = origin - m_pred_org, dv = start_vel - m_pred_vel;
		if ( dp.length( ) > 1e-4f || dv.length( ) > 1e-3f ) {
			++s_count[ m_pred_stomped ? wc_div_stomp : wc_div ];
			static int s_div_logged = 0;
			if ( !m_pred_stomped && s_div_logged < 64 && ++s_div_logged )
				botox_dbg_log( "WC DIV: dorg=%.5f,%.5f,%.5f dvel=%.3f,%.3f,%.3f start g=%d vz=%.2f pred vz=%.2f", dp.m_x, dp.m_y, dp.m_z, dv.m_x, dv.m_y, dv.m_z,
				               on_ground, start_vel.m_z, m_pred_vel.m_z );
		}
	}
	m_pred_cmd = -1;
	const float pin         = g_prediction.get_engine_target_predict_z_velocity( );
	const auto pinned       = [ pin ]( const c_vector& v ) { return v.m_x == 0.f && v.m_y == 0.f && v.m_z == pin; };
	const bool start_pinned = !on_ground && pinned( start_vel );
	// server pinned what wc simmed free = flush park is a coin flip here ( 10-07 log: 0 / 41 pins simmed ), park off till ground
	if ( start_pinned && m_free_cmd == cmd->m_command_number - 1 ) {
		++s_count[ wc_pin_miss ];
		m_park_off = true;
	}
	m_free_cmd = -1;

	c_trace_filter filter( local );
	trace_t wall_tr{ };
	const auto find_wall = [ & ]( const float reach, const bool count ) {
		for ( int i = 0; i < 4; ++i ) {
			const float a = deg2rad( static_cast< float >( i * 90 ) );
			ray_t ray( origin, origin + c_vector( std::cosf( a ), std::sinf( a ), 0.f ) * reach, mins, maxs );
			g_interfaces.m_engine_trace->trace_ray( ray, mask_playersolid, &filter, &wall_tr );
			if ( !wall_tr.did_hit( ) )
				continue;
			if ( wall_tr.m_start_solid || ( wall_tr.m_hit_entity && wall_tr.m_hit_entity->is_player( ) ) ) {
				s_count[ wc_skip ] += count;
				continue;
			}
			if ( std::fabsf( wall_tr.m_plane.m_normal.m_z ) <= k_wall_nz )
				return true;
			s_count[ wc_ramp ] += count;
		}
		return false;
	};
	// pinned off the touch range ( an exit or your keys left a lip edge pin 0.18u out ): still exit it
	bool far_pin = false;
	if ( !find_wall( k_contact, true ) ) {
		m_holding = false;
		if ( !start_pinned || !find_wall( k_pin_reach, false ) )
			return;
		far_pin = true;
		++s_count[ wc_far ];
	}
	// no yield to a pixel surf latch: wc parks flush, so crease pins come often and a latched ride hung you 27-34 ticks
	// ( 10-06 22:20 log ). pixel_surf yields to active( ) instead, it runs after wc
	if ( g_air_stuck_holding || HITGODA || HITGODA2 || g_texturebug.m_hit || g_texturebug.m_hs_hit || g_edgebug.m_found ) {
		m_holding = false;
		++s_count[ wc_yield ];
		return;
	}
	// fireman has a ladder ( last tick's state, it runs after us ): a wc park on a ladder face latches it jumpless = 12 tick hang, a pin
	// eats the fall ( 10-07 21:07 log: every [fr] alien + 3 misses sat on WC park / PIN CATCH at nuke #3 )
	const auto& fr = g_movement.m_fireman_data;
	if ( GET_VARIABLE( g_variables.m_fire_man, bool ) && g_input.check_input( &GET_VARIABLE( g_variables.m_fire_man_key, key_bind_t ) ) &&
	     ( fr.is_ladder || fr.owns_cmd || fr.yaw_valid || fr.ladder_lock || fr.drop_in_lock || fr.launch_ticks > 0 ) ) {
		m_holding = false;
		++s_count[ wc_yield ];
		return;
	}
	if ( !on_ground )
		m_active_cmd = cmd->m_command_number;
	// air stuck on the same key publishes a wall hug every air tick by a wall, pin or not: only a catch takes the cmd from it
	const bool as_steers    = g_air_stuck_owns_cmd;
	const c_user_cmd as_cmd = *cmd;
	const c_vector wall_n   = wall_tr.m_plane.m_normal;
	const bool disp_wall    = wall_tr.m_disp_flags != 0;
	const float wall_yaw    = rad2deg( std::atan2f( -wall_n.m_y, -wall_n.m_x ) );

	static auto sv_airaccelerate = g_interfaces.m_convar->find_var( "sv_airaccelerate" );
	static auto sv_jump_impulse  = g_interfaces.m_convar->find_var( "sv_jump_impulse" );
	static auto sv_gravity       = g_interfaces.m_convar->find_var( "sv_gravity" );
	const float airaccel = sv_airaccelerate ? sv_airaccelerate->get_float( ) : 12.f;
	const float ipt      = g_interfaces.m_global_vars_base->m_interval_per_tick;

	const c_user_cmd keys = *cmd;
	const bool user_duck  = ( keys.m_buttons & in_duck ) != 0;
	// leave = your raw keys: auto align's fix_air_stucks push off the wall lost the park / wall ( 10-07 replay 5 -> 9 / 21 )
	c_user_cmd you     = keys;
	you.m_forward_move  = g_movement.m_user_forward_move_raw;
	you.m_side_move     = g_movement.m_user_side_move_raw;
	const bool you_leave = wish_leaves_wall( you, wall_n );

	n_tick::c_sim_budget budget{ };
	budget.start( k_time_share, n_tick::engine_interval( ), n_tick::search_wc );
	int sims = 0, commit_end = 0;
	bool cut = false, was_cut = false, free_sent = false;
	const char* branch = "-";
	const auto read_out = [ & ]( sim_out_t& out ) {
		out.m_origin = local->get_abs_origin( );
		out.m_vel    = local->get_velocity( );
		out.m_flags  = local->get_flags( );
	};
	// engine order like tb pred_simulate: a tick's leaf list bounds come from the previous move's max speed ( m_real_max_speed after
	// a restore ), full think. lean + 0 bounds sims landed lips the engine then missed ( 10-06 22:50 log: 11/37 catches, 8 tick hover )
	float bounds = g_prediction.m_real_max_speed;
	const auto restore = [ & ]( ) {
		g_prediction.restore_entity_to_predicted_frame( frame );
		bounds = g_prediction.m_real_max_speed;
	};
	const auto step = [ & ]( c_user_cmd c ) {
		g_prediction.m_bounds_max_speed = bounds;
		g_prediction.begin( local, &c );
		g_prediction.end( local );
		bounds                          = g_prediction.m_last_max_speed;
		g_prediction.m_bounds_max_speed = 0.f;
		n_tb::s_pred_dirty              = true;
	};
	// chain = next tick from wherever the last sim left the entity ( air_stuck pin count does the same )
	const auto chain = [ & ]( c_user_cmd c, sim_out_t& out ) {
		if ( sims >= commit_end && ( cut || ( sims > 0 && budget.expired( ) ) ) ) {
			cut = was_cut = true;
			return false;
		}
		step( c );
		read_out( out );
		++sims;
		return true;
	};
	const auto sim = [ & ]( const c_user_cmd& c, sim_out_t& out ) {
		if ( sims >= commit_end && ( cut || ( sims > 0 && budget.expired( ) ) ) ) {
			cut = was_cut = true;
			return false;
		}
		restore( );
		return chain( c, out );
	};
	const auto slowed = [ & ]( const c_vector& v ) {
		if ( !GET_VARIABLE( g_variables.m_wall_climb_prevent_slow, bool ) )
			return false;
		const float old_xy = start_vel.length_2d( );
		return old_xy > k_slow_xy && v.length_2d( ) / old_xy < k_slow_ratio;
	};
	const auto with = [ & ]( const float yaw, const float fwd, const bool duck ) {
		c_user_cmd c     = keys;
		c.m_buttons      = duck ? ( c.m_buttons | in_duck ) : ( c.m_buttons & ~in_duck );
		c.m_forward_move = fwd;
		c.m_side_move    = 0.f;
		move_fix_to_yaw( yaw, c );
		return c;
	};
	const auto send = [ & ]( const c_user_cmd& c ) {
		cmd->m_forward_move = c.m_forward_move;
		cmd->m_side_move    = c.m_side_move;
		cmd->m_buttons      = c.m_buttons;
	};
	// entity sits on the state to hold from
	const auto hold_lands = [ & ]( const c_user_cmd& hold ) {
		for ( int k = 0; k < k_hold_ticks; ++k ) {
			sim_out_t q{ };
			if ( !chain( hold, q ) )
				return false;
			if ( q.m_flags & fl_onground )
				return q.m_origin.m_z > m_ground_z + k_land_above;
		}
		return false;
	};
	// first try that stays unpinned for k_exit_look ticks of your keys ( a one tick unpin that re-pins = staircase ), else first unpin
	const auto exit_pin = [ & ]( ) {
		const float r  = deg2rad( wall_yaw + 90.f );
		const float sg = start_vel.length_2d( ) >= k_pin_sign_xy && std::cosf( r ) * start_vel.m_x + std::sinf( r ) * start_vel.m_y < 0.f ? -1.f : 1.f;
		const c_user_cmd tries[ ] = { keys, with( wall_yaw + sg * 90.f, 30.f, user_duck ), with( wall_yaw - sg * 90.f, 30.f, user_duck ),
			                          with( wall_yaw, -5.f, user_duck ), with( wall_yaw, -450.f, user_duck ) };
		int pick = -1, first = -1;
		for ( int i = 0; i < 5 && pick < 0; ++i ) {
			sim_out_t o{ };
			if ( !sim( tries[ i ], o ) )
				break;
			if ( pinned( o.m_vel ) || slowed( o.m_vel ) )
				continue;
			if ( first < 0 )
				first = i;
			bool clean = true;
			for ( int k = 0; k < k_exit_look && clean && !( o.m_flags & fl_onground ); ++k ) {
				if ( !chain( keys, o ) )
					break;
				clean = !pinned( o.m_vel );
			}
			if ( clean )
				pick = i;
		}
		if ( pick < 0 )
			pick = first;
		if ( pick < 0 ) {
			++s_count[ wc_exit_none ];
			return;
		}
		send( tries[ pick ] );
		free_sent = true;
		++s_count[ wc_exit ];
	};

	// spot ( ground, no jump ): the catch depends on where along the wall you jump ( lab: same wall 5% vs 85% a few u apart ). replay one
	// hop per spot ( 0, +-2, +-4 .. reach ) with your keys + a lite wc ( 0 + 5e-4 .. 0.032 F x2 both stances, land / tightest park ), sliced
	// over ground ticks in snapshot slots 62 / 63, then walk you there along the wall. lab ( fl_lab P64 as predictor ): reach 16 first
	// catches +20%, 32 +34%. your own along wish = no spot ( you walk on purpose )
	const auto spot = [ & ]( ) {
		auto& j          = m_spot;
		const float reach = static_cast< float >( std::clamp( GET_VARIABLE( g_variables.m_wall_climb_spot_reach, int ), 0, 32 ) );
		const c_vector t( wall_n.m_y, -wall_n.m_x, 0.f );
		float rx, ry;
		world_wish( keys.m_view_point.m_y, g_movement.m_user_forward_move_raw, g_movement.m_user_side_move_raw, rx, ry );
		const float rl = std::sqrtf( rx * rx + ry * ry );
		if ( reach < k_spot_step || user_duck || g_prediction.snapshot_slots( ) <= k_spot_slot_roll ||
		     ( rl > 0.f && std::fabsf( rx * t.m_x + ry * t.m_y ) > k_leave_cos * rl ) ) {
			j.valid = false;
			return false;
		}
		const c_vector off = origin - j.base;
		if ( !j.valid || j.reach != reach || j.n.m_x * wall_n.m_x + j.n.m_y * wall_n.m_y < 0.999f || std::fabsf( off.m_z ) > 1.f ||
		     std::fabsf( off.m_x * wall_n.m_x + off.m_y * wall_n.m_y ) > 1.f || std::fabsf( off.m_x * t.m_x + off.m_y * t.m_y ) > reach + k_spot_step ) {
			restore( );
			j = { };
			if ( !g_prediction.snapshot_save( k_spot_slot_base ) )
				return false;
			j.valid = true, j.base = origin, j.n = wall_n, j.reach = reach;
			j.count = std::min( 33, 1 + 2 * static_cast< int >( reach / k_spot_step ) );
		}
		const float F        = k_hull_step / ( ipt * airaccel * ipt * friction );
		const float floor_z  = j.base.m_z;
		const auto at_idx    = [ & ]( const int i ) { return ( i & 1 ? 1.f : -1.f ) * k_spot_step * static_cast< float >( ( i + 1 ) / 2 ); };
		const auto load      = [ & ]( const int slot ) {
			g_prediction.snapshot_load( slot );
			bounds = g_prediction.m_real_max_speed;
		};
		const auto finish = [ & ]( const int score ) {
			j.score[ j.idx ] = score;
			j.rolling        = false;
			if ( ++j.idx >= j.count ) {
				j.done = true;
				int best = 0;
				for ( int i = 1; i < j.count; ++i )
					best = j.score[ i ] > j.score[ best ] ? i : best;
				botox_dbg_log( "WC SPOT: n=%d best=%+.0f score=%d here=%d sims=%d slices=%d z=%.2f wall=%.2f,%.2f", j.count, at_idx( best ), j.score[ best ],
				               j.score[ 0 ], j.sims, j.slices, floor_z, wall_n.m_x, wall_n.m_y );
			}
		};
		++j.slices;
		while ( !j.done && !budget.cannot_fit( ( sims ? budget.used_us( ) / sims : 40ll ) * k_spot_tick_sims ) ) {
			if ( !j.rolling ) {
				load( k_spot_slot_base );
				const c_vector o = j.base + t * at_idx( j.idx );
				trace_t tr{ };
				ray_t path( j.base, o, mins, maxs );
				g_interfaces.m_engine_trace->trace_ray( path, mask_playersolid, &filter, &tr );
				trace_t gr{ };
				ray_t down( o, o - c_vector( 0.f, 0.f, 2.f ), mins, maxs );
				g_interfaces.m_engine_trace->trace_ray( down, mask_playersolid, &filter, &gr );
				if ( tr.m_start_solid || tr.m_fraction < 1.f || gr.m_start_solid || gr.m_fraction >= 1.f || gr.m_plane.m_normal.m_z < 0.7f ) {
					finish( -1 );
					continue;
				}
				local->set_abs_origin( o );
				local->get_origin( ) = o;
				c_user_cmd jc = keys;
				jc.m_buttons |= in_jump;
				step( jc );
				++sims, ++j.sims;
				if ( ( local->get_flags( ) & fl_onground ) || !g_prediction.snapshot_save( k_spot_slot_roll ) ) {
					finish( 0 );
					continue;
				}
				j.rolling = true, j.ticks = 0;
				continue;
			}
			int score       = -2;
			float best_d    = FLT_MAX;
			c_user_cmd pick = keys;
			for ( float m = 0.f; m <= 0.032f && score < 0; m = m > 0.f ? m * 2.f : 5e-4f )
				for ( int pass = 0; pass < 2 && score < 0; ++pass ) {
					if ( pass && !GET_VARIABLE( g_variables.m_wall_climb_duck, bool ) )
						continue;
					const bool duck    = pass ? !user_duck : user_duck;
					const c_user_cmd c = with( wall_yaw, m * F / ( duck ? 0.34f : 1.f ), duck );
					load( k_spot_slot_roll );
					step( c );
					++sims, ++j.sims;
					sim_out_t o{ };
					read_out( o );
					if ( o.m_flags & fl_onground ) {
						if ( o.m_origin.m_z > floor_z + k_spot_rise )
							score = 1000 + static_cast< int >( o.m_origin.m_z - floor_z );
					} else if ( !pass && !pinned( o.m_vel ) && face_d( o.m_origin, wall_n ) < best_d )
						best_d = face_d( o.m_origin, wall_n ), pick = c;
				}
			if ( score < 0 ) {
				load( k_spot_slot_roll );
				step( pick );
				++sims, ++j.sims;
				sim_out_t o{ };
				read_out( o );
				if ( o.m_flags & fl_onground )
					score = o.m_origin.m_z > floor_z + k_spot_rise ? 1000 + static_cast< int >( o.m_origin.m_z - floor_z ) : 0;
				else if ( pinned( o.m_vel ) || ++j.ticks >= k_spot_ticks || !g_prediction.snapshot_save( k_spot_slot_roll ) )
					score = 0; /* a pin here = a sticky spot */
			}
			if ( score >= 0 )
				finish( score );
		}
		s_count[ wc_spot_sims ] += j.sims;
		// walk to the best spot found so far ( ties: nearest to where you are )
		const c_vector rel = origin - j.base;
		const float here   = rel.m_x * t.m_x + rel.m_y * t.m_y;
		cut                = false;
		commit_end         = sims + static_cast< int >( std::size( k_spot_along ) );
		int best = -1;
		for ( int i = 0; i < j.idx; ++i )
			if ( j.score[ i ] > 0 && ( best < 0 || j.score[ i ] > j.score[ best ] ||
			                           ( j.score[ i ] == j.score[ best ] && std::fabsf( at_idx( i ) - here ) < std::fabsf( at_idx( best ) - here ) ) ) )
				best = i;
		if ( best < 0 || std::fabsf( at_idx( best ) - here ) <= k_spot_snap )
			return false;
		const float target = at_idx( best );
		float wx, wy;
		world_wish( keys.m_view_point.m_y, keys.m_forward_move, keys.m_side_move, wx, wy );
		const float in_amt = std::max( 0.f, -( wx * wall_n.m_x + wy * wall_n.m_y ) );
		const float yr = deg2rad( keys.m_view_point.m_y ), cy = std::cosf( yr ), sy = std::sinf( yr );
		float best_cost = FLT_MAX;
		c_user_cmd walk = keys;
		for ( const float a : k_spot_along ) {
			const float x = -wall_n.m_x * in_amt + t.m_x * a, y = -wall_n.m_y * in_amt + t.m_y * a;
			c_user_cmd c     = keys;
			c.m_forward_move = x * cy + y * sy;
			c.m_side_move    = x * sy - y * cy;
			sim_out_t o{ };
			if ( !sim( c, o ) )
				break;
			const c_vector d  = o.m_origin - j.base;
			const float cost = std::fabsf( d.m_x * t.m_x + d.m_y * t.m_y + ( o.m_vel.m_x * t.m_x + o.m_vel.m_y * t.m_y ) * ipt * k_spot_look - target );
			if ( cost < best_cost )
				best_cost = cost, walk = c;
		}
		if ( best_cost == FLT_MAX )
			return false;
		send( walk );
		++s_count[ wc_spot ];
		branch = "spot";
		return true;
	};

	bool landed = false, held = false, jump_fix = false;
	if ( !on_ground ) {
		m_spot.valid = false;
		const bool was_holding = m_holding;
		m_holding              = false;
		s_count[ wc_leave_hang ] += ( start_pinned || was_holding ) && you_leave;
		if ( ( start_pinned || was_holding ) && !you_leave ) {
			restore( );
			const c_user_cmd hold = with( wall_yaw, 0.f, was_holding ? ( m_hold_buttons & in_duck ) != 0 : user_duck );
			if ( hold_lands( hold ) ) {
				// lab P67: a land now beats one more hang tick ( hang -14%, catches same )
				const float F  = k_hull_step / ( ipt * airaccel * ipt * friction );
				const bool hd  = ( hold.m_buttons & in_duck ) != 0;
				const auto& fine = fine_rungs( );
				for ( size_t i = 1; i < fine.size( ) && !held; ++i ) {
					const c_user_cmd c = with( wall_yaw, fine[ i ] * F / ( hd ? 0.34f : 1.f ), hd );
					sim_out_t o{ };
					if ( !sim( c, o ) )
						break;
					if ( ( o.m_flags & fl_onground ) && o.m_origin.m_z > m_ground_z + k_land_above ) {
						send( c );
						landed = held = true;
						++s_count[ wc_hold_land ];
						branch = "hold_land";
					}
				}
				if ( !held ) {
					send( hold );
					held = m_holding = true;
					m_hold_buttons    = hold.m_buttons;
					++s_count[ wc_hold ];
					branch = "hold";
				}
			}
		}
		if ( !held && start_pinned ) {
			exit_pin( );
			branch = "exit";
		} else if ( !held && !far_pin ) {
			const float F  = k_hull_step / ( ipt * airaccel * ipt * friction );
			// crouch catches off = no crouch pops in demos: other stance never sent ( lab P61 vs P57: rows -30% )
			bool live[ 2 ] = { true, GET_VARIABLE( g_variables.m_wall_climb_duck, bool ) };
			float best_d   = FLT_MAX, best_xy = 0.f;
			c_user_cmd best_c{ };
			bool have_park = false;
			int pin_tries  = 0;
			sim_out_t last{ };
			struct park_t {
				float d, xy;
				c_user_cmd c;
			};
			std::vector< park_t > parks;
			// 1 caught, 0 go on, -1 out of clock
			const auto test = [ & ]( const c_user_cmd& c, const bool flip, const bool park ) {
				if ( !sim( c, last ) )
					return -1;
				if ( last.m_flags & fl_onground ) {
					if ( last.m_origin.m_z < m_ground_z - k_low_ledge ) {
						++s_count[ wc_low ];
						branch = "low";
						return 1;
					}
					sim_out_t own{ };
					if ( sim( keys, own ) && ( own.m_flags & fl_onground ) ) {
						++s_count[ wc_own ];
						branch = "own";
						return 1;
					}
					send( c );
					landed = true;
					++s_count[ wc_land ];
					s_count[ wc_flip ] += flip;
					branch = flip ? "land_flip" : "land";
					botox_dbg_log( "WC LAND: duck=%d fwd=%.5f side=%.5f z=%.3f vz=%.2f n=%.3f,%.3f,%.3f disp=%d", ( c.m_buttons & in_duck ) != 0, c.m_forward_move,
					               c.m_side_move, origin.m_z, start_vel.m_z, wall_n.m_x, wall_n.m_y, wall_n.m_z, disp_wall ? 1 : 0 );
					return 1;
				}
				if ( pinned( last.m_vel ) ) {
					if ( pin_tries < k_pin_tries && !you_leave ) {
						++pin_tries;
						if ( hold_lands( with( wall_yaw, 0.f, ( c.m_buttons & in_duck ) != 0 ) ) ) {
							send( c );
							landed = held = m_holding = true;
							m_hold_buttons            = c.m_buttons;
							++s_count[ wc_pin_catch ];
							branch = "pin_catch";
							botox_dbg_log( "WC PIN CATCH: duck=%d fwd=%.5f z=%.3f vz=%.2f n=%.3f,%.3f disp=%d", ( c.m_buttons & in_duck ) != 0, c.m_forward_move, origin.m_z,
							               start_vel.m_z, wall_n.m_x, wall_n.m_y, disp_wall ? 1 : 0 );
							return 1;
						}
					}
					return 0;
				}
				if ( park && !flip && !slowed( last.m_vel ) ) {
					const float d = face_d( last.m_origin, wall_n );
					parks.push_back( { d, last.m_vel.length_2d( ), c } );
					if ( d < best_d )
						best_d = d, best_c = c, best_xy = last.m_vel.length_2d( ), have_park = true;
				}
				return 0;
			};
			const auto scan_cut = [ & ]( ) {
				if ( !sims || !budget.cannot_fit( budget.used_us( ) * k_reserve_sims / sims ) )
					return false;
				++s_count[ wc_scan_cut ];
				return true;
			};
			const auto& fine = fine_rungs( );
			int r = 0;
			for ( size_t i = 0; i < fine.size( ) && !r; ++i )
				for ( int pass = 0; pass < 2 && !r; ++pass ) {
					if ( !live[ pass ] )
						continue;
					if ( scan_cut( ) ) {
						r = -1;
						break;
					}
					const bool duck = pass ? !user_duck : user_duck;
					r               = test( with( wall_yaw, fine[ i ] * F / ( duck ? 0.34f : 1.f ), duck ), pass != 0, true );
					if ( !r && !i && pass && last.m_vel.m_z > k_rise_vz ) {
						live[ 1 ] = false; /* flip stance never grounds rising this fast; your stance still parks */
						++s_count[ wc_rise ];
					}
				}
			// park sits at contact, a lip lands 1-2 floats out ( 10-08 lab P54 ): away rungs land / pin catch only, never park
			for ( size_t i = 1; i < fine.size( ) && !r; ++i ) {
				if ( fine[ i ] < k_away_lo )
					continue;
				if ( scan_cut( ) ) {
					r = -1;
					break;
				}
				r = test( with( wall_yaw, -fine[ i ] * F / ( user_duck ? 0.34f : 1.f ), user_duck ), false, false );
				s_count[ wc_away ] += r > 0 && ( landed || held );
			}
			for ( const float m : k_coarse ) {
				if ( r || scan_cut( ) ) {
					r = r ? r : -1;
					break;
				}
				r = test( with( wall_yaw, m * F / ( user_duck ? 0.34f : 1.f ), user_duck ), false, true );
			}
			const float vel_yaw = rad2deg( std::atan2f( start_vel.m_y, start_vel.m_x ) );
			const float side    = std::remainderf( vel_yaw - wall_yaw, 360.f ) < 0.f ? 1.f : -1.f;
			for ( const float f : k_along_fwd )
				for ( const float a : k_along_off )
					for ( int pass = 0; pass < 2 && !r; ++pass ) {
						if ( !live[ pass ] )
							continue;
						if ( scan_cut( ) ) {
							r = -1;
							break;
						}
						r = test( with( wall_yaw + side * a, f, pass ? !user_duck : user_duck ), pass != 0, false );
						s_count[ wc_along ] += r > 0;
					}
			if ( r <= 0 ) {
				// a clock cut mid ladder must still compare your keys and exit a pin ( lab: 3 sims past the cap )
				cut        = false;
				commit_end = sims + k_commit_sims + 3 * k_robust_parks;
				// 10-31 09:57 log: 21 parks the server pinned that wc simmed free ( 14 rising = jump killed ). START drifts z 1 ulp + 1e-3 u/s a
				// tick ( lab FL_NOISE 1 ): the park must stay free from START nudged into the face, lab P62 pinned ticks -53%, catches same
				if ( have_park ) {
					std::sort( parks.begin( ), parks.end( ), [ ]( const park_t& a, const park_t& b ) { return a.d < b.d; } );
					have_park = false;
					for ( int p = 0; p < static_cast< int >( parks.size( ) ) && p < k_robust_parks && !have_park; ++p ) {
						bool ok = true;
						for ( int v = 0; v < 3 && ok; ++v ) {
							c_vector o = origin, vel = start_vel;
							if ( v < 2 ) {
								for ( int u = 0; u <= v; ++u ) {
									if ( std::fabsf( wall_n.m_x ) > 0.05f )
										o.m_x = std::nextafter( o.m_x, wall_n.m_x > 0.f ? -FLT_MAX : FLT_MAX );
									if ( std::fabsf( wall_n.m_y ) > 0.05f )
										o.m_y = std::nextafter( o.m_y, wall_n.m_y > 0.f ? -FLT_MAX : FLT_MAX );
								}
							} else {
								vel.m_x -= wall_n.m_x * k_robust_vel;
								vel.m_y -= wall_n.m_y * k_robust_vel;
							}
							restore( );
							local->set_abs_origin( o );
							local->get_origin( )   = o;
							local->get_velocity( ) = vel;
							sim_out_t q{ };
							ok = chain( parks[ p ].c, q ) && !pinned( q.m_vel );
						}
						if ( ok )
							best_d = parks[ p ].d, best_c = parks[ p ].c, best_xy = parks[ p ].xy, have_park = true;
					}
					s_count[ wc_robust ] += !have_park;
				}
				sim_out_t k{ };
				const bool kok = sim( keys, k );
				const bool kpin = kok && !( k.m_flags & fl_onground ) && pinned( k.m_vel );
				s_count[ wc_aa_leave ] += !you_leave && wish_leaves_wall( keys, wall_n );
				if ( you_leave ) {
					++s_count[ wc_leave ];
					branch = "leave";
					if ( kpin )
						exit_pin( );
				} else if ( have_park && ( kpin || !m_park_off ) ) {
					if ( kok && !kpin && !( k.m_flags & fl_onground ) && face_d( k.m_origin, wall_n ) <= best_d + k_keys_ulp ) {
						++s_count[ wc_keys ];
						branch = "keys";
					} else if ( kok && !kpin && !( k.m_flags & fl_onground ) && best_xy < k.m_vel.length_2d( ) - k_park_keep && !wish_leaves_wall( keys, wall_n ) ) {
						// park sends ~0 wish: froze w+strafe gain ( 10-08 log u 37 s 32 x10 ) and sat in corners at xy 0. not auto align's push off
						// ( 10-08 20:40 log: 18 / 40 sent it, wall lost 2-3 ticks )
						++s_count[ wc_park_keep ];
						branch = "keys_pk";
					} else {
						send( best_c );
						++s_count[ wc_park ];
						branch = "park";
					}
					free_sent = true;
				} else if ( kpin ) {
					exit_pin( );
					branch = "exit";
				} else if ( have_park ) {
					++s_count[ wc_no_park ];
					branch    = "keys_np";
					free_sent = kok;
				} else if ( !parks.empty( ) )
					branch = "keys_rob";
			}
		}
	} else {
		m_holding = false;
		branch    = "ground";
		sim_out_t out{ };
		const bool spotted = !( cmd->m_buttons & in_jump ) && spot( );
		if ( !spotted && sim( *cmd, out ) ) {
			if ( pinned( out.m_vel ) || slowed( out.m_vel ) )
				exit_pin( );
			else if ( cmd->m_buttons & in_jump ) {
				trace_t tr{ };
				const c_vector from = origin + wall_n * k_seam_off;
				ray_t ray( from, from - c_vector( 0.f, 0.f, k_seam_below ), c_vector( -16.f, -16.f, 0.f ), c_vector( 16.f, 16.f, 72.f ) );
				g_interfaces.m_engine_trace->trace_ray( ray, mask_playersolid, &filter, &tr );
				if ( tr.m_fraction >= 1.f || from.m_z - tr.m_end.m_z > k_seam_floor ) {
					const float impulse = sv_jump_impulse ? sv_jump_impulse->get_float( ) : 301.993377f;
					const float gravity = sv_gravity ? sv_gravity->get_float( ) : 800.f;
					const float target  = k_jump_share * ( impulse * std::clamp( 1.f - stamina / 100.f, 0.f, 1.f ) - gravity * ipt );
					if ( out.m_vel.m_z < target ) {
						bool reached = false;
						for ( int i = 0; i <= k_back_steps && !reached; ++i ) {
							const c_user_cmd c = with( wall_yaw, -( static_cast< float >( i ) / static_cast< float >( k_back_steps ) ), user_duck );
							sim_out_t back{ };
							if ( !sim( c, back ) )
								break;
							if ( !( back.m_vel.m_z < target ) ) {
								cmd->m_forward_move = c.m_forward_move;
								cmd->m_side_move    = c.m_side_move;
								reached = jump_fix = true;
								++s_count[ wc_jump_back ];
								branch = "jump_back";
							}
						}
						if ( !reached && !cut ) {
							cmd->m_buttons &= ~in_jump;
							jump_fix = true;
							++s_count[ wc_jump_strip ];
							branch = "jump_strip";
							botox_dbg_log( "WC JUMP STRIP: vz=%.2f target=%.2f stam=%.1f z=%.3f", out.m_vel.m_z, target, stamina, origin.m_z );
						}
					}
				}
			}
		}
	}

	// visualize: tick 1 = what goes out ( air stuck's cmd when it keeps the steering ), then wc's hold on a catch ( what every catch is
	// proven with ), else that cmd again. extra sims only with the option on, off the clock
	const long long us = budget.used_us( );
	if ( GET_VARIABLE( g_variables.m_wall_climb_visualize, bool ) ) {
		m_viz        = { };
		m_viz.valid  = true;
		m_viz.ground = on_ground;
		m_viz.branch = branch;
		m_viz.n      = wall_n;
		m_viz.sims   = sims;
		m_viz.half   = maxs.m_x;
		m_viz.height = maxs.m_z - mins.m_z;
		const c_vector mid = wall_tr.m_end + c_vector( 0.f, 0.f, ( mins.m_z + maxs.m_z ) * 0.5f );
		const float off    = face_d( mid, wall_n ) - wall_tr.m_plane.m_distance;
		m_viz.wall = std::fabsf( off ) <= 32.f ? mid - wall_n * off : mid - wall_n * ( maxs.m_x * ( std::fabsf( wall_n.m_x ) + std::fabsf( wall_n.m_y ) ) );
		if ( !on_ground ) {
			const bool catching  = landed || held;
			const c_user_cmd out = as_steers && !catching && !jump_fix ? as_cmd : *cmd;
			const c_user_cmd next = catching ? with( wall_yaw, 0.f, ( out.m_buttons & in_duck ) != 0 ) : out;
			restore( );
			m_viz.path[ m_viz.path_n++ ] = origin;
			for ( int k = 0; k < k_viz_ticks; ++k ) {
				step( k ? next : out );
				sim_out_t o{ };
				read_out( o );
				m_viz.path[ m_viz.path_n++ ] = o.m_origin;
				if ( o.m_flags & fl_onground ) {
					m_viz.lands = o.m_origin.m_z > m_ground_z + k_land_above;
					m_viz.land  = o.m_origin;
					break;
				}
			}
		}
	}

	// wc t: every wall tick. u = end of your cmd, s = end of what wall climb sent ( s.xy < u.xy = it cost speed ). org / vel / yaw
	// = START + your keys, enough to replay the tick in fl_lab. extra sims, debug log only, not in us=
	if ( GET_VARIABLE( g_variables.m_debug_log, bool ) ) {
		const auto raw = [ & ]( c_user_cmd c, sim_out_t& out ) {
			restore( );
			step( c );
			read_out( out );
		};
		sim_out_t u{ }, s{ };
		raw( keys, u );
		if ( cmd->m_forward_move != keys.m_forward_move || cmd->m_side_move != keys.m_side_move || cmd->m_buttons != keys.m_buttons )
			raw( *cmd, s );
		else
			s = u;
		botox_dbg_log( "WC T: %s g=%d n=%.2f,%.2f disp=%d xy=%.1f u=%.1f/%.1f/%d s=%.1f/%.1f/%d fs=%.1f,%.1f->%.3f,%.3f j=%d/%d d=%d/%d sims=%d%s org=%.4f,%.4f,%.4f vel=%.3f,%.3f,%.3f yaw=%.3f gz=%.3f",
		               branch, on_ground, wall_n.m_x, wall_n.m_y, disp_wall ? 1 : 0, start_vel.length_2d( ), u.m_vel.length_2d( ), u.m_vel.m_z,
		               ( u.m_flags & fl_onground ) != 0, s.m_vel.length_2d( ), s.m_vel.m_z, ( s.m_flags & fl_onground ) != 0, keys.m_forward_move, keys.m_side_move,
		               cmd->m_forward_move, cmd->m_side_move, ( keys.m_buttons & in_jump ) != 0, ( cmd->m_buttons & in_jump ) != 0, user_duck,
		               ( cmd->m_buttons & in_duck ) != 0, sims, was_cut ? " CUT" : "", origin.m_x, origin.m_y, origin.m_z, start_vel.m_x, start_vel.m_y,
		               start_vel.m_z, keys.m_view_point.m_y, m_ground_z );
		if ( !as_steers ) {
			m_pred_cmd     = cmd->m_command_number;
			m_pred_org     = s.m_origin;
			m_pred_vel     = s.m_vel;
			m_pred_stomped = false;
		}
	}
	g_prediction.restore_entity_to_predicted_frame( frame );
	s_count[ wc_sims ] += sims;
	++s_count[ wc_ticks ];
	s_us += us;
	s_us_worst = std::max( s_us_worst, us );
	if ( was_cut )
		++s_count[ wc_cut ];
	const bool caught_now = landed || held || jump_fix;
	if ( as_steers ) {
		if ( caught_now ) {
			g_air_stuck_owns_cmd = g_air_stuck_authored_view_valid = g_air_stuck_stamp_valid = false;
			++s_count[ wc_as_take ];
		} else
			*cmd = as_cmd;
	}
	m_hit          = landed;
	m_free_cmd     = free_sent && ( !as_steers || caught_now ) ? cmd->m_command_number : -1;
	m_sent_fwd     = cmd->m_forward_move;
	m_sent_side    = cmd->m_side_move;
	m_sent_buttons = cmd->m_buttons;
	m_sent_yaw     = cmd->m_view_point.m_y;
	if ( caught_now )
		m_catch_cmd = cmd->m_command_number;
	if ( landed || jump_fix )
		m_act_tick = g_interfaces.m_global_vars_base->m_tick_count;
}

// face wc reads, where the sent cmd takes you ( green end = a land over your jump floor ), on ground the spot scan along the wall
void n_wall_climb::impl_t::render( )
{
	if ( !m_viz.valid || !GET_VARIABLE( g_variables.m_wall_climb_visualize, bool ) || !GET_VARIABLE( g_variables.m_wall_climb, bool ) ||
	     !g_input.check_input( &GET_VARIABLE( g_variables.m_wall_climb_key, key_bind_t ) ) || !g_ctx.m_local || !g_ctx.m_local->is_alive( ) ||
	     !g_interfaces.m_engine_client->is_in_game( ) )
		return;

	const c_color& col       = GET_VARIABLE( g_variables.m_wall_climb_visualize_color, c_color );
	const unsigned int color = col.get_u32( );
	const unsigned int green = c_color( 90, 255, 120, 255 ).get_u32( );
	const unsigned int red   = c_color( 255, 80, 80, 255 ).get_u32( );
	const unsigned int grey  = c_color( 160, 160, 160, 170 ).get_u32( );

	const auto line = [ & ]( const c_vector& a, const c_vector& b, const unsigned int c, const float w ) {
		c_vector_2d sa, sb;
		if ( g_render.world_to_screen( a, sa ) && g_render.world_to_screen( b, sb ) )
			g_render.m_draw_data.emplace_back( e_draw_type::draw_type_line, std::make_any< line_draw_object_t >( line_draw_object_t{ sa, sb, c, w } ) );
	};
	const auto dot = [ & ]( const c_vector& p, const float r, const unsigned int c ) {
		c_vector_2d s;
		if ( g_render.world_to_screen( p, s ) )
			g_render.m_draw_data.emplace_back( e_draw_type::draw_type_filled_circle,
			                                   std::make_any< filled_circle_draw_object_t >( filled_circle_draw_object_t( s, r, c, 12 ) ) );
	};
	const auto label = [ & ]( const c_vector& p, const std::string& str, const unsigned int c ) {
		auto* font = g_render.m_fonts[ e_font_names::font_name_verdana_11 ];
		c_vector_2d s;
		if ( !font || !g_render.world_to_screen( p, s ) )
			return;
		text_draw_object_t text;
		text.m_font       = font;
		text.m_position   = c_vector_2d( s.m_x - font->CalcTextSizeA( font->FontSize, FLT_MAX, 0.f, str.c_str( ) ).x * 0.5f, s.m_y - font->FontSize );
		text.m_text       = str;
		text.m_color      = c;
		text.m_draw_flags = text_flag_dropshadow;
		g_render.m_draw_data.emplace_back( e_draw_type::draw_type_text, std::make_any< text_draw_object_t >( text ) );
	};

	const c_vector& n = m_viz.n;
	const float nl    = std::sqrtf( n.m_x * n.m_x + n.m_y * n.m_y );
	const c_vector t  = nl > 1e-3f ? c_vector( n.m_y / nl, -n.m_x / nl, 0.f ) : c_vector( 1.f, 0.f, 0.f );
	const c_vector up( 0.f, 0.f, m_viz.height * 0.5f );
	const c_vector side = t * m_viz.half;
	const c_vector face[ 4 ] = { m_viz.wall - side - up, m_viz.wall + side - up, m_viz.wall + side + up, m_viz.wall - side + up };

	poly_draw_object_t poly{ };
	poly.m_color   = col.get_u32( 0.18f );
	bool on_screen = true;
	for ( int i = 0; i < 4 && on_screen; i++ )
		on_screen = g_render.world_to_screen( face[ i ], poly.m_points[ i ] );
	if ( on_screen ) {
		poly.m_count = 4;
		g_render.m_draw_data.emplace_back( e_draw_type::draw_type_poly, std::make_any< poly_draw_object_t >( poly ) );
	}
	for ( int i = 0; i < 4; i++ )
		line( face[ i ], face[ ( i + 1 ) % 4 ], color, 1.f );
	line( m_viz.wall, m_viz.wall + n * 8.f, color, 1.f );
	label( m_viz.wall + up + c_vector( 0.f, 0.f, 4.f ), std::format( "wc {} {}", m_viz.branch, m_viz.sims ), color );

	if ( !m_viz.ground ) {
		for ( int i = 0; i + 1 < m_viz.path_n; i++ )
			line( m_viz.path[ i ], m_viz.path[ i + 1 ], color, 1.5f );
		if ( m_viz.path_n > 1 )
			dot( m_viz.path[ m_viz.path_n - 1 ], m_viz.lands ? 5.f : 3.f, m_viz.lands ? green : color );
		if ( m_viz.lands )
			label( m_viz.land + c_vector( 0.f, 0.f, 8.f ), std::format( "+{:.0f}", m_viz.land.m_z - m_ground_z ), green );
		return;
	}

	// spot scan: green = climbed off that spot, red = nothing / sticky, grey = blocked or not run yet, big = where you get walked
	const auto& j = m_spot;
	if ( !j.valid )
		return;
	const c_vector st( j.n.m_y, -j.n.m_x, 0.f );
	const auto at_idx = [ ]( const int i ) { return ( i & 1 ? 1.f : -1.f ) * k_spot_step * static_cast< float >( ( i + 1 ) / 2 ); };
	int best = -1;
	for ( int i = 0; i < j.idx; ++i )
		if ( j.score[ i ] > 0 && ( best < 0 || j.score[ i ] > j.score[ best ] ) )
			best = i;
	for ( int i = 0; i < j.count; ++i ) {
		const c_vector p    = j.base + st * at_idx( i ) + c_vector( 0.f, 0.f, 1.f );
		const int s         = i < j.idx ? j.score[ i ] : -2;
		const unsigned int c = s > 0 ? green : s == 0 ? red : grey;
		dot( p, i == best ? 5.f : 2.5f, c );
	}
	if ( best >= 0 ) {
		const c_vector p = j.base + st * at_idx( best ) + c_vector( 0.f, 0.f, 1.f );
		line( g_ctx.m_local->get_abs_origin( ) + c_vector( 0.f, 0.f, 1.f ), p, green, 1.5f );
		label( p + c_vector( 0.f, 0.f, 10.f ), std::format( "+{:.0f}", static_cast< float >( j.score[ best ] - 1000 ) ), green );
	}
}
