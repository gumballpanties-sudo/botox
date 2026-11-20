#include "movement_internal.h"
#include "texturebug_common.h"
#include "edgebug.h"
#include "edge_catch.h"
#include "edge_skip.h"
#include "wall_climb.h"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <string>

using namespace n_tb;

extern void botox_dbg_log( const char* fmt, ... );
bool edge_surf_latched( const c_user_cmd* cmd );
float es_ulp( float a );
float es_hull_gap( const c_vector& o, const c_vector& n, float d, const c_vector& mins, const c_vector& maxs );
float es_max_speed( c_base_entity* local );
char edge_surf_guide( float err, float gap, float r_min, float v_old, float vxy, int kc, float authority, float turn, float reach, bool owned, float ipt,
                      float& v );

/* this cmd's move is air align's: auto strafe / strafe optimizer / jb assist / edge catch / edge skip yield, indicator lights. final stamp
   clears it if rewritten. _aa_off = auto align yields too: a pj ( its creep shoves the gap parked inside the lip ) or any approach not parked yet
   ( lumia contact seek / flush push under a closing plan = the 1/32 wall hit, log 11-18 sill ) */
bool g_air_align_owns   = false;
bool g_air_align_aa_off = false;
/* a ps point on a ledge top in radius, key down: arms edge surf ( edge_surf_armed ), its feet mode plans that pin's arrival gap in ulps */
bool g_air_align_ledge = false;

static struct {
	int cmd     = -1;
	bool duck   = false;
	float crop  = 1.f, wx = 0.f, wy = 0.f;
} s_aal_sent;

static void aal_wish( const c_user_cmd& c, float& wx, float& wy )
{
	const float a = deg2rad( c.m_view_point.m_y );
	wx            = c.m_forward_move * std::cosf( a ) + c.m_side_move * std::sinf( a );
	wy            = c.m_forward_move * std::sinf( a ) - c.m_side_move * std::cosf( a );
}

/* finder dots sit on their face: lines 2u out -> 2u in, the plane nearest the dot wins ( a seam / lip top can start or end a face there ).
   a miss retries after 1 s ( a door / player in the way ) */
static bool aal_wall( points_check_t& p, c_base_entity* local, const bool debug )
{
	const float now = g_interfaces.m_global_vars_base->m_real_time;
	if ( p.aa_state == 1 || ( p.aa_state == 2 && now < p.aa_retry ) )
		return p.aa_state == 1;
	c_trace_filter_tb_world_props filter( local );
	float best = 0.25f;
	for ( const float dz : { 1.f, -1.f, 4.f, -4.f, 16.f, -16.f } )
		for ( int i = 0; i < 16; ++i ) {
			const float a = deg2rad( 22.5f * static_cast< float >( i ) );
			const c_vector dir( std::cosf( a ), std::sinf( a ), 0.f ), at( p.pos.m_x, p.pos.m_y, p.pos.m_z + dz );
			trace_t tr{ };
			ray_t ray( at + dir * 2.f, at - dir * 2.f );
			tb_trace( ray, mask_playersolid, &filter, &tr );
			const c_vector& tn = tr.m_plane.m_normal;
			if ( tr.m_start_solid || tr.m_fraction >= 1.f || std::fabsf( tn.m_z ) > 1e-6f || botox_is_ladder_trace( tr ) )
				continue;
			const float len = std::sqrt( tn.m_x * tn.m_x + tn.m_y * tn.m_y );
			const c_vector n( tn.m_x / len, tn.m_y / len, 0.f );
			const float d   = ( hull_plane_dist( tr, c_vector( 0.f, 0.f, 0.f ), c_vector( 0.f, 0.f, 0.f ) ) - tn.m_z * p.pos.m_z ) / len;
			const float off = std::fabsf( p.pos.m_x * n.m_x + p.pos.m_y * n.m_y - d );
			if ( off >= best )
				continue;
			best   = off;
			p.aa_n = n;
			p.aa_d = d;
		}
	p.aa_state = best < 0.25f ? 1 : 2;
	p.aa_retry = now + 1.f;
	p.aa_ledge = false;
	if ( p.aa_state == 1 ) {
		/* the same face 2u over the dot = a wall; nothing / another plane there = the dot sits on a ledge top */
		const c_vector at( p.pos.m_x, p.pos.m_y, p.pos.m_z + 2.f );
		trace_t tr{ };
		ray_t ray( at + p.aa_n * 2.f, at - p.aa_n * 2.f );
		tb_trace( ray, mask_playersolid, &filter, &tr );
		p.aa_ledge = tr.m_start_solid || tr.m_fraction >= 1.f || tr.m_plane.m_normal.m_x * p.aa_n.m_x + tr.m_plane.m_normal.m_y * p.aa_n.m_y < 0.9999f ||
		             std::fabsf( hull_plane_dist( tr, c_vector( 0.f, 0.f, 0.f ), c_vector( 0.f, 0.f, 0.f ) ) - p.aa_d ) > 0.25f;
	}
	if ( debug )
		botox_dbg_log( "AIRAL: wall %s pos=%.2f %.2f %.2f off=%.4f n=%.3f %.3f ledge=%d", p.aa_state == 1 ? "ok" : "none", p.pos.m_x, p.pos.m_y, p.pos.m_z, best,
		               p.aa_n.m_x, p.aa_n.m_y, p.aa_ledge ? 1 : 0 );
	return p.aa_state == 1;
}

/* how far the face sticks out toward you where one hull tick sweeps ( along sa..sb, feet z_lo .. head z_hi ): + = a lip past the face plane,
   0 = the face, -1 = nothing. traced in from 0.5 off; a hull further off than a lip never touches it ( collisionutils d1 > 0 && d2 > 0 ).
   axial faces only */
static float aal_stick( const c_vector& n, const float d, const float sa, const float sb, const float z_lo, const float z_hi, c_trace_filter_tb_world_props& filter )
{
	constexpr float k_out = 0.5f;
	const bool x_wall     = std::fabsf( n.m_x ) > 0.5f;
	const float half = 0.5f * std::fabsf( sb - sa ) + 16.f, mid = 0.5f * ( sa + sb ), c_n = d + 16.f + k_out;
	const c_vector mins( x_wall ? -16.f : -half, x_wall ? -half : -16.f, 0.f ), maxs( x_wall ? 16.f : half, x_wall ? half : 16.f, z_hi - z_lo );
	const c_vector at( n.m_x * c_n - n.m_y * mid, n.m_y * c_n + n.m_x * mid, z_lo );
	trace_t tr{ };
	ray_t ray( at, at - n * ( k_out + 0.1f ), mins, maxs );
	tb_trace( ray, mask_playersolid, &filter, &tr );
	if ( tr.m_start_solid )
		return k_out;
	if ( tr.m_fraction >= 1.f )
		return -1.f;
	if ( tr.m_plane.m_normal.m_x * n.m_x + tr.m_plane.m_normal.m_y * n.m_y > 0.9999f )
		return hull_plane_dist( tr, mins, maxs ) - d;
	return tr.m_end.m_x * n.m_x + tr.m_end.m_y * n.m_y - 16.f - d - 0.03125f;
}

/* ticks before the one that crosses target: z += dt ( vz + pin ), vz += 2 pin ( Start / FinishGravity halves ). -1 = never on this arc */
static int aal_ticks_to( float z, float vz, const float pin, const float ipt, const float target, const bool rising )
{
	for ( int k = 0; k < 256; ++k ) {
		const float v1 = vz + pin, z1 = z + ipt * v1;
		if ( rising ? z < target && z1 >= target : z > target && z1 <= target )
			return k;
		if ( rising ? v1 <= 0.f : v1 < 0.f && z1 < target )
			return -1;
		z  = z1;
		vz = v1 + pin;
	}
	return -1;
}

/* CategorizePosition grounds an END whose 2u hull trace crosses the floor face, 1/32 under the standing z ( route_engine sim_categorize ) */
constexpr float k_aal_ground_band = 2.f - 1.f / 32.f;

/* a tb rise past its crossing: ticks to the next bhop's, route_engine sim_tick order. stamina bleeds at each tick's start ( a takeoff cmd adds its
   jump cost after it ), the landing tick ( END within the ground band over the standing z, no snap ) adds land cost x its START fall speed, the
   takeoff tick bleeds, then the impulse scales ( virtual START vz: ducked impulse, standing impulse + pin ). exact on purpose: U coasts to arrive
   at the deadline, an early one = in at 119 u/s before the real crossing ( twin ). ponytail: sv_jump_impulse, a jumpFactor < 1 surface rises
   slower ( route_engine measures it at one spot only ). -1 = no landing / that jump never rises past target */
static int aal_ticks_bhop( float z, float vz, const float pin, const float ipt, const float floor_z, float stam, const bool duck, const bool takeoff,
                           const float target )
{
	static auto cv_jump  = g_interfaces.m_convar->find_var( "sv_jump_impulse" );
	static auto cv_rec   = g_interfaces.m_convar->find_var( "sv_staminarecoveryrate" );
	static auto cv_land  = g_interfaces.m_convar->find_var( "sv_staminalandcost" );
	static auto cv_jcost = g_interfaces.m_convar->find_var( "sv_staminajumpcost" );
	static auto cv_max   = g_interfaces.m_convar->find_var( "sv_staminamax" );
	const float stam_max = cv_max ? cv_max->get_float( ) : 80.f;
	const int land       = aal_ticks_to( z, vz, pin, ipt, floor_z + k_aal_ground_band, false );
	if ( land < 0 )
		return -1;
	const float bleed = ( cv_rec ? cv_rec->get_float( ) : 60.f ) * ipt;
	float fall        = 0.f;
	for ( int k = 0; k <= land; ++k ) {
		stam = std::max( 0.f, stam - bleed );
		if ( k == 0 && takeoff )
			stam = std::min( stam + ( cv_jcost ? cv_jcost->get_float( ) : 0.08f ) * vz, stam_max );
		fall           = -vz;
		const float v1 = vz + pin;
		z += ipt * v1;
		vz = v1 + pin;
	}
	stam             = std::max( 0.f, std::min( stam + ( cv_land ? cv_land->get_float( ) : 0.05f ) * fall, stam_max ) - bleed );
	const float jump = cv_jump ? cv_jump->get_float( ) : 301.993377f;
	const int rise   = aal_ticks_to( std::max( z, floor_z ), ( duck ? jump : jump + pin ) * std::clamp( 1.f - stam / 100.f, 0.f, 1.f ), pin, ipt, target, true );
	return rise < 0 ? -1 : land + 1 + rise;
}

/* edge surf's guide ( K park / D delta strafe / S stop-in-time ) against the point's wall: the hull parks h off the face, closing 0, before
   it crosses the point z. auto align / tb run after and own the last ulps. ponytail: arrival along the wall gated at 64u, never steered */
void n_movement::impl_t::air_align( c_user_cmd* cmd )
{
	constexpr float k_duck_crop = 0.34f;
	constexpr float k_wish_max  = 450.f;
	constexpr float k_along     = 64.f;
	constexpr float k_jump_rise = 66.f; /* 57 jump + 9 air duck lift: a floor further under the seam never rises past it */
	constexpr int k_move_keys   = in_forward | in_back | in_moveleft | in_moveright;
	static int s_cmd    = -1;
	static bool s_own   = false;
	static bool s_fall  = false; /* the owned cmd flew a fall plan */
	static c_vector s_pick{ };
	g_air_align_owns   = false;
	g_air_align_aa_off = false;
	g_air_align_ledge  = false;
	if ( !cmd )
		return;
	if ( s_cmd != cmd->m_command_number - 1 )
		s_own = false;
	s_cmd = cmd->m_command_number;

	c_base_entity* const local = g_ctx.m_local;
	const bool debug           = GET_VARIABLE( g_variables.m_debug_log, bool );
	const auto release         = [ & ]( const char* why ) {
		if ( s_own && debug )
			botox_dbg_log( "AIRAL: release why=%s cmd=%d", why, cmd->m_command_number );
		s_own = false;
	};
	if ( !GET_VARIABLE( g_variables.m_air_align, bool ) || g_ctx.m_eject_restoring || !local || !local->is_alive( ) || !local->get_collideable( ) ||
	     !g_interfaces.m_global_vars_base || !g_input.check_input( &GET_VARIABLE( g_variables.m_air_align_key, key_bind_t ) ) )
		return release( "key" );
	const std::string map = g_interfaces.m_engine_client->get_level_name_short( );
	/* before every yield: edge surf's own latch must keep finding it armed */
	for ( points_check_t& p : m_bounce_points_check )
		if ( p.point_type == 3 && p.active && p.aa_kind == air_align_ps && p.map == map && g_prediction.backup_data.m_origin.dist_to( p.pos ) <= p.radius &&
		     aal_wall( p, local, debug ) && p.aa_ledge ) {
			g_air_align_ledge = true;
			break;
		}
	if ( local->get_move_type( ) != e_move_types::move_type_walk )
		return release( "move" );
	if ( g_air_stuck_holding || edge_surf_latched( cmd ) || g_edgebug.m_found || g_edge_catch.active( ) || g_edge_skip.active( ) || g_wall_climb.active( cmd ) )
		return release( "trick" );

	const int frame = g_interfaces.m_prediction->m_commands_predicted - 1;
	g_prediction.restore_entity_to_predicted_frame( frame );
	const c_vector org = local->get_origin( ), vel = local->get_velocity( );
	const c_vector mins = local->get_collideable( )->get_obb_mins( ), maxs = local->get_collideable( )->get_obb_maxs( );
	/* prediction datamap field, offset 0 on a miss ( route_engine's guard ) */
	const float stam0   = local->get_stamina( ) >= 0.f && local->get_stamina( ) <= 200.f ? local->get_stamina( ) : 0.f;
	const float ipt     = g_interfaces.m_global_vars_base->m_interval_per_tick;
	const float pin     = g_prediction.get_engine_target_predict_z_velocity( );
	static auto sv_airaccelerate = g_interfaces.m_convar->find_var( "sv_airaccelerate" );
	const float k_acc            = ( sv_airaccelerate ? sv_airaccelerate->get_float( ) : 12.f ) * ipt * local->get_surface_friction( );
	if ( ipt <= 0.f || k_acc <= 0.f )
		return release( "rate" );
	const bool start_gnd = ( g_prediction.backup_data.m_flags & fl_onground ) != 0;
	if ( !start_gnd && std::fabsf( vel.m_z - pin ) < 1e-3f )
		return release( "pinned" );

	/* this cmd's vertical move off one idle sim: a takeoff ( ground + jump ) is an air move ( CheckJumpButton clears the ground before Friction /
	   WalkMove ), an air duck / unduck moves the feet 9 at once, a blocked unduck keeps FL_DUCKING. HandleDuckingSpeedCrop crops on IN_DUCK or
	   FL_DUCKING, a done air unduck does not. the deadline steps from the START that move came from: z1 = z0 + dt ( vz0 + pin ), vz1 = vz0 + 2 pin */
	const bool duck_cmd = ( cmd->m_buttons & in_duck ) != 0;
	c_user_cmd idle     = *cmd;
	idle.m_buttons &= ~k_move_keys;
	idle.m_forward_move = idle.m_side_move = 0.f;
	s_bounds_next       = g_prediction.m_real_max_speed;
	pred_simulate( &idle );
	const bool end_air  = !( local->get_flags( ) & fl_onground ) && local->get_move_type( ) == e_move_types::move_type_walk;
	const bool end_duck = ( local->get_flags( ) & fl_ducking ) != 0;
	const float tall    = local->get_collideable( )->get_obb_maxs( ).m_z;
	const float z_end = local->get_origin( ).m_z, vz_end = local->get_velocity( ).m_z;
	g_prediction.restore_entity_to_predicted_frame( frame );
	/* a fall plan's landing tick: the idle sim lands too. it is an air move ( START airborne ), the plan runs through it off the START itself ( the
	   ground zeroed the END vz ). any other landing / ground START hands off */
	const bool land_tick = !end_air && !start_gnd && s_own && s_fall;
	if ( !end_air && !land_tick )
		return release( start_gnd ? "ground" : "land" );
	const float vz0  = land_tick ? vel.m_z : vz_end - 2.f * pin;
	const float z0   = land_tick ? org.m_z : z_end - ipt * ( vz_end - pin );
	const float crop = duck_cmd || end_duck ? k_duck_crop : 1.f;
	/* a fall plan bets on a hop on the first ground tick: autohop ( bunny hop / sv_autobunnyhopping ) with your jump key down ( raw: bunny hop
	   strips it in the air ), not under delay hop / the jb key. a manual scroll hop is unknown ahead = no fall plan */
	const bool hop = ( m_user_buttons_raw & in_jump ) &&
	                 ( GET_VARIABLE( g_variables.m_bunny_hop, bool ) || g_convars.float_or( HASH_BT( "sv_autobunnyhopping" ), 0.f ) != 0.f ) &&
	                 !( GET_VARIABLE( g_variables.m_delay_hop, bool ) && g_input.check_input( &GET_VARIABLE( g_variables.m_delay_hop_key, key_bind_t ) ) ) &&
	                 !( GET_VARIABLE( g_variables.m_jump_bug, bool ) && g_input.check_input( &GET_VARIABLE( g_variables.m_jump_bug_key, key_bind_t ) ) );

	const float turn      = std::clamp( GET_VARIABLE( g_variables.m_air_align_delta_strafe, float ), 0.f, 15.f );
	const float reach     = std::clamp( GET_VARIABLE( g_variables.m_air_align_reach, float ), 0.f, 64.f );
	const float max_spd   = es_max_speed( local );
	const float authority = k_acc * max_spd * crop;
	const float brake     = std::max( authority, 1.f );
	const float vxy       = vel.length_2d( );

	struct pick_t {
		int i    = -1;
		int kc   = -1;
		char act = 'F';
		c_vector n{ };
		float d = 0.f, gap = 0.f, h = 0.f, v_old = 0.f, v = 0.f, along = 0.f, dist = FLT_MAX;
		bool fall = false;
		float lo = 0.f, at = -1.f;
	} best;
	const char* none = "radius";
	float none_dist  = FLT_MAX;
	for ( int i = 0; i < static_cast< int >( m_bounce_points_check.size( ) ); ++i ) {
		points_check_t& p = m_bounce_points_check[ i ];
		if ( p.point_type != 3 || !p.active || p.map != map )
			continue;
		const float dist = org.dist_to( p.pos );
		const bool mine  = s_own && p.pos == s_pick;
		if ( dist > p.radius || ( !mine && dist >= best.dist ) )
			continue;
		const auto miss = [ & ]( const char* why ) {
			if ( dist < none_dist ) {
				none_dist = dist;
				none      = why;
			}
		};
		if ( !aal_wall( p, local, debug ) ) {
			miss( "wall" );
			continue;
		}
		const float target = p.pos.m_z + p.aa_dz - ( p.aa_kind == air_align_tb_head ? tall : 0.f );
		const bool rise    = p.aa_kind == air_align_tb_rise;
		int kc             = aal_ticks_to( z0, vz0, pin, ipt, target, rise );
		const c_vector& n  = p.aa_n;
		const float gap    = es_hull_gap( org, n, p.aa_d, mins, maxs );
		/* tb rise on a small ledge: crossed 1-2 ticks after takeoff, no air tick to align in ( log 11-18 mirage 8u step, 10 bhops: rise every
		   arc ). past it = the next bhop's crossing off the floor: the one under you = the one at your landing along the wall ( a step / drop
		   there = no plan ). landing + takeoff are air moves, the plan runs through them */
		bool fall = false;
		if ( rise && kc < 0 && hop && gap > 0.f && org.m_z > target - k_jump_rise ) {
			c_trace_filter_tb_world_props filter( local );
			const auto floor_at = [ & ]( const c_vector& at, float& z ) {
				trace_t tr{ };
				ray_t ray( at, c_vector( at.m_x, at.m_y, target - k_jump_rise ), mins, maxs );
				tb_trace( ray, mask_playersolid, &filter, &tr );
				z = tr.m_end.m_z;
				return !tr.m_start_solid && tr.m_fraction < 1.f && tr.m_plane.m_normal.m_z >= 0.7f;
			};
			float f0 = 0.f, f1 = 0.f;
			if ( const int land = floor_at( org, f0 ) ? aal_ticks_to( z0, vz0, pin, ipt, f0 + k_aal_ground_band, false ) : -1; land >= 0 ) {
				const float vn = vel.m_x * n.m_x + vel.m_y * n.m_y, t = static_cast< float >( land + 1 ) * ipt;
				const c_vector at( org.m_x + ( vel.m_x - vn * n.m_x ) * t, org.m_y + ( vel.m_y - vn * n.m_y ) * t, org.m_z );
				if ( floor_at( at, f1 ) && std::fabsf( f1 - f0 ) < 0.5f ) {
					kc   = aal_ticks_bhop( z0, vz0, pin, ipt, f0, stam0, duck_cmd || end_duck, start_gnd, target );
					fall = kc >= 0;
				}
			}
		}
		if ( kc < 0 || gap <= 0.f ) {
			miss( kc < 0 ? rise ? "rise" : "arc" : "past" );
			continue;
		}
		const float t_arr = static_cast< float >( kc + 1 ) * ipt;
		const float along = ( org.m_x + vel.m_x * t_arr - p.pos.m_x ) * -n.m_y + ( org.m_y + vel.m_y * t_arr - p.pos.m_y ) * n.m_x;
		if ( std::fabsf( along ) > k_along ) {
			miss( "along" );
			continue;
		}
		/* square face: 32 ulps of the wall axis coord ( edge surf's standoff ); pj: inside the lip, a lip is >= k_creep_gap ( 5e-4 ) past the
		   face ( aa_pj_lip ), a hull further off falls past it. slanted face: outside DIST_EPSILON, a hull inside that shell wedges / flips the
		   server's end-pos stuck box ( aa_slanted ) */
		const bool axial  = !aa_slanted( n );
		const float u     = es_ulp( std::fabsf( n.m_x ) > 0.5f ? org.m_x : org.m_y );
		float h           = !axial ? 0.05f : p.aa_kind == air_align_pj ? std::max( 2.f * u, 0.5f * n_route::k_creep_gap ) : std::max( 32.f * u, 5e-4f );
		/* lips your last ticks fall past ( aal_stick ): a ps parks inside a lip narrower than the standoff ( log 11-20: 32 ulps fell past a
		   3.7e-3 .. 3.9e-3 lip 5 / 5, air stuck caught it at 3.6e-3 ), a pj parks outside a lip over its own ( log 11-20: the -156 lip 4u over
		   the -160 pj took 9 landings ). no lip at the target = the old standoff. ponytail: lips > 3 ticks over the crossing unchecked */
		float lo = 0.f, up = -1.f, at = -1.f;
		if ( axial && ( p.aa_kind == air_align_ps || p.aa_kind == air_align_pj ) ) {
			c_trace_filter_tb_world_props filter( local );
			const float s0 = -org.m_x * n.m_y + org.m_y * n.m_x, ds = ( -vel.m_x * n.m_y + vel.m_y * n.m_x ) * ipt;
			float z = z0, vz = vz0;
			for ( int k = 0; k <= kc; ++k ) {
				const float v1 = vz + pin, z1 = z + ipt * v1, sa = s0 + ds * static_cast< float >( k );
				if ( k >= kc - 3 )
					up = std::max( up, aal_stick( n, p.aa_d, sa, sa + ds, std::max( z1, target + 1.f ), z + tall, filter ) );
				if ( k == kc )
					at = aal_stick( n, p.aa_d, sa, sa + ds, target - 1.f, target + 1.f, filter );
				z  = z1;
				vz = v1 + pin;
			}
			if ( at > 2.f * u ) {
				lo = std::max( up, 0.f );
				h  = p.aa_kind == air_align_pj ? lo + h : std::min( h, at - 2.f * u );
				if ( h > at - 2.f * u || h < lo + u ) {
					miss( "lip" );
					continue;
				}
			}
		}
		/* inside the standoff = auto align / tb parked you flush: a K here pulls back out on every tick they leave the cmd alone. under an upper
		   lip it must pull out */
		if ( gap < 0.5f * h && lo <= 0.f )
			return release( "flush" );
		const float v_old = -( vel.m_x * n.m_x + vel.m_y * n.m_y );
		float v           = 0.f;
		char act          = edge_surf_guide( gap - h, gap, lo - h, v_old, vxy, kc, authority, turn, reach, mine, ipt, v );
		/* far walls: D's turn in past edge surf's d2 cap leaves velocity near the normal, the turn back has no authority and S presses the rest
		   off your speed ( twin: 250 -> 23 u/s ). capped there <= ~8 u/s lost; undershoot ( that cap can't get there by the crossing ) lifts it
		   to the triangle peak that does, at most half your speed ( twin: aligned 54.6 -> 63.4 %, worst 15 u/s more in 3 / 2880 ) */
		const float cap = std::max( std::min( 60.f, 0.24f * vxy ),
		                            std::min( 2.f * ( gap - h ) / ( static_cast< float >( std::max( kc - 3, 1 ) ) * ipt ), 0.5f * vxy ) );
		/* a run-in already over the cap: the turn in it wanted is no tick at all, the guide flies it again straight ( S brakes in time ). log 11-18
		   sill: D clamped to 0 every 2nd tick at closing 148 = no brake till H, too late = wall hit */
		if ( act == 'D' && v > 0.f && v_old + v > cap ) {
			if ( v_old < cap )
				v = cap - v_old;
			else
				act = edge_surf_guide( gap - h, gap, lo - h, v_old, vxy, kc, authority, 0.f, reach, mine, ipt, v );
		}
		/* edge surf's let_go: an approach we own never goes back closing faster than reaches the face by the crossing, nor while your keys
		   could reach the face from here ( one tick of full air accel + its brake ) */
		const float v_ok = std::max( 0.f, ( gap - h ) / ( static_cast< float >( kc + 1 ) * ipt ) );
		const float v_1  = std::max( v_old, 0.f ) + std::max( 30.f, std::min( k_acc * max_spd, 30.f + vxy ) );
		if ( act == 'F' && mine && ( v_old > v_ok || gap <= v_1 * ipt * ( 2.f + v_1 / ( 2.f * brake ) ) + 0.05f ) ) {
			act = 'H';
			v   = std::min( v_old, v_ok );
		}
		/* undershoot: no park fits before the crossing but the capped closing still gets there one tick after it = turn in anyway ( U ), the
		   guide / H take it once a plan or the face comes in reach */
		if ( const float v_u = ( gap - h ) / ( static_cast< float >( kc + 2 ) * ipt ); act == 'F' && turn > 0.f && vxy > 30.f && v_u <= cap && v_old < v_u ) {
			const float d = std::min( std::min( vxy * std::tanf( deg2rad( turn ) ), 30.f ), authority ) * std::sqrt( std::max( vxy * vxy - v_old * v_old, 0.f ) ) / vxy;
			if ( d >= 1.f ) {
				act = 'U';
				v   = std::min( d, v_u - v_old );
			}
		}
		if ( act == 'F' ) {
			miss( "plan" );
			continue;
		}
		best = { i, kc, act, n, p.aa_d, gap, h, v_old, v, along, mine ? -1.f : dist, fall, lo, at };
	}
	if ( best.i < 0 ) {
		static unsigned long long s_none_next = 0ull;
		if ( const unsigned long long now = GetTickCount64( ); debug && none_dist < FLT_MAX && now >= s_none_next ) {
			s_none_next = now + 1000ull;
			botox_dbg_log( "AIRAL: no plan why=%s dist=%.1f xy=%.1f vz=%.1f", none, none_dist, vxy, vel.m_z );
		}
		return release( "none" );
	}

	const c_vector n    = best.n;
	const float yaw_in  = rad2deg( std::atan2f( -n.m_y, -n.m_x ) );
	c_user_cmd base     = *cmd;
	base.m_buttons &= ~k_move_keys;
	/* AirAccelerate inverse on the wall normal ( edge surf press_v ): closing v_old -> v_t, VectorNormalize's FLT_EPSILON kept */
	const auto press = [ & ]( const float v_t ) {
		float w = v_t >= best.v_old ? std::max( ( v_t - best.v_old ) / k_acc, v_t ) : -std::max( ( best.v_old - v_t ) / k_acc, -v_t );
		w += w >= 0.f ? FLT_EPSILON : -FLT_EPSILON;
		w                = std::clamp( w / crop, -k_wish_max, k_wish_max );
		c_user_cmd c     = base;
		c.m_forward_move = std::fabsf( w );
		c.m_side_move    = 0.f;
		move_fix_to_yaw( w >= 0.f ? yaw_in : yaw_in + 180.f, c );
		return c;
	};
	/* delta strafe: wish square to the velocity moves the closing speed by dv, speed kept */
	const auto turn_cmd = [ & ]( const float dv ) {
		float px = -vel.m_y / vxy, py = vel.m_x / vxy, into = -( px * n.m_x + py * n.m_y );
		if ( ( into < 0.f ) != ( dv < 0.f ) ) {
			px   = -px;
			py   = -py;
			into = -into;
		}
		const float add  = std::min( std::fabsf( dv ) / std::max( std::fabsf( into ), 1e-3f ), 30.f );
		c_user_cmd c     = base;
		c.m_forward_move = std::min( std::max( add / k_acc, add ) / crop, k_wish_max );
		c.m_side_move    = 0.f;
		move_fix_to_yaw( rad2deg( std::atan2f( py, px ) ), c );
		return c;
	};
	c_user_cmd out = best.act == 'D' || best.act == 'U' ? turn_cmd( best.v ) : press( best.v );

	/* flown first: lands = hands off, closing past half the standoff = brake on the normal ( why=v ) */
	char why = '-';
	s_bounds_next = g_prediction.m_real_max_speed;
	c_user_cmd probe = out;
	pred_simulate( &probe );
	const points_check_t& pt = m_bounce_points_check[ best.i ];
	const bool gnd           = ( local->get_flags( ) & fl_onground ) != 0 || local->get_move_type( ) != e_move_types::move_type_walk;
	const float ge           = es_hull_gap( local->get_origin( ), n, best.d, mins, maxs );
	/* a pj point's landing on its lip top IS the pj: that cmd is sent as planned. a tb rise's fall plan lands as planned on the floor: its
	   press holds the closing into the takeoff ( twin: you pressing on that tick instead = 2146 / 2304 rows hit the face ) */
	const bool lip  = gnd && pt.aa_kind == air_align_pj && std::fabsf( local->get_origin( ).m_z - pt.pos.m_z ) < 0.05f;
	const bool keep = gnd && best.fall && ge > 0.f && local->get_move_type( ) == e_move_types::move_type_walk;
	g_prediction.restore_entity_to_predicted_frame( frame );
	if ( gnd && !lip && !keep )
		return release( "land" );
	if ( lip )
		why = 'j';
	else if ( ge < best.gap && ge < best.lo + 0.5f * ( best.h - best.lo ) ) {
		out = press( 0.f );
		why = 'v';
	} else if ( keep )
		why = 'l';

	cmd->m_forward_move = out.m_forward_move;
	cmd->m_side_move    = out.m_side_move;
	cmd->m_buttons      = out.m_buttons;
	g_air_align_owns    = true;
	/* a fall plan's park holds ~40 ticks to the takeoff, tb takes the last ulps there: auto align's contact seek would only shove it in */
	g_air_align_aa_off  = pt.aa_kind == air_align_pj || best.fall || best.gap > 2.f * best.h || std::fabsf( best.v_old ) > 1.f;
	s_own               = true;
	s_fall              = best.fall;
	s_pick              = pt.pos;
	s_aal_sent.cmd      = cmd->m_command_number;
	s_aal_sent.duck     = duck_cmd;
	s_aal_sent.crop     = crop;
	aal_wish( *cmd, s_aal_sent.wx, s_aal_sent.wy );

	static unsigned long long s_next = 0ull;
	static char s_last              = 0;
	if ( const unsigned long long now = GetTickCount64( ); debug && ( now >= s_next || best.act != s_last ) ) {
		s_next = now + 250ull;
		s_last = best.act;
		botox_dbg_log( "AIRAL: act=%c why=%c kind=%d gap=%.4e h=%.1e close=%.3f v=%.3f kc=%d fall=%d stam=%.1f along=%.1f xy=%.1f vz=%.1f duck=%d crop=%.2f dz=%.1f gnd=%d n=%.3f %.3f pos=%.1f %.1f %.1f lo=%.3e at=%.3e",
		               best.act, why, pt.aa_kind, best.gap, best.h, best.v_old, best.v, best.kc, best.fall ? 1 : 0, stam0, best.along, vxy, vel.m_z,
		               duck_cmd ? 1 : 0, crop, z0 - org.m_z, start_gnd ? 1 : 0, n.m_x, n.m_y, org.m_x, org.m_y, org.m_z, best.lo, best.at );
	}
}

/* end of create move: a later feature that only flipped the duck bit ( auto crouch, pixel surf, tb auto duck, eb ) changed this cmd's crop,
   the press is rescaled to the wish it was sized for. a rewritten move = no longer ours ( indicator ) */
void air_align_final_stamp( c_user_cmd* cmd )
{
	if ( !cmd || s_aal_sent.cmd != cmd->m_command_number ) {
		g_air_align_owns = g_air_align_aa_off = false;
		return;
	}
	float wx = 0.f, wy = 0.f;
	aal_wish( *cmd, wx, wy );
	if ( std::fabsf( wx - s_aal_sent.wx ) + std::fabsf( wy - s_aal_sent.wy ) > 1e-3f ) {
		g_air_align_owns = g_air_align_aa_off = false;
		return;
	}
	const bool duck = ( cmd->m_buttons & in_duck ) != 0;
	if ( duck == s_aal_sent.duck )
		return;
	float s       = s_aal_sent.crop / ( duck ? 0.34f : 1.f );
	const float m = std::max( std::fabsf( cmd->m_forward_move ), std::fabsf( cmd->m_side_move ) ) * s;
	if ( m > 450.f )
		s *= 450.f / m;
	cmd->m_forward_move *= s;
	cmd->m_side_move *= s;
	if ( GET_VARIABLE( g_variables.m_debug_log, bool ) )
		botox_dbg_log( "AIRAL: stamp duck %d -> %d scale=%.3f cmd=%d", s_aal_sent.duck ? 1 : 0, duck ? 1 : 0, s, cmd->m_command_number );
}
