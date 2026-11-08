#include "movement_internal.h"
#include "texturebug_common.h"
#include <algorithm>
#include <cmath>
#include <format>
#include "../../utilities/perf/perf_watch.h"
#include "../aimbot/aimbot.h"

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
} s_lock;

/* flush on the wall, no proven pin: resend the same approach move ( 11-05 online log: 17 server pins on approach cmds the client saw fall,
   all died 1-5t later on the next in-flight cmd = lumi side flip / fwd 1-5-30 hop ). same cmd after a server pin = kept 35/35.
   side: a lock move carries one */
static struct {
	bool valid = false;
	float yaw = 0.0f, fwd = 0.0f, side = 0.0f;
	c_vector normal{ };
	int ticks = 0;
} s_sticky;

/* last cmd we sent ( pin frame ). START pinned by it + nothing re-proves = resend it: same cmd after a server pin = kept 35/35 ( 11-04
   online ); 11-05 09:20 offline log: 2 lumi 1-tick pins the server took were lost to a fresh approach move the next tick */
static struct {
	int cmd = -1, resent = 0;
	float pitch = 0.0f, yaw = 0.0f, fwd = 0.0f, side = 0.0f;
} s_last;

static void lock_end( const char* why )
{
	if ( s_lock.valid )
		botox_dbg_log( "AS: sv lock end t=%d moved=%d why=%s", s_lock.ticks, s_lock.moved, why );
	s_lock.valid = false;
}

/* online the engine keeps its own START while the server's END differs < 0.0625u / 0.5u/s ( cl_pred_optimize 2, c_baseentity.cpp:656/681,
   prediction.cpp:1681 ); pins are exact floats, so armed = full re-predict off the server snapshot every packet ( offline already is ).
   log: server END of the acked cmd vs the START we built on */
static void as_sync( c_user_cmd* cmd, const bool armed )
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
	if ( !sv_pinned ) {
		/* fell on an older in-flight cmd, the lock's own copies still coming = keep sending the caught move ( 11-08 16:24 log: 11/24 lock
		   deaths = next cmd differed, lock copy re-pinned at 34897 then a fresh pick knocked it off ). fell on the lock cmd = re-pick */
		if ( s_lock.valid && ( a->m_forward_move != s_lock.fwd || a->m_side_move != s_lock.side || a->m_view_point.m_y != s_lock.view.m_y ) ) {
			s_sticky = { true, s_lock.view.m_y, s_lock.fwd, s_lock.side, s_sticky.normal, 0 };
			botox_dbg_log( "AS: sticky lock yaw=%.1f fwd=%.4f side=%.4f", s_lock.view.m_y, s_lock.fwd, s_lock.side );
		}
		lock_end( "fell" );
	} else if ( !match || s_lock.valid ) {
		if ( !s_lock.valid ) {
			s_lock.ticks = s_lock.moved = 0;
			botox_dbg_log( "AS: sv lock ack=%d cmd=%.4f/%.4f v=%.4f/%.4f", ack, a->m_forward_move, a->m_side_move, a->m_view_point.m_x, a->m_view_point.m_y );
		}
		s_lock.valid = true;
		s_lock.view  = a->m_view_point;
		s_lock.fwd   = a->m_forward_move;
		s_lock.side  = a->m_side_move;
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

void n_movement::impl_t::air_stuck( c_user_cmd* cmd )
{
	PERF_ZONE( zone_cmd_air_stuck );
	static float start_angle = 0.0f;
	static bool was_stuck    = false;

	s_proof.valid = false;
	const bool hold_armed = s_hold.valid;
	s_hold.valid = false;
	const int saved_auto_align_block_until_tick = m_air_stuck_data.m_auto_align_block_until_tick;
	m_air_stuck_data.reset( );
	m_air_stuck_data.m_auto_align_block_until_tick = saved_auto_align_block_until_tick;

	const bool feature = GET_VARIABLE( g_variables.m_air_stuck, bool );
	const bool key     = g_input.check_input( &GET_VARIABLE( g_variables.m_air_stuck_key, key_bind_t ) );
	const bool alive   = g_ctx.m_local && g_ctx.m_local->is_alive( );
	as_sync( cmd, feature && key && alive );

	if ( !feature || !key || !alive || ( g_ctx.m_local->get_flags( ) & fl_onground ) ) {
		if ( hold_armed )
			botox_dbg_log( "AS: end t=%d why=%s blk=%d", s_hold.ticks, !feature ? "off" : !key ? "key" : !alive ? "dead" : "ground", ( int )g_input.keys_blocked( ) );
		s_hold.ticks   = 0;
		was_stuck      = false;
		s_expect.valid = false;
		s_sticky.valid = false;
		return;
	}
	const int move_type = g_ctx.m_local->get_move_type( );
	if ( move_type == move_type_ladder || move_type == move_type_noclip || move_type == move_type_fly || move_type == move_type_observer ) {
		was_stuck      = false;
		s_expect.valid = false;
		s_sticky.valid = false;
		return;
	}

	n_tick::c_sim_budget scan_clock;
	scan_clock.start( 0.9f, n_tick::engine_interval( ) );

	constexpr float k_pi          = 3.14159265358979323846f;
	const c_vector origin         = g_ctx.m_local->get_abs_origin( );
	const c_vector live_velocity  = g_ctx.m_local->get_velocity( ); /* not backup_data: stale one cmd behind on choked/hitched cmds */
	const c_angle live_view       = cmd->m_view_point;
	const int live_buttons        = cmd->m_buttons;
	const float live_fwd          = cmd->m_forward_move;
	const float live_side         = cmd->m_side_move;
	const auto mins               = g_ctx.m_local->get_collideable( )->get_obb_mins( );
	const auto maxs               = g_ctx.m_local->get_collideable( )->get_obb_maxs( );
	const float ext_x             = std::max( std::fabsf( mins.m_x ), std::fabsf( maxs.m_x ) );
	const float ext_y             = std::max( std::fabsf( mins.m_y ), std::fabsf( maxs.m_y ) );

	c_trace_filter_trace_type_everything_filter_props filter;
	const auto line = [ & ]( const c_vector& from, const c_vector& to, const unsigned int mask ) {
		trace_t tr{ };
		ray_t ray( from, to );
		g_interfaces.m_engine_trace->trace_ray( ray, mask, &filter, &tr );
		return tr;
	};

	const auto apply_predicted_state = [ & ]( ) noexcept {
		g_ctx.m_local->set_abs_origin( origin );
		g_ctx.m_local->get_origin( )   = origin;
		g_ctx.m_local->get_velocity( ) = live_velocity;
	};

	/* silent: the sent cmd always keeps your view; the proven move ( pin yaw frame ) is rotated into it and air_stuck_shot_proof re-proves
	   the final cmd ( 10-30 log: 125/125 live-view re-proofs pinned, <= 18 ulp tries ). zero move: wishvel 0, AirAccelerate returns on
	   addspeed 0 ( gamemovement.cpp:2028/1985 ) = any view sims the same */
	const auto publish = [ & ]( const float yaw, const float move, const bool stuck, const float pitch, const bool exact, const float side = 0.0f ) {
		if ( stuck )
			s_sticky.valid = false;
		s_last                    = { cmd->m_command_number, 0, pitch, yaw, move, side };
		s_proof                   = { stuck, exact, origin, live_velocity };
		s_hold.valid              = stuck;
		s_hold.pitch              = pitch;
		s_hold.yaw                = yaw;
		s_hold.fwd                = move;
		s_hold.side               = side;
		cmd->m_view_point         = live_view;
		stamp_rotate( live_view.m_y, yaw, move, side, cmd->m_forward_move, cmd->m_side_move );
		/* use movement fix off ( ridobebra apply_air_stuck_view ): send the proof cmd as simmed, pin view + raw move, not silent */
		g_air_stuck_view_forced = !GET_VARIABLE( g_variables.m_movement_fix, bool );
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
	const auto pinned     = [ ]( ) { return g_prediction.is_target_predict_z_velocity( g_ctx.m_local->get_velocity( ).m_z, 0.05f ); };
	const auto pin_held   = [ & ]( ) {
		return !( g_ctx.m_local->get_flags( ) & fl_onground ) && g_ctx.m_local->get_move_type( ) == move_type_walk && vel_pinned( g_ctx.m_local->get_velocity( ) );
	};
	/* the server ( offline: START == server ) zeroed us last tick */
	const bool start_pinned = vel_pinned( live_velocity );

	/* pins ranked by how long the move keeps pinning; a repeat ( END == START ) is a fixed point = holds for good = k_chain.
	   k_ahead = ticks a held pin is replayed ahead, a fall inside it = search early from the still-pinned START */
	constexpr int k_chain = 16;
	constexpr int k_ahead = 4;
	const auto bits_eq    = [ ]( const c_vector& a, const c_vector& b ) { return a.m_x == b.m_x && a.m_y == b.m_y && a.m_z == b.m_z; };

	/* server rounding != ours: a moving-START catch only ports if its first tick pins from START +-1 ulp on every origin / velocity axis
	   ( 10-10 offline log: 71/83 sim-proven catches fell, pinned-START ones all held ). early out once it cannot beat `need` */
	constexpr int k_crob  = 12;
	const auto catch_rob = [ & ]( int& sims, const int need, const n_tick::c_sim_budget& clk ) {
		int n = 0;
		for ( int k = 0; k < k_crob && n + k_crob - k > need && !( k > 0 && clk.expired( ) ); ++k ) {
			c_vector o = origin, v = live_velocity;
			c_vector& w = k < 6 ? o : v;
			const int a = ( k % 6 ) >> 1;
			float& c    = a == 0 ? w.m_x : a == 1 ? w.m_y : w.m_z;
			c           = std::nextafter( c, ( k & 1 ) ? FLT_MAX : -FLT_MAX );
			g_prediction.restore_entity_to_predicted_frame( g_interfaces.m_prediction->m_commands_predicted - 1 );
			g_ctx.m_local->set_abs_origin( o );
			g_ctx.m_local->get_origin( )   = o;
			g_ctx.m_local->get_velocity( ) = v;
			g_prediction.begin( g_ctx.m_local, cmd );
			g_prediction.end( g_ctx.m_local );
			++sims;
			n += pin_held( );
		}
		return n;
	};

	/* server pinned, our sim can't prove its pins ( it predicts the fall ) = resend the server's cmd, no client proof / search.
	   silent: exact wire cmd while your view is still, else rotated ( `moved=` ) */
	if ( s_lock.valid ) {
		s_expect.valid = false;
		s_hold.ticks   = 0;
		publish( s_lock.view.m_y, s_lock.fwd, true, s_lock.view.m_x, true, s_lock.side );
		s_proof.valid = false;
		s_hold.valid  = false;
		++s_lock.ticks;
		s_lock.moved += !g_air_stuck_view_forced && ( live_view.m_x != s_lock.view.m_x || live_view.m_y != s_lock.view.m_y );
		return;
	}

	/* last tick sent a proven pin that did not hold: d = real START - our sim end */
	if ( s_expect.valid ) {
		s_expect.valid = false;
		if ( !g_prediction.is_target_predict_z_velocity( live_velocity.m_z, 0.05f ) ) {
			const c_vector d = origin - s_expect.origin;
			botox_dbg_log( "AS: lost d=%.6f,%.6f,%.6f vel=%.3f,%.3f,%.3f cp=%d", d.m_x, d.m_y, d.m_z, live_velocity.m_x, live_velocity.m_y, live_velocity.m_z,
			               g_interfaces.m_prediction->m_commands_predicted );
		}
	}

	/* hold: START pinned by last tick's pin = replay it in the latched view, 1 sim proves this tick ( clarity + lumi; was 0 sims / a full fan ).
	   break = search below from this still-pinned START, never send a cmd that falls.
	   ahead = pins now, replay falls within k_ahead ticks: search now, keep the hold unless a pin that lasts longer turns up */
	bool ahead  = false;
	int ahead_h = 0;
	c_vector ahead_end{ };
	if ( hold_armed && vel_pinned( live_velocity ) ) {
		g_prediction.restore_entity_to_predicted_frame( g_interfaces.m_prediction->m_commands_predicted - 1 );
		apply_predicted_state( );
		cmd->m_view_point.m_x = s_hold.pitch;
		cmd->m_view_point.m_y = s_hold.yaw;
		cmd->m_forward_move   = s_hold.fwd;
		cmd->m_side_move      = s_hold.side;
		cmd->m_buttons        = live_buttons;
		g_prediction.begin( g_ctx.m_local, cmd );
		g_prediction.end( g_ctx.m_local );
		const c_vector end = g_ctx.m_local->get_abs_origin( );
		const bool held    = !( g_ctx.m_local->get_flags( ) & fl_onground ) && g_ctx.m_local->get_move_type( ) == move_type_walk &&
		                  vel_pinned( g_ctx.m_local->get_velocity( ) );
		const bool fixed   = end.m_x == origin.m_x && end.m_y == origin.m_y && end.m_z == origin.m_z;
		int held_for       = held ? k_ahead + 1 : 0;
		if ( held && !fixed ) {
			c_vector prev = end;
			for ( int t = 0; t < k_ahead; ++t ) {
				g_prediction.begin( g_ctx.m_local, cmd );
				g_prediction.end( g_ctx.m_local );
				if ( !pin_held( ) ) {
					held_for = t + 1;
					break;
				}
				if ( bits_eq( g_ctx.m_local->get_abs_origin( ), prev ) )
					break;
				prev = g_ctx.m_local->get_abs_origin( );
			}
		}
		g_prediction.restore_entity_to_predicted_frame( g_interfaces.m_prediction->m_commands_predicted - 1 );
		apply_predicted_state( );
		cmd->m_view_point   = live_view;
		cmd->m_forward_move = live_fwd;
		cmd->m_side_move    = live_side;
		if ( held && held_for <= k_ahead ) {
			ahead     = true;
			ahead_h   = held_for;
			ahead_end = end;
			botox_dbg_log( "AS: hold ahead t=%d h=%d yaw=%.1f fwd=%.4f", s_hold.ticks, held_for, s_hold.yaw, s_hold.fwd );
		} else if ( held ) {
			if ( s_hold.ticks++ == 0 )
				botox_dbg_log( "AS: hold yaw=%.1f fwd=%.4f pitch=%.2f fixed=%d", s_hold.yaw, s_hold.fwd, s_hold.pitch, ( int )fixed );
			publish( s_hold.yaw, s_hold.fwd, true, s_hold.pitch, true, s_hold.side );
			s_expect = { true, end };
			return;
		} else
			botox_dbg_log( "AS: hold break t=%d yaw=%.1f fwd=%.4f pitch=%.2f b=%x", s_hold.ticks, s_hold.yaw, s_hold.fwd, s_hold.pitch, live_buttons );
	}
	if ( !ahead )
		s_hold.ticks = 0;

	/* search result vs the doomed hold: true = hold sent ( search found nothing that pins longer ) */
	const auto keep_hold = [ & ]( const int ticks ) {
		if ( !ahead )
			return false;
		if ( ticks > ahead_h ) {
			botox_dbg_log( "AS: ahead swap t=%d h=%d to=%d", s_hold.ticks, ahead_h, ticks );
			s_hold.ticks = 0;
			return false;
		}
		const auto latched = s_hold;
		++s_hold.ticks;
		publish( latched.yaw, latched.fwd, true, latched.pitch, true, latched.side );
		s_expect = { true, ahead_end };
		return true;
	};

	/* pinned START, no proven re-pin: the cmd that pinned it beats any fresh unproven move. armed, so a sim-held replay becomes a hold */
	const auto resend = [ & ]( ) {
		if ( !start_pinned || s_last.cmd != cmd->m_command_number - 1 )
			return false;
		const int n = s_last.resent + 1;
		botox_dbg_log( "AS: resend n=%d yaw=%.1f fwd=%.4f side=%.4f", n, s_last.yaw, s_last.fwd, s_last.side );
		/* resend = same move, unproven: keeps the sticky ( 11-08 16:24 log: server took resend 101.1 fwd 1 at 34917, sticky was dropped, a fresh pick fell ) */
		const auto stk = s_sticky;
		publish( s_last.yaw, s_last.fwd, true, s_last.pitch, false, s_last.side );
		s_last.resent = n;
		s_proof.valid = false;
		s_sticky      = stk;
		s_sticky.ticks += s_sticky.valid;
		s_expect      = { true, origin };
		return true;
	};

	bool contact = false;
	c_vector contact_n{ };
	float contact_d = 0.0f;
	/* near-axis wall, moving START: clarity's cardinal pin waits for lumi ( 11-05 MARKETWALL n=0.028,-1 logs: clarity moving-START pins
	   fell 0/9, lumi fwd 5/30 1-tick pins put the server on the pin 10/14, then clarity re-pins from that pinned START 7/7 ) */
	struct {
		bool valid = false;
		float yaw = 0.f, fwd = 0.f, pitch = 0.f;
		int ticks = 0;
		c_vector end{ };
	} cl_defer;

	{
		c_vector cl_normal{ };
		bool cl_contact = false;
		bool cl_disp    = false;
		int cl_ent      = -1;
		const char* cl_surf = "";
		for ( int i = 0; i < 4 && !cl_contact; ++i ) {
			const float a = deg2rad( static_cast< float >( i * 90 ) );
			trace_t tr{ };
			ray_t ray( origin, c_vector( origin.m_x + std::cos( a ) * 0.05f, origin.m_y + std::sin( a ) * 0.05f, origin.m_z ), mins, maxs );
			g_interfaces.m_engine_trace->trace_ray( ray, mask_playersolid, &filter, &tr );
			cl_contact = tr.did_hit( );
			cl_normal  = tr.m_plane.m_normal;
			contact_d  = cl_contact ? n_tb::hull_plane_dist( tr, mins, maxs ) : 0.0f;
			cl_disp    = tr.m_disp_flags != 0;
			cl_ent     = tr.m_hit_entity ? tr.m_hit_entity->get_index( ) : -1;
			cl_surf    = tr.surface.m_name ? tr.surface.m_name : "";
		}
		contact        = cl_contact;
		contact_n      = cl_normal;
		bool cl_held   = false;
		bool cl_parked = false;
		int cl_rob     = -1;
		int cl_crob    = -1;
		int cl_score   = -1;
		int cl_ticks   = 0;
		int cl_pins    = 0;
		float cl_yaw = 0.f, cl_fwd = 0.f, cl_pitch = 0.f;
		c_vector cl_end{ };
		float cl_park_yaw = 0.f, cl_park_fwd = 0.f, cl_park_gap = -1.f;

		/* exact-axis brush: the cardinal ladder never pins there ( 10-30 21:19 log 0/61 ticks, lab 0% ) while lumi's tangent fan does
		   ( 3/3 pin=3 held ) and was starved to cand=1 behind it = leave lumi the whole clock */
		const bool cl_axis = cl_contact && cl_normal.m_z == 0.0f && ( std::fabsf( cl_normal.m_x ) == 1.0f || std::fabsf( cl_normal.m_y ) == 1.0f );
		if ( cl_axis )
			botox_dbg_log( "AS: clarity axis n=%.3f,%.3f vz=%.3f", cl_normal.m_x, cl_normal.m_y, live_velocity.m_z );
		if ( cl_contact && !cl_axis && cl_normal.m_z == 0.0f ) {
			const float wall_yaw = rad2deg( std::atan2( -cl_normal.m_y, -cl_normal.m_x ) );
			const float base     = std::roundf( wall_yaw / 90.0f ) * 90.0f;
			const float off      = std::remainder( wall_yaw - base, 360.0f );
			const bool cl_near   = std::fabsf( off ) < 10.0f;

			int cl_sims = 0;
			n_tick::c_sim_budget cl_clock;
			/* 0.6 of the cmd clock ( 0.4 near-axis ): 11-05 09:20 log, clarity ate it all and lumi ran cand=1 every other tick while its pins
			   were the ones the server took. pinned START = own clock, the whole slider: re-pins there took 97-176 sims at 45 us/sim and the
			   3 misses had 28-81 ( 09:48 log ) */
			constexpr float k_cl_share = 0.6f;
			cl_clock.start( GET_VARIABLE( g_variables.m_air_stuck_budget, float ) * 0.01f, n_tick::engine_interval( ), n_tick::search_as,
			                start_pinned ? 1.0f : cl_near ? 0.4f : k_cl_share, start_pinned );
			const float yaws[ 2 ] = { std::remainder( base + ( off > 0.0f ? 90.0f : -90.0f ), 360.0f ), std::remainder( base, 360.0f ) };
			const int first_yaw   = off == 0.0f ? 1 : 0;

			static auto sv_airaccelerate = g_interfaces.m_convar->find_var( "sv_airaccelerate" );
			const float airaccel         = sv_airaccelerate ? sv_airaccelerate->get_float( ) : 12.0f;
			const float ipt              = n_tick::engine_interval( );
			const float duck_mul         = ( live_buttons & in_duck ) ? 0.34f : 1.0f;
			const float step = ( 0.03125f / ( airaccel * ipt * g_ctx.m_local->get_surface_friction( ) * duck_mul * ipt ) ) / 100.0f;

			/* server float noise != ours ( online: near-axis pins die after 1 rtt ): prefer a pin that survives 1 ulp of origin each way */
			constexpr int k_rob  = 6;
			constexpr int k_rank = k_crob * ( k_rob + 1 ) + k_rob + 1; /* one more pinned tick outranks any robustness */
			constexpr int k_best = ( k_chain + 1 ) * k_rank - 1;
			const auto robust = [ & ]( ) {
				const c_vector base = g_ctx.m_local->get_abs_origin( );
				int n               = 0;
				for ( int k = 0; k < k_rob; ++k ) {
					c_vector o = base;
					float& c   = k < 2 ? o.m_x : k < 4 ? o.m_y : o.m_z;
					c          = std::nextafter( c, ( k & 1 ) ? FLT_MAX : -FLT_MAX );
					g_ctx.m_local->set_abs_origin( o );
					g_ctx.m_local->get_origin( )   = o;
					g_ctx.m_local->get_velocity( ) = c_vector( 0.0f, 0.0f, pin_vz );
					/* EFL_DIRTY_ABSVELOCITY: SetupMove reads GetAbsVelocity ( prediction.cpp:781 ), else a fallen k-1 sim's END leaks in */
					if ( int& eflags = g_ctx.m_local->get_eflags( ); reinterpret_cast< std::uintptr_t >( &eflags ) != reinterpret_cast< std::uintptr_t >( g_ctx.m_local ) )
						eflags |= 1 << 12;
					bool held = true;
					for ( int t = 0; t < 2 && held; ++t ) {
						g_prediction.begin( g_ctx.m_local, cmd );
						g_prediction.end( g_ctx.m_local );
						++cl_sims;
						held = !( g_ctx.m_local->get_flags( ) & fl_onground ) && g_ctx.m_local->get_move_type( ) == move_type_walk &&
						       vel_pinned( g_ctx.m_local->get_velocity( ) );
					}
					n += held;
				}
				return n;
			};

			/* i = 0 once: a zero-move pin is view-free ( turn / shoot / silent all sim the same ), it wins ties */
			for ( int y = first_yaw; y < 2 && cl_score < k_best; ++y ) {
				for ( int i = y == first_yaw ? 0 : 1; i <= 100 && cl_score < k_best && !( cl_sims > 0 && cl_clock.expired( ) ); ++i ) {
					const float fwd = static_cast< float >( i ) * step;
					g_prediction.restore_entity_to_predicted_frame( g_interfaces.m_prediction->m_commands_predicted - 1 );
					apply_predicted_state( );
					cmd->m_view_point     = live_view;
					cmd->m_view_point.m_y = yaws[ y ];
					cmd->m_forward_move   = fwd;
					cmd->m_side_move      = 0.0f;
					cmd->m_buttons = live_buttons;

					int ticks = 0;
					c_vector end0{ };
					for ( int t = 0; t < k_chain; ++t ) {
						const c_vector from = g_ctx.m_local->get_abs_origin( );
						g_prediction.begin( g_ctx.m_local, cmd );
						g_prediction.end( g_ctx.m_local );
						++cl_sims;
						if ( t == 0 )
							end0 = g_ctx.m_local->get_abs_origin( );
						const bool ok = !( g_ctx.m_local->get_flags( ) & fl_onground ) && g_ctx.m_local->get_move_type( ) == move_type_walk &&
						                vel_pinned( g_ctx.m_local->get_velocity( ) );
						if ( !ok )
							break;
						++ticks;
						if ( t > 0 && bits_eq( g_ctx.m_local->get_abs_origin( ), from ) ) {
							ticks = k_chain;
							break;
						}
					}
					if ( ticks < 6 )
						continue;
					++cl_pins;
					const int base = ticks * k_rank;
					if ( base + k_rank - 1 <= cl_score )
						continue;
					const int rob = robust( );
					if ( base + k_crob * ( k_rob + 1 ) + rob <= cl_score )
						continue;
					const int rem   = cl_score - base;
					const int crob  = catch_rob( cl_sims, rem < rob ? -1 : ( rem - rob ) / ( k_rob + 1 ), cl_clock );
					const int score = base + crob * ( k_rob + 1 ) + rob;
					if ( score > cl_score ) {
						cl_held  = true;
						cl_ticks = ticks;
						cl_score = score;
						cl_rob   = rob;
						cl_crob  = crob;
						cl_yaw   = yaws[ y ];
						cl_fwd   = fwd;
						cl_pitch = live_view.m_x;
						cl_end   = end0;
					}
				}
			}

			constexpr float k_park_gap   = 0.01f;
			constexpr float k_park_probe = 0.05f;
			if ( !cl_held && !cl_disp ) {
				const auto hull_hits = [ & ]( const float d ) {
					trace_t tr{ };
					ray_t ray( origin, origin - cl_normal * d, mins, maxs );
					g_interfaces.m_engine_trace->trace_ray( ray, mask_playersolid, &filter, &tr );
					return tr.did_hit( ) || tr.m_start_solid;
				};
				if ( hull_hits( k_park_probe ) ) {
					float glo = 0.f, ghi = k_park_probe;
					for ( int i = 0; i < 24; ++i ) {
						const float mid = ( glo + ghi ) * 0.5f;
						( hull_hits( mid ) ? ghi : glo ) = mid;
					}
					cl_park_gap = glo;
				}
				if ( cl_park_gap > k_park_gap ) {
					const float start_d = origin.m_x * cl_normal.m_x + origin.m_y * cl_normal.m_y;
					float lo = 1.17549435e-38f, hi = step * 100.f, best_d = FLT_MAX;
					bool best_pinned = false;
					for ( int i = 0; i < 16; ++i ) {
						const float mid = ( lo + hi ) * 0.5f;
						g_prediction.restore_entity_to_predicted_frame( g_interfaces.m_prediction->m_commands_predicted - 1 );
						apply_predicted_state( );
						cmd->m_view_point = live_view;
						stamp_rotate( live_view.m_y, wall_yaw, mid, 0.f, cmd->m_forward_move, cmd->m_side_move );
						cmd->m_buttons = live_buttons;
						g_prediction.begin( g_ctx.m_local, cmd );
						g_prediction.end( g_ctx.m_local );
						++cl_sims;
						const c_vector o = g_ctx.m_local->get_abs_origin( );
						const float d    = o.m_x * cl_normal.m_x + o.m_y * cl_normal.m_y;
						const bool back  = start_d <= d;
						if ( !back && d < best_d && !( g_ctx.m_local->get_flags( ) & fl_onground ) ) {
							best_d      = d;
							best_pinned = vel_pinned( g_ctx.m_local->get_velocity( ) );
							cl_park_fwd = mid;
							cl_parked   = true;
						}
						( back ? hi : lo ) = mid;
					}
					if ( best_pinned )
						cl_parked = false;
					cl_park_yaw = wall_yaw;
				}
			}
			g_prediction.restore_entity_to_predicted_frame( g_interfaces.m_prediction->m_commands_predicted - 1 );
			apply_predicted_state( );
			cmd->m_view_point   = live_view;
			cmd->m_forward_move = live_fwd;
			cmd->m_side_move    = live_side;
			cmd->m_buttons      = live_buttons;

			botox_dbg_log( "AS: clarity %s n=%.3f,%.3f yaw=%.0f fwd=%.4f sims=%d vz=%.3f vxy=%.1f gap=%.5f t=%d disp=%d rob=%d crob=%d np=%d cp=%d ent=%d surf=%s us=%lld",
			               cl_held ? "pin" : cl_parked ? "park" : "miss", cl_normal.m_x, cl_normal.m_y,
			               cl_parked ? cl_park_yaw : cl_yaw, cl_parked ? cl_park_fwd : cl_fwd, cl_sims, live_velocity.m_z, live_velocity.length_2d( ), cl_park_gap,
			               cl_ticks, ( int )cl_disp, cl_rob, cl_crob, cl_pins, g_interfaces.m_prediction->m_commands_predicted, cl_ent, cl_surf, cl_clock.used_us( ) );
			if ( cl_held ) {
				if ( keep_hold( cl_ticks ) )
					return;
				if ( cl_near && !start_pinned ) {
					cl_defer = { true, cl_yaw, cl_fwd, cl_pitch, cl_ticks, cl_end };
					botox_dbg_log( "AS: clarity defer off=%.1f", off );
				} else {
					publish( cl_yaw, cl_fwd, true, cl_pitch, true );
					s_expect = { true, cl_end };
					return;
				}
			}
			/* park is unpinned: a doomed hold still pins this tick, lumi decides; a pinned START is never pushed off its pin */
			if ( cl_parked && !ahead && !start_pinned ) {
				s_sticky.valid = false;
				publish( cl_park_yaw, cl_park_fwd, false, live_view.m_x, false );
				return;
			}
		}
	}

	/* wall, touch scan: 16 headings from last tick's, 1u ray column feet..head reaching 2u past the hull edge.
	   |nz| <= 0.35 ( slopes / disps count ), must face us; lowest fraction in the column wins */
	const float extra_reach = std::clamp( GET_VARIABLE( g_variables.m_air_stuck_extra_reach, float ), 0.0f, 32.0f );
	/* touching ( 0.05u hull contact ): that face is the wall, no line scan. 11-05 offline log: at convex corners the hull touches the edge
	   ( axial bevel ) while the line scan locked faces 10-15u off ( gap=9.9 / 15.5, normal flipping ) = 4 falls, 0 catches */
	const bool touch = contact && std::fabsf( contact_n.m_z ) <= 0.35f;
	/* one box holds every A/B ray below: empty = no wall, skips ~1170 traces ( 4.5 ms/tick ate the cmd clock, eb starved ) */
	if ( !touch ) {
		const float half = std::max( ext_x, ext_y ) + std::max( extra_reach, 0.05f ) + 0.5f;
		trace_t tr{ };
		ray_t ray( origin, origin, c_vector( -half, -half, mins.m_z - 1.0f ), c_vector( half, half, maxs.m_z + 1.0f ) );
		g_interfaces.m_engine_trace->trace_ray( ray, mask_playersolid, &filter, &tr );
		if ( !tr.did_hit( ) ) {
			if ( keep_hold( 0 ) || resend( ) )
				return;
			start_angle    = 0.0f;
			was_stuck      = false;
			s_sticky.valid = false;
			return;
		}
	}
	char scan        = touch ? 'C' : 'A';
	bool found       = touch;
	c_vector normal  = touch ? contact_n : c_vector{ };
	float hit_z = origin.m_z;
	int hit_ent = -1;
	if ( touch ) {
		start_angle = std::atan2( -contact_n.m_y, -contact_n.m_x );
		if ( start_angle < 0.0f )
			start_angle += k_pi * 2.0f;
	}
	for ( float a = start_angle; !touch && a < start_angle + k_pi * 2.0f; a += k_pi / 8.0f ) {
		const float ang   = std::fmod( a, k_pi * 2.0f );
		const float c     = std::cos( ang );
		const float s     = std::sin( ang );
		const float reach = std::min( ext_x / std::max( std::fabsf( c ), 1e-6f ), ext_y / std::max( std::fabsf( s ), 1e-6f ) ) +
		                    std::clamp( extra_reach, 0.05f, 2.0f );
		float best_frac   = 1.0f;

		for ( float z = origin.m_z + mins.m_z; z <= origin.m_z + maxs.m_z; z += 1.0f ) {
			const trace_t tr =
				line( c_vector( origin.m_x, origin.m_y, z ), c_vector( origin.m_x + c * reach, origin.m_y + s * reach, z ), mask_playersolid );
			const c_vector& n = tr.m_plane.m_normal;
			if ( tr.m_fraction < 1.0f && std::fabsf( n.m_z ) <= 0.35f && n.m_x * c + n.m_y * s <= -0.1f && tr.m_fraction < best_frac ) {
				found     = true;
				best_frac = tr.m_fraction;
				normal    = n;
				hit_z     = tr.m_end.m_z;
				hit_ent   = tr.m_hit_entity ? tr.m_hit_entity->get_index( ) : -1;
			}
		}
		if ( found ) {
			start_angle = a;
			break;
		}
	}

	if ( !found && extra_reach > 2.0f ) {
		scan               = 'B';
		const float radius = std::max( ext_x, ext_y ) + extra_reach;
		const float z0     = origin.m_z + mins.m_z + 2.0f;
		const float dz     = ( ( origin.m_z + maxs.m_z - 2.0f ) - z0 ) / 6.0f;
		int best_hits      = 0;
		float best_frac    = 1.0f;

		for ( float a = 0.0f; a < k_pi * 2.0f; a += k_pi / 16.0f ) {
			const float c = std::cos( a );
			const float s = std::sin( a );
			int hits      = 0;
			float frac    = 1.0f;
			c_vector n_a{ };
			float z_a = 0.0f;
			int e_a   = -1;

			for ( int i = 0; i < 7; ++i ) {
				const float z = static_cast< float >( i ) * dz + z0;
				const trace_t tr =
					line( c_vector( origin.m_x, origin.m_y, z ), c_vector( origin.m_x + c * radius, origin.m_y + s * radius, z ), mask_playersolid );
				if ( tr.m_fraction < 1.0f && std::fabsf( tr.m_plane.m_normal.m_z ) <= 0.08f && ( ++hits == 1 || tr.m_fraction < frac ) ) {
					frac = tr.m_fraction;
					n_a  = tr.m_plane.m_normal;
					z_a  = tr.m_end.m_z;
					e_a  = tr.m_hit_entity ? tr.m_hit_entity->get_index( ) : -1;
				}
			}
			if ( hits > 0 && ( !found || hits > best_hits || ( hits == best_hits && frac < best_frac ) ) ) {
				found       = true;
				start_angle = a;
				best_hits   = hits;
				best_frac   = frac;
				normal      = n_a;
				hit_z       = z_a;
				hit_ent     = e_a;
			}
		}
	}

	if ( !found ) {
		if ( keep_hold( 0 ) || resend( ) )
			return;
		start_angle    = 0.0f;
		was_stuck      = false;
		s_sticky.valid = false;
		return;
	}
	const long long scan_us = scan_clock.used_us( );

	const float wall_yaw = rad2deg( std::atan2( -normal.m_y, -normal.m_x ) );
	float drift_m = std::fmod( rad2deg( std::atan2( live_velocity.m_y, live_velocity.m_x ) ) - wall_yaw + 180.0f, 360.0f );
	if ( drift_m < 0.0f )
		drift_m += 360.0f;
	const float side    = drift_m < 180.0f ? 1.0f : -1.0f;
	const float support = GetWallSupportDistance( g_ctx.m_local, c_vector( normal.m_x, normal.m_y, 0.0f ) );
	/* touch: hull to contact plane at END, no line. 11-06 log: the feet-z gap line missed the wall on every cand ( gap=14.048 / 15.419 ) =
	   rank dead, first cand ( fwd 1 ) sent 12-23t, no catch; lab near-axis mirage: dead rank 159/190 caught at 3-9t vs 190/190 at 2t */
	const float hull_off = std::min( normal.m_x * mins.m_x, normal.m_x * maxs.m_x ) + std::min( normal.m_y * mins.m_y, normal.m_y * maxs.m_y ) +
	                       std::min( normal.m_z * mins.m_z, normal.m_z * maxs.m_z ) - contact_d;

	n_tick::c_sim_budget clock;
	clock.start( GET_VARIABLE( g_variables.m_air_stuck_budget, float ) * 0.01f, n_tick::engine_interval( ), n_tick::search_as );
	int cands        = 0;
	int crob_sims    = 0;
	int best_crob    = -1;
	int best_ticks   = 0;
	float best_gap   = FLT_MAX;
	float best_yaw   = wall_yaw;
	float best_move  = 0.0f;
	float best_side  = 0.0f;
	c_vector best_end{ };
	const auto out_of_time = [ & ]( ) { return cands >= 512 || ( cands > 0 && clock.expired( ) ); };

	const auto try_yaw = [ & ]( const float yaw, const float move, const float gap_len, const float side_move = 0.0f ) {
		++cands;
		g_prediction.restore_entity_to_predicted_frame( g_interfaces.m_prediction->m_commands_predicted - 1 );
		apply_predicted_state( );

		cmd->m_view_point.m_y = yaw;
		cmd->m_forward_move   = move;
		cmd->m_side_move      = side_move;
		cmd->m_buttons        = live_buttons;

		g_prediction.begin( g_ctx.m_local, cmd );
		g_prediction.end( g_ctx.m_local );
		const c_vector end1 = g_ctx.m_local->get_abs_origin( );

		/* lumi chains 3 ticks; ours runs to a fall or a repeat ( fixed point ) so a pin that lasts longer ranks higher */
		int ticks = 0;
		if ( pinned( ) ) {
			ticks = 1;
			for ( int k = 1; k < k_chain; ++k ) {
				const c_vector from = g_ctx.m_local->get_abs_origin( ), from_v = g_ctx.m_local->get_velocity( );
				g_prediction.begin( g_ctx.m_local, cmd );
				g_prediction.end( g_ctx.m_local );
				if ( !pinned( ) )
					break;
				++ticks;
				if ( bits_eq( g_ctx.m_local->get_abs_origin( ), from ) && bits_eq( g_ctx.m_local->get_velocity( ), from_v ) ) {
					ticks = k_chain;
					break;
				}
			}
		}

		const c_vector end = g_ctx.m_local->get_abs_origin( );
		const float gap    = touch ? end.dot_product( normal ) + hull_off
		                           : line( c_vector( end.m_x, end.m_y, hit_z ), c_vector( end.m_x - normal.m_x * gap_len, end.m_y - normal.m_y * gap_len, hit_z ),
		                                   mask_all ).m_fraction * gap_len - support;
		/* 10-30 21:19 log: pin=1 fell 28/28, pin=3 held 3/3; crob only breaks ties */
		const int crob     = ticks > 0 && ticks >= best_ticks ? catch_rob( crob_sims, ticks > best_ticks ? -1 : best_crob - 1, clock ) : -1;

		if ( ticks > best_ticks || ( ticks == best_ticks && ( crob > best_crob || ( crob == best_crob && gap < best_gap ) ) ) ) {
			best_crob  = crob;
			best_ticks = ticks;
			best_gap   = gap;
			best_yaw   = cmd->m_view_point.m_y;
			best_move  = move;
			best_side  = side_move;
			best_end   = end1;
		}
	};
	const auto try_move = [ & ]( const float rel, const float move, const float gap_len ) { try_yaw( std::remainder( wall_yaw + rel * side, 360.0f ), move, gap_len ); };

	/* sticky first: a pin it proves wins outright = keeps the cmd the server may already be pinned on ( 11-08 log: client's own fwd 5 pin
	   replaced the fwd 1 the server took = fell ). online only: offline nothing is in flight. no speed gate ( 11-08: all 17 server catches
	   on approach cmds came at client vxy 3-205, next cmd differed, 17/17 fell; same cmd kept 82/83 ). window = 3 cp sends, then re-pick:
	   a catch on copy k survives only if the next cp copies match = 1 in cp+1 at a cp window ( 11-08 16:24 log: 11/24 locks died to the
	   next cmd differing ), ~2/3 at 3 cp. capped k_chain, never a long freeze ( 11-05 offline 30-75t, 11-06 sticky-all ) */
	const int cp        = g_interfaces.m_prediction->m_commands_predicted;
	const bool stick_ok = contact && cp > 0;
	const bool sticky   = s_sticky.valid && stick_ok && s_sticky.ticks < std::min( cp * 3, k_chain ) && s_sticky.normal.dot_product( normal ) > 0.999f;
	if ( sticky )
		try_yaw( s_sticky.yaw, s_sticky.fwd, 32.0f, s_sticky.side );
	const bool sticky_pin = sticky && best_ticks >= 3;

	constexpr float k_moves_near[ 3 ] = { 1.0f, 5.0f, 30.0f };
	for ( const float move : k_moves_near ) {
		if ( sticky_pin )
			break;
		if ( out_of_time( ) )
			break;
		for ( float rel = 85.999f; rel < 90.0f; rel += 1.0f ) {
			if ( out_of_time( ) )
				break;
			try_move( rel, move, 32.0f );
		}
	}

	if ( !sticky_pin && best_ticks < k_chain && !out_of_time( ) ) {
		constexpr float k_moves_far[ 6 ] = { 1.0f, 5.0f, 30.0f, 60.0f, 90.0f, 120.0f };
		for ( const float move : k_moves_far ) {
			if ( out_of_time( ) )
				break;
			for ( float rel = 78.0f; rel < 90.0f; rel += 0.5f ) {
				if ( out_of_time( ) )
					break;
				try_move( rel, move, 64.0f );
			}
		}
	}

	g_prediction.restore_entity_to_predicted_frame( g_interfaces.m_prediction->m_commands_predicted - 1 );
	apply_predicted_state( );
	botox_dbg_log( "AS: lumi scan=%c n=%.3f,%.3f,%.3f hz=%.1f side=%.0f pin=%d crob=%d/%d gap=%.3f yaw=%.1f fwd=%.0f cand=%d vz=%.2f ent=%d scan_us=%lld us=%lld stk=%d",
	               scan, normal.m_x, normal.m_y, normal.m_z, hit_z - origin.m_z, side, best_ticks, best_crob, crob_sims, best_gap, best_yaw, best_move, cands,
	               live_velocity.m_z, hit_ent, scan_us, clock.used_us( ), sticky ? s_sticky.ticks : -1 );
	if ( keep_hold( best_ticks ) )
		return;
	/* lumi found no pin at all: the deferred clarity pin still beats an approach move */
	if ( best_ticks < 1 && cl_defer.valid ) {
		publish( cl_defer.yaw, cl_defer.fwd, true, cl_defer.pitch, true );
		s_expect = { true, cl_defer.end };
		return;
	}
	if ( best_ticks < 3 && resend( ) )
		return;
	if ( best_ticks < 3 && sticky ) {
		++s_sticky.ticks;
		publish( s_sticky.yaw, s_sticky.fwd, false, live_view.m_x, false, s_sticky.side );
		s_expect.valid = false;
		return;
	}
	/* a hold is armed only by the full 3-tick chain; shorter pins go out as the approach move, unarmed */
	publish( best_yaw, best_move, best_ticks >= 3, live_view.m_x, false, best_side );
	s_expect = { best_ticks >= 3, best_end };
	if ( best_ticks < 3 ) {
		s_sticky = { stick_ok, best_yaw, best_move, best_side, normal, 0 };
		if ( stick_ok )
			botox_dbg_log( "AS: sticky yaw=%.1f fwd=%.0f", best_yaw, best_move );
	}
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
