/* edge surf, real servers: own feature, pure movement ( no server cmds ). two real edge glides in CS:GO:
   feet on a lone ledge TOP edge whose top is off the hull centre's float grid ( edge_surf_feet, bottom of this file, runs first ), and
   the head under a brush BOTTOM edge, hull 1 ulp beside the face: the +z face is picked at t 0 every tick, z never moves ( lab
   tools/wc_engine/edge_probe: A feet on a grid top never chain, D every input line == the phase law, E the exact law below ).
   per tick: wall ( reach, else the lead arc ) -> brush bottom behind it -> hold band per stance -> no band = hands off -> else delta
   strafe / park to 1 ulp off the face in the band stance -> whole-ulp pin ladder.
   shares texture bug's sim plumbing only ( n_tb budget / restore / trace ). no fake / server side hold anywhere ( removed 11-17 ) */
#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"
#include "movement.h"
#include "../prediction/prediction.h"
#include "texturebug_common.h"
#include <algorithm>
#include <cfloat>
#include <cmath>

using namespace n_tb;

extern void botox_dbg_log( const char* fmt, ... );
extern bool botox_is_ladder_trace( const trace_t& tr );
extern bool g_edge_surf_active;
extern bool g_air_stuck_owns_cmd;

/* the feet approach latch ( edge_surf_feet ): cmd number of the last tick it owned. air stuck yields to the next one, a tick it
   misses names its gate in the log */
static int g_edge_surf_latch_cmd = -1;
/* cmd number of the last cmd that sent a proven edge surf pin ( chat detection "edgesurfed", movement.cpp run_detections ) */
int g_edge_surf_pin_cmd = -1;

bool edge_surf_latched( const c_user_cmd* cmd )
{
	return cmd && g_edge_surf_latch_cmd >= 0 && g_edge_surf_latch_cmd == cmd->m_command_number - 1;
}

/* log 11-17: after all 10 act=U no ESURF line for the whole 9u fall back. whoever skips an owned tick says so here */
void edge_surf_skip( const c_user_cmd* cmd, const char* why )
{
	if ( edge_surf_latched( cmd ) && GET_VARIABLE( g_variables.m_debug_log, bool ) )
		botox_dbg_log( "ESURF: feet skip why=%s cmd=%d", why, cmd->m_command_number );
}

/* the move an owned feet tick committed. features after movement post ( edgebug post, edge catch post, lua, silent view ... ) may
   rewrite the cmd: one closing ulp over the top lands you on the ledge, so the final stamp puts ours back */
static struct {
	int cmd = -1, buttons = 0;
	float yaw = 0.f, fwd = 0.f, side = 0.f;
} s_es_sent;

/* px "observe" ( Desktop\px pixel_control.cpp ): a plan is judged by the REAL result. log 11-17: 10 of 22 arrival pins the sim rode
   fell through on the server, 9 more the sim itself refused at the crossing, the same gap again on every retry. an arrival gap that
   failed is bad at that ledge / stance / fall phase: the next approach parks on another one */
struct es_bad_t {
	float top = 0.f, face = 0.f, f0 = 0.f;
	int m = 0;
	bool duck = false;
};
static es_bad_t s_es_bad[ 16 ]{ };
static int s_es_bad_n = 0;
/* the arrival pin the last cmd sent, checked against this START */
static struct {
	int cmd = -1, m = 0;
	float top = 0.f, face = 0.f, f0 = 0.f;
	bool duck = false;
} s_es_pin_sent;

static bool es_is_bad( const float top, const float face, const bool duck, const float f0, const int m )
{
	for ( int i = 0; i < std::min( s_es_bad_n, 16 ); ++i ) {
		const es_bad_t& b = s_es_bad[ i ];
		if ( b.m == m && b.duck == duck && std::fabsf( b.top - top ) < 1e-3f && std::fabsf( b.face - face ) < 0.01f && std::fabsf( b.f0 - f0 ) < 2e-3f )
			return true;
	}
	return false;
}
static void es_mark_bad( const float top, const float face, const bool duck, const float f0, const int m )
{
	if ( m > 0 && !es_is_bad( top, face, duck, f0, m ) )
		s_es_bad[ s_es_bad_n++ % 16 ] = { top, face, f0, m, duck };
}
/* START after a sent arrival pin: on = feet rest on the stop plane, vz pinned, airborne.
   ponytail: with ping the START is our own prediction of that cmd: a refusal only the server saw is not marked. judge on the cmd's
   ack if online logs show `feet real hit=1` on pins that then snap back */
static void es_real_check( const c_user_cmd* cmd, const bool on, const float dz, const float vz, const bool debug )
{
	if ( s_es_pin_sent.cmd < 0 || !cmd || s_es_pin_sent.cmd != cmd->m_command_number - 1 )
		return;
	if ( !on )
		es_mark_bad( s_es_pin_sent.top, s_es_pin_sent.face, s_es_pin_sent.duck, s_es_pin_sent.f0, s_es_pin_sent.m );
	if ( debug )
		botox_dbg_log( "ESURF: feet real hit=%d m=%d f0=%.4f duck=%d top=%.3f face=%.3f dz=%.4f vz=%.2f bad=%d", on ? 1 : 0, s_es_pin_sent.m,
		               s_es_pin_sent.f0, s_es_pin_sent.duck ? 1 : 0, s_es_pin_sent.top, s_es_pin_sent.face, dz, vz, std::min( s_es_bad_n, 16 ) );
	s_es_pin_sent.cmd = -1;
}

/* create_move_proxy, last thing before the cmd leaves ( after silent view, lua, tb preserve_move ) */
void edge_surf_final_stamp( c_user_cmd* cmd )
{
	constexpr int k_keys = in_forward | in_back | in_moveleft | in_moveright | in_duck;
	if ( !cmd || s_es_sent.cmd != cmd->m_command_number )
		return;
	if ( cmd->m_view_point.m_y == s_es_sent.yaw && cmd->m_forward_move == s_es_sent.fwd && cmd->m_side_move == s_es_sent.side &&
	     ( cmd->m_buttons & k_keys ) == ( s_es_sent.buttons & k_keys ) )
		return;
	const float a0 = deg2rad( s_es_sent.yaw ), a1 = deg2rad( cmd->m_view_point.m_y );
	const float wx0 = s_es_sent.fwd * std::cosf( a0 ) + s_es_sent.side * std::sinf( a0 ), wy0 = s_es_sent.fwd * std::sinf( a0 ) - s_es_sent.side * std::cosf( a0 );
	const float wx1 = cmd->m_forward_move * std::cosf( a1 ) + cmd->m_side_move * std::sinf( a1 );
	const float wy1 = cmd->m_forward_move * std::sinf( a1 ) - cmd->m_side_move * std::cosf( a1 );
	const bool keys = ( cmd->m_buttons & k_keys ) != ( s_es_sent.buttons & k_keys );
	/* a rotation that keeps the world wish ( silent view's movement fix ) is not a stomp */
	if ( !keys && std::fabsf( wx1 - wx0 ) + std::fabsf( wy1 - wy0 ) < 1e-4f )
		return;
	if ( GET_VARIABLE( g_variables.m_debug_log, bool ) )
		botox_dbg_log( "ESURF: feet stomp cmd=%d wish %.5f,%.5f -> %.5f,%.5f keys %x -> %x yaw %.3f -> %.3f", cmd->m_command_number, wx0, wy0, wx1, wy1,
		               s_es_sent.buttons & k_keys, cmd->m_buttons & k_keys, s_es_sent.yaw, cmd->m_view_point.m_y );
	cmd->m_forward_move = s_es_sent.fwd;
	cmd->m_side_move    = s_es_sent.side;
	/* same view = the exact bits ( a pin press is ulp sized ), else rotated onto this view */
	if ( cmd->m_view_point.m_y != s_es_sent.yaw )
		move_fix_to_yaw( s_es_sent.yaw, *cmd );
	cmd->m_buttons = ( cmd->m_buttons & ~k_keys ) | ( s_es_sent.buttons & k_keys );
}

/* no wall within reach -> hull sweeps along the free-fall arc ( 4 segments ) up to the lead range; first vertical world face = the wall
   to close on. floor / ramp hit ends the path */
static bool edge_lead( i_trace_filter& filter, const c_vector& org, const c_vector& vel, const c_vector& mins, const c_vector& maxs,
                       const float ipt, const float g, float& gap, c_vector& n, float& plane_d )
{
	const float range = std::clamp( GET_VARIABLE( g_variables.m_edge_surf_range, float ), 8.f, 512.f );
	const float vxy   = vel.length_2d( );
	if ( vxy < 1.f )
		return false;
	const float horizon = std::min( range / vxy, ipt * static_cast< float >( n_tick::ticks( 32 ) ) );
	constexpr int k_segments = 4;
	const auto at = [ & ]( const float t ) { return c_vector( org.m_x + vel.m_x * t, org.m_y + vel.m_y * t, org.m_z + vel.m_z * t - 0.5f * g * t * t ); };
	for ( int i = 0; i < k_segments; ++i ) {
		const float t0 = horizon * static_cast< float >( i ) / k_segments, t1 = horizon * static_cast< float >( i + 1 ) / k_segments;
		trace_t tr{ };
		ray_t ray( at( t0 ), at( t1 ), mins, maxs );
		tb_trace( ray, mask_playersolid, &filter, &tr );
		if ( tr.m_start_solid || tr.m_fraction >= 1.f )
			continue;
		const c_vector& tn = tr.m_plane.m_normal;
		if ( std::fabsf( tn.m_z ) > 0.05f || botox_is_ladder_trace( tr ) )
			return false;
		c_base_entity* ent = tr.m_hit_entity;
		if ( ent && ent->get_index( ) != 0 && !( ent->get_collideable( ) && ent->get_collideable( )->get_solid( ) == 1 ) )
			return false;
		plane_d = n_tb::hull_plane_dist( tr, mins, maxs );
		gap     = org.m_x * tn.m_x + org.m_y * tn.m_y + org.m_z * tn.m_z - plane_d + std::min( tn.m_x * mins.m_x, tn.m_x * maxs.m_x ) +
		      std::min( tn.m_y * mins.m_y, tn.m_y * maxs.m_y ) + std::min( tn.m_z * mins.m_z, tn.m_z * maxs.m_z );
		n = tn;
		return gap > 0.f;
	}
	return false;
}

/* the brush behind the face at the hull's xy: bottom B / top T from line traces 0.5u inside the wall ( m_fraction_left_solid = exit of
   the start brush ). T capped at z_hi + 256 when the brush keeps going up */
struct edge_info_t {
	bool ok = false;
	float B = 0.f, T = 0.f;
};
static edge_info_t edge_find( i_trace_filter& filter, const c_vector& org, const c_vector& n, const float plane_d, const float z_hi, const float z_lo )
{
	edge_info_t e;
	if ( z_hi <= z_lo + 1.f )
		return e;
	const float off  = org.m_x * n.m_x + org.m_y * n.m_y + org.m_z * n.m_z - plane_d + 0.5f;
	const c_vector q = org - n * off;
	const auto line  = [ & ]( const float a, const float b, trace_t& tr ) {
		ray_t ray( c_vector( q.m_x, q.m_y, a ), c_vector( q.m_x, q.m_y, b ), c_vector( 0.f, 0.f, 0.f ), c_vector( 0.f, 0.f, 0.f ) );
		tb_trace( ray, mask_playersolid, &filter, &tr );
	};
	trace_t t1{ };
	line( z_hi, z_lo, t1 );
	float top = z_hi;
	if ( t1.m_start_solid ) {
		if ( t1.m_all_solid || t1.m_fraction_left_solid <= 0.f || t1.m_fraction_left_solid >= 1.f )
			return e;
		e.B = z_hi - t1.m_fraction_left_solid * ( z_hi - z_lo );
		trace_t up{ };
		line( z_hi, z_hi + 256.f, up );
		top = ( up.m_start_solid && !up.m_all_solid && up.m_fraction_left_solid > 0.f && up.m_fraction_left_solid < 1.f ) ? z_hi + up.m_fraction_left_solid * 256.f
		                                                                                                                 : z_hi + 256.f;
	} else {
		if ( t1.m_fraction >= 1.f )
			return e;
		top = z_hi - t1.m_fraction * ( z_hi - z_lo );
		trace_t t2{ };
		line( top - 0.05f, z_lo, t2 );
		if ( !t2.m_start_solid || t2.m_all_solid || t2.m_fraction_left_solid <= 0.f || t2.m_fraction_left_solid >= 1.f )
			return e;
		e.B = top - 0.05f - t2.m_fraction_left_solid * ( top - 0.05f - z_lo );
	}
	/* CM_ClipBoxToBrush leaves a brush DIST_EPSILON late: log 10-30 B=-160.031 T=-112.969 for a -160..-113 brush */
	e.B  = e.B + 0.03125f;
	e.T  = top - 0.03125f;
	e.ok = e.B > z_lo + 0.1f && e.T > e.B;
	return e;
}

/* feet on a lone ledge TOP edge, defined under edge_surf_online */
static bool edge_surf_feet( c_user_cmd* cmd, const c_user_cmd& in, c_base_entity* local, int frame, const c_vector& org, const c_vector& vel,
                            const c_vector& mins, const c_vector& maxs, bool ducked0, float ipt, float pin, float grav, float k_acc, bool debug,
                            bool& pinned, bool& held );

/* true = edge surf owns this cmd ( texture bug + head bounce skip it ), pinned = this cmd is a proven pin, held = nothing after tb may
   rewrite it ( a pin, or a feet tick whose wall axis keeps you off the top: no texture bug detection on those ). called from texture
   bug's on_create_move: the sim budget and the sent-cmd resim live there */
bool edge_surf_online( c_user_cmd* cmd, bool& pinned, bool& held )
{
	pinned = held = false;
	constexpr float k_eps       = 0.03125f;
	constexpr float k_touch     = k_eps * 1.3f;
	constexpr float k_ratio     = 1.4f;
	constexpr int k_ladder_cap  = 48;
	constexpr float k_duck_crop = 0.34f;
	constexpr float k_wish_max  = 450.f;
	constexpr int k_move_keys   = in_forward | in_back | in_moveleft | in_moveright;
	static const float k_keep[] = { 0.000001f, 0.0001f, 0.002f, 0.05f, 0.3f };

	static int s_pick = -1, s_cmd = -1, s_run = 0;
	static int s_park_margin = 0, s_park_cmd = -1;
	static float s_park_from = 0.f, s_park_to = 0.f;
	const auto drop = [ & ]( ) {
		s_pick        = -1;
		s_run         = 0;
		s_park_margin = 0;
		s_park_cmd    = -1;
	};

	/* an owned tick reached edge surf ( with the per tick act lines: no enter = tb never called it ) */
	if ( edge_surf_latched( cmd ) && GET_VARIABLE( g_variables.m_debug_log, bool ) )
		botox_dbg_log( "ESURF: feet enter cmd=%d", cmd->m_command_number );
	c_base_entity* const local = g_ctx.m_local;
	g_edge_surf_active         = false;
	const bool armed = cmd && local && local->is_alive( ) && local->get_collideable( ) && !g_ctx.m_eject_restoring &&
	                   GET_VARIABLE( g_variables.m_edge_surf, bool ) && g_input.check_input( &GET_VARIABLE( g_variables.m_edge_surf_key, key_bind_t ) );
	/* START must be the server's END, like texture bug */
	g_prediction.sync_arm( n_prediction::impl_t::k_sync_es, armed );
	if ( !armed || ( g_prediction.backup_data.m_flags & fl_onground ) || g_prediction.backup_data.m_move_type != e_move_types::move_type_walk ) {
		edge_surf_skip( cmd, !armed ? "key" : ( g_prediction.backup_data.m_flags & fl_onground ) ? "ground" : "move" );
		/* a sent pin that ends on the ground / a ladder stood on the ledge = that gap is bad ( key up = no verdict ) */
		if ( armed )
			es_real_check( cmd, false, 0.f, 0.f, GET_VARIABLE( g_variables.m_debug_log, bool ) );
		s_es_pin_sent.cmd = -1;
		g_edge_surf_latch_cmd = -1;
		s_es_sent.cmd         = -1;
		drop( );
		return false;
	}

	const int frame = g_interfaces.m_prediction->m_commands_predicted - 1;
	tb_restore( frame );
	const c_vector org   = local->get_origin( );
	const c_vector vel   = local->get_velocity( );
	const bool ducked0   = ( local->get_flags( ) & fl_ducking ) != 0;
	const float friction = local->get_surface_friction( );
	const c_vector mins  = local->get_collideable( )->get_obb_mins( );
	const c_vector maxs  = local->get_collideable( )->get_obb_maxs( );
	const float ipt      = g_interfaces.m_global_vars_base->m_interval_per_tick;
	const float pin      = s_target_predict_z_vel;
	const float grav     = -2.f * pin / ( ipt > 0.f ? ipt : 0.015625f );
	static auto sv_airaccelerate = g_interfaces.m_convar->find_var( "sv_airaccelerate" );
	const float k_acc            = ( sv_airaccelerate ? sv_airaccelerate->get_float( ) : 12.f ) * ipt * friction;
	const bool debug             = GET_VARIABLE( g_variables.m_debug_log, bool );
	/* a rising head never crosses a bottom edge downward */
	if ( vel.m_z + pin > 1e-3f || k_acc <= 0.f ) {
		edge_surf_skip( cmd, "rise" );
		drop( );
		return false;
	}
	const c_user_cmd in = *cmd;
	es_real_check( cmd, std::fabsf( org.m_z - ( s_es_pin_sent.top + k_eps ) ) < 0.01f && std::fabsf( vel.m_z - pin ) < 1e-3f,
	               org.m_z - ( s_es_pin_sent.top + k_eps ), vel.m_z, debug );

	/* feet on a lone ledge top edge: its own search + guide, before the head search. air stuck parked / froze this cmd = its, not ours
	   ( log 11-17: its 1 ulp park held the gap, the planned arrival gap was 22 = miss ). it yields to a latched approach itself */
	if ( g_air_stuck_owns_cmd )
		edge_surf_skip( cmd, "as" );
	if ( !g_air_stuck_owns_cmd && edge_surf_feet( cmd, in, local, frame, org, vel, mins, maxs, ducked0, ipt, pin, grav, k_acc, debug, pinned, held ) )
		return true;

	/* nearest vertical face along 8 world yaws within reach, else the lead arc */
	const float reach = std::clamp( GET_VARIABLE( g_variables.m_edge_surf_reach, float ), 0.f, 32.f );
	c_trace_filter_tb_world_props filter( local );
	c_vector n{ };
	float gap = FLT_MAX, plane_d = 0.f;
	bool lead = false;
	for ( int i = 0; i < 8; ++i ) {
		const float a = deg2rad( static_cast< float >( i < 4 ? i * 90 : ( i - 4 ) * 90 + 45 ) );
		trace_t tr{ };
		ray_t ray( org, org + c_vector( std::cosf( a ), std::sinf( a ), 0.f ) * ( reach + 0.05f ), mins, maxs );
		tb_trace( ray, mask_playersolid, &filter, &tr );
		if ( tr.m_start_solid || tr.m_fraction >= 1.f || std::fabsf( tr.m_plane.m_normal.m_z ) > 0.05f || botox_is_ladder_trace( tr ) )
			continue;
		const c_vector& tn = tr.m_plane.m_normal;
		const float d      = n_tb::hull_plane_dist( tr, mins, maxs );
		const float g      = org.m_x * tn.m_x + org.m_y * tn.m_y + org.m_z * tn.m_z - d + std::min( tn.m_x * mins.m_x, tn.m_x * maxs.m_x ) +
		                std::min( tn.m_y * mins.m_y, tn.m_y * maxs.m_y ) + std::min( tn.m_z * mins.m_z, tn.m_z * maxs.m_z );
		if ( g < gap ) {
			gap     = g;
			n       = tn;
			plane_d = d;
		}
	}
	if ( gap == FLT_MAX )
		lead = edge_lead( filter, org, vel, mins, maxs, ipt, grav, gap, n, plane_d );
	const float nxy = std::sqrt( n.m_x * n.m_x + n.m_y * n.m_y );
	if ( gap == FLT_MAX || nxy < 0.5f ) {
		drop( );
		return false;
	}
	/* your keys steer off the wall = you leave, never a fight. off the face the key alone decides: the park press holds the closing
	   speed, so the accel test would never trip */
	if ( gap > k_touch ? tb_steering_away_from_wall( &in, n, in.m_forward_move, in.m_side_move )
	                   : tb_move_leaves_wall( &in, n, in.m_forward_move, in.m_side_move, vel, local->get_max_speed( ),
	                                          ( ducked0 || ( in.m_buttons & in_duck ) ) ? k_duck_crop : 1.f ) ) {
		drop( );
		return false;
	}
	const float yaw_in = rad2deg( std::atan2f( -n.m_y, -n.m_x ) );

	/* one ulp of the origin coordinate along the wall normal ON CONTACT ( a lead starts up to the lead range off the wall ) */
	float ulp = 1e-7f;
	{
		const float coord = std::max( std::fabsf( ( org.m_x - n.m_x * gap ) * n.m_x ), std::fabsf( ( org.m_y - n.m_y * gap ) * n.m_y ) );
		int ex            = 0;
		std::frexp( coord, &ex );
		if ( coord > 0.f )
			ulp = std::ldexp( 1.f, ex - 24 );
	}

	/* the brush bottom B the head will cross, per stance ( other stance = air duck / unduck, head at z + 63 ). h* = head over B on the
	   crossing tick; from the first pin every fall is a multiple of g*dt^2, so the ride phase phi = h* mod g*dt^2 is locked */
	const float head_now = org.m_z + maxs.m_z;
	const edge_info_t edge = edge_find( filter, org, n, plane_d, head_now + 9.05f, org.m_z - 24.f );
	const float period     = grav * ipt * ipt;
	const auto crossing    = [ & ]( const float head_off, float& h_star, float& feet_at ) {
		float z = org.m_z, vz = vel.m_z;
		for ( int k = 0; k < 64; ++k ) {
			const float vmid = vz - grav * 0.5f * ipt;
			const float zn   = z + vmid * ipt;
			if ( z + head_off > edge.B && zn + head_off <= edge.B ) {
				h_star  = z + head_off - edge.B;
				feet_at = z;
				return k;
			}
			z  = zn;
			vz = vmid - grav * 0.5f * ipt;
			if ( z + head_off < edge.B )
				break;
		}
		return -1;
	};
	float hc = 0.f, ho = 0.f, fc = org.m_z, fo = org.m_z;
	const int kc = edge.ok ? crossing( maxs.m_z, hc, fc ) : -1;
	const int ko = edge.ok ? crossing( 63.f, ho, fo ) : -1;
	/* air duck lifts the feet 9 ( unduck drops them ): D = T - feet moves with it */
	fo += ducked0 ? -9.f : 9.f;
	/* hold tick, exact ( lab edge_probe E, 11-13 ): the into-wall step is k whole ulps of the origin and pins iff period / phi <= k and
	   k * ulp < ( eps - ulp ) * period / ( D + eps ), D = brush top - feet on the hold. kmax = whole ulps under that bound:
	   hold iff phi >= period / kmax, kmax 1 = never ( 64 tick: wall coord >= 512 never ). gap math floor = one ulp of 16 */
	const auto band = [ & ]( const int k, const float h, const float feet, float& lo, float& phi ) {
		phi = k >= 0 ? std::fmod( h, period ) : -1.f;
		const float u  = std::max( ulp, 1.9073486e-6f );
		const float D  = std::max( edge.T - ( feet - ( h - phi ) ), 1.f );
		const int kmax = static_cast< int >( 0.97f * ( k_eps - u ) * period / ( ( D + k_eps ) * u ) );
		lo             = kmax >= 2 ? 1.03f * period / static_cast< float >( kmax ) : period;
		return k >= 0 && phi >= lo && phi < period - 1e-4f;
	};
	float lo_c = 0.f, lo_o = 0.f, phi_c = -1.f, phi_o = -1.f;
	const bool in_c = band( kc, hc, fc, lo_c, phi_c ), in_o = band( ko, ho, fo, lo_o, phi_o );
	/* band stance: the one you are in first ( a flip moves the feet 9 ), kept while it stays in band */
	const auto in_band = [ & ]( const int duck ) { return duck == ( ducked0 ? 1 : 0 ) ? in_c : in_o; };
	if ( s_cmd != cmd->m_command_number - 1 )
		drop( );
	s_cmd = cmd->m_command_number;
	if ( s_pick < 0 || !in_band( s_pick ) )
		s_pick = in_c ? ( ducked0 ? 1 : 0 ) : in_o ? ( ducked0 ? 0 : 1 ) : -1;

	char act     = 'N';
	float dx_hit = 0.f;
	int sims     = 0;
	const auto log = [ & ]( ) {
		if ( !debug )
			return;
		static unsigned long long s_next = 0ull;
		static char s_last_act           = 0;
		const unsigned long long now     = GetTickCount64( );
		if ( now < s_next && act == s_last_act )
			return;
		s_next     = now + 250ull;
		s_last_act = act;
		botox_dbg_log( "ESURF: %s act=%c gap=%.6f edge=%d B=%.3f T=%.3f head=%.3f k=%d/%d h=%.4f/%.4f phi=%.4f/%.4f lo=%.3f/%.3f period=%.4f band=%d/%d ulp=%.1e pick=%d run=%d dx=%.3e sims=%d mg=%d vz=%.1f xy=%.1f",
		               lead ? "lead" : "touch", act, gap, edge.ok ? 1 : 0, edge.B, edge.T, head_now, kc, ko, hc, ho, phi_c, phi_o, lo_c, lo_o, period,
		               in_c ? 1 : 0, in_o ? 1 : 0, ulp, s_pick, s_run, dx_hit, sims, s_park_margin, vel.m_z, vel.length_2d( ) );
	};
	/* no stance can hold here = a 1-tick pin at best ( 10-31 log ): hands off */
	if ( s_pick < 0 ) {
		s_run = 0;
		log( );
		return false;
	}
	g_edge_surf_active = true;
	const int stance   = s_pick;

	struct end_t {
		bool ok = false, ride = false, ground = false;
		c_vector org{ }, vel{ };
	};
	const auto sim = [ & ]( c_user_cmd c ) {
		end_t e;
		if ( sims_left( ) <= 0 )
			return e;
		tb_restore( frame );
		pred_simulate( &c );
		++sims;
		e.org    = local->get_origin( );
		e.vel    = local->get_velocity( );
		e.ground = ( local->get_flags( ) & fl_onground ) != 0;
		e.ok     = local->get_move_type( ) == e_move_types::move_type_walk; /* a ladder latch is never ours */
		/* START vz == -pin lands on the pin with no contact: falling STARTs only */
		e.ride   = e.ok && !e.ground && std::fabsf( e.vel.m_z - pin ) < 1e-3f && e.vel.length_2d( ) >= 1.f && vel.m_z + pin < 0.f;
		return e;
	};
	const auto with_duck = [ ]( c_user_cmd c, const int duck ) {
		c.m_buttons = duck ? ( c.m_buttons | in_duck ) : ( c.m_buttons & ~in_duck );
		return c;
	};
	const auto commit = [ & ]( const c_user_cmd& c ) {
		cmd->m_forward_move = c.m_forward_move;
		cmd->m_side_move    = c.m_side_move;
		cmd->m_buttons      = c.m_buttons;
	};
	const float crop  = ( stance || ducked0 ) ? k_duck_crop : 1.f;
	const float v_old = -( vel.m_x * n.m_x + vel.m_y * n.m_y ) / nxy;
	/* silent press along the wall normal that ends the tick closing at v_plane ( AirAccelerate inverse ) */
	const auto press = [ & ]( const float v_plane ) {
		const float v_t  = ( v_plane + ( vel.m_z + pin ) * n.m_z ) / nxy;
		float w          = v_t >= v_old ? std::max( ( v_t - v_old ) / k_acc, v_t ) : -std::max( ( v_old - v_t ) / k_acc, -v_t );
		w                = std::clamp( w / crop, -k_wish_max, k_wish_max );
		c_user_cmd c     = with_duck( in, stance );
		c.m_forward_move = std::fabsf( w );
		c.m_side_move    = 0.f;
		move_fix_to_yaw( w >= 0.f ? yaw_in : yaw_in + 180.f, c );
		c.m_buttons &= ~k_move_keys;
		return c;
	};

	/* server took less of the last park than planned = knife edge: leave margin ulps ( texture bug 10-02 ) */
	if ( s_park_cmd >= 0 && cmd->m_command_number == s_park_cmd + 1 ) {
		if ( s_park_from - gap < 0.5f * ( s_park_from - s_park_to ) || ( s_park_to < 0.5f * k_eps && gap > 0.9f * k_eps ) )
			s_park_margin = s_park_margin ? s_park_margin * 2 : 2;
		else
			s_park_margin = 0;
	}
	s_park_cmd = -1;

	/* touching: whole-ulp ladder, send the middle of the pinning run */
	if ( gap <= k_touch ) {
		const float g  = std::max( gap, 1e-7f );
		const float dz = std::fabsf( ( vel.m_z + pin ) * ipt ) + 1e-4f;
		const float hi = std::max( ( k_eps - g ) * dz / ( ( ( stance || ducked0 ) ? 54.f : 72.f ) + k_eps ), g * 1.5f );
		/* ponytail: no server shift comp ( texture bug's dn median ), add if ESURF shows holds lost with run=1 windows */
		float run_dx[ k_ladder_cap ];
		int run_n = 0, k = 0;
		for ( float dx = g * 1.0005f + 1e-7f; dx <= hi && k < k_ladder_cap; dx = std::max( dx * k_ratio, dx + ulp ), ++k ) {
			const end_t e = sim( press( dx / ipt ) );
			if ( !e.ok && sims_left( ) <= 0 )
				break;
			if ( e.ok && e.ride )
				run_dx[ run_n++ ] = dx;
			else if ( run_n )
				break;
		}
		if ( run_n ) {
			dx_hit = run_dx[ run_n / 2 ];
			commit( press( dx_hit / ipt ) );
			pinned = held = true;
			g_edge_surf_pin_cmd = cmd->m_command_number;
			++s_run;
			act = 'P';
			log( );
			return true;
		}
		s_run = 0;
	}

	/* delta strafe: more than two ticks off the face, turn the velocity toward the wall by the set degrees per tick with a wish
	   square to it ( air strafe: keeps the speed ), instead of the straight press */
	const float delta = std::clamp( GET_VARIABLE( g_variables.m_edge_surf_delta_strafe, float ), 0.f, 5.f );
	const float vxy   = vel.length_2d( );
	/* closing speed stays under what one park tick can brake: past that the wall eats it as a hard hit */
	if ( delta > 0.f && vxy > 30.f && gap > std::max( v_old * nxy, 0.f ) * ipt * 2.f + 1.f &&
	     v_old * nxy < k_acc * std::max( local->get_max_speed( ), 0.f ) * crop ) {
		const float side = ( vel.m_y * n.m_x - vel.m_x * n.m_y ) >= 0.f ? 1.f : -1.f; /* wall left of the velocity = turn left */
		const float add  = std::min( vxy * std::tanf( deg2rad( delta ) ), 30.f );
		c_user_cmd c     = with_duck( in, stance );
		c.m_forward_move = std::min( std::max( add / k_acc, add ) / crop, k_wish_max );
		c.m_side_move    = 0.f;
		move_fix_to_yaw( rad2deg( std::atan2f( vel.m_y, vel.m_x ) ) + side * 90.f, c );
		c.m_buttons &= ~k_move_keys;
		if ( const end_t e = sim( c ); e.ok && !e.ground ) {
			commit( c );
			act = 'D';
			log( );
			return true;
		}
	}

	/* park: press that ends short of the plane, tightest first, never under 1 ulp ( a sub-ulp end = the plane or a hard hit ) */
	if ( s_park_margin <= 16 ) {
		const float floor_gap = static_cast< float >( std::max( s_park_margin, 1 ) ) * ulp;
		float last_end        = -1.f;
		for ( const float keep : k_keep ) {
			const float end = std::max( gap * keep, floor_gap );
			if ( end >= gap )
				break;
			if ( end == last_end )
				continue;
			last_end           = end;
			const c_user_cmd c = press( ( gap - end ) / ipt );
			const end_t e      = sim( c );
			if ( !e.ok || e.ground )
				continue;
			const float g1     = ( e.org.m_x - org.m_x ) * n.m_x + ( e.org.m_y - org.m_y ) * n.m_y + ( e.org.m_z - org.m_z ) * n.m_z + gap;
			const bool touched = -( e.vel.m_x * n.m_x + e.vel.m_y * n.m_y ) <= 1e-3f;
			if ( g1 > 0.f && g1 < gap && !touched && g1 >= floor_gap * 0.5f ) {
				commit( c );
				s_park_cmd  = cmd->m_command_number;
				s_park_from = gap;
				s_park_to   = g1;
				act         = 'K';
				log( );
				return true;
			}
		}
	}

	/* flush, no pin this tick ( the head is not on the edge yet ): your move in the band stance */
	const c_user_cmd mine = with_duck( in, stance );
	commit( mine );
	log( );
	return mine.m_buttons != in.m_buttons || gap <= k_touch;
}

/* ---- feet mode: a LONE ledge top edge, real ( lab tools/wc_engine: edge_probe G, edge_real 62 / 64 flights with real cmds,
   de_ancient bsp 63 / 64 at 128 tick ).
   the box sweep reads the hull CENTRE ( feet + half the hull ). feet rest at top + 1/32; on a top off the centre's float grid ( top
   in ( 2^k - 36, 2^k ), odd low bit ) the centre rounds up: the engine reads the feet r over the stop plane EVERY tick, so the +z
   hit has fraction r / fall. past MINIMUM_MOVE_FRACTION the bump after it runs ( 1 - fraction ) of the tick: a closing step of half
   an ulp rounds onto the ledge on bump 1 ( = the +z hit ) and stays off it on bump 2 = vz 0, never grounded. nothing ever clips
   the closing speed: one press, then the hold is a coast.
   ANY ledge pins on the arrival tick the same way ( its +z fraction is the fall phase, the gap is planned for it ). a top on the grid
   has fraction 0 after that = bump 2 would land you: one tick of surf ( vz reset, speed kept, never grounded ), then it kills the
   closing speed and you drop beside the ledge ( lab 60-63 / 64 flights, any top, any wall coord, 64 + 128 tick ).
   longer on a grid top: standing, an air duck on the tick after the pin lifts the feet 9, the fall back is a second arrival = a
   second pin ( act U; 12 ticks at the edge at 64 tick, 21 at 128 ). nothing else lifts the feet off the stop plane, so 2 pins is
   the cap. ponytail: arriving ducked = one pin ( an unduck on the way in would be eaten by the 0.4 s duck gate before the lift ).
   guide: reach slider = straight pull distance, delta strafe slider = turn the velocity in and back ( speed kept ) out to the lead range,
   a run-in that reaches the face anyway = the straight stop-in-time profile ( act S ).
   an approach is OURS from its first guide / pin tick ( latch ) until the feet are under the top: over the top one closing ulp lands
   you ON the ledge, so nothing we own goes back to your keys / other tricks there, a miss = closing 0 = drop beside it ( act H ).
   ponytail: your along-edge keys are dropped while it holds ( any wish leaks > the window into the wall axis ); a combined cmd
   needs a 2 sim secant per tick. stance is held. world brushes with axial faces only. */
static float es_ulp( const float a )
{
	int ex = 0;
	std::frexp( a, &ex );
	return a != 0.f ? std::ldexp( 1.f, ex - 24 ) : 1e-7f;
}

/* how far over top + eps the sweep reads resting feet: cmodel.cpp IntersectRayWithBoxBrush float steps. volatile: the dll's float
   model must not fold these. a non-box brush ( CM_ClipBoxToBrush pushes the plane out by the hull first: both sides round alike )
   reads 0 on every top, and a trace cannot tell the two apart: such a ledge parks, then misses ( act M ) */
static float es_seen_box( const float top, const float off )
{
	volatile float z = top + 0.03125f;
	volatile float c = z + off;
	volatile float a = top - c;
	a                = a + off;
	a                = a + 0.03125f;
	return -a;
}

struct es_face_t {
	int ax     = 0;    /* wall axis */
	float sg   = 1.f;  /* side of the face the hull is on */
	float face = 0.f;  /* face coordinate on that axis */
	float hw   = 16.f; /* hull half width */
	float top  = 0.f;
};

/* hull face to the ledge face plane at centre coordinate x, the sweep's own float steps */
static float es_gap_at( const es_face_t& f, const float x )
{
	volatile float d = f.sg < 0.f ? f.face - x : x - f.face;
	d                = d - f.hw;
	return d;
}

/* free fall from ( z, vz ), engine float steps: ticks until the tick whose fall crosses the top + that tick's +z fraction.
   false = feet under the top, or the crossing tick starts inside the eps band ( fraction 0 = bump 2 never runs short = no catch ) */
static bool es_crossing( const es_face_t& f, float z, float vz, const float off, const float pin, const float ipt, int& k, float& frac0,
                         float* zc = nullptr, float* vzc = nullptr )
{
	for ( k = 0; k < 256; ++k ) {
		volatile float v1 = vz + pin;
		volatile float dz = ipt * v1;
		volatile float z1 = z + dz;
		volatile float c  = z + off;
		volatile float a  = f.top - c;
		a                 = a + off;
		if ( a >= 0.f )
			return false;
		volatile float dlt = z1 - z;
		if ( !( dlt > a ) ) {
			volatile float ae  = a + 0.03125f;
			volatile float inv = 1.f / dlt;
			volatile float t   = ae * inv;
			frac0              = t;
			/* START of the crossing tick */
			if ( zc )
				*zc = z;
			if ( vzc )
				*vzc = vz;
			return t >= 1.05e-4f;
		}
		z  = z1;
		vz = v1 + pin;
	}
	return false;
}

/* arrival from gap m ulps: bump 1 ( the +z hit, fraction frac0 ) moves round( m * frac0 ) ulps in, bump 2 round( ( m - .5 )( 1 - frac0 ) ).
   both must round DOWN or the hull ends on the plane = grounded */
static bool es_arrival_ok( const es_face_t& f, const int m, const float frac0, const float e, const float u, const float ipt )
{
	const float in    = -f.sg;
	volatile float c0 = f.face + f.sg * f.hw;
	volatile float c  = c0 - in * ( static_cast< float >( m ) * u );
	volatile float v  = ( static_cast< float >( m ) - 0.5f ) * u * ( 1.f + e ) / ipt;
	volatile float st = ipt * v;
	volatile float e1 = c + in * st;
	volatile float d  = e1 - c;
	volatile float mv = d * frac0;
	volatile float p1 = c + mv;
	volatile float tl = ipt * frac0;
	tl                = ipt - tl;
	volatile float s2 = tl * v;
	volatile float e2 = p1 + in * s2;
	return es_gap_at( f, e1 ) <= 0.f && es_gap_at( f, p1 ) > 0.f && es_gap_at( f, e2 ) > 0.f;
}
/* smallest arrival gap that survives a 1e-5 slip of the fraction, inside half the eps shell. 0 = none */
static int es_pick_gap( const es_face_t& f, const float frac0, const float e, const float u, const float ipt, const int from = 1, const bool duck = false )
{
	for ( int m = std::max( from, 1 ); m <= 512 && static_cast< float >( m ) * u < 0.015f; ++m )
		if ( !es_is_bad( f.top, f.face, duck, frac0, m ) && es_arrival_ok( f, m, frac0, e, u, ipt ) &&
		     es_arrival_ok( f, m, frac0 * 0.99999f, e, u, ipt ) && es_arrival_ok( f, m, frac0 * 1.00001f, e, u, ipt ) )
			return m;
	return 0;
}

/* straight press profile along the wall axis ( act S, lab edge_real twin ): r = distance left to the arrival gap ( + = toward the
   face ), w = closing speed, a = air accel a tick. the fastest speed that still stops in whole decel ticks; a straight press never
   raises the speed along its own wish past sv_air_max_wishspeed 30 */
static float es_stop_speed( const float r, const float a, const float dt )
{
	const float ar = std::fabsf( r ), k = std::floor( ( std::sqrt( 1.f + 8.f * ar / ( a * dt ) ) - 1.f ) * 0.5f );
	return ( r >= 0.f ? 1.f : -1.f ) * ( ar / ( ( k + 1.f ) * dt ) + k * a * 0.5f );
}
/* stop speed planned at ap ( 3/4 of a ), the step may use the full a to get back on it: a tick that delivered less is made up next
   tick */
static float es_profile_step( const float r, const float w, const float ap, const float a, const float dt )
{
	const float up = std::min( w + a, std::max( w, 30.f ) ), dn = std::max( w - a, std::min( w, -30.f ) );
	return std::clamp( es_stop_speed( r, ap, dt ), dn, up );
}
/* ticks of that profile until one press lands the gap ( K's window at the full a ), -1 = not inside n. r_min = the face plane ( - the
   arrival gap ): a plan that touches it puts the hull over the top = standing on the ledge ( lab 11-17: overshoot and come back ) */
static int es_profile_ticks( float r, float w, const float ap, const float a, const float park, const float dt, const int n, const float r_min )
{
	for ( int t = 0; t <= n; ++t ) {
		if ( std::fabsf( r ) / dt <= park && std::fabsf( std::clamp( r / dt, -park, park ) - w ) <= a )
			return t;
		w = es_profile_step( r, w, ap, a, dt );
		r -= w * dt;
		if ( r <= r_min )
			return -1;
	}
	return -1;
}

static bool edge_surf_feet( c_user_cmd* cmd, const c_user_cmd& in, c_base_entity* local, const int frame, const c_vector& org, const c_vector& vel,
                            const c_vector& mins, const c_vector& maxs, const bool ducked0, const float ipt, const float pin, const float grav,
                            const float k_acc, const bool debug, bool& pinned, bool& held )
{
	constexpr float k_eps          = 0.03125f;
	constexpr float k_min_fraction = 1.05e-4f; /* gamemovement MINIMUM_MOVE_FRACTION 1e-4 + margin */
	constexpr float k_duck_crop    = 0.34f;
	constexpr float k_wish_max     = 450.f;
	constexpr float k_park_speed   = 25.f;
	constexpr int k_move_keys      = in_forward | in_back | in_moveleft | in_moveright;

	static bool s_on = false, s_own = false;
	static int s_cmd = -1, s_run = 0;
	static es_face_t s_face{ }, s_own_face{ };
	/* the engine checked arrival gap of this approach: ok = proven, cur = next to check, first = the float model's pick */
	static struct {
		es_face_t f{ };
		bool duck = false;
		float f0 = -1.f, u = 0.f;
		int ok = 0, cur = 0, first = 0, tries = 0;
	} s_plan;
	/* a missed tick ( key up, ground, another trick took it ) ends the latch: its START is not our plan any more */
	if ( s_cmd != cmd->m_command_number - 1 ) {
		s_on  = false;
		s_own = false;
		s_run = 0;
		/* a new approach searches again ( 24 tries each ), only a proven gap is kept */
		if ( !s_plan.ok )
			s_plan.f0 = -1.f;
	}
	s_cmd = cmd->m_command_number;
	/* every return publishes the latch for the next tick ( air stuck yields, skips get logged ) */
	struct latch_out_t {
		const bool& own;
		const int cmd;
		~latch_out_t( ) { g_edge_surf_latch_cmd = own ? cmd : -1; }
	} const latch_out{ s_own, cmd->m_command_number };

	/* curtain = hull footprint from the feet down to the fall ahead, swept along 8 yaws: first vertical world face under the feet */
	const float period = grav * ipt * ipt;
	const float off    = maxs.m_z * 0.5f;
	const float t_h    = ipt * static_cast< float >( n_tick::ticks( 48 ) );
	const float drop_z = std::clamp( -( vel.m_z + pin ) * t_h + 0.5f * grav * t_h * t_h, 4.f, 256.f );
	/* menu sliders: reach = how far the straight pull takes a ledge, delta strafe = deg a tick the velocity may turn toward one
	   ( then the lead range is the scan distance ) */
	const float reach = std::clamp( GET_VARIABLE( g_variables.m_edge_surf_reach, float ), 0.f, 32.f );
	/* lab edge_real 11-17: turn 10 vs 5 from gap 12 = 27 / 37 / 55 caught of 64 vs 3 / 17 / 37 ( closing -30 / 0 / 60 ), 0 stood. the
	   step is capped by one tick of air accel anyway ( 30: 6.8 deg at 250 u/s, 16.7 at 100 ) */
	const float turn  = std::clamp( GET_VARIABLE( g_variables.m_edge_surf_delta_strafe, float ), 0.f, 15.f );
	const float range = turn > 0.f ? std::max( std::clamp( GET_VARIABLE( g_variables.m_edge_surf_range, float ), 8.f, 512.f ), reach ) : reach + 0.05f;
	const c_vector cmins( mins.m_x, mins.m_y, -drop_z ), cmaxs( maxs.m_x, maxs.m_y, 0.f ), zero( 0.f, 0.f, 0.f );
	c_trace_filter_tb_world_props filter( local );
	const auto world_brush = [ ]( const trace_t& tr ) {
		return ( !tr.m_hit_entity || tr.m_hit_entity->get_index( ) == 0 ) && tr.m_hitbox == 0 &&
		       !( tr.surface.m_name && strstr( tr.surface.m_name, "displacement" ) );
	};
	es_face_t f{ };
	c_vector n{ };
	float gap = FLT_MAX, frac = 0.f;
	/* brush top behind a face at the hull: line down 0.5u inside. start solid = the wall goes on over the feet = no top edge */
	const auto top_behind = [ & ]( const es_face_t& c, float& top ) {
		c_vector q( org.m_x, org.m_y, org.m_z + 1.f );
		( c.ax ? q.m_y : q.m_x ) = c.face - c.sg * 0.5f;
		trace_t tt{ };
		ray_t line( q, c_vector( q.m_x, q.m_y, org.m_z - drop_z - 1.f ), zero, zero );
		tb_trace( line, mask_playersolid, &filter, &tt );
		if ( tt.m_start_solid || tt.m_fraction >= 1.f || tt.m_plane.m_normal.m_z < 0.999f || !world_brush( tt ) )
			return false;
		top = tt.m_plane.m_distance;
		return true;
	};
	for ( int i = 0; i < 8; ++i ) {
		const float a = deg2rad( static_cast< float >( i < 4 ? i * 90 : ( i - 4 ) * 90 + 45 ) );
		trace_t tr{ };
		ray_t ray( org, org + c_vector( std::cosf( a ), std::sinf( a ), 0.f ) * range, cmins, cmaxs );
		tb_trace( ray, mask_playersolid, &filter, &tr );
		const c_vector& tn = tr.m_plane.m_normal;
		if ( tr.m_start_solid || tr.m_fraction >= 1.f || std::fabsf( tn.m_z ) > 1e-4f || !world_brush( tr ) || botox_is_ladder_trace( tr ) )
			continue;
		es_face_t c{ };
		c.ax = std::fabsf( tn.m_x ) > 0.9999f ? 0 : std::fabsf( tn.m_y ) > 0.9999f ? 1 : -1;
		if ( c.ax < 0 )
			continue;
		c.sg          = ( c.ax ? tn.m_y : tn.m_x ) > 0.f ? 1.f : -1.f;
		c.face        = tr.m_plane.m_distance * c.sg;
		c.hw          = c.ax ? maxs.m_y : maxs.m_x;
		const float g = es_gap_at( c, c.ax ? org.m_y : org.m_x );
		if ( g <= 0.f )
			continue;
		if ( !top_behind( c, c.top ) )
			continue;
		if ( org.m_z <= c.top )
			continue;
		if ( g < gap ) {
			gap  = g;
			f    = c;
			frac = es_seen_box( c.top, off ) / period;
			n    = tn;
		}
	}
	/* owned and the scan lost it ( mid approach ): the latched face while its top still runs under the hull. a face the scan found
	   is the nearer danger and takes over ( cold review 11-17: past the ledge's end / a second face the latch never looked at ).
	   latch end: z = feet under the top ( dropped beside it, the goal ), p = hull on / past the face plane over the top ( = landed
	   on it, must never show ), e = the ledge ended under the hull */
	char ended = 0;
	if ( s_own && gap == FLT_MAX ) {
		es_face_t c   = s_own_face;
		c.hw          = c.ax ? maxs.m_y : maxs.m_x;
		const float g = es_gap_at( c, c.ax ? org.m_y : org.m_x );
		float t       = 0.f;
		if ( g > 0.f && org.m_z > c.top && top_behind( c, t ) && std::fabsf( t - c.top ) < 1e-3f ) {
			f    = c;
			gap  = g;
			n    = c_vector( c.ax ? 0.f : c.sg, c.ax ? c.sg : 0.f, 0.f );
			frac = es_seen_box( c.top, off ) / period;
		} else {
			ended = org.m_z <= c.top ? 'z' : g <= 0.f ? 'p' : 'e';
			s_own = false;
		}
	}
	/* every ledge pins on the arrival tick. only a top off the float grid keeps a +z fraction after it = holds on */
	const bool hold_ok = frac >= k_min_fraction;

	char act = 'N', why = '-';
	int m = 0, kc = 0, sims = 0;
	float f0 = 0.f;
	const float v_old = gap < FLT_MAX ? -( vel.m_x * n.m_x + vel.m_y * n.m_y ) : 0.f;
	const auto log = [ & ]( ) {
		if ( !debug )
			return;
		static unsigned long long s_next = 0ull;
		static char s_last_act           = 0;
		const unsigned long long now     = GetTickCount64( );
		/* an owned approach logs every tick ( 11-17: a repeated act inside the 250 ms throttle hid the whole fall back ) */
		if ( now < s_next && act == s_last_act && !s_own )
			return;
		s_next     = now + 250ull;
		s_last_act = act;
		botox_dbg_log( "ESURF: feet act=%c why=%c top=%.5f face=%.3f gap=%.3e m=%d kc=%d f0=%.4f frac=%.2e hold=%d close=%.4e run=%d own=%d sims=%d vz=%.1f xy=%.1f duck=%d",
		               act, why, f.top, f.face, gap, m, kc, f0, frac, hold_ok ? 1 : 0, v_old, s_run, s_own ? 1 : 0, sims, vel.m_z, vel.length_2d( ),
		               ducked0 ? 1 : 0 );
	};
	if ( ended ) {
		act = 'L';
		why = ended;
		log( );
	}
	if ( gap == FLT_MAX ) {
		s_on  = false;
		s_run = 0;
		return false;
	}
	if ( s_on && ( f.ax != s_face.ax || f.sg != s_face.sg || std::fabsf( f.face - s_face.face ) > 0.01f || std::fabsf( f.top - s_face.top ) > 1e-3f ) ) {
		s_on  = false;
		s_run = 0;
	}
	/* your keys off the ledge = you leave. your RAW keys in your own view: auto strafe / auto align / air stuck moves in `in` are not
	   you leaving */
	const float x = f.ax ? org.m_y : org.m_x;
	const float u = es_ulp( x );
	c_user_cmd raw   = in;
	raw.m_view_point = g_ctx.old_view_point;
	const bool away  = tb_steering_away_from_wall( &raw, n, g_movement.m_user_forward_move_raw, g_movement.m_user_side_move_raw );

	const float yaw_in = rad2deg( std::atan2f( -n.m_y, -n.m_x ) );
	const float crop   = ducked0 ? k_duck_crop : 1.f;
	const auto hold_stance = [ & ]( c_user_cmd c ) {
		c.m_buttons = ducked0 ? ( c.m_buttons | in_duck ) : ( c.m_buttons & ~in_duck );
		c.m_buttons &= ~k_move_keys;
		return c;
	};
	/* silent press along the wall axis that ends this tick's accel closing at v_t ( AirAccelerate inverse ). VectorNormalize divides
	   by ( len + FLT_EPSILON ): a tiny wish comes out FLT_EPSILON short, 2e-5 of a hold step. cr = the move scale THIS cmd gets:
	   pressing duck crops it by 0.34 even while standing ( HandleDuckingSpeedCrop, the U lift ) */
	const auto press_v = [ & ]( const float v_0, const float v_t, const float cr ) {
		float w = v_t >= v_0 ? std::max( ( v_t - v_0 ) / k_acc, v_t ) : -std::max( ( v_0 - v_t ) / k_acc, -v_t );
		w += w >= 0.f ? FLT_EPSILON : -FLT_EPSILON;
		w                = std::clamp( w / cr, -k_wish_max, k_wish_max );
		c_user_cmd c     = hold_stance( in );
		c.m_forward_move = std::fabsf( w );
		c.m_side_move    = 0.f;
		move_fix_to_yaw( w >= 0.f ? yaw_in : yaw_in + 180.f, c );
		return c;
	};
	const auto press_cr = [ & ]( const float v_t, const float cr ) { return press_v( v_old, v_t, cr ); };
	const auto press = [ & ]( const float v_t ) { return press_cr( v_t, crop ); };
	const auto commit = [ & ]( const c_user_cmd& c ) {
		cmd->m_forward_move = c.m_forward_move;
		cmd->m_side_move    = c.m_side_move;
		cmd->m_buttons      = c.m_buttons;
	};

	/* this tick is ours: the approach latches, nothing after tb rewrites the cmd ( held ), the indicator lights */
	const auto own = [ & ]( const char a ) {
		s_own              = true;
		s_own_face         = f;
		held               = true;
		g_edge_surf_active = true;
		act                = a;
		s_es_sent          = { cmd->m_command_number, cmd->m_buttons, cmd->m_view_point.m_y, cmd->m_forward_move, cmd->m_side_move };
		log( );
		return true;
	};
	/* hands off. an approach we own, or a ledge inside reach, never goes back over the top to your keys / other tricks: one closing
	   ulp there lands you ON the ledge ( 11-17 log, all 10 act=U; lab edge_real ER_GAME_UP 64 / 64 stood ). act H instead: a closing
	   speed that still ends the crossing tick short of the face plane stays yours ( a later tick may still catch ), only the excess
	   is braked = you drop beside the ledge, along-edge speed kept. why = the miss */
	const auto let_go = [ & ]( const char miss ) {
		act = miss;
		if ( !( s_own || gap <= reach ) || org.m_z <= f.top ) {
			log( );
			return false;
		}
		int kh  = 0;
		float fh = 0.f;
		es_crossing( f, org.m_z, vel.m_z, off, pin, ipt, kh, fh );
		const float v_ok = std::max( 0.f, ( gap - u ) / ( static_cast< float >( kh + 1 ) * ipt ) );
		/* act Y: a ledge we never guided that your keys cannot reach this tick = the fall stays yours ( log 11-17: 2 approaches held
		   22 ticks each from 9 / 31 u out and never arrived ). one tick of your air accel adds min( accel * max speed, 30 + speed ) at
		   most ( 47 at sv_airaccelerate 12, far more on a surf server: cold review 11-17 ); the room left covers that tick, one more
		   and the brake's stop distance */
		const float max_spd = std::max( local->get_max_speed( ), 0.f );
		const float v_1     = std::max( v_old, 0.f ) + std::max( 30.f, std::min( k_acc * max_spd, 30.f + vel.length_2d( ) ) );
		const float brake   = std::max( k_acc * max_spd * crop, 1.f );
		if ( !s_own && v_old <= v_ok && gap > v_1 * ipt * ( 2.f + v_1 / ( 2.f * brake ) ) + 0.05f ) {
			act = 'Y';
			why = miss;
			log( );
			return false;
		}
		why = miss;
		commit( press( std::min( v_old, v_ok ) ) );
		return own( 'H' );
	};

	/* |coord| < 16: the gap grid ( ulp of 16 ) is coarser than the origin's */
	if ( away || u < 1.9073486e-6f ) {
		const bool over = away && ( s_own || gap <= reach ) && org.m_z > f.top;
		s_on            = false;
		s_own           = false;
		s_run           = 0;
		if ( !over )
			return false;
		/* leaving over the top: YOUR raw keys go out, not `in` ( auto strafe / auto align in it may close on the face = land on it,
		   cold review 11-17 ). next tick is yours again */
		c_user_cmd c     = in;
		c.m_forward_move = g_movement.m_user_forward_move_raw;
		c.m_side_move    = g_movement.m_user_side_move_raw;
		move_fix_to_yaw( g_ctx.old_view_point.m_y, c );
		c.m_buttons = ( in.m_buttons & ~k_move_keys ) | ( g_movement.m_user_buttons_raw & k_move_keys );
		commit( c );
		held      = true;
		s_es_sent = { cmd->m_command_number, cmd->m_buttons, cmd->m_view_point.m_y, cmd->m_forward_move, cmd->m_side_move };
		act       = 'A';
		log( );
		return true;
	}

	const bool riding = std::fabsf( org.m_z - ( f.top + k_eps ) ) < 0.01f && std::fabsf( vel.m_z - pin ) < 1e-3f;
	/* START already pinned somewhere else ( head hold, tb ride, pixel surf over this ledge ): the fall model below is wrong, not ours */
	if ( !riding && std::fabsf( vel.m_z - pin ) < 1e-3f ) {
		if ( s_own ) {
			act = 'O';
			log( );
		}
		s_on  = false;
		s_own = false;
		s_run = 0;
		return false;
	}
	float zc = org.m_z, vzc = vel.m_z;
	const bool cross = !riding && es_crossing( f, org.m_z, vel.m_z, off, pin, ipt, kc, f0, &zc, &vzc );
	/* closing step window, relative: the hold fraction. a plain top has only the arrival fraction: stay low in it ( a step far over
	   the half ulp leaves few arrival gaps: lab 38 / 64 at half the fraction, 62 / 64 capped at 2e-3 ) */
	const float base = hold_ok ? frac : std::min( f0 * 0.5f, 2e-3f );

	/* guide: above the top = park the hull at the arrival gap, closing speed 0 after */
	if ( !riding && ( !cross || kc > 0 ) ) {
		s_on  = false;
		s_run = 0;
		/* crossing tick inside the eps band = no catch */
		if ( !cross )
			return let_go( 'B' );
		/* px "checked plan": the arrival gap is flown in the ENGINE from the crossing tick's START ( hull parked on the gap, closing
		   0, the fall es_crossing flew ) before the park commits to it. the float model alone parked on gaps the game refused ( log
		   11-17: 9 of 29 approaches, lab edge_real pins the same ledges 62 / 64 ). 3 sims a tick, 24 an approach; kept while ledge /
		   stance / fall phase stay. nothing proven = the model's first gap */
		/* a gap the server / the crossing sim refused since ( bad ring ) drops the plan that holds it ( cold review 11-17: a proven
		   gap came back on every retry ) */
		if ( !( s_plan.f0 >= 0.f && s_plan.f.ax == f.ax && s_plan.f.sg == f.sg && std::fabsf( s_plan.f.face - f.face ) < 0.01f &&
		        std::fabsf( s_plan.f.top - f.top ) < 1e-3f && s_plan.duck == ducked0 && s_plan.u == u && std::fabsf( s_plan.f0 - f0 ) < 5e-5f ) ||
		     es_is_bad( f.top, f.face, ducked0, f0, s_plan.ok ? s_plan.ok : s_plan.cur ? s_plan.cur : s_plan.first ) ) {
			s_plan       = { };
			s_plan.f     = f;
			s_plan.duck  = ducked0;
			s_plan.u     = u;
			s_plan.f0    = f0;
			s_plan.first = s_plan.cur = es_pick_gap( f, f0, base * 0.5f, u, ipt, 1, ducked0 );
		}
		for ( int n = 0; n < 3 && !s_plan.ok && s_plan.cur && s_plan.tries < 24 && sims_left( ) > 8; ++n ) {
			/* built START: hull centre mm ulps off the face plane, wall axis speed 0 */
			c_vector o = org, v = vel;
			( f.ax ? o.m_y : o.m_x ) = ( f.face + f.sg * f.hw ) + f.sg * ( static_cast< float >( s_plan.cur ) * u );
			o.m_z                    = zc;
			( f.ax ? v.m_y : v.m_x ) = 0.f;
			v.m_z                    = vzc;
			tb_restore( frame );
			local->set_abs_origin( o );
			local->get_origin( )   = o;
			/* SetupMove reads GetAbsVelocity: a velocity write needs EFL_DIRTY_ABSVELOCITY ( wall_climb.cpp set_vel ) */
			local->get_velocity( ) = v;
			if ( int& eflags = local->get_eflags( ); reinterpret_cast< std::uintptr_t >( &eflags ) != reinterpret_cast< std::uintptr_t >( local ) )
				eflags |= 1 << 12;
			c_user_cmd c = press_v( 0.f, ( static_cast< float >( s_plan.cur ) - 0.5f ) * u * ( 1.f + 0.5f * base ) / ipt, crop );
			pred_simulate( &c );
			++sims;
			const c_vector eo = local->get_origin( ), ev = local->get_velocity( );
			const bool gnd    = ( local->get_flags( ) & fl_onground ) != 0;
			const bool pins   = local->get_move_type( ) == e_move_types::move_type_walk && !gnd && std::fabsf( ev.m_z - pin ) < 1e-3f &&
			                  std::fabsf( eo.m_z - ( f.top + k_eps ) ) < 0.01f;
			tb_restore( frame );
			if ( debug )
				botox_dbg_log( "ESURF: feet plan m=%d pins=%d tries=%d first=%d f0=%.4f kc=%d end gap=%.3e dz=%.5f vz=%.2f gnd=%d bad=%d", s_plan.cur,
				               pins ? 1 : 0, s_plan.tries, s_plan.first, f0, kc, es_gap_at( f, f.ax ? eo.m_y : eo.m_x ), eo.m_z - ( f.top + k_eps ),
				               ev.m_z, gnd ? 1 : 0, std::min( s_es_bad_n, 16 ) );
			if ( pins ) {
				s_plan.ok = s_plan.cur;
				break;
			}
			++s_plan.tries;
			s_plan.cur = es_pick_gap( f, f0, base * 0.5f, u, ipt, s_plan.cur + 1, ducked0 );
		}
		m = s_plan.ok ? s_plan.ok : s_plan.cur ? s_plan.cur : s_plan.first;
		if ( !m )
			return let_go( 'G' );
		const float err = gap - static_cast< float >( m ) * u;
		/* what one tick of air accel can change along the wall axis ( ducked = cropped ) */
		const float authority = k_acc * std::max( local->get_max_speed( ), 0.f ) * crop;
		const float park      = std::min( k_park_speed, authority );
		const float v_park    = std::clamp( err / ipt, -park, park );
		/* K needs a run-in one press settles; a faster one goes to the profile ( S ) */
		const bool settles = std::fabsf( v_park - v_old ) <= authority;
		/* K: one press lands the hull on the arrival gap */
		if ( std::fabsf( err ) / ipt <= park && settles ) {
			commit( press( v_park ) );
			return own( 'K' );
		}
		/* D, delta strafe: a wish square to the velocity turns it in, then back, on the fastest closing profile that still stops
		   before the crossing ( speed kept; lab edge_real ER_TURN ). d = closing speed one tick of turn moves = the turn step x the
		   wish's share of the wall normal. the step is one tick of air accel at most, not only 30 ( 128 tick standing 23, ducked 16 /
		   8 ): the 30 cap planned turns the engine cannot fly = late, onto the top ( lab 11-17 ducked turn 2 gap 24 closing 120 ) */
		const float vxy = vel.length_2d( );
		if ( turn > 0.f && vxy > 30.f ) {
			const float d = std::min( std::min( vxy * std::tanf( deg2rad( turn ) ), 30.f ), authority ) * std::sqrt( std::max( vxy * vxy - v_old * v_old, 0.f ) ) / vxy;
			if ( d >= 1.f ) {
				const float ae = std::fabsf( err );
				const int k    = static_cast< int >( std::floor( ( std::sqrt( 1.f + 8.f * ae / ( d * ipt ) ) - 1.f ) * 0.5f ) );
				const float vs = ( err >= 0.f ? 1.f : -1.f ) * ( ae / ( static_cast< float >( k + 1 ) * ipt ) + static_cast< float >( k ) * d * 0.5f );
				/* and D's own plan reaches K's window without touching the face plane: a run-in faster than its stop speed may never
				   catch up ( D sheds d a tick, the stop speed shrinks with the distance ) = past the face over the top ( cold review
				   11-17 ). K's last press absorbs a small excess, so it is flown, not refused outright. a touch = S / H instead */
				const auto d_reaches = [ & ] {
					float rr = err, ww = v_old;
					for ( int t = 0; t < kc; ++t ) {
						if ( std::fabsf( rr ) / ipt <= park && std::fabsf( std::clamp( rr / ipt, -park, park ) - ww ) <= authority )
							return true;
						ww = std::clamp( es_stop_speed( rr, d, ipt ), ww - d, ww + d );
						rr -= ww * ipt;
						if ( rr <= -static_cast< float >( m ) * u )
							return false;
					}
					return false;
				};
				if ( static_cast< float >( k + 1 ) + std::fabsf( vs - v_old ) / d <= static_cast< float >( kc - 1 ) && d_reaches( ) ) {
					const float dv = std::clamp( vs, v_old - d, v_old + d ) - v_old;
					float px = -vel.m_y / vxy, py = vel.m_x / vxy, into = -( px * n.m_x + py * n.m_y );
					if ( ( into < 0.f ) != ( dv < 0.f ) ) {
						px   = -px;
						py   = -py;
						into = -into;
					}
					const float add  = std::min( std::fabsf( dv ) / std::max( std::fabsf( into ), 1e-3f ), 30.f );
					c_user_cmd c     = hold_stance( in );
					c.m_forward_move = std::min( std::max( add / k_acc, add ) / crop, k_wish_max );
					c.m_side_move    = 0.f;
					move_fix_to_yaw( rad2deg( std::atan2f( py, px ) ), c );
					commit( c );
					return own( 'D' );
				}
			}
		}
		/* slow press drift: inside reach only, when it still arrives in time */
		if ( gap <= reach && std::fabsf( err ) / ( static_cast< float >( kc ) * ipt ) <= park && settles ) {
			commit( press( v_park ) );
			return own( 'K' );
		}
		/* S: inside reach, an approach already ours, or a run-in that reaches the face before the crossing anyway: the straight
		   stop-in-time profile lands K's window with a tick to spare, never touching the face plane. along the wall axis only,
		   along-edge speed kept ( lab 11-17 64 tick gap 24 closing 250: caught 0 -> 60 / 64, 0 planned misses ) */
		if ( ( gap <= reach || s_own || ( v_old > 0.f && v_old * ipt * static_cast< float >( kc ) >= err ) ) && kc >= 3 &&
		     es_profile_ticks( err, v_old, 0.75f * authority, authority, park, ipt, kc - 3, -static_cast< float >( m ) * u ) >= 0 ) {
			commit( press( es_profile_step( err, v_old, 0.75f * authority, authority, ipt ) ) );
			return own( 'S' );
		}
		return let_go( 'F' );
	}

	/* plain top, the tick after our pin: bump 2 would land you on the ledge. closing speed 0 = no hit = you drop beside it, speed kept.
	   U: standing, an air duck on that tick lifts the feet 9 ( FinishDuck in air ) = the fall back is a second arrival, guided and
	   pinned like the first: lab 12 ticks at the edge instead of 2 at 64 tick, 21 at 128. a refused duck ( 0.4 s gate ) = plain E */
	if ( riding && !hold_ok ) {
		if ( !s_on && !s_own )
			return false;
		c_user_cmd c = press( 0.f );
		act          = 'E';
		if ( !ducked0 && sims_left( ) > 0 ) {
			/* the lift's brake goes through the duck crop: at the standing scale 0.028 of the 0.043 u/s closing stayed = the 9u fall
			   back drifted 25 ulps over the ledge = standing on it ( 11-17 game log, all 10 act=U; lab ER_GAME_UP coast 27 / 64 ) */
			c_user_cmd up = press_cr( 0.f, k_duck_crop );
			up.m_buttons |= in_duck;
			tb_restore( frame );
			pred_simulate( &up );
			++sims;
			const auto fl    = local->get_flags( );
			const c_vector v = local->get_velocity( );
			if ( ( fl & fl_ducking ) && !( fl & fl_onground ) && local->get_origin( ).m_z > org.m_z + 8.f && std::fabsf( v.m_x * n.m_x + v.m_y * n.m_y ) < 1e-3f ) {
				c   = up;
				act = 'U';
			}
		}
		commit( c );
		s_on  = false;
		s_run = 0;
		/* tb must keep this cmd too: a wiped brake = the landing bump. U keeps the latch = the fall back is still ours */
		return own( act );
	}

	/* crossing / holding tick: coast if the sim rides, else press to ( m - 0.5 ) ulps a tick inside the window */
	m = std::max( static_cast< int >( std::lround( gap / u ) ), 1 );
	struct end_t {
		bool ok = false, ride = false, gnd = false;
		float close = 0.f, gap = 0.f, dz = 0.f, vz = 0.f;
	};
	const auto sim = [ & ]( c_user_cmd c ) {
		end_t e;
		if ( sims_left( ) <= 0 )
			return e;
		tb_restore( frame );
		pred_simulate( &c );
		++sims;
		const c_vector o = local->get_origin( ), v = local->get_velocity( );
		e.ok    = local->get_move_type( ) == e_move_types::move_type_walk;
		e.gnd   = ( local->get_flags( ) & fl_onground ) != 0;
		e.ride  = e.ok && !e.gnd && std::fabsf( v.m_z - pin ) < 1e-3f && std::fabsf( o.m_z - ( f.top + k_eps ) ) < 0.01f;
		e.close = -( v.m_x * n.m_x + v.m_y * n.m_y );
		e.gap   = es_gap_at( f, f.ax ? o.m_y : o.m_x );
		e.dz    = o.m_z - ( f.top + k_eps );
		e.vz    = v.m_z;
		return e;
	};
	const c_user_cmd idle = [ & ] {
		c_user_cmd c     = hold_stance( in );
		c.m_forward_move = c.m_side_move = 0.f;
		return c;
	}( );
	c_user_cmd best = idle;
	bool got        = false;
	/* clock gone mid hold: the coast is the hold. a coast never catches: the crossing tick goes straight to the press */
	if ( sims_left( ) <= 0 )
		got = riding && s_on;
	else if ( riding && sim( idle ).ride )
		got = true;
	act = 'C';
	if ( !got && static_cast< float >( m ) * u < 0.015f ) {
		static const float k_e[] = { 0.5f, 0.25f, 0.75f, 0.1f, 0.9f };
		for ( const float e : k_e ) {
			const float want = ( static_cast< float >( m ) - 0.5f ) * u * ( 1.f + e * base ) / ipt;
			float ask        = want;
			for ( int pass = 0; pass < 2 && !got; ++pass ) {
				const c_user_cmd c = press( ask );
				const end_t r      = sim( c );
				/* every crossing sim, START + cmd + END: the lab pins what the game refuses ( 11-17 ), this line says where they part */
				if ( debug )
					botox_dbg_log( "ESURF: feet sim m=%d e=%.2f pass=%d ask=%.9g end close=%.9g gap=%.3e dz=%.5f vz=%.3f gnd=%d ride=%d | start gap=%.9g h=%.6f vz=%.4f v0=%.3e f0=%.5f | cmd fwd=%.9g side=%.9g yaw=%.4f pitch=%.3f btn=%x duck=%d kacc=%.5f max=%.1f",
					               m, e, pass, ask, r.close, r.gap, r.dz, r.vz, r.gnd ? 1 : 0, r.ride ? 1 : 0, gap, org.m_z - f.top, vel.m_z, v_old, f0,
					               c.m_forward_move, c.m_side_move, c.m_view_point.m_y, c.m_view_point.m_x, c.m_buttons, ducked0 ? 1 : 0, k_acc,
					               local->get_max_speed( ) );
				if ( r.ride ) {
					best = c;
					got  = true;
					act  = 'P';
					break;
				}
				/* the sim's closing speed is unclipped off the wall: one additive fix */
				if ( !r.ok || !( r.close > 0.f ) || std::fabsf( r.close - want ) > 0.5f * want || r.close == want )
					break;
				ask += want - r.close;
			}
			if ( got || sims_left( ) <= 0 )
				break;
		}
	}
	if ( !got ) {
		const char miss = s_on ? 'X' : 'M';
		/* an arrival the engine refused at the crossing: the next approach here parks on another gap */
		if ( !riding && static_cast< float >( m ) * u < 0.015f )
			es_mark_bad( f.top, f.face, ducked0, f0, m );
		s_on            = false;
		s_run           = 0;
		/* the head search under us reads the START state, not a failed sim's end */
		tb_restore( frame );
		return let_go( miss );
	}
	commit( best );
	/* a proven pin, like the head one: tb keeps the cmd ( wall climb must not wipe it ), other tricks yield */
	pinned = true;
	held   = true;
	s_on   = true;
	g_edge_surf_pin_cmd = cmd->m_command_number;
	s_face = f;
	++s_run;
	/* an arrival pin: the next START says whether the server took it ( es_real_check ) */
	if ( !riding )
		s_es_pin_sent = { cmd->m_command_number, m, f.top, f.face, f0, ducked0 };
	return own( act );
}
