#include "movement_internal.h"
#include <algorithm>
#include <atomic>
#include <climits>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <share.h>
#include <cwchar>
#include <format>
#include <fstream>
#include <mutex>
#include <span>
#include <string>
#include "../../dependencies/imgui/imgui.h"
#include "../../dependencies/json/json.hpp"
#include "../chat_hud.h"
#include "texturebug.h"
#include "texturebug_common.h"
#include "wall_climb.h"
#include "edgebug.h"
#include "pixel_finder_scan.h"
#include "../../utilities/perf/perf_watch.h"

namespace
{
	void ps_finder_unclip( const c_vector& eye, const c_vector& end, trace_t& trace )
	{
		if ( !trace.m_start_solid && !trace.m_all_solid )
			return;
		constexpr unsigned int mask_noclip = mask_playersolid & ~static_cast< unsigned int >( contents_playerclip );
		ray_t retry( eye, end );
		n_tb::c_trace_filter_tb_world_props retry_flt( g_ctx.m_local );
		g_interfaces.m_engine_trace->trace_ray( retry, mask_noclip, &retry_flt, &trace );
	}
}

static c_vector StartPos, EndPos{ };
static c_vector WallNormal{ };
static bool s_pf_anchored = false;
static std::string s_pf_anchor_hit;
static std::vector< c_vector > Points{ };
static std::vector< c_vector > Point_bounce{ };
static std::vector< c_vector > Point_bounce_ceil{ };

bool pf_hb_is_ceiling( const c_vector& dot )
{
	return std::find( Point_bounce_ceil.begin( ), Point_bounce_ceil.end( ), dot ) != Point_bounce_ceil.end( );
}
static std::vector< c_vector > Point_tb{ };
static std::vector< c_vector > Point_tb_head{ };

bool pf_tb_is_head( const c_vector& dot )
{
	return std::find( Point_tb_head.begin( ), Point_tb_head.end( ), dot ) != Point_tb_head.end( );
}
static std::vector< std::pair< c_vector, float > > Point_tb_rise{ };

static std::vector< c_vector > pf_tb_drawn( )
{
	std::vector< c_vector > out = Point_tb;
	for ( const auto& [ seam, h ] : Point_tb_rise )
		std::replace( out.begin( ), out.end( ), seam, c_vector( seam.m_x, seam.m_y, seam.m_z - h ) );
	return out;
}

void pf_tb_rise_seam( c_vector& dot )
{
	for ( const auto& [ seam, h ] : Point_tb_rise )
		if ( c_vector( seam.m_x, seam.m_y, seam.m_z - h ) == dot ) {
			dot = seam;
			return;
		}
}
static std::vector< c_vector > Point_pj{ };

constexpr float k_pf_share        = 0.2f;
constexpr float k_pf_max_span     = 1024.f;
constexpr float k_pf_hull         = 54.f;
constexpr float k_pf_stand_hull   = 72.f;
constexpr float k_pf_stand_reach  = 4.f;
constexpr float k_pf_face_reach   = 24.f;
constexpr float k_pf_spot_out     = 40.f;
constexpr float k_pf_hb_speed     = 200.f;
constexpr float k_pf_press        = 10.f;
constexpr float k_pf_slide        = 1.f;
constexpr float k_pf_bounds_slide = 450.f;
constexpr float k_pf_air_accel    = 12.f;
constexpr int k_pf_arm_tries      = 128;
constexpr float k_pf_gap_brush    = 0.00928f;
constexpr float k_pf_gap_disp     = 0.001f;
constexpr float k_pf_gap_rest     = 0.0325f;
constexpr float k_pf_hb_nz        = 0.1f;
constexpr float k_pf_hb_cut       = 1.f;
constexpr float k_pf_hb_under     = 0.02f;
constexpr float k_pf_hb_ctrl      = 0.1f;
constexpr float k_pf_over_reach   = 0.35f;
constexpr float k_pf_pj_floor     = 1.f;
constexpr float k_pf_gap_pj       = 0.001f;
constexpr float k_pf_skin_band    = 0.035f;
constexpr float k_pf_skin_over    = 0.015f;
constexpr int k_pf_skin_bisect    = 12;
constexpr int k_pf_prop_bisect    = 20;
constexpr float k_pf_head_over    = 0.146f;
constexpr float k_pf_head_press   = 2.f;
constexpr float k_pf_duck_move    = 0.34f;
constexpr float k_pf_edge_fall[ 2 ][ 2 ] = { { -150.f, 1.84f }, { -60.f, 0.78f } };
constexpr float k_pf_rise[ 2 ][ 2 ]      = { { 250.f, 2.93f }, { 75.f, 0.88f } };
constexpr float k_pf_rise_jam[ 2 ]       = { 25.f, 0.1f };

enum : int { pf_held = 0, pf_grounded, pf_unpinned, pf_moved, pf_left_walk, pf_stuck, pf_void_start, pf_dup, pf_off, pf_cut, pf_unverified };
constexpr int k_pf_outcomes   = 11;
constexpr char k_pf_why[ 12 ] = "HGPMLXVDOCU";
constexpr int k_pf_wait       = -1;

enum : int { pf_k_surf = 0, pf_k_stand, pf_k_hb, pf_k_head, pf_k_rise, pf_kinds };
constexpr const char* k_pf_kind_name[ pf_kinds ] = { "ps", "stand", "hb", "head", "rise" };
constexpr float k_pf_kind_step[ pf_kinds ]       = { n_pf::k_lat_surf, n_pf::k_lat_surf, n_pf::k_lat_hb, n_pf::k_lat_tb, n_pf::k_lat_tb };
constexpr float k_pf_kind_reach[ pf_kinds ]      = { n_pf::k_lat, k_pf_stand_reach, n_pf::k_lat, n_pf::k_tb_reach, n_pf::k_tb_reach };
constexpr float k_pf_edge_d[ 2 ]                 = { n_pf::k_edge_off, 0.025f };

static int pf_hold_verdict( int t, float z0, float z_prev, float z, float vz, float pin, float step, bool grounded, bool walking )
{
	if ( !walking )
		return pf_left_walk;
	if ( grounded )
		return pf_grounded;
	if ( std::fabs( vz - pin ) >= 0.01f )
		return pf_unpinned;
	if ( t == 0 ? ( z > z0 + 0.001f || z < z0 - step - 0.05f ) : std::fabs( z - z_prev ) > 0.001f )
		return pf_moved;
	return pf_held;
}

static int pf_bump_verdict( float z0, float z, float vz, float pin, float rise, bool grounded, bool walking )
{
	if ( !walking )
		return pf_left_walk;
	if ( grounded )
		return pf_grounded;
	const bool stop = std::fabs( vz - pin ) < 0.01f;
	if ( !stop && k_pf_hb_speed + pin - vz <= k_pf_hb_cut )
		return pf_unpinned;
	if ( z < z0 - 0.001f || z > z0 + rise + 0.01f )
		return pf_moved;
	return stop ? pf_held : pf_cut;
}

static float s_pf_h = k_pf_hull;
static c_vector pf_mins( ) { return c_vector( -16.f, -16.f, 0.f ); }
static c_vector pf_maxs( ) { return c_vector( 16.f, 16.f, s_pf_h ); }

static unsigned char& pf_move_type( c_base_entity* e ) { return reinterpret_cast< unsigned char& >( e->get_move_type( ) ); }

static bool pf_clear( const c_vector& at )
{
	trace_t tr;
	ray_t ray( at, at, pf_mins( ), pf_maxs( ) );
	c_trace_filter flt( g_ctx.m_local );
	g_interfaces.m_engine_trace->trace_ray( ray, mask_playersolid, &flt, &tr );
	return !tr.m_start_solid && !tr.m_all_solid;
}

static void pf_mover_trace( const c_vector& at, const c_vector& seed, float max_speed, float dt, const ray_t& ray, trace_t& tr )
{
	static i_trace_list_data* list = nullptr;
	if ( list )
		list->reset( );
	else
		list = g_interfaces.m_engine_trace->alloc_trace_list_data( );
	c_trace_filter flt( g_ctx.m_local );
	if ( list ) {
		const float r = ( seed.length( ) + max_speed ) * dt + 1.f;
		const c_vector bloat( r, r, r + g_ctx.m_local->get_step_size( ) );
		g_interfaces.m_engine_trace->setup_leaf_and_entity_list_box( at + c_vector( -16.f, -16.f, 0.f ) - bloat, at + c_vector( 16.f, 16.f, 72.f ) + bloat,
		                                                             list );
		if ( list->can_trace_ray( ray ) ) {
			g_interfaces.m_engine_trace->trace_ray_against_leaf_and_entity_list( ray, list, mask_playersolid, &flt, &tr );
			return;
		}
	}
	g_interfaces.m_engine_trace->trace_ray( ray, mask_playersolid, &flt, &tr );
}

static bool pf_void( const c_vector& at )
{
	const c_vector mid = at + c_vector( 0.f, 0.f, s_pf_h * 0.5f );
	return ( g_interfaces.m_engine_trace->get_point_contents_world_only( mid, contents_solid ) & contents_solid ) != 0;
}

static void pf_place( const c_vector& at, const c_vector& vel )
{
	auto* local        = g_ctx.m_local;
	const auto mapped = [ local ]( const void* field ) { return reinterpret_cast< std::uintptr_t >( field ) != reinterpret_cast< std::uintptr_t >( local ); };
	if ( unsigned char& move = pf_move_type( local ); mapped( &move ) )
		move = static_cast< unsigned char >( move_type_walk );
	local->get_origin( ) = at;
	local->set_abs_origin( at );
	local->get_velocity( ) = vel;
	local->get_flags( ) &= ~fl_onground;
	if ( unsigned int& ground = local->get_ground_entity_handle( ); mapped( &ground ) )
		ground = 0xFFFFFFFFu;
	if ( int& eflags = local->get_eflags( ); mapped( &eflags ) )
		eflags |= 1 << 12;
}

static int pf_buttons( int buttons, bool duck ) { return duck ? ( buttons | in_duck ) & ~in_jump : buttons & ~( in_duck | in_jump ); }

/* airborne duck and unduck are instant ( CCSGameMovement::Duck !playerTouchingGround ) */
static bool pf_arm( c_user_cmd* cmd, const c_vector& at, int buttons, bool duck = true )
{
	auto* local = g_ctx.m_local;
	for ( const float dz : { 0.f, 40.f, -40.f } ) {
		pf_place( at + c_vector( 0.f, 0.f, dz ), c_vector( 0.f, 0.f, 300.f ) );
		cmd->m_buttons      = pf_buttons( buttons, duck );
		cmd->m_forward_move = 0.f;
		cmd->m_side_move    = 0.f;
		g_prediction.begin( local, cmd );
		g_prediction.end( local );
		if ( ( ( local->get_flags( ) & fl_ducking ) != 0 ) == duck )
			return true;
	}
	return false;
}

static int pf_hold( c_user_cmd* cmd, const c_vector& at, const c_vector& vel_xy, float fwd, float side, float pin, float step, float& out_z )
{
	auto* local = g_ctx.m_local;
	pf_place( at, c_vector( vel_xy.m_x, vel_xy.m_y, pin - 0.5f ) );
	cmd->m_forward_move = fwd;
	cmd->m_side_move    = side;
	float z_prev        = at.m_z;
	for ( int t = 0; t < 3; ++t ) {
		g_prediction.begin( local, cmd );
		g_prediction.end( local );
		const float z  = local->get_origin( ).m_z;
		const float vz = local->get_velocity( ).m_z;
		const int v = pf_hold_verdict( t, at.m_z, z_prev, z, vz, pin, step, ( local->get_flags( ) & fl_onground ) != 0,
		                               pf_move_type( local ) == move_type_walk );
		if ( v != pf_held ) {
			out_z = z;
			return v;
		}
		z_prev = z;
	}
	out_z = z_prev;
	return pf_held;
}

static int pf_bump( c_user_cmd* cmd, const c_vector& at, const c_vector& vel_xy, float fwd, float side, float pin, float dt, float& out_z,
                    float speed = k_pf_hb_speed )
{
	auto* local = g_ctx.m_local;
	pf_place( at, c_vector( vel_xy.m_x, vel_xy.m_y, speed - pin ) );
	cmd->m_forward_move = fwd;
	cmd->m_side_move    = side;
	g_prediction.begin( local, cmd );
	g_prediction.end( local );
	out_z          = local->get_origin( ).m_z;
	const float vz = local->get_velocity( ).m_z;
	return pf_bump_verdict( at.m_z, out_z, vz, pin, speed * dt, ( local->get_flags( ) & fl_onground ) != 0, pf_move_type( local ) == move_type_walk );
}

static int pf_fall( c_user_cmd* cmd, const c_vector& at, float vz0, float fwd, float side, float pin, float step )
{
	auto* local = g_ctx.m_local;
	pf_place( at, c_vector( 0.f, 0.f, vz0 ) );
	cmd->m_forward_move = fwd;
	cmd->m_side_move    = side;
	g_prediction.begin( local, cmd );
	g_prediction.end( local );
	const float z  = local->get_origin( ).m_z;
	const float vz = local->get_velocity( ).m_z;
	return pf_hold_verdict( 0, at.m_z, at.m_z, z, vz, pin, step, ( local->get_flags( ) & fl_onground ) != 0, pf_move_type( local ) == move_type_walk );
}

float pf_head_over( )
{
	const float s = n_tick::engine_interval( ) * 64.f;
	return k_pf_head_over * s * s;
}

static float pf_press( float dt )
{
	const float s = dt * 64.f;
	return k_pf_press / ( s * s );
}

static float pf_air_accel( )
{
	static auto var = g_interfaces.m_convar->find_var( "sv_airaccelerate" );
	return var ? var->get_float( ) : k_pf_air_accel;
}

static int pf_contact( c_vector& at, const c_vector& n, float dt, float& hull, float& move, int& traces )
{
	const bool on_x = std::fabs( n.m_x ) > 0.9999f;
	const float sgn = ( on_x ? n.m_x : n.m_y ) > 0.f ? 1.f : -1.f;
	float& c        = on_x ? at.m_x : at.m_y;
	c += sgn * 16.f;
	hull = c;
	move = 0.f;
	const auto clear = [ & ]( ) {
		++traces;
		return pf_clear( at );
	};
	for ( int k = 0; !clear( ); c = std::nextafter( c, c + sgn * 1000.f ) )
		if ( ++k > 8 )
			return pf_stuck;
	for ( int k = 0;; ++k ) {
		if ( k == 8 )
			return pf_off;
		const float keep = c;
		c = std::nextafter( c, c - sgn * 1000.f );
		if ( !clear( ) ) {
			c = keep;
			break;
		}
	}
	hull = c;
	if ( pf_void( at ) )
		return pf_void_start;
	const float u = std::fabs( std::nextafter( c, c + sgn * 1000.f ) - c );
	move          = k_pf_head_press * u / ( pf_air_accel( ) * dt * dt ) / k_pf_duck_move;
	return pf_held;
}

static int pf_head_probe( c_user_cmd* cmd, const c_vector& face_p, const c_vector& n, float seam, float rot, float pin, float step, float dt,
                          float& hull, float& move, int& how, int& traces )
{
	how = 0;
	c_vector at( face_p.m_x, face_p.m_y, seam - k_pf_hull + pf_head_over( ) );
	if ( const int c = pf_contact( at, n, dt, hull, move, traces ); c != pf_held )
		return c;
	const float fx = std::cos( rot ) * move, sx = -std::sin( rot ) * move;
	float got      = 0.f;
	const int o    = pf_hold( cmd, at, c_vector( 0.f, 0.f, 0.f ), fx, sx, pin, step, got );
	if ( o == pf_held )
		return o;
	const float s = dt * 64.f;
	for ( int i = 0; i < 2; ++i ) {
		at.m_z = seam - k_pf_hull + k_pf_edge_fall[ i ][ 1 ] * s;
		++traces;
		if ( pf_clear( at ) && pf_fall( cmd, at, k_pf_edge_fall[ i ][ 0 ], fx, sx, pin, step ) == pf_held ) {
			how = 1 + i;
			return pf_held;
		}
	}
	return o;
}

static std::string pf_edge_how( int how, float dt )
{
	const float s = dt * 64.f;
	if ( how == 0 )
		return std::format( "rest, head {:.4f} over", pf_head_over( ) );
	return std::format( "falling {:.0f}, head {:.4f} over", -k_pf_edge_fall[ how - 1 ][ 0 ], k_pf_edge_fall[ how - 1 ][ 1 ] * s );
}

static bool pf_world_brush( const trace_t& tr )
{
	if ( tr.surface.m_name && ( strstr( tr.surface.m_name, "displacement" ) || strstr( tr.surface.m_name, "**studio**" ) ) )
		return false;
	c_base_entity* const e = tr.m_hit_entity;
	if ( !e || e == g_interfaces.m_client_entity_list->get< c_base_entity >( 0 ) )
		return true;
	const model_t* const model = e->get_model( );
	const char* const name     = model ? g_interfaces.m_model_info->get_model_name( model ) : nullptr;
	const c_angle& ang         = e->get_abs_angles( );
	return name && name[ 0 ] == '*' && ang.m_x == 0.f && ang.m_y == 0.f && ang.m_z == 0.f;
}

static bool pf_depth( const c_vector& col, const c_vector& n, float z, float& out, bool& box, int& traces, trace_t* tr_out = nullptr )
{
	const c_vector at( col.m_x, col.m_y, z );
	trace_t tr;
	ray_t ray( at + n * k_pf_face_reach, at - n * 16.f );
	n_tb::c_trace_filter_tb_world_props flt( g_ctx.m_local );
	g_interfaces.m_engine_trace->trace_ray( ray, mask_playersolid, &flt, &tr );
	++traces;
	if ( tr_out )
		*tr_out = tr;
	if ( tr.m_start_solid || tr.m_fraction >= 1.f )
		return false;
	const c_vector pn = tr.m_plane.m_normal;
	const c_vector p  = tr.m_end - pn * ( tr.m_end.dot_product( pn ) - tr.m_plane.m_distance );
	out               = p.m_x * n.m_x + p.m_y * n.m_y;
	box               = pf_world_brush( tr ) && ( std::fabs( pn.m_x ) > 0.9999f || std::fabs( pn.m_y ) > 0.9999f );
	return true;
}

/* only a face the hull touches ( <= DIST_EPSILON along its normal ) stopped the head */
static bool pf_overhead( const c_vector& stop, trace_t& tr, int& traces )
{
	ray_t ray( stop, stop + c_vector( 0.f, 0.f, k_pf_over_reach ), pf_mins( ), pf_maxs( ) );
	n_tb::c_trace_filter_tb_world_props flt( g_ctx.m_local );
	g_interfaces.m_engine_trace->trace_ray( ray, mask_playersolid, &flt, &tr );
	++traces;
	return !tr.m_start_solid && tr.m_fraction < 1.f && tr.m_fraction * k_pf_over_reach * -tr.m_plane.m_normal.m_z <= 0.03125f;
}

static int pf_rise_probe( c_user_cmd* cmd, const c_vector& face_p, const c_vector& n, float seam, float rot, float pin, float dt,
                          float& hull, float& move, int& how, int& traces )
{
	how           = 0;
	const float s = dt * 64.f;
	c_vector at( face_p.m_x, face_p.m_y, seam - k_pf_rise[ 0 ][ 1 ] * s );
	if ( const int c = pf_contact( at, n, dt, hull, move, traces ); c != pf_held )
		return c;
	const float fx   = std::cos( rot ) * move, sx = -std::sin( rot ) * move;
	const auto rise1 = [ & ]( float vz, float h ) {
		at.m_z = seam - h * s;
		++traces;
		float got   = 0.f;
		const int v = pf_clear( at ) ? pf_bump( cmd, at, c_vector( 0.f, 0.f, 0.f ), fx, sx, pin, dt, got, vz ) : pf_stuck;
		return v == pf_cut ? pf_unpinned : v;
	};
	int o = pf_unpinned;
	for ( int i = 0; i < 2 && o != pf_held; ++i ) {
		o   = rise1( k_pf_rise[ i ][ 0 ], k_pf_rise[ i ][ 1 ] );
		how = 1 + i;
		trace_t over;
		if ( o == pf_held && pf_overhead( g_ctx.m_local->get_origin( ), over, traces ) )
			o = pf_unpinned;
	}
	if ( o == pf_held && rise1( k_pf_rise_jam[ 0 ], k_pf_rise_jam[ 1 ] ) == pf_held )
		return pf_stuck;
	return o;
}

static std::string pf_tally( const int* n )
{
	std::string out;
	for ( int i = 0; i < k_pf_outcomes; ++i )
		if ( n[ i ] > 0 )
			out += std::format( " {}{}", k_pf_why[ i ], n[ i ] );
	return out.empty( ) ? std::string( " -" ) : out;
}

struct pf_ctx_t {
	c_user_cmd* cmd = nullptr;
	c_vector n{ }, along{ }, into{ }, push{ };
	float pin = 0.f, dt = 0.f, step = 0.f, rot = 0.f, fwd = 0.f, side = 0.f, support = 0.f;
	bool axial = false;
	int traces = 0;
};

static pf_ctx_t pf_make_ctx( c_user_cmd* cmd, const c_vector& wall_normal )
{
	pf_ctx_t c;
	c.cmd     = cmd;
	c.n       = c_vector( wall_normal.m_x, wall_normal.m_y, 0.f ).normalized( );
	c.along   = c_vector( c.n.m_y, -c.n.m_x, 0.f );
	c.into    = c.n * -1.f;
	c.push    = c.into * k_pf_press;
	c.pin     = g_prediction.get_engine_target_predict_z_velocity( );
	c.dt      = n_tick::engine_interval( );
	c.step    = -2.f * c.pin * c.dt;
	c.rot     = deg2rad( c.into.to_angle( ).m_y - cmd->m_view_point.m_y );
	c.fwd     = std::cos( c.rot ) * pf_press( c.dt );
	c.side    = -std::sin( c.rot ) * pf_press( c.dt );
	c.support = 16.f * ( std::fabs( c.n.m_x ) + std::fabs( c.n.m_y ) );
	c.axial   = std::fabs( c.n.m_x ) > 0.9999f || std::fabs( c.n.m_y ) > 0.9999f;
	return c;
}

struct pf_spot_t {
	c_vector o{ };
	float gap = 0.f;
	bool disp = false, prop = false;
};

/* hull origin `gap` off the wall at this column and height: exact plane math on brushes / disps, last clear spot on models ( their
   hull trace plane reads 1/32 out ) */
static int pf_spot( pf_ctx_t& c, const c_vector& col, float z, pf_spot_t& sp )
{
	const c_vector at( col.m_x, col.m_y, z );
	trace_t tr;
	ray_t ray( at + c.n * k_pf_spot_out, at - c.n * 16.f, pf_mins( ), pf_maxs( ) );
	n_tb::c_trace_filter_tb_world_props flt( g_ctx.m_local );
	g_interfaces.m_engine_trace->trace_ray( ray, mask_playersolid, &flt, &tr );
	++c.traces;
	if ( tr.m_start_solid || tr.m_all_solid )
		return pf_stuck;
	if ( tr.m_fraction >= 1.f )
		return pf_off;
	const c_vector pn = tr.m_plane.m_normal;
	const float pn_xy = pn.length_2d( );
	const bool disp_hit = tr.surface.m_name && strstr( tr.surface.m_name, "displacement" );
	sp.prop             = tr.surface.m_name && strstr( tr.surface.m_name, "**studio**" );
	if ( pn_xy < 0.1f || ( pn.m_x * c.n.m_x + pn.m_y * c.n.m_y ) / pn_xy < ( disp_hit ? 0.95f : 0.999f ) ||
	     std::fabs( pn.m_z ) > ( disp_hit ? 0.35f : 0.05f ) )
		return pf_off;
	sp.disp = disp_hit;
	if ( disp_hit && ( std::fabs( pn.m_x ) > 0.9999f || std::fabs( pn.m_y ) > 0.9999f ) ) {
		/* disp edge flush on a brush wall wins the hull tie: brush face at hull mid height = brush spot ( ps + seam tb ) */
		float d  = 0.f;
		bool box = false;
		const float face_d = ( tr.m_end - c.n * ( c.support + 0.03125f ) ).dot_product( c.n );
		if ( pf_depth( col, c.n, z + s_pf_h * 0.5f, d, box, c.traces ) && box && std::fabs( d - face_d ) < 0.05f )
			sp.disp = false;
	}
	sp.gap = sp.disp ? k_pf_gap_disp : k_pf_gap_brush;
	if ( !sp.prop ) {
		const float dist = n_tb::hull_plane_dist( tr, pf_mins( ), pf_maxs( ) );
		const float t    = n_pf::wall_shift( tr.m_end.m_x, tr.m_end.m_y, tr.m_end.m_z, pn.m_x, pn.m_y, pn.m_z, dist, c.n.m_x, c.n.m_y, sp.gap, s_pf_h );
		sp.o = tr.m_end + c.n * t;
	} else {
		float in = -0.1f, out = 0.f;
		++c.traces;
		if ( pf_clear( tr.m_end + c.n * in ) )
			return pf_off;
		for ( int i = 0; i < k_pf_prop_bisect; ++i ) {
			const float mid = 0.5f * ( in + out );
			++c.traces;
			( pf_clear( tr.m_end + c.n * mid ) ? out : in ) = mid;
		}
		sp.o = tr.m_end + c.n * ( out + sp.gap );
	}
	for ( int i = 0;; ++i ) {
		++c.traces;
		if ( pf_clear( sp.o ) )
			break;
		if ( i == 8 )
			return pf_stuck;
		sp.o = sp.o + c.n * 1e-4f;
	}
	return pf_void( sp.o ) ? pf_void_start : pf_held;
}

static c_vector pf_dot( const c_vector& face_pt, float z ) { return c_vector( face_pt.m_x, face_pt.m_y, z ); }

static c_vector pf_face_pt( const pf_ctx_t& c, const pf_spot_t& sp ) { return sp.o - c.n * ( c.support + sp.gap ); }

static bool pf_near_z( const std::vector< c_vector >& dots, float z, float tol )
{
	return std::any_of( dots.begin( ), dots.end( ), [ & ]( const c_vector& q ) { return std::fabs( q.m_z - z ) < tol; } );
}

/* the mover's own first bump ( leaf list, same t 0 tie order ) must land on an up face, else the hold sim can't pin */
static bool pf_surf_gate( pf_ctx_t& c, const pf_spot_t& sp, bool slides )
{
	for ( const float a : { 0.f, k_pf_slide, -k_pf_slide } ) {
		if ( a != 0.f && !slides )
			break;
		trace_t tr;
		ray_t ray( sp.o, sp.o + ( c.push + c.along * a + c_vector( 0.f, 0.f, 2.f * c.pin - 0.5f ) ) * c.dt, pf_mins( ), pf_maxs( ) );
		pf_mover_trace( sp.o, c.push + c.along * a + c_vector( 0.f, 0.f, c.pin - 0.5f ), a != 0.f ? k_pf_bounds_slide : 0.f, c.dt, ray, tr );
		++c.traces;
		if ( !tr.m_start_solid && tr.m_fraction < 1.f && tr.m_plane.m_normal.m_z > 0.7f )
			return true;
	}
	return false;
}

/* rising bump: pressed ( t 0 tie with the face over the head ) or straight up ( a ceiling the wall clip slides into ) */
static bool pf_hb_gate( pf_ctx_t& c, const pf_spot_t& sp )
{
	for ( const bool press : { true, false } ) {
		const c_vector v = ( press ? c.push : c_vector( 0.f, 0.f, 0.f ) ) + c_vector( 0.f, 0.f, k_pf_hb_speed );
		trace_t tr;
		ray_t ray( sp.o, sp.o + v * c.dt, pf_mins( ), pf_maxs( ) );
		pf_mover_trace( sp.o, v, 0.f, c.dt, ray, tr );
		++c.traces;
		if ( !tr.m_start_solid && tr.m_fraction < 1.f && tr.m_plane.m_normal.m_z < -k_pf_hb_nz )
			return true;
	}
	return false;
}

struct pf_row_t {
	bool ok = false, disp = false;
	float depth = 0.f, dist = 0.f;
	c_vector pn{ };
	const void* ent  = nullptr;
	const char* name = nullptr;
};
constexpr int k_pf_col_edges = 12;

static pf_row_t pf_row( pf_ctx_t& c, const c_vector& col, float z )
{
	pf_row_t r;
	const c_vector at( col.m_x, col.m_y, z );
	trace_t tr;
	ray_t ray( at + c.n * k_pf_face_reach, at - c.n * 16.f );
	n_tb::c_trace_filter_tb_world_props flt( g_ctx.m_local );
	g_interfaces.m_engine_trace->trace_ray( ray, mask_playersolid, &flt, &tr );
	++c.traces;
	const c_vector pn = tr.m_plane.m_normal;
	const float pn_xy = pn.length_2d( );
	if ( tr.m_start_solid || tr.m_fraction >= 1.f || pn_xy < 0.1f || ( pn.m_x * c.n.m_x + pn.m_y * c.n.m_y ) / pn_xy < 0.95f || std::fabs( pn.m_z ) > 0.35f )
		return r;
	const c_vector p = tr.m_end - pn * ( tr.m_end.dot_product( pn ) - tr.m_plane.m_distance );
	r.ok    = true;
	r.depth = p.m_x * c.n.m_x + p.m_y * c.n.m_y;
	r.pn    = pn;
	r.dist  = tr.m_plane.m_distance;
	r.ent   = tr.m_hit_entity;
	r.name  = tr.surface.m_name;
	r.disp  = r.name && strstr( r.name, "displacement" );
	return r;
}

static bool pf_row_same( const pf_row_t& a, const pf_row_t& b )
{
	if ( a.ok != b.ok )
		return false;
	if ( !a.ok || ( a.disp && b.disp ) )
		return true;
	return a.pn.dot_product( b.pn ) > 0.9999f && std::fabs( a.dist - b.dist ) < 0.002f && a.ent == b.ent &&
	       ( a.name == b.name || ( a.name && b.name && !strcmp( a.name, b.name ) ) );
}

/* one wall column top down, 1u rows: every face change ( depth, entity, texture ) bisected to its exact height. top = the lower
   face sticks out ( a ledge / prop top to surf on ), under = the upper one does ( a bottom edge to bounce under ) */
static void pf_edges( pf_ctx_t& c, const c_vector& col, float lo, float hi, std::vector< n_pf::height_t >& out )
{
	pf_row_t up = pf_row( c, col, hi + 1.5f );
	int found   = 0;
	for ( float z = hi + 0.5f; z >= lo - 1.5f; z -= 1.f ) {
		const pf_row_t dn = pf_row( c, col, z );
		if ( !pf_row_same( up, dn ) && found < k_pf_col_edges ) {
			const float e = n_pf::bisect( z, z + 1.f, [ & ]( float m ) { return pf_row_same( pf_row( c, col, m ), up ); } );
			const pf_row_t a = pf_row( c, col, e + 0.01f ), b = pf_row( c, col, e - 0.01f );
			n_pf::height_t h;
			h.z     = e;
			h.top   = b.ok && ( !a.ok || b.depth > a.depth + 0.002f );
			h.under = a.ok && ( !b.ok || a.depth > b.depth + 0.002f );
			if ( !h.top && !h.under )
				h.top = h.under = a.ok && b.ok;
			if ( h.top || h.under ) {
				out.push_back( h );
				++found;
			}
		}
		up = dn;
	}
}

static int pf_surf( pf_ctx_t& c, const pf_spot_t& sp, float& got, float& slid )
{
	slid  = 0.f;
	int o = pf_hold( c.cmd, sp.o, c.push, c.fwd, c.side, c.pin, c.step, got );
	for ( const float a : { k_pf_slide, -k_pf_slide } ) {
		if ( o != pf_unpinned )
			break;
		slid                            = a;
		g_prediction.m_bounds_max_speed = k_pf_bounds_slide;
		o                               = pf_hold( c.cmd, sp.o, c.push + c.along * a, c.fwd, c.side, c.pin, c.step, got );
		g_prediction.m_bounds_max_speed = 0.f;
	}
	return o;
}

static int pf_hb( pf_ctx_t& c, const pf_spot_t& sp, float& head, bool& ceil, bool& rose, std::string& over_s )
{
	float got = 0.f;
	const int o = pf_bump( c.cmd, sp.o, c.push, c.fwd, c.side, c.pin, c.dt, got );
	if ( o != pf_held && o != pf_cut )
		return o;
	head                = got + k_pf_hull;
	rose                = got - sp.o.m_z >= 0.01f;
	const c_vector stop = g_ctx.m_local->get_origin( );
	trace_t over;
	ceil           = pf_overhead( stop, over, c.traces );
	const float nz = ceil ? over.m_plane.m_normal.m_z : 0.f;
	const bool never = !over.m_start_solid && !ceil && !rose;
	bool window      = false;
	if ( never ) {
		float got_c = 0.f;
		pf_bump( c.cmd, sp.o - c_vector( 0.f, 0.f, k_pf_hb_ctrl ), c.push, c.fwd, c.side, c.pin, c.dt, got_c );
		window = got_c > got + 0.01f;
	}
	over_s = ceil ? std::format( "{} nz {:.3f}", over.surface.m_name ? over.surface.m_name : "?", nz ) : window ? std::string( "nothing ( seam window )" )
	                                                                                                         : std::string( "nothing ( wall pin )" );
	if ( over.m_start_solid || ( never && !window ) )
		return pf_stuck;
	if ( !ceil && !window && sp.disp )
		return pf_unverified;
	if ( o == pf_cut && ( !ceil || nz >= -k_pf_hb_nz ) )
		return pf_unpinned;
	return pf_held;
}

static struct {
	bool busy = false;
	int buttons = 0, arm_fails = 0;
	float lo = 0.f, hi = 0.f, k = 0.f;
	int stage = 0, col_i = 0, h_i = 0;
	std::vector< n_pf::height_t > edges, hs;
	bool stand_off = false, surf_hit = false;
	int kind = 0, li = 0, first = pf_off;
	bool hit = false;
	float hit_lat = 0.f;
	std::string trail;
	bool row_live = false;
	float row_z = 0.f;
	const char* row_name = nullptr;
	const void* row_ent  = nullptr;
	int row_box          = 0;
	int tally[ pf_kinds ][ k_pf_outcomes ]{ };
	int tb_seams = 0, tb_heads = 0, tb_edges = 0, tb_rises = 0, tb_unverified = 0, pj_lips = 0, pj_skins = 0;
	int chunks = 0, sims = 0, traces = 0;
	long long used_us = 0, next_qpc = 0, worst_chunk_us = 0;
} s_pf_scan;

static bool pf_pj_taken( float pz )
{
	return pz < s_pf_scan.lo - 0.5f || pz > s_pf_scan.hi + 0.5f ||
	       std::any_of( Point_pj.begin( ), Point_pj.end( ), [ & ]( const c_vector& q ) { return q.m_z <= pz + 0.05f && pz <= q.m_z + 2.05f; } );
}

static void pf_pj_add( const c_vector& dot )
{
	Point_pj.erase( std::remove_if( Point_pj.begin( ), Point_pj.end( ), [ & ]( const c_vector& q ) { return dot.m_z < q.m_z && q.m_z <= dot.m_z + 2.05f; } ),
	                Point_pj.end( ) );
	Point_pj.push_back( dot );
}

/* skin lip ( prop top flush with the wall ): a hull 0.001 off lands AT end, 0.035 deeper meets nothing, the face continues over the top */
static bool pf_skin_lip( pf_ctx_t& c, const pf_spot_t& sp, float end, const char* where )
{
	const c_vector q = sp.o + c.n * ( k_pf_gap_pj - sp.gap );
	const auto sweep = [ & ]( float e, trace_t& t ) {
		ray_t r( c_vector( q.m_x, q.m_y, e + 2.f ), c_vector( q.m_x, q.m_y, e ), pf_mins( ), pf_maxs( ) );
		c_trace_filter f( g_ctx.m_local );
		g_interfaces.m_engine_trace->trace_ray( r, mask_playersolid, &f, &t );
		++c.traces;
		return !t.m_start_solid && t.m_fraction < 1.f;
	};
	const auto lands = [ & ]( float e, bool walk ) {
		trace_t t;
		return sweep( e, t ) && t.m_end.m_z < e + 0.05f && ( !walk || t.m_plane.m_normal.m_z >= 0.7f );
	};
	if ( !lands( end, true ) )
		return false;
	trace_t deep;
	if ( sweep( end - k_pf_skin_band, deep ) || deep.m_start_solid )
		return false;
	float lo = end - k_pf_skin_band, top = end;
	for ( int i = 0; i < k_pf_skin_bisect; ++i ) {
		const float mid = 0.5f * ( lo + top );
		( lands( mid, false ) ? top : lo ) = mid;
	}
	const c_vector face_pt = pf_face_pt( c, sp );
	float d  = 0.f;
	bool box = false;
	if ( !pf_depth( face_pt, c.n, top + 0.5f, d, box, c.traces ) || std::fabs( d - ( face_pt.m_x * c.n.m_x + face_pt.m_y * c.n.m_y ) ) > 0.05f ) {
		botox_dbg_log( "[pf pj] skin top %.4f: no face over it ( wall top = floor ) | %s", top, where );
		return true;
	}
	const float pz = top + 0.03125f;
	if ( pf_pj_taken( pz ) )
		return true;
	pf_pj_add( pf_dot( face_pt, pz ) );
	++s_pf_scan.pj_skins;
	botox_dbg_log( "[pf pj] dot z %.4f | %s, skin: prop top %.4f flush with the face, trace end %.4f over it", pz, where, top, end - top );
	return true;
}

/* hull `off` from the face, traced from just over got down 2.5u: an up face there that a hull 1u further out does not reach = lip */
static void pf_pixel_jump( pf_ctx_t& c, const pf_spot_t& sp, float got, const char* where, float off )
{
	const c_vector o = c.n * ( off - sp.gap );
	const c_vector at( sp.o.m_x, sp.o.m_y, 0.f );
	trace_t tr;
	ray_t ray( at + c_vector( 0.f, 0.f, got + 0.1f ) + o, at + c_vector( 0.f, 0.f, got - 2.5f ) + o, pf_mins( ), pf_maxs( ) );
	c_trace_filter flt( g_ctx.m_local );
	g_interfaces.m_engine_trace->trace_ray( ray, mask_playersolid, &flt, &tr );
	++c.traces;
	const c_vector ln = tr.m_plane.m_normal;
	if ( tr.m_start_solid || tr.m_fraction >= 1.f || ln.m_z < 0.7f ) {
		pf_skin_lip( c, sp, got - 2.f, where );
		return;
	}
	const float pz = tr.m_end.m_z;
	if ( pf_pj_taken( pz ) )
		return;
	const auto reaches = [ & ]( float out ) {
		const c_vector p = at + c.n * ( out - sp.gap );
		const float z    = pz - ( out - off ) * ( ln.m_x * c.n.m_x + ln.m_y * c.n.m_y ) / ln.m_z;
		trace_t t;
		ray_t r( c_vector( p.m_x, p.m_y, z + 0.1f ), c_vector( p.m_x, p.m_y, z - 0.1f ), pf_mins( ), pf_maxs( ) );
		g_interfaces.m_engine_trace->trace_ray( r, mask_playersolid, &flt, &t );
		++c.traces;
		return !t.m_start_solid && t.m_fraction < 1.f && t.m_plane.m_normal.m_z >= 0.7f;
	};
	if ( reaches( k_pf_pj_floor ) )
		return;
	const bool natural = reaches( k_pf_gap_rest );
	pf_pj_add( pf_dot( pf_face_pt( c, sp ), pz ) );
	++s_pf_scan.pj_lips;
	botox_dbg_log( "[pf pj] dot z %.4f ( probe %.4f ) | %s, %s", pz, got, where, natural ? "natural" : "tight" );
}

/* texture / entity change on the face between two rows = a model top somewhere in between: bisect it, test it as a skin lip */
static void pf_skin_row( pf_ctx_t& c, const pf_spot_t& sp, float z )
{
	auto& s                = s_pf_scan;
	const c_vector face_pt = pf_face_pt( c, sp );
	const float face_d     = face_pt.m_x * c.n.m_x + face_pt.m_y * c.n.m_y;
	const auto row = [ & ]( float rz, trace_t& t ) {
		float d  = 0.f;
		bool box = false;
		return pf_depth( face_pt, c.n, rz, d, box, c.traces, &t ) && std::fabs( d - face_d ) < 0.05f;
	};
	const auto same = [ & ]( const trace_t& t ) {
		const char* nm = t.surface.m_name;
		return t.m_hit_entity == s.row_ent && t.m_hitbox == s.row_box && ( nm == s.row_name || ( nm && s.row_name && !strcmp( nm, s.row_name ) ) );
	};
	trace_t t;
	const bool live = row( z, t );
	if ( live && s.row_live && !same( t ) ) {
		float lo = z, top = s.row_z;
		for ( int i = 0; i < k_pf_skin_bisect; ++i ) {
			const float mid = 0.5f * ( lo + top );
			trace_t m;
			( row( mid, m ) && same( m ) ? top : lo ) = mid;
		}
		pf_skin_lip( c, sp, top + k_pf_skin_over, "skin row" );
	}
	s.row_live = live;
	s.row_z    = z;
	s.row_name = t.surface.m_name;
	s.row_ent  = t.m_hit_entity;
	s.row_box  = t.m_hitbox;
}

static struct {
	bool pending = false;
	c_vector pos{ }, n{ };
	float slid = 0.f;
	bool ducked = true;
} s_pf_catch;

void pf_check_catch( const c_vector& pos, const c_vector& wall_n, float slid, bool ducked )
{
	s_pf_catch.pending = true;
	s_pf_catch.pos     = pos;
	s_pf_catch.n       = wall_n;
	s_pf_catch.slid    = slid;
	s_pf_catch.ducked  = ducked;
}

static void pf_catch_check( c_user_cmd* cmd )
{
	s_pf_catch.pending = false;
	pf_ctx_t c         = pf_make_ctx( cmd, s_pf_catch.n );
	const c_vector pos = s_pf_catch.pos;
	const c_vector col = pos - c.n * c.support;

	std::string dot = "none";
	for ( const c_vector& q : Points )
		if ( std::fabs( q.m_z - pos.m_z ) < 0.05f && ( q - col ).length_2d( ) < 40.f ) {
			dot = std::format( "z {:.4f} {:.1f}u off", q.m_z, ( q - col ).length_2d( ) );
			break;
		}
	std::string scan = "none";
	if ( s_pf_scan.hi >= s_pf_scan.lo && StartPos.length( ) > 0.f ) {
		const c_vector off = col - StartPos;
		scan = std::format( "along {:+.1f} depth {:+.2f} seams {:.0f}..{:.0f}", off.dot_product( c.along ), off.dot_product( c.n ), s_pf_scan.lo, s_pf_scan.hi );
	}

	const n_pf::height_t* near_h = nullptr;
	for ( const n_pf::height_t& h : s_pf_scan.hs )
		if ( !near_h || std::fabs( h.z - pos.m_z ) < std::fabs( near_h->z - pos.m_z ) )
			near_h = &h;
	std::string probe = near_h ? std::format( " | nearest finder height {:.4f} ( {} ) {:+.4f} under the feet", near_h->z,
	                                          near_h->seam ? "seam" : near_h->top ? "edge top" : "edge under", pos.m_z - near_h->z )
	                           : std::string( " | no finder heights" );
	{
		const int buttons = cmd->m_buttons;
		const float fwd0 = cmd->m_forward_move, side0 = cmd->m_side_move;
		const bool duck  = s_pf_catch.ducked;
		s_pf_h           = duck ? k_pf_hull : k_pf_stand_hull;
		g_prediction.restore_entity_to_predicted_frame( g_interfaces.m_prediction->m_commands_predicted - 1 );
		pf_spot_t sp;
		const int at = pf_spot( c, col, pos.m_z, sp );
		if ( at != pf_held )
			probe += std::format( " | probe at catch z: spot {}", k_pf_why[ at ] );
		else if ( !pf_arm( cmd, pos + c.n * 40.f, buttons, duck ) )
			probe += " | stance refused";
		else {
			cmd->m_buttons = pf_buttons( buttons, duck );
			float got = 0.f, slid = 0.f;
			const bool gate = pf_surf_gate( c, sp, true );
			const int o     = pf_surf( c, sp, got, slid );
			probe += std::format( " | probe at catch z {}: gate {} sim {} slide {:+.0f}{}", duck ? "ducked" : "standing", gate ? "pass" : "fail", k_pf_why[ o ], slid,
			                      sp.prop ? " prop" : sp.disp ? " disp" : "" );
		}
		g_prediction.restore_entity_to_predicted_frame( g_interfaces.m_prediction->m_commands_predicted - 1 );
		s_pf_h              = k_pf_hull;
		cmd->m_buttons      = buttons;
		cmd->m_forward_move = fwd0;
		cmd->m_side_move    = side0;
	}
	botox_dbg_log( "[pf check] real surf z %.4f at %.2f %.2f n %.2f %.2f slid %+.0f | finder dot %s | last scan %s%s", pos.m_z, pos.m_x, pos.m_y, c.n.m_x,
	               c.n.m_y, s_pf_catch.slid, dot.c_str( ), scan.c_str( ), probe.c_str( ) );
}

struct pf_trick_t {
	const char* kind;
	c_vector pos;
	bool ducked;
};
static std::vector< pf_trick_t > s_pf_tricks;
constexpr size_t k_pf_tricks        = 64;
constexpr float k_pf_trick_dz       = 2.5f;
constexpr float k_pf_trick_rise     = 4.f;
constexpr float k_pf_trick_hb_reach = 26.f;

static std::string pf_trick_dot( const std::vector< c_vector >& dots, const char* name, const c_vector& pos, float head, float reach, float& dz )
{
	const c_vector* best = nullptr;
	float best_dz = reach;
	bool at_head  = false;
	for ( const c_vector& q : dots ) {
		if ( ( q - pos ).length_2d( ) > 48.f )
			continue;
		for ( const bool h : { false, true } )
			if ( const float d = q.m_z - ( h ? head : pos.m_z ); std::fabs( d ) < std::fabs( best_dz ) ) {
				best    = &q;
				best_dz = d;
				at_head = h;
			}
	}
	dz = best ? best_dz : NAN;
	return best ? std::format( " | {} z {:.3f} ( {} {:+.3f} ) {:.1f}u off", name, best->m_z, at_head ? "head" : "feet", best_dz, ( *best - pos ).length_2d( ) )
	            : std::format( " | {} none", name );
}

static std::string pf_trick_dots( const pf_trick_t& t, bool& found )
{
	const float head = t.pos.m_z + ( t.ducked ? k_pf_hull : 72.f );
	float tb = NAN, hb = NAN, ps = NAN, pj = NAN;
	std::string out = pf_trick_dot( Point_tb, "tb", t.pos, head, 4.f, tb ) + pf_trick_dot( Point_bounce, "hb", t.pos, head, k_pf_trick_hb_reach, hb ) +
	                  pf_trick_dot( Points, "ps", t.pos, head, 4.f, ps ) + pf_trick_dot( Point_pj, "pj", t.pos, head, 4.f, pj );
	const auto within = [ ]( float dz ) { return std::fabs( dz ) <= k_pf_trick_dz; }; /* NAN reads false */
	const bool hb_ok  = hb >= -k_pf_trick_dz && hb <= k_pf_trick_hb_reach;
	const bool tb_rise = std::any_of( Point_tb.begin( ), Point_tb.end( ), [ & ]( const c_vector& q ) {
		return ( q - t.pos ).length_2d( ) <= 48.f && q.m_z > t.pos.m_z && q.m_z - t.pos.m_z <= k_pf_trick_rise;
	} );
	const bool tb_ok = within( tb ) || tb_rise;
	const bool is_hb = !strcmp( t.kind, "head_bounce" ), is_hs = !strcmp( t.kind, "head_surf" );
	found = is_hb ? hb_ok : is_hs ? hb_ok || tb_ok : tb_ok;
	return out;
}

void pf_check_trick( const char* kind, const c_vector& pos, bool ducked )
{
	const pf_trick_t t{ kind, pos, ducked };
	if ( s_pf_tricks.size( ) >= k_pf_tricks )
		s_pf_tricks.erase( s_pf_tricks.begin( ) );
	s_pf_tricks.push_back( t );
	bool found = false;
	const std::string dots = pf_trick_dots( t, found );
	const std::string scan = s_pf_scan.hi >= s_pf_scan.lo && StartPos.length( ) > 0.f
	                             ? std::format( "last scan anchor {:.0f}u off, seams {:.0f}..{:.0f}", ( StartPos - pos ).length_2d( ), s_pf_scan.lo, s_pf_scan.hi )
	                             : std::string( "no scan" );
	botox_dbg_log( "[pf trick] %s at %.2f %.2f feet %.3f head %.3f duck %d | %s%s", kind, pos.m_x, pos.m_y, pos.m_z, pos.m_z + ( ducked ? k_pf_hull : 72.f ),
	               ducked ? 1 : 0, scan.c_str( ), dots.c_str( ) );
}

static void pf_recheck_tricks( )
{
	const c_vector n     = c_vector( WallNormal.m_x, WallNormal.m_y, 0.f ).normalized( );
	const c_vector along = c_vector( n.m_y, -n.m_x, 0.f );
	int in_band = 0, found_n = 0;
	for ( const pf_trick_t& t : s_pf_tricks ) {
		const c_vector off = t.pos - StartPos;
		const float head   = t.pos.m_z + ( t.ducked ? k_pf_hull : 72.f );
		const bool z_in    = ( t.pos.m_z >= s_pf_scan.lo - 1.f && t.pos.m_z <= s_pf_scan.hi + 1.f ) || ( head >= s_pf_scan.lo - 1.f && head <= s_pf_scan.hi + 1.f );
		if ( std::fabs( off.dot_product( along ) ) > n_pf::k_lat + 2.f || off.dot_product( n ) < 0.f || off.dot_product( n ) > 48.f || !z_in )
			continue;
		++in_band;
		bool found = false;
		const std::string dots = pf_trick_dots( t, found );
		found_n += found;
		botox_dbg_log( "[pf trick] rescan %s %s at %.2f %.2f feet %.3f head %.3f duck %d | along %+.1f%s", found ? "FOUND" : "MISSED", t.kind, t.pos.m_x,
		               t.pos.m_y, t.pos.m_z, head, t.ducked ? 1 : 0, off.dot_product( along ), dots.c_str( ) );
	}
	if ( in_band > 0 )
		botox_dbg_log( "[pf trick] rescan: %d of %d catch(es) in this band have a dot of their kind within %.1fu", found_n, in_band, k_pf_trick_dz );
}

void n_movement::impl_t::on_level_init( )
{
	StartPos = EndPos = c_vector( 0.f, 0.f, 0.f );
	s_pf_scan         = { };
	s_pf_catch        = { };
	s_pf_tricks.clear( );
	s_pf_anchored     = false;
	Points.clear( );
	Point_bounce.clear( );
	Point_bounce_ceil.clear( );
	Point_tb.clear( );
	Point_tb_head.clear( );
	Point_tb_rise.clear( );
	Point_pj.clear( );
	animated_points.clear( );
	animated_points2.clear( );
	animated_points3.clear( );
	animated_points4.clear( );
	m_route_calc_data.clear( );
	m_pixel_calc_data.clear( );
}

void n_movement::impl_t::pixel_finder( c_user_cmd* cmd )
{
	if ( !GET_VARIABLE( g_variables.m_pixel_finder, bool ) ) {
		s_pf_scan.busy     = false;
		s_pf_catch.pending = false;
		s_pf_anchored      = false;
		if ( !Points.empty( ) || !Point_bounce.empty( ) || !Point_tb.empty( ) || !Point_pj.empty( ) ) {
			Points.clear( );
			Point_bounce.clear( );
			Point_bounce_ceil.clear( );
			Point_tb.clear( );
			Point_tb_head.clear( );
			Point_tb_rise.clear( );
			Point_pj.clear( );
			animated_points.clear( );
			animated_points2.clear( );
			animated_points3.clear( );
			animated_points4.clear( );
		}
		return;
	}
	if ( cmd == nullptr ) {
		std::vector< c_vector > none;
		pf_render_dots( GET_VARIABLE( g_variables.m_pixel_finder_pixelsurfs, bool ) ? Points : none,
		                GET_VARIABLE( g_variables.m_pixel_finder_headbounces, bool ) ? Point_bounce : none,
		                GET_VARIABLE( g_variables.m_pixel_finder_texturebugs, bool ) ? pf_tb_drawn( ) : none,
		                GET_VARIABLE( g_variables.m_pixel_finder_pixeljumps, bool ) ? Point_pj : none );
		if ( !s_pf_anchored || !g_input.check_input( &GET_VARIABLE( g_variables.m_pixel_finder_key, key_bind_t ) ) )
			return;
		c_vector_2d start2D, end2D;
		if ( !g_render.world_to_screen( StartPos, start2D ) || !g_render.world_to_screen( EndPos, end2D ) )
			return;
		c_color accent          = GET_VARIABLE( g_variables.m_accent, c_color );
		float outline_thickness = 3.f;
		c_color outline_color   = c_color( 0.f, 0.f, 0.f, 0.5f );
		float s                 = 10.f;
		float speed             = 50.f;
		float l                 = start2D.distance( end2D );
		if ( l > 0.f ) {
			static float offset = 0.f;
			offset              = fmod( offset + speed * ImGui::GetIO( ).DeltaTime, 2 * s );
			g_render.m_draw_data.emplace_back( e_draw_type::draw_type_line,
			                                   std::make_any< line_draw_object_t >( start2D, end2D, outline_color.get_u32( ), outline_thickness ) );
			std::vector< float > transitions;
			transitions.push_back( 0.f );
			int m_start = static_cast< int >( ceil( offset / s ) );
			for ( int m = m_start;; m++ ) {
				float d = -offset + m * s;
				if ( d > l )
					break;
				float u = d / l;
				if ( u >= 0.f && u <= 1.f ) {
					transitions.push_back( u );
				}
			}
			transitions.push_back( 1.f );
			for ( size_t i = 0; i < transitions.size( ) - 1; i++ ) {
				float u0 = transitions[ i ];
				float u1 = transitions[ i + 1 ];
				if ( u1 > u0 ) {
					c_vector_2d p0 = start2D + ( end2D - start2D ) * u0;
					c_vector_2d p1 = start2D + ( end2D - start2D ) * u1;
					float u_mid    = ( u0 + u1 ) / 2.f;
					float d_mid    = u_mid * l;
					int phase      = static_cast< int >( floor( ( d_mid + offset ) / s ) ) % 2;
					c_color color  = ( phase == 0 ) ? accent : c_color( 128, 128, 128, 255 );
					g_render.m_draw_data.emplace_back( e_draw_type::draw_type_line,
					                                   std::make_any< line_draw_object_t >( p0, p1, color.get_u32( ), 1.f ) );
				}
			}
		}
		return;
	}

	static bool held_prev = false;
	const bool held       = g_input.check_input( &GET_VARIABLE( g_variables.m_pixel_finder_key, key_bind_t ) );
	const c_vector eye    = g_ctx.m_local->get_eye_position( );
	const c_vector dir    = c_vector::fromAngle( c_vector( cmd->m_view_point.m_x, cmd->m_view_point.m_y, 0.f ) );

	if ( held && !held_prev ) {
		s_pf_scan.busy = false;
		s_pf_anchored  = false;
		Points.clear( );
		Point_bounce.clear( );
		Point_bounce_ceil.clear( );
		Point_tb.clear( );
		Point_tb_head.clear( );
		Point_tb_rise.clear( );
		Point_pj.clear( );
		const c_vector end = eye + dir * 6000.f;
		trace_t trace      = { };
		ray_t ray( eye, end );
		n_tb::c_trace_filter_tb_world_props flt( g_ctx.m_local );
		g_interfaces.m_engine_trace->trace_ray( ray, mask_playersolid, &flt, &trace );
		ps_finder_unclip( eye, end, trace );
		if ( trace.m_fraction >= 1.f || trace.m_start_solid || std::fabs( trace.m_plane.m_normal.m_z ) > 0.7f ) {
			movement_add_window( 3.f, "pixel finder: aim at a wall" );
		} else {
			WallNormal    = trace.m_plane.m_normal;
			StartPos = EndPos = trace.m_end;
			s_pf_anchored = true;
			c_base_entity* const e     = trace.m_hit_entity;
			const model_t* const model = e && e != g_interfaces.m_client_entity_list->get< c_base_entity >( 0 ) ? e->get_model( ) : nullptr;
			s_pf_anchor_hit = std::format( "{} on {}", trace.surface.m_name ? trace.surface.m_name : "?",
			                               model ? g_interfaces.m_model_info->get_model_name( model ) : "world" );
		}
	} else if ( held && s_pf_anchored ) {
		const float denom = dir.dot_product( WallNormal );
		if ( denom < -0.01f ) {
			const float t = ( StartPos - eye ).dot_product( WallNormal ) / denom;
			EndPos = c_vector( StartPos.m_x, StartPos.m_y,
			                   std::clamp( eye.m_z + dir.m_z * t, StartPos.m_z - k_pf_max_span, StartPos.m_z + k_pf_max_span ) );
		}
	} else if ( !held && held_prev && s_pf_anchored ) {
		s_pf_anchored = false;
		auto& s       = s_pf_scan;
		s             = { };
		s.busy        = true;
		s.buttons     = cmd->m_buttons; /* frozen: a key tapped mid-scan must not change half the scan */
		s.hi          = n_pf::seam_top( StartPos.m_z, EndPos.m_z );
		s.lo          = n_pf::seam_bottom( StartPos.m_z, EndPos.m_z );
		s.k           = s.hi;
		const char* const map = g_interfaces.m_engine_client->get_level_name_short( );
		botox_dbg_log( "[pf] scan heights %.0f..%.0f ( drag %.2f..%.2f ) n %.4f %.4f %.4f at %.2f %.2f | pin %.4f | map %s, aim hit %s", s.lo, s.hi,
		               ( std::min )( StartPos.m_z, EndPos.m_z ), ( std::max )( StartPos.m_z, EndPos.m_z ), WallNormal.m_x, WallNormal.m_y, WallNormal.m_z,
		               StartPos.m_x, StartPos.m_y, g_prediction.get_engine_target_predict_z_velocity( ), map ? map : "?", s_pf_anchor_hit.c_str( ) );
	}
	held_prev = held;

	if ( s_pf_catch.pending && !s_pf_scan.busy && g_ctx.m_local->is_alive( ) )
		pf_catch_check( cmd );

	if ( !s_pf_scan.busy || n_tick::qpc_now( ) < s_pf_scan.next_qpc )
		return;
	if ( !g_ctx.m_local->is_alive( ) ) {
		s_pf_scan.busy = false;
		return;
	}

	PERF_ZONE( zone_cmd_pixel_finder );
	auto& s                        = s_pf_scan;
	const bool ps_on               = GET_VARIABLE( g_variables.m_pixel_finder_pixelsurfs, bool );
	const bool hb_on               = GET_VARIABLE( g_variables.m_pixel_finder_headbounces, bool );
	const bool pj_on               = GET_VARIABLE( g_variables.m_pixel_finder_pixeljumps, bool );
	const bool tb_on               = GET_VARIABLE( g_variables.m_pixel_finder_texturebugs, bool );
	const int live_buttons         = cmd->m_buttons;
	const float live_fwd           = cmd->m_forward_move;
	const float live_side          = cmd->m_side_move;
	const unsigned int sims_before = g_prediction.m_sim_count;
	pf_ctx_t c                     = pf_make_ctx( cmd, WallNormal );
	const bool kind_on[ pf_kinds ] = { ps_on || tb_on || pj_on, ps_on || tb_on, hb_on, tb_on && c.axial, tb_on && c.axial };

	bool armed = false, arm_ok = false, armed_duck = true;
	const auto ready = [ & ]( bool duck ) -> int {
		if ( !armed || armed_duck != duck ) {
			if ( !armed )
				g_prediction.restore_entity_to_predicted_frame( g_interfaces.m_prediction->m_commands_predicted - 1 );
			armed          = true;
			armed_duck     = duck;
			arm_ok         = pf_arm( cmd, StartPos + c.n * 40.f, s.buttons, duck );
			cmd->m_buttons = pf_buttons( s.buttons, duck );
			if ( duck )
				s.arm_fails = arm_ok ? 0 : s.arm_fails + 1;
			else if ( !arm_ok ) {
				s.stand_off = true;
				botox_dbg_log( "[pf] no room to stand at the anchor, standing probes skipped" );
			}
		}
		if ( arm_ok )
			return 1;
		if ( !duck )
			return -1;
		if ( s.arm_fails > k_pf_arm_tries ) {
			movement_add_window( 3.f, "pixel finder: crouch kept failing, scan stopped" );
			botox_dbg_log( "[pf] duck refused %d chunks in a row, scan stopped", s.arm_fails );
			s.h_i = static_cast< int >( s.hs.size( ) );
		}
		return 0;
	};

	const auto probe = [ & ]( int kind, float lat, int di, const n_pf::height_t& h ) -> int {
		const c_vector col = StartPos + c.along * lat;
		const bool line    = lat == 0.f;
		s_pf_h             = kind == pf_k_stand ? k_pf_stand_hull : k_pf_hull;
		if ( kind == pf_k_surf || kind == pf_k_stand ) {
			const bool duck = kind == pf_k_surf;
			pf_spot_t sp;
			const float z = h.seam ? n_pf::marker( h.z ) : h.z + k_pf_edge_d[ di ];
			if ( const int at = pf_spot( c, col, z, sp ); at != pf_held )
				return at;
			if ( duck && line && di == 0 && pj_on ) {
				pf_pixel_jump( c, sp, z, "lip", k_pf_gap_pj );
				if ( h.seam && !sp.disp && !sp.prop )
					pf_skin_row( c, sp, h.z + 0.5f );
			}
			if ( !ps_on && !tb_on ) {
				s.li = INT_MAX / 2;
				return pf_off;
			}
			if ( !pf_surf_gate( c, sp, line ) )
				return pf_unpinned;
			if ( const int r = ready( duck ); r != 1 )
				return r == 0 ? k_pf_wait : pf_off;
			float got = 0.f, slid = 0.f;
			const int o = pf_surf( c, sp, got, slid );
			if ( o == pf_grounded && duck && pj_on )
				pf_pixel_jump( c, sp, got, "grounded", sp.gap );
			if ( o != pf_held )
				return o;
			if ( pf_near_z( Points, got, 0.02f ) )
				return pf_dup;
			const c_vector dot = pf_dot( pf_face_pt( c, sp ), got );
			Points.push_back( dot );
			botox_dbg_log( "[pf ps] dot z %.4f | %s %.4f lat %+.1f %s slide %+.0f, %s%s", got, h.seam ? "seam" : "edge", h.z, lat, duck ? "ducked" : "standing", slid,
			               sp.disp ? "disp" : "brush", sp.prop ? " prop" : "" );
			if ( n_pf::is_marker( got ) ) {
				if ( !c.axial || sp.disp )
					++s.tb_unverified;
				else if ( !pf_near_z( Point_tb, std::floor( got ), 0.05f ) ) {
					Point_tb.emplace_back( dot.m_x, dot.m_y, std::floor( got ) );
					++s.tb_seams;
				}
			}
			return pf_held;
		}
		if ( kind == pf_k_hb ) {
			pf_spot_t sp;
			if ( const int at = pf_spot( c, col, h.z - k_pf_hb_under - k_pf_hull, sp ); at != pf_held )
				return at;
			if ( !pf_hb_gate( c, sp ) )
				return pf_unpinned;
			if ( const int r = ready( true ); r != 1 )
				return r == 0 ? k_pf_wait : pf_off;
			float head = 0.f;
			bool ceil = false, rose = false;
			std::string over_s;
			const int o = pf_hb( c, sp, head, ceil, rose, over_s );
			if ( o == pf_unverified )
				++s.tb_unverified;
			if ( o != pf_held )
				return o;
			if ( head < s.lo - 0.5f || head > s.hi + 0.5f )
				return pf_off;
			if ( pf_near_z( Point_bounce, head, rose ? 0.02f : k_pf_over_reach ) )
				return pf_dup;
			Point_bounce.push_back( pf_dot( pf_face_pt( c, sp ), head ) );
			if ( ceil )
				Point_bounce_ceil.push_back( Point_bounce.back( ) );
			botox_dbg_log( "[pf hb] dot head z %.4f | %s %.4f lat %+.1f, over %s", head, h.seam ? "seam" : "edge", h.z, lat, over_s.c_str( ) );
			return pf_held;
		}
		const bool head_kind = kind == pf_k_head;
		float depth = 0.f;
		bool box    = false;
		if ( !pf_depth( col, c.n, h.z + ( head_kind ? 0.05f : -0.05f ), depth, box, c.traces ) || !box )
			return pf_off;
		if ( pf_near_z( Point_tb, h.z, 0.05f ) )
			return pf_dup;
		if ( const int r = ready( true ); r != 1 )
			return r == 0 ? k_pf_wait : pf_off;
		const c_vector face_p = col + c.n * ( depth - ( col.m_x * c.n.m_x + col.m_y * c.n.m_y ) );
		float hull = 0.f, move = 0.f;
		int how     = 0;
		const int o = head_kind ? pf_head_probe( cmd, face_p, c.n, h.z, c.rot, c.pin, c.step, c.dt, hull, move, how, c.traces )
		                        : pf_rise_probe( cmd, face_p, c.n, h.z, c.rot, c.pin, c.dt, hull, move, how, c.traces );
		if ( o != pf_held )
			return o;
		Point_tb.push_back( pf_dot( face_p, h.z ) );
		if ( head_kind ) {
			if ( how == 0 ) {
				Point_tb_head.push_back( Point_tb.back( ) );
				++s.tb_heads;
			} else
				++s.tb_edges;
			botox_dbg_log( "[pf tb] head dot z %.0f | lat %+.1f, %s, hull %.7f, move %.4f", h.z, lat, pf_edge_how( how, c.dt ).c_str( ), hull, move );
		} else {
			const float rh = k_pf_rise[ how - 1 ][ 1 ] * c.dt * 64.f;
			Point_tb_rise.emplace_back( Point_tb.back( ), rh );
			++s.tb_rises;
			botox_dbg_log( "[pf tb] rise dot z %.0f | lat %+.1f, rising %.0f, feet %.4f under, hull %.7f", h.z, lat, k_pf_rise[ how - 1 ][ 0 ], rh, hull );
		}
		return pf_held;
	};

	const auto applies = [ & ]( int kind, const n_pf::height_t& h ) {
		if ( !kind_on[ kind ] )
			return false;
		switch ( kind ) {
		case pf_k_surf: return h.top;
		case pf_k_stand: return h.top && !s.surf_hit && !s.stand_off;
		case pf_k_hb: return h.under;
		default: return h.seam;
		}
	};

	n_tick::c_sim_budget budget;
	budget.start( k_pf_share, 0.f, n_tick::search_pf );
	while ( s.busy && !budget.expired( ) ) {
		if ( s.stage == 0 ) {
			if ( s.col_i < n_pf::lat_count( n_pf::k_lat_edge ) ) {
				pf_edges( c, StartPos + c.along * n_pf::lat( s.col_i++, n_pf::k_lat_edge ), s.lo, s.hi, s.edges );
				continue;
			}
			s.hs = n_pf::heights( s.lo, s.hi, s.edges );
			std::string list;
			int n_edges = 0;
			for ( const n_pf::height_t& h : s.hs )
				if ( !h.seam && ++n_edges <= 24 )
					list += std::format( " {:.4f}{}{}", h.z, h.top ? "t" : "", h.under ? "u" : "" );
			botox_dbg_log( "[pf] %d height(s): %d seam(s) + %d edge(s) from %d raw |%s", static_cast< int >( s.hs.size( ) ),
			               static_cast< int >( s.hs.size( ) ) - n_edges, n_edges, static_cast< int >( s.edges.size( ) ), list.c_str( ) );
			s.stage = 1;
			continue;
		}
		if ( s.h_i >= static_cast< int >( s.hs.size( ) ) ) {
			s.busy = false;
			break;
		}
		const n_pf::height_t h = s.hs[ s.h_i ];
		s.k                    = h.z;
		if ( s.kind >= pf_kinds ) {
			if ( !s.trail.empty( ) )
				botox_dbg_log( h.seam ? "[pf h] %.0f |%s" : "[pf h] %.4f edge |%s", h.z, s.trail.c_str( ) );
			s.trail.clear( );
			++s.h_i;
			s.kind     = 0;
			s.li       = 0;
			s.hit      = false;
			s.surf_hit = false;
			s.first    = pf_off;
			continue;
		}
		const int nd    = s.kind <= pf_k_stand && !h.seam ? 2 : 1;
		const int count = n_pf::lat_count( k_pf_kind_step[ s.kind ], k_pf_kind_reach[ s.kind ] ) * nd;
		const bool on   = applies( s.kind, h );
		if ( !on || s.hit || s.li >= count ) {
			if ( on ) {
				++s.tally[ s.kind ][ s.hit ? pf_held : s.first ];
				s.trail += s.hit ? std::format( " {} H{:+.0f}", k_pf_kind_name[ s.kind ], s.hit_lat ) : std::format( " {} {}", k_pf_kind_name[ s.kind ], k_pf_why[ s.first ] );
			}
			if ( s.kind == pf_k_surf )
				s.surf_hit = s.hit;
			++s.kind;
			s.li    = 0;
			s.hit   = false;
			s.first = pf_off;
			continue;
		}
		const float lat = n_pf::lat( s.li / nd, k_pf_kind_step[ s.kind ] );
		const int o     = probe( s.kind, lat, s.li % nd, h );
		if ( o == k_pf_wait )
			break;
		if ( s.li == 0 )
			s.first = o;
		if ( o == pf_held || o == pf_dup ) {
			s.hit     = true;
			s.hit_lat = lat;
		}
		++s.li;
	}
	s_pf_h = k_pf_hull;

	const long long chunk_us = budget.used_us( );
	s.used_us += chunk_us;
	s.worst_chunk_us = ( std::max )( s.worst_chunk_us, chunk_us );
	s.sims += static_cast< int >( g_prediction.m_sim_count - sims_before );
	s.traces += c.traces;
	s.next_qpc = n_tick::qpc_now( ) + 2ll * chunk_us * n_tick::qpc_freq( ) / 1000000ll;
	++s.chunks;
	if ( !s.busy ) {
		botox_dbg_log( "[pf] scan done heights %.0f..%.0f | ps%s | stand%s | hb%s | head%s | rise%s | tb %d seam %d head %d edge %d rise, %d diagonal/disp not marked | "
		               "pj %d lip %d skin | %d cmd(s), %lld us, worst chunk %lld us, %d sims, %d traces",
		               s.lo, s.hi, pf_tally( s.tally[ pf_k_surf ] ).c_str( ), pf_tally( s.tally[ pf_k_stand ] ).c_str( ), pf_tally( s.tally[ pf_k_hb ] ).c_str( ),
		               pf_tally( s.tally[ pf_k_head ] ).c_str( ),
		               pf_tally( s.tally[ pf_k_rise ] ).c_str( ), s.tb_seams, s.tb_heads, s.tb_edges, s.tb_rises, s.tb_unverified, s.pj_lips, s.pj_skins,
		               s.chunks, s.used_us, s.worst_chunk_us, s.sims, s.traces );
		pf_recheck_tricks( );
		const int pixelsurf_count = ps_on ? static_cast< int >( Points.size( ) ) : 0;
		const int bounce_count    = hb_on ? static_cast< int >( Point_bounce.size( ) ) : 0;
		const int tb_count        = tb_on ? static_cast< int >( Point_tb.size( ) ) : 0;
		const int pj_count        = pj_on ? static_cast< int >( Point_pj.size( ) ) : 0;
		std::string msg;
		const auto add_count = [ &msg ]( int count, const char* what ) {
			if ( count <= 0 )
				return;
			msg += msg.empty( ) ? "" : " and ";
			msg += std::to_string( count ) + " " + what;
		};
		add_count( pixelsurf_count, "pixelsurf" );
		add_count( bounce_count, "headbounce" );
		add_count( tb_count, "texturebug" );
		add_count( pj_count, "pixeljump" );
		if ( msg.empty( ) )
			msg = "no points found :(";
		else
			msg = std::to_string( pixelsurf_count + bounce_count + tb_count + pj_count ) + " points found: " + msg;
		if ( !tb_on && !Point_tb.empty( ) )
			msg += " (" + std::to_string( Point_tb.size( ) ) + " texturebug(s) hidden)";
		movement_add_window( 3.0f, msg );
	}
	cmd->m_buttons      = live_buttons;
	cmd->m_forward_move = live_fwd;
	cmd->m_side_move    = live_side;
	if ( armed )
		g_prediction.restore_entity_to_predicted_frame( g_interfaces.m_prediction->m_commands_predicted - 1 );
}