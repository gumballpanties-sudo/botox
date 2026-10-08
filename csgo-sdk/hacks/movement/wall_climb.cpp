#include "wall_climb.h"
#include "movement_internal.h"
#include "texturebug.h"
#include "texturebug_common.h"
#include "edgebug.h"
#include <cfloat>
#include <vector>

extern bool HITGODA;
extern bool HITGODA2;

// 10-06 rewrite ( tools/wc_engine/fl_lab.cpp P31 ): a disp lip lands only from the first 1-2 float positions off the face, so the
// hull is parked flush every air tick ( tb style press ladder ) and caught by the same ladder, or by a pin just over the lip
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

	enum e_wc_count {
		wc_land, wc_flip, wc_pin_catch, wc_hold, wc_park, wc_keys, wc_along, wc_own, wc_leave, wc_aa_leave, wc_exit, wc_exit_none, wc_far, wc_rise,
		wc_scan_cut, wc_cut, wc_div, wc_div_stomp, wc_stomp, wc_yield, wc_ramp, wc_skip, wc_jump_back, wc_jump_strip, wc_as_take, wc_pin_miss, wc_no_park,
		wc_park_keep, wc_away, wc_ticks, wc_sims, wc_max
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

void n_wall_climb::impl_t::on_create_move( c_user_cmd* cmd )
{
	if ( const unsigned long long now = GetTickCount64( ); now >= s_log_next ) {
		s_log_next = now + 1000ull;
		if ( s_count[ wc_ticks ] || s_count[ wc_yield ] || s_count[ wc_ramp ] || s_count[ wc_skip ] )
			botox_dbg_log( "WC: land=%d flip=%d pinc=%d hold=%d park=%d keys=%d along=%d own=%d leave=%d aa=%d exit=%d/%d far=%d rise=%d scut=%d cut=%d div=%d/%d stomp=%d yield=%d ramp=%d skip=%d jump back=%d strip=%d as=%d miss=%d np=%d pk=%d away=%d ticks=%d sims=%d us=%lld worst=%lld",
			               s_count[ wc_land ], s_count[ wc_flip ], s_count[ wc_pin_catch ], s_count[ wc_hold ], s_count[ wc_park ], s_count[ wc_keys ],
			               s_count[ wc_along ], s_count[ wc_own ], s_count[ wc_leave ], s_count[ wc_aa_leave ], s_count[ wc_exit ], s_count[ wc_exit_none ], s_count[ wc_far ],
			               s_count[ wc_rise ], s_count[ wc_scan_cut ], s_count[ wc_cut ], s_count[ wc_div ], s_count[ wc_div_stomp ], s_count[ wc_stomp ], s_count[ wc_yield ], s_count[ wc_ramp ], s_count[ wc_skip ],
			               s_count[ wc_jump_back ], s_count[ wc_jump_strip ], s_count[ wc_as_take ], s_count[ wc_pin_miss ], s_count[ wc_no_park ], s_count[ wc_park_keep ], s_count[ wc_away ], s_count[ wc_ticks ], s_count[ wc_sims ], s_us,
			               s_us_worst );
		for ( int& c : s_count )
			c = 0;
		s_us = s_us_worst = 0ll;
	}

	c_base_entity* const local = g_ctx.m_local;
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

	bool landed = false, held = false, jump_fix = false;
	if ( !on_ground ) {
		const bool was_holding = m_holding;
		m_holding              = false;
		if ( start_pinned || was_holding ) {
			restore( );
			const c_user_cmd hold = with( wall_yaw, 0.f, user_duck );
			if ( hold_lands( hold ) ) {
				send( hold );
				held = m_holding = true;
				++s_count[ wc_hold ];
				branch = "hold";
			}
		}
		if ( !held && start_pinned ) {
			exit_pin( );
			branch = "exit";
		} else if ( !held && !far_pin ) {
			const float F  = k_hull_step / ( ipt * airaccel * ipt * friction );
			// smooth demo view = no crouch pops: other stance never sent ( lab: first catches -10%, second rows -27% )
			bool live[ 2 ] = { true, !GET_VARIABLE( g_variables.m_silent_view, bool ) };
			float best_d   = FLT_MAX, best_xy = 0.f;
			c_user_cmd best_c{ };
			bool have_park = false;
			int pin_tries  = 0;
			sim_out_t last{ };
			// 1 caught, 0 go on, -1 out of clock
			const auto test = [ & ]( const c_user_cmd& c, const bool flip, const bool park ) {
				if ( !sim( c, last ) )
					return -1;
				if ( last.m_flags & fl_onground ) {
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
					if ( pin_tries < k_pin_tries ) {
						++pin_tries;
						if ( hold_lands( with( wall_yaw, 0.f, ( c.m_buttons & in_duck ) != 0 ) ) ) {
							send( c );
							landed = held = m_holding = true;
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
					if ( const float d = face_d( last.m_origin, wall_n ); d < best_d )
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
				commit_end = sims + k_commit_sims;
				sim_out_t k{ };
				const bool kok = sim( keys, k );
				const bool kpin = kok && !( k.m_flags & fl_onground ) && pinned( k.m_vel );
				// leave = your raw keys: auto align's fix_air_stucks push off the wall lost the park / wall ( 10-07 replay 5 -> 9 / 21 )
				c_user_cmd you     = keys;
				you.m_forward_move  = g_movement.m_user_forward_move_raw;
				you.m_side_move     = g_movement.m_user_side_move_raw;
				const bool you_leave = wish_leaves_wall( you, wall_n );
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
				}
			}
		}
	} else {
		m_holding = false;
		branch    = "ground";
		sim_out_t out{ };
		if ( sim( *cmd, out ) ) {
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

	// wc t: every wall tick. u = end of your cmd, s = end of what wall climb sent ( s.xy < u.xy = it cost speed ). org / vel / yaw
	// = START + your keys, enough to replay the tick in fl_lab. extra sims, debug log only, not in us=
	const long long us = budget.used_us( );
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
			g_air_stuck_owns_cmd = g_air_stuck_authored_view_valid = g_air_stuck_stamp_valid = g_air_stuck_stamp_force = false;
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
