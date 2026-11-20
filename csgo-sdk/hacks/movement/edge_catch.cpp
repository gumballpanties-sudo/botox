#include "edge_catch.h"
#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"
#include "../prediction/prediction.h"
#include "assist_predict.h"
#include "edge_skip.h"
#include "edgebug.h"
#include "wall_climb.h"

extern bool g_air_stuck_owns_cmd;
extern bool g_air_align_owns;
extern int g_jb_duck_cmd;
bool ps_latched( );
bool bind_cmd_taken( );
void start_movement_fix( c_user_cmd* cmd );
void end_movement_fix( c_user_cmd* cmd );
extern void botox_dbg_log( const char* fmt, ... );

namespace {
	constexpr int k_max_sims = 1024;
	constexpr int k_max_snap = 30; /* per side: slots 1..30 / 31..60, 62 / 63 = wall climb spot job */
	constexpr int k_jb_slot  = 61; /* baseline: START of the tick being simmed ( jb crouch re-sim ) */

	bool jb_bind( )
	{
		return GET_VARIABLE( g_variables.m_jump_bug, bool ) && g_input.check_input( &GET_VARIABLE( g_variables.m_jump_bug_key, key_bind_t ) );
	}

	void restore_start( )
	{
		g_prediction.restore_entity_to_predicted_frame( g_interfaces.m_prediction->m_commands_predicted - 1 );
	}

	bool slope_under( const c_vector& at, const bool ducked )
	{
		c_trace_filter filter( g_ctx.m_local );
		ray_t ray( at, c_vector( at.m_x, at.m_y, at.m_z - 2.f ), c_vector( -16.f, -16.f, 0.f ), c_vector( 16.f, 16.f, ducked ? 54.f : 72.f ) );
		trace_t tr;
		g_interfaces.m_engine_trace->trace_ray( ray, mask_playersolid, &filter, &tr );
		return tr.m_fraction < 1.f && tr.m_plane.m_normal.m_z > 0.f && tr.m_plane.m_normal.m_z < 0.7f;
	}

	/* probes move at the live leaf list's speed ( begin reads m_bounds_max_speed for non-real cmds ) */
	struct bounds_guard_t {
		bounds_guard_t( ) { g_prediction.m_bounds_max_speed = g_prediction.m_real_max_speed; }
		~bounds_guard_t( ) { g_prediction.m_bounds_max_speed = 0.f; }
	};

	/* z = hull resting on the standable floor within 32 u below, same frame as baseline's floor z */
	bool floor_under( const c_vector& at, const bool ducked, float& z )
	{
		c_trace_filter filter( g_ctx.m_local );
		ray_t ray( at, c_vector( at.m_x, at.m_y, at.m_z - 32.f ), c_vector( -16.f, -16.f, 0.f ), c_vector( 16.f, 16.f, ducked ? 54.f : 72.f ) );
		trace_t tr;
		g_interfaces.m_engine_trace->trace_ray( ray, mask_playersolid, &filter, &tr );
		z = tr.m_end.m_z;
		return !tr.m_start_solid && tr.m_fraction < 1.f && tr.m_plane.m_normal.m_z >= 0.7f;
	}
}

void n_edge_catch::impl_t::reset( const bool forget )
{
	m_active = false;
	m_hit    = 0;
	m_fail   = false;
	m_jb_out = false;
	if ( forget )
		m_yaw_valid = m_had_ground = m_dropped = false;
}

void n_edge_catch::impl_t::render( ) const
{
	if ( !m_active || !GET_VARIABLE( g_variables.m_edge_catch_visualize, bool ) )
		return;
	const n_edgebug::viz_vars_t style{ g_variables.m_edge_catch_visualize_color, g_variables.m_edge_catch_visualize_thickness,
		                               g_variables.m_edge_catch_visualize_shape, g_variables.m_edge_catch_visualize_fill,
		                               g_variables.m_edge_catch_visualize_fill_alpha, g_variables.m_edge_catch_visualize_pad_color,
		                               g_variables.m_edge_catch_visualize_projected, g_variables.m_edge_catch_visualize_size };
	n_edgebug::draw_plan( m_viz, m_last + 2, m_hit, m_land_pos, 1.f, &style );
}

void n_edge_catch::impl_t::pre( c_user_cmd* cmd )
{
	const auto local = g_ctx.m_local;
	if ( !cmd || !local || !local->is_alive( ) ) {
		m_yaw_valid = false;
		return;
	}
	m_start_flags     = local->get_flags( );
	m_start_move_type = local->get_move_type( );
	m_start_vel       = local->get_velocity( );
	m_eb_latched      = g_edgebug.m_found;
	const bool air = !( m_start_flags & fl_onground );
	if ( !air && m_start_move_type == move_type_walk ) {
		m_takeoff_z  = local->get_origin( ).m_z;
		m_had_ground = true;
	}
	/* airborne rise after a fall = jumpbug / bounce: that floor is the new takeoff ( rising search would pull you back onto it ) */
	float jb_z = 0.f;
	if ( air && m_yaw_valid && m_start_vel.m_z > 0.f && m_prev_vz <= 0.f && floor_under( local->get_origin( ), ( m_start_flags & fl_ducking ) != 0, jb_z ) ) {
		m_takeoff_z  = jb_z;
		m_had_ground = true;
	}
	m_prev_vz = air ? m_start_vel.m_z : 1.f;
	const float yaw   = rad2deg( atan2f( m_start_vel.m_y, m_start_vel.m_x ) );
	m_turn_left       = m_yaw_valid && std::remainderf( yaw - m_prev_yaw, 360.f ) > 0.f;
	m_prev_yaw        = yaw;
	m_yaw_valid       = true;
}

void n_edge_catch::impl_t::apply( c_user_cmd* cmd, const tick_t& t, const c_angle& view ) const
{
	cmd->m_forward_move   = 0.f;
	cmd->m_side_move      = t.side;
	cmd->m_view_point     = view;
	cmd->m_view_point.m_y = t.yaw;
	start_movement_fix( cmd );
	cmd->m_view_point = view;
	end_movement_fix( cmd );
	n_assist::set_move( cmd, cmd->m_forward_move, cmd->m_side_move );
}

void n_edge_catch::impl_t::sim( c_user_cmd* cmd, const int index, const tick_t& t, const int buttons )
{
	const auto local    = g_ctx.m_local;
	cmd->m_buttons      = t.duck ? buttons | in_duck : buttons;
	cmd->m_view_point   = m_view;
	cmd->m_forward_move = m_fwd0;
	cmd->m_side_move    = m_side0;
	if ( t.strafe )
		apply( cmd, t, m_view );
	local->get_tick_base( ) = m_tick_base0 + index;
	g_prediction.begin( local, cmd, false, true );
	g_prediction.end( local );
	m_sims++;
}

bool n_edge_catch::impl_t::out( ) const
{
	return m_sims >= k_max_sims || m_budget.expired( );
}

bool n_edge_catch::impl_t::landed( ) const
{
	return ( g_ctx.m_local->get_flags( ) & fl_onground ) != 0;
}

int n_edge_catch::impl_t::strafed( ) const
{
	int n = 0;
	for ( int i = 0; i <= m_last; i++ )
		n += m_plan[ i ].strafe ? 1 : 0;
	return n;
}

/* your keys as sent: kind 1 = vz rises airborne ( edgebug, lip = END z ), kind 2 = a standable floor within reach of the hull
   passed this tick without landing and the fall never lands on it. a floor purely behind the hull at takeoff height = the
   platform you just left, never a target */
bool n_edge_catch::impl_t::baseline( c_user_cmd* cmd )
{
	const auto local = g_ctx.m_local;
	restore_start( );
	c_trace_filter filter( local );
	float prev_vz = local->get_velocity( ).m_z;
	int cand_k    = -1;
	float cand_z  = 0.f;
	/* jb only: jb_will_work ( same model as jb_works ) vetoes the crouch where the jb can't fire, plain landings stand */
	const bool crouch = m_jb && !( GET_VARIABLE( g_variables.m_jump_bug_only, bool ) && g_prediction.snapshot_slots( ) > 0 );
	for ( int k = 0; k < m_max_ticks; k++ ) {
		if ( out( ) )
			return false;
		const c_vector s  = local->get_origin( );
		const bool ducked = ( local->get_flags( ) & fl_ducking ) != 0;
		/* jb crouch: this tick's START kept for the re-sim ( tick 0 = restore_start ) */
		const bool snap = crouch && !ducked && ( k == 0 || g_prediction.snapshot_save( k_jb_slot ) );
		tick_t t;
		t.origin = s;
		sim( cmd, k, t, m_buttons0 );
		if ( landed( ) ) {
			const float land_z = local->get_origin( ).m_z;
			if ( cand_k >= 0 && local->get_origin( ).m_z < cand_z - 2.f ) {
				m_kind = 2, m_miss = cand_k, m_ground_z = cand_z, m_miss_vz = local->get_velocity( ).m_z;
				return true;
			}
			/* jump_bug crouches this landing tick ( feet + 9 ), one more tick of travel: on the very edge its unduck misses */
			if ( snap && jb_drops( cmd, k, land_z ) ) {
				m_kind = 3, m_miss = k, m_ground_z = land_z, m_miss_vz = prev_vz;
				return true;
			}
			return false;
		}
		if ( local->get_move_type( ) != move_type_walk )
			return false;
		const c_vector e = local->get_origin( );
		const float vz   = local->get_velocity( ).m_z;
		/* vz > 0 = your own jumpbug / bounce ( edgebug.cpp drops it too ), not an eb */
		if ( vz > prev_vz ) {
			if ( vz <= 0.f && cand_k < 0 && !slope_under( e, ducked ) ) {
				m_kind = 1, m_miss = k, m_ground_z = e.m_z, m_miss_vz = vz;
				return true;
			}
			return false;
		}
		if ( cand_k < 0 && vz <= 0.f && e.m_z < s.m_z ) {
			const c_vector mins( -16.f - m_reach, -16.f - m_reach, 0.f ), maxs( 16.f + m_reach, 16.f + m_reach, ducked ? 54.f : 72.f );
			const auto floor_at = [ & ]( const c_vector& at, float& z ) {
				ray_t ray( at, c_vector( at.m_x, at.m_y, e.m_z - 0.5f ), mins, maxs );
				trace_t tr;
				g_interfaces.m_engine_trace->trace_ray( ray, mask_playersolid, &filter, &tr );
				z = tr.m_end.m_z;
				return !tr.m_start_solid && tr.m_fraction < 1.f && tr.m_plane.m_normal.m_z >= 0.7f && tr.m_end.m_z > e.m_z + 0.03f;
			};
			float z_all = 0.f, z_fwd = 0.f;
			if ( floor_at( s, z_all ) ) {
				const c_vector vel = local->get_velocity( );
				const float vxy    = vel.length_2d( );
				const c_vector ahead = vxy > 1.f ? s + c_vector( vel.m_x / vxy * m_reach, vel.m_y / vxy * m_reach, 0.f ) : s;
				const bool behind_only = !floor_at( ahead, z_fwd ) || std::fabs( z_fwd - z_all ) > 2.f;
				if ( !behind_only || !m_had_ground || std::fabs( z_all - m_takeoff_z ) > 2.f ) {
					cand_k = k;
					cand_z = z_all;
				}
			}
		}
		prev_vz = vz;
	}
	if ( cand_k >= 0 ) {
		m_kind = 2, m_miss = cand_k, m_ground_z = cand_z, m_miss_vz = local->get_velocity( ).m_z;
		return true;
	}
	return false;
}

/* jb bind on: the jump bug decides its landing duck on your unstrafed keys, so it never sees a catch's landing. the plan ducks
   tick k itself and needs the jb to work there ( jb_will_work: duck on k stays airborne, unduck + jump on k + 1 rises ).
   ducked START = crouched fall, plain landing */
bool n_edge_catch::impl_t::jb_works( c_user_cmd* cmd, const int k, const int from, const int base )
{
	const auto local = g_ctx.m_local;
	restore_start( );
	if ( from > 0 )
		g_prediction.snapshot_load( base + from );
	for ( int i = from; i < k; i++ ) {
		if ( out( ) )
			return false;
		sim( cmd, i, m_plan[ i ], m_buttons0 );
	}
	if ( local->get_flags( ) & fl_ducking )
		return true;
	sim( cmd, k, m_plan[ k ], ( m_buttons0 | in_duck ) & ~in_jump );
	if ( landed( ) )
		return false;
	sim( cmd, k + 1, tick_t{ }, ( m_buttons0 & ~in_duck ) | in_jump );
	m_jb_plan = !landed( ) && local->get_velocity( ).m_z > 0.f;
	return m_jb_plan;
}

/* your keys land on tick k ( k_jb_slot = its START ). true = jump_bug's crouch there neither lands ducked nor jbs, and the fall
   after it ends a step height under your plain landing ( carried off the edge; a downslope / small step under it isn't a lost lip ) */
bool n_edge_catch::impl_t::jb_drops( c_user_cmd* cmd, const int k, const float land_z )
{
	constexpr float k_step = 18.f;
	const auto local       = g_ctx.m_local;
	m_plan[ k ]            = tick_t{ };
	const bool jb          = jb_works( cmd, k, k, k_jb_slot - k );
	m_jb_plan              = false;
	if ( jb )
		return false;
	for ( int i = k + 2; i < k_max_ticks && !landed( ) && !out( ) && local->get_move_type( ) == move_type_walk &&
	                     local->get_origin( ).m_z >= land_z - k_step;
	      i++ )
		sim( cmd, i, tick_t{ }, m_buttons0 & ~in_duck );
	return local->get_origin( ).m_z < land_z - k_step;
}

/* the plan's landing tick, this cmd as it goes out, from the exact START ( no plan drift ): crouch stays airborne, unduck + jump
   next tick rises. same model as jb_works / jb_will_work */
bool n_edge_catch::impl_t::live_jb( c_user_cmd* cmd )
{
	const auto local = g_ctx.m_local;
	bounds_guard_t bounds_guard;
	c_user_cmd probe = *cmd;
	restore_start( );
	if ( local->get_flags( ) & fl_ducking )
		return false;
	const int tick_base = local->get_tick_base( );
	probe.m_buttons     = ( cmd->m_buttons | in_duck ) & ~in_jump;
	g_prediction.begin( local, &probe, false, true );
	g_prediction.end( local );
	bool ok = !landed( );
	if ( ok ) {
		probe.m_buttons         = ( cmd->m_buttons & ~in_duck ) | in_jump;
		local->get_tick_base( ) = tick_base + 1;
		g_prediction.begin( local, &probe, false, true );
		g_prediction.end( local );
		ok = !landed( ) && local->get_velocity( ).m_z > 0.f;
	}
	local->get_tick_base( ) = tick_base;
	restore_start( );
	return ok;
}

/* strafe ticks [ 0, prefix ) at the perfect angle, your keys after. save = full chain, slot base + k + 1 = START of tick k + 1,
   m_seed = its ticks ( a prefix chain starts mid-plan, ticks below prefix come from there ).
   0 = no landing on the lip, 2 = lands ( jb bind off / jb fires there ), 1 = lands but the jb can't fire = plain landing fallback */
int n_edge_catch::impl_t::chain( c_user_cmd* cmd, const int prefix, const bool neg, const bool save, const int base )
{
	const auto local = g_ctx.m_local;
	const int side   = base / k_max_snap;
	restore_start( );
	int k = 0;
	if ( !save ) {
		g_prediction.snapshot_load( base + prefix );
		std::copy_n( m_seed[ side ], prefix, m_plan );
		k = prefix;
	}
	m_chains++;
	for ( ; k < m_max_ticks; k++ ) {
		if ( out( ) )
			return 0;
		tick_t& t = m_plan[ k ];
		t.origin  = local->get_origin( );
		t.strafe  = k < prefix;
		t.duck    = m_duck;
		if ( t.strafe ) {
			const c_vector vel  = local->get_velocity( );
			const float ang     = n_edge_skip::perfect_angle( vel, local->get_surface_friction( ), local->get_max_speed( ) );
			const float vel_yaw = rad2deg( atan2f( vel.m_y, vel.m_x ) );
			/* brake: side -450 = wish at yaw + 90 = straight back, air accel caps it at ~5.6 u/s per tick ( forward travel -10 u / 15 ticks ) */
			t.yaw               = m_brake ? vel_yaw + 90.f : neg ? vel_yaw - ang : vel_yaw + ang;
			t.side              = m_brake || neg ? -450.f : 450.f;
		}
		if ( save && k < k_max_snap )
			m_seed[ side ][ k ] = t;
		sim( cmd, k, t, m_buttons0 );
		if ( save && k + 1 <= k_max_snap && g_prediction.snapshot_save( base + k + 1 ) )
			m_saved[ side ] = k + 1;
		if ( landed( ) ) {
			if ( local->get_origin( ).m_z < m_ground_z - 2.f )
				return 0;
			m_last     = k;
			m_land_pos = local->get_origin( );
			m_jb_plan  = false;
			return !m_jb || jb_works( cmd, k, save ? std::min( k, m_saved[ side ] ) : prefix, base ) ? 2 : 1;
		}
		if ( local->get_move_type( ) != move_type_walk )
			return 0;
		if ( local->get_origin( ).m_z + 9.f < m_ground_z - 2.f && local->get_velocity( ).m_z <= 0.f )
			return 0;
	}
	return 0;
}

/* full strafe chains per side seed the snapshots and anchor the search: landing is near monotone in the strafe count ( more
   ticks = more lateral pull ), so a side whose full chain lands bisects for its shortest landing prefix, turn side first, the
   other side only below it. every landing shorter than the best so far is copied out ( no re-run, budget-safe ).
   11-12 log: the linear 1..miss+2 scan hit the 3.9 ms budget on 8/20 misses ( ~300 sims ), bisect = ~5 chains per side */
bool n_edge_catch::impl_t::family( c_user_cmd* cmd, const bool first_neg )
{
	/* kind 3: your keys already land plain, only a catch the jb fires on is worth a strafe */
	const int need = m_kind == 3 ? 2 : 1;
	/* duck lift pass: duck alone ( no strafe ) first */
	if ( m_duck ) {
		m_saved[ 0 ] = 0;
		if ( chain( cmd, 0, first_neg, true, 0 ) >= need )
			return true;
	}
	int best_n = 0, best_s = -1;
	const auto keep = [ & ]( const int n, const int s ) {
		best_n = n, best_s = s;
		std::copy_n( m_plan, m_last + 1, m_best );
		m_best_last = m_last;
		m_best_land = m_land_pos;
		m_best_jb   = m_jb_plan;
	};
	const int n_max = std::min( m_miss + 2, m_max_ticks - 1 );
	/* brake wishes straight back: both sides = the same chain, run one */
	const int sides = m_brake ? 1 : 2;
	int full[ 2 ]{ }, n_full[ 2 ]{ };
	bool full_jb[ 2 ]{ };
	for ( int s = 0; s < sides; s++ ) {
		m_saved[ s ] = 0;
		full[ s ]    = chain( cmd, m_max_ticks, ( s == 0 ) == first_neg, true, s * k_max_snap );
		full_jb[ s ] = full[ s ] == 2 && m_jb_plan;
		/* prefix count the full chain stands for ( strafes every tick up to the landing ) */
		n_full[ s ] = m_saved[ s ] > 0 ? std::min( n_max, m_saved[ s ] - 1 ) + 1 : 1;
		if ( full[ s ] >= need && ( best_s < 0 || n_full[ s ] < best_n ) )
			keep( n_full[ s ], s );
	}
	for ( int s = 0; s < sides && !out( ); s++ ) {
		const bool neg = ( s == 0 ) == first_neg;
		const int base = s * k_max_snap;
		int lo = 0, hi = n_full[ s ];
		if ( full[ s ] < need ) {
			/* full pull passed the ledge ( narrow ): anchors 3/4, 1/2, 1/4 of it, first landing bisects below. snapshots exist up to n_full */
			hi = 0;
			for ( int q = 3, prev = n_full[ s ]; q >= 1 && !hi && !out( ); q-- ) {
				const int n = n_full[ s ] * q / 4;
				if ( n < 1 || n == prev || ( best_s >= 0 && n >= best_n ) )
					continue;
				prev = n;
				if ( chain( cmd, n, neg, false, base ) >= need )
					hi = n, m_anchors++;
			}
			if ( !hi )
				continue;
			keep( hi, s );
		} else if ( best_s != s ) {
			/* the other side already landed shorter: only a strictly shorter prefix here matters */
			hi = best_n - 1;
			if ( hi < 1 || chain( cmd, hi, neg, false, base ) < need )
				continue;
			keep( hi, s );
		}
		while ( hi - lo > 1 && !out( ) ) {
			const int mid = ( lo + hi ) / 2;
			if ( chain( cmd, mid, neg, false, base ) >= need )
				keep( hi = mid, s );
			else
				lo = mid;
		}
	}
	/* jb bind, kept catch lands plain: the jump bug's crouch lifts the feet 9 u for one more tick of travel, so a catch it fires on
	   lands deeper = more strafe. a side whose full chain jbs bisects below it ( hi = the full chain, prefixes < n_full only: slot
	   n_full = the grounded END ), nothing shorter = re-run the full chain ( its plan was overwritten ). the plain landing stays the
	   fallback on a budget cut. ducked chains return 2 without a jb, skipped */
	if ( m_jb && !m_duck && !( m_buttons0 & in_duck ) && best_s >= 0 && !m_best_jb ) {
		int jb_n = 0;
		for ( int s = 0; s < sides && !out( ); s++ ) {
			if ( !full_jb[ s ] )
				continue;
			const bool neg = ( s == 0 ) == first_neg;
			const int base = s * k_max_snap;
			int lo = s == best_s ? best_n : 0, hi = n_full[ s ];
			bool hi_full = !jb_n || jb_n > hi;
			if ( !hi_full )
				hi = jb_n;
			while ( hi - lo > 1 && !out( ) ) {
				const int mid = ( lo + hi ) / 2;
				if ( chain( cmd, mid, neg, false, base ) == 2 && m_jb_plan ) {
					keep( jb_n = hi = mid, s );
					hi_full = false;
					m_jb_up = true;
				} else
					lo = mid;
			}
			if ( !hi_full || out( ) )
				continue;
			m_saved[ s ] = 0;
			if ( chain( cmd, m_max_ticks, neg, true, base ) == 2 && m_jb_plan ) {
				keep( jb_n = n_full[ s ], s );
				m_jb_up = true;
			}
		}
	}
	if ( best_s < 0 )
		return false;
	std::copy_n( m_best, m_best_last + 1, m_plan );
	m_last     = m_best_last;
	m_land_pos = m_best_land;
	m_jb_plan  = m_best_jb;
	return true;
}

void n_edge_catch::impl_t::search( c_user_cmd* cmd )
{
	const auto local = g_ctx.m_local;
	const float predict_time = std::clamp( GET_VARIABLE( g_variables.m_edge_catch_predict_time, float ), 0.05f, 1.f );
	m_max_ticks = std::clamp( static_cast< int >( std::round( predict_time / n_tick::interval( ) ) ), 2, k_max_ticks );
	m_reach     = std::clamp( GET_VARIABLE( g_variables.m_edge_catch_reach, float ), 0.5f, 64.f );
	/* 11-11 log: 0.15 of 1/128 s cut 27/48 searches before n >= 3 strafes */
	m_budget.start( 0.25f, n_tick::engine_interval( ), n_tick::search_ec );
	m_sims = m_chains = m_anchors = 0;
	m_kind = 0;
	m_jb   = jb_bind( );
	m_jb_plan = false;
	m_jb_up   = false;
	m_stripped = 0;
	m_duck    = false;
	m_brake   = false;
	m_view     = cmd->m_view_point;
	m_fwd0     = cmd->m_forward_move;
	m_side0    = cmd->m_side_move;
	/* the jump bug's landing crouch on this cmd is one tick, sims model it ( jb_works / jb_drops ): held every tick = wrong fall */
	const int buttons_live = cmd->m_buttons;
	m_buttons0             = m_jb_stamp ? buttons_live & ~in_duck : buttons_live;
	restore_start( );
	m_tick_base0        = local->get_tick_base( );
	const c_vector org0 = local->get_origin( );
	const c_vector vel0 = local->get_velocity( );
	const bool ducked0  = ( local->get_flags( ) & fl_ducking ) != 0;

	/* nothing standable near the free-fall arc = nothing to catch, no sims */
	bool reach = false;
	{
		static auto sv_gravity = g_interfaces.m_convar->find_var( "sv_gravity" );
		const float grav       = sv_gravity ? sv_gravity->get_float( ) : 800.f;
		const float window     = static_cast< float >( m_max_ticks ) * n_tick::engine_interval( );
		const float grow       = m_reach + 2.f;
		const float seg_t      = window / 3.f;
		const c_vector mins( -16.f - grow, -16.f - grow, ducked0 ? -10.f : -1.f );
		const c_vector maxs( 16.f + grow, 16.f + grow, 72.f + grav * seg_t * seg_t * 0.125f );
		c_trace_filter filter( local );
		c_vector prev = org0;
		for ( int seg = 1; seg <= 3 && !reach; seg++ ) {
			const float ts = seg_t * static_cast< float >( seg );
			c_vector p     = org0 + vel0 * ts;
			p.m_z          = org0.m_z + vel0.m_z * ts - 0.5f * grav * ts * ts;
			ray_t ray( prev, p, mins, maxs );
			trace_t tr;
			g_interfaces.m_engine_trace->trace_ray( ray, mask_playersolid, &filter, &tr );
			reach = tr.m_start_solid || tr.m_fraction < 1.f;
			prev  = p;
		}
	}

	bounds_guard_t bounds_guard;

	const bool miss  = reach && baseline( cmd );
	/* full search already failed on this lip: fewer ticks now, it can't land */
	const bool known = miss && m_fail && m_kind == m_fail_kind && std::fabs( m_ground_z - m_fail_gz ) < 0.5f;
	bool found       = miss && !known && family( cmd, m_turn_left );
	/* kind 3, no strafe lands the jb: on the landing tick itself ( cached fail too ) your keys land plain once post takes the jump
	   bug's crouch off. earlier = no plan, a held no-strafe plan would block eb / tung the whole fall */
	if ( miss && !found && m_kind == 3 && m_miss == 0 && !out( ) ) {
		m_saved[ 0 ] = 0;
		found        = chain( cmd, 0, m_turn_left, true, 0 ) != 0;
	}
	/* strafe alone can't reach: hold duck ( feet +9 u ). kind 3 already lands */
	if ( miss && !known && !found && m_kind != 3 && GET_VARIABLE( g_variables.m_edge_catch_duck, bool ) && !( m_buttons0 & in_duck ) && !out( ) ) {
		m_duck = true;
		found  = family( cmd, m_turn_left );
	}
	/* silent plans can't shorten the flight ( strafe = lateral only ): opt-in air brake, last resort, costs speed */
	if ( miss && !known && !found && m_kind != 3 && GET_VARIABLE( g_variables.m_edge_catch_brake, bool ) && !out( ) ) {
		m_duck  = false;
		m_brake = true;
		found   = family( cmd, m_turn_left );
	}
	if ( !known ) {
		m_fail      = miss && !found && !out( );
		m_fail_kind = m_kind;
		m_fail_gz   = m_ground_z;
	}

	cmd->m_view_point       = m_view;
	cmd->m_forward_move     = m_fwd0;
	cmd->m_side_move        = m_side0;
	cmd->m_buttons          = buttons_live;
	local->get_tick_base( ) = m_tick_base0;
	/* never leave a sim tick live */
	if ( m_sims > 0 )
		restore_start( );

	if ( miss && ( !known || found ) )
		botox_dbg_log( "EC: kind=%d miss=%d gz=%.2f z=%.2f vz=%.2f evz=%.2f reach=%.1f win=%d turn=%d chains=%d anc=%d sims=%d us=%lld found=%d last=%d strafe=%d jb=%d/%d up=%d duck=%d brake=%d\n",
		               m_kind, m_miss, m_ground_z, org0.m_z, vel0.m_z, m_miss_vz, m_reach, m_max_ticks, m_turn_left ? 1 : 0, m_chains, m_anchors, m_sims, m_budget.used_us( ),
		               found ? 1 : 0, found ? m_last : -1, found ? strafed( ) : 0, m_jb ? 1 : 0, found && m_jb_plan ? 1 : 0, found && m_jb_up ? 1 : 0,
		               m_duck ? ( found ? 2 : 1 ) : 0, m_brake ? ( found ? 2 : 1 ) : 0 );
	if ( !found )
		return;
	m_active  = true;
	m_hit     = 0;
	m_dropped = false;
	for ( int i = 0; i <= m_last; i++ )
		m_viz[ i ] = m_plan[ i ].origin;
	m_viz[ m_last + 1 ] = m_land_pos;
}

void n_edge_catch::impl_t::post( c_user_cmd* cmd )
{
	const auto local = g_ctx.m_local;
	/* jump_bug's crouch stamp, used up once per cmd ( stale after a reconnect = never ). eb replay with a plan owns the buttons */
	m_jb_stamp    = cmd && g_jb_duck_cmd == cmd->m_command_number && !g_edgebug.m_found;
	g_jb_duck_cmd = -1;
	/* witness: START after the jb plan's unduck tick. vz > 0 = jb fired, air + vz ~ -6 = edgebug, air 0 = plain landing */
	if ( m_jb_out && local )
		botox_dbg_log( "EC: jb out vz=%.2f air=%d\n", m_start_vel.m_z, ( m_start_flags & fl_onground ) ? 0 : 1 );
	m_jb_out = false;
	if ( !cmd || !local || !local->is_alive( ) || !GET_VARIABLE( g_variables.m_edge_catch, bool ) ||
	     !g_input.check_input( &GET_VARIABLE( g_variables.m_edge_catch_key, key_bind_t ) ) ) {
		reset( );
		return;
	}
	if ( const char* gate = m_start_move_type != move_type_walk ? "mt" : g_air_stuck_owns_cmd ? "as" : g_air_align_owns ? "aal" : g_wall_climb.caught( cmd ) ? "wc" :
	                        ps_latched( ) ? "ps" : bind_cmd_taken( ) ? "bind" : g_edge_skip.active( ) ? "es" : nullptr ) {
		if ( m_active )
			botox_dbg_log( "EC: cut by=%s at %d/%d jb=%d\n", gate, m_hit, m_last, m_jb_plan ? 1 : 0 );
		reset( );
		return;
	}
	/* START state: the entity holds this cmd's END here */
	const c_vector start = g_prediction.backup_data.m_origin;
	if ( m_start_flags & fl_onground ) {
		if ( m_active ) {
			const bool on_lip = start.m_z >= m_ground_z - 2.f;
			if ( on_lip )
				++g_ec_fired;
			botox_dbg_log( "EC: landed hit=%d last=%d gz=%.2f z=%.2f lip=%d kind=%d jbx=%d\n", m_hit - 1, m_last, m_ground_z, start.m_z, on_lip ? 1 : 0,
			               m_kind, m_stripped );
		}
		else if ( m_dropped )
			botox_dbg_log( "EC: after drop gz=%.2f z=%.2f lip=%d\n", m_drop_gz, start.m_z, start.m_z >= m_drop_gz - 2.f ? 1 : 0 );
		m_dropped = false;
		reset( );
		return;
	}
	if ( m_active ) {
		/* backup_data origin = abs origin = render interp, lags a falling player up to 2 ticks ( 11-12 log: drop dist=7.6 at
		   vz -223 = 2 ticks of fall ). the plan compares against the predicted START, same as edge skip */
		restore_start( );
		const c_vector clean = local->get_origin( );
		if ( m_jb_plan && m_hit == m_last + 1 ) {
			const bool on_lip = clean.m_z >= m_ground_z - 2.f;
			if ( on_lip )
				++g_ec_fired;
			botox_dbg_log( "EC: jb last=%d gz=%.2f z=%.2f ducked=%d lip=%d\n", m_last, m_ground_z, clean.m_z, ( m_start_flags & fl_ducking ) ? 1 : 0,
			               on_lip ? 1 : 0 );
			reset( );
			m_jb_out = true;
			return;
		} else if ( m_hit > m_last ) {
			botox_dbg_log( "EC: missed spent=%d z=%.2f gz=%.2f\n", m_last, clean.m_z, m_ground_z );
			reset( );
		} else if ( const float dist = clean.dist_to( m_plan[ m_hit ].origin ); dist > 1.f ) {
			int left = 0;
			for ( int i = m_hit; i <= m_last; i++ )
				left += m_plan[ i ].strafe ? 1 : 0;
			const c_vector d = clean - m_plan[ m_hit ].origin;
			botox_dbg_log( "EC: drop at %d/%d dist=%.3f dz=%.3f dxy=%.3f interp=%.3f left=%d jb=%d\n", m_hit, m_last, dist, d.m_z, d.length_2d( ),
			               start.dist_to( m_plan[ m_hit ].origin ), left, m_jb_plan ? 1 : 0 );
			m_dropped = true;
			m_drop_gz = m_ground_z;
			reset( );
		}
	}
	if ( !m_active ) {
		static int n_eb = 0, n_run = 0;
		if ( static unsigned long long next = 0ull; GetTickCount64( ) >= next ) {
			next = GetTickCount64( ) + 1000ull;
			if ( n_eb || n_run )
				botox_dbg_log( "EC: gates eb=%d run=%d\n", n_eb, n_run );
			n_eb = n_run = 0;
		}
		/* rising too: an early plan has more ticks to strafe */
		if ( m_eb_latched )
			n_eb++;
		else {
			n_run++;
			search( cmd );
		}
	}
	if ( !m_active )
		return;
	if ( m_plan[ m_hit ].strafe )
		apply( cmd, m_plan[ m_hit ], cmd->m_view_point );
	/* the plan owns the duck: jump_bug crouched on its unstrafed prediction, the sims already hold the crouch where it fires
	   ( fallback = plain landing, the crouch would carry you off the lip ) */
	const bool jb_duck = m_jb_stamp && ( cmd->m_buttons & in_duck );
	if ( jb_duck )
		cmd->m_buttons &= ~in_duck;
	if ( m_plan[ m_hit ].duck )
		cmd->m_buttons |= in_duck;
	/* the jump bug's landing duck, decided on this cmd from the exact START: a plan verdict carries up to 1 u of drift = crouch on
	   a hair of lip = edgebug ( 11-20 ). landing tick ( plain fallback too ) or a tick the jump bug crouched ( you land early ):
	   crouch iff the jb fires from where you really are, so no working jb is lost and no dead one crouches */
	if ( ( m_hit == m_last || jb_duck ) && !m_plan[ m_hit ].duck && jb_bind( ) ) {
		const bool jb      = live_jb( cmd );
		const bool planned = m_hit == m_last && m_jb_plan;
		if ( jb != planned )
			botox_dbg_log( "EC: jb live=%d plan=%d at %d/%d\n", jb ? 1 : 0, planned ? 1 : 0, m_hit, m_last );
		if ( jb ) {
			m_last         = m_hit;
			cmd->m_buttons = ( cmd->m_buttons | in_duck ) & ~in_jump;
		}
		if ( m_hit == m_last )
			m_jb_plan = jb;
	}
	m_stripped += jb_duck && !( cmd->m_buttons & in_duck ) ? 1 : 0;
	m_hit++;
}
