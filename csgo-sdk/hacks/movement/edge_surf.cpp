#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"
#include "movement.h"
#include "../prediction/prediction.h"
#include "texturebug_common.h"
#include "edgebug.h"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>

using namespace n_tb;

extern void botox_dbg_log( const char* fmt, ... );
extern bool botox_is_ladder_trace( const trace_t& tr );
extern bool g_edge_surf_active;
extern bool g_air_stuck_owns_cmd;

float es_ulp( const float a )
{
	int ex = 0;
	std::frexp( a, &ex );
	return a != 0.f ? std::ldexp( 1.f, ex - 24 ) : 1e-7f;
}

float es_hull_gap( const c_vector& o, const c_vector& n, const float d, const c_vector& mins, const c_vector& maxs )
{
	return o.m_x * n.m_x + o.m_y * n.m_y + o.m_z * n.m_z - d + std::min( n.m_x * mins.m_x, n.m_x * maxs.m_x ) + std::min( n.m_y * mins.m_y, n.m_y * maxs.m_y ) +
	       std::min( n.m_z * mins.m_z, n.m_z * maxs.m_z );
}

static c_vector es_scan_dir( const int i )
{
	const float a = deg2rad( static_cast< float >( i < 4 ? i * 90 : ( i - 4 ) * 90 + 45 ) );
	return c_vector( std::cosf( a ), std::sinf( a ), 0.f );
}
/* 8 yaws + the way you fly: a narrow ledge ahead between two yaws */
static int es_scan_dirs( const c_vector& vel, c_vector ( &d )[ 9 ] )
{
	for ( int i = 0; i < 8; ++i )
		d[ i ] = es_scan_dir( i );
	const float v = vel.length_2d( );
	if ( v < 1.f )
		return 8;
	d[ 8 ] = c_vector( vel.m_x / v, vel.m_y / v, 0.f );
	return 9;
}

/* the surface name, never dispFlags: only disp code writes those, a closer brush hit later in the same trace keeps them ( cmodel.cpp:1670 ) */
static bool es_disp_hit( const trace_t& t )
{
	return t.surface.m_name && strstr( t.surface.m_name, "displacement" );
}
/* flat displacement ledges ( a flat top, an axial edge, a 2u+ drop beside ): terrain stays out ( slope / bumps fail the flat top, a soft rise
   fails the floor rule ) */
static bool es_flat_disp_on( )
{
	return GET_VARIABLE( g_variables.m_edge_surf_flat_disp, bool );
}

struct es_log_gate_t {
	unsigned long long next = 0ull;
	char last               = 0;
	bool due( const char act, const bool every = false )
	{
		const unsigned long long now = GetTickCount64( );
		if ( now < next && act == last && !every )
			return false;
		next = now + 250ull;
		last = act;
		return true;
	}
};

static int g_edge_surf_latch_cmd = -1;
int g_edge_surf_pin_cmd = -1;

bool edge_surf_latched( const c_user_cmd* cmd )
{
	return cmd && g_edge_surf_latch_cmd >= 0 && g_edge_surf_latch_cmd == cmd->m_command_number - 1;
}

/* air_align.cpp: its key near a ps point on a ledge top = that pin is edge surf's ( set in movement pre before its yields ) */
extern bool g_air_align_ledge;

bool edge_surf_armed( )
{
	return GET_VARIABLE( g_variables.m_edge_surf, bool ) && !g_ctx.m_eject_restoring && g_input.check_input( &GET_VARIABLE( g_variables.m_edge_surf_key, key_bind_t ) ) ||
	       g_air_align_ledge;
}

/* edgebug "edgesurf on eb": eb key arms only the takeover at a feet pin + the latched chain after it, never a fresh guide */
bool edge_surf_eb_armed( )
{
	return GET_VARIABLE( g_variables.m_edgebug_edge_surf, bool ) && GET_VARIABLE( g_variables.edge_bug, bool ) && GET_VARIABLE( g_variables.m_edgebug_style, int ) == 0 &&
	       !g_ctx.m_eject_restoring && g_input.check_input( &GET_VARIABLE( g_variables.edge_bug_key, key_bind_t ) );
}

void edge_surf_skip( const c_user_cmd* cmd, const char* why )
{
	if ( edge_surf_latched( cmd ) && GET_VARIABLE( g_variables.m_debug_log, bool ) )
		botox_dbg_log( "ESURF: feet skip why=%s cmd=%d", why, cmd->m_command_number );
}

/* later features may rewrite the cmd: one closing ulp over the top = standing on the ledge */
static struct {
	int cmd = -1, buttons = 0;
	float yaw = 0.f, fwd = 0.f, side = 0.f;
} s_es_sent;

struct es_bad_t {
	float top = 0.f, face = 0.f, f0 = 0.f;
	int m = 0;
	bool duck = false;
};
constexpr int k_es_bad = 16;
static es_bad_t s_es_bad[ k_es_bad ]{ };
static int s_es_bad_n = 0;
static struct {
	int cmd = -1, m = 0;
	float top = 0.f, face = 0.f, f0 = 0.f;
	bool duck = false;
} s_es_pin_sent;

static bool es_is_bad( const float top, const float face, const bool duck, const float f0, const int m )
{
	for ( int i = 0; i < std::min( s_es_bad_n, k_es_bad ); ++i ) {
		const es_bad_t& b = s_es_bad[ i ];
		if ( b.m == m && b.duck == duck && std::fabsf( b.top - top ) < 1e-3f && std::fabsf( b.face - face ) < 0.01f && std::fabsf( b.f0 - f0 ) < 2e-3f )
			return true;
	}
	return false;
}
static void es_mark_bad( const float top, const float face, const bool duck, const float f0, const int m )
{
	if ( m > 0 && !es_is_bad( top, face, duck, f0, m ) )
		s_es_bad[ s_es_bad_n++ % k_es_bad ] = { top, face, f0, m, duck };
}
/* ponytail: with ping START = our own prediction, a server-only refusal is not marked */
static void es_real_check( const c_user_cmd* cmd, const bool on, const float dz, const float vz, const bool debug )
{
	if ( s_es_pin_sent.cmd < 0 || !cmd || s_es_pin_sent.cmd != cmd->m_command_number - 1 )
		return;
	if ( !on )
		es_mark_bad( s_es_pin_sent.top, s_es_pin_sent.face, s_es_pin_sent.duck, s_es_pin_sent.f0, s_es_pin_sent.m );
	if ( debug )
		botox_dbg_log( "ESURF: feet real hit=%d m=%d f0=%.4f duck=%d top=%.3f face=%.3f dz=%.4f vz=%.2f bad=%d", on ? 1 : 0, s_es_pin_sent.m,
		               s_es_pin_sent.f0, s_es_pin_sent.duck ? 1 : 0, s_es_pin_sent.top, s_es_pin_sent.face, dz, vz, std::min( s_es_bad_n, k_es_bad ) );
	s_es_pin_sent.cmd = -1;
}

void edge_surf_final_stamp( c_user_cmd* cmd )
{
	constexpr int k_keys = in_forward | in_back | in_moveleft | in_moveright | in_duck;
	if ( !cmd || s_es_sent.cmd != cmd->m_command_number )
		return;
	const auto wish = []( const float fwd, const float side, const float yaw ) {
		const float a = deg2rad( yaw );
		return std::pair{ fwd * std::cosf( a ) + side * std::sinf( a ), fwd * std::sinf( a ) - side * std::cosf( a ) };
	};
	const auto [ wx0, wy0 ] = wish( s_es_sent.fwd, s_es_sent.side, s_es_sent.yaw );
	const auto [ wx1, wy1 ] = wish( cmd->m_forward_move, cmd->m_side_move, cmd->m_view_point.m_y );
	const bool keys         = ( cmd->m_buttons & k_keys ) != ( s_es_sent.buttons & k_keys );
	if ( !keys && std::fabsf( wx1 - wx0 ) + std::fabsf( wy1 - wy0 ) < 1e-4f )
		return;
	if ( GET_VARIABLE( g_variables.m_debug_log, bool ) )
		botox_dbg_log( "ESURF: feet stomp cmd=%d wish %.5f,%.5f -> %.5f,%.5f keys %x -> %x yaw %.3f -> %.3f", cmd->m_command_number, wx0, wy0, wx1, wy1,
		               s_es_sent.buttons & k_keys, cmd->m_buttons & k_keys, s_es_sent.yaw, cmd->m_view_point.m_y );
	cmd->m_forward_move = s_es_sent.fwd;
	cmd->m_side_move    = s_es_sent.side;
	/* same view = exact bits ( a pin press is ulp sized ) */
	if ( cmd->m_view_point.m_y != s_es_sent.yaw )
		move_fix_to_yaw( s_es_sent.yaw, *cmd );
	cmd->m_buttons = ( cmd->m_buttons & ~k_keys ) | ( s_es_sent.buttons & k_keys );
}

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
		if ( std::fabsf( tn.m_z ) > 0.05f || botox_is_ladder_trace( tr ) || es_disp_hit( tr ) )
			return false;
		c_base_entity* ent = tr.m_hit_entity;
		if ( ent && ent->get_index( ) != 0 && !( ent->get_collideable( ) && ent->get_collideable( )->get_solid( ) == 1 ) )
			return false;
		plane_d = n_tb::hull_plane_dist( tr, mins, maxs );
		gap     = es_hull_gap( org, tn, plane_d, mins, maxs );
		n = tn;
		return gap > 0.f;
	}
	return false;
}

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
	/* CM_ClipBoxToBrush leaves a brush DIST_EPSILON late */
	e.B  = e.B + 0.03125f;
	e.T  = top - 0.03125f;
	e.ok = e.B > z_lo + 0.1f && e.T > e.B;
	return e;
}

static bool edge_surf_feet( c_user_cmd* cmd, const c_user_cmd& in, c_base_entity* local, int frame, const c_vector& org, const c_vector& vel,
                            const c_vector& mins, const c_vector& maxs, bool ducked0, float ipt, float pin, float grav, float k_acc, bool debug,
                            bool eb_only, bool& pinned, bool& held );

/* true = edge surf owns this cmd ( tb + head bounce skip it ), pinned = proven pin, held = nothing after tb may rewrite it */
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

	if ( edge_surf_latched( cmd ) && GET_VARIABLE( g_variables.m_debug_log, bool ) )
		botox_dbg_log( "ESURF: feet enter cmd=%d", cmd->m_command_number );
	c_base_entity* const local = g_ctx.m_local;
	g_edge_surf_active         = false;
	const bool armed = cmd && local && local->is_alive( ) && local->get_collideable( ) && ( edge_surf_armed( ) || edge_surf_eb_armed( ) );
	/* START must be the server's END, like texture bug */
	g_prediction.sync_arm( n_prediction::impl_t::k_sync_es, armed );
	if ( !armed || ( g_prediction.backup_data.m_flags & fl_onground ) || g_prediction.backup_data.m_move_type != e_move_types::move_type_walk ) {
		edge_surf_skip( cmd, !armed ? "key" : ( g_prediction.backup_data.m_flags & fl_onground ) ? "ground" : "move" );
		if ( armed )
			es_real_check( cmd, false, 0.f, 0.f, GET_VARIABLE( g_variables.m_debug_log, bool ) );
		s_es_pin_sent.cmd = -1;
		g_edge_surf_latch_cmd = -1;
		s_es_sent.cmd         = -1;
		drop( );
		return false;
	}
	/* eb key alone: only a fresh pin START ( an edgebug just ended, its plan spent ) or our latched chain, else no restore / scan */
	const bool eb_only = !edge_surf_armed( );
	static int s_pin_cmd = -1;
	const bool pin_start = std::fabsf( g_prediction.backup_data.m_velocity.m_z - s_target_predict_z_vel ) < 1e-3f;
	const bool fresh     = pin_start && s_pin_cmd != cmd->m_command_number - 1;
	if ( pin_start )
		s_pin_cmd = cmd->m_command_number;
	if ( eb_only && !edge_surf_latched( cmd ) && ( !fresh || ( g_edgebug.m_found && !g_edgebug.replay_spent( ) ) ) ) {
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
	if ( vel.m_z + pin > 1e-3f || k_acc <= 0.f ) {
		edge_surf_skip( cmd, "rise" );
		drop( );
		return false;
	}
	const c_user_cmd in = *cmd;
	es_real_check( cmd, std::fabsf( org.m_z - ( s_es_pin_sent.top + k_eps ) ) < 0.01f && std::fabsf( vel.m_z - pin ) < 1e-3f,
	               org.m_z - ( s_es_pin_sent.top + k_eps ), vel.m_z, debug );

	/* a locked edgebug plan flies first: a tick we own makes it drop ( EB: drop why=taken ) */
	if ( g_edgebug.m_found && std::fabsf( vel.m_z - pin ) >= 1e-3f ) {
		edge_surf_skip( cmd, "eb" );
		static int s_eb_cmd = -1;
		if ( debug && s_eb_cmd != cmd->m_command_number - 1 )
			botox_dbg_log( "ESURF: eb first cmd=%d vz=%.1f", cmd->m_command_number, vel.m_z );
		s_eb_cmd = cmd->m_command_number;
		drop( );
		return false;
	}

	if ( g_air_stuck_owns_cmd )
		edge_surf_skip( cmd, "as" );
	if ( !g_air_stuck_owns_cmd && edge_surf_feet( cmd, in, local, frame, org, vel, mins, maxs, ducked0, ipt, pin, grav, k_acc, debug, eb_only, pinned, held ) )
		return true;
	if ( eb_only ) {
		drop( );
		return false;
	}

	const float reach = std::clamp( GET_VARIABLE( g_variables.m_edge_surf_reach, float ), 0.f, 32.f );
	c_trace_filter_tb_world_props filter( local );
	c_vector n{ };
	float gap = FLT_MAX, plane_d = 0.f;
	bool lead = false;
	for ( int i = 0; i < 8; ++i ) {
		trace_t tr{ };
		ray_t ray( org, org + es_scan_dir( i ) * ( reach + 0.05f ), mins, maxs );
		tb_trace( ray, mask_playersolid, &filter, &tr );
		if ( tr.m_start_solid || tr.m_fraction >= 1.f || std::fabsf( tr.m_plane.m_normal.m_z ) > 0.05f || botox_is_ladder_trace( tr ) || es_disp_hit( tr ) )
			continue;
		const c_vector& tn = tr.m_plane.m_normal;
		const float d      = n_tb::hull_plane_dist( tr, mins, maxs );
		const float g      = es_hull_gap( org, tn, d, mins, maxs );
		if ( g < gap ) {
			gap     = g;
			n       = tn;
			plane_d = d;
		}
	}
	if ( gap == FLT_MAX )
		lead = edge_lead( filter, org, vel, mins, maxs, ipt, grav, gap, n, plane_d );
	const float nxy = std::sqrt( n.m_x * n.m_x + n.m_y * n.m_y );
	/* your keys / view never end the guide: key up does */
	if ( gap == FLT_MAX || nxy < 0.5f ) {
		drop( );
		return false;
	}
	const float yaw_in = rad2deg( std::atan2f( -n.m_y, -n.m_x ) );

	/* ulp read ON CONTACT: a lead starts up to the lead range off the wall */
	const float ulp = es_ulp( std::max( std::fabsf( ( org.m_x - n.m_x * gap ) * n.m_x ), std::fabsf( ( org.m_y - n.m_y * gap ) * n.m_y ) ) );

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
	fo += ducked0 ? -9.f : 9.f;
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
		static es_log_gate_t s_gate;
		if ( !debug || !s_gate.due( act ) )
			return;
		botox_dbg_log( "ESURF: %s act=%c gap=%.6f edge=%d B=%.3f T=%.3f head=%.3f k=%d/%d h=%.4f/%.4f phi=%.4f/%.4f lo=%.3f/%.3f period=%.4f band=%d/%d ulp=%.1e pick=%d run=%d dx=%.3e sims=%d mg=%d vz=%.1f xy=%.1f",
		               lead ? "lead" : "touch", act, gap, edge.ok ? 1 : 0, edge.B, edge.T, head_now, kc, ko, hc, ho, phi_c, phi_o, lo_c, lo_o, period,
		               in_c ? 1 : 0, in_o ? 1 : 0, ulp, s_pick, s_run, dx_hit, sims, s_park_margin, vel.m_z, vel.length_2d( ) );
	};
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
	/* AirAccelerate inverse: press that ends the tick closing at v_plane */
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

	if ( s_park_cmd >= 0 && cmd->m_command_number == s_park_cmd + 1 ) {
		if ( s_park_from - gap < 0.5f * ( s_park_from - s_park_to ) || ( s_park_to < 0.5f * k_eps && gap > 0.9f * k_eps ) )
			s_park_margin = s_park_margin ? s_park_margin * 2 : 2;
		else
			s_park_margin = 0;
	}
	s_park_cmd = -1;

	if ( gap <= k_touch ) {
		const float g  = std::max( gap, 1e-7f );
		const float dz = std::fabsf( ( vel.m_z + pin ) * ipt ) + 1e-4f;
		const float hi = std::max( ( k_eps - g ) * dz / ( ( ( stance || ducked0 ) ? 54.f : 72.f ) + k_eps ), g * 1.5f );
		/* ponytail: no server shift comp ( texture bug's dn median ), add if ESURF shows holds lost with run=1 */
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

	const float delta = std::clamp( GET_VARIABLE( g_variables.m_edge_surf_delta_strafe, float ), 0.f, 5.f );
	const float vxy   = vel.length_2d( );
	/* closing speed must stay under one park tick's brake, else the wall eats it as a hard hit */
	if ( delta > 0.f && vxy > 30.f && gap > std::max( v_old * nxy, 0.f ) * ipt * 2.f + 1.f &&
	     v_old * nxy < k_acc * std::max( local->get_max_speed( ), 0.f ) * crop ) {
		const float side = ( vel.m_y * n.m_x - vel.m_x * n.m_y ) >= 0.f ? 1.f : -1.f;
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

	/* never under 1 ulp: a sub-ulp end = the plane or a hard hit */
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

	const c_user_cmd mine = with_duck( in, stance );
	commit( mine );
	log( );
	return mine.m_buttons != in.m_buttons || gap <= k_touch;
}

/* mv->m_flMaxSpeed = weapon speed ( kisak cs_player_shared.cpp:365 ) x stamina scale ( cs_gamemovement.cpp:283 ), never raw m_flMaxspeed */
float es_max_speed( c_base_entity* local )
{
	float speed = std::max( local->get_max_speed( ), 0.f );
	if ( auto* const weapon = g_interfaces.m_client_entity_list->get< c_base_entity >( local->get_active_weapon_handle( ) );
	     weapon && g_interfaces.m_weapon_system )
		if ( const c_weapon_data* const data = g_interfaces.m_weapon_system->get_weapon_data( weapon->get_item_definition_index( ) ) )
			if ( const float w = data->m_max_speed[ local->is_scoped( ) ? 1 : 0 ]; w > 1.f )
				speed = std::min( speed, w );
	if ( const float st = local->get_stamina( ); st > 0.f ) {
		const float s = std::clamp( 1.f - st / 100.f, 0.f, 1.f );
		speed *= s * s;
	}
	return speed;
}

/* cmodel.cpp IntersectRayWithBoxBrush float steps, volatile so the compiler never folds them */
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

static float es_gap_at( const es_face_t& f, const float x )
{
	volatile float d = f.sg < 0.f ? f.face - x : x - f.face;
	d                = d - f.hw;
	return d;
}
static float es_gap_to( const es_face_t& f, const c_vector& o )
{
	return es_gap_at( f, f.ax ? o.m_y : o.m_x );
}
static bool es_same_face( const es_face_t& a, const es_face_t& b )
{
	return a.ax == b.ax && a.sg == b.sg && std::fabsf( a.face - b.face ) < 0.01f && std::fabsf( a.top - b.top ) < 1e-3f;
}

/* visualize: edge_surf_feet writes it ( create move ), edge_surf_render draws it ( paint traverse ), both on the game thread */
static struct {
	float time = -1.f;
	int n      = 0;
	es_face_t cand[ 9 ]{ };
	float cand_s[ 9 ]{ };
	bool pick = false, arrive_ok = false, pinned = false, own = false;
	es_face_t f{ };
	float s = 0.f;
	c_vector arrive{ };
	char act = 'N';
} s_es_viz;

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

/* both bumps must round DOWN or the hull ends on the plane = grounded */
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
static int es_pick_gap( const es_face_t& f, const float frac0, const float e, const float u, const float ipt, const int from = 1, const bool duck = false )
{
	for ( int m = std::max( from, 1 ); m <= 512 && static_cast< float >( m ) * u < 0.015f; ++m )
		if ( !es_is_bad( f.top, f.face, duck, frac0, m ) && es_arrival_ok( f, m, frac0, e, u, ipt ) &&
		     es_arrival_ok( f, m, frac0 * 0.99999f, e, u, ipt ) && es_arrival_ok( f, m, frac0 * 1.00001f, e, u, ipt ) )
			return m;
	return 0;
}

/* sv_air_max_wishspeed 30: a straight press never raises its own wish speed past it */
static float es_stop_speed( const float r, const float a, const float dt )
{
	const float ar = std::fabsf( r ), k = std::floor( ( std::sqrt( 1.f + 8.f * ar / ( a * dt ) ) - 1.f ) * 0.5f );
	return ( r >= 0.f ? 1.f : -1.f ) * ( ar / ( ( k + 1.f ) * dt ) + k * a * 0.5f );
}
static float es_profile_step( const float r, const float w, const float ap, const float a, const float dt )
{
	const float up = std::min( w + a, std::max( w, 30.f ) ), dn = std::max( w - a, std::min( w, -30.f ) );
	return std::clamp( es_stop_speed( r, ap, dt ), dn, up );
}
static bool es_in_window( const float r, const float w, const float park, const float a, const float dt )
{
	return std::fabsf( r ) / dt <= park && std::fabsf( std::clamp( r / dt, -park, park ) - w ) <= a;
}
/* r_min = face plane: a plan that touches it puts the hull over the top = standing on the ledge */
static int es_profile_ticks( float r, float w, const float ap, const float a, const float park, const float dt, const int n, const float r_min )
{
	for ( int t = 0; t <= n; ++t ) {
		if ( es_in_window( r, w, park, a, dt ) )
			return t;
		w = es_profile_step( r, w, ap, a, dt );
		r -= w * dt;
		if ( r <= r_min )
			return -1;
	}
	return -1;
}

/* own align: a run-in parks h over the arrival gap, brakes there, creeps in at h / dt. K straight onto the gap left the run-in's closing ( up to
   25 u/s ) 1 ulp off the face for the next tick's brake ( 11-17 log: K 0.115 at 36.8 u/s -> next START on the plane = stood ). lab edge_real
   ER_SLIP sweep picked h = max( 32 ulps, 5e-4 ), kc >= 3, drift 4 ticks early: 0 stood at |slip| <= 3e-3 where the old K stood 1049 */
static float es_stage( const float err_m, const float u, const float v_old, const int kc, const float ipt )
{
	const float h     = std::max( 32.f * u, 5e-4f );
	const bool creep  = err_m <= 1.5f * h && std::fabsf( v_old ) * ipt <= 0.5f * h;
	return kc >= 3 && err_m > 0.5f * u && !creep ? h : 0.f;
}

/* above the top: err = distance left to the aim ( + = toward the face ), r_min = the face plane on that scale. K = one press lands K's window,
   D = delta strafe turn ( v = closing change ), S = straight stop-in-time profile, F = nothing fits. pure: the scan ranks faces with it */
struct es_guide_t {
	char act = 'F';
	float v  = 0.f;
};
static es_guide_t es_guide( const float err, const float gap, const float r_min, const float v_old, const float vxy, const int kc, const float authority,
                            const float turn, const float reach, const bool owned, const bool staged, const float ipt )
{
	constexpr float k_park_speed = 25.f;
	const float park    = std::min( k_park_speed, authority );
	const float v_park  = std::clamp( err / ipt, -park, park );
	const bool settles  = std::fabsf( v_park - v_old ) <= authority;
	if ( std::fabsf( err ) / ipt <= park && settles )
		return { 'K', v_park };
	if ( turn > 0.f && vxy > 30.f ) {
		const auto d_at = [ & ]( const float ww ) {
			return std::min( std::min( vxy * std::tanf( deg2rad( turn ) ), 30.f ), authority ) * std::sqrt( std::max( vxy * vxy - ww * ww, 0.f ) ) / vxy;
		};
		const float d = d_at( v_old );
		if ( d >= 1.f ) {
			const float ae = std::fabsf( err );
			const int k    = static_cast< int >( std::floor( ( std::sqrt( 1.f + 8.f * ae / ( d * ipt ) ) - 1.f ) * 0.5f ) );
			const float vs = ( err >= 0.f ? 1.f : -1.f ) * ( ae / ( static_cast< float >( k + 1 ) * ipt ) + static_cast< float >( k ) * d * 0.5f );
			const auto d_reaches = [ & ] {
				float rr = err, ww = v_old;
				for ( int t = 0; t < kc; ++t ) {
					if ( es_in_window( rr, ww, park, authority, ipt ) )
						return true;
					ww = std::clamp( es_stop_speed( rr, d, ipt ), ww - d, ww + d );
					rr -= ww * ipt;
					if ( rr <= r_min )
						return false;
				}
				return false;
			};
			constexpr float k_d2 = 60.f;
			const float d2_cap   = std::min( k_d2, 0.24f * vxy );
			const auto d2_target = [ & ]( const float rr, const float ww ) { return std::min( es_stop_speed( rr, 0.75f * authority, ipt ), std::max( ww, d2_cap ) ); };
			const auto d2_reaches = [ & ] {
				float rr = err, ww = v_old;
				for ( int t = 0; t <= kc - 3 && rr > r_min; ++t ) {
					if ( es_in_window( rr, ww, park, authority, ipt ) )
						return true;
					const float g = d2_target( rr, ww ), dd = d_at( ww );
					ww            = g > ww + 0.5f * dd ? std::min( ww + dd, g ) : es_profile_step( rr, ww, 0.75f * authority, authority, ipt );
					rr -= ww * ipt;
				}
				return false;
			};
			const bool d1 = static_cast< float >( k + 1 ) + std::fabsf( vs - v_old ) / d <= static_cast< float >( kc - 1 ) && d_reaches( );
			const bool d2 = !d1 && err > 0.f && kc >= 3 && d2_target( err, v_old ) > v_old + 0.5f * d && d2_reaches( );
			if ( d1 || d2 )
				return { 'D', d1 ? std::clamp( vs, v_old - d, v_old + d ) - v_old : std::min( d2_target( err, v_old ) - v_old, d ) };
		}
	}
	/* reach drift: staged = the slowest press that reaches the standoff 4 ticks before the crossing ( brake, creep, brake, correct ), else park speed */
	const float k_drift = static_cast< float >( staged ? std::max( kc - 4, 1 ) : kc );
	const float v_drift = staged ? std::clamp( err / ( k_drift * ipt ), -park, park ) : v_park;
	if ( gap <= reach && std::fabsf( err ) / ( k_drift * ipt ) <= park && std::fabsf( v_drift - v_old ) <= authority )
		return { 'K', v_drift };
	if ( ( gap <= reach || owned || ( v_old > 0.f && v_old * ipt * static_cast< float >( kc ) >= err ) ) && kc >= 3 &&
	     es_profile_ticks( err, v_old, 0.75f * authority, authority, park, ipt, kc - 3, r_min ) >= 0 )
		return { 'S', es_profile_step( err, v_old, 0.75f * authority, authority, ipt ) };
	return { };
}

static bool es_world_hit( const trace_t& t );
static float es_along( const es_face_t& c, const c_vector& o )
{
	return c.ax ? o.m_x : o.m_y;
}
/* highest top 0.5u inside face c under the hull's span ( hw - in ) at along coordinate s: a narrow ledge off the hull centre counts */
static bool es_top_at( i_trace_filter& filter, const es_face_t& c, const float s, const float z_hi, const float z_lo, const float in, float& top )
{
	c_vector q( 0.f, 0.f, z_hi );
	( c.ax ? q.m_y : q.m_x ) = c.face - c.sg * 0.5f;
	( c.ax ? q.m_x : q.m_y ) = s;
	const float w = std::max( c.hw - in, 0.f );
	const c_vector bmin( c.ax ? -w : -0.25f, c.ax ? -0.25f : -w, 0.f ), bmax( c.ax ? w : 0.25f, c.ax ? 0.25f : w, 0.f ), zero( 0.f, 0.f, 0.f );
	trace_t tt{ };
	ray_t strip( q, c_vector( q.m_x, q.m_y, z_lo ), bmin, bmax );
	tb_trace( strip, mask_playersolid, &filter, &tt );
	const auto flat_world = [ & ] { return tt.m_fraction < 1.f && tt.m_plane.m_normal.m_z >= 0.999f && es_world_hit( tt ); };
	/* a displacement in the strip = no ledge, unless it is a FLAT one and flat displacement ledges are on ( nuke's yellow parking curb = disp
	   132, top -408, no brush under it: lab edge_real_map 63 / 64 one tick pins ). top = the centre line's triangle plane, the strip agrees */
	if ( !tt.m_start_solid && tt.m_fraction < 1.f && es_disp_hit( tt ) ) {
		if ( !es_flat_disp_on( ) || tt.m_plane.m_normal.m_z < 0.99999f )
			return false;
		const float z_strip = tt.m_end.m_z;
		tt                  = trace_t{ };
		ray_t line( q, c_vector( q.m_x, q.m_y, z_lo ), zero, zero );
		tb_trace( line, mask_playersolid, &filter, &tt );
		if ( tt.m_start_solid || tt.m_fraction >= 1.f || !es_disp_hit( tt ) || tt.m_plane.m_normal.m_z < 0.99999f || std::fabsf( tt.m_end.m_z - z_strip ) > 0.05f )
			return false;
		top = tt.m_plane.m_distance;
		return true;
	}
	/* a wall part above z_hi ( start solid ), a prop / slope in the strip: the centre line decides */
	if ( tt.m_start_solid || ( tt.m_fraction < 1.f && !flat_world( ) ) ) {
		tt = trace_t{ };
		ray_t line( q, c_vector( q.m_x, q.m_y, z_lo ), zero, zero );
		tb_trace( line, mask_playersolid, &filter, &tt );
	}
	if ( tt.m_start_solid || !flat_world( ) )
		return false;
	top = tt.m_plane.m_distance;
	return true;
}
/* this top under the hull ( 0.5u in ) on the crossing tick, else the ledge ends before you get there. along speed = now ( K / S press on
   the wall axis only, a turn moves it a little: rechecked every tick ) */
static bool es_under_at_cross( i_trace_filter& filter, const es_face_t& c, const c_vector& o, const c_vector& v, const float off, const float pin, const float ipt )
{
	int k    = 0;
	float fr = 0.f, t = 0.f;
	if ( !es_crossing( c, o.m_z, v.m_z, off, pin, ipt, k, fr ) )
		return false;
	const float s = es_along( c, o ) + ( c.ax ? v.m_x : v.m_y ) * ( static_cast< float >( k ) + fr ) * ipt;
	return es_top_at( filter, c, s, o.m_z + 1.f, c.top - 1.f, 0.5f, t ) && std::fabsf( t - c.top ) < 1e-3f;
}
/* along coordinate of the hull on the crossing tick ( where it pins or lands ), s0 when it never crosses ( riding ) */
static float es_arrive_s( const es_face_t& c, const c_vector& o, const float s0, const c_vector& v, const float off, const float pin, const float ipt )
{
	int k    = 0;
	float fr = 0.f;
	return es_crossing( c, o.m_z, v.m_z, off, pin, ipt, k, fr ) ? es_along( c, o ) + ( c.ax ? v.m_x : v.m_y ) * ( static_cast< float >( k ) + fr ) * ipt : s0;
}
/* the hull beside the face at the arrival height: CategorizePosition's 2u ground probe ( gamemovement.cpp:4171 ) hits a floor = a landing, never a surf
   ( lab edge_real_map, mirage x -324 top -98: 1 of 64 left airborne ). a steep first hit tries the 4 quadrant boxes like the engine ( :4061 ).
   0.1 out, not ulps: a box that close reads the ledge top itself */
static bool es_floor_near( i_trace_filter& filter, const es_face_t& c, const float s )
{
	c_vector p( 0.f, 0.f, c.top + 0.03125f );
	( c.ax ? p.m_y : p.m_x ) = c.face + c.sg * ( c.hw + 0.1f );
	( c.ax ? p.m_x : p.m_y ) = s;
	const c_vector to( p.m_x, p.m_y, p.m_z - 2.f );
	const auto ground = [ & ]( const c_vector& lo, const c_vector& hi, bool& steep ) {
		trace_t t{ };
		ray_t probe( p, to, lo, hi );
		tb_trace( probe, mask_playersolid, &filter, &t );
		steep = !t.m_start_solid && t.m_fraction < 1.f && t.m_plane.m_normal.m_z < 0.7f;
		return t.m_start_solid || ( t.m_fraction < 1.f && t.m_plane.m_normal.m_z >= 0.7f );
	};
	const float w = c.hw;
	bool steep    = false, q = false;
	if ( ground( c_vector( -w, -w, 0.f ), c_vector( w, w, 0.f ), steep ) )
		return true;
	return steep && ( ground( c_vector( -w, -w, 0.f ), c_vector( 0.f, 0.f, 0.f ), q ) || ground( c_vector( 0.f, 0.f, 0.f ), c_vector( w, w, 0.f ), q ) ||
	                  ground( c_vector( -w, 0.f, 0.f ), c_vector( 0.f, w, 0.f ), q ) || ground( c_vector( 0.f, -w, 0.f ), c_vector( w, 0.f, 0.f ), q ) );
}
/* feet scan candidate ( face c from a sweep that ended at `end`, curtain `band` deep ): sets c.top, -1 = no ledge. the sweep met the face
   inside its band, so the top is never under it ( deeper = the floor beside a pillar ). rank: rides it 16 ( a running pin never loses its
   face ), the owned face 8 ( kept until its own act R ), crosses over the ledge 4, a top that holds 1; ties go to the nearest.
   at_end = the top came from the sweep's end, not beside you ( depends on the sweep ). edge_surf_eb_end ranks with it too */
static int es_rank_face( i_trace_filter& filter, es_face_t& c, const c_vector& o, const c_vector& end, const c_vector& v, const float off, const float pin,
                         const float ipt, const float band, const bool pinned, const es_face_t* own, bool& under, bool& at_end )
{
	at_end = false;
	if ( !es_top_at( filter, c, es_along( c, o ), o.m_z + 1.f, o.m_z - band - 0.1f, 0.5f, c.top ) ) {
		at_end = true;
		if ( !es_top_at( filter, c, es_along( c, end ), o.m_z + 1.f, o.m_z - band - 0.1f, 0.5f, c.top ) )
			return -1;
	}
	if ( o.m_z <= c.top || es_floor_near( filter, c, es_arrive_s( c, o, es_along( c, at_end ? end : o ), v, off, pin, ipt ) ) )
		return -1;
	under             = es_under_at_cross( filter, c, o, v, off, pin, ipt );
	const bool riding = pinned && std::fabsf( o.m_z - ( c.top + 0.03125f ) ) < 0.01f;
	return ( riding ? 16 : 0 ) + ( own && es_same_face( c, *own ) ? 8 : 0 ) + ( under ? 4 : 0 ) +
	       ( es_seen_box( c.top, off ) / ( -2.f * pin * ipt ) >= 1.05e-4f ? 1 : 0 );
}

struct es_cand_t {
	es_face_t c{ };
	c_vector n{ };
	float g    = FLT_MAX;
	float s    = 0.f; /* along coordinate its top was read at */
	int r      = -1;
	bool under = false;
};
/* a face ranked from beside you ( rank does not depend on the sweep ): a ledge stops the next sweep meeting it, a rejected one is swept over */
struct es_done_t {
	es_face_t f{ };
	int r = -1;
};
/* one scan direction, curtain dz deep. a hit that is no ledge ( slope, displacement, prop, a face with no top ) but sits under the feet:
   the curtain ends 0.5 above it and sweeps again, 3 times at most ( a curb on uneven / displacement ground: lab scan_probe sweep, mirage
   range 96 = 52 more ledges of 638 ). done / prop_d = feet's dedupe + prop log, null in the eb end twin */
static bool es_sweep( i_trace_filter& filter, const c_vector& o, const c_vector& dir, const float range, const c_vector& mins, const c_vector& maxs, float dz,
                      const c_vector& v, const float off, const float pin, const float ipt, const bool pinned, const es_face_t* own, es_done_t* done, int* n_done,
                      float* prop_d, es_cand_t& out )
{
	constexpr int k_done = 16;
	for ( int pass = 0; pass < 4; ++pass ) {
		trace_t tr{ };
		ray_t ray( o, o + dir * range, c_vector( mins.m_x, mins.m_y, -dz ), c_vector( maxs.m_x, maxs.m_y, 0.f ) );
		tb_trace( ray, mask_playersolid, &filter, &tr );
		if ( tr.m_start_solid || tr.m_fraction >= 1.f )
			return false;
		const c_vector& tn = tr.m_plane.m_normal;
		const bool wall    = std::fabsf( tn.m_z ) <= 1e-4f;
		if ( prop_d && wall && !es_world_hit( tr ) && !es_disp_hit( tr ) )
			*prop_d = std::min( *prop_d, tr.m_fraction * range );
		es_face_t c{ };
		c.ax = std::fabsf( tn.m_x ) > 0.9999f ? 0 : std::fabsf( tn.m_y ) > 0.9999f ? 1 : -1;
		/* a flat displacement's edge: the box meets its triangle extent ( AxisPlanesXYZ ), the top test below decides if it is flat */
		const bool disp_edge = es_disp_hit( tr ) && es_flat_disp_on( ) && ( !tr.m_hit_entity || tr.m_hit_entity->get_index( ) == 0 );
		if ( wall && c.ax >= 0 && ( es_world_hit( tr ) || disp_edge ) && !botox_is_ladder_trace( tr ) ) {
			c.sg   = ( c.ax ? tn.m_y : tn.m_x ) > 0.f ? 1.f : -1.f;
			c.face = tr.m_plane.m_distance * c.sg;
			if ( disp_edge ) {
				/* disp hull hits carry a raw vertex coord as dist ( sign lost, nuke 1636 ): rebuilt from the end, snapped to 1/32 when that
				   close ( the end's float step is ~1 ulp off, the arrival plan counts ulps ) */
				c.face           = n_tb::hull_plane_dist( tr, c_vector( mins.m_x, mins.m_y, -dz ), c_vector( maxs.m_x, maxs.m_y, 0.f ) ) * c.sg;
				const float snap = std::round( c.face * 32.f ) / 32.f;
				if ( std::fabsf( snap - c.face ) < 2e-3f )
					c.face = snap;
			}
			c.hw          = c.ax ? maxs.m_y : maxs.m_x;
			const float g        = es_gap_to( c, o );
			const es_done_t* dup = done ? std::find_if( done, done + *n_done, [ & ]( const es_done_t& d ) {
				return d.f.ax == c.ax && d.f.sg == c.sg && std::fabsf( d.f.face - c.face ) < 0.01f;
			} )
			                            : nullptr;
			const bool seen = dup && dup != done + *n_done;
			if ( g <= 0.f || ( seen && dup->r >= 0 ) )
				return false;
			if ( !seen ) {
				bool under = false, at_end = false;
				const int r = es_rank_face( filter, c, o, tr.m_end, v, off, pin, ipt, dz, pinned, own, under, at_end );
				if ( done && !at_end && *n_done < k_done )
					done[ ( *n_done )++ ] = { c, r };
				if ( r >= 0 ) {
					out = { c, tn, g, es_along( c, at_end ? tr.m_end : o ), r, under };
					return true;
				}
			}
		}
		if ( pass == 3 )
			return false;
		/* 0.25u past the stop = inside what the sweep met; start solid = it rises past the feet ( a wall ). each pass lifts the bottom >= 0.5 */
		const c_vector p = tr.m_end + dir * 0.25f;
		trace_t up{ };
		ray_t probe( p, c_vector( p.m_x, p.m_y, p.m_z - dz ), c_vector( mins.m_x, mins.m_y, 0.f ), c_vector( maxs.m_x, maxs.m_y, 0.f ) );
		tb_trace( probe, mask_playersolid, &filter, &up );
		const float next = up.m_fraction * dz - 0.5f;
		if ( up.m_start_solid || up.m_fraction >= 1.f || next < 0.5f )
			return false;
		dz = next;
	}
	return false;
}

/* ponytail: along-edge keys dropped while holding; axial world brushes only */
static bool edge_surf_feet( c_user_cmd* cmd, const c_user_cmd& in, c_base_entity* local, const int frame, const c_vector& org, const c_vector& vel,
                            const c_vector& mins, const c_vector& maxs, const bool ducked0, const float ipt, const float pin, const float grav,
                            const float k_acc, const bool debug, const bool eb_only, bool& pinned, bool& held )
{
	constexpr float k_eps          = 0.03125f;
	constexpr float k_min_fraction = 1.05e-4f; /* gamemovement MINIMUM_MOVE_FRACTION 1e-4 + margin */
	constexpr float k_duck_crop    = 0.34f;
	constexpr float k_wish_max     = 450.f;
	constexpr float k_park_speed   = 25.f;
	constexpr int k_move_keys      = in_forward | in_back | in_moveleft | in_moveright;

	static bool s_on = false, s_own = false, s_guard = false;
	static int s_cmd = -1, s_run = 0;
	static es_face_t s_face{ }, s_own_face{ }, s_guard_face{ };
	/* act Q stood you up: your held duck stays off until the feet pass that top ( a re-duck = feet +9 + 2.0 duck speed a toggle ) */
	static int s_up_cmd   = -1;
	static float s_up_top = 0.f;
	if ( s_up_cmd == cmd->m_command_number - 1 && org.m_z > s_up_top ) {
		s_up_cmd = cmd->m_command_number;
		cmd->m_buttons &= ~in_duck;
	} else
		s_up_cmd = -1;
	static struct {
		es_face_t f{ };
		bool duck = false;
		float f0 = -1.f, u = 0.f;
		int ok = 0, cur = 0, first = 0, tries = 0;
	} s_plan;
	if ( s_cmd != cmd->m_command_number - 1 ) {
		s_on    = false;
		s_own   = false;
		s_guard = false;
		s_run   = 0;
		if ( !s_plan.ok )
			s_plan.f0 = -1.f;
	}
	s_cmd = cmd->m_command_number;
	struct latch_out_t {
		const bool& own;
		const int cmd;
		~latch_out_t( ) { g_edge_surf_latch_cmd = own ? cmd : -1; }
	} const latch_out{ s_own, cmd->m_command_number };

	const float period = grav * ipt * ipt;
	const float off    = maxs.m_z * 0.5f;
	const float t_h    = ipt * static_cast< float >( n_tick::ticks( 48 ) );
	float drop_z       = std::clamp( -( vel.m_z + pin ) * t_h + 0.5f * grav * t_h * t_h, 4.f, 256.f );
	/* top line keeps full depth: something under the hull above a latched top must not end the latch */
	const float drop_full = drop_z;
	const float reach = std::clamp( GET_VARIABLE( g_variables.m_edge_surf_reach, float ), 0.f, 32.f );
	const float turn  = std::clamp( GET_VARIABLE( g_variables.m_edge_surf_delta_strafe, float ), 0.f, 15.f );
	const float range = turn > 0.f ? std::max( std::clamp( GET_VARIABLE( g_variables.m_edge_surf_range, float ), 8.f, 512.f ), reach ) : reach + 0.05f;
	c_trace_filter_tb_world_props filter( local );
	/* curtain must end above what lies under the hull: a floor inside it = every sweep starts solid = blind */
	{
		trace_t dn{ };
		ray_t down( org, c_vector( org.m_x, org.m_y, org.m_z - drop_z ), c_vector( mins.m_x, mins.m_y, 0.f ), c_vector( maxs.m_x, maxs.m_y, 0.f ) );
		tb_trace( down, mask_playersolid, &filter, &dn );
		if ( !dn.m_start_solid && dn.m_fraction < 1.f )
			drop_z = std::max( dn.m_fraction * drop_z - 0.5f, 0.5f );
	}
	const bool pinned0 = std::fabsf( vel.m_z - pin ) < 1e-3f;
	if ( pinned0 )
		drop_z = std::min( drop_z, 2.f );
	es_face_t f{ };
	c_vector n{ };
	float gap = FLT_MAX, frac = 0.f, best_tp = FLT_MAX;
	int rank = -1;
	bool f_under = false;
	/* faces already ranked from beside you: another sweep meeting them gives the same answer */
	es_done_t done[ 16 ]{ };
	float f_s = 0.f;
	int n_done = 0;
	const bool viz = GET_VARIABLE( g_variables.m_edge_surf_visualize, bool );
	if ( viz )
		s_es_viz.n = 0;
	/* a prop / brush entity face ( nuke curbs = static props ): vphysics pins only a fall that ENDS within 1/32 over its top, not plannable */
	float prop_d = FLT_MAX;
	/* delta strafe pick: the guide's own verdict per face ( arrival gap + K / D / S plan ), a face it can fly outranks one it cannot */
	const float authority0 = k_acc * es_max_speed( local ) * ( ducked0 ? k_duck_crop : 1.f );
	int n_fly = 0, n_cand = 0;
	const auto flyable = [ & ]( const es_face_t& c, const c_vector& cn ) {
		const float uc = es_ulp( c.ax ? org.m_y : org.m_x );
		int kq          = 0;
		float fq        = 0.f;
		if ( uc < 1.9073486e-6f || !es_crossing( c, org.m_z, vel.m_z, off, pin, ipt, kq, fq ) || kq <= 0 )
			return false;
		const float fr = es_seen_box( c.top, off ) / period;
		const int mq   = es_pick_gap( c, fq, ( fr >= k_min_fraction ? fr : std::min( fq * 0.5f, 2e-3f ) ) * 0.5f, uc, ipt, 1, ducked0 );
		if ( !mq )
			return false;
		const float gq = es_gap_to( c, org ), vq = -( vel.m_x * cn.m_x + vel.m_y * cn.m_y ), e = gq - static_cast< float >( mq ) * uc;
		const float sq = es_stage( e, uc, vq, kq, ipt );
		return es_guide( e - sq, gq, -( static_cast< float >( mq ) * uc + sq ), vq, vel.length_2d( ), kq, authority0, turn, reach,
		                 s_own && es_same_face( c, s_own_face ), sq > 0.f, ipt ).act != 'F';
	};
	c_vector dirs[ 9 ];
	const int n_dirs = es_scan_dirs( vel, dirs );
	for ( int i = 0; i < n_dirs; ++i ) {
		es_cand_t h{ };
		if ( !es_sweep( filter, org, dirs[ i ], range, mins, maxs, drop_z, vel, off, pin, ipt, pinned0, s_own ? &s_own_face : nullptr, done, &n_done, &prop_d, h ) )
			continue;
		const es_face_t& c = h.c;
		const c_vector& tn = h.n;
		const float g      = h.g;
		const int r        = h.r;
		const bool under   = h.under;
		if ( viz && s_es_viz.n < 9 ) {
			s_es_viz.cand[ s_es_viz.n ]   = c;
			s_es_viz.cand_s[ s_es_viz.n++ ] = h.s;
		}
		/* flyable sits inside `under` ( 4 ): fly + hold 7 > fly 6 > hold 5 > under 4. a running pin ( 16 ) keeps its face */
		const int rf = r + ( ( r & 4 ) && !( r & 16 ) && flyable( c, tn ) ? 2 : 0 );
		++n_cand;
		n_fly += rf != r;
		/* danger: the hull reaches this face's plane before it crosses its top = on the ledge unless this face is braked first. the earliest
		   one goes first, flyable or not ( critic 11-17: an inside corner picked the farther flyable face, the near one's closing ran on ) */
		float t_p = FLT_MAX;
		if ( ( r & 4 ) && !( r & 16 ) ) {
			const float vc = -( vel.m_x * tn.m_x + vel.m_y * tn.m_y );
			int kq         = 0;
			float fq       = 0.f;
			if ( vc > 0.f && es_crossing( c, org.m_z, vel.m_z, off, pin, ipt, kq, fq ) && g / vc < static_cast< float >( kq + 1 ) * ipt )
				t_p = g / vc;
		}
		bool take = rf > rank || ( rf == rank && g < gap );
		if ( rf < 16 && rank < 16 && ( t_p < FLT_MAX || best_tp < FLT_MAX ) )
			take = t_p < best_tp;
		if ( take ) {
			rank    = rf;
			best_tp = t_p;
			gap     = g;
			f       = c;
			f_s     = h.s;
			f_under = under;
			frac    = es_seen_box( c.top, off ) / period;
			n       = tn;
		}
	}
	/* latch end: z = dropped beside it, p = hull past the face over the top with no top line ( landed ), e = ledge ended. a hull past the owned
	   face over its top is kept ( gap < 0 ): the guide pulls it out before the crossing ( 11-17 log: K left it on the plane 29u over the top,
	   kc 14, the sweeps cannot see a face from past it = hands off = stood ) */
	char ended = 0;
	if ( s_own && ( gap == FLT_MAX || !es_same_face( f, s_own_face ) ) ) {
		es_face_t c   = s_own_face;
		c.hw          = c.ax ? maxs.m_y : maxs.m_x;
		const float g = es_gap_to( c, org );
		float t       = 0.f;
		const bool keep = g > -2.f * c.hw && org.m_z > c.top && es_top_at( filter, c, es_along( c, org ), org.m_z + 1.f, org.m_z - drop_full - 1.f, 0.5f, t ) &&
		                  std::fabsf( t - c.top ) < 1e-3f && !es_floor_near( filter, c, es_arrive_s( c, org, es_along( c, org ), vel, off, pin, ipt ) );
		if ( keep && ( gap == FLT_MAX || g <= 0.f ) ) {
			f       = c;
			f_s     = es_along( c, org );
			gap     = g;
			n       = c_vector( c.ax ? 0.f : c.sg, c.ax ? c.sg : 0.f, 0.f );
			frac    = es_seen_box( c.top, off ) / period;
			f_under = es_under_at_cross( filter, c, org, vel, off, pin, ipt );
		} else if ( gap == FLT_MAX ) {
			ended = org.m_z <= c.top ? 'z' : g <= 0.f ? 'p' : 'e';
			s_own = false;
		}
	}
	const bool hold_ok = frac >= k_min_fraction;
	if ( viz ) {
		s_es_viz.time      = g_interfaces.m_global_vars_base->m_real_time;
		s_es_viz.pick      = gap < FLT_MAX;
		s_es_viz.f         = f;
		s_es_viz.s         = f_s;
		int k_a            = 0;
		float fr_a         = 0.f;
		s_es_viz.arrive_ok = s_es_viz.pick && es_crossing( f, org.m_z, vel.m_z, off, pin, ipt, k_a, fr_a );
		if ( s_es_viz.arrive_ok ) {
			const float s_a = es_along( f, org ) + ( f.ax ? vel.m_x : vel.m_y ) * ( static_cast< float >( k_a ) + fr_a ) * ipt;
			s_es_viz.arrive = f.ax ? c_vector( s_a, f.face, f.top ) : c_vector( f.face, s_a, f.top );
		}
	}

	char act = 'N', why = '-';
	/* the tick's verdict on every way out, also the returns that skip log( ) */
	struct viz_out_t {
		const bool on;
		const char& act;
		const bool &pinned, &own;
		~viz_out_t( )
		{
			if ( on ) {
				s_es_viz.act    = act;
				s_es_viz.pinned = pinned;
				s_es_viz.own    = own;
			}
		}
	} const viz_out{ viz, act, pinned, s_own };
	int m = 0, kc = 0, sims = 0;
	float f0 = 0.f, stage = 0.f;
	const float v_old = gap < FLT_MAX ? -( vel.m_x * n.m_x + vel.m_y * n.m_y ) : 0.f;
	const auto log = [ & ]( ) {
		static es_log_gate_t s_gate;
		if ( !debug || !s_gate.due( act, s_own ) )
			return;
		botox_dbg_log( "ESURF: feet act=%c why=%c top=%.5f face=%.3f gap=%.3e m=%d kc=%d f0=%.4f frac=%.2e hold=%d close=%.4e run=%d own=%d sims=%d vz=%.1f xy=%.1f duck=%d pos=%.1f %.1f stage=%.1e fly=%d/%d",
		               act, why, f.top, f.face, gap, m, kc, f0, frac, hold_ok ? 1 : 0, v_old, s_run, s_own ? 1 : 0, sims, vel.m_z, vel.length_2d( ),
		               ducked0 ? 1 : 0, org.m_x, org.m_y, stage, n_fly, n_cand );
	};
	if ( ended ) {
		act = 'L';
		why = ended;
		log( );
	}
	if ( gap == FLT_MAX ) {
		if ( debug && pinned0 && g_edgebug.m_found )
			botox_dbg_log( "ESURF: feet eb in lift=0 why=scan drop=%.2f range=%.2f z=%.3f xy=%.1f", drop_z, range, org.m_z, vel.length_2d( ) );
		static es_log_gate_t s_prop_gate;
		if ( debug && prop_d < FLT_MAX && s_prop_gate.due( 'p' ) )
			botox_dbg_log( "ESURF: feet prop face only ( static prop / entity, not a brush ledge ) dist=%.1f pos=%.1f %.1f %.1f", prop_d, org.m_x, org.m_y, org.m_z );
		s_on    = false;
		s_guard = false;
		s_run   = 0;
		return false;
	}
	if ( s_on && !es_same_face( f, s_face ) ) {
		s_on  = false;
		s_run = 0;
	}
	const float u = es_ulp( f.ax ? org.m_y : org.m_x );

	const float yaw_in = rad2deg( std::atan2f( -n.m_y, -n.m_x ) );
	const float crop   = ducked0 ? k_duck_crop : 1.f;
	const auto hold_stance = [ & ]( c_user_cmd c ) {
		c.m_buttons = ducked0 ? ( c.m_buttons | in_duck ) : ( c.m_buttons & ~in_duck );
		c.m_buttons &= ~k_move_keys;
		return c;
	};
	/* AirAccelerate inverse. VectorNormalize divides by len + FLT_EPSILON; cr = this cmd's move scale ( a duck press crops 0.34 even standing ) */
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
	/* delta strafe: wish square to the velocity moves the closing speed by dv, speed kept */
	const auto turn_cmd = [ & ]( const float dv ) {
		const float vxy = vel.length_2d( );
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
		return c;
	};

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
	/* over the top one closing ulp lands you ON the ledge: an owned / in-reach approach, or one act Y handed back, never goes back to
	   your keys there unchecked */
	const auto let_go = [ & ]( const char miss ) {
		act = miss;
		if ( !( s_own || gap <= reach || ( s_guard && es_same_face( f, s_guard_face ) ) ) || org.m_z <= f.top ) {
			log( );
			return false;
		}
		int kh  = 0;
		float fh = 0.f;
		es_crossing( f, org.m_z, vel.m_z, off, pin, ipt, kh, fh );
		const float v_ok = std::max( 0.f, ( gap - u ) / ( static_cast< float >( kh + 1 ) * ipt ) );
		const float max_spd = es_max_speed( local );
		const float v_1     = std::max( v_old, 0.f ) + std::max( 30.f, std::min( k_acc * max_spd, 30.f + vel.length_2d( ) ) );
		const float brake   = std::max( k_acc * max_spd * crop, 1.f );
		/* no plan and your keys cannot reach the face this tick: yours again, owned or not ( the latch goes, the guard stays ) */
		if ( v_old <= v_ok && gap > v_1 * ipt * ( 2.f + v_1 / ( 2.f * brake ) ) + 0.05f ) {
			if ( s_own ) {
				s_guard      = true;
				s_guard_face = f;
			}
			s_own = false;
			act   = 'Y';
			why   = miss;
			log( );
			return false;
		}
		why = miss;
		commit( press( std::min( v_old, v_ok ) ) );
		return own( 'H' );
	};

	const bool own0 = s_own && es_same_face( f, s_own_face );
	/* |coord| < 16: the gap grid ( ulp of 16 ) is coarser than the origin's */
	if ( u < 1.9073486e-6f ) {
		s_on  = false;
		s_own = false;
		s_run = 0;
		if ( debug && pinned0 && g_edgebug.m_found )
			botox_dbg_log( "ESURF: feet eb in lift=0 why=coord top=%.5f face=%.3f gap=%.3e", f.top, f.face, gap );
		return false;
	}

	const bool riding = std::fabsf( org.m_z - ( f.top + k_eps ) ) < 0.01f && pinned0;
	if ( !riding && pinned0 ) {
		if ( s_own ) {
			act = 'O';
			log( );
		}
		if ( debug && g_edgebug.m_found )
			botox_dbg_log( "ESURF: feet eb in lift=0 why=plane top=%.5f face=%.3f gap=%.3e dz=%.4f", f.top, f.face, gap, org.m_z - ( f.top + k_eps ) );
		s_on  = false;
		s_own = false;
		s_run = 0;
		return false;
	}
	/* eb key alone: another face = a fresh eb end or nothing; a latch left set = air stuck / wall climb yield to nothing */
	if ( eb_only && !own0 ) {
		s_own = false;
		if ( !riding )
			return false;
	}
	float zc = org.m_z, vzc = vel.m_z;
	const bool cross = !riding && es_crossing( f, org.m_z, vel.m_z, off, pin, ipt, kc, f0, &zc, &vzc );
	/* plain top: stay low in the arrival fraction, a big step leaves few arrival gaps */
	const float base = hold_ok ? frac : std::min( f0 * 0.5f, 2e-3f );

	if ( !riding && ( !cross || kc > 0 ) ) {
		s_on  = false;
		s_run = 0;
		if ( !cross )
			return let_go( 'B' );
		/* ducked, no hold here = one pin: stand up early = pin, air duck lift, second pin ( or the standing hold, its centre crosses the
		   power of two first ). unduck in air = feet -9, no move crop, never starts the 0.4 s re-duck timer ( only FinishDuck, cs_gamemovement.cpp:949 ).
		   standing crosses sooner: a short ledge the ducked crossing overshoots may still be under it */
		if ( ducked0 && !hold_ok && kc >= 4 && sims_left( ) > 8 && org.m_z - f.top > 9.5f ) {
			const int tick0 = local->get_tick_base( );
			const float last_duck = local->get_last_duck_time( ), duck_speed = local->get_duck_speed( );
			/* ponytail: the stand up brake aims at 1 ulp + standoff, the standing plan's gap ( mq ) is known only after this sim; aim at mq if a log shows
			   stand ups that then pull out hard ( lab: 0 stood under +-3e-3 slip either way ) */
			const float a_o = k_acc * es_max_speed( local ), park_o = std::min( k_park_speed, a_o ), e_o = gap - u - es_stage( gap - u, u, v_old, kc, ipt );
			const float v_t = es_in_window( e_o, v_old, park_o, a_o, ipt ) ? std::clamp( e_o / ipt, -park_o, park_o ) : es_profile_step( e_o, v_old, 0.75f * a_o, a_o, ipt );
			c_user_cmd q = press_cr( v_t, 1.f );
			q.m_buttons &= ~in_duck;
			tb_restore( frame );
			pred_simulate( &q );
			++sims;
			const auto fl     = local->get_flags( );
			const c_vector eo = local->get_origin( ), ev = local->get_velocity( );
			const float oq = local->get_collideable( )->get_obb_maxs( ).m_z * 0.5f, fd = es_seen_box( f.top, oq ) / period;
			const float wq = -( ev.m_x * n.m_x + ev.m_y * n.m_y ), gq = es_gap_to( f, eo );
			int kq = 0, mq = 0;
			float fq     = 0.f;
			const bool stood_up = !( fl & fl_ducking ) && !( fl & fl_onground ) && local->get_move_type( ) == e_move_types::move_type_walk;
			if ( stood_up && gq > 0.f && es_crossing( f, eo.m_z, ev.m_z, oq, pin, ipt, kq, fq ) && kq >= 3 )
				mq = es_pick_gap( f, fq, ( fd >= k_min_fraction ? fd : std::min( fq * 0.5f, 2e-3f ) ) * 0.5f, u, ipt, 1, false );
			/* the lift after the standing pin ( tick kq + 2 ) needs the duck back: 0.4 s since your last FinishDuck, duck speed left for this
			   release + that press ( 2.0 each, < 1.5 strips it, cs_gamemovement.cpp:153-188 ). a top the standing hull holds needs no lift */
			const bool lift_ok = duck_speed >= 5.5f && static_cast< float >( tick0 + kq + 2 ) * ipt >= last_duck + g_convars.float_or( HASH_BT( "sv_timebetweenducks" ), 0.4f );
			bool ok = mq && ( lift_ok || fd >= k_min_fraction ) &&
			          es_profile_ticks( gq - static_cast< float >( mq ) * u, wq, 0.75f * a_o, a_o, park_o, ipt, kq - 3, -static_cast< float >( mq ) * u ) >= 0 &&
			          es_under_at_cross( filter, f, eo, ev, oq, pin, ipt );
			/* a proven ducked pin is never traded for a paper plan: the standing arrival is flown from its crossing START first */
			if ( ok ) {
				float zq = eo.m_z, vzq = ev.m_z, fk = 0.f;
				int kk = 0;
				es_crossing( f, eo.m_z, ev.m_z, oq, pin, ipt, kk, fk, &zq, &vzq );
				c_vector po = eo, pv = ev;
				( f.ax ? po.m_y : po.m_x ) = ( f.face + f.sg * f.hw ) + f.sg * ( static_cast< float >( mq ) * u );
				po.m_z                     = zq;
				( f.ax ? pv.m_y : pv.m_x ) = 0.f;
				pv.m_z                     = vzq;
				local->set_abs_origin( po );
				local->get_origin( )   = po;
				local->get_velocity( ) = pv;
				if ( int& eflags = local->get_eflags( ); reinterpret_cast< std::uintptr_t >( &eflags ) != reinterpret_cast< std::uintptr_t >( local ) )
					eflags |= 1 << 12;
				const float base_q = fd >= k_min_fraction ? fd : std::min( fq * 0.5f, 2e-3f );
				c_user_cmd c       = press_v( 0.f, ( static_cast< float >( mq ) - 0.5f ) * u * ( 1.f + 0.5f * base_q ) / ipt, 1.f );
				c.m_buttons &= ~in_duck;
				pred_simulate( &c );
				++sims;
				const c_vector ao = local->get_origin( ), av = local->get_velocity( );
				ok = local->get_move_type( ) == e_move_types::move_type_walk && !( local->get_flags( ) & fl_onground ) && std::fabsf( av.m_z - pin ) < 1e-3f &&
				     std::fabsf( ao.m_z - ( f.top + k_eps ) ) < 0.01f;
			}
			tb_restore( frame );
			static es_log_gate_t s_stance_gate;
			if ( debug && s_stance_gate.due( ok ? 'y' : 'n' ) )
				botox_dbg_log( "ESURF: feet stand up ok=%d hold=%d kc=%d -> %d m=%d gap=%.3e close=%.3f -> gap=%.3e close=%.3f dz=%.3f", ok ? 1 : 0,
				               fd >= k_min_fraction ? 1 : 0, kc, kq, mq, gap, v_old, gq, wq, eo.m_z - org.m_z );
			if ( ok ) {
				commit( q );
				s_up_cmd = cmd->m_command_number;
				s_up_top = f.top;
				return own( 'Q' );
			}
		}
		if ( !f_under )
			return let_go( 'R' );
		const auto plan_gap = [ & ] { return s_plan.ok ? s_plan.ok : s_plan.cur ? s_plan.cur : s_plan.first; };
		if ( !( s_plan.f0 >= 0.f && es_same_face( s_plan.f, f ) && s_plan.duck == ducked0 && s_plan.u == u && std::fabsf( s_plan.f0 - f0 ) < 5e-5f ) ||
		     es_is_bad( f.top, f.face, ducked0, f0, plan_gap( ) ) ) {
			s_plan       = { };
			s_plan.f     = f;
			s_plan.duck  = ducked0;
			s_plan.u     = u;
			s_plan.f0    = f0;
			s_plan.first = s_plan.cur = es_pick_gap( f, f0, base * 0.5f, u, ipt, 1, ducked0 );
		}
		for ( int n = 0; n < 3 && !s_plan.ok && s_plan.cur && s_plan.tries < 24 && sims_left( ) > 8; ++n ) {
			c_vector o = org, v = vel;
			( f.ax ? o.m_y : o.m_x ) = ( f.face + f.sg * f.hw ) + f.sg * ( static_cast< float >( s_plan.cur ) * u );
			o.m_z                    = zc;
			( f.ax ? v.m_y : v.m_x ) = 0.f;
			v.m_z                    = vzc;
			tb_restore( frame );
			local->set_abs_origin( o );
			local->get_origin( )   = o;
			/* SetupMove reads GetAbsVelocity: a velocity write needs EFL_DIRTY_ABSVELOCITY */
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
				               pins ? 1 : 0, s_plan.tries, s_plan.first, f0, kc, es_gap_to( f, eo ), eo.m_z - ( f.top + k_eps ), ev.m_z, gnd ? 1 : 0,
				               std::min( s_es_bad_n, k_es_bad ) );
			if ( pins ) {
				s_plan.ok = s_plan.cur;
				break;
			}
			++s_plan.tries;
			s_plan.cur = es_pick_gap( f, f0, base * 0.5f, u, ipt, s_plan.cur + 1, ducked0 );
		}
		m = plan_gap( );
		if ( !m )
			return let_go( 'G' );
		const float err_m     = gap - static_cast< float >( m ) * u;
		stage                 = es_stage( err_m, u, v_old, kc, ipt );
		const float authority = k_acc * es_max_speed( local ) * crop;
		const es_guide_t gd   = es_guide( err_m - stage, gap, -( static_cast< float >( m ) * u + stage ), v_old, vel.length_2d( ), kc, authority, turn, reach, s_own, stage > 0.f, ipt );
		if ( gd.act == 'F' )
			return let_go( 'F' );
		c_user_cmd c = gd.act == 'D' ? turn_cmd( gd.v ) : press( gd.v );
		/* flown in the engine first: closing in to past the arrival gap ( or the face plane ), or on the ground = brake here instead ( why=v ).
		   a hull already past the plane over the top pulls out: never blocked */
		if ( sims_left( ) > 0 ) {
			tb_restore( frame );
			pred_simulate( &c );
			++sims;
			const bool gnd = ( local->get_flags( ) & fl_onground ) != 0 || local->get_move_type( ) != e_move_types::move_type_walk;
			const float ge = es_gap_to( f, local->get_origin( ) );
			const c_vector ev = local->get_velocity( );
			tb_restore( frame );
			/* staged: past half the standoff faster than the creep = the closing staging exists to take off before the gap */
			const bool over = stage > 0.f && ge < static_cast< float >( m ) * u + 0.5f * stage && -( ev.m_x * n.m_x + ev.m_y * n.m_y ) > stage / ipt;
			if ( gnd || over || ( ge < gap && ( ge <= 0.f || ge < ( static_cast< float >( m ) - 0.5f ) * u ) ) ) {
				c   = press( 0.f );
				why = 'v';
			}
		}
		commit( c );
		return own( gd.act );
	}
	/* crossing this tick past the ledge's end: a refusal there says nothing about the gap */
	if ( !riding && !f_under )
		return let_go( 'R' );

	/* ponytail: lift goes out with the ducked brake; standing brake ticks first would take steeper edgebug exits */
	if ( riding && !s_on && !s_own && !( hold_ok && std::fabsf( v_old ) < 0.01f ) ) {
		bool lift = false;
		if ( !ducked0 && v_old <= 1e-3f && sims_left( ) > 0 ) {
			const float a  = k_acc * es_max_speed( local ) * k_duck_crop;
			c_user_cmd hop = press_cr( 0.f, k_duck_crop );
			hop.m_buttons |= in_duck;
			tb_restore( frame );
			pred_simulate( &hop );
			++sims;
			const auto fl     = local->get_flags( );
			const c_vector eo = local->get_origin( ), ev = local->get_velocity( );
			const float w = -( ev.m_x * n.m_x + ev.m_y * n.m_y ), ge = es_gap_to( f, eo );
			const float oq = local->get_collideable( )->get_obb_maxs( ).m_z * 0.5f, fd = es_seen_box( f.top, oq ) / period;
			int kq   = 0;
			float fq = 0.f;
			lift     = ( fl & fl_ducking ) && !( fl & fl_onground ) && eo.m_z > org.m_z + 8.f && w <= 1e-3f && a > 0.f &&
			       es_crossing( f, eo.m_z, ev.m_z, oq, pin, ipt, kq, fq ) && kq >= 3 &&
			       es_profile_ticks( ge - u, w, 0.75f * a, a, std::min( k_park_speed, a ), ipt, kq - 3, -u ) >= 0 &&
			       es_pick_gap( f, fq, ( fd >= k_min_fraction ? fd : std::min( fq * 0.5f, 2e-3f ) ) * 0.5f, u, ipt, 1, true ) != 0;
			tb_restore( frame );
			if ( debug )
				botox_dbg_log( "ESURF: feet eb in lift=%d top=%.5f face=%.3f gap=%.3e close=%.3f -> gap=%.3e close=%.3f dz=%.3f kc=%d xy=%.1f", lift ? 1 : 0, f.top,
				               f.face, gap, v_old, ge, w, eo.m_z - org.m_z, kq, vel.length_2d( ) );
			if ( lift ) {
				commit( hop );
				why = 'b';
				return own( 'U' );
			}
		} else if ( debug )
			botox_dbg_log( "ESURF: feet eb in lift=0 why=%s top=%.5f face=%.3f gap=%.3e close=%.3f", ducked0 ? "duck" : v_old > 1e-3f ? "closing" : "sims", f.top,
			               f.face, gap, v_old );
		if ( !hold_ok )
			return false;
	}

	if ( riding && !hold_ok ) {
		c_user_cmd c = press( 0.f );
		act          = 'E';
		if ( !ducked0 && sims_left( ) > 0 ) {
			/* a duck press crops this cmd's move 0.34: brake via press_cr or the fall back drifts onto the ledge */
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
				/* ducked by us now: the stand up hold ends */
				s_up_cmd = -1;
			}
		}
		/* closing 0 at 1 ulp may freeze ( TryPlayerMove stuck check, gamemovement.cpp:3352 ): step out an ulp until the sim falls, why=f */
		if ( act == 'E' ) {
			c = press( -u / ipt );
			for ( int j = 0; j <= 3 && sims_left( ) > 0; ++j ) {
				c = press( -static_cast< float >( j ) * u / ipt );
				tb_restore( frame );
				pred_simulate( &c );
				++sims;
				const bool gnd = ( local->get_flags( ) & fl_onground ) != 0;
				if ( local->get_move_type( ) == e_move_types::move_type_walk && !gnd && local->get_origin( ).m_z < org.m_z - 1e-3f )
					break;
				if ( !gnd )
					why = 'f';
			}
			tb_restore( frame );
		}
		commit( c );
		s_on  = false;
		s_run = 0;
		return own( act );
	}

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
		e.gap   = es_gap_to( f, o );
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
		/* refused at the crossing: next approach parks on another gap */
		if ( !riding && static_cast< float >( m ) * u < 0.015f )
			es_mark_bad( f.top, f.face, ducked0, f0, m );
		s_on            = false;
		s_run           = 0;
		/* the head search under us reads the START state, not a failed sim's end */
		tb_restore( frame );
		return let_go( miss );
	}
	commit( best );
	pinned = true;
	held   = true;
	s_on   = true;
	g_edge_surf_pin_cmd = cmd->m_command_number;
	s_face = f;
	++s_run;
	if ( !riding )
		s_es_pin_sent = { cmd->m_command_number, m, f.top, f.face, f0, ducked0 };
	return own( act );
}

static bool es_world_hit( const trace_t& t )
{
	if ( t.m_hit_entity && t.m_hit_entity->get_index( ) != 0 )
		return false;
	return t.m_hitbox == 0 && !es_disp_hit( t );
}

/* edgebug.cpp search sim END: true = feet pin at an edge top ( vz = -g dt / 2, airborne ); lift = edge_surf_feet's takeover scan sees it
   standing on an axial world ledge, not closing, exit under 0.046 s of ducked brake ( lab ER_EB sweep: 0 false lifts, 64 + 128, weapon 150..250 ) */
bool edge_surf_eb_end( bool& lift )
{
	lift                       = false;
	c_base_entity* const local = g_ctx.m_local;
	auto* const col            = local ? local->get_collideable( ) : nullptr;
	if ( !col || !g_interfaces.m_global_vars_base )
		return false;
	const c_vector o = local->get_origin( ), v = local->get_velocity( );
	const float pin  = g_prediction.get_engine_target_predict_z_velocity( );
	if ( ( local->get_flags( ) & fl_onground ) || std::fabsf( v.m_z - pin ) >= 1e-3f )
		return false;
	if ( local->get_flags( ) & fl_ducking )
		return true;
	const c_vector lo = col->get_obb_mins( ), hi = col->get_obb_maxs( );
	const float dt    = g_interfaces.m_global_vars_base->m_interval_per_tick;
	const float t_h   = dt * static_cast< float >( n_tick::ticks( 48 ) );
	const float depth = std::clamp( -2.f * pin * t_h - pin / dt * t_h * t_h, 4.f, 256.f );
	const float reach = std::clamp( GET_VARIABLE( g_variables.m_edge_surf_reach, float ), 0.f, 32.f );
	float scan        = reach + 0.05f;
	if ( GET_VARIABLE( g_variables.m_edge_surf_delta_strafe, float ) > 0.f )
		scan = std::max( std::clamp( GET_VARIABLE( g_variables.m_edge_surf_range, float ), 8.f, 512.f ), reach );
	c_trace_filter_tb_world_props filter( local );
	float curtain = depth;
	{
		trace_t fl{ };
		ray_t floor_ray( o, c_vector( o.m_x, o.m_y, o.m_z - depth ), c_vector( lo.m_x, lo.m_y, 0.f ), c_vector( hi.m_x, hi.m_y, 0.f ) );
		tb_trace( floor_ray, mask_playersolid, &filter, &fl );
		if ( !fl.m_start_solid && fl.m_fraction < 1.f )
			curtain = std::max( fl.m_fraction * depth - 0.5f, 0.5f );
	}
	curtain = std::min( curtain, 2.f );
	es_face_t best{ };
	c_vector bn{ };
	float best_gap = FLT_MAX;
	int best_rank  = -1;
	c_vector dirs[ 9 ];
	const int n_dirs = es_scan_dirs( v, dirs );
	for ( int i = 0; i < n_dirs; ++i ) {
		/* feet's own sweep + pick ( strip top, rides first ): the lift verdict must be about the face the takeover sees */
		es_cand_t h{ };
		if ( !es_sweep( filter, o, dirs[ i ], scan, lo, hi, curtain, v, hi.m_z * 0.5f, pin, dt, true, nullptr, nullptr, nullptr, nullptr, h ) || h.r < best_rank ||
		     ( h.r == best_rank && h.g >= best_gap ) )
			continue;
		best      = h.c;
		best_gap  = h.g;
		best_rank = h.r;
		bn        = h.n;
	}
	if ( best_gap == FLT_MAX || std::fabsf( o.m_z - ( best.top + 0.03125f ) ) >= 0.01f || es_ulp( best.ax ? o.m_y : o.m_x ) < 1.9e-6f )
		return true;
	static auto sv_airaccelerate = g_interfaces.m_convar->find_var( "sv_airaccelerate" );
	const float away  = v.m_x * bn.m_x + v.m_y * bn.m_y;
	const float brake = ( sv_airaccelerate ? sv_airaccelerate->get_float( ) : 12.f ) * local->get_surface_friction( ) * es_max_speed( local ) * 0.34f;
	lift              = away >= -1e-3f && away <= 0.046f * brake;
	return true;
}

/* visualize: every ledge the scan found ( dim ), the pick ( your color while edge surf owns the cmd, green on a pin, grey once it let go ),
   its wall under it, the arrival point on the crossing tick, the act. the hull's span along the edge, not the ledge's real length */
void edge_surf_render( )
{
	/* no edge surf checkbox gate: edgebug's "edgesurf on eb" runs it too, a picture only exists while it scans */
	if ( !GET_VARIABLE( g_variables.m_edge_surf_visualize, bool ) || !g_ctx.m_local || !g_ctx.m_local->is_alive( ) || !g_interfaces.m_global_vars_base ||
	     !g_interfaces.m_engine_client->is_in_game( ) )
		return;
	const float age = g_interfaces.m_global_vars_base->m_real_time - s_es_viz.time;
	if ( s_es_viz.time < 0.f || age < 0.f || age > 0.15f )
		return;
	const c_color& col = GET_VARIABLE( g_variables.m_edge_surf_visualize_color, c_color );
	const auto line    = [ ]( const c_vector& a, const c_vector& b, const unsigned int c, const float w ) {
		c_vector_2d sa, sb;
		if ( g_render.world_to_screen( a, sa ) && g_render.world_to_screen( b, sb ) )
			g_render.m_draw_data.emplace_back( e_draw_type::draw_type_line, std::make_any< line_draw_object_t >( line_draw_object_t{ sa, sb, c, w } ) );
	};
	const auto edge = [ & ]( const es_face_t& c, const float s, const unsigned int color, const float w ) {
		const auto at = [ & ]( const float a, const float z ) { return c.ax ? c_vector( a, c.face, z ) : c_vector( c.face, a, z ); };
		line( at( s - c.hw, c.top ), at( s + c.hw, c.top ), color, w );
		line( at( s - c.hw, c.top ), at( s - c.hw, c.top - 8.f ), color, 1.f );
		line( at( s + c.hw, c.top ), at( s + c.hw, c.top - 8.f ), color, 1.f );
	};
	for ( int i = 0; i < s_es_viz.n; ++i )
		if ( !s_es_viz.pick || !es_same_face( s_es_viz.cand[ i ], s_es_viz.f ) )
			edge( s_es_viz.cand[ i ], s_es_viz.cand_s[ i ], col.get_u32( 0.35f ), 1.f );
	if ( !s_es_viz.pick )
		return;
	const unsigned int pick = s_es_viz.pinned ? c_color( 90, 255, 120, 255 ).get_u32( ) : s_es_viz.own ? col.get_u32( ) : c_color( 160, 160, 160, 200 ).get_u32( );
	edge( s_es_viz.f, s_es_viz.s, pick, 2.f );
	if ( c_vector_2d sp; s_es_viz.arrive_ok && g_render.world_to_screen( s_es_viz.arrive, sp ) )
		g_render.m_draw_data.emplace_back( e_draw_type::draw_type_filled_circle, std::make_any< filled_circle_draw_object_t >( filled_circle_draw_object_t( sp, 3.f, pick, 12 ) ) );
	auto* const font = g_render.m_fonts[ e_font_names::font_name_verdana_11 ];
	const es_face_t& f = s_es_viz.f;
	const c_vector mid = f.ax ? c_vector( s_es_viz.s, f.face, f.top + 6.f ) : c_vector( f.face, s_es_viz.s, f.top + 6.f );
	c_vector_2d sm;
	if ( !font || !g_render.world_to_screen( mid, sm ) )
		return;
	char buf[ 32 ];
	std::snprintf( buf, sizeof( buf ), "esurf %c%s", s_es_viz.act, s_es_viz.pinned ? " pin" : "" );
	text_draw_object_t text;
	text.m_font       = font;
	text.m_position   = c_vector_2d( sm.m_x - font->CalcTextSizeA( font->FontSize, FLT_MAX, 0.f, buf ).x * 0.5f, sm.m_y - font->FontSize );
	text.m_text       = buf;
	text.m_color      = pick;
	text.m_draw_flags = text_flag_dropshadow;
	g_render.m_draw_data.emplace_back( e_draw_type::draw_type_text, std::make_any< text_draw_object_t >( text ) );
}

/* air_align.cpp flies this guide against a point's wall */
char edge_surf_guide( const float err, const float gap, const float r_min, const float v_old, const float vxy, const int kc, const float authority,
                      const float turn, const float reach, const bool owned, const float ipt, float& v )
{
	const es_guide_t g = es_guide( err, gap, r_min, v_old, vxy, kc, authority, turn, reach, owned, true, ipt );
	v                  = g.v;
	return g.act;
}
