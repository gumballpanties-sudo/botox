#include "movement.h"
#include "assist_predict.h"
#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"
#include "../prediction/prediction.h"
#include "tick_scale.h"
#include <cmath>
#include <iterator>
#include <numbers>
#include <vector>

extern void botox_dbg_log( const char* fmt, ... );

bool HITGODA2 = false;

void n_movement::impl_t::autobounce_assist( c_user_cmd* cmd )
{
	static n_assist::plan_t plan;
	static c_angle last_view{ };
	static unsigned long long render_after = 0ull;

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
		stop( );
		return;
	}

	const c_angle live_view = cmd->m_view_point;
	const float live_fwd    = cmd->m_forward_move;
	const float live_side   = cmd->m_side_move;
	const c_angle turn      = live_view - last_view;
	last_view               = live_view;

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
	for ( int i = 0; i < static_cast< int >( m_bounce_points_check.size( ) ); i++ )
		consider( m_bounce_points_check[ i ] );
	for ( int i = 0; i < static_cast< int >( m_points_check.size( ) ); i++ ) {
		if ( m_points_check[ i ].is_free_point && m_points_check[ i ].point_type == 1 )
			consider( m_points_check[ i ] );
	}

	const c_angle delta = cand_count > 0 ? turn * cands[ 0 ].delta_strafe : c_angle{ };

	if ( !plan.active ) {
		HITGODA2 = false;
		if ( cand_count == 0 ) {
			static uint64_t last_unreachable_msg_bounce = 0;
			if ( const uint64_t now = GetTickCount64( ); now - last_unreachable_msg_bounce > 3000 ) {
				if ( GET_VARIABLE( g_variables.m_bouncee_assist_render, bool ) )
					movement_add_window( 3.0f, "[-] point unreachable" );
				last_unreachable_msg_bounce = now;
			}
			return;
		}

		/* the restored frame = where the sims start ( backup_data can be a cmd stale ) */
		const bool on_ground = ( g_ctx.m_local->get_flags( ) & fl_onground ) != 0;
		if ( !GET_VARIABLE( g_variables.m_bouncee_assist_brokehop, bool ) && on_ground && g_ctx.m_local->get_stamina( ) != 0.f )
			return;
		/* old gate: never plans off a crouch already in */
		if ( g_ctx.m_local->get_flags( ) & fl_ducking )
			return;

		n_assist::ctx_t c;
		c.kind      = n_assist::arrival_bang;
		c.frame     = g_interfaces.m_prediction->m_commands_predicted - 1;
		c.buttons   = cmd->m_buttons;
		c.view      = live_view;
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
			if ( !r.hit )
				continue;

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
			botox_dbg_log( "[bsa] plan %s%s bang %.4f launch k=%d arrival k=%d sims=%d", n_assist::k_launch_names[ prog.launch ],
			               prog.crouch ? " crouch" : "", plan.target_z, plan.launch, plan.arrival, c.sims );
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
	}

	if ( !plan.active )
		return;

	const int k = cmd->m_tick_count - plan.start_tick;
	if ( k > plan.arrival + 2 || ( k <= plan.arrival && !n_assist::on_track( plan, k ) ) ) {
		if ( k <= plan.arrival )
			botox_dbg_log( "[bsa] diverge %s%s k=%d/%d", n_assist::k_launch_names[ plan.program.launch ], plan.program.crouch ? " crouch" : "", k,
			               plan.arrival );
		stop( );
		return;
	}

	HITGODA2 = true;
	const int press = k <= plan.arrival ? plan.ticks[ k ].buttons : ( plan.ticks.back( ).buttons & in_duck );
	cmd->m_buttons  = ( cmd->m_buttons & ~( in_jump | in_duck ) ) | press;

	if ( k < plan.launch ) {
		cmd->m_forward_move  = 0.f;
		cmd->m_side_move     = 0.f;
		cmd->m_mouse_delta_x = 0;
		cmd->m_mouse_delta_y = 0;
		n_assist::set_move( cmd, 0.f, 0.f );
		return;
	}

	auto colidable = g_ctx.m_local->get_collideable( );
	trace_t trace;
	const float step = std::numbers::pi_v< float > * 2.0f / 16.f;
	for ( float a = 0.f; a < std::numbers::pi_v< float > * 2.0f; a += step ) {
		c_vector wishdir     = c_vector( cosf( a ), sinf( a ), 0.f );
		const auto start_pos = g_ctx.m_local->get_abs_origin( );
		const auto end_pos   = start_pos + wishdir;
		c_trace_filter flt( g_ctx.m_local );
		ray_t ray( start_pos, end_pos, colidable->get_obb_mins( ), colidable->get_obb_maxs( ) );
		g_interfaces.m_engine_trace->trace_ray( ray, mask_playersolid, &flt, &trace );
		if ( trace.m_fraction < 1.f && trace.m_plane.m_normal.m_z == 0.f ) {
			c_vector NormalPlane = c_vector( trace.m_plane.m_normal.m_x * -1.f, trace.m_plane.m_normal.m_y * -1.f, 0.f );
			c_angle WallAngle    = NormalPlane.to_angle( );
			float rotation       = deg2rad( WallAngle.m_y - ( cmd->m_view_point.m_y + delta.m_y ) );
			float cos_rot        = cos( rotation );
			float sin_rot        = sin( rotation );
			float multiplier     = 6.f;
			float forwardmove    = cos_rot * multiplier;
			float sidemove       = -sin_rot * multiplier;
			cmd->m_forward_move  = forwardmove;
			cmd->m_side_move     = sidemove;
		}
	}
}
