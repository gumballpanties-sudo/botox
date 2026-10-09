#include "movement.h"
#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"
#include "../../dependencies/imgui/imgui.h"
#include "../prediction/prediction.h"
#include "../../utilities/perf/perf_watch.h"
#include "tick_scale.h"
#include <algorithm>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

extern void botox_dbg_log( const char* fmt, ... );
extern bool g_air_stuck_owns_cmd;

namespace
{
	float quantize_pixel_z( float z )
	{
		const float plane   = z - 0.03125f;
		const float aligned = roundf( plane );
		if ( fabsf( plane - aligned ) > 0.01f )
			return z;
		return aligned < 0.f ? aligned + 1.f - 0.97125f : aligned + 0.03125f;
	}

}

namespace n_route
{
	/* z the arc must hit, derived every solve ( the type can change in the menu after placing ) */
	float target_z( const route_point_t& point )
	{
		if ( point.type == route_pt_ground || point.type == route_pt_pixeljump )
			return point.pos.m_z;
		if ( point.type == route_pt_headbang )
			return point.has_measured ? point.measured_z : point.pos.m_z;
		if ( point.type == route_pt_texturebug )
			return point.pos.m_z;
		if ( point.type == route_pt_headbounce ) {
			if ( point.hb_ceiling )
				return point.pos.m_z;
			return point.pos.m_z + ( point.snap_type == route_pt_headbounce ? 0.f : 0.03125f );
		}
		if ( point.has_measured )
			return point.measured_z;
		if ( point.snap_type == route_pt_texturebug ) {
			const float plane = roundf( point.pos.m_z );
			return fabsf( point.pos.m_z - plane ) < 0.0625f ? quantize_pixel_z( plane + 0.03125f ) : point.pos.m_z;
		}
		return point.snapped ? point.pos.m_z : quantize_pixel_z( point.pos.m_z );
	}

	std::string route_line( const solution_t& s ) { return route_text( s.elements ); }
}

namespace
{
	constexpr unsigned int k_mask_noclip = mask_playersolid & ~static_cast< unsigned int >( contents_playerclip );
	bool rc_ray_clip = false;

	bool rc_ray( const c_vector& eye, const c_vector& dir, trace_t& out )
	{
		const c_vector end = eye + dir * 8192.f;
		ray_t ray( eye, end );
		c_trace_filter filter( g_ctx.m_local );
		g_interfaces.m_engine_trace->trace_ray( ray, mask_playersolid, &filter, &out );
		if ( out.m_start_solid || out.m_all_solid ) {
			ray_t retry( eye, end );
			c_trace_filter retry_filter( g_ctx.m_local );
			g_interfaces.m_engine_trace->trace_ray( retry, k_mask_noclip, &retry_filter, &out );
		}
		return out.m_fraction < 1.f && !out.m_start_solid && !out.m_all_solid;
	}

	/* thin geometry: the centre ray can miss a rail / pipe by a pixel. a ring of rays ~1 deg out,
	   NEAREST hit wins, only if it beats the centre by k_fan_win ( a flat wall never moves it ). */
	constexpr float k_fan_deg = 1.1f;
	constexpr int k_fan_rays  = 8;
	constexpr float k_fan_win = 2.f; /* units a ring hit must beat the centre by */

	bool rc_aim_world( const c_vector& eye, const c_vector& forward, trace_t& out )
	{
		const bool centre_hit = rc_ray( eye, forward, out );
		const bool centre_clip =
			centre_hit && ( out.m_contents & contents_playerclip ) != 0 && ( out.m_contents & contents_solid ) == 0;
		const float centre_d = centre_hit ? out.m_fraction * 8192.f : FLT_MAX;

		c_vector right = c_vector( forward.m_y, -forward.m_x, 0.f );
		if ( right.length( ) < 0.001f )
			right = c_vector( 1.f, 0.f, 0.f );
		right = right.normalized( );
		const c_vector up = right.cross_product( forward ).normalized( );

		trace_t nearest;
		bool nearest_clip = false;
		float near_d      = FLT_MAX;
		const float t     = tanf( k_fan_deg * 3.14159265f / 180.f );
		for ( int k = 0; k < k_fan_rays; ++k ) {
			const float a = 6.28318531f * static_cast< float >( k ) / static_cast< float >( k_fan_rays );
			const c_vector d = ( forward + right * ( cosf( a ) * t ) + up * ( sinf( a ) * t ) ).normalized( );
			trace_t tr;
			if ( !rc_ray( eye, d, tr ) )
				continue;
			const float dist = tr.m_fraction * 8192.f;
			if ( dist < near_d ) {
				near_d       = dist;
				nearest      = tr;
				nearest_clip = ( tr.m_contents & contents_playerclip ) != 0 && ( tr.m_contents & contents_solid ) == 0;
			}
		}

		/* measured against the CENTRE only, so ring hits never bid against each other */
		if ( near_d < centre_d - ( centre_hit ? k_fan_win : 0.f ) ) {
			out         = nearest;
			rc_ray_clip = nearest_clip;
		} else {
			rc_ray_clip = centre_clip;
		}
		return out.m_fraction < 1.f && !out.m_start_solid && !out.m_all_solid;
	}

	bool rc_clip_top( const trace_t& tr, c_vector& top )
	{
		const c_vector n( tr.m_plane.m_normal.m_x, tr.m_plane.m_normal.m_y, 0.f );
		if ( n.length( ) < 0.1f )
			return false;
		const c_vector inside = tr.m_end - n.normalized( ) * 1.f;
		trace_t down;
		ray_t ray( inside + c_vector( 0.f, 0.f, 72.f ), inside );
		c_trace_filter flt( g_ctx.m_local );
		g_interfaces.m_engine_trace->trace_ray( ray, mask_playersolid, &flt, &down );
		if ( down.m_start_solid || down.m_fraction >= 1.f || down.m_plane.m_normal.m_z <= 0.7f || !( down.m_contents & contents_playerclip ) )
			return false;
		top = down.m_end;
		return true;
	}

	void rc_follow( route_point_t& pt )
	{
		if ( pt.ent <= 0 || !g_interfaces.m_client_entity_list )
			return;
		auto* e = g_interfaces.m_client_entity_list->get< c_base_entity >( pt.ent );
		if ( !e || e == g_ctx.m_local || !e->is_player( ) || !e->is_alive( ) || e->is_dormant( ) )
			return;
		auto* col = e->get_collideable( );
		if ( !col )
			return;
		const c_vector o = e->get_abs_origin( );
		const float z = pt.type == route_pt_headbang ? o.m_z + col->get_obb_mins( ).m_z - 0.03125f : o.m_z + col->get_obb_maxs( ).m_z + 0.03125f;
		pt.pos        = c_vector( o.m_x + pt.ent_offset.m_x, o.m_y + pt.ent_offset.m_y, z );
	}

	bool bind_down( key_bind_t& bind )
	{
		if ( bind.m_key <= 0 )
			return false;
		return g_input.check_input( &bind );
	}

	std::string key_label( const key_bind_t& bind )
	{
		const int k = bind.m_key;
		if ( k <= 0 )
			return "none";

		switch ( k ) {
		case VK_LBUTTON: return "m1";
		case VK_RBUTTON: return "m2";
		case VK_MBUTTON: return "m3";
		case VK_XBUTTON1: return "m4";
		case VK_XBUTTON2: return "m5";
		case VK_SPACE: return "spa";
		case VK_SHIFT: return "shi";
		case VK_CONTROL: return "ctl";
		case VK_MENU: return "alt";
		case VK_TAB: return "tab";
		case VK_RETURN: return "ret";
		case VK_BACK: return "bac";
		case VK_INSERT: return "ins";
		case VK_DELETE: return "del";
		case VK_END: return "end";
		case VK_HOME: return "hom";
		case VK_PRIOR: return "pgu";
		case VK_NEXT: return "pgd";
		case VK_CAPITAL: return "cap";
		case VK_OEM_4: return "[";
		case VK_OEM_6: return "]";
		case VK_OEM_1: return ";";
		case VK_OEM_7: return "'";
		case VK_OEM_COMMA: return ",";
		case VK_OEM_PERIOD: return ".";
		default: break;
		}

		if ( k >= '0' && k <= '9' )
			return std::string( 1, static_cast< char >( k ) );
		if ( k >= 'A' && k <= 'Z' )
			return std::string( 1, static_cast< char >( k + 32 ) );
		if ( k >= VK_NUMPAD0 && k <= VK_NUMPAD9 )
			return "num" + std::to_string( k - VK_NUMPAD0 );
		if ( k >= VK_F1 && k <= VK_F24 )
			return "f" + std::to_string( k - VK_F1 + 1 );
		return "key";
	}

	void add_text( ImFont* font, float x, float y, const std::string& str, unsigned int color )
	{
		if ( !font )
			return;
		text_draw_object_t text;
		text.m_font       = font;
		text.m_position   = c_vector_2d( x, y );
		text.m_text       = str;
		text.m_color      = color;
		text.m_draw_flags = e_text_flags::text_flag_dropshadow;
		g_render.m_draw_data.emplace_back( e_draw_type::draw_type_text, std::make_any< text_draw_object_t >( text ) );
	}

	float text_width( ImFont* font, const std::string& str )
	{
		if ( !font )
			return 0.f;
		return font->CalcTextSizeA( font->FontSize, FLT_MAX, 0.f, str.c_str( ) ).x;
	}

	constexpr float k_band_height = 25.f;
	constexpr float k_pad         = 8.f;

	std::string two_dp( float v )
	{
		char buf[ 32 ]{ };
		sprintf_s( buf, "%.2f", v );
		return buf;
	}

	constexpr c_unsigned_char_color k_con_text( 180, 180, 180 );
	constexpr c_unsigned_char_color k_con_bad( 220, 120, 120 );

	c_unsigned_char_color con_good( )
	{
		c_color accent = GET_VARIABLE( g_variables.m_accent, c_color );
		return c_unsigned_char_color( accent[ color_type_r ], accent[ color_type_g ], accent[ color_type_b ], 255 );
	}

	void con( const c_unsigned_char_color& colour, const std::string& text )
	{
		if ( !g_interfaces.m_convar )
			return;
		g_interfaces.m_convar->console_color_printf( colour, "%s", text.c_str( ) );
	}

	std::string commas( long double v )
	{
		char raw[ 64 ]{ };
		sprintf_s( raw, "%.0Lf", v < 0.L ? 0.L : v );
		std::string digits( raw );
		std::string out;
		int count = 0;
		for ( std::size_t i = digits.size( ); i-- > 0; ) {
			out.insert( out.begin( ), digits[ i ] );
			if ( ++count % 3 == 0 && i > 0 )
				out.insert( out.begin( ), ',' );
		}
		return out.empty( ) ? "0" : out;
	}

	std::string seconds_text( double s )
	{
		char buf[ 32 ]{ };
		if ( s >= 1.0 )
			sprintf_s( buf, "%.1f", s );
		else
			sprintf_s( buf, "%.3f", s );
		return buf;
	}

	void print_report( const n_route::solve_output_t& r, double seconds, const std::string& fail,
	                   const n_route::solve_input_t& in, const std::vector< int >& marker_no )
	{
		if ( !g_interfaces.m_convar )
			return;

		con( k_con_text, "\n[ " );
		con( con_good( ), "botox" );
		con( k_con_text, " route calculator ]\n\n" );

		{
			char row[ 192 ]{ };
			sprintf_s( row, "start z %.4f ( %s )\n", in.start_z, in.start_on_ground ? "standing" : "in air" );
			con( k_con_text, row );
			for ( std::size_t i = 0; i < in.points.size( ); ++i ) {
				const route_point_t& pt = in.points[ i ];
				const int no            = i < marker_no.size( ) ? marker_no[ i ] : static_cast< int >( i ) + 1;
				const bool died         = static_cast< int >( i ) == r.failed_at;
				std::string state = died ? "<- no arc reached it" : "";
				if ( !died && i < r.arcs_after.size( ) )
					state = std::to_string( r.arcs_after[ i ] ) + " arcs";
				if ( pt.roof < 3.0e38f ) {
					char roof[ 48 ]{ };
					sprintf_s( roof, "  ( ceiling on the way, head %.2f )", pt.roof );
					state += roof;
				}
				sprintf_s( row, "   point %-2d %-10s z %10.4f  %-8s %s\n", no, route_point_type_name( pt.type ), pt.quantized_z,
				           pt.snapped ? "snapped" : ( pt.ent > 0 ? "player" : "aimed" ), state.c_str( ) );
				con( died ? k_con_bad : k_con_text, row );
			}
			con( k_con_text, "\n" );
		}

		con( k_con_text, "the calculation has ended with the reason: " );
		if ( !fail.empty( ) ) {
			con( k_con_bad, fail );
			con( k_con_text, ".\n\n" );
			if ( !r.miss_lines.empty( ) ) {
				con( k_con_text, "closest tries:\n" );
				for ( const auto& line : r.miss_lines )
					con( k_con_text, "   " + line + "\n" );
				con( k_con_text, "\n" );
			}
		} else if ( r.trimmed ) {
			con( k_con_bad, "the search width limit was reached, so some combos were dropped" );
			con( k_con_text, ".\n\n" );
		} else {
			con( con_good( ), "successfully searched all possible combos" );
			con( k_con_text, ".\n\n" );
		}

		con( k_con_text, "total calculation time elapsed: " );
		con( con_good( ), seconds_text( seconds ) );
		con( k_con_text, " seconds.\n\n" );

		const long double pct = r.space > 0.L ? r.searched / r.space * 100.L : 0.L;
		char pct_buf[ 16 ]{ };
		if ( pct >= 99.995L )
			sprintf_s( pct_buf, "%.0Lf", pct );
		else
			sprintf_s( pct_buf, "%.2Lf", pct );
		con( k_con_text, "searched " );
		con( con_good( ), commas( r.searched ) );
		con( k_con_text, " out of " );
		con( con_good( ), commas( r.space ) );
		con( k_con_text, " possible combinations (" );
		con( con_good( ), std::string( pct_buf ) + "%" );
		con( k_con_text, ") and found " );
		con( r.total > 0 ? con_good( ) : k_con_bad, commas( static_cast< long double >( r.total ) ) );
		con( k_con_text, " solutions.\n\n" );

		if ( r.jumps > 0 ) {
			con( k_con_text, "route lengths:\n" );
			for ( int n = 1; n <= r.jumps && n < 16; ++n ) {
				char row[ 128 ]{ };
				if ( r.reach_hits[ n ] > 0 ) {
					sprintf_s( row, "   %d jumps   %d route%s\n", n, r.reach_hits[ n ], r.reach_hits[ n ] == 1 ? "" : "s" );
					con( con_good( ), row );
				} else if ( r.reach_gap[ n ] < 3.4e38f ) {
					sprintf_s( row, "   %d jumps   no route - closest missed by %.3f\n", n, r.reach_gap[ n ] );
					con( k_con_bad, row );
				}
			}
			con( k_con_text, "\n" );
		}

		for ( const auto& s : r.routes )
			con( k_con_text, route_line( s ) + "\n" );

		const long long more = r.total - static_cast< long long >( r.routes.size( ) );
		if ( more > 0 ) {
			con( k_con_text, "\nand +" );
			con( con_good( ), commas( static_cast< long double >( more ) ) );
			con( k_con_text, " more...\n" );
		}
		con( k_con_text, "\n" );
	}
}

namespace
{
	float gravity_per_tick( )
	{
		auto* cv = g_interfaces.m_convar ? g_interfaces.m_convar->find_var( "sv_gravity" ) : nullptr;
		return ( cv ? cv->get_float( ) : 800.f ) * n_tick::engine_interval( );
	}

	struct recorder_t {
		bool started      = false;
		int idle          = 0;
		int presses       = 0;
		float apex        = 0.f;
		float prev_z      = 0.f;
		float prev_vz     = 0.f;
		float prev_stam   = 0.f;
		bool prev_ground  = true;
		bool prev_duck    = false;
		bool prev_walk    = false;
		bool have_prev    = false;
		float tail[ 6 ]{ };
		int tail_n = 0;
		int hang      = 0;
		float hang_z  = 0.f;
		float hang_vz = 0.f;
		c_vector hang_pos{ };
	};
	recorder_t g_rec;

	constexpr float k_wall_reach = 0.5f;

	void hull_trace( const c_vector& a, const c_vector& b, trace_t& t )
	{
		auto* local = g_ctx.m_local;
		auto* col   = local->get_collideable( );
		c_trace_filter flt( local );
		ray_t r( a, b, col->get_obb_mins( ), col->get_obb_maxs( ) );
		g_interfaces.m_engine_trace->trace_ray( r, mask_playersolid, &flt, &t );
	}

	bool rc_start_snap( float& z )
	{
		auto* local = g_ctx.m_local;
		if ( !local || !local->get_collideable( ) )
			return false;
		const c_vector o = local->get_origin( );
		trace_t up, down;
		hull_trace( o, o + c_vector( 0.f, 0.f, 2.f ), up );
		hull_trace( up.m_end, o - c_vector( 0.f, 0.f, 18.f ), down );
		if ( down.m_fraction <= 0.f || down.m_fraction >= 1.f || down.m_start_solid || down.m_plane.m_normal.m_z < 0.7f ||
		     std::fabs( o.m_z - down.m_end.m_z ) <= 0.5f / 32.f )
			return false;
		z = down.m_end.m_z;
		return true;
	}

	bool rc_ceiling_probe( const route_point_t& pt, float& head, bool& at_me )
	{
		auto* local       = g_ctx.m_local;
		const c_vector& n = pt.wall_normal;
		if ( !local || !local->get_collideable( ) || n.m_z > -0.7f )
			return false;
		const c_vector me = local->get_origin( );
		const float me_head = me.m_z + local->get_collideable( )->get_obb_maxs( ).m_z;
		const auto sweep = [ & ]( float x, float y, float from_z ) {
			const float plane  = pt.pos.m_z - ( n.m_x * ( x - pt.pos.m_x ) + n.m_y * ( y - pt.pos.m_y ) ) / n.m_z;
			const float expect = plane - 16.f * ( std::fabs( n.m_x ) + std::fabs( n.m_y ) ) / -n.m_z;
			trace_t t;
			c_trace_filter flt( local );
			ray_t r( c_vector( x, y, ( std::min )( from_z, expect - 2.f ) ), c_vector( x, y, ( std::max )( plane, pt.pos.m_z ) + 8.f ),
			         c_vector( -16.f, -16.f, -1.f ), c_vector( 16.f, 16.f, 0.f ) );
			g_interfaces.m_engine_trace->trace_ray( r, mask_playersolid, &flt, &t );
			if ( t.m_start_solid || t.m_all_solid || t.m_fraction >= 1.f || t.m_plane.m_normal.m_z >= 0.f ||
			     std::fabs( t.m_end.m_z - expect ) > 8.f )
				return false;
			head = t.m_end.m_z;
			return true;
		};
		const float dx = me.m_x - pt.pos.m_x, dy = me.m_y - pt.pos.m_y;
		at_me = dx * dx + dy * dy <= 48.f * 48.f && me_head < pt.pos.m_z && sweep( me.m_x, me.m_y, me_head );
		return at_me || sweep( pt.pos.m_x, pt.pos.m_y, FLT_MAX );
	}

	bool rc_leg_roof( const c_vector& from, const c_vector& to, float dep_z, float& roof, c_vector& at )
	{
		auto* local = g_ctx.m_local;
		if ( !local )
			return false;
		const float head0 = dep_z + 72.f;
		const float top   = head0 + 70.f;
		const float dx = to.m_x - from.m_x, dy = to.m_y - from.m_y;
		const int steps = std::clamp( static_cast< int >( std::sqrt( dx * dx + dy * dy ) / 16.f ), 1, 64 );
		roof = FLT_MAX;
		for ( int k = 0; k <= steps; ++k ) {
			const float f = static_cast< float >( k ) / static_cast< float >( steps );
			const float x = from.m_x + dx * f, y = from.m_y + dy * f;
			trace_t t;
			c_trace_filter flt( local );
			ray_t r( c_vector( x, y, head0 ), c_vector( x, y, top ), c_vector( -16.f, -16.f, -1.f ), c_vector( 16.f, 16.f, 0.f ) );
			g_interfaces.m_engine_trace->trace_ray( r, mask_playersolid, &flt, &t );
			if ( t.m_start_solid || t.m_all_solid || t.m_fraction >= 1.f || t.m_plane.m_normal.m_z >= 0.f )
				continue;
			if ( t.m_end.m_z < roof ) {
				roof = t.m_end.m_z;
				at   = t.m_end;
			}
		}
		return roof < FLT_MAX;
	}

	bool wall_probe( const c_vector& o, c_vector& n, float& gap, float reach = k_wall_reach )
	{
		constexpr float k_d = 0.70710678f;
		constexpr float dirs[ 8 ][ 2 ] = { { 1, 0 }, { k_d, k_d }, { 0, 1 }, { -k_d, k_d }, { -1, 0 }, { -k_d, -k_d }, { 0, -1 }, { k_d, -k_d } };
		float best = FLT_MAX;
		for ( const auto& d2 : dirs ) {
			const c_vector d( d2[ 0 ], d2[ 1 ], 0.f );
			trace_t t;
			hull_trace( o, o + d * reach, t );
			if ( t.m_start_solid || t.m_fraction >= 1.f || std::fabs( t.m_plane.m_normal.m_z ) > 0.7f || t.m_fraction >= best )
				continue;
			best = t.m_fraction;
			n    = c_vector( t.m_plane.m_normal.m_x, t.m_plane.m_normal.m_y, 0.f ).normalized( );
		}
		if ( best == FLT_MAX )
			return false;
		gap                 = -1.f;
		const auto overlaps = [ & ]( const c_vector& p ) {
			trace_t t;
			hull_trace( p, p, t );
			return t.m_start_solid || t.m_all_solid;
		};
		const float k_far = reach + 0.1f;
		if ( overlaps( o ) || !overlaps( o - n * k_far ) )
			return true;
		float lo = 0.f;
		gap      = k_far;
		for ( int i = 0; i < 24; ++i ) {
			const float mid = 0.5f * ( lo + gap );
			if ( overlaps( o - n * mid ) )
				gap = mid;
			else
				lo = mid;
		}
		return true;
	}

	c_vector log_wall_ground( const char* what, const float* at_z = nullptr )
	{
		auto* local = g_ctx.m_local;
		if ( !local || !local->get_collideable( ) )
			return { };
		c_vector o = local->get_origin( );
		if ( at_z )
			o.m_z = *at_z;
		c_vector n{ };
		float gap = -1.f;
		if ( !wall_probe( o, n, gap ) ) {
			if ( at_z )
				botox_dbg_log( "[rc lip] %s z %.4f | no wall within %.1fu\n", what, o.m_z, k_wall_reach );
			return { };
		}
		if ( gap < 0.f )
			return n;
		const auto ground = [ & ]( float out, float& pz ) {
			const c_vector p = o + n * ( out - gap );
			trace_t t;
			hull_trace( p, p - c_vector( 0.f, 0.f, 2.f ), t );
			pz = t.m_end.m_z;
			return !t.m_start_solid && t.m_fraction < 1.f && t.m_plane.m_normal.m_z >= 0.7f;
		};
		float pz        = 0.f;
		const bool under = ground( gap, pz );
		char reach[ 96 ]{ };
		int used = 0;
		for ( const float out : { 0.002f, 0.00928f, 0.03125f, 0.0325f, 1.f } ) {
			float z2        = 0.f;
			const bool same = under && ground( out, z2 ) && std::fabs( z2 - pz ) < 0.01f;
			used += sprintf_s( reach + used, sizeof( reach ) - used, " %g%s", out, same ? "+" : "-" );
		}
		botox_dbg_log( "[rc lip] %s z %.4f | wall n %.2f %.2f gap %.5f | 2u ground %s %.4f | that plane off the face:%s\n", what, o.m_z,
		               n.m_x, n.m_y, gap, under ? "HIT" : "none", under ? pz : 0.f, under ? reach : " -" );
		return n;
	}

	int pj_lip_kind( const c_vector& at, float pz, float& gap_out )
	{
		constexpr float k_d  = 0.70710678f;
		constexpr float dirs[ 8 ][ 2 ] = { { 1, 0 }, { k_d, k_d }, { 0, 1 }, { -k_d, k_d }, { -1, 0 }, { -k_d, -k_d }, { 0, -1 }, { k_d, -k_d } };
		const float top      = pz - 0.03125f;
		const c_vector base( at.m_x, at.m_y, pz + 1.f );
		for ( const auto& d2 : dirs ) {
			const c_vector d( d2[ 0 ], d2[ 1 ], 0.f );
			const c_vector o = base + d * ( 16.f * ( std::fabs( d.m_x ) + std::fabs( d.m_y ) ) + 8.f );
			trace_t w;
			hull_trace( o, o - d * 12.f, w );
			if ( w.m_start_solid || w.m_fraction >= 1.f || std::fabs( w.m_plane.m_normal.m_z ) > 0.7f )
				continue;
			const c_vector n = c_vector( w.m_plane.m_normal.m_x, w.m_plane.m_normal.m_y, 0.f ).normalized( );
			const float sup  = 16.f * ( std::fabs( n.m_x ) + std::fabs( n.m_y ) );
			gap_out = ( w.m_end - base ).dot_product( n ) - sup;
			if ( n.dot_product( d ) < 0.5f || std::fabs( gap_out ) > 0.1f )
				continue;
			const c_vector q = base + n * ( sup + 0.001f );
			const auto on_lip = [ & ]( const trace_t& t ) {
				return !t.m_start_solid && t.m_fraction < 1.f && t.m_plane.m_normal.m_z >= 0.7f && std::fabs( t.m_end.m_z - pz ) < 0.05f;
			};
			trace_t t;
			hull_trace( q, q - c_vector( 0.f, 0.f, 2.f ), t );
			if ( on_lip( t ) )
				return 0;
			const c_vector s( q.m_x, q.m_y, top + 2.015f );
			hull_trace( s, s - c_vector( 0.f, 0.f, 2.f ), t );
			return on_lip( t ) ? 1 : -1;
		}
		return -1;
	}

	void record_tick( n_movement::impl_t::route_calc_t& data )
	{
		auto* local = g_ctx.m_local;
		if ( !local )
			return;

		const int flags        = local->get_flags( );
		const bool on_ground   = ( flags & fl_onground ) != 0;
		const bool ducking     = ( flags & fl_ducking ) != 0;
		const float z          = local->get_origin( ).m_z;
		const float vz         = local->get_velocity( ).m_z;
		const float stam_raw   = local->get_stamina( );
		const float stam       = ( stam_raw >= 0.f && stam_raw <= 200.f ) ? stam_raw : -1.f;

		if ( g_rec.have_prev ) {
			if ( g_rec.prev_ground && !on_ground ) {
				++g_rec.presses;
				g_rec.started = true;
				g_rec.apex    = z;
				g_rec.tail_n  = 0;
				/* launch z off THIS tick ( the launch tick's move = vz + half a gravity step ). a jumpbug's `from` / stam
				   read a landing that never happened ( log 40862: from 17.649 stam 12.06, the catch says it left 17.876 ) */
				const float launch_z = z - ( vz + 0.5f * gravity_per_tick( ) ) * n_tick::engine_interval( );
				botox_dbg_log( "[rc rec] press %d from %9.3f stam %5.2f duck %d | launch z %9.3f vz %7.2f\n", g_rec.presses,
				               g_rec.prev_z, g_rec.prev_stam, g_rec.prev_duck ? 1 : 0, launch_z, vz );
			}
			if ( !on_ground ) {
				if ( z > g_rec.apex )
					g_rec.apex = z;
				if ( g_rec.tail_n < 6 ) {
					g_rec.tail[ g_rec.tail_n++ ] = z;
				} else {
					for ( int i = 0; i < 5; ++i )
						g_rec.tail[ i ] = g_rec.tail[ i + 1 ];
					g_rec.tail[ 5 ] = z;
				}
			}
			if ( !g_rec.prev_ground && on_ground && g_rec.started ) {
				char tail[ 96 ]{ };
				int used = 0;
				for ( int i = 0; i < g_rec.tail_n && used < 80; ++i )
					used += sprintf_s( tail + used, sizeof( tail ) - used, "%.3f ", g_rec.tail[ i ] );
				botox_dbg_log( "[rc rec]   -> apex %9.3f land %9.3f fall %6.1f stam %5.2f duck %d | last ticks %s\n",
				                   g_rec.apex, z, -g_rec.prev_vz, stam, ducking ? 1 : 0, tail );
				log_wall_ground( "land" );
			}
		}

		/* [rc surf]: a pixel surf never sets FL_ONGROUND, so press/land lines are blind to it. the catch
		   is the tick the fall STOPS with no ground; that z is the pixel. */
		/* test VELOCITY, not height ( the apex barely moves either ): a real fall loses a whole gravity
		   step per tick, a clipped move ~0. half a step splits the two at any tickrate, never a constant. */
		const bool clipped =
			!on_ground && g_rec.prev_vz <= 0.f && vz > g_rec.prev_vz - gravity_per_tick( ) * 0.5f;
		if ( g_rec.have_prev && clipped && !g_rec.prev_ground ) {
			if ( ++g_rec.hang == 1 ) {
				g_rec.hang_z  = z;
				g_rec.hang_vz = -g_rec.prev_vz;
				g_rec.hang_pos = local->get_origin( );
			}
			if ( g_rec.hang == 2 ) {
				botox_dbg_log( "[rc surf] CAUGHT at z %9.4f  ( arrived at %6.1f, duck %d ) at %.2f %.2f\n", g_rec.hang_z,
				               g_rec.hang_vz, ducking ? 1 : 0, g_rec.hang_pos.m_x, g_rec.hang_pos.m_y );
				if ( const c_vector wall_n = log_wall_ground( "surf" ); wall_n.length_2d( ) > 0.5f )
					pf_check_catch( g_rec.hang_pos, wall_n, local->get_velocity( ).dot_product( c_vector( wall_n.m_y, -wall_n.m_x, 0.f ) ), ducking );
				for ( std::size_t i = 0; i < data.points.size( ); ++i ) {
					route_point_t& pt = data.points[ i ];
					if ( pt.type != route_pt_pixelsurf )
						continue;
					const float dx = pt.pos.m_x - g_rec.hang_pos.m_x;
					const float dy = pt.pos.m_y - g_rec.hang_pos.m_y;
					if ( dx * dx + dy * dy > 32.f * 32.f || fabsf( pt.pos.m_z - g_rec.hang_z ) > 1.f )
						continue;
					if ( pt.has_measured && g_rec.hang_z <= pt.measured_z + 0.0005f )
						continue;
					pt.measured_z   = g_rec.hang_z;
					pt.has_measured = true;
					botox_dbg_log( "[rc surf] marker %d LEARNED: catches at %9.4f ( %.4f under its %9.4f )\n",
					               static_cast< int >( i ) + 1, pt.measured_z, pt.pos.m_z - pt.measured_z, pt.pos.m_z );
					break;
				}
			}
		} else {
			if ( g_rec.hang >= 2 )
				botox_dbg_log( "[rc surf] left after %d ticks, z %9.4f -> %9.4f\n", g_rec.hang, g_rec.hang_z, z );
			g_rec.hang = 0;
		}

		const bool walk = local->get_move_type( ) == e_move_types::move_type_walk;
		if ( g_rec.have_prev && !g_rec.prev_ground && !on_ground && walk && g_rec.prev_walk && g_rec.prev_vz > gravity_per_tick( ) &&
		     vz < g_rec.prev_vz - gravity_per_tick( ) * 1.5f ) {
			const c_vector o = local->get_origin( );
			botox_dbg_log( "[rc bang] head stopped at %9.4f ( vz %7.2f -> %7.2f, duck %d ) at %.2f %.2f\n", z + ( ducking ? 54.f : 72.f ),
			               g_rec.prev_vz, vz, ducking ? 1 : 0, o.m_x, o.m_y );
		}

		if ( g_rec.have_prev && !g_rec.prev_ground ) {
			const c_vector o = local->get_origin( );
			for ( std::size_t i = 0; i < data.points.size( ); ++i ) {
				const route_point_t& pt = data.points[ i ];
				if ( pt.type != route_pt_pixeljump )
					continue;
				const float dx = pt.pos.m_x - o.m_x, dy = pt.pos.m_y - o.m_y;
				if ( dx * dx + dy * dy > 40.f * 40.f )
					continue;
				const float pz   = pt.pos.m_z;
				const bool land  = on_ground && fabsf( z - pz ) < 2.1f;
				const bool past  = !on_ground && g_rec.prev_z >= pz - 0.03125f && z < pz - 0.03125f;
				if ( !land && !past )
					continue;
				const char* what = past ? "FELL PAST" : fabsf( z - pz ) < 0.05f ? "LANDED lip" : "LANDED hover";
				botox_dbg_log( "[rc pj] marker %d %s: z %9.4f -> %9.4f ( over plane %.4f -> %.4f ) vz %7.2f -> %7.2f duck %d -> %d\n",
				               static_cast< int >( i ) + 1, what, g_rec.prev_z, z, g_rec.prev_z - pz, z - pz,
				               g_rec.prev_vz, vz, g_rec.prev_duck ? 1 : 0, ducking ? 1 : 0 );
				const float probe_z = pz + 1.f;
				if ( past || !g_rec.started )
					log_wall_ground( land ? "pj land" : "pj past", land ? nullptr : &probe_z );
			}
		}

		if ( g_rec.started && on_ground ) {
			if ( ++g_rec.idle > static_cast< int >( 0.5f / n_tick::engine_interval( ) ) ) {
				botox_dbg_log( "[rc rec] chain over — %d presses\n", g_rec.presses );
				g_rec = recorder_t{ };
			}
		} else if ( !on_ground ) {
			g_rec.idle = 0;
		}

		g_rec.prev_z      = z;
		g_rec.prev_vz     = vz;
		g_rec.prev_stam   = stam;
		g_rec.prev_ground = on_ground;
		g_rec.prev_duck   = ducking;
		g_rec.prev_walk   = walk;
		g_rec.have_prev   = true;
	}
}

int n_route::lip_kind( const c_vector& at, float pz, float& gap_out )
{
	return pj_lip_kind( at, pz, gap_out );
}

float n_route::floor_nz( )
{
	auto* local = g_ctx.m_local;
	if ( !local || !local->get_collideable( ) )
		return 1.f;
	const c_vector o = local->get_origin( );
	trace_t t;
	hull_trace( o, o - c_vector( 0.f, 0.f, 2.f ), t );
	return t.m_start_solid || t.m_fraction >= 1.f ? 1.f : t.m_plane.m_normal.m_z;
}

void n_movement::impl_t::route_pj_creep( c_user_cmd* cmd )
{
	constexpr int k_creep_ticks   = 10;
	constexpr float k_creep_done  = n_route::k_creep_gap;
	constexpr float k_creep_share = 0.75f;
	constexpr float k_catch       = 1.96875f;

	auto* local = g_ctx.m_local;
	if ( !cmd || !local || !local->is_alive( ) || !local->get_collideable( ) )
		return;
	if ( !GET_VARIABLE( g_variables.m_route_calc, bool ) )
		return;
	if ( local->get_move_type( ) != e_move_types::move_type_walk || ( local->get_flags( ) & fl_onground ) )
		return;
	if ( g_air_stuck_owns_cmd || m_pixelsurf_data.should_pixel_surf )
		return;

	const c_vector o  = local->get_origin( );
	const c_vector v  = local->get_velocity( );
	const float dt    = n_tick::engine_interval( ), half_g = gravity_per_tick( ) * 0.5f;
	const auto ticks_to = [ & ]( float top ) {
		float z = o.m_z, vz = v.m_z;
		for ( int i = 1; i <= k_creep_ticks; ++i ) {
			vz -= half_g;
			z += vz * dt;
			if ( vz <= 140.f && z <= top )
				return i;
			vz -= half_g;
		}
		return -1;
	};

	int idx = -1, ticks = k_creep_ticks + 1;
	float pz = 0.f;
	for ( std::size_t i = 0; i < m_route_calc_data.points.size( ); ++i ) {
		const route_point_t& pt = m_route_calc_data.points[ i ];
		if ( pt.type != route_pt_pixeljump || !pt.enabled || pt.ent > 0 )
			continue;
		const float dx = pt.pos.m_x - o.m_x, dy = pt.pos.m_y - o.m_y, z = n_route::target_z( pt );
		if ( dx * dx + dy * dy > 40.f * 40.f || o.m_z < z - 0.03125f )
			continue;
		const int t = ticks_to( z + ( pt.pj_skin ? n_route::k_skin_hi : k_catch ) );
		if ( t < 0 || t >= ticks )
			continue;
		ticks = t;
		idx   = static_cast< int >( i );
		pz    = z;
	}
	if ( idx < 0 )
		return;
	for ( const route_point_t& pt : m_route_calc_data.points ) {
		if ( !pt.enabled || pt.type == route_pt_ground || pt.type == route_pt_pixeljump || pt.type == route_pt_headbang ||
		     pt.type == route_pt_headbounce )
			continue;
		const float dx = pt.pos.m_x - o.m_x, dy = pt.pos.m_y - o.m_y, z = n_route::target_z( pt );
		if ( dx * dx + dy * dy <= 40.f * 40.f && z > pz + k_catch && z <= o.m_z + 0.03125f )
			return;
	}

	c_vector n{ };
	float gap = -1.f;
	if ( !wall_probe( o, n, gap, 1.f ) || gap < 0.f )
		return;
	const float support = local->get_collideable( )->get_obb_maxs( ).m_x * ( std::fabs( n.m_x ) + std::fabs( n.m_y ) );
	if ( std::fabs( ( o.m_x - m_route_calc_data.points[ idx ].pos.m_x ) * n.m_x + ( o.m_y - m_route_calc_data.points[ idx ].pos.m_y ) * n.m_y -
	                support - gap ) > 1.f )
		return;

	const c_vector into( -n.m_x, -n.m_y, 0.f );
	const float v_in = v.m_x * into.m_x + v.m_y * into.m_y;
	const float want = gap > k_creep_done ? gap * k_creep_share / dt : 0.f;
	const bool push  = want > v_in;
	const float need = std::fabs( want - v_in );
	const float cur  = push ? v_in : -v_in;
	const float friction = v.m_z + half_g > 0.f ? 0.25f : 1.f;
	const float accel    = g_convars.float_or( HASH_BT( "sv_airaccelerate" ), 12.f ) * dt * friction;
	const float cap   = g_convars.float_or( HASH_BT( "sv_air_max_wishspeed" ), 30.f );
	const auto add    = [ & ]( float w ) { return ( std::min )( accel * w, ( std::min )( w, cap ) - cur ); };
	float lo = 0.f, w = 250.f;
	if ( add( w ) > need ) {
		for ( int i = 0; i < 32; ++i ) {
			const float mid = 0.5f * ( lo + w );
			if ( add( mid ) >= need )
				w = mid;
			else
				lo = mid;
		}
	}
	if ( cmd->m_buttons & in_duck )
		w /= 0.34f;
	if ( need <= 0.f )
		w = 0.f;

	const c_vector wish = ( push ? into : into * -1.f ) * w;
	const float yaw     = cmd->m_view_point.m_y * ( 3.14159265f / 180.f );
	cmd->m_forward_move = wish.m_x * std::cos( yaw ) + wish.m_y * std::sin( yaw );
	cmd->m_side_move    = wish.m_x * std::sin( yaw ) - wish.m_y * std::cos( yaw );

	botox_dbg_log( "[rc creep] marker %d | catch in %d | gap %.5f v_in %.3f -> want %.3f ( %s w %.3f )\n", idx + 1, ticks, gap, v_in, want,
	               push ? "in" : "out", w );
}

namespace
{
	/* solve runs on a worker ( 10-06 log: one solve held the game thread 4.6 - 5.2 s ). route_calc( ) builds the job,
	   polls it every tick and runs rc_finish( ) back on the game thread ( console, toasts ). */
	struct rc_job_t {
		n_route::solve_input_t in{ };
		n_route::solve_output_t out{ };
		std::vector< int > marker_no{ };
		bool advanced  = false;
		int gen        = 0;
		double seconds = 0.0;
		std::string fail{ }, hint{ }, example{ };
		std::atomic< bool > done{ false };
	};

	std::shared_ptr< rc_job_t > g_rc_job{ };
	std::atomic< int > g_rc_permille{ -1 };
	std::atomic< bool > g_rc_cancel{ false };
	std::atomic< bool > g_rc_running{ false };

	void rc_work( rc_job_t& job )
	{
		const auto clock_start = std::chrono::steady_clock::now( );
		n_route::solve( job.in, job.out );
		job.seconds = std::chrono::duration< double >( std::chrono::steady_clock::now( ) - clock_start ).count( );
		const n_route::solve_output_t& result = job.out;
		if ( !result.routes.empty( ) || ( job.in.cancel && job.in.cancel->load( ) ) )
			return;

		const int idx      = result.failed_at >= 0 && result.failed_at < static_cast< int >( job.marker_no.size( ) ) ? result.failed_at : 0;
		const int point_no = job.marker_no.empty( ) ? idx + 1 : job.marker_no[ idx ];
		char buf[ 192 ]{ };
		if ( result.best_gap < 3.4e38f )
			sprintf_s( buf, "point %d unreachable - wants z %.4f, closest %.4f (off by %.4f)", point_no,
			           result.want_z, result.closest_z, result.best_gap );
		else if ( result.blocked_moves != 0u )
			sprintf_s( buf, "point %d needs %s - turned off in jump types", point_no,
			           n_route::move_list( result.blocked_moves, " or " ).c_str( ) );
		else
			sprintf_s( buf, "point %d unreachable - no arc got near it", point_no );
		job.fail = buf;
		botox_dbg_log( "[rc] %s\n", buf );
		if ( job.advanced )
			job.hint = n_route::solve_hint( job.in, result.refused_moves, job.example );
	}

	void rc_finish( n_movement::impl_t::route_calc_t& data, rc_job_t& job )
	{
		const auto say = []( int type, const std::string& text ) {
			if ( !GET_VARIABLE( g_variables.m_route_calc_show_bar, bool ) )
				movement_add_window( type, text );
		};
		const n_route::solve_output_t& result = job.out;
		/* popup clock restarts on the answer, not the press */
		data.popup_started   = GetTickCount64( ) / 1000.f;
		data.solutions       = result.routes;
		data.total_solutions = result.total;
		data.solved          = !result.routes.empty( );

		if ( data.solved ) {
			say( 6, "route: " + std::to_string( result.total ) + " combos" );
			say( 6, n_route::route_line( data.solutions.front( ) ) );
			botox_dbg_log( "[rc] solved %lld combos, best: %s\n", result.total, n_route::route_line( data.solutions.front( ) ).c_str( ) );
			print_report( result, job.seconds, "", job.in, job.marker_no );
			return;
		}
		data.message = job.advanced ? job.fail : "no solutions found";
		say( 3, "route: " + data.message );
		if ( !job.hint.empty( ) ) {
			data.note = "with " + job.hint + " on ( off in jump types ): " + job.example;
			say( 3, "route: " + data.note );
			botox_dbg_log( "[rc] %s\n", data.note.c_str( ) );
		}
		print_report( result, job.seconds, job.fail, job.in, job.marker_no );
		if ( !data.note.empty( ) )
			con( k_con_bad, data.note + "\n\n" );
	}

	void rc_start( const std::shared_ptr< rc_job_t >& job )
	{
		g_rc_cancel.store( false );
		g_rc_permille.store( 0 );
		g_rc_running.store( true );
		job->in.progress = &g_rc_permille;
		job->in.cancel   = &g_rc_cancel;
		g_rc_job         = job;
		std::thread( [ job ]( ) {
			n_perf::background_thread( );
			rc_work( *job );
			/* running drops first: the next job can only start after rc_poll saw done */
			g_rc_running.store( false );
			job->done.store( true );
		} ).detach( );
	}

	/* game thread, every tick: cancel a job whose points changed, finish a done one */
	void rc_poll( n_movement::impl_t::route_calc_t& data )
	{
		if ( !g_rc_job )
			return;
		if ( g_rc_job->gen != data.gen )
			g_rc_cancel.store( true );
		if ( !g_rc_job->done.load( ) )
			return;
		const std::shared_ptr< rc_job_t > job = std::move( g_rc_job );
		g_rc_permille.store( -1 );
		if ( job->gen != data.gen ) {
			botox_dbg_log( "[rc] worker result dropped: points or settings changed while calculating\n" );
			return;
		}
		botox_dbg_log( "[rc] worker: solve %.0f ms off the game thread\n", job->seconds * 1000.0 );
		rc_finish( data, *job );
	}
}

bool n_route::solve_busy( )
{
	return g_rc_running.load( );
}

void n_route::route_calc_shutdown( )
{
	if ( !g_rc_running.load( ) && !g_rc_job )
		return;
	g_rc_cancel.store( true );
	for ( int waited = 0; g_rc_running.load( ) && waited < 300; ++waited )
		Sleep( 10 );
	/* the lambda epilogue + std::thread wrapper still run our code after the flag drops */
	Sleep( 20 );
}

void n_movement::impl_t::route_calc( c_user_cmd* cmd )
{
	if ( !g_ctx.m_local || !g_ctx.m_local->is_alive( ) )
		return;
	if ( !GET_VARIABLE( g_variables.m_route_calc, bool ) )
		return;

	auto& data = m_route_calc_data;

	if ( cmd )
		record_tick( data );

	if ( !data.map.empty( ) && data.map != g_interfaces.m_engine_client->get_level_name_short( ) )
		data.clear( );

	for ( auto& pt : data.points )
		rc_follow( pt );

	static bool add_prev = false;
	const bool add_now   = bind_down( GET_VARIABLE( g_variables.m_route_calc_add_key, key_bind_t ) );
	if ( cmd && add_now && !add_prev ) {
		c_angle cam{ };
		g_interfaces.m_engine_client->get_view_angles( cam );
		const c_vector eye     = g_ctx.m_local->get_eye_position( );
		const c_vector forward = c_vector::fromAngle( c_vector( cam.m_x, cam.m_y, 0.f ) );

		trace_t trace;
		if ( !rc_aim_world( eye, forward, trace ) ) {
			movement_add_window( 3, "route: nothing under the crosshair" );
			add_prev = add_now;
			return;
		}
		c_vector hit = trace.m_end;

		c_base_entity* on_player = trace.m_hit_entity;
		if ( on_player && ( on_player == g_ctx.m_local || !on_player->is_player( ) ) )
			on_player = nullptr;

		constexpr float k_snap_world = 8.f;
		constexpr float k_snap_ring  = 12.f;
		const c_vector_2d centre( g_ctx.m_width * 0.5f, g_ctx.m_height * 0.5f );
		const float hit_dist = hit.dist_to( eye );
		float best_dist = FLT_MAX;
		c_vector snapped{ };
		bool snap_found = false;
		bool head_tb    = false;
		float head_dot_z = 0.f;
		std::vector< std::pair< float, float > > near_dots;
		int snap_type = route_pt_pixelsurf;
		const auto try_snap = [ & ]( const std::vector< AnimatedPoint >& points, int type ) {
			for ( const auto& ap : points ) {
				if ( ap.is_removing )
					continue;
				const float d = ap.position.dist_to( hit );
				c_vector_2d s;
				const bool drawn = pf_dot_screen( ap, s );
				/* rings draw through walls: a ring-only pick must not sit behind what the ray hit */
				const bool ringed = drawn && s.distance( centre ) <= k_snap_ring && ap.position.dist_to( eye ) < hit_dist + 32.f;
				const float px    = drawn ? s.distance( centre ) : FLT_MAX;
				if ( d < 48.f || ringed )
					near_dots.emplace_back( d, ap.position.m_z );
				if ( d >= k_snap_world && !ringed )
					continue;
				const float rank = drawn ? px : 1e6f + d;
				if ( rank < best_dist ) {
					best_dist  = rank;
					snapped    = ap.position;
					snap_type  = type;
					snap_found = true;
				}
			}
		};
		if ( !on_player ) {
			try_snap( animated_points, route_pt_pixelsurf );
			try_snap( animated_points2, route_pt_headbounce );
			try_snap( animated_points3, route_pt_texturebug );
			try_snap( animated_points4, route_pt_pixeljump );
			if ( snap_found && snap_type == route_pt_texturebug )
				pf_tb_rise_seam( snapped );
			if ( snap_found && snap_type == route_pt_texturebug && pf_tb_is_head( snapped ) ) {
				snap_type  = route_pt_pixelsurf;
				head_dot_z = snapped.m_z;
				snapped.m_z += pf_head_over( ) - 54.f;
				head_tb   = true;
			}
		}

		{
			std::sort( near_dots.begin( ), near_dots.end( ),
			           []( const auto& a, const auto& b ) { return a.first < b.first; } );
			std::string dots;
			for ( std::size_t i = 0; i < near_dots.size( ) && i < 5; ++i ) {
				char cell[ 48 ]{ };
				sprintf_s( cell, "%s%.4f (d %.2f)", dots.empty( ) ? "" : ", ", near_dots[ i ].second, near_dots[ i ].first );
				dots += cell;
			}
			botox_dbg_log( "[rc snap] aim hit z %.4f | %d finder dots within 48u: %s | eye %.1f %.1f %.1f cam %.1f/%.1f cmd %.1f/%.1f\n", hit.m_z,
			               static_cast< int >( near_dots.size( ) ), dots.empty( ) ? "NONE" : dots.c_str( ), eye.m_x, eye.m_y, eye.m_z, cam.m_x,
			               cam.m_y, cmd->m_view_point.m_x, cmd->m_view_point.m_y );
		}

		route_point_t point;
		bool clip_top   = false;
		float stand_up  = 0.f;
		bool stand_clip = false;
		if ( on_player ) {
			const c_vector o = on_player->get_abs_origin( );
			point.ent        = on_player->get_index( );
			point.ent_offset = c_vector( hit.m_x - o.m_x, hit.m_y - o.m_y, 0.f );
			point.type       = trace.m_plane.m_normal.m_z < -0.7f ? route_pt_headbang : route_pt_ground;
			point.pos        = hit;
			rc_follow( point );
		} else if ( snap_found ) {
			point.pos       = snapped;
			point.snapped   = true;
			point.type       = snap_type;
			point.snap_type  = snap_type;
			point.hb_ceiling = snap_type == route_pt_headbounce && pf_hb_is_ceiling( snapped );
			if ( head_tb ) {
				point.allow_stand = false;
				point.head_tb     = true;
				point.mark_z      = head_dot_z;
			}
		} else {
			if ( rc_ray_clip && std::fabs( trace.m_plane.m_normal.m_z ) <= 0.7f ) {
				c_vector top;
				if ( rc_clip_top( trace, top ) ) {
					hit      = top;
					clip_top = true;
				} else {
					trace_t through;
					ray_t ray( eye, eye + forward * 8192.f );
					c_trace_filter flt( g_ctx.m_local );
					g_interfaces.m_engine_trace->trace_ray( ray, k_mask_noclip, &flt, &through );
					if ( through.m_fraction < 1.f && !through.m_start_solid ) {
						trace       = through;
						hit         = through.m_end;
						rc_ray_clip = false;
					}
				}
			}
			point.pos         = hit;
			point.wall_normal = clip_top ? c_vector( 0.f, 0.f, 1.f ) : trace.m_plane.m_normal;
			const float nz       = clip_top ? 1.f : trace.m_plane.m_normal.m_z;
			const bool standable = nz > 0.7f;
			const bool ceiling   = nz < -0.7f;
			const bool allow_any = GET_VARIABLE( g_variables.m_route_calc_allow_invalid, bool );
			const bool as_pixel  = !standable || allow_any;
			point.type = ceiling ? route_pt_headbang : ( as_pixel ? route_pt_pixelsurf : route_pt_ground );
			if ( point.type == route_pt_pixelsurf && !allow_any ) {
				movement_add_window( 3, "route: no finder dot here - scan the wall with the pixel finder and aim at a "
				                        "green (pixelsurf), yellow (pixeljump) or cyan (texturebug) dot" );
				botox_dbg_log( "[rc add] refused: wall hit z %.4f with no finder dot within %.0fu\n", hit.m_z, k_snap_world );
				add_prev = add_now;
				return;
			}
			if ( point.type == route_pt_ground && GET_VARIABLE( g_variables.m_route_calc_snap_edge, bool ) ) {
				hit       = movement_nearest_edge( hit, 50.f );
				point.pos = hit;
				trace_t under;
				ray_t down( hit + c_vector( 0.f, 0.f, 4.f ), hit - c_vector( 0.f, 0.f, 4.f ) );
				c_trace_filter under_flt( g_ctx.m_local );
				g_interfaces.m_engine_trace->trace_ray( down, mask_playersolid, &under_flt, &under );
				if ( !under.m_start_solid && under.m_fraction < 1.f && under.m_plane.m_normal.m_z > 0.7f )
					point.wall_normal = under.m_plane.m_normal;
			}
			if ( point.type == route_pt_ground && g_ctx.m_local->get_collideable( ) ) {
				const c_vector& fn        = point.wall_normal;
				const float corner        = fn.m_z > 0.7f ? 16.f * ( std::fabs( fn.m_x ) + std::fabs( fn.m_y ) ) / fn.m_z : 0.f;
				const float k_stand_reach = 2.f + corner;
				trace_t stand;
				hull_trace( hit + c_vector( 0.f, 0.f, k_stand_reach ), hit - c_vector( 0.f, 0.f, 0.5f ), stand );
				const c_vector& sn  = stand.m_plane.m_normal;
				const bool on_aimed = stand.m_end.m_z - hit.m_z <= 2.f || sn.m_x * fn.m_x + sn.m_y * fn.m_y + sn.m_z * fn.m_z > 0.99f;
				if ( !stand.m_start_solid && stand.m_fraction < 1.f && sn.m_z > 0.7f && stand.m_end.m_z > hit.m_z + 0.001f && on_aimed ) {
					stand_up      = stand.m_end.m_z - hit.m_z;
					stand_clip    = ( stand.m_contents & contents_playerclip ) != 0;
					point.pos.m_z = stand.m_end.m_z;
				}
			}
		}
		point.quantized_z = n_route::target_z( point );

		data.points.emplace_back( point );
		data.map = g_interfaces.m_engine_client->get_level_name_short( );
		data.drop_solutions( );
		std::string placed = "route: point " + std::to_string( data.points.size( ) ) + " (" +
		                     route_point_type_name( route_point_shown_type( point ) ) + ", z " + two_dp( point.quantized_z ) + ")";
		if ( point.head_tb )
			placed += " - head seam at " + two_dp( point.mark_z ) + ", solved as a ducked pixelsurf";
		if ( on_player )
			placed += " on a player (follows them)";
		else if ( clip_top )
			placed += " on top of a player clip (invisible)";
		else if ( rc_ray_clip && !snap_found )
			placed += " on a player clip (invisible, but it is what you stand on)";
		if ( stand_up > 0.f )
			placed += " - you stand " + two_dp( stand_up ) + " higher here" + ( stand_clip ? " (on an invisible player clip)" : "" );
		if ( point.type == route_pt_pixelsurf && !snap_found )
			placed += " - NOT a finder dot, unverified";
		if ( point.hb_ceiling )
			placed += " - under a ceiling";
		movement_add_window( 3, GET_VARIABLE( g_variables.m_route_calc_advanced_readout, bool )
		                            ? placed
		                            : "added " + std::string( route_point_type_name( route_point_shown_type( point ) ) ) + " point at " +
		                                  two_dp( point.head_tb ? point.mark_z : point.quantized_z ) );
		botox_dbg_log( "[rc add] %s | plane n %.2f %.2f %.2f contents 0x%X%s | at %.2f %.2f, hull stand +%.4f\n", placed.c_str( ),
		               trace.m_plane.m_normal.m_x, trace.m_plane.m_normal.m_y, trace.m_plane.m_normal.m_z,
		               trace.m_contents, snap_found ? " ( snapped to a finder dot )" : "", point.pos.m_x, point.pos.m_y, stand_up );
	}
	add_prev = add_now;

	static bool del_prev = false;
	const bool del_now   = bind_down( GET_VARIABLE( g_variables.m_route_calc_delete_key, key_bind_t ) );
	if ( del_now && !del_prev && !data.points.empty( ) ) {
		std::size_t best = data.points.size( );
		float best_dist  = FLT_MAX;
		for ( std::size_t i = 0; i < data.points.size( ); ++i ) {
			c_vector_2d screen_pos;
			if ( !g_render.world_to_screen( route_point_mark( data.points[ i ] ), screen_pos ) )
				continue;
			const float dx = screen_pos.m_x - g_ctx.m_width * 0.5f;
			const float dy = screen_pos.m_y - g_ctx.m_height * 0.5f;
			const float d  = sqrtf( dx * dx + dy * dy );
			if ( d < 60.f && d < best_dist ) {
				best_dist = d;
				best      = i;
			}
		}
		if ( best >= data.points.size( ) )
			best = data.points.size( ) - 1;
		data.points.erase( data.points.begin( ) + static_cast< long >( best ) );
		if ( data.selected_row > static_cast< int >( data.points.size( ) ) )
			data.selected_row = static_cast< int >( data.points.size( ) );
		data.drop_solutions( );
	}
	del_prev = del_now;

	static bool clear_prev = false;
	const bool clear_now   = bind_down( GET_VARIABLE( g_variables.m_route_calc_clear_key, key_bind_t ) );
	if ( clear_now && !clear_prev )
		data.clear( );
	clear_prev = clear_now;

	rc_poll( data );

	static bool solve_prev = false;
	const bool solve_now   = bind_down( GET_VARIABLE( g_variables.m_route_calc_solve_key, key_bind_t ) );
	const bool go          = solve_now && !solve_prev;
	solve_prev             = solve_now;
	/* one solve at a time: a press while calculating is ignored ( drop_solutions would cancel it ) */
	if ( !go || !cmd || g_rc_job )
		return;

	data.drop_solutions( );
	/* popup clock starts on the PRESS: every exit is a route list or a message, both popup.
	   ( drop_solutions zeroes it, so editing points hides a stale box ) */
	data.popup_started = GetTickCount64( ) / 1000.f;

	const auto say = []( int type, const std::string& text ) {
		if ( !GET_VARIABLE( g_variables.m_route_calc_show_bar, bool ) )
			movement_add_window( type, text );
	};
	const auto refuse = [ & ]( ) {
		say( 3, "route: " + data.message );
		con( k_con_bad, "[ botox route calculator ] " + data.message + "\n" );
	};

	const bool start_here = GET_VARIABLE( g_variables.m_route_calc_start_here, bool );
	if ( data.points.empty( ) || ( !start_here && data.points.size( ) < 2 ) ) {
		data.message = start_here ? "add a point to jump to" : "add a start point and a point to jump to";
		refuse( );
		return;
	}

	const auto job             = std::make_shared< rc_job_t >( );
	n_route::solve_input_t& in = job->in;
	in.delay_ticks = GET_VARIABLE( g_variables.m_route_calc_delay_ticks, int );
	in.max_results = GET_VARIABLE( g_variables.m_route_calc_max_results, int );

	in.global_moves = static_cast< unsigned int >( GET_VARIABLE( g_variables.m_route_calc_global_moves, int ) ) &
	                  n_route::k_all_moves;
	if ( in.global_moves == 0u ) {
		data.message = "no jump types selected - tick some in jump types to calculate";
		refuse( );
		return;
	}

	for ( std::size_t i = 0; i < data.points.size( ); ++i ) {
		route_point_t& pt = data.points[ i ];
		if ( pt.type != route_pt_pixeljump || pt.ent > 0 )
			continue;
		float gap      = -1.f;
		const int kind = pj_lip_kind( pt.pos, n_route::target_z( pt ), gap );
		pt.pj_skin     = kind == 1;
		botox_dbg_log( "[rc pj] marker %d lip %s ( probe gap %.4f ) -> catch %.6f..%.6f over it\n", static_cast< int >( i ) + 1,
		               kind == 1 ? "SKIN ( flush prop top )" : kind == 0 ? "solid" : "not found", gap,
		               kind == 1 ? n_route::k_skin_lo : 0.f, kind == 1 ? n_route::k_skin_hi : 1.96875f );
	}

	std::vector< int >& marker_no = job->marker_no;
	std::vector< route_point_t > live;
	for ( std::size_t i = 0; i < data.points.size( ); ++i ) {
		if ( !data.points[ i ].enabled )
			continue;
		live.emplace_back( data.points[ i ] );
		/* non-floor points show no jump list in the menu, so their hidden mask must not filter.
		   a pixel jump is PRESSED off like a floor, so it keeps its list. */
		if ( live.back( ).type != route_pt_ground && live.back( ).type != route_pt_pixeljump )
			live.back( ).styles = n_route::k_all_styles;
		marker_no.emplace_back( static_cast< int >( i ) + 1 );
	}

	if ( live.empty( ) || ( !start_here && live.size( ) < 2 ) ) {
		data.message = "every point is switched off";
		refuse( );
		return;
	}

	if ( start_here ) {
		auto* local = g_ctx.m_local;
		if ( local->get_move_type( ) != e_move_types::move_type_walk ) {
			data.message = "you are on a ladder or in noclip - calculate on foot or in the air";
			refuse( );
			return;
		}
		const int flags    = local->get_flags( );
		in.start_z         = local->get_origin( ).m_z;
		in.start_on_ground = ( flags & fl_onground ) != 0;
		in.start_on_player = n_route::ground_player( ) > 0;
		in.start_styles    = static_cast< unsigned int >( GET_VARIABLE( g_variables.m_route_calc_start_styles, int ) );
		in.points          = live;
		const float stam       = local->get_stamina( );
		const float duck_speed = local->get_duck_speed( );
		const float duck_amt   = local->get_duck_amount( );
		if ( !in.start_on_ground ) {
			in.start_vz      = local->get_velocity( ).m_z;
			in.start_stamina = ( stam >= 0.f && stam <= 200.f ) ? stam : 0.f;
		}
		in.start_ducked      = ( flags & fl_ducking ) != 0;
		in.start_duck_amount = ( duck_amt >= 0.f && duck_amt <= 1.f ) ? duck_amt : 0.f;
		in.start_duck_speed  = ( duck_speed >= 0.f && duck_speed <= 8.f ) ? duck_speed : 8.f;
		float snap_z         = 0.f;
		if ( in.start_on_ground && rc_start_snap( snap_z ) ) {
			botox_dbg_log( "[rc pts] start hovers %.4f over the floor: solving from the snap z %.4f ( StayOnGround )\n",
			               in.start_z - snap_z, snap_z );
			in.start_z = snap_z;
		}
		if ( !in.start_on_ground || in.start_ducked || in.start_duck_amount > 0.f )
			botox_dbg_log( "[rc pts] start live: vz %.4f stamina %.2f %s duck %.3f speed %.2f\n", in.start_vz, in.start_stamina,
			               in.start_ducked ? "DUCKED" : "standing", in.start_duck_amount, in.start_duck_speed );
	} else {
		if ( live.front( ).type == route_pt_pixeljump && live.front( ).pj_skin ) {
			data.message = "point 1 is a prop pixel jump - you can't stand on it, start from a floor";
			refuse( );
			return;
		}
		in.start_z         = n_route::target_z( live.front( ) );
		in.start_on_ground = true;
		in.start_on_player = live.front( ).ent > 0 && live.front( ).type == route_pt_ground;
		in.start_styles    = live.front( ).styles;
		in.points.assign( live.begin( ) + 1, live.end( ) );
		marker_no.erase( marker_no.begin( ) );
	}

	for ( std::size_t i = 0; i < in.points.size( ); ++i ) {
		route_point_t& pt = in.points[ i ];
		float head = 0.f;
		bool at_me = false;
		if ( pt.type == route_pt_headbang && !pt.snapped && pt.ent == 0 && rc_ceiling_probe( pt, head, at_me ) ) {
			pt.measured_z   = head;
			pt.has_measured = true;
			botox_dbg_log( "[rc probe] marker %d head stops %9.4f under %s ( %.4f under the ray hit %9.4f, n %.2f %.2f %.2f )\n",
			               marker_no[ i ], head, at_me ? "YOU" : "the marker", pt.pos.m_z - head, pt.pos.m_z, pt.wall_normal.m_x,
			               pt.wall_normal.m_y, pt.wall_normal.m_z );
		}
		pt.quantized_z = n_route::target_z( pt );
	}

	for ( std::size_t i = 0; i < in.points.size( ); ++i ) {
		route_point_t& pt = in.points[ i ];
		pt.roof           = 3.4e38f;
		if ( pt.type == route_pt_headbang || pt.type == route_pt_headbounce || pt.type == route_pt_texturebug || pt.ent > 0 )
			continue;
		const bool first       = i == 0;
		const int prev_type    = first ? route_pt_ground : in.points[ i - 1 ].type;
		if ( prev_type != route_pt_ground && prev_type != route_pt_pixeljump )
			continue;
		const c_vector from = first ? ( start_here ? g_ctx.m_local->get_origin( ) : live.front( ).pos ) : in.points[ i - 1 ].pos;
		const float dep_z   = first ? in.start_z : in.points[ i - 1 ].quantized_z;
		float roof          = 0.f;
		c_vector at{ };
		if ( !rc_leg_roof( from, pt.pos, dep_z, roof, at ) )
			continue;
		pt.roof = roof;
		botox_dbg_log( "[rc roof] leg to marker %d: ceiling, head stops %9.4f at %.1f %.1f ( %.2f over a standing head off %.4f )\n",
		               marker_no[ i ], roof, at.m_x, at.m_y, roof - ( dep_z + 72.f ), dep_z );
	}

	botox_dbg_log( "[rc pts] start z %9.4f ( %s%s )\n", in.start_z, in.start_on_ground ? "standing" : "in air",
	               in.start_on_player ? ", ON A PLAYER - absolute launch" : "" );
	for ( std::size_t i = 0; i < in.points.size( ); ++i ) {
		const route_point_t& pt = in.points[ i ];
		const bool ceil_pt = pt.type == route_pt_headbang || ( pt.type == route_pt_headbounce && pt.hb_ceiling );
		const bool sloped  = ceil_pt && !pt.has_measured && !pt.snapped && pt.ent == 0 && pt.wall_normal.m_z < -0.5f &&
		                    pt.wall_normal.m_z > -0.999f;
		const bool slope_floor = pt.type == route_pt_ground && !pt.snapped && pt.ent == 0 && i + 1 < in.points.size( ) &&
		                         pt.wall_normal.m_z > 0.7f && pt.wall_normal.m_z < n_route::k_flat_nz;
		botox_dbg_log( "[rc pts]   %d %-9s raw z %9.4f -> target %9.4f  %s%s%s%s%s%s\n", static_cast< int >( i ) + 1,
		                   route_point_type_name( pt.type ), pt.pos.m_z, pt.quantized_z,
		                   pt.has_measured ? ( ceil_pt ? "PROBED - hull top stops here" : "MEASURED - this pixel has been ridden" )
		                                   : ( pt.snapped ? "snapped to finder" : "aimed" ),
		                   pt.ent > 0 ? " ON PLAYER" : "", pt.enabled ? "" : " ( OFF )",
		                   pt.type == route_pt_headbounce ? ( pt.hb_ceiling ? " | ceiling: any crossing stops" : " | seam window" ) : "",
		                   sloped ? " | SLOPED ceiling, hull probe missed: raw ray z" : "",
		                   slope_floor ? " | SLOPED floor: the landing slides, later heights approximate" : "" );
		if ( sloped && GET_VARIABLE( g_variables.m_route_calc_advanced_readout, bool ) )
			movement_add_window( 3, "route: point " + std::to_string( i + 1 ) +
			                            " is a sloped ceiling the hull probe could not reach - stand under it and calculate again" );
		if ( slope_floor && GET_VARIABLE( g_variables.m_route_calc_advanced_readout, bool ) )
			movement_add_window( 3, "route: point " + std::to_string( i + 1 ) +
			                            " is on a slope - landings slide there, combos after it may miss. a flat spot is exact" );
	}

	for ( std::size_t i = 0; i < in.points.size( ); ++i ) {
		const route_point_t& pt = in.points[ i ];
		if ( pt.type != route_pt_edgebug || pt.snapped || pt.ent > 0 || pt.wall_normal.m_z <= 0.7f || pt.wall_normal.m_z >= n_route::k_flat_nz )
			continue;
		data.message = "point " + std::to_string( marker_no[ i ] ) + " is an edgebug on a slanted floor - aim it at a flat top";
		botox_dbg_log( "[rc] refused: %s ( n.z %.3f )\n", data.message.c_str( ), pt.wall_normal.m_z );
		refuse( );
		return;
	}

	const bool on_ground = ( g_ctx.m_local->get_flags( ) & fl_onground ) != 0;
	if ( !n_route::measure_launch( cmd ) && on_ground ) {
		data.message = "could not measure the jumps - try again on flat ground";
		refuse( );
		return;
	}
	if ( !on_ground )
		botox_dbg_log( "[rc] not on the ground - default jump impulse, start is %s\n",
		               start_here ? "your live arc ( vz / stamina / duck above )" : "marker 1" );

	job->advanced = GET_VARIABLE( g_variables.m_route_calc_advanced_readout, bool );
	job->gen      = data.gen;
	rc_start( job );
}

void n_movement::impl_t::route_calc_render( )
{
	if ( !g_ctx.m_local || !g_ctx.m_local->is_alive( ) )
		return;
	if ( !GET_VARIABLE( g_variables.m_route_calc, bool ) )
		return;

	auto& data = m_route_calc_data;
	if ( !data.map.empty( ) && data.map != g_interfaces.m_engine_client->get_level_name_short( ) )
		data.clear( );

	const auto accent_var = GET_VARIABLE( g_variables.m_accent, c_color );
	const unsigned int accent =
		c_color( static_cast< int >( accent_var.get< 0 >( ) ), static_cast< int >( accent_var.get< 1 >( ) ),
		         static_cast< int >( accent_var.get< 2 >( ) ), 255 )
			.get_u32( );
	const unsigned int white = c_color( 255, 255, 255, 255 ).get_u32( );
	const unsigned int black = c_color( 0, 0, 0, 255 ).get_u32( );

	auto* font = g_render.m_fonts[ e_font_names::font_name_verdana_11 ];
	if ( !font )
		return;

	for ( std::size_t i = 0; i < data.points.size( ); ++i ) {
		c_vector_2d screen_pos;
		if ( !g_render.world_to_screen( route_point_mark( data.points[ i ] ), screen_pos ) )
			continue;

		const float radius = 11.f;

		filled_circle_draw_object_t filled;
		filled.m_center = screen_pos;
		filled.m_radius = radius;
		filled.m_color  = black;
		g_render.m_draw_data.emplace_back( e_draw_type::draw_type_filled_circle,
		                                   std::make_any< filled_circle_draw_object_t >( filled ) );

		circle_draw_object_t outline;
		outline.m_center = screen_pos;
		outline.m_radius = radius;
		outline.m_color  = accent;
		g_render.m_draw_data.emplace_back( e_draw_type::draw_type_circle, std::make_any< circle_draw_object_t >( outline ) );

		const std::string index = std::to_string( i + 1 );
		const ImVec2 index_size = font->CalcTextSizeA( font->FontSize, FLT_MAX, 0.f, index.c_str( ) );
		add_text( font, screen_pos.m_x - index_size.x * 0.5f, screen_pos.m_y - index_size.y * 0.5f - 1.f, index, accent );

		std::string label = route_point_type_name( route_point_shown_type( data.points[ i ] ) );
		const bool head_shown = data.points[ i ].head_tb && data.points[ i ].type == route_pt_pixelsurf;
		if ( head_shown )
			label += " (head)";
		const unsigned int styles = data.points[ i ].styles;
		if ( styles != 0u && styles != n_route::k_all_styles )
			label += " *";
		if ( !data.points[ i ].enabled )
			label += " (off)";
		if ( data.points[ i ].ent > 0 )
			label += " (player)";
		if ( !head_shown && !data.points[ i ].allow_stand )
			label += " (ducked)";
		else if ( !head_shown && !data.points[ i ].allow_duck )
			label += " (stand)";
		const float label_w = text_width( font, label );
		add_text( font, screen_pos.m_x - label_w * 0.5f, screen_pos.m_y - radius - font->FontSize - 3.f, label, white );
	}
}

void n_movement::impl_t::route_calc_ui( )
{
	if ( !g_ctx.m_local || !g_ctx.m_local->is_alive( ) )
		return;
	if ( !GET_VARIABLE( g_variables.m_route_calc, bool ) )
		return;

	auto& data = m_route_calc_data;

	auto* font = g_render.m_fonts[ e_font_names::font_name_verdana_11 ];
	auto* bold = g_render.m_fonts[ e_font_names::font_name_verdana_bd_11 ];
	if ( !font )
		return;
	if ( !bold )
		bold = font;

	const auto box_chrome = [ & ]( const char* title, float alpha ) {
		auto* window    = ImGui::GetCurrentWindow( );
		auto* draw_list = window->DrawList;
		const auto size = window->Size;
		const auto pos  = window->Pos;

		const ImVec2 title_size = bold->CalcTextSizeA( bold->FontSize, FLT_MAX, 0.f, title );

		ImGui::PushClipRect( ImVec2( pos.x, pos.y ), ImVec2( pos.x + size.x, pos.y + k_band_height ), false );
		draw_list->AddRectFilled( ImVec2( pos.x, pos.y ), ImVec2( pos.x + size.x, pos.y + k_band_height ),
		                          ImColor( 25 / 255.f, 25 / 255.f, 25 / 255.f, alpha ), ImGui::GetStyle( ).WindowRounding,
		                          ImDrawFlags_RoundCornersTop );
		ImGui::PopClipRect( );

		RenderFadedGradientLine( draw_list, ImVec2( pos.x, pos.y + k_band_height - 1.f ), ImVec2( size.x, 1.f ),
		                         static_cast< ImColor >( ImGui::GetColorU32( ImGuiCol_::ImGuiCol_Accent ) ) );

		draw_list->AddText( bold, bold->FontSize,
		                    ImVec2( pos.x + ( size.x - title_size.x ) / 2.f, pos.y + ( k_band_height - title_size.y ) / 2.f ),
		                    ImColor( 1.f, 1.f, 1.f, alpha ), title );

		ImGui::PushClipRect( ImVec2( pos.x + 1.f, pos.y + 1.f ), ImVec2( pos.x + size.x - 1.f, pos.y + size.y - 1.f ), false );
		draw_list->AddRect( ImVec2( pos.x + 1.f, pos.y + 1.f ), ImVec2( pos.x + size.x - 1.f, pos.y + size.y - 1.f ),
		                    ImColor( 50 / 255.f, 50 / 255.f, 50 / 255.f, alpha ), ImGui::GetStyle( ).WindowRounding );
		ImGui::PopClipRect( );
	};

	int window_flags = ImGuiWindowFlags_::ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_::ImGuiWindowFlags_NoScrollbar |
	                   ImGuiWindowFlags_::ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_::ImGuiWindowFlags_AlwaysAutoResize;
	if ( !g_ctx.m_is_window_focused )
		window_flags |= ImGuiWindowFlags_::ImGuiWindowFlags_NoMove;

	if ( GET_VARIABLE( g_variables.m_route_calc_show_keys, bool ) ) {
		struct hint_t {
			std::string label;
			std::string key;
		};
		const hint_t hints[] = {
			{ "add point", key_label( GET_VARIABLE( g_variables.m_route_calc_add_key, key_bind_t ) ) },
			{ "calculate combos", key_label( GET_VARIABLE( g_variables.m_route_calc_solve_key, key_bind_t ) ) },
			{ "delete point", key_label( GET_VARIABLE( g_variables.m_route_calc_delete_key, key_bind_t ) ) },
			{ "clear all points", key_label( GET_VARIABLE( g_variables.m_route_calc_clear_key, key_bind_t ) ) },
		};
		const float hint_rows = static_cast< float >( IM_ARRAYSIZE( hints ) );

		const float row_h = ImGui::GetTextLineHeight( ) + 2.f;
		const float gap   = 22.f;
		float body_w      = 0.f;
		for ( const auto& hint : hints )
			body_w = ( std::max )( body_w, text_width( font, hint.label ) + gap + text_width( font, hint.key ) );

		ImGui::SetNextWindowSize( ImVec2( body_w + k_pad * 2.f, k_band_height + k_pad + row_h * hint_rows ), ImGuiCond_::ImGuiCond_Always );
		ImGui::Begin( "botox-route-calc-keys", 0, window_flags & ~ImGuiWindowFlags_::ImGuiWindowFlags_AlwaysAutoResize );
		{
			box_chrome( "route calculator", 1.f );

			auto* draw_list = ImGui::GetCurrentWindow( )->DrawList;
			const auto pos  = ImGui::GetCurrentWindow( )->Pos;
			const auto size = ImGui::GetCurrentWindow( )->Size;
			float row_y     = pos.y + k_band_height + 2.f;

			for ( const auto& hint : hints ) {
				draw_list->AddText( font, font->FontSize, ImVec2( pos.x + k_pad, row_y ), ImColor( 0.63f, 0.63f, 0.63f, 1.f ),
				                    hint.label.c_str( ) );
				draw_list->AddText( font, font->FontSize, ImVec2( pos.x + size.x - k_pad - text_width( font, hint.key ), row_y ),
				                    ImColor( 1.f, 1.f, 1.f, 1.f ), hint.key.c_str( ) );
				row_y += row_h;
			}
		}
		ImGui::End( );
	}

	if ( const int permille = g_rc_permille.load( ); permille >= 0 && !g_rc_cancel.load( ) ) {
		const std::vector< n_route::popup_row_t > busy{ { "calculating (" + std::to_string( permille / 10 ) + "%)", n_route::popup_row_combo } };
		/* started one fade-in ago: full alpha, never times out while the worker runs */
		n_route::draw_popup( "route calculator", busy, GetTickCount64( ) / 1000.f - 0.25f );
		return;
	}
	if ( !GET_VARIABLE( g_variables.m_route_calc_show_bar, bool ) )
		return;
	if ( data.solutions.empty( ) && data.message.empty( ) )
		return;

	std::vector< n_route::popup_row_t > rows;
	if ( !data.solutions.empty( ) ) {
		for ( const auto& solution : data.solutions )
			rows.push_back( { n_route::route_line( solution ), n_route::popup_row_combo } );
		if ( !data.note.empty( ) )
			rows.push_back( { data.note, n_route::popup_row_note } );
	} else {
		rows.push_back( { data.message, n_route::popup_row_error } );
		if ( !data.note.empty( ) )
			rows.push_back( { data.note, n_route::popup_row_note } );
	}
	n_route::draw_popup( "route calculator", rows, data.popup_started );
}

void n_route::draw_popup( const char* title, const std::vector< popup_row_t >& rows, float popup_started )
{
	if ( rows.empty( ) || popup_started <= 0.f )
		return;

	auto* font = g_render.m_fonts[ e_font_names::font_name_verdana_11 ];
	auto* bold = g_render.m_fonts[ e_font_names::font_name_verdana_bd_11 ];
	if ( !font )
		return;
	if ( !bold )
		bold = font;

	const ImColor accent_imcolor = static_cast< ImColor >( ImGui::GetColorU32( ImGuiCol_::ImGuiCol_Accent ) );

	constexpr float k_fade_in  = 0.25f;
	constexpr float k_fade_out = 0.2f;
	const float visible_time   = static_cast< float >( GET_VARIABLE( g_variables.m_route_calc_popup_time, int ) );
	const float total_time     = k_fade_in + visible_time + k_fade_out;
	const float age            = GetTickCount64( ) / 1000.f - popup_started;
	if ( age < 0.f || age >= total_time )
		return;

	float alpha = 1.f;
	if ( age < k_fade_in )
		alpha = age / k_fade_in;
	else if ( age > k_fade_in + visible_time )
		alpha = 1.f - ( age - k_fade_in - visible_time ) / k_fade_out;
	alpha = std::clamp( alpha, 0.f, 1.f );

	const auto row_font = [ & ]( const popup_row_t& r ) { return r.kind == popup_row_head ? bold : font; };

	const float row_h = ImGui::GetTextLineHeight( ) + 2.f;
	float body_w      = 0.f;
	for ( const auto& r : rows )
		body_w = ( std::max )( body_w, text_width( row_font( r ), r.text ) );
	body_w = ( std::max )( body_w + k_pad * 4.f, text_width( bold, title ) + k_pad * 4.f );

	constexpr float k_edge = 6.f;
	constexpr float k_gap  = 6.f;
	const float title_h    = bold->FontSize;
	const int row_count    = static_cast< int >( rows.size( ) );
	const float content_h  = row_h * static_cast< float >( row_count ) - 2.f;
	const float body_h     = k_edge + title_h + k_gap + content_h + k_edge;

	const ImVec2 display = ImVec2( g_ctx.m_width, g_ctx.m_height );
	const float box_x    = ( display.x - body_w ) * 0.5f;
	/* top is the anchor ( a one-line box centred at 0.72 ), list grows down so it never climbs over
	   the crosshair. moved only if it would run off the bottom. */
	const float k_one_line_h = k_edge + title_h + k_gap + ( row_h - 2.f ) + k_edge;
	float box_y              = display.y * 0.72f - k_one_line_h * 0.5f;
	if ( box_y + body_h > display.y - 4.f )
		box_y = display.y - 4.f - body_h;
	box_y = ( std::max )( box_y, 4.f );

	auto* draw_list      = ImGui::GetBackgroundDrawList( );
	const auto block     = g_render.begin_stretch_block( draw_list );
	const int a          = static_cast< int >( alpha * 255.f );
	const float rounding = ImGui::GetStyle( ).WindowRounding;

	draw_list->AddRectFilled( ImVec2( box_x, box_y ), ImVec2( box_x + body_w, box_y + body_h ), IM_COL32( 15, 15, 15, a ), rounding );
	draw_list->AddRect( ImVec2( box_x + 1.f, box_y + 1.f ), ImVec2( box_x + body_w - 1.f, box_y + body_h - 1.f ),
	                    IM_COL32( 50, 50, 50, a ), rounding );

	ImColor accent_line = accent_imcolor;
	accent_line.Value.w = alpha;
	const float bar_w   = ( body_w - 2.f ) * ( 1.f - std::clamp( age / total_time, 0.f, 1.f ) );
	if ( bar_w > 0.f ) {
		const float centre_x = box_x + body_w * 0.5f;
		draw_list->AddRectFilled( ImVec2( centre_x - bar_w * 0.5f, box_y ), ImVec2( centre_x + bar_w * 0.5f, box_y + 1.f ), accent_line );
	}

	const auto centred_text = [ & ]( ImFont* use_font, const std::string& text, float y, ImU32 colour ) {
		const float x = box_x + ( body_w - text_width( use_font, text ) ) * 0.5f;
		draw_list->AddText( use_font, use_font->FontSize, ImVec2( x + 1.f, y + 1.f ), IM_COL32( 0, 0, 0, a / 4 ), text.c_str( ) );
		draw_list->AddText( use_font, use_font->FontSize, ImVec2( x, y ), colour, text.c_str( ) );
	};

	centred_text( bold, title, box_y + k_edge, IM_COL32( 255, 255, 255, a ) );

	float row_y = box_y + k_edge + title_h + k_gap;
	for ( const auto& r : rows ) {
		ImU32 colour = IM_COL32( 240, 240, 240, a );
		if ( r.kind == popup_row_error )
			colour = IM_COL32( 199, 102, 102, a );
		else if ( r.kind == popup_row_note )
			colour = IM_COL32( 140, 140, 140, a );
		else if ( r.kind == popup_row_head )
			colour = IM_COL32( 170, 170, 170, a );
		centred_text( row_font( r ), r.text, row_y, colour );
		row_y += row_h;
	}

	g_render.end_stretch_block( block );
}
