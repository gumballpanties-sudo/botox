/* pixel finder: surf, head bounce, texturebug seam and pixel jump dots */
#include "movement_internal.h"
#include "texturebug.h"
#include "texturebug_common.h"
#include "edgebug.h"
#include "../chat_hud.h"
#include "../../utilities/perf/perf_watch.h"
#include "pixel_finder_bounce.h"
#include "pixel_finder_bounce_state.h"
#include <algorithm>
#include <atomic>
#include <climits>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <format>
#include <fstream>
#include <mutex>
#include <memory>
#include <span>
#include <string>
#include "../../dependencies/imgui/imgui.h"
#include "../../dependencies/json/json.hpp"
#include "wall_climb.h"
#include "../../game/sdk/enums/e_flags.h"


namespace
{
	/* eye inside a player clip ( railings, stair noses ) = start_solid aim ray, zero normal: retry
	   without playerclip. a clip the ray runs INTO is kept ( real surface to the mover ) */
	void ps_finder_unclip( const c_vector& eye, const c_vector& end, trace_t& trace )
	{
		if ( !trace.m_start_solid && !trace.m_all_solid )
			return;
		constexpr unsigned int mask_noclip = mask_playersolid & ~static_cast< unsigned int >( contents_playerclip );
		ray_t retry( eye, end );
		n_tb::c_trace_filter_tb_world_props retry_flt( g_ctx.m_local );
		g_interfaces.m_engine_trace->trace_ray( retry, mask_noclip, &retry_flt, &trace );
	}

	/* +/-16u band along the wall ( seams have ends ): centre first, then ends, then fill. how many of
	   the nine run is a cost cap: a tall column gets fewer */
	constexpr float k_pf_band_lat[ 9 ] = { 0.f, -16.f, 16.f, -8.f, 8.f, -12.f, 12.f, -4.f, 4.f };
	/* column runs 8u past both aimed ends */
	constexpr float k_pf_margin = 8.f;

	c_vector pf_wall_tangent( const c_vector& wall_normal )
	{
		return c_vector( wall_normal.m_y, -wall_normal.m_x, 0.f ).normalized( );
	}

	bool pf_band_column( const c_vector& anchor, const c_vector& wall_normal, float lat, c_vector& out )
	{
		if ( lat == 0.f ) {
			out = anchor;
			return true;
		}
		const c_vector n   = c_vector( wall_normal.m_x, wall_normal.m_y, 0.f ).normalized( );
		const c_vector mid = anchor + pf_wall_tangent( wall_normal ) * lat;
		trace_t tr;
		ray_t ray( mid + n * 12.f, mid - n * 12.f );
		n_tb::c_trace_filter_tb_world_props flt( g_ctx.m_local );
		g_interfaces.m_engine_trace->trace_ray( ray, mask_playersolid, &flt, &tr );
		if ( tr.m_start_solid || tr.m_fraction >= 1.f || tr.m_plane.m_normal.dot_product( wall_normal ) < 0.99f )
			return false;
		/* a crate standing out of the wall is not the wall; a prop flush with the aim's plane is ( nuke roof trim ) */
		if ( tr.surface.m_name && strstr( tr.surface.m_name, "**studio**" ) && std::fabs( ( tr.m_end - anchor ).dot_product( n ) ) > 0.05f )
			return false;
		out = c_vector( tr.m_end.m_x, tr.m_end.m_y, anchor.m_z );
		return true;
	}

} // namespace

/* file scope, not function statics: on_level_init has to reach them */
static c_vector StartPos, EndPos{ };
static c_vector WallNormal{ };
static bool s_pf_anchored = false;              /* this hold has a column on a wall */
static std::string s_pf_anchor_hit;             /* what the aim hit ( [pf] scan line ) */
static std::vector< c_vector > Points, Point_bounce, Point_tb, Point_pj;
// Modern grid/sweep holds used only as texturebug probe bookkeeping. Public
// Points is filled by the legacy falling pixelsurf scan.
static std::vector< c_vector > s_pf_tb_scratch{ };
// the Point_bounce dots with a ceiling over the stop (pf_overhead): any rise crossing it stops there, not only a head
// starting in the seam window
static std::vector< c_vector > Point_bounce_ceil{ };
struct pf_bounce_window_t { c_vector dot; float lower, upper; };
static std::vector< pf_bounce_window_t > Point_bounce_windows;

bool pf_hb_window( const c_vector& dot, float& lower, float& upper )
{
	for ( const auto& window : Point_bounce_windows )
		if ( window.dot == dot ) { lower = window.lower; upper = window.upper; return true; }
	return false;
}

/* route calc: this hb dot is a ceiling stop ( crossing ), not a flush seam window */
bool pf_hb_is_ceiling( const c_vector& dot )
{
	return std::find( Point_bounce_ceil.begin( ), Point_bounce_ceil.end( ), dot ) != Point_bounce_ceil.end( );
}
static std::vector< c_vector > Point_tb_head{ }; /* the Point_tb dots a ducked hull's TOP pins over ( 5. ): rider origin z - 54 + pf_head_over( ) */

/* route calc: this tb dot pins the head, not the feet */
bool pf_tb_is_head( const c_vector& dot )
{
	return std::find( Point_tb_head.begin( ), Point_tb_head.end( ), dot ) != Point_tb_head.end( );
}
// the Point_tb dots a rising hull's feet pin under (6.) + how far under (the k_pf_rise probe that held). drawn there, where the
// feet go: at the seam the ring sat 2-3u over the catch
static std::vector< std::pair< c_vector, float > > Point_tb_rise{ };

/* Point_tb as drawn: rising edge dots moved down to the feet */
static std::vector< c_vector > pf_tb_drawn( )
{
	std::vector< c_vector > out = Point_tb;
	for ( const auto& [ seam, h ] : Point_tb_rise )
		std::replace( out.begin( ), out.end( ), seam, c_vector( seam.m_x, seam.m_y, seam.m_z - h ) );
	return out;
}

/* route calc: a drawn rising edge dot back onto its seam ( the plane its texturebug arrival wants ) */
void pf_tb_rise_seam( c_vector& dot )
{
	for ( const auto& [ seam, h ] : Point_tb_rise )
		if ( c_vector( seam.m_x, seam.m_y, seam.m_z - h ) == dot ) {
			dot = seam;
			return;
		}
}
/* Point_pj is Point_pixeljump (see #define) */

constexpr float k_pf_share      = 0.08f;  // of one tick per cmd (0.35 = 5.5ms a cmd = lag)
constexpr float k_pf_max_span   = 1024.f; // drag clamp, a grazing ray put the plane hit miles off
constexpr float k_pf_hull       = 54.f;   // ducked hull top (VEC_DUCK_HULL_MAX)
constexpr int k_pf_sim_cap      = 8000;   /* whole scan; band columns = this / one column's estimate */
constexpr int k_pf_faces        = 2;      /* face depths per column: a sill or trim in front has its own edge */
constexpr float k_pf_face_reach = 24.f;   /* further out than this from the first face = another wall */
constexpr int k_pf_face_rows    = 128;    /* pf_faces rows out from the aim: the faces NEAR it, and a bounded step */
constexpr float k_pf_hb_speed   = 200.f;  /* bump probe vz after StartGravity: > 140, or the move-start categorize grounds it */
constexpr float k_pf_fall_speed = 100.f;  /* sweep seed, down. A crease zeroes vz at any speed */
constexpr float k_pf_press      = 10.f;   /* DNA grids: forward / side 10 and 10 u/s into the wall ( 64 tick: pf_press ) */
constexpr float k_pf_slide      = 1.f;    /* grid retry along-wall speed: only its SIGN picks the BSP side, 1/64u a tick keeps the column */
/* slide retries run on the brush list a sliding rider gets ( SetupMovementBounds box ~8u out ): the list order picks which brush
   wins a t 0 tie. pf_lab bounds: still at 0 + slides at 450 = riders' pins 670/671 nuke, 146/146 mirage; 0 alone 654, 145 */
constexpr float k_pf_bounds_slide = 450.f;
constexpr float k_pf_sub        = 0.5f;   /* grid sub-step along the wall between band columns ( pins hold in runs this narrow ) */
/* tight probe, for a brush face < DIST_EPSILON proud of the brush behind it: hull this far off the proud face, this high over
   the seam ( top of the window ), pressed like a held key ( sv_airaccelerate 12 x 30 x dt a tick ) */
constexpr float k_pf_tight_gap  = 0.0005f;
/* and for a model face: a model trace stops 1/32 out and its plane goes through that end ( [pf hb] step 0.0313 on a flush nuke
   trim ), so pf_faces reads it 1/32 proud. offsets off that read, nearest the real face first; first clear one probes */
constexpr float k_pf_tight_prop[ 5 ] = { -0.031f, -0.025f, -0.02f, -0.01f, -0.0005f };
constexpr float k_pf_tight_d    = 0.031f;
constexpr float k_pf_air_accel  = 12.f;
constexpr float k_pf_press_hard = 30.f;   /* sweep + crawl: a held strafe key, AirAccelerate's 30 u/s cap every tick */
constexpr float k_pf_move_hard  = 450.f;  /* the cmd move that gets it */
constexpr int k_pf_hold_ticks   = 3;      /* sweep: pinned ticks in a row, z still after the first */
constexpr int k_pf_sweep_ticks  = 256;    /* per unit, runaway guard */
constexpr int k_pf_crawls       = 8;      /* crawls per unit: a slope clips every tick it is touched */
constexpr int k_pf_crawl_max    = 32;     /* probes per crawl */
constexpr float k_pf_drift      = 0.1f;   /* sweep re-based when pushed this far off its gap ( slid off a sill ) */
constexpr int k_pf_seek_steps   = 32;     /* start_below: 1u tries per loop step, then it resumes */
constexpr int k_pf_arm_tries    = 128;    /* chunks in a row the duck may be refused before the scan gives up */
constexpr float k_pf_dna_above  = 0.0287018f; /* DNA's marker over a unit seam >= 0 */
constexpr float k_pf_dna_below  = 0.027908f;  /* and under 0: DNA's trunc( z ) - 0.972092 */
constexpr float k_pf_gap_brush  = 0.00928f;   /* hull gap off the face plane: DNA's 15.97803 off a trace end 0.03125 out */
constexpr float k_pf_gap_disp   = 0.001f;
/* pj lip reaching past a hard press's 0.03125 park = "natural", shallower = "tight". log label only, both dot */
constexpr float k_pf_gap_rest   = 0.0325f;
constexpr float k_pf_hb_nz      = 0.1f;
constexpr float k_pf_hb_cut     = 1.f;
/* bounce head under each unit seam: inside the flush seam's stop window [ seam - 0.03125, seam ) ( pf_lab, both signs ) */
constexpr float k_pf_hb_under   = 0.02f;
/* jam control start this much lower: under the window, over the next seam's */
constexpr float k_pf_hb_ctrl    = 0.1f;
constexpr float k_pf_over_reach = 0.35f;
/* point trace this far off the wall. a lip still there is a floor, not a pixeljump */
constexpr float k_pf_pj_floor   = 10.f;
constexpr int k_pf_pj_max       = 24;
constexpr float k_pf_pj_dedup   = 0.08f;
constexpr float k_pf_gap_pj     = 0.001f;
/* skin lip ( route_calc pj_lip_kind ): a prop top flush with the face grounds a hull trace ENDING 0..~0.031 over it only
   ( n_route::k_skin_lo ), one ending under straddles it. a hit this much deeper too = a real lip / floor, not skin */
constexpr float k_pf_skin_band   = 0.035f;
constexpr float k_pf_skin_over   = 0.015f; /* trace end over a candidate top ( route calc's skin test ) */
constexpr int k_pf_skin_bisect   = 12;
/* head seam probe ( pf_lab headlab spot, nuke x=168 seam 48 ): holds head 0.098..0.191 over at 2 ulps, 3 / 4 ulps lose seam 64.
   64 tick value, pf_head_over scales it. recall nuke 43 / 57 ( every pin run >= 4u ), mirage 31 / 32 */
constexpr float k_pf_head_over  = 0.146f;
constexpr float k_pf_head_press = 2.f;   /* float ulps of the wall coord a tick, from rest */
constexpr float k_pf_duck_move  = 0.34f; /* CS_PLAYER_SPEED_DUCK_MODIFIER: a ducked cmd move is scaled by it */
constexpr float k_pf_edge_fall[ 2 ][ 2 ] = { { -150.f, 1.84f }, { -60.f, 0.78f } };
constexpr float k_pf_rise[ 2 ][ 2 ] = { { 250.f, 2.93f }, { 75.f, 0.88f } };
constexpr float k_pf_rise_jam[ 2 ] = { 25.f, 0.1f };

enum : int { pf_held = 0, pf_grounded, pf_unpinned, pf_moved, pf_left_walk, pf_stuck, pf_void_start, pf_dup, pf_off, pf_cut, pf_unverified };
constexpr int k_pf_outcomes   = 11;
constexpr char k_pf_why[ 12 ] = "HGPMLXVDOCU";

struct pf_face_t {
	c_vector p{ };     /* on the face PLANE at this column, z unused */
	c_vector trace_end{ }; /* backup bounce placement starts at the native ray end */
	bool disp = false; /* displacement: graze gap, not dna */
	bool prop = false; /* static prop / model ( [pf u] log ) */
};

/* one grid / crawl tick. tick 0 may settle one pinned drop, then holds still */
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

enum : int { pf_sw_fall = 0, pf_sw_land, pf_sw_pin, pf_sw_dot, pf_sw_leave };
/* one sweep tick. hold = pinned ticks in a row; first from any height, rest hold still */
static int pf_sweep_tick( int& hold, float z_prev, float z, float vz, float pin, bool grounded, bool walking )
{
	if ( !walking ) {
		hold = 0;
		return pf_sw_leave;
	}
	if ( grounded ) {
		hold = 0;
		return pf_sw_land;
	}
	if ( std::fabs( vz - pin ) >= 0.01f ) {
		hold = 0;
		return pf_sw_fall;
	}
	hold = hold > 0 && std::fabs( z - z_prev ) <= 0.001f ? hold + 1 : 1;
	return hold >= k_pf_hold_ticks ? pf_sw_dot : pf_sw_pin;
}

/* dvz = vz minus free seed + 2 pin. a NEW touch changes it; a steady slope clip stays within ~nz^2 * 12.5 u/s a tick */
static bool pf_touched( float dvz, float dvz_prev ) { return std::fabs( dvz - dvz_prev ) > 2.f; }
/* DNA's probe height over the unit seam k */
static float pf_marker( float k ) { return k + ( k < 0.f ? k_pf_dna_below : k_pf_dna_above ); }
/* dot at a DNA marker ( route calc + assists are tuned on these ) */
static bool pf_is_marker( float z ) { return std::fabs( z - pf_marker( std::floor( z ) ) ) < 1e-4f; }
/* highest seam whose marker is <= hi; grid walks down 1u a probe */
static float pf_grid_seam( float hi )
{
	const float k = std::floor( hi );
	return pf_marker( k ) > hi ? k - 1.f : k;
}
/* bounce grid's first head: k_pf_hb_under under the lowest unit seam over lo, then 1u a probe ( one seam window each ) */
static float pf_hb_first( float lo ) { return std::floor( lo ) + 1.f - k_pf_hb_under; }
/* sims per band column ~1.3 faces x ( range / 2 + 20 + 3 range + range / 3 + 2 range ), grid = 3 probes an unpinned marker, rising
   edge grid = 2 a gated seam. ponytail: estimate; [pf] done line has real count */
static int pf_col_sims( float range ) { return static_cast< int >( 7.5f * range ) + 30; }
static void pf_cell( int c, int cols, int ok, float& lo, float& hi )
{
	bool lowest = true, has_next = false;
	float next = 16.f;
	for ( int i = 0; i < cols; ++i ) {
		if ( !( ok >> i & 1 ) )
			continue;
		lowest = lowest && k_pf_band_lat[ i ] >= k_pf_band_lat[ c ];
		if ( k_pf_band_lat[ i ] > k_pf_band_lat[ c ] && ( !has_next || k_pf_band_lat[ i ] < next ) ) {
			next     = k_pf_band_lat[ i ];
			has_next = true;
		}
	}
	lo = lowest ? -16.f - k_pf_band_lat[ c ] : 0.f;
	hi = next - k_pf_band_lat[ c ];
}
/* next sub-step of a cell, nearest the column first ( +0.5, -0.5, +1, -1 .. ) inside [lo, hi): the dot lands by the aim, not
   at the cell's far end. false = cell done */
static bool pf_sub_next( int& k, float lo, float hi, float& d )
{
	const int k_max = static_cast< int >( 2.f * ( std::max )( -lo, hi ) / k_pf_sub ) + 2;
	while ( ++k <= k_max ) {
		d = static_cast< float >( ( k + 1 ) / 2 ) * k_pf_sub * ( k & 1 ? 1.f : -1.f );
		if ( d >= lo && d < hi )
			return true;
	}
	return false;
}

/* engine parts */
static c_vector pf_mins( ) { return c_vector( -16.f, -16.f, 0.f ); }
static c_vector pf_maxs( ) { return c_vector( 16.f, 16.f, k_pf_hull ); }

static unsigned char& pf_move_type( c_base_entity* e ) { return reinterpret_cast< unsigned char& >( e->get_move_type( ) ); }

/* TestPlayerPosition: start == end hull trace; probes start only where a player can BE */
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
	static i_trace_list_data* list = nullptr; /* ponytail: one for the process, like gamemovement's m_pTraceListData */
	if ( list )
		list->reset( );
	else
		list = g_interfaces.m_engine_trace->alloc_trace_list_data( );
	c_trace_filter flt( g_ctx.m_local );
	if ( list ) {
		const float r = ( seed.length( ) + max_speed ) * dt + 1.f;
		const c_vector bloat( r, r, r + 18.f );
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
	const c_vector mid = at + c_vector( 0.f, 0.f, k_pf_hull * 0.5f );
	return ( g_interfaces.m_engine_trace->get_point_contents_world_only( mid, contents_solid ) & contents_solid ) != 0;
}

static void pf_place( const c_vector& at, const c_vector& vel )
{
	auto* local        = g_ctx.m_local;
	const auto mapped = [ local ]( const void* field ) { return reinterpret_cast< std::uintptr_t >( field ) != reinterpret_cast< std::uintptr_t >( local ); };
	if ( unsigned char& move = pf_move_type( local ); mapped( &move ) )
		move = static_cast< unsigned char >( move_type_walk ); /* noclip / ladder: run the walk mover anyway */
	local->get_origin( ) = at;
	local->set_abs_origin( at );
	local->get_velocity( ) = vel;
	local->get_flags( ) &= ~fl_onground;
	if ( unsigned int& ground = local->get_ground_entity_handle( ); mapped( &ground ) )
		ground = 0xFFFFFFFFu;
	if ( int& eflags = local->get_eflags( ); mapped( &eflags ) )
		eflags |= 1 << 12;
}

static bool pf_arm( c_user_cmd* cmd, const c_vector& at, int buttons )
{
	auto* local = g_ctx.m_local;
	for ( const float dz : { 0.f, 40.f, -40.f } ) {
		pf_place( at + c_vector( 0.f, 0.f, dz ), c_vector( 0.f, 0.f, 300.f ) );
		cmd->m_buttons      = ( buttons | in_duck ) & ~in_jump;
		cmd->m_forward_move = 0.f;
		cmd->m_side_move    = 0.f;
		g_prediction.begin( local, cmd );
		g_prediction.end( local );
		if ( local->get_flags( ) & fl_ducking )
			return true;
	}
	return false;
}

static bool pf_pj_taken( const c_vector& wall, float z )
{
	return std::any_of( Point_pj.begin( ), Point_pj.end( ), [ & ]( const c_vector& q ) {
		return std::fabs( q.m_z - z ) < k_pf_pj_dedup && ( q - c_vector( wall.m_x, wall.m_y, z ) ).length_2d( ) < 40.f;
	} );
}

/* ray, not a hull. a 32-wide hull centered near the wall is still inside it and comes back start-solid, so every floor looked like a lip */
static bool pf_pj_extended( const c_vector& wall, const c_vector& n, float z, int* traces )
{
	const c_vector flat( n.m_x, n.m_y, 0.f );
	if ( flat.length_2d( ) < 0.001f )
		return false;
	const c_vector out = c_vector( wall.m_x, wall.m_y, z ) + flat.normalized( ) * k_pf_pj_floor;
	trace_t tr{ };
	ray_t ray( c_vector( out.m_x, out.m_y, z + 4.f ), c_vector( out.m_x, out.m_y, z - 4.f ) );
	c_trace_filter flt( g_ctx.m_local );
	g_interfaces.m_engine_trace->trace_ray( ray, mask_playersolid, &flt, &tr );
	if ( traces )
		++*traces;
	return !tr.m_start_solid && tr.m_fraction < 1.f && tr.m_plane.m_normal.m_z >= 0.7f && std::fabs( tr.m_end.m_z - z ) <= 2.f;
}

static bool pf_pj_accept( const c_vector& wall, const c_vector& n, float z, int* traces )
{
	if ( pf_pj_extended( wall, n, z, traces ) )
		return false;
	if ( Point_pj.size( ) >= static_cast< std::size_t >( k_pf_pj_max ) || pf_pj_taken( wall, z ) )
		return false;
	Point_pj.emplace_back( wall.m_x, wall.m_y, z );
	return true;
}

static c_vector pf_pj_hull_origin( const c_vector& start, const c_vector& n, float al, float z )
{
	c_vector o( start.m_x, start.m_y, z );
	if ( n.m_x < 0 && n.m_y < 0.f ) {
		o.m_x -= al;
		o.m_y -= al;
	} else if ( n.m_x < 0 && n.m_y > 0.f ) {
		o.m_x -= al;
		o.m_y += al;
	} else if ( n.m_x > 0 && n.m_y < 0.f ) {
		o.m_x += al;
		o.m_y -= al;
	} else if ( n.m_x > 0 && n.m_y > 0.f ) {
		o.m_x += al;
		o.m_y += al;
	} else if ( n.m_x == 0.f && n.m_y > 0.f ) {
		o.m_y += al;
	} else if ( n.m_x == 0.f && n.m_y < 0.f ) {
		o.m_y -= al;
	} else if ( n.m_x < 0 && n.m_y == 0.f ) {
		o.m_x -= al;
	} else if ( n.m_x > 0 && n.m_y == 0.f ) {
		o.m_x += al;
	}
	return o;
}

static void pf_pj_set_origin( const c_vector& start, const c_vector& n, float al, float z )
{
	pf_place( pf_pj_hull_origin( start, n, al, z ), c_vector( 0.f, 0.f, -80.f ) );
	g_ctx.m_local->set_abs_origin( c_vector( start.m_x, start.m_y, z ) );
	*( int* )( ( uintptr_t )( c_base_entity* )g_ctx.m_local + 0x25C ) = 2;
}

static c_vector pf_pj_wall_point( const c_vector& origin, const c_vector& n, float al )
{
	c_vector p = origin;
	if ( n.m_x < 0 && n.m_y < 0.f ) {
		p.m_x += al;
		p.m_y += al;
	} else if ( n.m_x < 0 && n.m_y > 0.f ) {
		p.m_x += al;
		p.m_y -= al;
	} else if ( n.m_x > 0 && n.m_y < 0.f ) {
		p.m_x -= al;
		p.m_y += al;
	} else if ( n.m_x > 0 && n.m_y > 0.f ) {
		p.m_x -= al;
		p.m_y -= al;
	} else if ( n.m_x == 0.f && n.m_y > 0.f ) {
		p.m_y -= al;
	} else if ( n.m_x == 0.f && n.m_y < 0.f ) {
		p.m_y += al;
	} else if ( n.m_x < 0 && n.m_y == 0.f ) {
		p.m_x += al;
	} else if ( n.m_x > 0 && n.m_y == 0.f ) {
		p.m_x -= al;
	}
	return p;
}

/* proof the seed reached the mover: on a FREE tick dz / dt = vz - pin and vz must be the seed's free value */
static void pf_seed_check( float z0, float z, float vz, float pin, float dt, float want_vz, int& odd, int& checked )
{
	const float rate = ( z - z0 ) / dt;
	if ( std::fabs( rate ) < 1.f || std::fabs( rate - ( vz - pin ) ) > 0.1f )
		return;
	++checked;
	if ( std::fabs( vz - want_vz ) > 0.02f )
		++odd;
}

/* grid / crawl surf probe: 3 real ticks, pf_hold_verdict each. out_z = last tick's z */
static int pf_hold( c_user_cmd* cmd, const c_vector& at, const c_vector& vel_xy, float fwd, float side, float pin, float step, float dt,
                    float& out_z, int& odd, int& checked )
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
		if ( t == 0 )
			pf_seed_check( at.m_z, z, vz, pin, dt, 3.f * pin - 0.5f, odd, checked );
		const int v = pf_hold_verdict( t, at.m_z, z_prev, z, vz, pin, step, ( local->get_flags( ) & fl_onground ) != 0,
		                               pf_move_type( local ) == move_type_walk );
		if ( v != pf_held ) {
			out_z = z; /* a grounded probe's z = where it can jump off ( Point_pj ) */
			return v;
		}
		z_prev = z;
	}
	out_z = z_prev;
	return pf_held;
}

/* bounce probe: vz `speed` after StartGravity, pressing, one tick */
static int pf_bump( c_user_cmd* cmd, const c_vector& at, const c_vector& vel_xy, float fwd, float side, float pin, float dt, float& out_z,
                    int& odd, int& checked, float speed = k_pf_hb_speed )
{
	auto* local = g_ctx.m_local;
	pf_place( at, c_vector( vel_xy.m_x, vel_xy.m_y, speed - pin ) );
	cmd->m_forward_move = fwd;
	cmd->m_side_move    = side;
	g_prediction.begin( local, cmd );
	g_prediction.end( local );
	out_z          = local->get_origin( ).m_z;
	const float vz = local->get_velocity( ).m_z;
	pf_seed_check( at.m_z, out_z, vz, pin, dt, speed + pin, odd, checked );
	return pf_bump_verdict( at.m_z, out_z, vz, pin, speed * dt, ( local->get_flags( ) & fl_onground ) != 0,
	                        pf_move_type( local ) == move_type_walk );
}

/* falling edge probe: one tick from vz0, pressing. held = the TB ride's pin ( vz back to the pin, z kept ) */
static int pf_fall( c_user_cmd* cmd, const c_vector& at, float vz0, float fwd, float side, float pin, float step, float dt, int& odd, int& checked )
{
	auto* local = g_ctx.m_local;
	pf_place( at, c_vector( 0.f, 0.f, vz0 ) );
	cmd->m_forward_move = fwd;
	cmd->m_side_move    = side;
	g_prediction.begin( local, cmd );
	g_prediction.end( local );
	const float z  = local->get_origin( ).m_z;
	const float vz = local->get_velocity( ).m_z;
	pf_seed_check( at.m_z, z, vz, pin, dt, vz0 + 2.f * pin, odd, checked );
	return pf_hold_verdict( 0, at.m_z, at.m_z, z, vz, pin, step, ( local->get_flags( ) & fl_onground ) != 0, pf_move_type( local ) == move_type_walk );
}

/* head over the seam: half..one fall a tick ( headlab: 0.105..0.195 at 64, 0.027..0.047 at 128 ) = 64's offset x ( dt 64 )^2 */
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

/* live: the head probe's press is counted in float ulps */
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
	/* AirAccelerate from rest: accel x wish x dt a tick, dt again to distance ( headlab press_fwd ) */
	const float u = std::fabs( std::nextafter( c, c + sgn * 1000.f ) - c );
	move          = k_pf_head_press * u / ( pf_air_accel( ) * dt * dt ) / k_pf_duck_move;
	return pf_held;
}

/* head seam probe ( 5. ). how = the probe that held: 0 rest, 1 + i falling k_pf_edge_fall[ i ] */
static int pf_head_probe( c_user_cmd* cmd, const c_vector& face_p, const c_vector& n, float seam, float rot, float pin, float step, float dt,
                          float& hull, float& move, int& how, int& traces, int& odd, int& checked )
{
	how = 0;
	c_vector at( face_p.m_x, face_p.m_y, seam - k_pf_hull + pf_head_over( ) );
	if ( const int c = pf_contact( at, n, dt, hull, move, traces ); c != pf_held )
		return c;
	const auto clear = [ & ]( ) {
		++traces;
		return pf_clear( at );
	};
	const float fx = std::cos( rot ) * move, sx = -std::sin( rot ) * move;
	float got      = 0.f;
	const int o    = pf_hold( cmd, at, c_vector( 0.f, 0.f, 0.f ), fx, sx, pin, step, dt, got, odd, checked );
	if ( o == pf_held )
		return o;
	const float s = dt * 64.f;
	for ( int i = 0; i < 2; ++i ) {
		at.m_z = seam - k_pf_hull + k_pf_edge_fall[ i ][ 1 ] * s;
		if ( clear( ) && pf_fall( cmd, at, k_pf_edge_fall[ i ][ 0 ], fx, sx, pin, step, dt, odd, checked ) == pf_held ) {
			how = 1 + i;
			return pf_held;
		}
	}
	return o;
}

/* [pf tb] head dot label: which probe held ( pf_head_probe how ) and the top's height over the edge */
static std::string pf_edge_how( int how, float dt )
{
	const float s = dt * 64.f;
	if ( how == 0 )
		return std::format( "rest, head {:.4f} over", pf_head_over( ) );
	return std::format( "falling {:.0f}, head {:.4f} over", -k_pf_edge_fall[ how - 1 ][ 0 ], k_pf_edge_fall[ how - 1 ][ 1 ] * s );
}

static int pf_faces( const c_vector& col, const c_vector& n, float anchor_z, float lo, float hi, pf_face_t* out, int& traces )
{
	const auto depth = [ & ]( const c_vector& p ) { return p.m_x * n.m_x + p.m_y * n.m_y; };
	const int rows   = ( std::min )( static_cast< int >( hi - lo ) + 1, k_pf_face_rows );
	int count = 0, first_k = -1;
	/* stop 32 rows each way past the first face ( plain wall has no second ) */
	for ( int k = 0; k <= 2 * rows && count < k_pf_faces && ( first_k < 0 || k <= first_k + 64 ); ++k ) {
		const float off = ( k & 1 ) ? static_cast< float >( ( k + 1 ) / 2 ) : -static_cast< float >( k / 2 );
		const float z   = anchor_z + off;
		if ( z < lo - 0.5f || z > hi + 0.5f )
			continue;
		const c_vector at( col.m_x, col.m_y, z );
		trace_t tr;
		/* starts k_pf_face_reach out: the hull can't fit in front of anything nearer. a player there is not a face */
		ray_t ray( at + n * k_pf_face_reach, at - n * 16.f );
		n_tb::c_trace_filter_tb_world_props flt( g_ctx.m_local );
		g_interfaces.m_engine_trace->trace_ray( ray, mask_playersolid, &flt, &tr );
		++traces;
		const c_vector pn = tr.m_plane.m_normal;
		const float pn_xy = pn.length_2d( );
		if ( tr.m_start_solid || tr.m_fraction >= 1.f || pn_xy < 0.1f )
			continue;
		const bool disp = tr.surface.m_name && strstr( tr.surface.m_name, "displacement" );
		if ( ( pn.m_x * n.m_x + pn.m_y * n.m_y ) / pn_xy < ( disp ? 0.95f : 0.999f ) || std::fabs( pn.m_z ) > ( disp ? 0.35f : 0.05f ) )
			continue;
		const float gap = ( tr.m_end.dot_product( pn ) - tr.m_plane.m_distance ) / pn_xy; /* the end's own offset, flat */
		const c_vector p( tr.m_end.m_x - n.m_x * gap, tr.m_end.m_y - n.m_y * gap, 0.f );
		if ( count > 0 && std::fabs( depth( p ) - depth( out[ 0 ].p ) ) > k_pf_face_reach )
			continue;
		/* same PLANE, not "close": a lip 0.005 proud is its own face */
		bool known = false;
		for ( int i = 0; i < count; ++i )
			known |= std::fabs( depth( p ) - depth( out[ i ].p ) ) < ( disp || out[ i ].disp ? 1.f : 0.0005f );
		if ( !known ) {
			out[ count ].p    = p;
			out[ count ].trace_end = tr.m_end;
			out[ count ].disp = disp;
			out[ count ].prop = tr.surface.m_name && strstr( tr.surface.m_name, "**studio**" );
			if ( count++ == 0 )
				first_k = k;
		}
	}
	return count;
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

/* face depth along n at z ( on its plane, like pf_faces ). box = an axial world brush face. start solid ( a ceiling
   reaching past k_pf_face_reach ) / miss = false. tr_out = the trace ( what the face is made of ) */
static bool pf_depth( const c_vector& col, const c_vector& n, float z, float& out, bool& box, int& traces, trace_t* tr_out = nullptr )
{
	const c_vector at( col.m_x, col.m_y, z );
	trace_t tr;
	ray_t ray( at + n * k_pf_face_reach, at - n * 16.f );
	n_tb::c_trace_filter_tb_world_props flt( g_ctx.m_local ); /* a bot in front is not a face ( head seam gate ) */
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

/* what stopped the head: the hull traced up with the executors' filter ( props count, triggers don't ). only a face the
   hull touches ( <= 2 DIST_EPSILON off along its normal ): one further up did not stop it. false = nothing over it */
static bool pf_overhead( const c_vector& stop, trace_t& tr, int& traces )
{
	ray_t ray( stop, stop + c_vector( 0.f, 0.f, k_pf_over_reach ), pf_mins( ), pf_maxs( ) );
	n_tb::c_trace_filter_tb_world_props flt( g_ctx.m_local );
	g_interfaces.m_engine_trace->trace_ray( ray, mask_playersolid, &flt, &tr );
	++traces;
	return !tr.m_start_solid && tr.m_fraction < 1.f && tr.m_fraction * k_pf_over_reach * -tr.m_plane.m_normal.m_z <= 0.03125f;
}

// The backup restores the same release-time prediction frame for every pass.
// Keep our own copy: native history slots advance while this scan yields.
struct pf_bounce_seed_t {
	c_base_entity* local = nullptr;
	unsigned int identity = 0;
	n_pf_bounce::layout_t layout;
	n_pf_bounce::snapshot_t state;
	c_user_cmd cmd{ };
	bool valid = false;
	void capture( c_base_entity* player, const c_user_cmd& command ) {
		*this = { };
		if ( !player || !player->get_collideable( ) || !g_interfaces.m_move_helper || !g_interfaces.m_prediction ) return;
		layout.add( player->get_prediction_desc_map( ) );
		if ( layout.ranges.empty( ) ) return;
		n_pf_bounce::snapshot_t caller;
		caller.save( player, layout );
		g_prediction.restore_entity_to_predicted_frame( g_interfaces.m_prediction->m_commands_predicted - 1 );
		state.save( player, layout );
		caller.load( player, layout );
		local = player; identity = player->get_ref_ehandle( ); cmd = command; valid = true;
	}
};
static pf_bounce_seed_t s_pf_bounce_seed;

// One face's legacy ascent. Value-field snapshots let it yield after every
// prediction without leaving a hypothetical player in normal gameplay.
struct pf_bounce_backend_t {
	c_base_entity* local = g_ctx.m_local;
	n_pf_bounce::layout_t layout;
	n_pf_bounce::snapshot_t baseline, carried, witness, caller;
	n_pf_bounce::cursor_t cursor;
	c_user_cmd source{ }, trial{ }, witness_cmd{ };
	c_vector xy{ };
	float target = 0.f, interval = 0.f;
	int* traces = nullptr;
	bool valid = false;
	unsigned int identity = 0;

	pf_bounce_backend_t( const pf_bounce_seed_t& seed, const pf_face_t& face, const c_vector& n,
	                     float lo, float hi, float pin, float dt, int& trace_count, bool ascent = true )
		: source( seed.cmd ), target( pin ), interval( dt ), traces( &trace_count )
	{
		if ( !seed.valid || seed.local != local || !local || seed.identity != local->get_ref_ehandle( ) ||
		     !local->get_collideable( ) || !g_interfaces.m_move_helper || !g_interfaces.m_prediction ||
	     dt <= 0.f || !std::isfinite( dt ) || !std::isfinite( pin ) || !std::isfinite( lo ) || !std::isfinite( hi ) ||
	     source.m_command_number <= 0 || source.m_command_number > INT_MAX - 2049 ||
		     source.m_tick_count < 0 || source.m_tick_count > INT_MAX - 2049 ) return;
		layout = seed.layout;
		if ( layout.ranges.empty( ) || &pf_move_type( local ) == reinterpret_cast< unsigned char* >( local ) ) return;
		identity = local->get_ref_ehandle( );
		baseline = seed.state;
		if ( baseline.tick < 0 || baseline.tick > INT_MAX - 2049 ) return;
		carried = baseline;
		// Consistent network/abs XY. Backup wrote different component offsets
		// onto origin vs abs origin; keep both at the hull seat from the ray end.
		xy = pf_pj_hull_origin( face.trace_end, n, face.disp ? 16.001f : 15.97803f, 0.f );
		const float rotation = deg2rad( ( n * -1.f ).to_angle( ).m_y - source.m_view_point.m_y );
		source.m_forward_move = std::cos( rotation ) * 10.f;
		source.m_side_move = -std::sin( rotation ) * 10.f;
		source.m_up_move = 0.f;
		source.m_buttons = ( source.m_buttons & ( in_speed | in_walk | in_bullrush ) ) | in_jump | in_duck;
		source.m_weapon_select = source.m_weapon_sub_type = 0; source.m_impulse = 0;
		source.m_has_been_predicted = false;
		cursor.start( lo, hi, pin, dt, ascent ); valid = true;
	}
	n_pf_bounce::sample_t sample( ) const {
		return { local->get_origin( ), local->get_velocity( ), local->get_collideable( )->get_obb_maxs( ).m_z,
		         pf_move_type( local ) == move_type_walk, ( local->get_flags( ) & fl_onground ) != 0 };
	}
	void reset( ) { baseline.load( local, layout ); pf_move_type( local ) = move_type_walk; carried.save( local, layout ); }
	void resume( ) { carried.load( local, layout ); }
	bool place( float z ) {
		const c_vector p( xy.m_x, xy.m_y, z );
		trace_t check{ };
		ray_t ray( p, p, local->get_collideable( )->get_obb_mins( ), local->get_collideable( )->get_obb_maxs( ) );
		n_tb::c_trace_filter_tb_world_props filter( local );
		g_interfaces.m_engine_trace->trace_ray( ray, mask_playersolid, &filter, &check ); ++*traces;
		if ( check.m_start_solid || check.m_all_solid || pf_void( p ) ) return false;
		local->get_origin( ) = p; local->set_abs_origin( p );
		const c_vector head( p.m_x, p.m_y, p.m_z + 54.f );
		trace_t down{ }, up{ };
		n_tb::c_trace_filter_tb_world_props down_filter( local ), up_filter( local );
		ray_t down_ray( head, c_vector( p.m_x, p.m_y, p.m_z - 1000.f ) );
		ray_t up_ray( head, c_vector( p.m_x, p.m_y, p.m_z + 1000.f ) );
		g_interfaces.m_engine_trace->trace_ray( down_ray, mask_playersolid, &down_filter, &down ); ++*traces;
		g_interfaces.m_engine_trace->trace_ray( up_ray, mask_playersolid, &up_filter, &up ); ++*traces;
		if ( p.m_z + 1.f < down.m_end.m_z || p.m_z + 1.f > up.m_end.m_z ) return false;
		// Retain the carried velocity, ground state and native duck/jump history.
		return true;
	}
	void save_before( ) {
		witness.save( local, layout ); trial = source;
		const int step = local->get_tick_base( ) - baseline.tick;
		trial.m_command_number += step; trial.m_tick_count += step;
		witness_cmd = trial;
	}
	n_pf_bounce::sample_t predict( ) {
		g_prediction.begin( local, &trial, true ); g_prediction.end( local );
		const auto result = sample( ); ++local->get_tick_base( ); return result;
	}
	void save_after( ) { carried.save( local, layout ); }
	n_pf_bounce::sample_t probe( float head ) {
		witness.load( local, layout ); trial = witness_cmd;
		c_vector p = local->get_origin( ); p.m_z = head - 54.f;
		local->get_origin( ) = p; local->set_abs_origin( p );
		return predict( );
	}
};
static std::unique_ptr< pf_bounce_backend_t > s_pf_bounce;
static std::unique_ptr< pf_bounce_backend_t > s_pf_surf;

/* head seam ( 5. ): the hull top presses the face just over the seam, so that must be this unit's own box face */
static bool pf_head_face( const c_vector& face_p, const c_vector& n, float seam, int& traces )
{
	float d  = 0.f;
	bool box = false;
	return pf_depth( face_p, n, seam + 0.05f, d, box, traces ) && box && std::fabs( d - ( face_p.m_x * n.m_x + face_p.m_y * n.m_y ) ) < 0.01f;
}

/* rising edge ( 6. ): the hull BOTTOM presses the face just under the seam, so that must be this unit's own box face ( pf_head_face
   reads 0.05 over its seam: 0.05 under this one ). over it anything: flush seams pin too ( riserecall: 292 mirage spots ) */
static bool pf_rise_face( const c_vector& face_p, const c_vector& n, float seam, int& traces ) { return pf_head_face( face_p, n, seam - 0.1f, traces ); }

static int pf_rise_probe( c_user_cmd* cmd, const c_vector& face_p, const c_vector& n, float seam, float rot, float pin, float dt,
                          float& hull, float& move, int& how, int& traces, int& odd, int& checked )
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
		const int v = pf_clear( at ) ? pf_bump( cmd, at, c_vector( 0.f, 0.f, 0.f ), fx, sx, pin, dt, got, odd, checked, vz ) : pf_stuck;
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

static struct {
	bool busy   = false;
	int stage   = 0;           /* 0 units, 1 tick pixeljumps, 2 report */
	int col     = 0, face = 0; /* stage 0 cursor */
	int phase   = 0;           /* unit: 0 enter, 1 seam grid, 2 sweep, 3 bounce grid, 4 head seam grid, 5 rising edge grid, 6 log */
	bool col_in = false;
	int buttons = 0, chunks = 0, sims = 0, traces = 0, arm_fails = 0;
	float lo = 0.f, hi = 0.f; /* drag + margin: FEET range for surf, HEAD range for bounce */
	int cols = 0, cols_run = 0, tb_seams = 0, tb_unverified = 0, tb_edges = 0, tb_rises = 0;
	int col_ok = 0; /* bit c = band column c re-traced onto the wall ( pf_cell ) */
	pf_face_t faces[ k_pf_faces ]{ };
	int n_faces = 0;
	c_vector base{ };              /* this unit's hull x/y */
	c_vector sw_pos{ }, sw_vel{ }; /* sweep probe, carried tick to tick */
	int sw_hold = 0, sw_ticks = 0, u_crawls = 0;
	int u_pj = 0, u_pj_lip = 0; /* this unit's pixel jump traces, and how many stood on something */
	int u_skin = 0, u_skin_rows = 0; /* this unit's skin lip dots, and surface changes between grid rows it tested */
	bool row_live = false;           /* the last grid row ( row_z ) hit this face, made of row_name on row_ent ( row_box: static prop ) */
	float row_z = 0.f;
	const char* row_name = nullptr;
	const void* row_ent  = nullptr;
	int row_box          = 0;
	float sw_dvz = 0.f;                    /* last tick's vz off the free value ( pf_touched ) */
	bool sw_live = false, sw_seek = false; /* seek = looking for a clear start, resumes at sw_pos.m_z */
	bool cr_live = false;                  /* crawl: probes from cr_z down to cr_end, one step apart */
	float cr_z = 0.f, cr_end = 0.f;
	int cr_n = 0;
	float gz = 0.f;    /* grid cursor: seam ( surf, head seam ) or head ( bounce ) */
	bool sub_live = false;                   /* grid sub-steps: the marker sub_z's cell [ sub_lo, sub_hi ), pf_sub_next cursor sub_k */
	float sub_z = 0.f, sub_lo = 0.f, sub_hi = 0.f;
	int sub_k = 0, sub_probes = 0, sub_dots = 0;
	float tight = 0.f, tight_q = 0.f; /* this unit's tight gap ( 0 = none ) and how proud the face is */
	float tight_off_used = 0.f;       /* last offset a tight hull fit at ( k_pf_tight_prop / k_pf_tight_gap ), 0 = none */
	bool tight_live = false;          /* the tight probe for marker sub_z is the next step */
	int tight_probes = 0, tight_dots = 0, tight_off = 0, tight_stuck = 0;
	std::string trail; /* this unit's sweep events */
	int u_ps[ k_pf_outcomes ]{ }, u_hb[ k_pf_outcomes ]{ }, u_cr[ k_pf_outcomes ]{ }, u_ht[ k_pf_outcomes ]{ }, u_rt[ k_pf_outcomes ]{ }; /* this unit's */
	int ps[ k_pf_outcomes ]{ }, hb[ k_pf_outcomes ]{ }, cr[ k_pf_outcomes ]{ }, ht[ k_pf_outcomes ]{ }, rt[ k_pf_outcomes ]{ };           /* the scan's */
	int sw_all = 0, sw_lands = 0, sw_pins = 0, sw_dots = 0, sw_crawls = 0, sw_rebases = 0, odd = 0, checked = 0;
	long long used_us = 0, next_qpc = 0, worst_step_us = 0, worst_chunk_us = 0;
	bool pj_inited = false, pj_done = false, pj_yield = false;
	int pj_i = 0, pj_mode = 0, pj_rest_t = 0;
	float pj_zref = 0.f, pj_max = 0.f, pj_lerp = 0.f;
	c_vector pj_hull{ }, pj_wall{ }, pj_catch{ }, pj_pos{ }, pj_vel{ };
} s_pf_scan;

/* a surf the player really rode ( route calc [rc surf] CAUGHT ), probed by the finder next cmd between scans */
static struct {
	bool pending = false;
	c_vector pos{ }, n{ }; /* hull origin where it held, the wall it held on */
	float slid = 0.f;      /* the player's own along-wall speed ( sign = BSP side it tested first ) */
	bool ducked = true;    /* standing catch: head 72 over the feet, not 54 */
} s_pf_catch;

void pf_check_catch( const c_vector& pos, const c_vector& wall_n, float slid, bool ducked )
{
	s_pf_catch.pending = true;
	s_pf_catch.pos     = pos;
	s_pf_catch.n       = wall_n;
	s_pf_catch.slid    = slid;
	s_pf_catch.ducked  = ducked;
}

/* [pf check]: finder dot there?, where the last scan's band was, and the grid probe AT the spot still / sliding each way.
   dot + probe H = found. no dot + probe H = the band missed the spot. probe P every way = the finder can't see it */
static void pf_catch_check( c_user_cmd* cmd )
{
	s_pf_catch.pending      = false;
	const c_vector pos      = s_pf_catch.pos;
	const c_vector n        = c_vector( s_pf_catch.n.m_x, s_pf_catch.n.m_y, 0.f ).normalized( );
	const c_vector along    = c_vector( n.m_y, -n.m_x, 0.f );
	const float support     = 16.f * ( std::fabs( n.m_x ) + std::fabs( n.m_y ) );
	const c_vector face_pt  = pos - n * support; /* dots sit on the face plane */
	const float seam        = std::floor( pos.m_z );
	const float z           = pf_marker( seam );
	const float catch_hull  = s_pf_catch.ducked ? k_pf_hull : 72.f;
	const float head_seam   = std::floor( pos.m_z + catch_hull );
	const float head_over   = pos.m_z + catch_hull - head_seam;

	std::string dot = "none";
	for ( const c_vector& q : Points )
		if ( std::fabs( q.m_z - pos.m_z ) < 0.05f && ( q - face_pt ).length_2d( ) < 40.f ) {
			dot = std::format( "z {:.4f} {:.1f}u off", q.m_z, ( q - face_pt ).length_2d( ) );
			break;
		}
	for ( const c_vector& q : Point_tb_head )
		if ( dot == "none" && std::fabs( q.m_z - head_seam ) < 0.05f && ( q - face_pt ).length_2d( ) < 40.f )
			dot = std::format( "head tb seam {:.0f} {:.1f}u off", q.m_z, ( q - face_pt ).length_2d( ) );
	std::string scan = "none";
	if ( s_pf_scan.hi > s_pf_scan.lo ) {
		const c_vector off = face_pt - StartPos;
		scan = std::format( "along {:+.1f} depth {:+.2f} z {:.2f}..{:.2f} n.n {:.2f}", off.dot_product( along ), off.dot_product( n ), s_pf_scan.lo,
		                    s_pf_scan.hi, n.dot_product( c_vector( WallNormal.m_x, WallNormal.m_y, 0.f ).normalized( ) ) );
	}

	/* a seam pin holds within DIST_EPSILON over its unit, a head seam pin with the head < 0.25 over one ( 5., axial ): the
	   head probe. anything else ( air stuck, crease ) gets no grid probe */
	const bool on_grid = pos.m_z - seam <= 0.035f;
	const bool on_head = !on_grid && head_over < 0.25f && ( std::fabs( n.m_x ) > 0.9999f || std::fabs( n.m_y ) > 0.9999f );
	std::string probes = " | off the unit grid, no probe";
	if ( on_grid || on_head ) {
		const int buttons     = cmd->m_buttons;
		const float fwd0      = cmd->m_forward_move;
		const float side0     = cmd->m_side_move;
		const float pin       = g_prediction.get_engine_target_predict_z_velocity( );
		const float dt        = n_tick::engine_interval( );
		const float step      = -2.f * pin * dt;
		const c_vector into   = n * -1.f;
		const float rot       = deg2rad( into.to_angle( ).m_y - cmd->m_view_point.m_y );
		const float fwd       = std::cos( rot ) * pf_press( dt );
		const float side      = -std::sin( rot ) * pf_press( dt );
		pf_face_t faces[ k_pf_faces ]{ };
		int traces = 0, odd = 0, checked = 0;
		const float fz    = on_grid ? seam : head_seam;
		const int n_faces = pf_faces( face_pt, n, fz + 1.f, fz - 2.f, fz + 2.f, faces, traces );
		probes = on_grid ? std::format( " | probe {:.4f}:", z ) : std::format( " | head {:.4f} over seam {:.0f}, head probe:", head_over, head_seam );
		g_prediction.restore_entity_to_predicted_frame( g_interfaces.m_prediction->m_commands_predicted - 1 );
		if ( n_faces == 0 )
			probes += " no face at the spot";
		else if ( !pf_arm( cmd, pos + n * 40.f, buttons ) )
			probes += " duck refused";
		else {
			cmd->m_buttons = ( buttons & ~in_jump ) | in_duck;
			for ( int i = 0; i < n_faces; ++i ) {
				if ( on_head ) {
					float hull = 0.f, move = 0.f;
					int how     = 0;
					const int o = pf_head_probe( cmd, faces[ i ].p, n, head_seam, rot, pin, step, dt, hull, move, how, traces, odd, checked );
					probes += std::format( " face {} {:+.4f} {}{} hull {:.7f}{}", i, ( faces[ i ].p - face_pt ).dot_product( n ), k_pf_why[ o ],
					                       o == pf_held ? " ( " + pf_edge_how( how, dt ) + " )" : std::string( ), hull,
					                       pf_head_face( faces[ i ].p, n, head_seam, traces ) ? "" : " ( no own face over, scan skips )" );
					continue;
				}
				const float gap = faces[ i ].disp ? k_pf_gap_disp : k_pf_gap_brush;
				const c_vector b = faces[ i ].p + n * ( support + gap );
				const c_vector at( b.m_x, b.m_y, z );
				probes += std::format( " face {} {:+.4f}", i, ( faces[ i ].p - face_pt ).dot_product( n ) );
				if ( !pf_clear( at ) )
					probes += " X";
				else if ( pf_void( at ) )
					probes += " V";
				else
					for ( const float a : { 0.f, k_pf_slide, -k_pf_slide } ) {
						float got                       = 0.f;
						g_prediction.m_bounds_max_speed = a != 0.f ? k_pf_bounds_slide : 0.f; /* the scan's hold_slid */
						const int o = pf_hold( cmd, at, into * k_pf_press + along * a, fwd, side, pin, step, dt, got, odd, checked );
						g_prediction.m_bounds_max_speed = 0.f;
						probes += std::format( " {:+.0f}{}", a, k_pf_why[ o ] );
					}
			}
			/* replay: the exact spot, the player's own slide, a held press ( ducked ). H here but P above = the finder's probe
			   set can't make this catch */
			/* ducked hull at a standing catch's feet is another spot: no replay */
			probes += s_pf_catch.ducked ? std::format( " | replay ducked slide {:+.0f}:", s_pf_catch.slid ) : std::string( " | standing catch, no ducked replay" );
			for ( const float m : { 85.f, 250.f } ) { /* last move's m_flMaxSpeed: own ducked knife, standing / a bot's */
				if ( !s_pf_catch.ducked )
					break;
				float got                       = 0.f;
				g_prediction.m_bounds_max_speed = m;
				const int o = pf_clear( pos ) ? pf_hold( cmd, pos, into * ( k_pf_air_accel * 30.f * dt ) + along * s_pf_catch.slid, std::cos( rot ) * k_pf_move_hard,
				                                         -std::sin( rot ) * k_pf_move_hard, pin, step, dt, got, odd, checked )
				                              : pf_stuck;
				g_prediction.m_bounds_max_speed = 0.f;
				probes += std::format( " max {:.0f} {}", m, k_pf_why[ o ] );
			}
		}
		g_prediction.restore_entity_to_predicted_frame( g_interfaces.m_prediction->m_commands_predicted - 1 );
		cmd->m_buttons      = buttons;
		cmd->m_forward_move = fwd0;
		cmd->m_side_move    = side0;
	}
	botox_dbg_log( "[pf check] real surf z %.4f at %.2f %.2f n %.2f %.2f slid %+.0f | finder dot %s | last scan %s%s", pos.m_z, pos.m_x, pos.m_y,
	               n.m_x, n.m_y, s_pf_catch.slid, dot.c_str( ), scan.c_str( ), probes.c_str( ) );
}

struct pf_trick_t {
	const char* kind;
	c_vector pos;
	bool ducked;
};
static std::vector< pf_trick_t > s_pf_tricks;
constexpr size_t k_pf_tricks = 64;
/* a catch's top can sit up to one tick's drop over the edge it pinned on ( 5. ): a dot this near is that catch's */
constexpr float k_pf_trick_dz = 2.5f;
constexpr float k_pf_trick_rise = 4.f;
constexpr float k_pf_trick_hb_reach = 26.f;

/* nearest dot 48u across, z off the feet or the head, whichever is nearer: " | tb z .. ( head -0.159 ) 3.1u off" / " | tb none".
   dz = that offset ( NAN = none within reach ) */
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

/* none = missed, another kind = mislabel. found = a dot of its own kind within k_pf_trick_dz ( head_surf rides a ceiling: either ) */
static std::string pf_trick_dots( const pf_trick_t& t, bool& found )
{
	const float head = t.pos.m_z + ( t.ducked ? k_pf_hull : 72.f );
	float tb = NAN, hb = NAN, ps = NAN, pj = NAN;
	std::string out = pf_trick_dot( Point_tb, "tb", t.pos, head, 4.f, tb ) + pf_trick_dot( Point_bounce, "hb", t.pos, head, k_pf_trick_hb_reach, hb ) +
	                  pf_trick_dot( Points, "ps", t.pos, head, 4.f, ps ) + pf_trick_dot( Point_pj, "pj", t.pos, head, 4.f, pj );
	const auto within = [ ]( float dz ) { return std::fabs( dz ) <= k_pf_trick_dz; }; /* NAN reads false */
	const bool hb_ok  = hb >= -k_pf_trick_dz && hb <= k_pf_trick_hb_reach;   /* at or over the head, the chain's rise */
	/* or a rising edge's dot over the FEET ( 6. ): the nearest-of-feet-or-head dz can't say which */
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
	const std::string scan = s_pf_scan.hi > s_pf_scan.lo ? std::format( "last scan anchor {:.0f}u off, z {:.1f}..{:.1f}", ( StartPos - pos ).length_2d( ),
	                                                                     s_pf_scan.lo, s_pf_scan.hi )
	                                                     : std::string( "no scan" );
	botox_dbg_log( "[pf trick] %s at %.2f %.2f feet %.3f head %.3f duck %d | %s%s", kind, pos.m_x, pos.m_y, pos.m_z, pos.m_z + ( ducked ? k_pf_hull : 72.f ),
	               ducked ? 1 : 0, scan.c_str( ), dots.c_str( ) );
}

/* scan done: every kept catch inside this band ( along the wall within the band + margin, feet or head in the drag ) against the
   new dots. the answer to "doesn't find X" when the trick came first */
static void pf_recheck_tricks( )
{
	const c_vector n     = c_vector( WallNormal.m_x, WallNormal.m_y, 0.f ).normalized( );
	const c_vector along = c_vector( n.m_y, -n.m_x, 0.f );
	int in_band = 0, found_n = 0;
	for ( const pf_trick_t& t : s_pf_tricks ) {
		const c_vector off = t.pos - StartPos;
		const float head   = t.pos.m_z + ( t.ducked ? k_pf_hull : 72.f );
		const bool z_in    = ( t.pos.m_z >= s_pf_scan.lo - 1.f && t.pos.m_z <= s_pf_scan.hi + 1.f ) || ( head >= s_pf_scan.lo - 1.f && head <= s_pf_scan.hi + 1.f );
		if ( std::fabs( off.dot_product( along ) ) > 16.f + k_pf_margin || off.dot_product( n ) < 0.f || off.dot_product( n ) > 48.f || !z_in )
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

/* map change: clear dots + animated_points too ( route_calc / pixel_calc snap to them ) */
void n_movement::impl_t::on_level_init( )
{
	StartPos = EndPos = c_vector( 0.f, 0.f, 0.f );
	s_pf_scan         = { };
	s_pf_bounce.reset( );
	s_pf_surf.reset( );
	s_pf_bounce_seed = { };
	s_pf_catch        = { };
	s_pf_tricks.clear( );
	s_pf_anchored     = false;
	Points.clear( );
	s_pf_tb_scratch.clear( );
	Point_bounce.clear( );
	Point_bounce_ceil.clear( );
	Point_bounce_windows.clear( );
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
	/* one guard for create_move + paint_traverse. off = CLEAR dots ( route_calc / pixel_calc snap to them ) */
	if ( !GET_VARIABLE( g_variables.m_pixel_finder, bool ) ) {
		s_pf_scan.busy = false;
		s_pf_bounce.reset( );
		s_pf_surf.reset( );
		s_pf_bounce_seed = { };
		s_pf_catch.pending = false;
		s_pf_anchored      = false;
		if ( !Points.empty( ) || !Point_bounce.empty( ) || !Point_tb.empty( ) || !Point_pj.empty( ) || !s_pf_tb_scratch.empty( ) ) {
			Points.clear( );
			s_pf_tb_scratch.clear( );
			Point_bounce.clear( );
			Point_bounce_ceil.clear( );
			Point_bounce_windows.clear( );
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
		/* press: anchor on the wall under the crosshair. A new column outranks a running scan */
		s_pf_scan.busy = false;
		s_pf_bounce.reset( );
		s_pf_surf.reset( );
		s_pf_bounce_seed = { };
		s_pf_anchored  = false;
		Points.clear( );
		s_pf_tb_scratch.clear( );
		Point_bounce.clear( );
		Point_bounce_ceil.clear( );
		Point_bounce_windows.clear( );
		Point_tb.clear( );
		Point_tb_head.clear( );
		Point_tb_rise.clear( );
		Point_pj.clear( );
		const c_vector end = eye + dir * 6000.f;
		trace_t trace      = { };
		ray_t ray( eye, end );
		n_tb::c_trace_filter_tb_world_props flt( g_ctx.m_local ); /* a bot in front of the wall is not the wall */
		g_interfaces.m_engine_trace->trace_ray( ray, mask_playersolid, &flt, &trace );
		ps_finder_unclip( eye, end, trace );
		/* floor / ceiling / sky / start solid = no wall normal, every probe keys on one */
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
		/* drag end = where the aim crosses the WALL'S PLANE */
		const float denom = dir.dot_product( WallNormal );
		if ( denom < -0.01f ) {
			const float t = ( StartPos - eye ).dot_product( WallNormal ) / denom;
			EndPos = c_vector( StartPos.m_x, StartPos.m_y,
			                   std::clamp( eye.m_z + dir.m_z * t, StartPos.m_z - k_pf_max_span, StartPos.m_z + k_pf_max_span ) );
		}
	} else if ( !held && held_prev && s_pf_anchored ) {
		/* release: start the scan, runs in the cursor block below from this cmd */
		s_pf_anchored = false;
		auto& s       = s_pf_scan;
		s             = { };
		s.busy        = true;
		s.buttons     = cmd->m_buttons; /* frozen: a key tapped mid-scan must not change half the column */
		s.lo          = ( std::min )( StartPos.m_z, EndPos.m_z ) - k_pf_margin;
		s.hi          = ( std::max )( StartPos.m_z, EndPos.m_z ) + k_pf_margin;
		if ( GET_VARIABLE( g_variables.m_pixel_finder_headbounces, bool ) ||
		     GET_VARIABLE( g_variables.m_pixel_finder_pixelsurfs, bool ) )
			s_pf_bounce_seed.capture( g_ctx.m_local, *cmd );
		/* surf, bounce and step seams come from the stage 0 units. pixeljumps are the vel_z scan */
		const bool units_on = GET_VARIABLE( g_variables.m_pixel_finder_pixelsurfs, bool ) ||
		                      GET_VARIABLE( g_variables.m_pixel_finder_headbounces, bool ) ||
		                      GET_VARIABLE( g_variables.m_pixel_finder_texturebugs, bool );
		s.cols    = units_on ? std::clamp( k_pf_sim_cap / pf_col_sims( s.hi - s.lo ), 1, 9 ) : 0;
		for ( int c = 0; c < s.cols; ++c ) {
			c_vector unused;
			s.col_ok |= pf_band_column( StartPos, WallNormal, k_pf_band_lat[ c ], unused ) ? 1 << c : 0;
		}
		const char* const map = g_interfaces.m_engine_client->get_level_name_short( );
		botox_dbg_log( "[pf] scan feet/head z %.2f..%.2f ( bounce heads to %.2f ) n %.4f %.4f %.4f at %.2f %.2f %.2f | %d band column(s) mask %03x, pin %.4f | map %s, aim hit %s",
		               s.lo, s.hi, s.hi - k_pf_margin, WallNormal.m_x, WallNormal.m_y, WallNormal.m_z, StartPos.m_x, StartPos.m_y, StartPos.m_z, s.cols, s.col_ok,
		               g_prediction.get_engine_target_predict_z_velocity( ), map ? map : "?", s_pf_anchor_hit.c_str( ) );
	}
	held_prev = held;

	/* a running scan owns the entity: the catch waits for it */
	if ( s_pf_catch.pending && !s_pf_scan.busy && g_ctx.m_local->is_alive( ) )
		pf_catch_check( cmd );

	/* cooldown 2x this chunk's cost: a slow cmd spawns catch-up cmds, each running another unit = spiral */
	if ( !s_pf_scan.busy || n_tick::qpc_now( ) < s_pf_scan.next_qpc )
		return;
	/* died mid-scan: the probes would force the walk mover onto an observer */
	if ( !g_ctx.m_local->is_alive( ) ) {
		s_pf_scan.busy = false;
		s_pf_bounce.reset( );
		s_pf_surf.reset( );
		return;
	}

	PERF_ZONE( zone_cmd_pixel_finder );
	auto& s                        = s_pf_scan;
	/* surf off still runs grid + sweep when pixel jumps want them ( their lips come off those probes ) */
	const bool ps_on               = GET_VARIABLE( g_variables.m_pixel_finder_pixelsurfs, bool );
	const bool hb_on               = GET_VARIABLE( g_variables.m_pixel_finder_headbounces, bool );
	const bool pj_want             = GET_VARIABLE( g_variables.m_pixel_finder_pixeljumps, bool );
	const bool pj_on               = false; /* botox DIST_EPSILON lips; Mori PJs are the vel_z hundredths scan */
	const bool tb_on               = GET_VARIABLE( g_variables.m_pixel_finder_texturebugs, bool );
	auto* const local              = g_ctx.m_local;
	const c_user_cmd live_cmd      = *cmd;
	const c_user_cmd live_last     = local->get_last_command( );
	c_user_cmd* const live_current = *local->get_current_command( );
	const int live_tick            = local->get_tick_base( );
	bool bounce_ran                = false;
	bool surf_ran                  = false;
	const int live_buttons         = cmd->m_buttons;
	const float live_fwd           = cmd->m_forward_move;
	const float live_side          = cmd->m_side_move;
	const unsigned int sims_before = g_prediction.m_sim_count;
	/* ENGINE dt, not n_tick: probes run the real mover ( "128 tick fix" would give an unreadable pin ) */
	const float pin          = g_prediction.get_engine_target_predict_z_velocity( );
	const float dt           = n_tick::engine_interval( );
	const float step         = -2.f * pin * dt; /* one pinned tick's attempted drop, g * dt^2 */
	const int sweep_cap     = static_cast< int >( k_pf_sweep_ticks / ( 64.f * dt ) ); /* the same fall height at any tickrate */
	const c_vector n         = c_vector( WallNormal.m_x, WallNormal.m_y, 0.f ).normalized( );
	const c_vector along     = c_vector( n.m_y, -n.m_x, 0.f ); /* the wall's own horizontal */
	const c_vector into      = n * -1.f;
	const c_vector push      = into * k_pf_press;
	const c_vector push_hard = into * k_pf_press_hard;
	const float rot          = deg2rad( into.to_angle( ).m_y - cmd->m_view_point.m_y );
	const float fwd          = std::cos( rot ) * pf_press( dt );
	const float side         = -std::sin( rot ) * pf_press( dt );
	const float fwd_hard     = std::cos( rot ) * k_pf_move_hard;
	const float side_hard    = -std::sin( rot ) * k_pf_move_hard;
	const float support      = 16.f * ( std::fabs( n.m_x ) + std::fabs( n.m_y ) ); /* hull half-width along n */
	bool armed = false, arm_ok = false;
	const auto arm = [ & ]( ) {
		if ( armed )
			return arm_ok;
		armed = true;
		g_prediction.restore_entity_to_predicted_frame( g_interfaces.m_prediction->m_commands_predicted - 1 );
		arm_ok         = pf_arm( cmd, StartPos + n * 40.f, s.buttons );
		s.arm_fails    = arm_ok ? 0 : s.arm_fails + 1;
		cmd->m_buttons = ( s.buttons & ~in_jump ) | in_duck;
		return arm_ok;
	};
	/* before any sim: 1 go, 0 wait, -1 gave up ( standing probes would mix 72-tall dots with 54 ) */
	const auto ready = [ & ]( ) {
		if ( arm( ) )
			return 1;
		if ( s.arm_fails <= k_pf_arm_tries )
			return 0;
		movement_add_window( 3.f, "pixel finder: crouch kept failing, surf/bounce skipped" );
		botox_dbg_log( "[pf] duck refused %d chunks in a row, surf/bounce skipped", s.arm_fails );
		s.col    = s.cols;
		s.col_in = false;
		s.phase  = 0;
		return -1;
	};
	const auto at       = [ & ]( float z ) { return c_vector( s.base.m_x, s.base.m_y, z ); };
	const auto clear_at = [ & ]( float z ) {
		++s.traces;
		return pf_clear( at( z ) );
	};
	/* pf_hold still, then sliding each way while unpinned ( see 1. above ). slid = the slide that ran last */
	const auto hold_slid = [ & ]( const c_vector& p, const c_vector& press, float f, float sd, float& got, float& slid ) {
		slid  = 0.f;
		int o = pf_hold( cmd, p, press, f, sd, pin, step, dt, got, s.odd, s.checked );
		for ( const float a : { k_pf_slide, -k_pf_slide } ) {
			if ( o != pf_unpinned )
				break;
			slid                            = a;
			g_prediction.m_bounds_max_speed = k_pf_bounds_slide;
			o                               = pf_hold( cmd, p, press + along * a, f, sd, pin, step, dt, got, s.odd, s.checked );
			g_prediction.m_bounds_max_speed = 0.f;
		}
		return o;
	};
	/* sweep (re)start at or under z; seek finds the first clear in-map height */
	const auto sweep_from = [ & ]( float z ) {
		s.sw_live = true;
		s.sw_seek = true;
		s.sw_pos  = at( z );
		s.sw_vel  = push_hard + c_vector( 0.f, 0.f, -k_pf_fall_speed );
		s.sw_hold = 0;
		s.sw_dvz  = 0.f;
	};
	/* a dot within tol in z and 40u across ( one column band ) */
	const auto find_dot = []( std::vector< c_vector >& dots, const c_vector& p, float z, float tol ) {
		return std::find_if( dots.begin( ), dots.end( ), [ & ]( const c_vector& q ) {
			return std::fabs( q.m_z - z ) < tol && ( q - c_vector( p.m_x, p.m_y, z ) ).length_2d( ) < 40.f;
		} );
	};
	const auto near_dot = [ & ]( std::vector< c_vector >& dots, const c_vector& p, float z, float tol ) {
		return find_dot( dots, p, z, tol ) != dots.end( );
	};
	const bool tb_axial      = std::fabs( n.m_x ) > 0.9999f || std::fabs( n.m_y ) > 0.9999f;
	const auto add_seam_tb   = [ & ]( const c_vector& fp, float got, bool disp ) {
		if ( !pf_is_marker( got ) )
			return;
		if ( disp || !tb_axial ) {
			++s.tb_unverified;
			return;
		}
		if ( near_dot( Point_tb, fp, std::floor( got ), 0.05f ) )
			return;
		Point_tb.emplace_back( fp.m_x, fp.m_y, std::floor( got ) );
		++s.tb_seams;
	};
	const auto note = [ & ]( const std::string& cell ) {
		if ( s.trail.size( ) < 240 )
			s.trail += cell;
	};
	/* worst step = the overrun ( budget checked between steps only ) */
	long long t_step     = n_tick::qpc_now( );
	const auto step_done = [ & ]( ) {
		const long long now = n_tick::qpc_now( );
		s.worst_step_us     = ( std::max )( s.worst_step_us, ( now - t_step ) * 1000000ll / n_tick::qpc_freq( ) );
		t_step              = now;
	};

	n_tick::c_sim_budget budget;
	budget.start( k_pf_share, 0.f, n_tick::search_pf );
	const auto pj_step = [ & ]( ) -> bool {
		if ( !pj_want || std::fabs( WallNormal.m_z ) >= 0.72f ) {
			s.pj_done = true;
			return false;
		}
		if ( !s.pj_inited ) {
			s.pj_inited = true;
			s.pj_zref   = ( StartPos.m_z > EndPos.m_z ) ? StartPos.m_z : EndPos.m_z;
			s.pj_max    = std::fabs( StartPos.m_z - EndPos.m_z );
			if ( s.pj_max < 1.f )
				s.pj_max = 1.f;
			s.pj_lerp = 0.f;
		}
		if ( s.pj_lerp > s.pj_max || Point_pj.size( ) >= static_cast< std::size_t >( k_pf_pj_max ) ) {
			botox_dbg_log( "[pf pj] vel_z scan lerp %.2f / %.2f zref %.4f -> %d pixeljump(s)", s.pj_lerp, s.pj_max, s.pj_zref,
			               static_cast< int >( Point_pj.size( ) ) );
			s.pj_done = true;
			return false;
		}
		const float z = ( s.pj_zref < 0.f ) ? ( s.pj_zref - 0.972092f - s.pj_lerp ) : ( s.pj_zref + 0.0287018f - s.pj_lerp );
		s.pj_lerp += 0.01f;

		float test_al = 15.97803f;
		if ( s_pf_anchor_hit.find( "displacement" ) != std::string::npos )
			test_al = 16.001f;

		cmd->m_buttons      = ( s.buttons | in_jump | in_duck );
		c_vector angles{ WallNormal.m_x * -1.f, WallNormal.m_y * -1.f, 0.f };
		const float rotation = deg2rad( angles.to_angle( ).m_y - cmd->m_view_point.m_y );
		cmd->m_forward_move  = std::cos( rotation ) * 10.f;
		cmd->m_side_move     = -std::sin( rotation ) * 10.f;
		pf_pj_set_origin( StartPos, WallNormal, test_al, z );

		const c_vector o = local->get_origin( );
		trace_t down{ }, up{ };
		c_trace_filter flt( local );
		g_interfaces.m_engine_trace->trace_ray( ray_t( c_vector( o.m_x, o.m_y, o.m_z + 54.f ), c_vector( o.m_x, o.m_y, o.m_z - 1000.f ) ),
		                                        mask_playersolid, &flt, &down );
		g_interfaces.m_engine_trace->trace_ray( ray_t( c_vector( o.m_x, o.m_y, o.m_z + 54.f ), c_vector( o.m_x, o.m_y, o.m_z + 1000.f ) ),
		                                        mask_playersolid, &flt, &up );
		s.traces += 2;
		if ( o.m_z + 1.f < down.m_end.m_z || o.m_z + 1.f > up.m_end.m_z )
			return true;

		g_prediction.begin( local, cmd );
		g_prediction.end( local );
		const float vel_z = local->get_velocity( ).m_z;
		if ( std::fabs( vel_z ) < 0.5f ) {
			const c_vector pt = pf_pj_wall_point( local->get_origin( ), WallNormal, test_al );
			if ( pf_pj_accept( pt, n, pt.m_z, &s.traces ) ) {
				botox_dbg_log( "[pf pj] vel_z dot z %.4f ( probe %.4f vz %.3f )", pt.m_z, z, vel_z );
				s.pj_lerp += 0.07f;
			}
		}
		return true;
	};
	for ( ; s.busy && !budget.expired( ); step_done( ) ) {
		if ( !s.pj_done && !s.pj_yield ) {
			int n = 0;
			while ( n < 4 && !s.pj_done && !budget.expired( ) ) {
				if ( !pj_step( ) )
					break;
				++n;
			}
			s.pj_yield = ( s.stage == 0 && s.col < s.cols );
			continue;
		}
		s.pj_yield = false;
		if ( s.stage == 0 ) {
			if ( s.col >= s.cols ) {
				if ( s.pj_done || !pj_want ) {
					botox_dbg_log( "[pf ps] %d/%d column(s) | grid%s sub %d probe(s) %d dot(s) tight %d probe(s) %d dot(s) %d off %d stuck | sweep %d tick(s), "
					               "%d landing(s), %d pin run(s), %d dot(s), %d rebase(s), %d crawl(s)%s | hb%s | seed odd %d/%d -> %d surf, %d bounce, "
					               "%d seam tb, %d pixeljump",
					               s.cols_run, s.cols, pf_tally( s.ps ).c_str( ), s.sub_probes, s.sub_dots, s.tight_probes, s.tight_dots, s.tight_off, s.tight_stuck,
					               s.sw_all, s.sw_lands,
					               s.sw_pins, s.sw_dots, s.sw_rebases,
					               s.sw_crawls, pf_tally( s.cr ).c_str( ), pf_tally( s.hb ).c_str( ), s.odd, s.checked,
					               static_cast< int >( Points.size( ) ), static_cast< int >( Point_bounce.size( ) ),
					               s.tb_seams, static_cast< int >( Point_pj.size( ) ) );
					s.stage = 2;
				}
				continue;
			}
			if ( !s.col_in ) {
				/* its own step: up to 2 x k_pf_face_rows line traces */
				s.col_in = true;
				s.face = s.phase = 0;
				c_vector col;
				s.n_faces = pf_band_column( StartPos, WallNormal, k_pf_band_lat[ s.col ], col )
				                ? pf_faces( col, n, StartPos.m_z, s.lo, s.hi, s.faces, s.traces )
				                : 0;
				if ( s.n_faces == 0 ) {
					++s.col;
					s.col_in = false;
				} else
					++s.cols_run;
				continue;
			}
			const pf_face_t& face = s.faces[ s.face ];
			const float gap       = face.disp ? k_pf_gap_disp : k_pf_gap_brush;
			const char* gap_name  = face.disp ? "graze" : "dna";
			const float face_d    = face.p.m_x * n.m_x + face.p.m_y * n.m_y;
			const auto in_col   = [ & ]( const c_vector& q ) { return ( q - face.p ).length_2d( ) < 40.f; };
			const auto pj_taken = [ & ]( float pz ) {
				return pz < s.lo || pz > s.hi ||
				       std::any_of( Point_pj.begin( ), Point_pj.end( ),
				                    [ & ]( const c_vector& q ) { return in_col( q ) && q.m_z <= pz + 0.05f && pz <= q.m_z + 2.05f; } );
			};
			const auto pj_add = [ & ]( float pz ) {
				Point_pj.erase( std::remove_if( Point_pj.begin( ), Point_pj.end( ),
				                                [ & ]( const c_vector& q ) { return in_col( q ) && pz < q.m_z && q.m_z <= pz + 2.05f; } ),
				                Point_pj.end( ) );
				Point_pj.emplace_back( face.p.m_x, face.p.m_y, pz );
			};
			const auto skin_lip = [ & ]( float end, const char* where ) {
				const c_vector q  = at( 0.f ) + n * ( k_pf_gap_pj - gap );
				const auto sweep = [ & ]( float e, trace_t& t ) {
					ray_t r( c_vector( q.m_x, q.m_y, e + 2.f ), c_vector( q.m_x, q.m_y, e ), pf_mins( ), pf_maxs( ) );
					c_trace_filter f( g_ctx.m_local );
					g_interfaces.m_engine_trace->trace_ray( r, mask_playersolid, &f, &t );
					++s.traces;
					return !t.m_start_solid && t.m_fraction < 1.f;
				};
				/* the hit at the END ( route_calc on_lip ): a brush top the start sat on stops it at fraction 0, that is not it */
				const auto lands = [ & ]( float e, bool walk ) {
					trace_t t;
					return sweep( e, t ) && t.m_end.m_z < e + 0.05f && ( !walk || t.m_plane.m_normal.m_z >= 0.7f );
				};
				if ( !lands( end, true ) )
					return false;
				trace_t deep;
				if ( sweep( end - k_pf_skin_band, deep ) || deep.m_start_solid ) /* a real lip / floor there too */
					return false;
				float lo = end - k_pf_skin_band, top = end;
				for ( int i = 0; i < k_pf_skin_bisect; ++i ) {
					const float mid = 0.5f * ( lo + top );
					if ( lands( mid, false ) )
						top = mid;
					else
						lo = mid;
				}
				float d  = 0.f;
				bool box = false;
				if ( !pf_depth( face.p, n, top + 0.5f, d, box, s.traces ) || std::fabs( d - face_d ) > 0.05f ) {
					botox_dbg_log( "[pf pj] skin top %.4f: no face over it ( wall top = floor ) | %s, face %d, col %d", top, where, s.face, s.col );
					return true;
				}
				const float pz = top + 0.03125f;
				if ( pj_taken( pz ) || pf_pj_extended( face.p, n, pz, &s.traces ) )
					return true;
				pj_add( pz );
				++s.u_skin;
				botox_dbg_log( "[pf pj] dot z %.4f | %s, skin: prop top %.4f flush with the face, trace end %.4f over it, face %d, col %d", pz, where, top,
				               end - top, s.face, s.col );
				return true;
			};
			const auto skin_row = [ & ]( float z ) {
				const auto row = [ & ]( float rz, trace_t& t ) {
					float d  = 0.f;
					bool box = false;
					return pf_depth( face.p, n, rz, d, box, s.traces, &t ) && std::fabs( d - face_d ) < 0.05f; /* a model reads 1/32 out */
				};
				const auto same = [ & ]( const trace_t& t ) {
					const char* nm = t.surface.m_name;
					return t.m_hit_entity == s.row_ent && t.m_hitbox == s.row_box && ( nm == s.row_name || ( nm && s.row_name && !strcmp( nm, s.row_name ) ) );
				};
				trace_t t;
				const bool live = row( z, t );
				if ( live && s.row_live && !same( t ) ) {
					++s.u_skin_rows;
					float lo = z, top = s.row_z;
					for ( int i = 0; i < k_pf_skin_bisect; ++i ) {
						const float mid = 0.5f * ( lo + top );
						trace_t m;
						if ( row( mid, m ) && same( m ) )
							top = mid;
						else
							lo = mid;
					}
					skin_lip( top + k_pf_skin_over, "skin row" );
				}
				s.row_live = live;
				s.row_z    = z;
				s.row_name = t.surface.m_name;
				s.row_ent  = t.m_hit_entity;
				s.row_box  = t.m_hitbox;
			};
			const auto pixel_jump = [ & ]( float got, const char* where, float off ) {
				if ( !pj_on )
					return;
				++s.u_pj;
				/* dot = the PLANE ( top + DIST_EPSILON ), not got ( landing hovers up to 2u over ). route calc aims like a floor */
				const c_vector o = n * ( off - gap );
				trace_t tr;
				ray_t ray( at( got + 0.1f ) + o, at( got - 2.5f ) + o, pf_mins( ), pf_maxs( ) ); /* 0.1 > eps, clear at got */
				c_trace_filter flt( g_ctx.m_local );
				g_interfaces.m_engine_trace->trace_ray( ray, mask_playersolid, &flt, &tr );
				++s.traces;
				const c_vector ln = tr.m_plane.m_normal;
				if ( tr.m_start_solid || tr.m_fraction >= 1.f || ln.m_z < 0.7f ) {
					/* a prop top flush with the face still grounds the 2u trace of a hull hovering at got ( what grounded the probe ) */
					if ( skin_lip( got - 2.f, where ) )
						return;
					if ( off == gap ) /* a surf dot misses on every flush seam: quiet */
						botox_dbg_log( "[pf pj] no dot: plane trace %s under %.4f | %s, %s gap, face %d, col %d",
						               tr.m_start_solid ? "start solid" : tr.m_fraction >= 1.f ? "missed" : "steep", got, where, gap_name,
						               s.face, s.col );
					return;
				}
				++s.u_pj_lip;
				const float pz = tr.m_end.m_z;
				if ( pj_taken( pz ) )
					return;
				/* lip reaches a hull `out` off the face? vertical hull trace onto the plane ( slope-adjusted z ). label only */
				const auto reaches = [ & ]( float out ) {
					const c_vector p = at( pz ) + n * ( out - gap );
					const float z    = pz - ( out - off ) * ( ln.m_x * n.m_x + ln.m_y * n.m_y ) / ln.m_z;
					trace_t t;
					ray_t r( c_vector( p.m_x, p.m_y, z + 0.1f ), c_vector( p.m_x, p.m_y, z - 0.1f ), pf_mins( ), pf_maxs( ) );
					g_interfaces.m_engine_trace->trace_ray( r, mask_playersolid, &flt, &t );
					++s.traces;
					return !t.m_start_solid && t.m_fraction < 1.f && t.m_plane.m_normal.m_z >= 0.7f;
				};
				if ( pf_pj_extended( face.p, n, pz, &s.traces ) ) {
					botox_dbg_log( "[pf pj] floor z %.4f: still there %.0fu out | %s, face %d, col %d", pz, k_pf_pj_floor, where, s.face, s.col );
					return;
				}
				const bool natural = reaches( k_pf_gap_rest );
				pj_add( pz );
				botox_dbg_log( "[pf pj] dot z %.4f ( probe ended %.4f ) | %s, %s gap, face %d, col %d, %s", pz, got, where, gap_name,
				               s.face, s.col, natural ? "natural" : "tight" );
			};

			if ( s.phase == 0 ) {
				s_pf_bounce.reset( );
				s_pf_surf.reset( );
				s.base = face.p + n * ( support + gap );
				s.trail.clear( );
				std::fill_n( s.u_ps, k_pf_outcomes, 0 );
				std::fill_n( s.u_cr, k_pf_outcomes, 0 );
				std::fill_n( s.u_hb, k_pf_outcomes, 0 );
				std::fill_n( s.u_ht, k_pf_outcomes, 0 );
				std::fill_n( s.u_rt, k_pf_outcomes, 0 );
				s.tight          = face.prop ? k_pf_tight_gap : 0.f;
				s.tight_q        = 0.f;
				s.tight_off_used = 0.f;
				for ( int j = 0; j < s.n_faces; ++j ) {
					const float q = ( face.p - s.faces[ j ].p ).dot_product( n );
					if ( j != s.face && !face.disp && !s.faces[ j ].disp && q > 0.f && q + k_pf_tight_gap < 0.03125f ) {
						s.tight   = k_pf_tight_gap;
						s.tight_q = q;
					}
				}
				s.tight_live = false;
				s.sub_live = false;
				s.sw_ticks = s.u_crawls = s.u_pj = s.u_pj_lip = 0;
				s.u_skin = s.u_skin_rows = 0;
				s.row_live              = false;
				s.cr_live               = false;
				s.gz                    = pf_grid_seam( s.hi );
				s.phase                 = 1;
				/* grid is texturebug/pixeljump bookkeeping. legacy surf is phase 2. */
				if ( !ps_on && !pj_on && !tb_on ) {
					s.gz    = pf_hb_first( s.lo );
					s.phase = 3;
				} else if ( !tb_on && !pj_on ) {
					s.phase = 2;
				}
			} else if ( s.phase == 1 ) {
				if ( s.tight_live ) {
					/* tight probe ( s.tight, phase 0 ): hug the proud face, a held press, at the top of the window over the seam. only
					   where that face is really there at this height ( a cap ends ): one line trace must read its depth */
					const c_vector top = face.p + c_vector( 0.f, 0.f, std::floor( s.sub_z ) + k_pf_tight_d );
					float depth_here   = 0.f;
					bool box_here      = false;
					const bool present = pf_depth( face.p, n, top.m_z, depth_here, box_here, s.traces ) &&
					                     std::fabs( depth_here - ( face.p.m_x * n.m_x + face.p.m_y * n.m_y ) ) < 0.001f;
					if ( !present ) {
						++s.tight_off;
						s.tight_live = false;
						continue;
					}
					/* brush: one gap. model: nearest clear offset ( how close a hull may sit to a model is unknown, the log says ) */
					c_vector tp;
					bool clear = false;
					for ( const float off : face.prop ? std::span< const float >( k_pf_tight_prop ) : std::span< const float >( &k_pf_tight_gap, 1 ) ) {
						tp = top + n * ( support + off );
						++s.traces;
						if ( pf_clear( tp ) && !pf_void( tp ) ) {
							s.tight_off_used = off;
							clear            = true;
							break;
						}
					}
					if ( !clear ) { /* stuck = the face is not square to the wall, or a model allows no hull that near */
						++s.tight_stuck;
						s.tight_live = false;
						continue;
					}
					if ( const int r = ready( ); r != 1 ) {
						if ( r == 0 )
							break; /* tight_live stays: this probe again next cmd */
						continue;
					}
					s.tight_live = false;
					float got    = 0.f;
					++s.tight_probes;
					const int to = pf_hold( cmd, tp, into * ( k_pf_air_accel * 30.f * dt ), fwd_hard, side_hard, pin, step, dt, got, s.odd, s.checked );
					if ( to == pf_held && got >= s.lo && !near_dot( s_pf_tb_scratch, face.p, got, 0.05f ) ) {
						s_pf_tb_scratch.emplace_back( face.p.m_x, face.p.m_y, got );
						++s.tight_dots;
						s.sub_live = false; /* the marker has its dot */
						botox_dbg_log( "[pf ps] dot z %.4f | tight %+.4f off %s, face %.4f proud, face %d, col %d", got, s.tight_off_used,
						               face.prop ? "a model's 1/32 read" : "the face", s.tight_q, s.face, s.col );
					}
					continue;
				}
				if ( s.sub_live ) {
					const int k_prev = s.sub_k;
					float d          = 0.f;
					if ( !pf_sub_next( s.sub_k, s.sub_lo, s.sub_hi, d ) ) {
						s.sub_live = false;
						continue;
					}
					const c_vector p = at( s.sub_z ) + along * d;
					bool top         = false;
					for ( const float a : { 0.f, k_pf_slide, -k_pf_slide } ) {
						if ( top )
							break;
						trace_t tr;
						ray_t ray( p, p + ( push + along * a + c_vector( 0.f, 0.f, 2.f * pin - 0.5f ) ) * dt, pf_mins( ), pf_maxs( ) );
						pf_mover_trace( p, push + along * a + c_vector( 0.f, 0.f, pin - 0.5f ), a != 0.f ? k_pf_bounds_slide : 0.f, dt, ray, tr );
						++s.traces;
						top = !tr.m_start_solid && tr.m_fraction < 1.f && tr.m_plane.m_normal.m_z > 0.7f;
					}
					if ( !top || pf_void( p ) )
						continue;
					if ( const int r = ready( ); r != 1 ) {
						if ( r == 0 ) { /* this spot again next cmd */
							s.sub_k = k_prev;
							break;
						}
						continue;
					}
					float got = 0.f, slid = 0.f;
					++s.sub_probes;
					if ( hold_slid( p, push, fwd, side, got, slid ) != pf_held || got < s.lo || near_dot( s_pf_tb_scratch, face.p, got, 0.05f ) )
						continue;
					const c_vector fp = face.p + along * d;
					s_pf_tb_scratch.emplace_back( fp.m_x, fp.m_y, got );
					++s.sub_dots;
					s.sub_live = false;
					botox_dbg_log( "[pf ps] dot z %.4f | grid %.4f sub %+.1f slide %+.0f, %s gap, face %d, col %d", got, s.sub_z, d, slid, gap_name,
					               s.face, s.col );
					add_seam_tb( fp, got, face.disp );
					continue;
				}
				const float z = pf_marker( s.gz );
				if ( z < s.lo ) {
					/* modern sweep stays behind the dormant pj_on path. legacy surf is phase 2. */
					if ( pj_on ) {
						sweep_from( s.hi );
						s.phase = 2;
					} else if ( ps_on ) {
						s.phase = 2;
					} else {
						s.gz    = pf_hb_first( s.lo );
						s.phase = 3;
					}
					continue;
				}
				int o = pf_held;
				if ( const auto it = find_dot( s_pf_tb_scratch, face.p, z, 0.05f ); it != s_pf_tb_scratch.end( ) && pf_is_marker( it->m_z ) )
					o = pf_dup;
				else if ( !clear_at( z ) )
					o = pf_stuck;
				else if ( pf_void( at( z ) ) )
					o = pf_void_start;
				else {
					if ( const int r = ready( ); r != 1 ) {
						if ( r == 0 )
							break;
						continue;
					}
					float got = 0.f, slid = 0.f;
					o         = hold_slid( at( z ), push, fwd, side, got, slid );
					if ( o == pf_held ) {
						const auto hit = find_dot( s_pf_tb_scratch, face.p, got, 0.05f );
						if ( got < s.lo )
							o = pf_off;
						else if ( hit != s_pf_tb_scratch.end( ) && ( pf_is_marker( hit->m_z ) || ( !pf_is_marker( got ) && std::fabs( hit->m_z - got ) < 0.02f ) ) )
							o = pf_dup;
						else {
							if ( hit != s_pf_tb_scratch.end( ) )
								*hit = c_vector( face.p.m_x, face.p.m_y, got );
							else
								s_pf_tb_scratch.emplace_back( face.p.m_x, face.p.m_y, got );
							botox_dbg_log( "[pf ps] dot z %.4f | grid %.4f slide %+.0f, %s gap, face %d, col %d", got, z, slid, gap_name, s.face,
							               s.col );
							add_seam_tb( face.p, got, face.disp );
						}
					} else if ( o == pf_grounded )
						pixel_jump( got, "grid", gap );
				}
				if ( o == pf_dup ? clear_at( z ) && !pf_void( at( z ) ) : o != pf_grounded && o != pf_stuck && o != pf_void_start )
					pixel_jump( z, "grid lip", k_pf_gap_pj );
				/* a model face reads 1/32 out: its skin test would stand off it, the brush face over the top runs it */
				if ( pj_on && !face.disp && !face.prop )
					skin_row( s.gz + 0.5f );
				++s.u_ps[ o ];
				++s.ps[ o ];
				s.gz -= 1.f;
				/* then the tight probe ( surf only, a tight hold is never a marker ) and the rest of this column's cell, each its own
				   step ( above ), unless the marker already has a dot in the band */
				s.sub_z = z;
				pf_cell( s.col, s.cols, s.col_ok, s.sub_lo, s.sub_hi );
				s.sub_k      = 0;
				s.sub_live   = ( ps_on || tb_on ) && o != pf_held && !near_dot( s_pf_tb_scratch, face.p, z, 0.05f );
				s.tight_live = ps_on && s.tight > 0.f && o != pf_held && !near_dot( s_pf_tb_scratch, face.p, z, 0.05f );
			} else if ( s.phase == 2 ) {
				if ( pj_on ) {
				if ( s.cr_live ) {
					/* crawl, one probe: hard-pressed at pin speed through a clipped sweep tick's span */
					if ( s.cr_z < s.cr_end || s.cr_n >= k_pf_crawl_max ) {
						s.cr_live = false;
						continue;
					}
					const float z = s.cr_z;
					int o         = pf_held;
					if ( z < s.lo || z > s.hi )
						o = pf_off;
					else if ( near_dot( s_pf_tb_scratch, face.p, z, 0.05f ) )
						o = pf_dup;
					else if ( !clear_at( z ) )
						o = pf_stuck;
					else if ( pf_void( at( z ) ) )
						o = pf_void_start;
					else {
						if ( const int r = ready( ); r != 1 ) {
							if ( r == 0 )
								break;
							continue;
						}
						float got = 0.f, slid = 0.f;
						o         = hold_slid( at( z ), push_hard, fwd_hard, side_hard, got, slid );
						if ( o == pf_held ) {
							if ( got < s.lo )
								o = pf_off;
							else if ( near_dot( s_pf_tb_scratch, face.p, got, 0.02f ) )
								o = pf_dup;
							else {
								s_pf_tb_scratch.emplace_back( face.p.m_x, face.p.m_y, got );
								botox_dbg_log( "[pf ps] dot z %.4f | crawl %.4f slide %+.0f, %s gap, face %d, col %d", got, z, slid, gap_name, s.face,
								               s.col );
								pixel_jump( got, "crawl surf", k_pf_gap_pj );
							}
						}
					}
					++s.u_cr[ o ];
					++s.cr[ o ];
					s.cr_z -= step;
					++s.cr_n;
				} else if ( !s.sw_live || s.sw_ticks >= sweep_cap ) {
					s.gz    = pf_hb_first( s.lo );
					s.phase = 3;
				} else if ( s.sw_seek ) {
					/* seek: first clear in-map start at or under sw_pos, resumes next step */
					float z    = s.sw_pos.m_z;
					bool found = false;
					for ( int i = 0; i < k_pf_seek_steps && z >= s.lo; ++i, z -= 1.f ) {
						if ( clear_at( z ) && !pf_void( at( z ) ) ) {
							found = true;
							break;
						}
					}
					s.sw_pos = at( z );
					if ( found )
						s.sw_seek = false;
					else if ( z < s.lo )
						s.sw_live = false;
				} else {
					/* sweep, one tick, re-placed from its own carried state */
					if ( const int r = ready( ); r != 1 ) {
						if ( r == 0 )
							break;
						continue;
					}
					const c_vector from   = s.sw_pos;
					const float seed_vz   = s.sw_vel.m_z;
					const int hold_before = s.sw_hold;
					pf_place( from, s.sw_vel );
					cmd->m_forward_move = fwd_hard;
					cmd->m_side_move    = side_hard;
					g_prediction.begin( local, cmd );
					g_prediction.end( local );
					++s.sw_ticks;
					++s.sw_all;
					s.sw_pos = local->get_origin( );
					s.sw_vel = local->get_velocity( );
					s.sw_vel = s.sw_vel - along * s.sw_vel.dot_product( along ); /* a glancing hit must not slide it off the column */
					pf_seed_check( from.m_z, s.sw_pos.m_z, s.sw_vel.m_z, pin, dt, seed_vz + 2.f * pin, s.odd, s.checked );
					const float z       = s.sw_pos.m_z;
					const float dvz     = s.sw_vel.m_z - ( seed_vz + 2.f * pin );
					const bool touched  = pf_touched( dvz, s.sw_dvz );
					s.sw_dvz            = dvz;
					const float restart = ( std::min )( z, from.m_z ) - 1.f; /* never above where this tick began */
					const int ev = pf_sweep_tick( s.sw_hold, from.m_z, z, s.sw_vel.m_z, pin, ( local->get_flags( ) & fl_onground ) != 0,
					                              pf_move_type( local ) == move_type_walk );
					if ( ev == pf_sw_leave ) {
						/* a ladder ( the hard press climbs it ): go on under it, like a landing */
						note( " L" );
						sweep_from( restart );
					} else if ( ev == pf_sw_land ) {
						/* floor / ledge top: go on under it. a lip it grounded on is a pixel jump */
						pixel_jump( z, "sweep", gap );
						++s.sw_lands;
						note( std::format( " G{:.2f}", z ) );
						sweep_from( restart );
					} else if ( ev == pf_sw_pin ) {
						s.sw_pins += s.sw_hold == 1;
					} else if ( ev == pf_sw_dot ) {
						/* x/y back onto the face it pressed on ( = face.p unless a recess pulled it in ) */
						const c_vector fp = s.sw_pos - n * ( support + gap );
						char why          = 'H';
						if ( z < s.lo || z > s.hi )
							why = 'O';
						else if ( near_dot( s_pf_tb_scratch, fp, z, 0.02f ) )
							why = 'D';
						else {
							s_pf_tb_scratch.emplace_back( fp.m_x, fp.m_y, z );
							++s.sw_dots;
							botox_dbg_log( "[pf ps] dot z %.4f | sweep, %s gap, face %d, col %d", z, gap_name, s.face, s.col );
							pixel_jump( z, "sweep surf", k_pf_gap_pj );
						}
						note( std::format( " {}{:.4f}", why, z ) );
						sweep_from( z - step );
					} else if ( ( hold_before > 0 || touched ) && s.u_crawls < k_pf_crawls ) {
						/* touched something new and didn't hold: crawl that span */
						++s.u_crawls;
						++s.sw_crawls;
						s.cr_live = true;
						s.cr_z    = from.m_z + step;
						s.cr_end  = z - step;
						s.cr_n    = 0;
						note( std::format( " C{:.2f}", from.m_z ) );
					}
					/* pushed OUT off its gap ( slid off a sill ): back onto the column. inward / mid-hold = left alone */
					if ( s.sw_live && !s.sw_seek && s.sw_hold == 0 && ( s.sw_pos - s.base ).dot_product( n ) > k_pf_drift ) {
						++s.sw_rebases;
						note( " ~" );
						sweep_from( s.sw_pos.m_z );
					}
					if ( s.sw_live && !s.sw_seek && s.sw_pos.m_z < s.lo - 1.f )
						s.sw_live = false;
				}
				} else if ( !ps_on ) {
					s.gz    = pf_hb_first( s.lo );
					s.phase = 3;
				} else {
					surf_ran = true;
					if ( !s_pf_surf ) {
						s_pf_surf = std::make_unique< pf_bounce_backend_t >( s_pf_bounce_seed, face, n, s.lo + k_pf_margin,
						                                                      s.hi - k_pf_margin, pin, dt, s.traces, false );
						armed = false;
					}
					auto& surf = *s_pf_surf;
					if ( !surf.valid || surf.local != local || surf.identity != local->get_ref_ehandle( ) ||
					     surf.target != pin || surf.interval != dt ) {
						s_pf_surf.reset( );
						g_prediction.restore_entity_to_predicted_frame( g_interfaces.m_prediction->m_commands_predicted - 1 );
						armed = false; s.gz = pf_hb_first( s.lo ); s.phase = 3; continue;
					}
					const int before_sims = surf.cursor.sims;
					surf.caller.save( local, surf.layout );
					const auto event = surf.cursor.step( surf );
					surf.caller.load( local, surf.layout );
					s.sims += surf.cursor.sims - before_sims;
					if ( event == n_pf_bounce::event_t::done ) {
						s_pf_surf.reset( );
						g_prediction.restore_entity_to_predicted_frame( g_interfaces.m_prediction->m_commands_predicted - 1 );
						armed = false; s.gz = pf_hb_first( s.lo ); s.phase = 3; continue;
					}
					if ( event == n_pf_bounce::event_t::candidate ) {
						const auto& c = surf.cursor;
						const float feet = c.after.origin.m_z;
						const c_vector dot( face.p.m_x + c.after.origin.m_x - surf.xy.m_x,
						                    face.p.m_y + c.after.origin.m_y - surf.xy.m_y, feet );
						if ( !near_dot( Points, dot, feet, 0.05f ) ) {
							Points.push_back( dot );
							++s.u_ps[ pf_held ]; ++s.ps[ pf_held ];
							botox_dbg_log( "[pf ps] legacy dot %.4f | start %.4f, vz %.3f -> %.3f, row %d",
							               feet, c.head, c.before.velocity.m_z, c.after.velocity.m_z, c.row );
						}
					}
				}
			} else if ( s.phase == 3 ) {
				if ( !hb_on ) {
					s_pf_bounce.reset( );
					armed = false;
					s.gz = std::floor( s.hi ); s.phase = 4; continue;
				}
				bounce_ran = true;
				if ( !s_pf_bounce ) {
					s_pf_bounce = std::make_unique< pf_bounce_backend_t >( s_pf_bounce_seed, face, n, s.lo + k_pf_margin,
					                                                        s.hi - k_pf_margin, pin, dt, s.traces, true );
					armed = false;
				}
				auto& bounce = *s_pf_bounce;
				if ( !bounce.valid || bounce.local != local || bounce.identity != local->get_ref_ehandle( ) ||
				     bounce.target != pin || bounce.interval != dt ) {
					s_pf_bounce.reset( );
					g_prediction.restore_entity_to_predicted_frame( g_interfaces.m_prediction->m_commands_predicted - 1 );
					armed = false; s.gz = std::floor( s.hi ); s.phase = 4; continue;
				}
				const int before_sims = bounce.cursor.sims;
				bounce.caller.save( local, bounce.layout );
				const auto event = bounce.cursor.step( bounce );
				bounce.caller.load( local, bounce.layout );
				s.sims += bounce.cursor.sims - before_sims;
				if ( event == n_pf_bounce::event_t::done ) {
					s_pf_bounce.reset( );
					g_prediction.restore_entity_to_predicted_frame( g_interfaces.m_prediction->m_commands_predicted - 1 );
					armed = false; s.gz = std::floor( s.hi ); s.phase = 4; continue;
				}
				if ( event == n_pf_bounce::event_t::candidate ) {
					const auto& c = bounce.cursor;
					const float raw_head = c.after.origin.m_z + k_pf_hull;
					trace_t over{ };
					const bool ceiling = pf_overhead( c.after.origin, over, s.traces ) && over.m_plane.m_normal.m_z < -k_pf_hb_nz;
					// Keep actual ceiling stops measured; seam dots use the first
					// finder's saved-point height for every preview/snap/place path.
					const float head = ceiling ? raw_head : n_pf_bounce::marker_head( raw_head );
					const c_vector dot( face.p.m_x + c.after.origin.m_x - bounce.xy.m_x,
					                    face.p.m_y + c.after.origin.m_y - bounce.xy.m_y, head );
					int verdict = pf_off;
					if ( !near_dot( Point_bounce, dot, head, 0.05f ) ) {
						Point_bounce.push_back( dot );
						if ( ceiling )
							Point_bounce_ceil.push_back( dot );
						verdict = pf_held;
						botox_dbg_log( "[pf hb] legacy dot %.4f | raw %.4f, start %.4f, vz %.3f -> %.3f, ceiling %d, pass %d row %d",
						               head, raw_head, c.head, c.before.velocity.m_z, c.after.velocity.m_z, ceiling, c.pass, c.row );
					} else {
						verdict = pf_dup;
					}
					++s.u_hb[ verdict ]; ++s.hb[ verdict ];
				}
			} else if ( s.phase == 4 ) {
				/* head seam grid, top down, one probe a unit seam in the HEAD range. a seam with a feet tb dot keeps that one */
				if ( !tb_on || !tb_axial || face.disp || face.prop || s.gz < s.lo ) {
					s.gz    = std::floor( s.hi );
					s.phase = 5;
					continue;
				}
				const float seam = s.gz;
				int o            = pf_held;
				if ( near_dot( Point_tb, face.p, seam, 0.05f ) )
					o = pf_dup;
				else if ( !pf_head_face( face.p, n, seam, s.traces ) )
					o = pf_off;
				else {
					if ( const int r = ready( ); r != 1 ) {
						if ( r == 0 )
							break; /* this seam again next cmd */
						continue;
					}
					float hull = 0.f, move = 0.f;
					int how    = 0;
					o          = pf_head_probe( cmd, face.p, n, seam, rot, pin, step, dt, hull, move, how, s.traces, s.odd, s.checked );
					if ( o == pf_held ) {
						Point_tb.emplace_back( face.p.m_x, face.p.m_y, seam );
						/* route calc places a Point_tb_head rider at the rest offset: only the rest probe's. the others are the TB
						   ride's edge ( falling straddle ), route calc's plain texturebug arrival */
						if ( how == 0 )
							Point_tb_head.push_back( Point_tb.back( ) );
						else
							++s.tb_edges;
						botox_dbg_log( "[pf tb] head dot z %.4f | %s, hull %.7f, move %.4f ( %.0f ulp ), face %d, col %d", seam, pf_edge_how( how, dt ).c_str( ),
						               hull, move, k_pf_head_press, s.face, s.col );
					}
				}
				++s.u_ht[ o ];
				++s.ht[ o ];
				s.gz -= 1.f;
			} else if ( s.phase == 5 ) {
				/* rising edge grid, top down, one probe pair a unit seam in the FEET range. a seam with a tb dot keeps that one */
				if ( !tb_on || !tb_axial || face.disp || face.prop || s.gz < s.lo ) {
					s.phase = 6;
					continue;
				}
				const float seam = s.gz;
				int o            = pf_held;
				if ( near_dot( Point_tb, face.p, seam, 0.05f ) )
					o = pf_dup;
				else if ( !pf_rise_face( face.p, n, seam, s.traces ) )
					o = pf_off;
				else {
					if ( const int r = ready( ); r != 1 ) {
						if ( r == 0 )
							break; /* this seam again next cmd */
						continue;
					}
					float hull = 0.f, move = 0.f;
					int how    = 0;
					o          = pf_rise_probe( cmd, face.p, n, seam, rot, pin, dt, hull, move, how, s.traces, s.odd, s.checked );
					if ( o == pf_held ) {
						Point_tb.emplace_back( face.p.m_x, face.p.m_y, seam );
						Point_tb_rise.emplace_back( Point_tb.back( ), k_pf_rise[ how - 1 ][ 1 ] * dt * 64.f );
						++s.tb_rises;
						botox_dbg_log( "[pf tb] rise dot z %.4f | rising %.0f, feet %.4f under, hull %.7f, move %.4f, face %d, col %d", seam,
						               k_pf_rise[ how - 1 ][ 0 ], k_pf_rise[ how - 1 ][ 1 ] * dt * 64.f, hull, move, s.face, s.col );
					}
				}
				++s.u_rt[ o ];
				++s.rt[ o ];
				s.gz -= 1.f;
			} else {
				/* pj lips/traces 0/N = no standable plane k_pf_gap_pj out in this column. skin dots/rows = skin lip dots, surface
				   changes between grid rows tested */
				/* depth = this face out from face 0 ( + = proud ), tight = it stands < DIST_EPSILON proud of a face behind, clear = the
				   last tight offset a hull fit at ( model: how near a hull may sit, 0 = none fit / never ran ) */
				botox_dbg_log( "[pf u] col %d face %d %s%s%s depth %+.4f tight %.4f clear %+.4f | grid%s | sweep %d tick(s)%s | crawl%s | hb%s | head%s | rise%s | pj %d/%d skin %d/%d",
				               s.col, s.face, gap_name, face.disp ? " disp" : "", face.prop ? " prop" : "", ( face.p - s.faces[ 0 ].p ).dot_product( n ),
				               s.tight > 0.f ? s.tight_q : 0.f, s.tight_off_used, pf_tally( s.u_ps ).c_str( ), s.sw_ticks, s.trail.c_str( ), pf_tally( s.u_cr ).c_str( ),
				               pf_tally( s.u_hb ).c_str( ), pf_tally( s.u_ht ).c_str( ), pf_tally( s.u_rt ).c_str( ), s.u_pj_lip, s.u_pj, s.u_skin, s.u_skin_rows );
				s.phase = 0;
				if ( ++s.face >= s.n_faces ) {
					s.face   = 0;
					s.col_in = false;
					++s.col;
				}
			}
		} else if ( s.stage == 1 ) {
			s.stage = ( s.pj_done || !pj_want ) ? 2 : 0;
			continue;
		} else {
			if ( !s.pj_done && pj_want )
				continue;
			/* unit: the report */
			s.busy             = false;
			const int tb_count = tb_on ? static_cast< int >( Point_tb.size( ) ) : 0;
			if ( tb_on )
				botox_dbg_log( "[pf tb] column z %.2f..%.2f n %.4f %.4f -> %d seam(s), %d head seam(s) + %d falling edge(s)%s, "
				               "%d rising edge(s)%s, %d diagonal / disp hold(s) not marked",
				               s.lo, s.hi, WallNormal.m_x, WallNormal.m_y, s.tb_seams,
				               static_cast< int >( Point_tb_head.size( ) ), s.tb_edges, pf_tally( s.ht ).c_str( ), s.tb_rises, pf_tally( s.rt ).c_str( ),
				               s.tb_unverified );
			pf_recheck_tricks( );
			const int pixelsurf_count = ps_on ? static_cast< int >( Points.size( ) ) : 0;
			const int bounce_count    = hb_on ? static_cast< int >( Point_bounce.size( ) ) : 0;
			const int pj_count        = pj_want ? static_cast< int >( Point_pj.size( ) ) : 0;
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
			/* tb off: seams still found ( grid ), don't let them vanish silently */
			if ( !tb_on && !Point_tb.empty( ) )
				msg += " (" + std::to_string( Point_tb.size( ) ) + " texturebug(s) hidden)";
			movement_add_window( 3.0f, msg );
		}
	}

	const long long chunk_us = budget.used_us( );
	s.used_us += chunk_us;
	s.worst_chunk_us = ( std::max )( s.worst_chunk_us, chunk_us );
	s.sims += static_cast< int >( g_prediction.m_sim_count - sims_before );
	s.next_qpc = n_tick::qpc_now( ) + 2ll * chunk_us * n_tick::qpc_freq( ) / 1000000ll;
	++s.chunks;
	if ( !s.busy )
		botox_dbg_log( "[pf] scan done in %d cmd(s), %lld us total, worst chunk %lld us, worst step %lld us, %d sims, %d traces", s.chunks,
		               s.used_us, s.worst_chunk_us, s.worst_step_us, s.sims, s.traces );
	/* live cmd + entity go back as they came ( the real move still predicts ) */
	cmd->m_buttons      = live_buttons;
	cmd->m_forward_move = live_fwd;
	cmd->m_side_move    = live_side;
	g_prediction.restore_entity_to_predicted_frame( g_interfaces.m_prediction->m_commands_predicted - 1 );
	if ( bounce_ran || surf_ran ) {
		local->get_last_command( ) = live_last;
		*local->get_current_command( ) = live_current;
		local->get_tick_base( ) = live_tick;
	}
}
