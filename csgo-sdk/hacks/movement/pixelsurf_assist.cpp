#include "movement.h"
#include "assist_predict.h"
#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"
#include "../prediction/prediction.h"
#include "tick_scale.h"
#include <cmath>
#include <cstdio>
#include <iterator>
#include <vector>

extern void CorrectMovement( c_user_cmd* cmd, c_angle wish_angle, c_angle old_angles );
extern void botox_dbg_log( const char* fmt, ... );
extern bool ps_bailed( );

bool check( float a, float b )
{
	const float d = b - a;
	return d > -0.01f && d < 0.03f;
}

bool HITGODA = false;

static int s_ride_cmd = -1;
static char s_ride_start[ 512 ]{ };

static void ride_contact( const c_vector& org, char* out, const size_t size )
{
	const auto col = g_ctx.m_local->get_collideable( );
	if ( !col )
		return;
	const c_vector mins = col->get_obb_mins( ), maxs = col->get_obb_maxs( );
	const auto ent      = []( const trace_t& t ) { return t.m_hit_entity ? t.m_hit_entity->get_index( ) : -1; };
	c_trace_filter fil( g_ctx.m_local );
	float wall = 1.f, nx = 0.f, ny = 0.f;
	int wall_ent = -1;
	bool ss      = false;
	for ( int d = 0; d < 8; ++d ) {
		const float a = static_cast< float >( d ) * ( 3.14159265f / 4.f );
		trace_t tr;
		ray_t ray( org, c_vector( org.m_x + 2.f * std::cosf( a ), org.m_y + 2.f * std::sinf( a ), org.m_z ), mins, maxs );
		g_interfaces.m_engine_trace->trace_ray( ray, mask_playersolid, &fil, &tr );
		if ( tr.m_fraction < wall ) {
			wall     = tr.m_fraction;
			nx       = tr.m_plane.m_normal.m_x;
			ny       = tr.m_plane.m_normal.m_y;
			wall_ent = ent( tr );
			ss       = tr.m_start_solid;
		}
	}
	trace_t dn;
	ray_t down( org, c_vector( org.m_x, org.m_y, org.m_z - 0.25f ), mins, maxs );
	g_interfaces.m_engine_trace->trace_ray( down, mask_playersolid, &fil, &dn );
	snprintf( out, size, " wall=%.3f%s n=%.2f,%.2f #%d down=%.3f nz=%.2f #%d", wall * 2.f, ss ? "(ss)" : "", nx, ny, wall_ent,
	          dn.m_fraction * 0.25f, dn.m_fraction < 1.f ? dn.m_plane.m_normal.m_z : 0.f, dn.m_fraction < 1.f ? ent( dn ) : -1 );
}

void psa_ride_end( const c_user_cmd* cmd )
{
	if ( !cmd || s_ride_cmd != cmd->m_command_number )
		return;
	s_ride_cmd = -1;
	botox_dbg_log( "%s | duck=%d jump=%d fwd=%.0f side=%.0f yaw=%.1f ps=%d/%d", s_ride_start, ( cmd->m_buttons & in_duck ) ? 1 : 0,
	               ( cmd->m_buttons & in_jump ) ? 1 : 0, cmd->m_forward_move, cmd->m_side_move, cmd->m_view_point.m_y,
	               g_movement.m_pixelsurf_data.should_pixel_surf ? 1 : 0, g_movement.m_pixelsurf_data.should_unduck ? 1 : 0 );
}

static bool ps_assist_ground_reachable( const int horizon_ticks )
{
	const auto col  = g_ctx.m_local ? g_ctx.m_local->get_collideable( ) : nullptr;
	const float ipt = g_interfaces.m_global_vars_base ? g_interfaces.m_global_vars_base->m_interval_per_tick : 0.f;
	if ( !col || ipt <= 0.f || horizon_ticks < 1 )
		return true;

	const float grav = g_convars[ HASH_BT( "sv_gravity" ) ] ? g_convars[ HASH_BT( "sv_gravity" ) ]->get_float( ) : 800.f;

	const int seg_ticks = std::max( 1, ( horizon_ticks + 15 ) / 16 );
	const float seg_dt  = static_cast< float >( seg_ticks ) * ipt;
	const float sag     = 0.5f * grav * seg_dt * seg_dt;
	const float drift = 4.f + 30.f * ( static_cast< float >( horizon_ticks ) * ipt );

	/* the restored frame the sims start from ( backup_data can be a cmd stale ) */
	c_vector pos = g_ctx.m_local->get_origin( );
	c_vector vel = g_ctx.m_local->get_velocity( );

	const c_vector hull_mins = col->get_obb_mins( );
	const c_vector hull_maxs = col->get_obb_maxs( );
	const c_vector wide_mins( hull_mins.m_x - drift, hull_mins.m_y - drift, hull_mins.m_z - sag );
	const c_vector wide_maxs( hull_maxs.m_x + drift, hull_maxs.m_y + drift, hull_maxs.m_z + sag );

	/* not world-only: that skips static props + brush ents, the sims don't */
	c_trace_filter fil( g_ctx.m_local );
	for ( int t = 0; t < horizon_ticks; t += seg_ticks ) {
		const int steps = std::min( seg_ticks, horizon_ticks - t );
		const float dt  = static_cast< float >( steps ) * ipt;
		c_vector disp( vel.m_x * dt, vel.m_y * dt, vel.m_z * dt - 0.5f * grav * dt * dt );
		vel.m_z -= grav * dt;

		trace_t tr;
		ray_t ray( pos, pos + disp, wide_mins, wide_maxs );
		g_interfaces.m_engine_trace->trace_ray( ray, mask_playersolid, &fil, &tr );
		if ( tr.did_hit( ) )
			return true;

		pos = pos + disp;
	}

	return false;
}

void n_movement::impl_t::pixelsurf_assist( c_user_cmd* cmd )
{
	static n_assist::plan_t plan;
	static bool pending = false;
	static n_assist::program_t pending_program{ };
	static float pending_z  = 0.f;
	static int pending_tries = 0;
	static c_angle last_view{ };
	static unsigned long long render_after = 0ull;
	static int ride_arrival = 0, ride_until = -1, ride_pins = 0;
	static float ride_target = 0.f;
	static int nolip_skips   = 0;

	const auto stop = [ & ]( const char* why ) {
		if ( plan.active )
			botox_dbg_log( "[psa] drop %s k=%d/%d", why, cmd->m_tick_count - plan.start_tick, plan.arrival );
		plan.clear( );
		pending     = false;
		nolip_skips = 0;
		HITGODA     = false;
	};

	if ( !g_ctx.m_local || !GET_VARIABLE( g_variables.m_pixel_surf_assist, bool ) || !g_interfaces.m_engine_client->is_in_game( ) ||
	     !g_ctx.m_local->is_alive( ) ) {
		stop( "off" );
		return;
	}

	static int air_cmd     = 0;
	const bool on_ground   = ( g_ctx.m_local->get_flags( ) & fl_onground ) != 0;
	const bool landed      = on_ground && air_cmd == cmd->m_command_number - 1;
	if ( !on_ground )
		air_cmd = cmd->m_command_number;

	static bool key_prev = false;
	const bool key       = g_input.check_input( &GET_VARIABLE( g_variables.m_pixel_surf_assist_key, key_bind_t ) );
	if ( !key && ( !plan.active || cmd->m_tick_count - plan.start_tick <= plan.launch ) ) {
		key_prev = false;
		stop( "key" );
		return;
	}
	if ( !key && key_prev )
		botox_dbg_log( "[psa] key up k=%d/%d: flown out", cmd->m_tick_count - plan.start_tick, plan.arrival );
	key_prev = key;

	HITGODA = false;
	const int ps_type       = GET_VARIABLE( g_variables.m_pixel_surf_assist_type, int );
	const float pin         = g_prediction.get_engine_target_predict_z_velocity( );
	const c_angle live_view = cmd->m_view_point;
	const c_angle turn      = live_view - last_view;
	last_view               = live_view;

	static long long st_calls = 0ll, st_sims = 0ll, st_budget = 0ll, st_gate = 0ll, st_plans = 0ll, st_rejects = 0ll, st_diverge = 0ll, st_nolip = 0ll;
	static long long st_us = 0ll;

	if ( plan.active ) {
		const int k = cmd->m_tick_count - plan.start_tick;
		bool off    = k <= plan.arrival && !n_assist::on_track( plan, k );
		if ( !off && k <= plan.arrival && plan.ticks[ k ].ground )
			off = !n_assist::in_window( n_assist::make_target( plan.target_z, -1 ), plan.arrive_z + n_assist::start_dz( plan, k ) );
		if ( off ) {
			++st_diverge;
			const bool in_range = k >= 0 && k < static_cast< int >( plan.ticks.size( ) );
			botox_dbg_log( "[psa] diverge %s%s k=%d/%d dz=%.3f ground=%d/%d", n_assist::k_launch_names[ plan.program.launch ],
			               plan.program.crouch ? " crouch" : "", k, plan.arrival, in_range ? n_assist::start_dz( plan, k ) : 0.f,
			               on_ground ? 1 : 0, in_range && plan.ticks[ k ].ground ? 1 : 0 );
			plan.clear( );
		}
	}

	if ( !plan.active && ps_bailed( ) )
		return;
	if ( !plan.active && key ) {
		constexpr int ps_max_points = 4;
		const std::string map       = g_interfaces.m_engine_client->get_level_name_short( );
		const c_vector origin       = g_ctx.m_local->get_origin( );
		points_check_t cands[ ps_max_points ];
		float cand_dist[ ps_max_points ]{ };
		int cand_count = 0;
		for ( int i = 0; i < static_cast< int >( m_points_check.size( ) ); i++ ) {
			const points_check_t& p = m_points_check[ i ];
			if ( p.map != map || !p.active || ( p.is_free_point && p.point_type != 0 ) )
				continue;
			const float d = origin.dist_to( p.pos );
			if ( d > p.radius )
				continue;
			int slot = cand_count < ps_max_points ? cand_count++ : ps_max_points;
			for ( ; slot > 0 && cand_dist[ slot - 1 ] > d; --slot ) {
				if ( slot < ps_max_points ) {
					cands[ slot ]     = cands[ slot - 1 ];
					cand_dist[ slot ] = cand_dist[ slot - 1 ];
				}
			}
			if ( slot < ps_max_points ) {
				cands[ slot ]     = p;
				cand_dist[ slot ] = d;
			}
		}
		if ( ps_type == 1 ) {
			int kept = 1;
			for ( int c = 1; c < cand_count; ++c )
				if ( cands[ c ].delta_strafe == cands[ 0 ].delta_strafe )
					cands[ kept++ ] = cands[ c ];
			cand_count = cand_count > 0 ? kept : 0;
		}

		if ( cand_count == 0 ) {
			static uint64_t last_unreachable_msg = 0;
			if ( const uint64_t now = GetTickCount64( ); now - last_unreachable_msg > 3000 ) {
				if ( GET_VARIABLE( g_variables.m_pixel_surf_assist_render, bool ) )
					movement_add_window( 3.0f, "[-] point unreachable" );
				last_unreachable_msg = now;
			}
			return;
		}

		/* "stamina hops" off: never press off a stand that still carries landing stamina. the landing cmd itself is
		   the bhop tick an airborne plan presses on anyway ( landing stamina ~15 ), so it always searches */
		if ( !GET_VARIABLE( g_variables.m_pixel_surf_assist_brokehop, bool ) && on_ground && !landed && g_ctx.m_local->get_stamina( ) != 0.f )
			return;

		const int raw_ticks = GET_VARIABLE( g_variables.m_pixel_surf_assist_ticks, int );
		if ( raw_ticks <= 0 )
			return;
		const int assist_ticks = std::clamp( n_tick::ticks( raw_ticks ), 1, 256 );

		if ( !on_ground && g_ctx.m_local->get_velocity( ).m_z <= 0.f && !ps_assist_ground_reachable( assist_ticks ) ) {
			++st_gate;
			return;
		}

		n_assist::ctx_t c;
		c.kind      = n_assist::arrival_surf;
		c.frame     = g_interfaces.m_prediction->m_commands_predicted - 1;
		c.buttons   = cmd->m_buttons;
		c.fwd       = c.fwd0  = cmd->m_forward_move;
		c.side      = c.side0 = cmd->m_side_move;
		c.view      = live_view;
		c.delta     = ps_type == 1 ? turn * cands[ 0 ].delta_strafe : c_angle{ };
		c.pre_ticks = assist_ticks;
		c.air_ticks = n_tick::ticks( 256 );
		c.lj_hold   = n_tick::ticks( 3 );
		c.gravity   = g_convars.float_or( HASH_BT( "sv_gravity" ), 800.f );
		c.lowest    = cands[ 0 ].pos.m_z;
		for ( int i = 1; i < cand_count; ++i )
			c.lowest = std::min( c.lowest, static_cast< double >( cands[ i ].pos.m_z ) );
		if ( const c_vector v0 = g_ctx.m_local->get_velocity( ); GET_VARIABLE( g_variables.m_pixel_surf_fix, bool ) && v0.length_2d( ) >= 285.91f ) {
			const float razn = ( v0.length_2d( ) + 2.f - 285.91f ) / 12.f * n_tick::rate( );
			c_vector back    = v0 * -1.f;
			back.m_z         = 0.f;
			const float rot  = deg2rad( back.to_angle2( ).m_y - live_view.m_y );
			c.fwd0           = std::cos( rot ) * razn;
			c.side0          = -std::sin( rot ) * razn;
		}
		n_tick::c_sim_budget budget;
		budget.start( 0.45f, 0.f, n_tick::search_psa );
		c.budget   = &budget;
		c.max_sims = 512;

		const auto render = [ & ]( const n_assist::program_t& p, float surf_z ) {
			if ( !GET_VARIABLE( g_variables.m_pixel_surf_assist_render, bool ) )
				return;
			if ( const unsigned long long now = GetTickCount64( ); now >= render_after ) {
				render_after = now + 500ull;
				movement_add_window( 5, std::string( n_assist::k_launch_names[ p.launch ] )
				                            .append( " to surf at " )
				                            .append( std::to_string( surf_z ) )
				                            .append( " with " )
				                            .append( std::to_string( g_prediction.backup_data.m_origin.m_z ) )
				                            .append( p.crouch ? " (crouch)" : " (stand)" ) );
			}
		};
		const auto take = [ & ]( const n_assist::program_t& p, n_assist::run_t& r, int point, float surf_z ) {
			const int lip = r.lip;
			plan.take( p, r, cmd->m_tick_count, point, surf_z );
			ride_pins   = 0;
			nolip_skips = 0;
			++st_plans;
			botox_dbg_log( "[psa] plan %s%s surf %.4f arrive %.4f launch k=%d arrival k=%d sims=%d lip=%d", n_assist::k_launch_names[ p.launch ],
			               p.crouch ? " crouch" : "", surf_z, plan.arrive_z, plan.launch, plan.arrival, c.sims, lip );
			render( p, surf_z );
		};

		/* a window hit no press pins ( lip 0 ) is a wasted jump: keep searching programs and the next cmds ( new xy, stamina ),
		   take it only after ticks( 8 ) skips. the probe can't see auto align's ulp ladder pins on axial walls */
		n_assist::run_t fallback;
		n_assist::program_t fallback_prog{ };
		float fallback_z  = 0.f;
		int fallback_point = -1;
		const auto note_nolip = [ & ]( const n_assist::program_t& p, n_assist::run_t& r, const std::vector< n_assist::target_t >& tg ) {
			if ( r.nolip < 0 || fallback.nolip >= 0 )
				return;
			fallback_prog  = p;
			fallback_z     = tg[ r.nolip_target ].z;
			fallback_point = tg[ r.nolip_target ].point;
			fallback       = std::move( r );
		};

		bool landing_in_reach = on_ground;
		if ( !on_ground && ( !pending || pending_program.launch == n_assist::launch_jumpbug ) ) {
			const n_assist::run_t probe = n_assist::run( c, n_assist::program_t{ n_assist::launch_jump, false }, { }, false );
			c.jb_tick                   = probe.landing;
			landing_in_reach            = probe.landing >= 0 || !probe.complete;
		}

		if ( pending ) {
			pending = false;
			const std::vector< n_assist::target_t > one{ n_assist::make_target( pending_z, -1 ) };
			n_assist::run_t v = n_assist::run( c, pending_program, one, true );
			if ( v.hit )
				take( pending_program, v, -1, pending_z );
			else if ( !v.complete && ++pending_tries < 4 )
				pending = true;
		}

		/* the clock fits ~4 programs a tick: a cut search resumes at the program it cut next tick, or lj / crouch /
		   jb would never be flown with every flag on */
		static int resume     = 0;
		constexpr int n_progs = static_cast< int >( std::size( n_assist::k_programs ) );
		int last              = -1;
		char miss[ 512 ]{ };
		int miss_len = 0;
		std::vector< n_assist::target_t > targets;
		targets.reserve( ps_max_points );
		for ( int visit = 0; visit < n_progs && landing_in_reach && !plan.active && !pending && !c.out_of_budget; ++visit ) {
			last                           = ( resume + visit ) % n_progs;
			const n_assist::program_t& prog = n_assist::k_programs[ last ];
			if ( prog.launch == n_assist::launch_jumpbug && on_ground )
				continue;

			targets.clear( );
			for ( int ci = 0; ci < cand_count; ++ci ) {
				for ( const n_assist::flag_t& f : n_assist::k_flags ) {
					if ( n_assist::same( f.program, prog ) && cands[ ci ].*f.flag && !is_bind_restricted_in_radius( cands[ ci ], origin, f.bind ) ) {
						targets.push_back( n_assist::make_target( cands[ ci ].pos.m_z, ci ) );
						break;
					}
				}
			}
			if ( targets.empty( ) )
				continue;

			n_assist::run_t r = n_assist::run( c, prog, targets, false );
			if ( !r.hit ) {
				if ( ( r.gap < 1e8f || r.nolip >= 0 ) && miss_len < static_cast< int >( sizeof( miss ) ) - 32 ) {
					if ( r.gap < 1e8f )
						miss_len += sprintf_s( miss + miss_len, sizeof( miss ) - miss_len, " %s%s=%.3f%s%s", n_assist::k_launch_names[ prog.launch ],
						                       prog.crouch ? "(c)" : "", r.gap, r.complete ? "" : "(cut)", r.nolip >= 0 ? "(nolip)" : "" );
					else
						miss_len += sprintf_s( miss + miss_len, sizeof( miss ) - miss_len, " %s%s=nolip", n_assist::k_launch_names[ prog.launch ],
						                       prog.crouch ? "(c)" : "" );
				}
				note_nolip( prog, r, targets );
				continue;
			}

			const int t = r.target;
			if ( !r.exact ) {
				/* every target: a lip-less first crossing falls on to the lower points */
				n_assist::run_t v = n_assist::run( c, prog, targets, true );
				if ( !v.hit ) {
					if ( !v.complete ) {
						pending         = true;
						pending_program = prog;
						pending_z       = targets[ t ].z;
						pending_tries   = 0;
					} else {
						/* full = where the g_prediction flight stopped: launch -1 = press refused, gnd=1 = a floor
						   under the arc ( takeoff z = your keys never carried you off it ), nolip=k = window hit, nothing pinned */
						++st_rejects;
						botox_dbg_log( "[psa] verify miss %s%s surf %.4f stepped arrive %.4f k=%d | full launch=%d stop k=%d z=%.3f gnd=%d nolip=%d",
						               n_assist::k_launch_names[ prog.launch ], prog.crouch ? " crouch" : "", targets[ t ].z, r.arrive_z, r.arrival,
						               v.launch, static_cast< int >( v.ticks.size( ) ) - 1, v.end_z, v.end_ground ? 1 : 0, v.nolip );
						note_nolip( prog, v, targets );
					}
					continue;
				}
				r = std::move( v );
			}
			take( prog, r, targets[ r.target ].point, targets[ r.target ].z );
		}
		if ( !c.out_of_budget )
			resume = 0;
		else if ( last >= 0 )
			resume = last;

		if ( !plan.active && !pending && !c.out_of_budget && fallback.nolip >= 0 ) {
			++st_nolip;
			/* 10-09 11:14 log: ground never got past skip 2/8 ( a no-hit search resets it ). press-this-cmd arcs can't wait */
			const int k_wait = n_tick::ticks( 8 );
			const bool now   = fallback.launch == 0;
			botox_dbg_log( "[psa] nolip %s%s surf %.4f arrive %.4f k=%d skip %d/%d%s", n_assist::k_launch_names[ fallback_prog.launch ],
			               fallback_prog.crouch ? " crouch" : "", fallback_z, fallback.nolip_z, fallback.nolip, nolip_skips + 1, k_wait, now ? " now" : "" );
			if ( ++nolip_skips > k_wait || now ) {
				fallback.hit      = true;
				fallback.lip      = 0;
				fallback.target   = fallback.nolip_target;
				fallback.arrival  = fallback.nolip;
				fallback.arrive_z = fallback.nolip_z;
				fallback.ticks.resize( static_cast< size_t >( fallback.nolip ) + 1 );
				take( fallback_prog, fallback, fallback_point, fallback_z );
			}
		} else if ( !plan.active && !pending && !c.out_of_budget )
			nolip_skips = 0;

		cmd->m_buttons      = c.buttons;
		cmd->m_forward_move = c.fwd;
		cmd->m_side_move    = c.side;
		cmd->m_view_point   = live_view;
		g_prediction.restore_entity_to_predicted_frame( c.frame );

		if ( !plan.active && !pending && ( landed || c.jb_tick == 0 ) )
			botox_dbg_log( "[psa] no arc%s z=%.3f vz=%.1f surf=%.4f sims=%d%s", on_ground ? " (ground)" : " (landing)", origin.m_z,
			               g_ctx.m_local->get_velocity( ).m_z, cands[ 0 ].pos.m_z, c.sims, miss_len ? miss : " (nothing flown)" );

		++st_calls;
		st_sims += c.sims;
		st_budget += c.out_of_budget ? 1 : 0;
		st_us += budget.used_us( );
	}

	if ( plan.active ) {
		const int k  = cmd->m_tick_count - plan.start_tick;
		ride_arrival = plan.start_tick + plan.arrival;
		ride_target  = plan.target_z;
		if ( k <= plan.arrival ) {
			const n_assist::tick_t& t = plan.ticks[ k ];
			cmd->m_buttons            = ( cmd->m_buttons & ~( in_jump | in_duck ) ) | t.buttons;
		} else {
			const bool pinned = std::fabs( g_ctx.m_local->get_velocity( ).m_z - pin ) < 0.01f;
			const bool ground = ( g_ctx.m_local->get_flags( ) & fl_onground ) != 0;
			plan.unpinned     = pinned ? 0 : plan.unpinned + 1;
			ride_pins += pinned ? 1 : 0;
			if ( ground || ps_bailed( ) || ( k > plan.arrival + 2 && plan.unpinned > 2 ) ) {
				botox_dbg_log( "[psa] end %s%s rode=%d pinned=%d %s", n_assist::k_launch_names[ plan.program.launch ],
				               plan.program.crouch ? " crouch" : "", k - plan.arrival, ride_pins,
				               ground ? "ground" : ps_bailed( ) ? "bail" : "off pin" );
				ride_until = cmd->m_tick_count + n_tick::ticks( 6 );
				plan.clear( );
			} else
				cmd->m_buttons = ( cmd->m_buttons & ~in_duck ) | ( plan.ticks.back( ).buttons & in_duck );
		}
	}
	HITGODA = plan.active;

	if ( const int rk = cmd->m_tick_count - ride_arrival; plan.active ? rk >= -2 : cmd->m_tick_count <= ride_until ) {
		const c_vector o = g_ctx.m_local->get_origin( );
		const c_vector v = g_ctx.m_local->get_velocity( );
		const bool win   = n_assist::in_window( n_assist::make_target( ride_target, -1 ), o.m_z );
		const int k      = cmd->m_tick_count - plan.start_tick;
		char dz[ 32 ]{ };
		if ( plan.active && k >= 0 && k < static_cast< int >( plan.ticks.size( ) ) )
			snprintf( dz, sizeof( dz ), " dz=%+.4f", o.m_z - plan.ticks[ k ].z );
		else if ( plan.active && k == plan.arrival + 1 )
			snprintf( dz, sizeof( dz ), " dz=%+.4f", o.m_z - plan.arrive_z );
		const int n = snprintf( s_ride_start, sizeof( s_ride_start ),
		                        "[psr] k=%+d z=%.4f%s win=%d vz=%.2f pin=%d gnd=%d ducked=%d/%.2f pre=%d xy=%.2f,%.2f v=%.1f,%.1f user=%.0f/%.0f", rk,
		                        o.m_z, dz, win ? 1 : 0, v.m_z, std::fabs( v.m_z - pin ) < 0.01f ? 1 : 0, ( g_ctx.m_local->get_flags( ) & fl_onground ) ? 1 : 0,
		                        ( g_ctx.m_local->get_flags( ) & fl_ducking ) ? 1 : 0, g_ctx.m_local->get_duck_amount( ), ( cmd->m_buttons & in_duck ) ? 1 : 0,
		                        o.m_x, o.m_y, v.m_x, v.m_y, m_user_forward_move_raw, m_user_side_move_raw );
		if ( n > 0 && n < static_cast< int >( sizeof( s_ride_start ) ) ) {
			ride_contact( o, s_ride_start + n, sizeof( s_ride_start ) - static_cast< size_t >( n ) );
			s_ride_cmd = cmd->m_command_number;
		}
	}

	static unsigned long long next_report = 0ull;
	if ( const unsigned long long now = GetTickCount64( ); now >= next_report ) {
		next_report = now + 1000ull;
		if ( st_calls || st_plans || st_diverge )
			botox_dbg_log( "PS: calls=%lld sims=%lld avg=%.1f us=%lld budget=%lld gate=%lld plans=%lld rejects=%lld diverge=%lld nolip=%lld", st_calls,
			               st_sims, st_calls ? static_cast< float >( st_sims ) / static_cast< float >( st_calls ) : 0.f, st_calls ? st_us / st_calls : 0ll,
			               st_budget, st_gate, st_plans, st_rejects, st_diverge, st_nolip );
		st_calls = st_sims = st_budget = st_gate = st_plans = st_rejects = st_diverge = st_nolip = st_us = 0ll;
	}
}

void n_movement::impl_t::pixelsurf_assist_ground_help( c_user_cmd* cmd )
{
	if ( !GET_VARIABLE( g_variables.m_pixel_surf_assist_ground_help, bool ) )
		return;
	if ( !GET_VARIABLE( g_variables.m_pixel_surf_assist, bool ) )
		return;
	if ( !g_ctx.m_local )
		return;
	if ( !g_interfaces.m_engine_client->is_in_game( ) )
		return;
	if ( !g_ctx.m_local->is_alive( ) )
		return;
	if ( !g_input.check_input( &GET_VARIABLE( g_variables.m_pixel_surf_assist_key, key_bind_t ) ) )
		return;
	if ( !( g_ctx.m_local->get_flags( ) & 1 ) )
		return;
	if ( m_points_check.size( ) == 0 )
		return;
	int index     = -1;
	float nearest = FLT_MAX;
	for ( int i = 0; i < m_points_check.size( ); i++ ) {
		if ( g_interfaces.m_engine_client->get_level_name_short( ) == m_points_check.at( i ).map && m_points_check.at( i ).active ) {
			float distance     = g_ctx.m_local->get_origin( ).dist_to( m_points_check.at( i ).pos );
			float check_radius = m_points_check.at( i ).radius;
			if ( m_points_check.at( i ).use_custom_first_jump_radius ) {
				check_radius = m_points_check.at( i ).first_jump_radius;
			}
			if ( distance <= check_radius ) {
				if ( distance < nearest ) {
					nearest = distance;
					index   = i;
				}
			}
		}
	}
	if ( index == -1 )
		return;
	c_vector target_pos   = m_points_check[ index ].pos;
	c_vector direction    = ( target_pos - g_ctx.m_local->get_origin( ) ).normalized( );
	c_angle target_angle  = direction.to_angle2( );
	c_angle current_angle = cmd->m_view_point;
	float yaw_diff        = target_angle.m_y - current_angle.m_y;
	while ( yaw_diff > 180.0f )
		yaw_diff -= 360.0f;
	while ( yaw_diff < -180.0f )
		yaw_diff += 360.0f;
	float max_angle = GET_VARIABLE( g_variables.m_pixel_surf_assist_ground_angle_limit, float );
	if ( yaw_diff > max_angle )
		yaw_diff = max_angle;
	else if ( yaw_diff < -max_angle )
		yaw_diff = -max_angle;
	c_angle new_angle = current_angle;
	new_angle.m_y += yaw_diff;
	new_angle.normalize( ).clamp( );
	cmd->m_view_point = new_angle;
	CorrectMovement( cmd, new_angle, current_angle );
}

void n_movement::impl_t::pixelsurf_assist_render( )
{
	if ( m_lirili_larila_pending_render ) {
		movement_add_window( 5, std::format( "assisting to {}", static_cast< int >( m_lirili_larila_pending_origin.m_y ) ) );
		m_lirili_larila_pending_render = false;
	}
	if ( !GET_VARIABLE( g_variables.m_pixel_surf_assist, bool ) && !GET_VARIABLE( g_variables.m_bouncee_assist, bool ) )
		return;
	if ( !g_ctx.m_local )
		return;
	if ( !g_interfaces.m_engine_client->is_in_game( ) )
		return;
	if ( !g_ctx.m_local->is_alive( ) )
		return;
}

bool n_movement::impl_t::is_bind_restricted_in_radius( const points_check_t& point, const c_vector& player_pos, const std::string& bind_type )
{
	if ( !point.enable_restricted_binds || point.selected_restricted_bind == 0 ) {
		return false;
	}
	float distance       = player_pos.dist_to( point.pos );
	bool has_restriction = false;
	if ( bind_type == "mj" && point.selected_restricted_bind == 1 ) {
		has_restriction = true;
	} else if ( bind_type == "jb" && point.selected_restricted_bind == 2 ) {
		has_restriction = true;
	} else if ( bind_type == "lj" && point.selected_restricted_bind == 3 ) {
		has_restriction = true;
	} else if ( bind_type == "j" && point.selected_restricted_bind == 4 ) {
		has_restriction = true;
	} else if ( bind_type == "ch" && point.selected_restricted_bind == 5 ) {
		has_restriction = true;
	} else if ( bind_type == "mch" && point.selected_restricted_bind == 6 ) {
		has_restriction = true;
	} else if ( bind_type == "hcj" && point.selected_restricted_bind == 7 ) {
		has_restriction = true;
	}
	if ( has_restriction && distance <= point.restricted_binds_radius ) {
		return true;
	}
	return false;
}
