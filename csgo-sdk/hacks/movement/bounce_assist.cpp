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

extern void botox_dbg_log( const char* fmt, ... );

bool HITGODA2 = false;

/* lumi head bounce ( FUN_10121010 ): sim every program, the first arc whose head reaches the point's z is the plan, jump / duck
   replayed. lumi sims stand still + scales your keys on the plan tick; botox sims fly YOUR keys and view ( the point's delta
   strafe turns it ), plan writes jump / duck only, move + view stay yours */
void n_movement::impl_t::autobounce_assist( c_user_cmd* cmd )
{
	static n_assist::plan_t plan;
	static c_angle last_view{ };
	static unsigned long long render_after = 0ull;
	static bool ended                      = false;
	static long long st_calls = 0ll, st_sims = 0ll, st_plans = 0ll, st_diverge = 0ll, st_bang = 0ll, st_miss = 0ll, st_us = 0ll;
	static long long st_nopoint = 0ll, st_stam = 0ll, st_duck = 0ll; /* silent gates, key held */

	const auto stop = [ & ]( ) {
		plan.clear( );
		HITGODA2 = false;
	};

	if ( !GET_VARIABLE( g_variables.m_bouncee_assist, bool ) || !g_ctx.m_local || !g_interfaces.m_engine_client->is_in_game( ) ||
	     !g_ctx.m_local->is_alive( ) || !g_ctx.m_local->get_collideable( ) ) {
		stop( );
		return;
	}
	if ( !g_input.check_input( &GET_VARIABLE( g_variables.m_bounce_assist_key, key_bind_t ) ) ) {
		if ( plan.active )
			botox_dbg_log( "[bsa] key up k=%d/%d", cmd->m_tick_count - plan.start_tick, plan.arrival );
		stop( );
		return;
	}

	/* 1/s, before the silent gates: nopoint = no active point of this map in radius, stam = ground stamina ( brokehop off ), duck = ducked */
	static unsigned long long next_report = 0ull;
	if ( const unsigned long long now = GetTickCount64( ); now >= next_report ) {
		next_report = now + 1000ull;
		if ( st_calls || st_plans || st_diverge || st_bang || st_miss || st_nopoint || st_stam || st_duck )
			botox_dbg_log( "BSA: calls=%lld sims=%lld avg=%.1f us=%lld plans=%lld diverge=%lld bang=%lld missed=%lld nopoint=%lld stam=%lld duck=%lld", st_calls,
			               st_sims, st_calls ? static_cast< float >( st_sims ) / static_cast< float >( st_calls ) : 0.f, st_calls ? st_us / st_calls : 0ll,
			               st_plans, st_diverge, st_bang, st_miss, st_nopoint, st_stam, st_duck );
		st_calls = st_sims = st_plans = st_diverge = st_bang = st_miss = st_us = st_nopoint = st_stam = st_duck = 0ll;
	}

	const c_angle live_view = cmd->m_view_point;
	const float live_fwd    = cmd->m_forward_move;
	const float live_side   = cmd->m_side_move;
	const c_angle turn      = live_view - last_view;
	last_view               = live_view;
	const bool on_ground    = ( g_ctx.m_local->get_flags( ) & fl_onground ) != 0;

	/* bounce points + free points of type 1, 2d radius, nearest first ( copies: menu thread owns the vectors ) */
	constexpr int max_points = 8;
	const std::string map    = g_interfaces.m_engine_client->get_level_name_short( );
	const c_vector origin    = g_ctx.m_local->get_origin( );
	points_check_t cands[ max_points ];
	float cand_dist[ max_points ]{ };
	int cand_count = 0;
	const auto consider = [ & ]( const points_check_t& p ) {
		if ( p.map != map || !p.active )
			return;
		const float d = origin.dist_to_2d( p.pos );
		if ( d > p.radius )
			return;
		int slot = cand_count < max_points ? cand_count++ : max_points;
		for ( ; slot > 0 && cand_dist[ slot - 1 ] > d; --slot ) {
			if ( slot < max_points ) {
				cands[ slot ]     = cands[ slot - 1 ];
				cand_dist[ slot ] = cand_dist[ slot - 1 ];
			}
		}
		if ( slot < max_points ) {
			cands[ slot ]     = p;
			cand_dist[ slot ] = d;
		}
	};
	for ( int i = 0; i < static_cast< int >( m_bounce_points_check.size( ) ); i++ ) {
		if ( m_bounce_points_check[ i ].point_type != 2 ) /* 2 = pixeljump assist's */
			consider( m_bounce_points_check[ i ] );
	}
	for ( int i = 0; i < static_cast< int >( m_points_check.size( ) ); i++ ) {
		if ( m_points_check[ i ].is_free_point && m_points_check[ i ].point_type == 1 )
			consider( m_points_check[ i ] );
	}

	/* the restored frame = start of this cmd's tick: k - 1 already ran. bang = head on the point z with vz clipped to <= 0 */
	if ( plan.active ) {
		const int k = cmd->m_tick_count - plan.start_tick;
		if ( k <= plan.arrival && !n_assist::on_track( plan, k ) ) {
			++st_diverge;
			botox_dbg_log( "[bsa] diverge %s%s k=%d/%d dz=%.3f gnd=%d", n_assist::k_launch_names[ plan.program.launch ], plan.program.crouch ? " crouch" : "",
			               k, plan.arrival, k >= 0 && k < static_cast< int >( plan.ticks.size( ) ) ? n_assist::start_dz( plan, k ) : 0.f, on_ground ? 1 : 0 );
			plan.clear( );
		} else if ( k > plan.arrival && !ended ) {
			ended            = true;
			const float head = origin.m_z + g_ctx.m_local->get_collideable( )->get_obb_maxs( ).m_z;
			const float vz   = g_ctx.m_local->get_velocity( ).m_z;
			const bool bang  = vz <= 0.f && std::fabs( head - plan.target_z ) < 0.5f;
			++( bang ? st_bang : st_miss );
			botox_dbg_log( "[bsa] end %s%s %s k=%d/%d head=%.3f bang=%.3f vz=%.1f gnd=%d", n_assist::k_launch_names[ plan.program.launch ],
			               plan.program.crouch ? " crouch" : "", bang ? "bang" : "missed", k, plan.arrival, head, plan.target_z, vz, on_ground ? 1 : 0 );
		}
		if ( plan.active && k > plan.arrival + 2 )
			plan.clear( );
	}

	if ( !plan.active ) {
		HITGODA2 = false;
		if ( cand_count == 0 ) {
			++st_nopoint;
			static uint64_t last_unreachable_msg_bounce = 0;
			if ( const uint64_t now = GetTickCount64( ); now - last_unreachable_msg_bounce > 3000 ) {
				if ( GET_VARIABLE( g_variables.m_bouncee_assist_render, bool ) )
					movement_add_window( 3.0f, "[-] point unreachable" );
				last_unreachable_msg_bounce = now;
			}
			return;
		}

		if ( !GET_VARIABLE( g_variables.m_bouncee_assist_brokehop, bool ) && on_ground && g_ctx.m_local->get_stamina( ) != 0.f ) {
			++st_stam;
			return;
		}
		/* old gate: never plans off a crouch already in */
		if ( g_ctx.m_local->get_flags( ) & fl_ducking ) {
			++st_duck;
			return;
		}

		n_assist::ctx_t c;
		c.kind      = n_assist::arrival_bang;
		c.frame     = g_interfaces.m_prediction->m_commands_predicted - 1;
		c.buttons   = cmd->m_buttons;
		c.fwd       = c.fwd0 = live_fwd;
		c.side      = c.side0 = live_side;
		c.view      = live_view;
		c.delta     = turn * cands[ 0 ].delta_strafe;
		c.pre_ticks = n_tick::ticks( 48 );
		c.air_ticks = n_tick::ticks( 48 );
		c.lj_hold   = n_tick::ticks( 2 );
		c.gravity   = g_convars.float_or( HASH_BT( "sv_gravity" ), 800.f );
		n_tick::c_sim_budget budget;
		budget.start( 0.45f, 0.f, n_tick::search_ba );
		c.budget   = &budget;
		c.max_sims = 512;

		static int resume     = 0;
		constexpr int n_progs = static_cast< int >( std::size( n_assist::k_programs ) );
		int last              = -1;
		char miss[ 384 ]{ };
		int miss_len = 0;
		std::vector< n_assist::target_t > targets;
		targets.reserve( max_points );
		bool landing_in_reach = on_ground;
		if ( !on_ground ) {
			const n_assist::run_t probe = n_assist::run( c, n_assist::program_t{ n_assist::launch_jump, false }, { }, false );
			c.jb_tick                   = probe.landing;
			landing_in_reach            = probe.landing >= 0 || !probe.complete;
		}
		for ( int visit = 0; visit < n_progs && landing_in_reach && !plan.active && !c.out_of_budget; ++visit ) {
			last                           = ( resume + visit ) % n_progs;
			const n_assist::program_t& prog = n_assist::k_programs[ last ];
			if ( prog.launch == n_assist::launch_jumpbug && on_ground )
				continue;

			targets.clear( );
			for ( int ci = 0; ci < cand_count; ++ci ) {
				for ( const n_assist::flag_t& f : n_assist::k_flags ) {
					if ( n_assist::same( f.program, prog ) && cands[ ci ].*f.flag &&
					     !is_bind_restricted_in_radius( cands[ ci ], origin, f.bind ) ) {
						targets.push_back( n_assist::make_target( cands[ ci ].pos.m_z, ci ) );
						break;
					}
				}
			}
			if ( targets.empty( ) )
				continue;

			n_assist::run_t r = n_assist::run( c, prog, targets, false );
			if ( !r.hit ) {
				if ( r.gap < 1e8f && miss_len < static_cast< int >( sizeof( miss ) ) - 32 )
					miss_len += sprintf_s( miss + miss_len, sizeof( miss ) - miss_len, " %s%s=%.3f%s", n_assist::k_launch_names[ prog.launch ],
					                       prog.crouch ? "(c)" : "", r.gap, r.complete ? "" : "(cut)" );
				continue;
			}

			const int t = r.target;
			if ( !r.exact ) {
				const std::vector< n_assist::target_t > one{ targets[ t ] };
				n_assist::run_t v = n_assist::run( c, prog, one, true );
				if ( !v.hit ) {
					botox_dbg_log( "[bsa] verify miss %s%s bang %.4f stepped k=%d complete=%d", n_assist::k_launch_names[ prog.launch ],
					               prog.crouch ? " crouch" : "", targets[ t ].z, r.arrival, v.complete ? 1 : 0 );
					continue;
				}
				r = std::move( v );
			}

			plan.take( prog, r, cmd->m_tick_count, targets[ t ].point, targets[ t ].z );
			ended = false;
			++st_plans;
			botox_dbg_log( "[bsa] plan %s%s bang %.4f launch k=%d arrival k=%d sims=%d fwd=%.0f side=%.0f", n_assist::k_launch_names[ prog.launch ],
			               prog.crouch ? " crouch" : "", plan.target_z, plan.launch, plan.arrival, c.sims, live_fwd, live_side );
			if ( GET_VARIABLE( g_variables.m_bouncee_assist_render, bool ) ) {
				if ( const unsigned long long now = GetTickCount64( ); now >= render_after ) {
					render_after = now + 500ull;
					movement_add_window( 5, std::string( n_assist::k_launch_names[ prog.launch ] )
					                            .append( " to head bounce " )
					                            .append( std::to_string( plan.target_z ) )
					                            .append( " with " )
					                            .append( std::to_string( g_prediction.backup_data.m_origin.m_z ) )
					                            .append( prog.crouch ? " (crouch)" : " (stand)" ) );
				}
			}
		}
		resume = c.out_of_budget && last >= 0 ? last : 0;

		cmd->m_buttons      = c.buttons;
		cmd->m_forward_move = live_fwd;
		cmd->m_side_move    = live_side;
		cmd->m_view_point   = live_view;
		g_prediction.restore_entity_to_predicted_frame( c.frame );

		/* the last chance before a press found nothing: closest head gap per program */
		if ( !plan.active && ( on_ground || c.jb_tick == 0 ) && miss_len )
			botox_dbg_log( "[bsa] no arc%s z=%.3f bang=%.4f sims=%d%s", on_ground ? " (ground)" : " (landing)", origin.m_z, cands[ 0 ].pos.m_z, c.sims, miss );

		++st_calls;
		st_sims += c.sims;
		st_us += budget.used_us( );
	}

	if ( !plan.active )
		return;

	/* jump / duck only: your move and view go through untouched */
	HITGODA2        = true;
	const int k     = cmd->m_tick_count - plan.start_tick;
	const int press = k <= plan.arrival ? plan.ticks[ k ].buttons : ( plan.ticks.back( ).buttons & in_duck );
	cmd->m_buttons  = ( cmd->m_buttons & ~( in_jump | in_duck ) ) | press;
}
