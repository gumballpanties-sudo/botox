#include "movement_internal.h"
#include "texturebug_common.h"
#include <algorithm>
#include <cmath>
#include <format>
#include <vector>
#include "../../utilities/perf/perf_watch.h"
#include "../aimbot/aimbot.h"

/* 11-10 rewrite ( lab tools/wc_engine/as_catch_lab.cpp policy H, runs/asc5_h_* ): a pin is TryPlayerMove with every bump at
   fraction 0 on ONE wall plane ( gamemovement.cpp:3507 allFraction == 0 ). that happens when the START reads d1 = fl( p.n ) - D > 0
   ( not startsolid, cmodel.cpp:1569 ) while the END of the clipped tangent move rounds to d2 <= 0 on the engine's float grid
   ( coords ~1000-2000 -> 6e-5..2.4e-4 steps ). both are plain float arithmetic, so the catch is solved, not searched:
   park = next START on the xy float lattice whose tangent sweep predicts the most crossings ( 1 cmd, 0 sims ),
   pin  = the predicted crossing move, verified by <= 4 x 2 sims ( tick 1 + the fixed point from ( 0, 0, pin ) ), then held
   as a fixed point ( 1 sim / tick ). no fan, no ladder, no bisection. silent: cmd keeps your view, the move is rotated in. */

static struct {
	bool valid = false;
	bool exact = false;
	c_vector origin{ };
	c_vector velocity{ };
} s_proof;

static struct {
	bool valid = false;
	c_vector origin{ };
} s_expect;

/* last sent pin, view latched: pins live on wishdir float bits ( AngleVectors + VectorNormalize, gamemovement.cpp:2015-2032 ), so live pitch/yaw must never reach it */
static struct {
	bool valid  = false;
	/* vz on the pin, xy still moving: the only hold some walls give ( 11-10 nuke crane: full stops never continue, vz-pins do ).
	   held while vz stays pinned, xy braked toward 0 each tick, sim-verified = converges to a standstill */
	bool vzpin  = false;
	float pitch = 0.0f, yaw = 0.0f, fwd = 0.0f, side = 0.0f;
	int ticks   = 0;
} s_hold;

/* server says pinned where our sim says fall ( 11-04 online log: 64/85 `lost` had the server pinned, 34 still pinned after we gave up;
   server pinned + same wire cmd next = kept 35/35, any other cmd 24/53 ) = resend the acked cmd until the server stops pinning */
static struct {
	bool valid  = false;
	c_angle view{ };
	float fwd = 0.0f, side = 0.0f;
	int ticks = 0, moved = 0;
	c_vector origin{ }; /* the server's END of the acked cmd = where it is pinned; a fixed point is solved from here ( 16:01 log: copies of a
	                       straight-in park fell at t=1 61/70, no tangent = nothing rounds ) */
} s_lock;

/* last park cmd: online the server pins on it ( 11-10 10:58 log: 5/5 cycles `sv lock ack` = the park cmd, same cmd next kept 16/16,
   a different one 1/5 ), but the client learns cp ticks later and has already sent its own pin = knocked off. so the in-flight
   cmds after a park repeat it until the park's ack. not the 11-09 "park sticky" ( straight-in bisect, no tangent, 0 server pins ): this park keeps the
   tangent speed, which is what rounds across */
static struct {
	bool valid = false;
	int cmd = -1, ticks = 0;
	float yaw = 0.0f, fwd = 0.0f;
} s_park;

/* every cmd we publish. START pinned by it with no armed hold ( an unproven press pinned on a seam: 11-10 11:31 nuke crane, fwd 1 at 86
   deg pinned twice, lost next tick to a fresh fan ) = replay it: proven -> hold, else resend */
static struct {
	bool valid = false;
	int cmd    = -1;
	float pitch = 0.0f, yaw = 0.0f, fwd = 0.0f, side = 0.0f;
} s_last;

/* consecutive ticks the plane model predicted crossings the engine refused ( lab PREDBAD: first bump moves 0.0005u = a neighbouring
   coplanar brush an ulp off the probe's plane ). the model is a hint: after 2 misses the engine-verified fan owns this contact */
static int s_miss_streak = 0;
/* online: ticks the broken hold's pin cmd Q has been re-sent inside the in-flight window ( 17:11 log: 12/13 falls right after a
   client-proven hold = the server ran Q from a START 1e-3..3u off ours; Q is a tangent pin, the server keeps its own pin on the same
   cmd 476/536, so Q stays on the wire cp ticks before a re-solve may replace it ) */
static int s_hold_win = 0;

static void lock_end( const char* why )
{
	if ( s_lock.valid )
		botox_dbg_log( "AS: sv lock end t=%d moved=%d why=%s", s_lock.ticks, s_lock.moved, why );
	s_lock.valid = false;
}

/* online the engine keeps its own START while the server's END differs < 0.0625u / 0.5u/s ( cl_pred_optimize 2, c_baseentity.cpp:656/681,
   prediction.cpp:1681 ); pins are exact floats, so armed = full re-predict off the server snapshot every packet ( offline already is ).
   log: server END of the acked cmd vs the START we built on */
static void as_sync( c_user_cmd* cmd, const bool armed, const bool hold_armed )
{
	static auto cl_pred_optimize = g_interfaces.m_convar->find_var( "cl_pred_optimize" );
	g_prediction.sync_arm( n_prediction::impl_t::k_sync_as, armed );

	struct rec_t {
		int cmd = -1, tick_base = 0;
		c_vector origin{ }, velocity{ };
	};
	static rec_t ring[ 128 ];
	static int last_ack = -1, ok = 0;
	if ( !armed ) {
		last_ack = -1;
		lock_end( "off" );
		return;
	}
	c_base_entity* const local = g_ctx.m_local;
	const c_vector origin      = local->get_abs_origin( );
	const c_vector velocity    = local->get_velocity( );
	const int tick_base        = local->get_tick_base( );
	const int prev             = cmd->m_command_number - 1;
	ring[ prev & 127 ]         = { prev, tick_base, origin, velocity };

	const int ack = g_interfaces.m_client_state->m_last_command_ack;
	if ( ack == last_ack || g_interfaces.m_prediction->m_commands_predicted <= 0 || ring[ ack & 127 ].cmd != ack )
		return;
	last_ack = ack;

	g_prediction.restore_entity_to_predicted_frame( -1 );
	const c_vector sv_origin   = local->get_abs_origin( );
	const c_vector sv_velocity = local->get_velocity( );
	const int sv_tick_base     = local->get_tick_base( );
	g_prediction.restore_entity_to_predicted_frame( g_interfaces.m_prediction->m_commands_predicted - 1 );
	local->set_abs_origin( origin );
	local->get_origin( )    = origin;
	local->get_velocity( )  = velocity;
	local->get_tick_base( ) = tick_base;

	const c_vector d  = sv_origin - ring[ ack & 127 ].origin;
	const c_vector dv = sv_velocity - ring[ ack & 127 ].velocity;
	const bool match  = d.m_x == 0.0f && d.m_y == 0.0f && d.m_z == 0.0f && dv.m_x == 0.0f && dv.m_y == 0.0f && dv.m_z == 0.0f;
	const c_user_cmd* const a = g_interfaces.m_input->get_user_cmd( ack );
	const bool sv_pinned      = sv_velocity.m_x == 0.0f && sv_velocity.m_y == 0.0f && sv_velocity.m_z == g_prediction.get_engine_target_predict_z_velocity( );
	/* client holds a proven pin from this pinned START ( 1 sim re-proof per tick ): the server keeping it is not a lock. 11-09 log: 2 locks
	   latched an OLDER approach cmd while the server kept the hold 4-5 acks ( dv=0 ), the lock copies then knocked it off */
	const bool holding = hold_armed && velocity.m_x == 0.0f && velocity.m_y == 0.0f && velocity.m_z == g_prediction.get_engine_target_predict_z_velocity( );
	if ( !sv_pinned ) {
		lock_end( "fell" );
	} else if ( !holding && ( !match || s_lock.valid ) ) {
		if ( !s_lock.valid ) {
			s_lock.ticks = s_lock.moved = 0;
			botox_dbg_log( "AS: sv lock ack=%d cmd=%.4f/%.4f v=%.4f/%.4f", ack, a->m_forward_move, a->m_side_move, a->m_view_point.m_x, a->m_view_point.m_y );
		}
		s_lock.valid  = true;
		s_lock.view   = a->m_view_point;
		s_lock.fwd    = a->m_forward_move;
		s_lock.side   = a->m_side_move;
		s_lock.origin = sv_origin;
	}
	if ( match ) {
		++ok;
		return;
	}
	/* same cmd as the one before + dtb 0 + a pinned START = server float noise; dtb != 0 = server ran an extra / null cmd ( player.cpp:3603 ) */
	const c_user_cmd* const b = g_interfaces.m_input->get_user_cmd( ack - 1 );
	const bool same_cmd = a->m_view_point.m_x == b->m_view_point.m_x && a->m_view_point.m_y == b->m_view_point.m_y && a->m_forward_move == b->m_forward_move &&
	                      a->m_side_move == b->m_side_move && a->m_buttons == b->m_buttons;
	botox_dbg_log( "AS: sync miss ack=%d ok=%d d=%.3g,%.3g,%.3g dv=%.3g,%.3g,%.3g vz=%.3f opt=%d cmd=%.4f/%.4f v=%.4f/%.4f b=%x same=%d dtb=%d",
	               ack, ok, d.m_x, d.m_y, d.m_z, dv.m_x, dv.m_y, dv.m_z, sv_velocity.m_z, cl_pred_optimize ? cl_pred_optimize->get_int( ) : -1,
	               a->m_forward_move, a->m_side_move, a->m_view_point.m_x, a->m_view_point.m_y, a->m_buttons, ( int )same_cmd,
	               sv_tick_base - ring[ ack & 127 ].tick_base );
	ok = 0;
}

static void stamp_rotate( const float view_yaw, const float yaw, const float fwd, const float side, float& out_fwd, float& out_side )
{
	const float delta = deg2rad( std::remainder( view_yaw - yaw, 360.0f ) );
	const float cos_d = std::cos( delta );
	const float sin_d = std::sin( delta );
	out_fwd           = cos_d * fwd - sin_d * side;
	out_side          = sin_d * fwd + cos_d * side;
}

/* ---- the engine's float geometry ( cmodel.cpp:1553-1566, coll.cpp mirror ), n.z == 0 walls ---- */
namespace n_as_h
{
	struct plane_t {
		c_vector n{ };
		float dist = 0.0f;
	};
	struct hull_t {
		c_vector ext{ }, off{ }; /* ( maxs - mins ) * 0.5, ( mins + maxs ) * 0.5: Ray_t::Init */
	};
	static float dotv( const c_vector& a, const c_vector& b ) { return a.m_x * b.m_x + a.m_y * b.m_y + a.m_z * b.m_z; }
	/* d1 of a hull-trace START at ray centre p: Dot( p, n ) - ( plane.dist + DotProductAbs( n, extents ) ) */
	static float eng_dp( const c_vector& p, const plane_t& P, const hull_t& H )
	{
		float dist = std::fabsf( P.n.m_x * H.ext.m_x ) + std::fabsf( P.n.m_y * H.ext.m_y ) + std::fabsf( P.n.m_z * H.ext.m_z );
		dist       = P.dist + dist;
		return ( p.m_x * P.n.m_x + p.m_y * P.n.m_y + p.m_z * P.n.m_z ) - dist;
	}
	static float eng_d( const c_vector& origin, const plane_t& P, const hull_t& H ) { return eng_dp( origin + H.off, P, H ); }
	/* d at the END of a free move: end = VectorMA( origin, dt, v ); ray p2 = ( origin + off ) + ( end - origin ) */
	static float d_after( const c_vector& origin, const plane_t& P, const hull_t& H, const c_vector& v, const float dt )
	{
		const c_vector p1 = origin + H.off;
		const c_vector end( origin.m_x + v.m_x * dt, origin.m_y + v.m_y * dt, origin.m_z + v.m_z * dt );
		return eng_dp( p1 + ( end - origin ), P, H );
	}
	/* exact distance ( double ) and the float grid it is read on: q = ulp of the dot sum, J = half the product ulps */
	static double true_d( const c_vector& origin, const plane_t& P, const hull_t& H )
	{
		const double D = ( double )P.dist + ( double )H.ext.m_x * std::fabs( ( double )P.n.m_x ) + ( double )H.ext.m_y * std::fabs( ( double )P.n.m_y );
		return ( ( double )origin.m_x + H.off.m_x ) * P.n.m_x + ( ( double )origin.m_y + H.off.m_y ) * P.n.m_y - D;
	}
	static float ulp( float v )
	{
		v = std::fabsf( v );
		return std::nextafterf( v, FLT_MAX ) - v;
	}
	static float grid_q( const c_vector& origin, const plane_t& P, const hull_t& H )
	{
		const c_vector p = origin + H.off;
		return ulp( p.m_x * P.n.m_x + p.m_y * P.n.m_y );
	}
	/* StartGravity + AirAccelerate ( gamemovement.cpp:2293 ) for yaw / fwd, side 0 */
	struct accel_t {
		float dt, airaccel, max_wish, friction, max_speed, grav_half;
	};
	static c_vector accel_w( const c_vector& vel, const c_vector& w, const float fwd, const accel_t& A, float* a_out = nullptr )
	{
		c_vector v = vel;
		v.m_z -= A.grav_half;
		const float ws      = std::min( fwd, A.max_speed );
		const float wishspd = std::min( ws, A.max_wish );
		const float add     = wishspd - dotv( v, w );
		float a             = 0.0f;
		if ( add > 0.0f ) {
			a = std::min( A.airaccel * ws * A.dt * A.friction, add );
			v.m_x += a * w.m_x;
			v.m_y += a * w.m_y;
		}
		if ( a_out )
			*a_out = a;
		return v;
	}
	static c_vector accel( const c_vector& vel, const float yaw, const float fwd, const accel_t& A, float* a_out = nullptr )
	{
		const float r = deg2rad( yaw );
		return accel_w( vel, c_vector( std::cos( r ), std::sin( r ), 0.0f ), fwd, A, a_out );
	}
	static c_vector clip( const c_vector& v, const c_vector& n )
	{
		const float b = dotv( v, n );
		return c_vector( v.m_x - n.m_x * b, v.m_y - n.m_y * b, v.m_z - n.m_z * b );
	}
	struct cand_t {
		float yaw, fwd, a;
		int score; /* 2 = tick 1 + fixed point both predicted, 1 = tick 1 only */
	};
	/* pin candidates from a START: tangent wish ( leaned 5 deg in so bump 1 hits ) whose clipped END rounds across the plane.
	   out = nullptr counts only ( park scoring ). START must read d > 0, else it is startsolid, not a pin */
	static int build( const c_vector& origin, const c_vector& vel, const plane_t& P, const hull_t& H, const accel_t& A, const float pin_vz, const float fwd_cap,
	                  const float step, std::vector< cand_t >* out, const int cap )
	{
		const float da = eng_d( origin, P, H );
		if ( da <= 0.0f )
			return 0;
		int found = 0;
		const c_vector pinned( 0.0f, 0.0f, pin_vz );
		const float coef = A.airaccel * A.dt * A.friction;
		for ( int s = 0; s < 2 && found < cap; ++s ) {
			const float sg = s ? -1.0f : 1.0f;
			const c_vector t( -P.n.m_y * sg, P.n.m_x * sg, 0.0f );
			const c_vector w( t.m_x * 0.9961947f - P.n.m_x * 0.0871557f, t.m_y * 0.9961947f - P.n.m_y * 0.0871557f, 0.0f );
			const float y = rad2deg( std::atan2( w.m_y, w.m_x ) );
			/* trig hoisted: the verify sim uses y, the emulation uses w ( same direction, ~1e-7 apart, far below the grid ) */
			for ( float a_want = step; a_want <= 30.0f && found < cap; a_want += step ) {
				const float f = a_want / coef;
				if ( f > fwd_cap )
					break;
				float a           = 0.0f;
				const c_vector v1 = accel_w( vel, w, f, A, &a );
				if ( a <= 0.0f || dotv( v1, P.n ) * A.dt > -da * 2.0f )
					continue;
				if ( d_after( origin, P, H, clip( v1, P.n ), A.dt ) > 0.0f )
					continue;
				++found;
				if ( out ) {
					const c_vector v2 = accel_w( pinned, w, f, A );
					const bool fixd   = dotv( v2, P.n ) < 0.0f && d_after( origin, P, H, clip( v2, P.n ), A.dt ) <= 0.0f;
					out->push_back( { y, f, a, fixd ? 2 : 1 } );
				}
			}
		}
		return found;
	}
}

void n_movement::impl_t::air_stuck( c_user_cmd* cmd )
{
	PERF_ZONE( zone_cmd_air_stuck );
	static bool was_stuck = false;

	s_proof.valid = false;
	const bool hold_armed = s_hold.valid;
	s_hold.valid = false;
	const int saved_auto_align_block_until_tick = m_air_stuck_data.m_auto_align_block_until_tick;
	m_air_stuck_data.reset( );
	m_air_stuck_data.m_auto_align_block_until_tick = saved_auto_align_block_until_tick;

	const bool feature = GET_VARIABLE( g_variables.m_air_stuck, bool );
	const bool key     = g_input.check_input( &GET_VARIABLE( g_variables.m_air_stuck_key, key_bind_t ) );
	const bool alive   = g_ctx.m_local && g_ctx.m_local->is_alive( );
	as_sync( cmd, feature && key && alive, hold_armed );

	if ( !feature || !key || !alive || ( g_ctx.m_local->get_flags( ) & fl_onground ) ) {
		if ( hold_armed )
			botox_dbg_log( "AS: end t=%d why=%s blk=%d", s_hold.ticks, !feature ? "off" : !key ? "key" : !alive ? "dead" : "ground", ( int )g_input.keys_blocked( ) );
		s_hold.ticks   = 0;
		was_stuck      = false;
		s_expect.valid = false;
		return;
	}
	const int move_type = g_ctx.m_local->get_move_type( );
	if ( move_type == move_type_ladder || move_type == move_type_noclip || move_type == move_type_fly || move_type == move_type_observer ) {
		was_stuck      = false;
		s_expect.valid = false;
		return;
	}

	n_tick::c_sim_budget clock;
	clock.start( GET_VARIABLE( g_variables.m_air_stuck_budget, float ) * 0.01f, n_tick::engine_interval( ), n_tick::search_as );

	/* probes run with m_bounds_max_speed as the move's max speed ( prediction.cpp:118 ) = the trace leaf-list bounds; the real run uses
	   m_real_max_speed. unset ( 0 ) the sim sees a tighter brush list than the engine: 11-10 13:13 log, 55 rescues verified pinned and
	   fell live on bullet ticks. same bounds as texturebug / wall climb, cleared on every exit */
	struct bounds_guard_t {
		bounds_guard_t( ) { g_prediction.m_bounds_max_speed = g_prediction.m_real_max_speed; }
		~bounds_guard_t( ) { g_prediction.m_bounds_max_speed = 0.f; }
	} bounds_guard;

	const c_vector origin        = g_ctx.m_local->get_abs_origin( );
	const c_vector live_velocity = g_ctx.m_local->get_velocity( ); /* not backup_data: stale one cmd behind on choked/hitched cmds */
	const c_angle live_view      = cmd->m_view_point;
	const int live_buttons       = cmd->m_buttons;
	const float live_fwd         = cmd->m_forward_move;
	const float live_side        = cmd->m_side_move;
	const auto mins              = g_ctx.m_local->get_collideable( )->get_obb_mins( );
	const auto maxs              = g_ctx.m_local->get_collideable( )->get_obb_maxs( );
	c_trace_filter_trace_type_everything_filter_props filter;

	const auto apply_predicted_state = [ & ]( ) noexcept {
		g_ctx.m_local->set_abs_origin( origin );
		g_ctx.m_local->get_origin( )   = origin;
		g_ctx.m_local->get_velocity( ) = live_velocity;
	};
	const auto restore = [ & ]( ) {
		g_prediction.restore_entity_to_predicted_frame( g_interfaces.m_prediction->m_commands_predicted - 1 );
		apply_predicted_state( );
		cmd->m_view_point   = live_view;
		cmd->m_forward_move = live_fwd;
		cmd->m_side_move    = live_side;
		cmd->m_buttons      = live_buttons;
	};

	/* how the move goes on the wire: silent angles, always. the pin view itself is the cmd view on every tick no bullet leaves = the
	   proven bits exactly, your camera is the engine view ( silent_view keeps a forced view, fake_pov records the engine view ) so
	   turning cannot break a pin; a bullet tick sends your real view with the move rotated in ( the shot goes where you aim ) and
	   air_stuck_shot_proof re-proves it. "use movement fix" off = forced on bullet ticks too ( the shot view is then the pin view ).
	   zero move: wishvel 0 = any view sims the same */
	/* bullet = a GUN fires this tick ( shot_leaves is true for any non-gun: a knife swing needs no real view, 13:23 log "falls off
	   when i swing my knife" ). mutable: a bullet tick with no proven rotated pin falls back to the forced pin and delays the shot */
	const bool gun = [ & ] {
		const auto active = g_interfaces.m_client_entity_list->get< c_base_entity >( g_ctx.m_local->get_active_weapon_handle( ) );
		const auto data   = active ? g_interfaces.m_weapon_system->get_weapon_data( active->get_item_definition_index( ) ) : nullptr;
		return data && data->is_gun( );
	}( );
	const bool bullet = ( live_buttons & ( in_attack | in_second_attack ) ) && gun && g_aimbot.shot_leaves( );
	bool send_forced  = !bullet || !GET_VARIABLE( g_variables.m_movement_fix, bool );
	const auto publish = [ & ]( const float yaw, const float move, const bool stuck, const float pitch, const bool exact, const float side = 0.0f ) {
		s_last                    = { true, cmd->m_command_number, pitch, yaw, move, side };
		s_proof                   = { stuck, exact, origin, live_velocity };
		s_hold.valid              = stuck;
		s_hold.vzpin              = false;
		s_hold.pitch              = pitch;
		s_hold.yaw                = yaw;
		s_hold.fwd                = move;
		s_hold.side               = side;
		cmd->m_view_point         = live_view;
		stamp_rotate( live_view.m_y, yaw, move, side, cmd->m_forward_move, cmd->m_side_move );
		g_air_stuck_view_forced = send_forced;
		if ( g_air_stuck_view_forced ) {
			cmd->m_view_point.m_x = pitch;
			cmd->m_view_point.m_y = yaw;
			cmd->m_forward_move   = move;
			cmd->m_side_move      = side;
		}
		cmd->m_buttons            = live_buttons;
		g_air_stuck_stamp_yaw     = yaw;
		g_air_stuck_stamp_forward = move;
		g_air_stuck_stamp_side    = side;
		g_air_stuck_stamp_buttons = cmd->m_buttons;
		g_air_stuck_stamp_valid   = true;
		g_air_stuck_stamp_pitch   = pitch;
		g_air_stuck_authored_view       = cmd->m_view_point;
		g_air_stuck_authored_view_valid = true;

		m_air_stuck_data.m_air_stuck = stuck;
		if ( stuck && !was_stuck && GET_VARIABLE( g_variables.m_air_stuck_display_screen, bool ) )
			movement_add_window( 5, std::format( "stuck with fw={:.3f} sm={:.3f} yaw={:.3f}", cmd->m_forward_move, cmd->m_side_move, yaw ) );
		was_stuck = stuck;
	};

	const float pin_vz    = g_prediction.get_engine_target_predict_z_velocity( );
	const auto vel_pinned = [ pin_vz ]( const c_vector& v ) { return v.m_x == 0.0f && v.m_y == 0.0f && v.m_z == pin_vz; };
	const auto pin_held   = [ & ]( ) {
		return !( g_ctx.m_local->get_flags( ) & fl_onground ) && g_ctx.m_local->get_move_type( ) == move_type_walk && vel_pinned( g_ctx.m_local->get_velocity( ) );
	};
	const auto same_o = [ ]( const c_vector& a, const c_vector& b ) { return a.m_x == b.m_x && a.m_y == b.m_y && a.m_z == b.m_z; };
	/* vz exactly on the pin, airborne, xy free ( ridobebra's accept ) */
	const auto vz_held = [ & ]( ) {
		return !( g_ctx.m_local->get_flags( ) & fl_onground ) && g_ctx.m_local->get_move_type( ) == move_type_walk && g_ctx.m_local->get_velocity( ).m_z == pin_vz;
	};
	const bool start_vzpin = live_velocity.m_z == pin_vz && !vel_pinned( live_velocity );
	/* one engine tick of ( yaw, fwd ) from the live START; true = still pinned at the same origin ( fixed point ).
	   simmed in the SENT form: forced = the pin frame ( silent angles / movement fix off ), else your live view with the move rotated in.
	   11-10 12:20 log: the pin-frame sim said the hold falls 88 ticks in a row while the engine kept the live-view cmd pinned */
	bool send_fix       = !send_forced;
	const auto sim_pins = [ & ]( const float yaw, const float fwd, const float pitch, int& sims ) {
		g_prediction.restore_entity_to_predicted_frame( g_interfaces.m_prediction->m_commands_predicted - 1 );
		apply_predicted_state( );
		if ( send_fix ) {
			cmd->m_view_point = live_view;
			stamp_rotate( live_view.m_y, yaw, fwd, 0.0f, cmd->m_forward_move, cmd->m_side_move );
		} else {
			cmd->m_view_point.m_x = pitch;
			cmd->m_view_point.m_y = yaw;
			cmd->m_forward_move   = fwd;
			cmd->m_side_move      = 0.0f;
		}
		cmd->m_buttons        = live_buttons;
		g_prediction.begin( g_ctx.m_local, cmd );
		g_prediction.end( g_ctx.m_local );
		++sims;
		return pin_held( ) && same_o( g_ctx.m_local->get_abs_origin( ), origin );
	};
	const auto sim_again = [ & ]( int& sims ) {
		g_prediction.begin( g_ctx.m_local, cmd );
		g_prediction.end( g_ctx.m_local );
		++sims;
		return pin_held( ) && same_o( g_ctx.m_local->get_abs_origin( ), origin );
	};
	/* one tick of an exact sent move ( fwd / side bits as they go on the wire, your live view ) */
	const auto sim_raw = [ & ]( const float f, const float s, int& sims ) {
		g_prediction.restore_entity_to_predicted_frame( g_interfaces.m_prediction->m_commands_predicted - 1 );
		apply_predicted_state( );
		cmd->m_view_point   = live_view;
		cmd->m_forward_move = f;
		cmd->m_side_move    = s;
		cmd->m_buttons      = live_buttons;
		g_prediction.begin( g_ctx.m_local, cmd );
		g_prediction.end( g_ctx.m_local );
		++sims;
		return pin_held( ) && same_o( g_ctx.m_local->get_abs_origin( ), origin );
	};
	/* turning / shooting broke the replay: the rotated move's float bits changed. shot_proof's ladder around the rotated move in the
	   live view ( +-6 ulp side, +-3 ulp fwd, magnitude +-j/16 ) finds the same pin again 394/399 times ( 12:37 log ). true = published,
	   in the LIVE frame ( stamp yaw = live yaw ) so final_stamp / shot_proof pass the exact bits through */
	const auto hold_rescue = [ & ]( const float yaw, const float fwd, const float side, int& sims ) {
		if ( !send_fix )
			return false;
		float rot_f = 0.0f, rot_s = 0.0f;
		stamp_rotate( live_view.m_y, yaw, fwd, side, rot_f, rot_s );
		const auto ulp = [ ]( float x, int k ) {
			for ( ; k > 0; --k )
				x = std::nextafter( x, FLT_MAX );
			for ( ; k < 0; ++k )
				x = std::nextafter( x, -FLT_MAX );
			return x;
		};
		float f = 0.0f, s = 0.0f;
		bool got   = false;
		int tries  = 0;
		const auto attempt = [ & ]( const float af, const float as ) {
			if ( got || tries >= 40 || ( tries > 0 && clock.expired( ) ) )
				return;
			++tries;
			if ( sim_raw( af, as, sims ) ) {
				got = true;
				f   = af;
				s   = as;
			}
		};
		for ( int k = 1; k <= 6; ++k ) {
			attempt( rot_f, ulp( rot_s, k ) );
			attempt( rot_f, ulp( rot_s, -k ) );
		}
		for ( int k = 1; k <= 3; ++k ) {
			attempt( ulp( rot_f, k ), rot_s );
			attempt( ulp( rot_f, -k ), rot_s );
		}
		for ( int j = 1; j <= 8; ++j )
			for ( const float m : { 1.0f + j / 16.0f, 1.0f - j / 16.0f } ) {
				float mf = 0.0f, ms = 0.0f;
				stamp_rotate( live_view.m_y, yaw, fwd * m, side * m, mf, ms );
				attempt( mf, ms );
			}
		restore( );
		if ( !got )
			return false;
		botox_dbg_log( "AS: hold rescue t=%d yaw=%.1f fwd=%.4f -> live %.4f/%.4f tries=%d", s_hold.ticks, yaw, fwd, f, s, tries );
		++s_hold.ticks;
		publish( live_view.m_y, f, true, live_view.m_x, true, s );
		s_expect = { true, origin };
		return true;
	};

	/* server pinned where our sim predicts a fall. a fixed point solved from the server's own origin ( world diagonal walls, below )
	   beats copying its cmd; the copy stays the fallback ( 16:01 log: 61/70 copies of a straight-in park fell at t=1 ) */
	const bool lock_tick = s_lock.valid;
	const auto lock_copy = [ & ]( const char* why ) {
		s_expect.valid = false;
		s_hold.ticks   = 0;
		publish( s_lock.view.m_y, s_lock.fwd, true, s_lock.view.m_x, true, s_lock.side );
		s_proof.valid = false;
		s_hold.valid  = false;
		++s_lock.ticks;
		s_lock.moved += !g_air_stuck_view_forced && ( live_view.m_x != s_lock.view.m_x || live_view.m_y != s_lock.view.m_y );
		botox_dbg_log( "AS: lock copy t=%d why=%s", s_lock.ticks, why );
	};
	/* one tick from an arbitrary START ( the server's pinned origin ), velocity written with the abs-velocity cache dirtied */
	const auto sim_at = [ & ]( const c_vector& o, const c_vector& v, const float yaw, const float fwd, int& sims ) {
		g_prediction.restore_entity_to_predicted_frame( g_interfaces.m_prediction->m_commands_predicted - 1 );
		g_ctx.m_local->set_abs_origin( o );
		g_ctx.m_local->get_origin( )   = o;
		g_ctx.m_local->get_velocity( ) = v;
		if ( int& eflags = g_ctx.m_local->get_eflags( ); reinterpret_cast< std::uintptr_t >( &eflags ) != reinterpret_cast< std::uintptr_t >( g_ctx.m_local ) )
			eflags |= 1 << 12;
		if ( send_fix ) {
			cmd->m_view_point = live_view;
			stamp_rotate( live_view.m_y, yaw, fwd, 0.0f, cmd->m_forward_move, cmd->m_side_move );
		} else {
			cmd->m_view_point.m_x = live_view.m_x;
			cmd->m_view_point.m_y = yaw;
			cmd->m_forward_move   = fwd;
			cmd->m_side_move      = 0.0f;
		}
		cmd->m_buttons = live_buttons;
		g_prediction.begin( g_ctx.m_local, cmd );
		g_prediction.end( g_ctx.m_local );
		++sims;
		return pin_held( ) && same_o( g_ctx.m_local->get_abs_origin( ), o );
	};

	/* last tick sent a proven pin that did not hold: d = real START - our sim end */
	if ( s_expect.valid ) {
		s_expect.valid = false;
		if ( !vel_pinned( live_velocity ) ) {
			const c_vector d = origin - s_expect.origin;
			botox_dbg_log( "AS: lost d=%.6f,%.6f,%.6f vel=%.3f,%.3f,%.3f cp=%d", d.m_x, d.m_y, d.m_z, live_velocity.m_x, live_velocity.m_y, live_velocity.m_z,
			               g_interfaces.m_prediction->m_commands_predicted );
		}
	}

	/* vz-pin hold: vz still on the pin. candidates = the held cmd + brakes against the xy drift; keep the one that keeps vz pinned with
	   the least xy ( a full stop wins outright ). nothing keeps vz = break, search */
	if ( hold_armed && s_hold.vzpin && ( start_vzpin || vel_pinned( live_velocity ) ) ) {
		int sims = 0;
		const float vxy0 = live_velocity.length_2d( );
		const float dyaw = vxy0 > 0.01f ? rad2deg( std::atan2( -live_velocity.m_y, -live_velocity.m_x ) ) : s_hold.yaw; /* against the drift */
		struct { float yaw, fwd; } cands[ 7 ] = { { s_hold.yaw, s_hold.fwd }, { dyaw, 5.0f }, { dyaw, 30.0f }, { dyaw, 120.0f }, { dyaw, 450.0f }, { dyaw, 1.0f }, { s_hold.yaw, 0.0f } };
		float best_xy = FLT_MAX, b_yaw = 0.0f, b_fwd = 0.0f;
		bool b_full = false;
		for ( const auto& c : cands ) {
			if ( sims > 0 && clock.expired( ) )
				break;
			const bool full = sim_pins( c.yaw, c.fwd, s_hold.pitch, sims );
			if ( !full && !vz_held( ) )
				continue;
			const float xy = full ? 0.0f : g_ctx.m_local->get_velocity( ).length_2d( );
			if ( xy < best_xy ) {
				best_xy = xy;
				b_yaw   = c.yaw;
				b_fwd   = c.fwd;
				b_full  = full;
			}
			if ( full )
				break;
		}
		restore( );
		if ( best_xy < FLT_MAX ) {
			if ( s_hold.ticks++ == 0 || ( s_hold.ticks & 15 ) == 0 )
				botox_dbg_log( "AS: hold vzpin t=%d yaw=%.2f fwd=%.2f xy=%.2f -> %.2f full=%d sims=%d", s_hold.ticks, b_yaw, b_fwd, vxy0, best_xy, ( int )b_full, sims );
			publish( b_yaw, b_fwd, true, s_hold.pitch, b_full );
			s_hold.vzpin = !b_full;
			if ( !b_full )
				s_proof.valid = false;
			s_expect = { b_full, origin };
			return;
		}
		botox_dbg_log( "AS: hold vzpin break t=%d xy=%.2f sims=%d", s_hold.ticks, vxy0, sims );
	}
	/* hold: START pinned by last tick's pin = replay it in the latched view, 1 sim proves the fixed point */
	else if ( hold_armed && vel_pinned( live_velocity ) ) {
		int sims        = 0;
		const bool held = sim_pins( s_hold.yaw, s_hold.fwd, s_hold.pitch, sims );
		restore( );
		if ( !held && hold_rescue( s_hold.yaw, s_hold.fwd, s_hold.side, sims ) )
			return;
		/* bullet tick, nothing proven in your view: keep the forced pin ( proven frame ) and fire next tick instead ( 13:23 log: 46 rescues
		   + 9 unproven shot proofs fell; a bullet one tick late is invisible, a lost stuck is not ) */
		if ( !held && bullet && send_fix ) {
			send_forced = true;
			send_fix    = false;
			const bool forced_held = sim_pins( s_hold.yaw, s_hold.fwd, s_hold.pitch, sims );
			restore( );
			if ( forced_held ) {
				++s_hold.ticks;
				botox_dbg_log( "AS: shot held t=%d yaw=%.1f fwd=%.4f sims=%d", s_hold.ticks, s_hold.yaw, s_hold.fwd, sims );
				publish( s_hold.yaw, s_hold.fwd, true, s_hold.pitch, true, s_hold.side );
				cmd->m_buttons &= ~( in_attack | in_second_attack );
				g_air_stuck_stamp_buttons = cmd->m_buttons;
				s_expect = { true, origin };
				return;
			}
			send_forced = false;
			send_fix    = true;
		}
		if ( held ) {
			s_hold_win = 0;
			if ( s_hold.ticks++ == 0 )
				botox_dbg_log( "AS: hold yaw=%.1f fwd=%.4f pitch=%.2f fixed=1", s_hold.yaw, s_hold.fwd, s_hold.pitch );
			publish( s_hold.yaw, s_hold.fwd, true, s_hold.pitch, true, s_hold.side );
			s_expect = { true, origin };
			return;
		}
		/* ONLINE ( cp > 0 ): START still pinned = the server kept our cmd; only the client's re-predicted START moved 1-2 ulps so the replay
		   reads a fall ( 11-10 10:31 log: `hold break t=0` then a NEW pin from the pinned START knocked the server off 36/45, same cmd kept
		   10/13 ). resend the hold unproven. OFFLINE the START is the truth: a failed replay means the same cmd falls, search again from
		   this pinned START instead ( 11-10 11:49 nuke crane: 3 one-tick pins, each blindly resent, each `lost` ) */
		/* online: Q may be holding the SERVER ( its START differs from ours ); keep it on the wire for the in-flight window, then solve */
		if ( const int cpn = g_interfaces.m_prediction->m_commands_predicted; cpn > 0 && s_hold_win < cpn ) {
			++s_hold_win;
			++s_hold.ticks;
			botox_dbg_log( "AS: hold window t=%d/%d yaw=%.1f fwd=%.4f pinned=%d", s_hold_win, cpn, s_hold.yaw, s_hold.fwd, ( int )vel_pinned( live_velocity ) );
			publish( s_hold.yaw, s_hold.fwd, true, s_hold.pitch, false, s_hold.side );
			s_proof.valid = false;
			s_expect      = { true, origin };
			return;
		}
		botox_dbg_log( "AS: hold break t=%d yaw=%.1f fwd=%.4f pitch=%.2f b=%x pinned=%d cp=%d", s_hold.ticks, s_hold.yaw, s_hold.fwd, s_hold.pitch, live_buttons,
		               ( int )vel_pinned( live_velocity ), g_interfaces.m_prediction->m_commands_predicted );
	}
	s_hold.ticks = 0;
	s_hold_win   = 0;
	/* pinned START, no armed hold: the last cmd ( unproven press / park / resend ) pinned us. replay it before anything new
	   ( 11-10 11:31 nuke crane: the engine pinned on the fallback press twice, both lost next tick to a fresh fan ) */
	if ( !hold_armed && vel_pinned( live_velocity ) && s_last.valid && s_last.cmd == cmd->m_command_number - 1 ) {
		int sims         = 0;
		const bool held  = sim_pins( s_last.yaw, s_last.fwd, s_last.pitch, sims );
		const bool fixed = held && sim_again( sims );
		restore( );
		botox_dbg_log( "AS: catch last yaw=%.2f fwd=%.4f held=%d fixed=%d cp=%d", s_last.yaw, s_last.fwd, ( int )held, ( int )fixed, g_interfaces.m_prediction->m_commands_predicted );
		if ( held ) {
			publish( s_last.yaw, s_last.fwd, true, s_last.pitch, true, s_last.side );
			s_expect = { true, origin };
			return;
		}
	}
	/* vz-pinned START with no armed hold ( the last press / park did it ): take it as a vz-pin hold from here */
	if ( !hold_armed && start_vzpin && s_last.valid && s_last.cmd == cmd->m_command_number - 1 ) {
		int sims = 0;
		sim_pins( s_last.yaw, s_last.fwd, s_last.pitch, sims );
		const bool keep = vz_held( );
		restore( );
		botox_dbg_log( "AS: catch vzpin yaw=%.2f fwd=%.4f keep=%d vxy=%.1f", s_last.yaw, s_last.fwd, ( int )keep, live_velocity.length_2d( ) );
		if ( keep ) {
			publish( s_last.yaw, s_last.fwd, true, s_last.pitch, false, s_last.side );
			s_hold.vzpin  = true;
			s_proof.valid = false;
			return;
		}
	}
	/* a pinned START whose cmd does not re-pin ( offline ) = the one tick that matters most: the whole candidate family, no rotation */
	const bool pinned_search = vel_pinned( live_velocity ) || start_vzpin;

	/* wall: touching ( 4 cardinal 0.05u hull traces, clarity find_wall_cardinal4 ), else the nearest vertical face within reach:
	   along this tick's travel + 8 compass probes of `extra reach` ( grazing = still caught ) */
	using namespace n_as_h;
	const float reach = std::clamp( GET_VARIABLE( g_variables.m_air_stuck_extra_reach, float ), 0.5f, 32.0f );
	trace_t best_tr{ };
	bool found      = false;
	bool touching   = false;
	float best_frac = 2.0f;
	const auto probe = [ & ]( const c_vector& dir, const float len, const bool touch ) {
		trace_t tr{ };
		ray_t ray( origin, c_vector( origin.m_x + dir.m_x * len, origin.m_y + dir.m_y * len, origin.m_z ), mins, maxs );
		g_interfaces.m_engine_trace->trace_ray( ray, mask_playersolid, &filter, &tr );
		if ( !tr.did_hit( ) || tr.m_plane.m_normal.m_z != 0.0f )
			return;
		const float frac = tr.m_start_solid ? 0.0f : tr.m_fraction * len;
		if ( found && frac >= best_frac )
			return;
		found     = true;
		touching  = touch;
		best_frac = frac;
		best_tr   = tr;
	};
	for ( int i = 0; i < 4 && !found; ++i ) {
		const float a = deg2rad( static_cast< float >( i * 90 ) );
		probe( c_vector( std::cos( a ), std::sin( a ), 0.0f ), 0.05f, true );
	}
	if ( !found ) {
		const float vxy = live_velocity.length_2d( );
		if ( vxy > 1.0f )
			probe( c_vector( live_velocity.m_x / vxy, live_velocity.m_y / vxy, 0.0f ), vxy * n_tick::engine_interval( ) + reach, false );
		for ( int i = 0; i < 16; ++i ) {
			const float a = deg2rad( static_cast< float >( i * 22.5f ) );
			probe( c_vector( std::cos( a ), std::sin( a ), 0.0f ), reach, false );
		}
	}
	if ( !found ) {
		was_stuck     = false;
		s_miss_streak = 0;
		if ( lock_tick )
			lock_copy( "nowall" );
		return;
	}
	const plane_t P{ best_tr.m_plane.m_normal, best_tr.m_plane.m_distance };
	const int ent    = best_tr.m_hit_entity ? best_tr.m_hit_entity->get_index( ) : 0;
	const bool world = ent == 0 && best_tr.m_disp_flags == 0;
	const bool axis_wall_pre = std::fabsf( P.n.m_x ) == 1.0f || std::fabsf( P.n.m_y ) == 1.0f;
	const float wall_yaw = rad2deg( std::atan2( -P.n.m_y, -P.n.m_x ) );
	static auto sv_airaccelerate     = g_interfaces.m_convar->find_var( "sv_airaccelerate" );
	static auto sv_air_max_wishspeed = g_interfaces.m_convar->find_var( "sv_air_max_wishspeed" );
	const float dt  = n_tick::engine_interval( );
	const bool duck = ( live_buttons & in_duck ) != 0;
	const accel_t A{ dt, sv_airaccelerate ? sv_airaccelerate->get_float( ) : 12.0f, sv_air_max_wishspeed ? sv_air_max_wishspeed->get_float( ) : 30.0f,
	                 g_ctx.m_local->get_surface_friction( ), duck ? 85.0f : 250.0f, -pin_vz };
	const float coef = A.airaccel * A.dt * A.friction;
	const int cp     = g_interfaces.m_prediction->m_commands_predicted;

	/* engine-verified lean fan: tangent yaws leaned 5 / 10 / 2 deg into the wall x 8 fwds, every candidate a real sim ( <= 40 ). the
	   closed-form model below is world-brush float math only; props / brush entities / displacements ( vcollide and disp traces have
	   their own arithmetic; 11-10 nuke crane ) and exact-axis walls ( clarity's airstuck_run ladders them on contact too, only its
	   no-contact push skips them ) go through the sims. true = a pin was published */
	const c_vector t( -P.n.m_y, P.n.m_x, 0.0f );
	const auto sim_fan = [ & ]( const char* why ) {
		const int cap = pinned_search ? 144 : 48;
		int sims = 0, tried = 0;
		bool got = false, g_fixed = false;
		float g_yaw = 0.0f, g_fwd = 0.0f;
		/* fallback: the slowest vz-pin seen ( xy still moving ), taken only when no full stop exists in the whole sweep */
		float r_xy = FLT_MAX, r_yaw = 0.0f, r_fwd = 0.0f;
		/* ridobebra's family: nearly tangent, 0.001..4 deg in, its 9 fwds; 11-10 11:31 log the engine pinned this wall on fwd 1 at 4 deg
		   while my 5 / 10 / 2 deg leans found nothing. 8 leans x 9 fwds x 2 sides = 144, 48 per tick rotating ( full set in 3 ticks ),
		   drift side first. accept = exact pin only ( velocity ( 0, 0, pin ), origin fixed ): user wants a stuck, never a ride */
		constexpr float k_fwds[ 9 ]  = { 1.0f, 5.0f, 30.0f, 1.3856f, 10.0f, 22.0f, 60.0f, 120.0f, 450.0f };
		constexpr float k_leans[ 8 ] = { 1.0f, 2.0f, 0.5f, 3.0f, 4.0f, 0.111f, 1.5f, 6.0f };
		constexpr int k_total        = 9 * 8 * 2;
		static int s_fan_off         = 0;
		const int side0              = dotv( live_velocity, t ) >= 0.0f ? 0 : 1;
		for ( int k = 0; k < k_total; ++k ) {
			if ( tried >= cap || ( tried > 0 && clock.expired( ) ) )
				break;
			const int idx  = ( ( pinned_search ? 0 : s_fan_off ) + k ) % k_total;
			const int li   = idx / 18, si = ( idx / 9 ) % 2;
			const float f  = k_fwds[ idx % 9 ];
			const int s    = si ^ side0;
			const float sg = s ? -1.0f : 1.0f;
			++tried;
			/* tangent direction sg * t, rotated k_leans toward -n */
			const c_vector w( t.m_x * sg, t.m_y * sg, 0.0f );
			const float lean = ( w.m_x * -P.n.m_y - w.m_y * -P.n.m_x ) > 0.0f ? -k_leans[ li ] : k_leans[ li ];
			const float yaw  = std::remainder( rad2deg( std::atan2( w.m_y, w.m_x ) ) + lean, 360.0f );
			if ( !sim_pins( yaw, f, live_view.m_x, sims ) ) {
				if ( vz_held( ) ) {
					const float xy = g_ctx.m_local->get_velocity( ).length_2d( );
					if ( xy < r_xy ) {
						r_xy  = xy;
						r_yaw = yaw;
						r_fwd = f;
					}
				}
				continue;
			}
			got     = true;
			g_fixed = sim_again( sims );
			g_yaw   = yaw;
			g_fwd   = f;
			break;
		}
		if ( !pinned_search )
			s_fan_off = ( s_fan_off + tried ) % k_total;
		restore( );
		botox_dbg_log( "AS: fan %s ent=%d disp=%d n=%.3f,%.3f d=%.4f pin=%d fixed=%d yaw=%.2f fwd=%.2f vzpin_xy=%.1f tries=%d/%d sims=%d vxy=%.1f vz=%.1f cp=%d us=%lld org=%.7f,%.7f,%.7f vel=%.4f,%.4f pitch=%.2f b=%x surf=%s",
		               why, ent, ( int )best_tr.m_disp_flags, P.n.m_x, P.n.m_y, best_frac, ( int )got, ( int )g_fixed, g_yaw, g_fwd, r_xy < FLT_MAX ? r_xy : -1.0f, tried, cap,
		               sims, live_velocity.length_2d( ), live_velocity.m_z, cp, clock.used_us( ), origin.m_x, origin.m_y, origin.m_z, live_velocity.m_x,
		               live_velocity.m_y, live_view.m_x, live_buttons, best_tr.surface.m_name ? best_tr.surface.m_name : "" );
		if ( got ) {
			publish( g_yaw, g_fwd, true, live_view.m_x, true );
			s_expect = { true, origin };
			return true;
		}
		if ( r_xy < FLT_MAX ) {
			publish( r_yaw, r_fwd, true, live_view.m_x, false );
			s_hold.vzpin  = true;
			s_proof.valid = false;
			return true;
		}
		return false;
	};
	/* no pin from the fan: not touching = ridobebra's gentle approach ( fwd 5 straight in ), touching = the press that pinned this wall
	   ( fwd 1, 4 deg in, drift side ) */
	const auto fan_approach = [ & ]( ) {
		/* pinned START and the whole family falls: keep the cmd that pinned ( server noise online may hold it ) rather than a new press */
		if ( pinned_search && s_last.valid && s_last.cmd == cmd->m_command_number - 1 ) {
			botox_dbg_log( "AS: pinned resend yaw=%.2f fwd=%.4f", s_last.yaw, s_last.fwd );
			publish( s_last.yaw, s_last.fwd, true, s_last.pitch, false, s_last.side );
			s_proof.valid = false;
			s_expect      = { true, origin };
			return;
		}
		if ( best_frac > 0.05f ) {
			publish( wall_yaw, 5.0f, false, live_view.m_x, false );
			return;
		}
		/* touching, nothing pins: the pins this wall gives came at tangent speed 90-200 ( 11-10 12:20 log; at xy 0.1 the fan found none
		   for 40 ticks ). build speed along the wall, 1 deg in, drift side ( +22 u/s per tick ) */
		const float sg = dotv( live_velocity, t ) >= 0.0f ? 1.0f : -1.0f;
		publish( std::remainder( wall_yaw + sg * 89.0f, 360.0f ), live_velocity.length_2d( ) < 60.0f ? 120.0f : 1.0f, false, live_view.m_x, false );
	};
	if ( lock_tick && ( !world || axis_wall_pre ) ) {
		lock_copy( world ? "axis" : "ent" );
		return;
	}
	if ( !world ) {
		if ( !sim_fan( "ent" ) )
			fan_approach( );
		return;
	}

	/* exact-axis brush: the box-brush trace ( coll.cpp IntersectRayWithBoxBrush ) has no rounding to cross; velocity zeroes there only
	   as allsolid with the face EXACTLY on the plane, which the server's CheckStuck nudges out of in 0.05 s. the catch on these walls is
	   texturebug's ride ( vz pin + xy slide ): arm it for this tick ( texturebug.cpp `armed`, runs after us ) */
	/* exact-axis world face: the plane model predicts no crossing, yet the game pins there ( 11-10 nuke crane ) - every one of those pins
	   came with the hull 1-2 ulps off the face ( y -1021.9997 ), none from REST ( 12:29 log: 40 ticks at gap 0.03..0.0008, 0 pins ). so:
	   the lattice park below puts the START 1 ulp off ( it picks the smallest positive true d when nothing is predicted to cross ), and
	   only in the skin the engine-verified fan runs. no pin there = keep the tangent speed up, never texturebug's ride */
	const bool axis_wall = std::fabsf( P.n.m_x ) == 1.0f || std::fabsf( P.n.m_y ) == 1.0f;
	const hull_t H{ ( maxs - mins ) * 0.5f, ( mins + maxs ) * 0.5f };
	const float fwd_cap = duck ? 80.0f : 160.0f;
	const float d1      = eng_d( origin, P, H );
	const double dt1    = true_d( origin, P, H );
	const float vn      = dotv( live_velocity, P.n );
	if ( axis_wall && d1 > 0.0f && d1 <= 2.5f * grid_q( origin, P, H ) ) {
		if ( !sim_fan( "axis" ) )
			fan_approach( );
		return;
	}

	/* park window: the cmds after a park repeat it until the server acks the park itself ( consecutive cmds, START not pinned = the lock
	   owns a pinned one ). no client pin may cut it: 18:20 log, the server pinned on the park 67/113 times the window was cut by a
	   verified sc 2 pin, all 67 knocked off at t=1; the only long lock ( 73t ) came from a window that ran to its ack. ack, not cp:
	   the park's ack landed after a cp-tick window 2x ( Q already on the wire ) */
	const int ack = g_interfaces.m_client_state->m_last_command_ack;
	if ( s_park.valid && ( cmd->m_command_number != s_park.cmd + 1 + s_park.ticks || ack >= s_park.cmd || s_park.ticks >= 2 * cp + 2 || vel_pinned( live_velocity ) ) )
		s_park.valid = false;
	const bool park_window = s_park.valid;
	const auto park_resend = [ & ]( ) {
		++s_park.ticks;
		botox_dbg_log( "AS: park resend t=%d/%d yaw=%.2f fwd=%.4f d=%.3g vxy=%.1f vz=%.1f", s_park.ticks, cp, s_park.yaw, s_park.fwd, d1, live_velocity.length_2d( ),
		               live_velocity.m_z );
		publish( s_park.yaw, s_park.fwd, false, live_view.m_x, false );
		s_proof.valid = false;
	};

	/* locked: the server is pinned at s_lock.origin. solve the fixed point THERE ( closed form from ( 0, 0, pin ), verified at that
	   origin ), else copy its cmd */
	if ( lock_tick ) {
		std::vector< cand_t > lc;
		lc.reserve( 128 );
		const c_vector pinned_v( 0.0f, 0.0f, pin_vz );
		build( s_lock.origin, pinned_v, P, H, A, pin_vz, fwd_cap, 0.003f, &lc, 120 );
		std::stable_sort( lc.begin( ), lc.end( ), [ ]( const cand_t& a, const cand_t& b ) { return a.score != b.score ? a.score > b.score : a.a < b.a; } );
		int sims = 0, tried = 0;
		for ( const cand_t& c : lc ) {
			if ( c.score < 2 || tried >= 16 || ( tried > 0 && clock.expired( ) ) )
				break;
			++tried;
			if ( !sim_at( s_lock.origin, pinned_v, c.yaw, c.fwd, sims ) )
				continue;
			restore( );
			++s_lock.ticks;
			botox_dbg_log( "AS: lock pin t=%d yaw=%.2f fwd=%.4f try=%d sims=%d cands=%zu d_sv=%.3g", s_lock.ticks, c.yaw, c.fwd, tried, sims, lc.size( ),
			               eng_d( s_lock.origin, P, H ) );
			publish( c.yaw, c.fwd, true, live_view.m_x, true );
			s_proof.valid  = false; /* proven at the server's START, not this client START */
			s_expect.valid = false;
			return;
		}
		/* diag: the acked cmd as it went on the wire, from the server's pinned state. copy=1 + `lock end fell` = client physics != server
		   there; copy=0 + fell = same physics, the server pin was a 1-tick pin ( 18:20 log: 6/7 window locks fell t<=2 on the same cmd ) */
		g_prediction.restore_entity_to_predicted_frame( cp - 1 );
		g_ctx.m_local->set_abs_origin( s_lock.origin );
		g_ctx.m_local->get_origin( )   = s_lock.origin;
		g_ctx.m_local->get_velocity( ) = pinned_v;
		if ( int& eflags = g_ctx.m_local->get_eflags( ); reinterpret_cast< std::uintptr_t >( &eflags ) != reinterpret_cast< std::uintptr_t >( g_ctx.m_local ) )
			eflags |= 1 << 12;
		cmd->m_view_point   = s_lock.view;
		cmd->m_forward_move = s_lock.fwd;
		cmd->m_side_move    = s_lock.side;
		cmd->m_buttons      = live_buttons;
		g_prediction.begin( g_ctx.m_local, cmd );
		g_prediction.end( g_ctx.m_local );
		const bool copy_pins = pin_held( ) && same_o( g_ctx.m_local->get_abs_origin( ), s_lock.origin );
		restore( );
		botox_dbg_log( "AS: lock d_sv=%.3g true_sv=%.3g d_cl=%.3g cands=%zu copy=%d", eng_d( s_lock.origin, P, H ), true_d( s_lock.origin, P, H ), d1, lc.size( ),
		               ( int )copy_pins );
		lock_copy( tried ? "nofix" : "nocand" );
		return;
	}

	/* pin: predicted crossings from this START, verified ( tick 1 + the fixed point ), best first. axis wall: nothing to predict, park */
	std::vector< cand_t > cands;
	cands.reserve( 128 );
	if ( !axis_wall && !park_window )
		build( origin, live_velocity, P, H, A, pin_vz, fwd_cap, 0.003f, &cands, 120 );
	if ( !cands.empty( ) ) {
		std::stable_sort( cands.begin( ), cands.end( ), [ ]( const cand_t& a, const cand_t& b ) { return a.score != b.score ? a.score > b.score : a.a < b.a; } );
		int sims = 0, tried = 0;
		for ( const cand_t& c : cands ) {
			/* a pinned START gets 4x the verified tries ( losing it costs the hold ), a wall that already refused the model 3x */
			if ( tried >= ( pinned_search ? 16 : s_miss_streak ? 12 : 4 ) || ( tried > 0 && clock.expired( ) ) )
				break;
			++tried;
			if ( !sim_pins( c.yaw, c.fwd, live_view.m_x, sims ) )
				continue;
			const bool fixd = sim_again( sims );
			restore( );
			botox_dbg_log( "AS: pin yaw=%.2f fwd=%.4f a=%.3f sc=%d fixed=%d d=%.3g true=%.3g q=%.3g vn=%.3f vxy=%.1f vz=%.1f cands=%zu try=%d sims=%d cp=%d touch=%d surf=%s us=%lld",
			               c.yaw, c.fwd, c.a, c.score, ( int )fixd, d1, dt1, grid_q( origin, P, H ), vn, live_velocity.length_2d( ), live_velocity.m_z, cands.size( ), tried,
			               sims, cp, ( int )touching, best_tr.surface.m_name ? best_tr.surface.m_name : "", clock.used_us( ) );
			publish( c.yaw, c.fwd, true, live_view.m_x, true );
			s_expect      = { true, origin };
			s_miss_streak = 0;
			s_park.valid  = false;
			return;
		}
		restore( );
		++s_miss_streak;
		botox_dbg_log( "AS: pin miss d=%.3g true=%.3g cands=%zu try=%d sims=%d streak=%d us=%lld", d1, dt1, cands.size( ), tried, sims, s_miss_streak, clock.used_us( ) );
		/* the model keeps missing here: the engine decides ( same fan as entity / axis walls ) */
		if ( s_miss_streak >= 2 && sim_fan( "pred" ) ) {
			s_miss_streak = 0;
			return;
		}
	}
	if ( park_window ) {
		park_resend( );
		return;
	}
	/* pinned START, nothing re-proven: never park a hull that sits on a fixed point ( 12:37 log: park to=1.5e-07 then `lost` ). keep the cmd */
	if ( pinned_search && s_last.valid && s_last.cmd == cmd->m_command_number - 1 ) {
		botox_dbg_log( "AS: pinned resend yaw=%.2f fwd=%.4f", s_last.yaw, s_last.fwd );
		publish( s_last.yaw, s_last.fwd, true, s_last.pitch, false, s_last.side );
		s_proof.valid = false;
		s_expect      = { true, origin };
		return;
	}
	/* park: next tick's START on the xy float lattice near ( drift, then -n onto d = q/2 ), scored by its own predicted crossings.
	   coords ~2000 step 2.4e-4 per axis, so a 1-D push along -n alone often has no point that can pin */
	c_vector v0 = live_velocity;
	v0.m_z -= A.grav_half;
	const float q0   = grid_q( origin, P, H );
	const float back = static_cast< float >( dt1 + dt * vn - 0.5 * q0 );
	const float tx   = origin.m_x + v0.m_x * dt - P.n.m_x * back;
	const float ty   = origin.m_y + v0.m_y * dt - P.n.m_y * back;
	const float ux = ulp( tx ), uy = ulp( ty );
	int best_sc    = -1;
	double best_t  = 1e9;
	float best_fwd = -1.0f, best_yaw = wall_yaw;
	int lattice    = 0;
	const c_vector tdir( -P.n.m_y, P.n.m_x, 0.0f );
	/* pass 0: the 13x13 ulp lattice. pass 1 ( nothing there can pin, 11-10 log: 42 ticks of sc=0 creeping 1 ulp ): the same depth shifted
	   along the wall by k * 0.02u, the rounding phase of the big product changes with x ( near-axis walls have no jitter otherwise ) */
	for ( int pass = 0; pass < 2 && best_sc <= 0; ++pass )
	for ( int i = -6; i <= 6 && !( lattice > 0 && clock.expired( ) ); ++i )
		for ( int j = -6; j <= 6; ++j ) {
			if ( pass == 1 && ( i == 0 || j < -1 || j > 1 ) )
				continue;
			const float sh = pass == 1 ? 0.02f * i : 0.0f;
			const c_vector end( tx + ( pass ? j : i ) * ux + tdir.m_x * sh, ty + j * uy + tdir.m_y * sh, origin.m_z + v0.m_z * dt );
			/* velocity that lands there, as a wish: v = v0 + a * w */
			const float dvx = ( end.m_x - origin.m_x ) / dt - v0.m_x, dvy = ( end.m_y - origin.m_y ) / dt - v0.m_y;
			const float a   = std::sqrt( dvx * dvx + dvy * dvy );
			if ( a < 1e-6f )
				continue;
			const c_vector w( dvx / a, dvy / a, 0.0f );
			const float ws  = a / coef;
			const float add = std::min( ws, A.max_wish ) - dotv( v0, w );
			if ( ws > 450.0f || a > add )
				continue;
			const float y     = rad2deg( std::atan2( w.m_y, w.m_x ) );
			const c_vector v  = accel( live_velocity, y, ws, A );
			const c_vector nx( origin.m_x + v.m_x * dt, origin.m_y + v.m_y * dt, origin.m_z + v.m_z * dt );
			if ( eng_d( nx, P, H ) <= 0.0f )
				continue;
			const double t2 = true_d( nx, P, H );
			if ( t2 <= 0.0 )
				continue;
			++lattice;
			/* coarse sweep: a density estimate ranks STARTs as well as the fine one ( lab 29-33/33 either way ) at 1/10 the evals */
			const int sc = build( nx, v, P, H, A, pin_vz, fwd_cap, 0.1f, nullptr, 16 );
			if ( sc > best_sc || ( sc == best_sc && t2 < best_t ) ) {
				best_sc  = sc;
				best_t   = t2;
				best_fwd = ws;
				best_yaw = y;
			}
		}
	if ( best_fwd < 0.0f ) {
		/* out of reach this tick: max approach, straight in. online the server pins on the impact cmd itself ( 17:11 log: `sv lock`
		   on cmd fwd 71 / 49 with the server 1-3 ticks higher than us ) = same in-flight window as a park */
		botox_dbg_log( "AS: approach yaw=%.1f d=%.4f vn=%.2f vxy=%.1f touch=%d us=%lld", wall_yaw, dt1, vn, live_velocity.length_2d( ), ( int )touching, clock.used_us( ) );
		publish( wall_yaw, 450.0f, false, live_view.m_x, false );
		s_park = { cp > 0 && dt1 < 2.0 * live_velocity.length_2d( ) * dt + 2.0, cmd->m_command_number, 0, wall_yaw, 450.0f };
		return;
	}
	botox_dbg_log( "AS: park yaw=%.2f fwd=%.4f sc=%d to=%.3g d=%.3g true=%.3g q=%.3g vn=%.3f vxy=%.1f vz=%.1f lat=%d cp=%d touch=%d us=%lld", best_yaw, best_fwd, best_sc,
	               best_t, d1, dt1, q0, vn, live_velocity.length_2d( ), live_velocity.m_z, lattice, cp, ( int )touching, clock.used_us( ) );
	publish( best_yaw, best_fwd, false, live_view.m_x, false );
	s_park = { cp > 0, cmd->m_command_number, 0, best_yaw, best_fwd };
}

void air_stuck_final_stamp( c_user_cmd* cmd )
{
	if ( !g_air_stuck_stamp_valid || !cmd )
		return;

	float stamped_forward = 0.0f;
	float stamped_side    = 0.0f;
	stamp_rotate( cmd->m_view_point.m_y, g_air_stuck_stamp_yaw, g_air_stuck_stamp_forward, g_air_stuck_stamp_side, stamped_forward,
	              stamped_side );

	static bool was_drifting = false;
	const float drift = std::max( std::fabsf( cmd->m_forward_move - stamped_forward ), std::fabsf( cmd->m_side_move - stamped_side ) );
	const bool drifting = drift > 0.5f;
	if ( drifting != was_drifting ) {
		was_drifting = drifting;
		if ( drifting && GET_VARIABLE( g_variables.m_air_stuck_display_screen, bool ) )
			movement_add_window( 5, std::format( "as: stamp fixed drift {:.1f} (fwd {:.1f} -> {:.1f})", drift, cmd->m_forward_move,
			                                          stamped_forward ) );
	}

	cmd->m_forward_move = stamped_forward;
	cmd->m_side_move    = stamped_side;
	cmd->m_up_move      = 0.0f;

	constexpr int direction_mask = in_moveright | in_moveleft | in_back | in_forward | in_duck;
	cmd->m_buttons               = ( cmd->m_buttons & ~direction_mask ) | ( g_air_stuck_stamp_buttons & direction_mask );
}

/* last writer: the pin was proven in the stamp ( pin yaw ) view; the sent cmd keeps your view, so its rotated move is re-proven here
   every tick ( silent ). nothing pins in your view -> most robust pinned move, else the sent move; the view is never forced */
void air_stuck_shot_proof( c_user_cmd* cmd )
{
	const bool armed = s_proof.valid;
	s_proof.valid    = false;
	if ( !armed || !cmd || !g_air_stuck_stamp_valid || !g_ctx.m_local || !g_ctx.m_local->is_alive( ) )
		return;
	if ( cmd->m_view_point.m_x == g_air_stuck_stamp_pitch && cmd->m_view_point.m_y == g_air_stuck_stamp_yaw &&
	     cmd->m_forward_move == g_air_stuck_stamp_forward && cmd->m_side_move == g_air_stuck_stamp_side )
		return;
	/* zero-move pin: wishvel 0 in every view, the proof already covers this cmd */
	if ( g_air_stuck_stamp_forward == 0.0f && g_air_stuck_stamp_side == 0.0f && cmd->m_forward_move == 0.0f && cmd->m_side_move == 0.0f )
		return;

	c_base_entity* local    = g_ctx.m_local;
	/* same leaf-list bounds as the real run ( see air_stuck ) */
	struct bounds_guard_t {
		bounds_guard_t( ) { g_prediction.m_bounds_max_speed = g_prediction.m_real_max_speed; }
		~bounds_guard_t( ) { g_prediction.m_bounds_max_speed = 0.f; }
	} bounds_guard;
	const float pin_vz      = g_prediction.get_engine_target_predict_z_velocity( );
	const float sent_f      = cmd->m_forward_move;
	const float sent_s      = cmd->m_side_move;
	const c_angle sent_view = cmd->m_view_point;

	const auto reset = [ & ]( ) {
		g_prediction.restore_entity_to_predicted_frame( g_interfaces.m_prediction->m_commands_predicted - 1 );
		local->set_abs_origin( s_proof.origin );
		local->get_origin( )   = s_proof.origin;
		local->get_velocity( ) = s_proof.velocity;
	};
	const auto held = [ & ]( ) {
		if ( ( local->get_flags( ) & fl_onground ) || local->get_move_type( ) != move_type_walk )
			return false;
		const c_vector v = local->get_velocity( );
		return s_proof.exact ? v.m_x == 0.0f && v.m_y == 0.0f && v.m_z == pin_vz : g_prediction.is_target_predict_z_velocity( v.m_z, 0.05f );
	};
	const auto same = [ ]( const c_vector& a, const c_vector& b ) { return a.m_x == b.m_x && a.m_y == b.m_y && a.m_z == b.m_z; };

	int sims = 0;
	/* fixed point = next tick replays the sent cmd from the same state; else replay it 2 ticks */
	const auto pins = [ & ]( const float f, const float s ) {
		reset( );
		cmd->m_view_point   = sent_view;
		cmd->m_forward_move = f;
		cmd->m_side_move    = s;
		g_prediction.begin( local, cmd );
		g_prediction.end( local );
		++sims;
		if ( !held( ) )
			return false;
		if ( same( local->get_abs_origin( ), s_proof.origin ) && same( local->get_velocity( ), s_proof.velocity ) )
			return true;
		for ( int t = 0; t < 2; ++t ) {
			g_prediction.begin( local, cmd );
			g_prediction.end( local );
			++sims;
			if ( !held( ) )
				return false;
		}
		return true;
	};

	float rot_f = 0.0f, rot_s = 0.0f;
	stamp_rotate( cmd->m_view_point.m_y, g_air_stuck_stamp_yaw, g_air_stuck_stamp_forward, g_air_stuck_stamp_side, rot_f, rot_s );
	const auto ulp = [ ]( float x, int k ) {
		for ( ; k > 0; --k )
			x = std::nextafter( x, FLT_MAX );
		for ( ; k < 0; ++k )
			x = std::nextafter( x, -FLT_MAX );
		return x;
	};

	/* online: server keeps only pins that survive its float noise ( clarity robust( ) ); a swapped move must be as robust as the stamp */
	const auto robust = [ & ]( const c_angle& view, const float f, const float s ) {
		int n = 0;
		for ( int k = 0; k < 6; ++k ) {
			reset( );
			c_vector o = s_proof.origin;
			float& c   = k < 2 ? o.m_x : k < 4 ? o.m_y : o.m_z;
			c          = std::nextafter( c, ( k & 1 ) ? FLT_MAX : -FLT_MAX );
			local->set_abs_origin( o );
			local->get_origin( ) = o;
			cmd->m_view_point    = view;
			cmd->m_forward_move  = f;
			cmd->m_side_move     = s;
			bool ok = true;
			for ( int t = 0; t < 2 && ok; ++t ) {
				g_prediction.begin( local, cmd );
				g_prediction.end( local );
				++sims;
				ok = held( );
			}
			n += ok;
		}
		return n;
	};

	static struct {
		c_vector origin{ }, velocity{ };
		float pitch = 0.0f, yaw = 0.0f, f = 0.0f, s = 0.0f;
		int buttons = 0, rob = -1;
	} s_need;
	/* bar on every tick: 10-07 23:18 log 283/330 shot pins took 1 sim rob 0, holds died 39-89t into a spray */
	if ( !same( s_need.origin, s_proof.origin ) || !same( s_need.velocity, s_proof.velocity ) || s_need.pitch != g_air_stuck_stamp_pitch ||
	     s_need.yaw != g_air_stuck_stamp_yaw || s_need.f != g_air_stuck_stamp_forward || s_need.s != g_air_stuck_stamp_side ||
	     s_need.buttons != cmd->m_buttons || s_need.rob < 0 ) {
		c_angle stamp_view = sent_view;
		stamp_view.m_x     = g_air_stuck_stamp_pitch;
		stamp_view.m_y     = g_air_stuck_stamp_yaw;
		s_need = { s_proof.origin, s_proof.velocity, g_air_stuck_stamp_pitch, g_air_stuck_stamp_yaw, g_air_stuck_stamp_forward, g_air_stuck_stamp_side,
		           cmd->m_buttons, robust( stamp_view, g_air_stuck_stamp_forward, g_air_stuck_stamp_side ) };
	}
	const int need = s_need.rob;

	n_tick::c_sim_budget clock;
	clock.start( GET_VARIABLE( g_variables.m_air_stuck_budget, float ) * 0.01f, n_tick::engine_interval( ), n_tick::search_as );
	constexpr int k_floor = 24;
	constexpr int k_cap   = 64;
	int tries = 0, keep = -1, best_rob = -1;
	float out_f = sent_f, out_s = sent_s, best_f = sent_f, best_s = sent_s;
	const auto attempt = [ & ]( const float f, const float s ) {
		if ( keep >= 0 || tries >= k_cap || ( tries >= k_floor && clock.expired( ) ) )
			return;
		++tries;
		if ( !pins( f, s ) )
			return;
		if ( need > 0 ) {
			if ( best_rob >= 0 && clock.expired( ) )
				return;
			const int rob = robust( sent_view, f, s );
			if ( rob > best_rob ) {
				best_rob = rob;
				best_f   = f;
				best_s   = s;
			}
			if ( rob < need )
				return;
		}
		keep  = tries;
		out_f = f;
		out_s = s;
	};

	attempt( sent_f, sent_s );
	if ( rot_f != sent_f || rot_s != sent_s )
		attempt( rot_f, rot_s );
	for ( int k = 1; k <= 6; ++k ) {
		attempt( rot_f, ulp( rot_s, k ) );
		attempt( rot_f, ulp( rot_s, -k ) );
	}
	for ( int k = 1; k <= 3; ++k ) {
		attempt( ulp( rot_f, k ), rot_s );
		attempt( ulp( rot_f, -k ), rot_s );
	}
	for ( int j = 1; j <= 8; ++j ) {
		for ( const float m : { 1.0f + j / 16.0f, 1.0f - j / 16.0f } ) {
			float f = 0.0f, s = 0.0f;
			stamp_rotate( cmd->m_view_point.m_y, g_air_stuck_stamp_yaw, g_air_stuck_stamp_forward * m, g_air_stuck_stamp_side * m, f, s );
			attempt( f, s );
		}
	}
	attempt( 0.0f, 0.0f );

	/* nothing as robust as the stamp: the most robust pinned move beats the unproven sent one ( keep=0 ) */
	if ( keep < 0 && best_rob >= 0 ) {
		keep  = 0;
		out_f = best_f;
		out_s = best_s;
	}

	reset( );
	cmd->m_view_point   = sent_view;
	cmd->m_forward_move = out_f;
	cmd->m_side_move    = out_s;
	g_prediction.begin( local, cmd, true );
	g_prediction.end( local );

	static int s_last_kept = -1;
	const int kept         = keep >= 0 ? 1 : 0;
	if ( kept == s_last_kept && !g_aimbot.shot_leaves( ) )
		return;
	s_last_kept = kept;

	botox_dbg_log( "AS: shot keep=%d rob=%d/%d tries=%d sims=%d exact=%d fwd=%.4f/%.4f -> %.4f/%.4f view=%.2f/%.2f stamp=%.2f/%.2f av=%.2f/%.2f us=%lld",
	               keep, best_rob, need, tries, sims, ( int )s_proof.exact, sent_f, sent_s, out_f, out_s, cmd->m_view_point.m_x, cmd->m_view_point.m_y,
	               g_air_stuck_stamp_pitch, g_air_stuck_stamp_yaw, g_air_stuck_authored_view.m_x, g_air_stuck_authored_view.m_y, clock.used_us( ) );
}
