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

/* edge surf's guide ( K park / D delta strafe / S stop-in-time ) against the point's wall: the hull parks h off the face, closing 0, before
   it crosses the point z. auto align / tb run after and own the last ulps. ponytail: arrival along the wall gated at 64u, never steered */
void n_movement::impl_t::air_align( c_user_cmd* cmd )
{
	constexpr float k_duck_crop = 0.34f;
	constexpr float k_wish_max  = 450.f;
	constexpr float k_along     = 64.f;
	constexpr int k_move_keys   = in_forward | in_back | in_moveleft | in_moveright;
	static int s_cmd    = -1;
	static bool s_own   = false;
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
	if ( !end_air )
		return release( start_gnd ? "ground" : "land" );
	const float vz0  = vz_end - 2.f * pin;
	const float z0   = z_end - ipt * ( vz_end - pin );
	const float crop = duck_cmd || end_duck ? k_duck_crop : 1.f;

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
		const int kc       = aal_ticks_to( z0, vz0, pin, ipt, target, p.aa_kind == air_align_tb_rise );
		const c_vector& n  = p.aa_n;
		const float gap    = es_hull_gap( org, n, p.aa_d, mins, maxs );
		if ( kc < 0 || gap <= 0.f ) {
			miss( kc < 0 ? p.aa_kind == air_align_tb_rise ? "rise" : "arc" : "past" );
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
		const float h     = !axial ? 0.05f : p.aa_kind == air_align_pj ? std::max( 2.f * u, 0.5f * n_route::k_creep_gap ) : std::max( 32.f * u, 5e-4f );
		/* inside the standoff = auto align / tb parked you flush: a K here pulls back out on every tick they leave the cmd alone */
		if ( gap < 0.5f * h )
			return release( "flush" );
		const float v_old = -( vel.m_x * n.m_x + vel.m_y * n.m_y );
		float v           = 0.f;
		char act          = edge_surf_guide( gap - h, gap, -h, v_old, vxy, kc, authority, turn, reach, mine, ipt, v );
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
				act = edge_surf_guide( gap - h, gap, -h, v_old, vxy, kc, authority, 0.f, reach, mine, ipt, v );
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
		best = { i, kc, act, n, p.aa_d, gap, h, v_old, v, along, mine ? -1.f : dist };
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
	/* a pj point's landing on its lip top IS the pj: that cmd is sent as planned */
	const bool lip = gnd && pt.aa_kind == air_align_pj && std::fabsf( local->get_origin( ).m_z - pt.pos.m_z ) < 0.05f;
	g_prediction.restore_entity_to_predicted_frame( frame );
	if ( gnd && !lip )
		return release( "land" );
	if ( lip )
		why = 'j';
	else if ( ge < best.gap && ge < 0.5f * best.h ) {
		out = press( 0.f );
		why = 'v';
	}

	cmd->m_forward_move = out.m_forward_move;
	cmd->m_side_move    = out.m_side_move;
	cmd->m_buttons      = out.m_buttons;
	g_air_align_owns    = true;
	g_air_align_aa_off  = pt.aa_kind == air_align_pj || best.gap > 2.f * best.h || std::fabsf( best.v_old ) > 1.f;
	s_own               = true;
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
		botox_dbg_log( "AIRAL: act=%c why=%c kind=%d gap=%.4e h=%.1e close=%.3f v=%.3f kc=%d along=%.1f xy=%.1f vz=%.1f duck=%d crop=%.2f dz=%.1f gnd=%d n=%.3f %.3f pos=%.1f %.1f %.1f",
		               best.act, why, pt.aa_kind, best.gap, best.h, best.v_old, best.v, best.kc, best.along, vxy, vel.m_z,
		               duck_cmd ? 1 : 0, crop, z0 - org.m_z, start_gnd ? 1 : 0, n.m_x, n.m_y, org.m_x, org.m_y, org.m_z );
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
