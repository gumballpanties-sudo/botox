#include "wall_climb.h"
#include "movement_internal.h"
#include "texturebug.h"
#include "texturebug_common.h"
#include "edgebug.h"
#include <cfloat>
#include <format>

extern bool HITGODA;
extern bool HITGODA2;

// ebanat wallclimb FUN_100e8920 ( tools/decompiled/ebanat_all.c )
namespace
{
	constexpr float k_pi            = 3.14159265358979f;
	constexpr float k_scan_range    = k_pi * 2.f;
	constexpr float k_scan_step     = k_pi / 8.f;
	constexpr float k_wall_nz       = 0.05f;
	constexpr float k_wall_into     = -0.1f;
	constexpr float k_reach_pad     = 2.f;
	constexpr float k_feet_lo       = 1.f;
	constexpr float k_feet_hi       = 4.f;
	constexpr float k_gap_reach     = 32.f;
	constexpr float k_fwd[ ]        = { 0.001f, 0.01f, 0.05f, 0.25f, 1.f, 2.f, 5.f };
	constexpr float k_off[ ]        = { 8.f, 8.2f, 8.4f, 8.6f };
	constexpr float k_refine_off[ ] = { 0.f, -0.5f, 0.5f, -1.25f, 1.25f };
	constexpr float k_refine_mul[ ] = { 1.f, 0.6f, 1.6f };
	constexpr float k_fwd_lo        = 0.001f;
	constexpr float k_fwd_hi        = 30.f;
	constexpr float k_refine_yaw    = 5.f;
	constexpr float k_refine_side   = 0.5f;
	constexpr int k_refine_ticks    = 2;
	constexpr int k_no_tick         = -1000;
	constexpr int k_multi_ticks     = 2;
	constexpr float k_dead_xy       = 1.f;
	constexpr float k_bad_xy        = 5.f;
	constexpr float k_bad_gap       = -0.5f;
	constexpr float k_xy_tie        = 0.5f;
	constexpr float k_duck_gap      = 0.05f;
	constexpr float k_bail_start_xy = 10.f;
	constexpr float k_kill_start_xy = 40.f;
	constexpr float k_kill_xy       = 8.f;
	constexpr float k_kill_ratio    = 0.2f;
	constexpr float k_moving        = 0.01f;
	constexpr float k_fall_vz       = -1.f;
	constexpr float k_flip_gain     = 0.01f;
	constexpr float k_time_share    = 0.6f;
	constexpr int k_commit_sims     = 10;
	constexpr int k_arm_ticks       = 2;
	constexpr float k_nudge_xy      = 10.f;
	constexpr float k_nudge         = 5.f;
	constexpr float k_slope_reach   = 500.f;
	constexpr float k_div_vel       = 1.f;
	constexpr int k_div_lines       = 64;

	enum e_wc_count { wc_ticks, wc_sims, wc_refine, wc_full, wc_pin, wc_park, wc_arm, wc_hold, wc_bail, wc_kill, wc_keys, wc_flip, wc_duck, wc_cut, wc_yield, wc_div, wc_max };
	int s_count[ wc_max ]{ };
	long long s_us = 0ll, s_us_worst = 0ll;
	unsigned long long s_log_next = 0ull;

	struct cand_t {
		float fwd = 0.f, yaw = 0.f, gap = FLT_MAX, xy = 0.f;
		int pins = 0, buttons = 0, stand = 0, duck = 0;
		bool duck_more = false, valid = false;
	};

	float norm( const float a ) { return std::remainderf( a, 360.f ); }

	bool bad( const cand_t& c ) { return c.xy < k_bad_xy || c.gap < k_bad_gap; }

	bool good( const cand_t& c ) { return c.valid && c.pins > 0 && !bad( c ); }

	// FUN_100ebf60
	bool better( const cand_t& a, const cand_t& b )
	{
		if ( !b.valid || !a.valid )
			return a.valid;
		const bool ba = bad( a ), bb = bad( b );
		if ( ba != bb )
			return !ba;
		if ( ba )
			return a.xy > b.xy;
		if ( ( a.pins > 0 ) != ( b.pins > 0 ) )
			return a.pins > 0;
		if ( a.pins <= 0 )
			return a.gap < b.gap;
		if ( std::fabsf( a.xy - b.xy ) > k_xy_tie )
			return a.xy > b.xy;
		return a.fwd < b.fwd;
	}

	// ebanat std::clamp( x, 1.f, 0.f ): swapped bounds, refine yaw offset is only ever 0 or 1 deg
	float refine_off( const float x ) { return x <= 0.f ? 1.f : 0.f; }

	void set_move( c_user_cmd& c, const float yaw, const float fwd )
	{
		const float d    = deg2rad( norm( yaw - c.m_view_point.m_y ) );
		c.m_forward_move = std::cosf( d ) * fwd;
		c.m_side_move    = -std::sinf( d ) * fwd;
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
	if ( caught( cmd ) && ( cmd->m_forward_move != m_sent_fwd || cmd->m_side_move != m_sent_side ||
	                        ( cmd->m_buttons & k_move_buttons ) != ( m_sent_buttons & k_move_buttons ) ) )
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
	const auto world = [ ]( const float yaw, const float f, const float s, float& x, float& y ) {
		const float a = deg2rad( yaw );
		x             = f * std::cosf( a ) + s * std::sinf( a );
		y             = f * std::sinf( a ) - s * std::cosf( a );
	};
	float sx, sy, nx, ny;
	world( m_sent_yaw, m_sent_fwd, m_sent_side, sx, sy );
	world( cmd->m_view_point.m_y, cmd->m_forward_move, cmd->m_side_move, nx, ny );
	if ( std::fabsf( nx - sx ) + std::fabsf( ny - sy ) <= 1e-3f && ( cmd->m_buttons & k_move_buttons ) == ( m_sent_buttons & k_move_buttons ) )
		return;
	m_stomp_cmd = cmd->m_command_number;
	botox_dbg_log( "WC STOMP@%s: wish %.3f,%.3f->%.3f,%.3f j/d %d%d->%d%d catch=%d", stage, sx, sy, nx, ny, ( m_sent_buttons & in_jump ) != 0,
	               ( m_sent_buttons & in_duck ) != 0, ( cmd->m_buttons & in_jump ) != 0, ( cmd->m_buttons & in_duck ) != 0, caught( cmd ) ? 1 : 0 );
}

void n_wall_climb::impl_t::reset_latch( )
{
	m_latch_arm = m_latch_hold = false;
	m_latch_wait                = 0;
	m_latch_yaw = m_latch_fwd = 0.f;
	m_latch_stand = m_latch_duck = 0;
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
	ray_t ray( eye, eye + dir * k_slope_reach );
	g_interfaces.m_engine_trace->trace_ray( ray, mask_playersolid, &filter, &tr );
	if ( tr.m_fraction >= 1.f || tr.m_start_solid )
		return;
	movement_add_window( 5, std::format( "wall slope: {:.7f}", rad2deg( std::acosf( std::min( 1.f, std::fabsf( tr.m_plane.m_normal.m_z ) ) ) ) ) );
}

void n_wall_climb::impl_t::on_create_move( c_user_cmd* cmd )
{
	show_wall_slope( );

	if ( const unsigned long long now = GetTickCount64( ); now >= s_log_next ) {
		s_log_next = now + 1000ull;
		if ( s_count[ wc_ticks ] || s_count[ wc_yield ] )
			botox_dbg_log( "WC: ticks=%d sims=%d refine=%d full=%d pin=%d park=%d arm=%d hold=%d bail=%d kill=%d keys=%d flip=%d duck=%d cut=%d yield=%d div=%d us=%lld worst=%lld",
			               s_count[ wc_ticks ], s_count[ wc_sims ], s_count[ wc_refine ], s_count[ wc_full ], s_count[ wc_pin ], s_count[ wc_park ],
			               s_count[ wc_arm ], s_count[ wc_hold ], s_count[ wc_bail ], s_count[ wc_kill ], s_count[ wc_keys ], s_count[ wc_flip ],
			               s_count[ wc_duck ], s_count[ wc_cut ], s_count[ wc_yield ], s_count[ wc_div ], s_us, s_us_worst );
		for ( int& c : s_count )
			c = 0;
		s_us = s_us_worst = 0ll;
	}

	c_base_entity* const local = g_ctx.m_local;
	if ( !cmd || !local || !local->is_alive( ) || !GET_VARIABLE( g_variables.m_wall_climb, bool ) || local->get_move_type( ) != move_type_walk ||
	     !g_input.check_input( &GET_VARIABLE( g_variables.m_wall_climb_key, key_bind_t ) ) ) {
		reset_latch( );
		return;
	}
	auto* const collideable = local->get_collideable( );
	if ( !collideable ) {
		reset_latch( );
		return;
	}
	const bool on_ground = ( local->get_flags( ) & fl_onground ) != 0;
	if ( on_ground && !( cmd->m_buttons & in_jump ) ) {
		reset_latch( );
		return;
	}

	const int frame = g_interfaces.m_prediction->m_commands_predicted - 1;
	g_prediction.restore_entity_to_predicted_frame( frame );
	const c_vector origin    = local->get_abs_origin( );
	const c_vector start_vel = local->get_velocity( );
	// this START vs the END simmed for what went out last tick; stomped = a later feature rewrote it
	if ( m_div_cmd >= 0 && m_div_cmd == cmd->m_command_number - 1 ) {
		const c_vector dv = start_vel - m_div_vel;
		if ( dv.length( ) > k_div_vel ) {
			++s_count[ wc_div ];
			static int s_div_lines = 0;
			if ( s_div_lines < k_div_lines && ++s_div_lines )
				botox_dbg_log( "WC DIV: %s stomp=%d start xy=%.2f vz=%.2f pred xy=%.2f vz=%.2f dorg=%.4f,%.4f,%.4f", m_div_branch, m_stomp_cmd == m_div_cmd,
				               start_vel.length_2d( ), start_vel.m_z, m_div_vel.length_2d( ), m_div_vel.m_z, origin.m_x - m_div_org.m_x,
				               origin.m_y - m_div_org.m_y, origin.m_z - m_div_org.m_z );
		}
	}
	m_div_cmd = -1;
	const c_vector mins = collideable->get_obb_mins( ), maxs = collideable->get_obb_maxs( );
	const float hx = std::max( std::fabsf( mins.m_x ), std::fabsf( maxs.m_x ) );
	const float hy = std::max( std::fabsf( mins.m_y ), std::fabsf( maxs.m_y ) );
	c_trace_filter filter( local );

	// FUN_100ea5e0: closest vertical wall within hull edge + 2, line traces at feet + 1..4, 16 dirs from last hit
	trace_t wall_tr{ };
	float wall_d = FLT_MAX, wall_angle = 0.f;
	const float feet = origin.m_z + mins.m_z;
	for ( float a = m_scan_angle; a < m_scan_angle + k_scan_range; a += k_scan_step ) {
		const float ca = std::cosf( a ), sa = std::sinf( a );
		const float reach = std::min( hy / std::max( std::fabsf( sa ), 1e-6f ), hx / std::max( std::fabsf( ca ), 1e-6f ) ) + k_reach_pad;
		for ( float z = feet + k_feet_lo; z <= feet + k_feet_hi; z += 1.f ) {
			const c_vector from( origin.m_x, origin.m_y, z );
			trace_t tr{ };
			ray_t ray( from, from + c_vector( ca, sa, 0.f ) * reach );
			g_interfaces.m_engine_trace->trace_ray( ray, mask_playersolid, &filter, &tr );
			const c_vector& n = tr.m_plane.m_normal;
			if ( tr.m_fraction < 1.f && std::fabsf( n.m_z ) <= k_wall_nz && n.m_x * ca + n.m_y * sa <= k_wall_into && tr.m_fraction * reach < wall_d ) {
				wall_d     = tr.m_fraction * reach;
				wall_tr    = tr;
				wall_angle = a;
			}
		}
	}
	if ( wall_d == FLT_MAX ) {
		m_scan_angle = 0.f;
		reset_latch( );
		return;
	}
	m_scan_angle = std::fmod( wall_angle, k_scan_range );

	if ( g_air_stuck_holding || HITGODA || HITGODA2 || g_texturebug.m_hit || g_texturebug.m_hs_hit || g_edgebug.m_found ) {
		++s_count[ wc_yield ];
		reset_latch( );
		return;
	}
	// fireman has a ladder ( last tick's state, it runs after us ): a wc push on a ladder face latches it jumpless
	const auto& fr = g_movement.m_fireman_data;
	if ( GET_VARIABLE( g_variables.m_fire_man, bool ) && g_input.check_input( &GET_VARIABLE( g_variables.m_fire_man_key, key_bind_t ) ) &&
	     ( fr.is_ladder || fr.owns_cmd || fr.yaw_valid || fr.ladder_lock || fr.drop_in_lock || fr.launch_ticks > 0 ) ) {
		++s_count[ wc_yield ];
		reset_latch( );
		return;
	}

	const c_vector wall_n   = wall_tr.m_plane.m_normal;
	const float wall_yaw    = norm( rad2deg( std::atan2f( -wall_n.m_y, -wall_n.m_x ) ) );
	const float side        = norm( rad2deg( std::atan2f( start_vel.m_y, start_vel.m_x ) ) - wall_yaw ) < 0.f ? 1.f : -1.f;
	const float wall_z      = wall_tr.m_end.m_z;
	const float start_xy    = start_vel.length_2d( );
	const int tick          = g_interfaces.m_global_vars_base->m_tick_count;
	const c_user_cmd keys   = *cmd;
	const bool user_duck    = ( keys.m_buttons & in_duck ) != 0;
	const bool moving       = std::fabsf( keys.m_forward_move ) > k_moving || std::fabsf( keys.m_side_move ) > k_moving;
	const bool try_duck     = !user_duck && GET_VARIABLE( g_variables.m_wall_climb_duck, bool );
	const bool as_steers    = g_air_stuck_owns_cmd;
	const c_user_cmd as_cmd = *cmd;
	const auto pinned       = [ ]( const float vz ) { return g_prediction.is_target_predict_z_velocity( vz ); };

	n_tick::c_sim_budget budget{ };
	budget.start( k_time_share, n_tick::engine_interval( ), n_tick::search_wc );
	int sims = 0, commit_end = 0;
	bool cut = false, was_cut = false;
	float bounds = g_prediction.m_real_max_speed;
	const auto can_sim = [ & ]( ) {
		if ( sims >= commit_end && ( cut || ( sims > 0 && budget.expired( ) ) ) ) {
			cut = was_cut = true;
			return false;
		}
		return true;
	};
	const auto restore = [ & ]( ) {
		g_prediction.restore_entity_to_predicted_frame( frame );
		bounds = g_prediction.m_real_max_speed;
	};
	// engine order like tb pred_simulate: leaf list bounds from the previous move's max speed
	const auto step = [ & ]( c_user_cmd& c ) {
		g_prediction.m_bounds_max_speed = bounds;
		g_prediction.begin( local, &c );
		g_prediction.end( local );
		bounds                          = g_prediction.m_last_max_speed;
		g_prediction.m_bounds_max_speed = 0.f;
		n_tb::s_pred_dirty              = true;
		++sims;
	};
	const auto gap_at = [ & ]( const c_vector& o ) {
		const c_vector from( o.m_x, o.m_y, wall_z );
		trace_t tr{ };
		ray_t ray( from, from - c_vector( wall_n.m_x, wall_n.m_y, 0.f ) * k_gap_reach );
		g_interfaces.m_engine_trace->trace_ray( ray, mask_playersolid, &filter, &tr );
		return tr.m_fraction * k_gap_reach - ( std::fabsf( wall_n.m_x ) * hx + std::fabsf( wall_n.m_y ) * hy );
	};

	// ebanat sims on the real cmd so its bail / kill paths send the last sim; here only apply writes cmd, every other path = your keys
	c_user_cmd work = *cmd;

	// FUN_100eba50
	struct eval_t {
		float gap = FLT_MAX, xy = 0.f;
		int pins = 0, buttons = 0;
		bool rising = false, ran = false;
	};
	const auto eval = [ & ]( const float yaw, const float fwd, const bool duck, const bool multi ) {
		eval_t e{ };
		if ( !can_sim( ) )
			return e;
		restore( );
		const float vz0 = local->get_velocity( ).m_z;
		work.m_buttons  = duck ? keys.m_buttons | in_duck : keys.m_buttons;
		set_move( work, yaw, fwd );
		step( work );
		e.ran              = true;
		e.buttons          = work.m_buttons;
		const c_vector vel = local->get_velocity( );
		e.xy               = vel.length_2d( );
		if ( e.xy < k_dead_xy )
			return e;
		e.gap = gap_at( local->get_abs_origin( ) );
		if ( !pinned( vel.m_z ) )
			return e;
		e.pins   = 1;
		e.rising = vz0 > 0.f;
		for ( int k = 0; multi && k < k_multi_ticks && can_sim( ); ++k ) {
			step( work );
			if ( !pinned( local->get_velocity( ).m_z ) )
				break;
			++e.pins;
		}
		return e;
	};
	// FUN_100ebde0
	const auto eval_both = [ & ]( const float fwd, const float yaw, const bool multi ) {
		cand_t c{ };
		c.fwd = fwd, c.yaw = yaw;
		const eval_t s = eval( yaw, fwd, false, multi );
		if ( !s.ran )
			return c;
		c.valid = true, c.gap = s.gap, c.xy = s.xy, c.pins = s.pins;
		c.buttons = c.stand = s.buttons;
		c.duck              = s.buttons | in_duck;
		if ( !try_duck || ( !multi && s.pins > 0 ) )
			return c;
		const eval_t d = eval( yaw, fwd, true, multi );
		if ( !d.ran )
			return c;
		const bool more = s.pins > 0 && d.pins > s.pins && d.gap <= s.gap + k_duck_gap;
		const bool rise = s.pins <= 0 && d.pins > 0 && d.rising;
		if ( more || rise ) {
			c.gap = d.gap, c.xy = d.xy, c.pins = d.pins;
			c.buttons = c.duck = d.buttons;
			c.duck_more        = more;
		}
		return c;
	};

	cand_t best{ };
	const bool refine = tick - m_last_tick <= k_refine_ticks && std::fabsf( norm( wall_yaw - m_last_wall_yaw ) ) < k_refine_yaw &&
	                    std::fabsf( m_last_side - side ) < k_refine_side;
	if ( refine ) {
		++s_count[ wc_refine ];
		bool seen[ 2 ]{ };
		for ( const float d : k_refine_off ) {
			const float a = refine_off( m_last_off + d );
			if ( std::exchange( seen[ a > 0.f ], true ) )
				continue;
			const float yaw = a * side + wall_yaw;
			for ( const float m : k_refine_mul )
				if ( const cand_t c = eval_both( std::clamp( m * m_last_fwd, k_fwd_lo, k_fwd_hi ), yaw, false ); better( c, best ) )
					best = c;
		}
	}
	if ( !refine || !good( best ) ) {
		++s_count[ wc_full ];
		for ( const float off : k_off ) {
			cand_t row{ };
			for ( const float f : k_fwd )
				if ( const cand_t c = eval_both( f, off * side + wall_yaw, false ); better( c, row ) )
					row = c;
			if ( better( row, best ) )
				best = row;
		}
	}
	if ( good( best ) ) {
		m_last_off      = refine_off( norm( best.yaw - wall_yaw ) * side );
		m_last_fwd      = best.fwd;
		m_last_wall_yaw = wall_yaw;
		m_last_side     = side;
		m_last_tick     = tick;
	} else
		m_last_tick = k_no_tick;

	cut        = false;
	commit_end = sims + k_commit_sims;
	const cand_t fin = best.valid ? eval_both( best.fwd, best.yaw, true ) : cand_t{ };

	// FUN_100eae60
	const auto kills = [ & ]( const int buttons, const float yaw, const float fwd ) {
		if ( !GET_VARIABLE( g_variables.m_wall_climb_prevent_slow, bool ) || moving || start_xy < k_kill_start_xy || !can_sim( ) )
			return false;
		restore( );
		work.m_buttons = buttons;
		set_move( work, yaw, fwd );
		c_user_cmd c = work;
		step( c );
		const float xy = local->get_velocity( ).length_2d( );
		restore( );
		return xy < k_dead_xy || ( xy <= k_kill_xy && xy <= k_kill_ratio * start_xy );
	};
	// FUN_100eb670: your keys' fix_air_stucks flip
	bool wrote       = false;
	const auto apply = [ & ]( const int buttons, const float yaw, const float fwd ) {
		work.m_buttons = buttons;
		// FUN_100eb190: view yaw 5 deg off the wall when this move leaves you near still, wish kept
		if ( GET_VARIABLE( g_variables.m_wall_climb_visual_angles, bool ) && can_sim( ) ) {
			restore( );
			c_user_cmd c = work;
			set_move( c, yaw, fwd );
			step( c );
			const float xy = local->get_velocity( ).length_2d( );
			restore( );
			if ( xy < k_nudge_xy ) {
				const float out       = rad2deg( std::atan2f( wall_n.m_y, wall_n.m_x ) );
				work.m_view_point.m_y = norm( work.m_view_point.m_y + ( norm( out - work.m_view_point.m_y ) < 0.f ? -k_nudge : k_nudge ) );
				cmd->m_view_point.m_y = work.m_view_point.m_y;
			}
		}
		set_move( work, yaw, fwd );
		if ( ( std::fabsf( work.m_forward_move ) >= k_moving || std::fabsf( work.m_side_move ) >= k_moving ) && can_sim( ) ) {
			restore( );
			c_user_cmd c = work;
			step( c );
			const float in_xy = local->get_velocity( ).length_2d( );
			if ( in_xy < k_dead_xy && can_sim( ) ) {
				restore( );
				c                = work;
				c.m_forward_move = -work.m_forward_move;
				c.m_side_move    = -work.m_side_move;
				step( c );
				if ( in_xy + k_flip_gain < local->get_velocity( ).length_2d( ) ) {
					work.m_forward_move = c.m_forward_move;
					work.m_side_move    = c.m_side_move;
					++s_count[ wc_flip ];
				}
			}
		}
		cmd->m_forward_move = work.m_forward_move;
		cmd->m_side_move    = work.m_side_move;
		cmd->m_buttons      = work.m_buttons;
		wrote               = true;
	};
	// prevent slow: donor sends the slow move anyway, here your keys
	const auto kill = [ & ]( ) {
		reset_latch( );
		++s_count[ wc_kill ];
	};

	bool caught_now    = false;
	const char* branch = "none";
	if ( !fin.valid ) {
		reset_latch( );
	} else if ( fin.gap < k_bad_gap || ( fin.xy < k_bad_xy && start_xy > k_bail_start_xy ) ) {
		m_last_tick = k_no_tick;
		reset_latch( );
		++s_count[ wc_bail ];
		branch = "bail";
	} else if ( bad( fin ) && fin.pins <= 0 && !m_latch_arm && !m_latch_hold ) {
		// nothing catches and the best move is near still: donor parks you on it ( hover, your keys dropped ), here your keys
		++s_count[ wc_keys ];
		branch = "keys";
	} else {
		const bool falling_free = !pinned( start_vel.m_z ) && start_vel.m_z < k_fall_vz;
		if ( user_duck ) {
			branch = "duck";
			if ( fin.pins > 0 && kills( fin.buttons, fin.yaw, fin.fwd ) )
				kill( );
			else {
				caught_now = fin.pins > 0;
				apply( fin.buttons, fin.yaw, fin.fwd );
				reset_latch( );
			}
		} else if ( m_latch_hold ) {
			branch        = "hold";
			const int btn = falling_free ? m_latch_stand : m_latch_duck;
			if ( kills( btn, m_latch_yaw, m_latch_fwd ) )
				kill( );
			else {
				caught_now = true;
				apply( btn, m_latch_yaw, m_latch_fwd );
				++s_count[ wc_hold ];
				if ( falling_free )
					reset_latch( );
			}
		} else if ( m_latch_arm ) {
			branch = "arm";
			if ( kills( m_latch_stand, m_latch_yaw, m_latch_fwd ) )
				kill( );
			else {
				caught_now = true;
				apply( m_latch_stand, m_latch_yaw, m_latch_fwd );
				if ( m_latch_wait > 0 )
					--m_latch_wait;
				else {
					m_latch_hold = true;
					m_latch_arm  = false;
				}
			}
		} else if ( fin.duck_more ) {
			branch = "duck_more";
			if ( fin.pins > 0 && kills( fin.stand, fin.yaw, fin.fwd ) )
				kill( );
			else {
				caught_now    = fin.pins > 0;
				m_latch_arm   = true;
				m_latch_hold  = false;
				m_latch_wait  = std::max( 0, GET_VARIABLE( g_variables.m_wall_climb_duck_ticks, int ) - k_arm_ticks );
				m_latch_yaw   = fin.yaw;
				m_latch_fwd   = fin.fwd;
				m_latch_stand = fin.stand;
				m_latch_duck  = fin.duck;
				apply( fin.stand, fin.yaw, fin.fwd );
				++s_count[ wc_arm ];
			}
		} else {
			branch = fin.pins > 0 ? "pin" : "park";
			reset_latch( );
			if ( fin.pins > 0 && kills( fin.buttons, fin.yaw, fin.fwd ) )
				++s_count[ wc_kill ];
			else {
				caught_now = fin.pins > 0;
				apply( fin.buttons, fin.yaw, fin.fwd );
			}
		}
		s_count[ wc_pin ] += caught_now;
		s_count[ wc_park ] += wrote && !caught_now;
		s_count[ wc_duck ] += ( cmd->m_buttons & in_duck ) != 0 && !user_duck;
	}

	const long long us = budget.used_us( );
	if ( GET_VARIABLE( g_variables.m_debug_log, bool ) )
		botox_dbg_log( "WC T: %s g=%d pins=%d gap=%.4f xy=%.1f/%.1f off=%.2f fwd=%.3f d=%d/%d refine=%d sims=%d%s n=%.2f,%.2f vz=%.2f",
		               branch, on_ground, fin.pins, fin.gap, fin.xy, start_xy, norm( fin.yaw - wall_yaw ), fin.fwd, user_duck,
		               ( cmd->m_buttons & in_duck ) != 0, refine, sims, was_cut ? " CUT" : "", wall_n.m_x, wall_n.m_y, start_vel.m_z );
	g_prediction.restore_entity_to_predicted_frame( frame );
	s_count[ wc_sims ] += sims;
	++s_count[ wc_ticks ];
	s_count[ wc_cut ] += was_cut;
	s_us += us;
	s_us_worst = std::max( s_us_worst, us );

	if ( as_steers ) {
		if ( caught_now )
			g_air_stuck_owns_cmd = g_air_stuck_authored_view_valid = g_air_stuck_stamp_valid = g_air_stuck_stamp_force = false;
		else
			*cmd = as_cmd;
	}
	if ( GET_VARIABLE( g_variables.m_debug_log, bool ) ) {
		restore( );
		c_user_cmd c = *cmd;
		step( c );
		m_div_cmd    = cmd->m_command_number;
		m_div_org    = local->get_abs_origin( );
		m_div_vel    = local->get_velocity( );
		m_div_branch = branch;
		g_prediction.restore_entity_to_predicted_frame( frame );
	}
	if ( fin.valid && !on_ground )
		m_active_cmd = cmd->m_command_number;
	m_hit          = caught_now;
	m_sent_fwd     = cmd->m_forward_move;
	m_sent_side    = cmd->m_side_move;
	m_sent_buttons = cmd->m_buttons;
	m_sent_yaw     = cmd->m_view_point.m_y;
	if ( caught_now ) {
		m_catch_cmd = cmd->m_command_number;
		m_act_tick  = tick;
	}
}
