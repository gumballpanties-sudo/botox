/* edge surf, real servers: own feature, pure movement ( no server cmds ). the only real edge glide in CS:GO = the head under a brush
   BOTTOM edge, hull 1 ulp beside the face: the +z face is picked at t 0 every tick, z never moves ( lab tools/wc_engine/edge_probe:
   A feet on a top edge never chain, D every input line == the phase law, E the exact law below ).
   per tick: wall ( reach, else the lead arc ) -> brush bottom behind it -> hold band per stance -> no band = hands off -> else delta
   strafe / park to 1 ulp off the face in the band stance -> whole-ulp pin ladder.
   shares texture bug's sim plumbing only ( n_tb budget / restore / trace ). listen server = movement.cpp edge_surf ( fake hold ) */
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
bool edge_surf_local( );

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

/* true = edge surf owns this cmd ( texture bug + head bounce skip it ), pinned = this cmd is a proven head pin. called from texture
   bug's on_create_move: the sim budget and the sent-cmd resim live there */
bool edge_surf_online( c_user_cmd* cmd, bool& pinned )
{
	pinned = false;
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

	c_base_entity* const local = g_ctx.m_local;
	const bool online          = !edge_surf_local( );
	if ( online )
		g_edge_surf_active = false;
	const bool armed = online && cmd && local && local->is_alive( ) && local->get_collideable( ) && !g_ctx.m_eject_restoring &&
	                   GET_VARIABLE( g_variables.m_edge_surf, bool ) && g_input.check_input( &GET_VARIABLE( g_variables.m_edge_surf_key, key_bind_t ) );
	/* START must be the server's END, like texture bug */
	g_prediction.sync_arm( n_prediction::impl_t::k_sync_es, armed );
	if ( !armed || ( g_prediction.backup_data.m_flags & fl_onground ) || g_prediction.backup_data.m_move_type != e_move_types::move_type_walk ) {
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
		drop( );
		return false;
	}
	const c_user_cmd in = *cmd;

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
			pinned = true;
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
