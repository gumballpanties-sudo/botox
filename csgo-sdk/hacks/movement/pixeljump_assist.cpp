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

bool HITGODA3 = false;

/* lumi "pixel climb" ( FUN_100f6220 ) on assist_predict: at a press tick fly every program with your keys, the first arc whose
   landing tick comes down inside the point's radius is the plan. jump / duck replayed only, move + view stay yours
   ( lumi's side / yaw fan skipped: assists keep the user's move ) */
void n_movement::impl_t::pixeljump_assist( c_user_cmd* cmd )
{
	static n_assist::plan_t plan;
	static unsigned long long render_after = 0ull;
	static long long st_calls = 0ll, st_sims = 0ll, st_plans = 0ll, st_diverge = 0ll, st_us = 0ll;

	const auto stop = [ & ]( ) {
		plan.clear( );
		HITGODA3 = false;
	};

	if ( !GET_VARIABLE( g_variables.m_pixel_jump_assist, bool ) || !g_ctx.m_local || !g_interfaces.m_engine_client->is_in_game( ) ||
	     !g_ctx.m_local->is_alive( ) || !g_ctx.m_local->get_collideable( ) ) {
		stop( );
		return;
	}
	if ( !g_input.check_input( &GET_VARIABLE( g_variables.m_pixel_jump_assist_key, key_bind_t ) ) ) {
		stop( );
		return;
	}

	const c_angle live_view = cmd->m_view_point;
	const float live_fwd    = cmd->m_forward_move;
	const float live_side   = cmd->m_side_move;
	const bool on_ground    = ( g_ctx.m_local->get_flags( ) & fl_onground ) != 0;
	const float radius      = std::max( 1.f, GET_VARIABLE( g_variables.m_pixel_jump_assist_radius, float ) );

	/* pixeljump points = bounce list entries of point_type 2 ( placed off the pixel finder's yellow dots ), 2d radius, nearest first
	   ( copies: menu thread owns the vector ) */
	constexpr int max_points = 8;
	const std::string map    = g_interfaces.m_engine_client->get_level_name_short( );
	const c_vector origin    = g_ctx.m_local->get_origin( );
	points_check_t cands[ max_points ];
	float cand_dist[ max_points ]{ };
	int cand_count = 0;
	for ( int i = 0; i < static_cast< int >( m_bounce_points_check.size( ) ); i++ ) {
		const points_check_t& p = m_bounce_points_check[ i ];
		if ( p.point_type != 2 || p.map != map || !p.active )
			continue;
		const float d = origin.dist_to_2d( p.pos );
		if ( d > p.radius )
			continue;
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
	}

	if ( plan.active ) {
		const int k = cmd->m_tick_count - plan.start_tick;
		if ( k <= plan.arrival && !n_assist::on_track( plan, k ) ) {
			++st_diverge;
			botox_dbg_log( "[pja] diverge %s%s k=%d/%d dz=%.3f gnd=%d", n_assist::k_launch_names[ plan.program.launch ], plan.program.crouch ? " crouch" : "",
			               k, plan.arrival, k >= 0 && k < static_cast< int >( plan.ticks.size( ) ) ? n_assist::start_dz( plan, k ) : 0.f, on_ground ? 1 : 0 );
			plan.clear( );
		} else if ( k > plan.arrival + 2 ) {
			const float d = plan.point >= 0 && plan.point < cand_count ? origin.dist_to_2d( cands[ plan.point ].pos ) : -1.f;
			botox_dbg_log( "[pja] end %s%s %s z=%.3f top=%.3f d=%.2f", n_assist::k_launch_names[ plan.program.launch ], plan.program.crouch ? " crouch" : "",
			               on_ground && std::fabs( origin.m_z - plan.target_z ) <= 0.5f ? "landed" : "missed", origin.m_z, plan.target_z, d );
			plan.clear( );
		}
	}

	if ( !plan.active ) {
		HITGODA3 = false;
		if ( cand_count == 0 ) {
			static uint64_t last_unreachable_msg = 0;
			if ( const uint64_t now = GetTickCount64( ); now - last_unreachable_msg > 3000 ) {
				if ( GET_VARIABLE( g_variables.m_pixel_jump_assist_render, bool ) )
					movement_add_window( 3.0f, "[-] point unreachable" );
				last_unreachable_msg = now;
			}
			return;
		}
		const int raw_ticks = GET_VARIABLE( g_variables.m_pixel_jump_assist_ticks, int );
		if ( raw_ticks <= 0 )
			return;

		n_assist::ctx_t c;
		c.kind      = n_assist::arrival_land;
		c.frame     = g_interfaces.m_prediction->m_commands_predicted - 1;
		c.buttons   = cmd->m_buttons;
		c.fwd       = c.fwd0 = live_fwd;
		c.side      = c.side0 = live_side;
		c.view      = live_view;
		c.pre_ticks = std::clamp( n_tick::ticks( raw_ticks ), 1, 256 );
		c.air_ticks = n_tick::ticks( 128 );
		c.lj_hold   = n_tick::ticks( 3 );
		c.gravity   = g_convars.float_or( HASH_BT( "sv_gravity" ), 800.f );
		c.lowest    = cands[ 0 ].pos.m_z;
		for ( int i = 1; i < cand_count; ++i )
			c.lowest = std::min( c.lowest, static_cast< double >( cands[ i ].pos.m_z ) );
		n_tick::c_sim_budget budget;
		budget.start( 0.45f, 0.f, n_tick::search_pja );
		c.budget   = &budget;
		c.max_sims = 768;

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
					if ( n_assist::same( f.program, prog ) && cands[ ci ].*f.flag && !is_bind_restricted_in_radius( cands[ ci ], origin, f.bind ) ) {
						targets.push_back( n_assist::make_land_target( cands[ ci ].pos, radius, ci ) );
						break;
					}
				}
			}
			if ( targets.empty( ) )
				continue;

			/* land arcs are full engine flights: a hit is exact, no verify pass */
			n_assist::run_t r = n_assist::run( c, prog, targets, true );
			if ( !r.hit ) {
				if ( r.gap < 1e8f && miss_len < static_cast< int >( sizeof( miss ) ) - 32 )
					miss_len += sprintf_s( miss + miss_len, sizeof( miss ) - miss_len, " %s%s=%.2f%s", n_assist::k_launch_names[ prog.launch ],
					                       prog.crouch ? "(c)" : "", r.gap + radius, r.complete ? "" : "(cut)" );
				continue;
			}

			const int t = r.target;
			plan.take( prog, r, cmd->m_tick_count, targets[ t ].point, targets[ t ].z );
			++st_plans;
			botox_dbg_log( "[pja] plan %s%s top %.3f d=%.2f/%.1f launch k=%d arrival k=%d sims=%d", n_assist::k_launch_names[ prog.launch ],
			               prog.crouch ? " crouch" : "", plan.target_z, r.gap + radius, radius, plan.launch, plan.arrival, c.sims );
			if ( GET_VARIABLE( g_variables.m_pixel_jump_assist_render, bool ) ) {
				if ( const unsigned long long now = GetTickCount64( ); now >= render_after ) {
					render_after = now + 500ull;
					movement_add_window( 5, std::string( n_assist::k_launch_names[ prog.launch ] )
					                            .append( " to pixeljump " )
					                            .append( std::to_string( plan.target_z ) )
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

		if ( !plan.active && ( on_ground || c.jb_tick == 0 ) && miss_len )
			botox_dbg_log( "[pja] no arc%s z=%.3f top=%.3f sims=%d%s", on_ground ? " (ground)" : " (landing)", origin.m_z, cands[ 0 ].pos.m_z, c.sims, miss );

		++st_calls;
		st_sims += c.sims;
		st_us += budget.used_us( );
	}

	static unsigned long long next_report = 0ull;
	if ( const unsigned long long now = GetTickCount64( ); now >= next_report ) {
		next_report = now + 1000ull;
		if ( st_calls || st_plans || st_diverge )
			botox_dbg_log( "PJA: calls=%lld sims=%lld avg=%.1f us=%lld plans=%lld diverge=%lld", st_calls, st_sims,
			               st_calls ? static_cast< float >( st_sims ) / static_cast< float >( st_calls ) : 0.f, st_calls ? st_us / st_calls : 0ll, st_plans,
			               st_diverge );
		st_calls = st_sims = st_plans = st_diverge = st_us = 0ll;
	}

	if ( !plan.active )
		return;

	/* jump / duck only: your move and view go through untouched */
	HITGODA3        = true;
	const int k     = cmd->m_tick_count - plan.start_tick;
	const int press = k <= plan.arrival ? plan.ticks[ k ].buttons : ( plan.ticks.back( ).buttons & in_duck );
	cmd->m_buttons  = ( cmd->m_buttons & ~( in_jump | in_duck ) ) | press;
}
