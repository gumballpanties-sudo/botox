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
#include <string>
#include "../../dependencies/imgui/imgui.h"
#include "../../dependencies/json/json.hpp"
#include "../chat_hud.h"
#include "texturebug.h"
#include "edgebug.h"
#include "wall_climb.h"
#include "air_rollout.h"
#include "../visuals/screen/reflections.h"

struct fireman_ladder_hit_t {
	bool found      = false;
	bool flat_face  = false;
	float yaw       = 0.f;
	float dist      = 0.f;
	c_vector point  = c_vector( 0.f, 0.f, 0.f );
	c_vector normal = c_vector( 0.f, 0.f, 0.f );
	float nz        = 0.f;
};

constexpr float fireman_slant_nz_max = 0.7f;

static bool fireman_face_nz_ok( const float latch_nz, const float face_nz )
{
	// the fling is 270 * this plane: any downward tilt throws us into the ground, never wanted
	if ( latch_nz < -0.05f )
		return false;

	if ( std::fabsf( latch_nz ) < 0.3f )
		return true;

	return face_nz >= 0.3f && latch_nz > 0.f && latch_nz < fireman_slant_nz_max && std::fabsf( latch_nz - face_nz ) < 0.1f;
}

static bool fireman_is_left_ladder( const c_vector& hit, const c_vector& p, const c_vector& n )
{
	constexpr float below_left = 16.f;

	const float dx  = hit.m_x - p.m_x;
	const float dy  = hit.m_y - p.m_y;
	const float out = dx * n.m_x + dy * n.m_y;
	const float lat = dy * n.m_x - dx * n.m_y;

	return out < 8.f && out > -128.f && std::fabsf( lat ) < 96.f && hit.m_z > p.m_z - below_left;
}

static bool fireman_latch_is_left( const c_vector& origin, const c_vector& maxs, const c_vector& latch_normal, const bool left_valid,
                                   const c_vector& left_point, const c_vector& left_normal )
{
	if ( !left_valid )
		return false;

	const float reach = maxs.m_x * ( std::fabsf( latch_normal.m_x ) + std::fabsf( latch_normal.m_y ) );
	const c_vector face( origin.m_x - latch_normal.m_x * reach, origin.m_y - latch_normal.m_y * reach, origin.m_z + maxs.m_z * 0.3f );

	return fireman_is_left_ladder( face, left_point, left_normal );
}

static bool fireman_face_is_stub( const c_vector& point, const c_vector& normal, const c_vector& mins, const c_vector& maxs, const float from_z )
{
	constexpr float floor_probe = 1024.f;
	constexpr float under_floor = 32.f;
	constexpr float max_depth   = 64.f;

	const float reach = maxs.m_x * ( std::fabsf( normal.m_x ) + std::fabsf( normal.m_y ) ) + 1.f;
	const c_vector start( point.m_x + normal.m_x * reach, point.m_y + normal.m_y * reach, std::max( from_z, point.m_z ) );

	trace_t t;
	c_trace_filter_trace_type_everything_filter_props flt;
	ray_t r( start, c_vector( start.m_x, start.m_y, start.m_z - floor_probe ), mins, maxs );
	g_interfaces.m_engine_trace->trace_ray( r, mask_playersolid, &flt, &t );

	if ( t.m_start_solid || t.m_fraction >= 1.f || ( t.m_contents & e_contents::contents_ladder ) )
		return false;

	const c_vector inside( point.m_x - normal.m_x, point.m_y - normal.m_y, t.m_end.m_z - under_floor );

	trace_t s;
	c_trace_filter sflt( g_ctx.m_local );
	ray_t sr( inside, c_vector( inside.m_x, inside.m_y, inside.m_z - 1.f ) );
	g_interfaces.m_engine_trace->trace_ray( sr, e_contents::contents_ladder, &sflt, &s );

	if ( !s.m_start_solid )
		return false;

	const c_vector from( point.m_x - normal.m_x * max_depth, point.m_y - normal.m_y * max_depth, point.m_z );

	trace_t f;
	c_trace_filter fflt( g_ctx.m_local );
	ray_t fr( from, point );
	g_interfaces.m_engine_trace->trace_ray( fr, e_contents::contents_ladder, &fflt, &f );

	if ( f.m_start_solid || f.m_fraction >= 1.f )
		return false;

	const c_vector past( f.m_end.m_x - normal.m_x, f.m_end.m_y - normal.m_y, f.m_end.m_z );

	trace_t b;
	c_trace_filter bflt( g_ctx.m_local );
	ray_t br( past, past, c_vector( -0.1f, -0.1f, -0.1f ), c_vector( 0.1f, 0.1f, 0.1f ) );
	g_interfaces.m_engine_trace->trace_ray( br, mask_playersolid, &bflt, &b );

	return !b.m_start_solid && !b.m_all_solid;
}

static c_vector fireman_catch_point( const c_vector& point, const c_vector& normal, const c_vector& mins, const c_vector& maxs )
{
	constexpr float floor_probe = 1024.f;

	const c_vector start( point.m_x + normal.m_x * ( maxs.m_x + 1.f ), point.m_y + normal.m_y * ( maxs.m_x + 1.f ), point.m_z );

	trace_t t;
	c_trace_filter_trace_type_everything_filter_props flt;
	ray_t r( start, c_vector( start.m_x, start.m_y, start.m_z - floor_probe ) );
	g_interfaces.m_engine_trace->trace_ray( r, mask_playersolid, &flt, &t );

	if ( t.m_start_solid || t.m_fraction >= 1.f )
		return point;

	const float want = t.m_end.m_z + mins.m_z + ( maxs.m_z - mins.m_z ) * 0.3f;
	if ( want >= point.m_z )
		return point;

	const c_vector in( point.m_x - normal.m_x * 0.5f, point.m_y - normal.m_y * 0.5f, want );

	trace_t s;
	c_trace_filter sflt( g_ctx.m_local );
	ray_t sr( in, c_vector( in.m_x, in.m_y, point.m_z ) );
	g_interfaces.m_engine_trace->trace_ray( sr, e_contents::contents_ladder, &sflt, &s );

	if ( s.m_start_solid )
		return c_vector( point.m_x, point.m_y, want );
	if ( s.m_fraction >= 1.f )
		return point;

	return c_vector( point.m_x, point.m_y, std::min( s.m_end.m_z + 4.f, point.m_z ) );
}

static bool fireman_through_face( const c_vector& back_point, const c_vector& back_normal, const c_vector& ray_start, const c_vector& mins,
                                  const c_vector& maxs, const float from_z, fireman_ladder_hit_t& out )
{
	constexpr float max_depth = 64.f;

	const c_vector from( back_point.m_x - back_normal.m_x * max_depth, back_point.m_y - back_normal.m_y * max_depth, back_point.m_z );

	trace_t t;
	c_trace_filter flt( g_ctx.m_local );
	ray_t r( from, back_point );
	g_interfaces.m_engine_trace->trace_ray( r, e_contents::contents_ladder, &flt, &t );

	if ( t.m_start_solid || t.m_fraction >= 1.f )
		return false;

	const c_vector& normal = t.m_plane.m_normal;
	const float nlen       = sqrtf( normal.m_x * normal.m_x + normal.m_y * normal.m_y );

	if ( normal.m_z <= -0.3f || normal.m_z >= fireman_slant_nz_max || nlen < 0.01f )
		return false;

	const c_vector front_n( normal.m_x / nlen, normal.m_y / nlen, 0.f );

	if ( front_n.m_x * back_normal.m_x + front_n.m_y * back_normal.m_y > -0.9f )
		return false;

	trace_t s;
	c_trace_filter_trace_type_everything_filter_props sflt;
	ray_t sr( from, back_point );
	g_interfaces.m_engine_trace->trace_ray( sr, mask_playersolid, &sflt, &s );

	if ( s.m_start_solid || s.m_fraction < t.m_fraction - 2.f / max_depth )
		return false;

	if ( fireman_face_is_stub( t.m_end, front_n, mins, maxs, from_z ) )
		return false;

	out           = fireman_ladder_hit_t{ };
	out.found     = true;
	out.flat_face = true;
	out.nz        = normal.m_z;
	out.dist      = c_vector( t.m_end.m_x - ray_start.m_x, t.m_end.m_y - ray_start.m_y, 0.f ).length_2d( );
	out.point     = t.m_end;
	out.normal    = front_n;
	out.yaw       = c_vector( -front_n.m_x, -front_n.m_y, 0.f ).to_angle( ).m_y;

	return true;
}

struct fireman_map_ladder_t {
	c_vector lo;
	c_vector hi;
};

// every CONTENTS_LADDER brush AABB off the bsp (lumps 1/18/19), once per map. gate: tools/fr_map_ladders_check.py
static void fireman_map_ladders_load( const char* level, std::vector< fireman_map_ladder_t >& out )
{
	std::vector< unsigned char > header;
	if ( !read_game_file( level, header, 0, 8 + 64 * 16 ) || std::memcmp( header.data( ), "VBSP", 4 ) != 0 )
		return;

	const auto lump = [ & ]( const int index, const unsigned int stride, std::vector< unsigned char >& data ) {
		int info[ 2 ]{ };
		std::memcpy( info, header.data( ) + 8 + index * 16, sizeof( info ) );

		if ( info[ 0 ] <= 0 || info[ 1 ] < static_cast< int >( stride ) )
			return false;
		if ( !read_game_file( level, data, static_cast< unsigned int >( info[ 0 ] ), static_cast< unsigned int >( info[ 1 ] ) - static_cast< unsigned int >( info[ 1 ] ) % stride ) )
			return false;

		return std::memcmp( data.data( ), "LZMA", 4 ) != 0;
	};

	std::vector< unsigned char > planes, brushes, sides;
	if ( !lump( 1, 20, planes ) || !lump( 18, 12, brushes ) || !lump( 19, 8, sides ) )
		return;

	const size_t plane_count = planes.size( ) / 20;
	const size_t side_count  = sides.size( ) / 8;

	for ( size_t b = 0; b + 12 <= brushes.size( ); b += 12 ) {
		int brush[ 3 ]{ };
		std::memcpy( brush, brushes.data( ) + b, sizeof( brush ) );

		if ( !( brush[ 2 ] & e_contents::contents_ladder ) || brush[ 0 ] < 0 || brush[ 1 ] <= 0 ||
		     static_cast< size_t >( brush[ 0 ] ) + static_cast< size_t >( brush[ 1 ] ) > side_count )
			continue;

		float lo[ 3 ] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
		float hi[ 3 ] = { FLT_MAX, FLT_MAX, FLT_MAX };

		for ( int s = brush[ 0 ]; s < brush[ 0 ] + brush[ 1 ]; ++s ) {
			unsigned short plane_index = 0;
			std::memcpy( &plane_index, sides.data( ) + static_cast< size_t >( s ) * 8, sizeof( plane_index ) );
			if ( plane_index >= plane_count )
				continue;

			float plane[ 4 ]{ };
			std::memcpy( plane, planes.data( ) + static_cast< size_t >( plane_index ) * 20, sizeof( plane ) );

			for ( int a = 0; a < 3; ++a ) {
				if ( plane[ a ] > 0.9999f )
					hi[ a ] = std::min( hi[ a ], plane[ 3 ] );
				else if ( plane[ a ] < -0.9999f )
					lo[ a ] = std::max( lo[ a ], -plane[ 3 ] );
			}
		}

		if ( lo[ 0 ] < hi[ 0 ] && lo[ 1 ] < hi[ 1 ] && lo[ 2 ] < hi[ 2 ] && lo[ 0 ] > -FLT_MAX && lo[ 1 ] > -FLT_MAX && lo[ 2 ] > -FLT_MAX &&
		     hi[ 0 ] < FLT_MAX && hi[ 1 ] < FLT_MAX && hi[ 2 ] < FLT_MAX )
			out.push_back( { c_vector( lo[ 0 ], lo[ 1 ], lo[ 2 ] ), c_vector( hi[ 0 ], hi[ 1 ], hi[ 2 ] ) } );
	}
}

static const std::vector< fireman_map_ladder_t >& fireman_map_ladders( )
{
	static std::string level;
	static std::vector< fireman_map_ladder_t > ladders;

	const char* name = g_interfaces.m_engine_client->is_in_game( ) ? g_interfaces.m_engine_client->get_level_name( ) : nullptr;

	if ( !name || std::strlen( name ) < 5 ) {
		level.clear( );
		ladders.clear( );
		return ladders;
	}

	if ( level != name ) {
		level = name;
		ladders.clear( );
		fireman_map_ladders_load( name, ladders );
		botox_dbg_log( "[fr] map ladders n=%d %s", static_cast< int >( ladders.size( ) ), name );
	}

	return ladders;
}

static bool fireman_find_ladder( const c_vector& origin, const c_vector& mins, const c_vector& maxs, const float max_dist, fireman_ladder_hit_t& out, const float look_down = 0.f,
                                 const c_vector* skip_point = nullptr, const c_vector* skip_normal = nullptr, const float extra_depth = -1.f )
{
	out = fireman_ladder_hit_t{ };

	const float hull_height = maxs.m_z - mins.m_z;

	constexpr int max_heights  = 6;

	const float slice_step = std::clamp( look_down / static_cast< float >( max_heights - 2 ), 24.f, 176.f );

	float heights[ max_heights ] = { mins.m_z + hull_height * 0.3f, mins.m_z + hull_height * 0.7f };
	int height_count             = 2;

	for ( float d = slice_step; d <= look_down && height_count < max_heights - 1; d += slice_step )
		heights[ height_count++ ] = mins.m_z + hull_height * 0.5f - d;

	if ( extra_depth >= 0.f )
		heights[ height_count++ ] = mins.m_z - extra_depth;

	const auto cast = [ & ]( const c_vector& dir, const float z_lo, const float z_hi ) {
		for ( int hi = 0; hi < height_count; ++hi ) {
			const float h = heights[ hi ];
			if ( origin.m_z + h < z_lo || origin.m_z + h > z_hi )
				continue;

			const c_vector start( origin.m_x, origin.m_y, origin.m_z + h );
			const c_vector end = start + dir * max_dist;

			trace_t ladder_trace;
			c_trace_filter ladder_filter( g_ctx.m_local );
			ray_t ladder_ray( start, end );
			g_interfaces.m_engine_trace->trace_ray( ladder_ray, e_contents::contents_ladder, &ladder_filter, &ladder_trace );

			if ( ladder_trace.m_fraction >= 1.f )
				continue;

			const float hit_dist = ladder_trace.m_fraction * max_dist;
			if ( out.found && hit_dist >= out.dist )
				continue;

			if ( skip_point && skip_normal && fireman_is_left_ladder( ladder_trace.m_end, *skip_point, *skip_normal ) )
				continue;

			trace_t solid_trace;
			c_trace_filter_trace_type_everything_filter_props solid_filter;
			ray_t solid_ray( start, end );
			g_interfaces.m_engine_trace->trace_ray( solid_ray, mask_playersolid, &solid_filter, &solid_trace );

			if ( solid_trace.m_fraction * max_dist < hit_dist - 2.f )
				continue;

			const c_vector& normal = ladder_trace.m_plane.m_normal;
			const bool usable_normal = normal.m_z > -0.3f && normal.m_z < fireman_slant_nz_max && ( normal.m_x != 0.f || normal.m_y != 0.f );

			{
				const float nlen    = usable_normal ? sqrtf( normal.m_x * normal.m_x + normal.m_y * normal.m_y ) : 1.f;
				const c_vector out_n = usable_normal ? c_vector( normal.m_x / nlen, normal.m_y / nlen, 0.f ) : c_vector( -dir.m_x, -dir.m_y, 0.f );

				if ( fireman_face_is_stub( ladder_trace.m_end, out_n, mins, maxs, origin.m_z ) ) {
					fireman_ladder_hit_t front;
					const bool through = usable_normal && fireman_through_face( ladder_trace.m_end, out_n, start, mins, maxs, origin.m_z, front ) &&
					                     !( skip_point && skip_normal && fireman_is_left_ladder( front.point, *skip_point, *skip_normal ) );

					static int back_log = 0;
					if ( ( back_log++ % 32 ) == 0 )
						botox_dbg_log( "[fr] stub refused z=%.0f yaw=%.0f through=%d dist=%.0f", ladder_trace.m_end.m_z,
						               c_vector( -out_n.m_x, -out_n.m_y, 0.f ).to_angle( ).m_y, ( int )through, through ? front.dist : -1.f );

					if ( through && ( !out.found || front.dist < out.dist ) )
						out = front;
					continue;
				}
			}

			out.found     = true;
			out.flat_face = usable_normal;
			out.nz        = usable_normal ? normal.m_z : 0.f;
			out.dist      = hit_dist;
			out.yaw       = usable_normal ? c_vector( -normal.m_x, -normal.m_y, 0.f ).to_angle( ).m_y : dir.to_angle( ).m_y;
			out.point     = ladder_trace.m_end;

			if ( usable_normal ) {
				const float nlen = sqrtf( normal.m_x * normal.m_x + normal.m_y * normal.m_y );
				out.normal       = c_vector( normal.m_x / nlen, normal.m_y / nlen, 0.f );
			} else
				out.normal = c_vector( -dir.m_x, -dir.m_y, 0.f );
		}
	};

	constexpr int steps = 24;
	const float step    = std::numbers::pi_v< float > * 2.0f / static_cast< float >( steps );

	for ( int i = 0; i < steps; ++i ) {
		const float a = step * static_cast< float >( i );
		cast( c_vector( cosf( a ), sinf( a ), 0.f ), -FLT_MAX, FLT_MAX );
	}

	// fan rays sit 15 deg apart (84u at 320): a 24u face past ~90u or a 1u side strip falls between them. aim at every map ladder too
	const bool fan_found = out.found;

	for ( const fireman_map_ladder_t& ladder : fireman_map_ladders( ) ) {
		const float dx = std::max( { ladder.lo.m_x - origin.m_x, 0.f, origin.m_x - ladder.hi.m_x } );
		const float dy = std::max( { ladder.lo.m_y - origin.m_y, 0.f, origin.m_y - ladder.hi.m_y } );

		if ( ( dx == 0.f && dy == 0.f ) || dx * dx + dy * dy > max_dist * max_dist )
			continue;

		const float in_x = std::min( 2.f, ( ladder.hi.m_x - ladder.lo.m_x ) * 0.25f );
		const float in_y = std::min( 2.f, ( ladder.hi.m_y - ladder.lo.m_y ) * 0.25f );

		const c_vector targets[ 2 ] = {
			c_vector( std::clamp( origin.m_x, ladder.lo.m_x + in_x, ladder.hi.m_x - in_x ), std::clamp( origin.m_y, ladder.lo.m_y + in_y, ladder.hi.m_y - in_y ), 0.f ),
			c_vector( ( ladder.lo.m_x + ladder.hi.m_x ) * 0.5f, ( ladder.lo.m_y + ladder.hi.m_y ) * 0.5f, 0.f ) };

		for ( const c_vector& target : targets ) {
			const c_vector to( target.m_x - origin.m_x, target.m_y - origin.m_y, 0.f );
			const float len = to.length_2d( );

			if ( len > 0.01f )
				cast( c_vector( to.m_x / len, to.m_y / len, 0.f ), ladder.lo.m_z, ladder.hi.m_z );
		}
	}

	if ( out.found && !fan_found ) {
		static int map_log = 0;
		if ( ( map_log++ % 16 ) == 0 )
			botox_dbg_log( "[fr] map seen dist=%.0f yaw=%.0f z=%.0f (fan blind)", out.dist, out.yaw, out.point.m_z );
	}

	return out.found;
}

static float fireman_ladder_drop( const c_vector& origin, const c_vector& mins, const c_vector& maxs, const float yaw, const float reach, const float max_depth )
{
	if ( max_depth <= 0.f )
		return 0.f;

	const float hull_height = maxs.m_z - mins.m_z;
	const float bands[ 2 ]  = { mins.m_z + hull_height * 0.3f, mins.m_z + hull_height * 0.7f };

	c_vector dir;
	g_math.angle_vectors( c_angle( 0.f, yaw, 0.f ), &dir );

	const auto ladder_at = [ & ]( const float depth ) {
		for ( const float band : bands ) {
			const c_vector start( origin.m_x, origin.m_y, origin.m_z + band - depth );

			trace_t t;
			c_trace_filter flt( g_ctx.m_local );
			ray_t r( start, start + dir * reach );
			g_interfaces.m_engine_trace->trace_ray( r, e_contents::contents_ladder, &flt, &t );

			if ( t.m_fraction < 1.f )
				return true;
		}

		return false;
	};

	if ( !ladder_at( 0.f ) )
		return 0.f;
	if ( ladder_at( max_depth ) )
		return max_depth;

	float lo = 0.f, hi = max_depth;
	for ( int i = 0; i < 8; ++i ) {
		const float mid = ( lo + hi ) * 0.5f;
		if ( ladder_at( mid ) )
			lo = mid;
		else
			hi = mid;
	}

	return lo;
}

static float fireman_face_gap( const c_vector& origin, const c_vector& mins, const c_vector& maxs, const float yaw, const float probe )
{
	c_vector dir;
	g_math.angle_vectors( c_angle( 0.f, yaw, 0.f ), &dir );

	trace_t t;
	c_trace_filter flt( g_ctx.m_local );
	ray_t r( origin, origin + dir * probe, mins, maxs );
	g_interfaces.m_engine_trace->trace_ray( r, e_contents::contents_ladder, &flt, &t );

	return t.m_fraction >= 1.f ? probe : t.m_fraction * probe;
}

static float fireman_column_top( const c_vector& origin, const c_vector& mins, const c_vector& maxs, const c_vector& point, const c_vector& normal )
{
	constexpr float max_depth = 512.f;

	const float s = ( origin.m_x - point.m_x ) * normal.m_x + ( origin.m_y - point.m_y ) * normal.m_y;
	const c_vector tangent( -normal.m_y, normal.m_x, 0.f );
	const float lats[ 3 ] = { 0.f, maxs.m_x - 1.f, mins.m_x + 1.f };

	float top = -FLT_MAX;

	for ( const float lat : lats ) {
		const c_vector start( origin.m_x - normal.m_x * ( s + 0.5f ) + tangent.m_x * lat, origin.m_y - normal.m_y * ( s + 0.5f ) + tangent.m_y * lat,
		                      origin.m_z + maxs.m_z );

		trace_t t;
		c_trace_filter flt( g_ctx.m_local );
		ray_t r( start, c_vector( start.m_x, start.m_y, start.m_z - max_depth ) );
		g_interfaces.m_engine_trace->trace_ray( r, e_contents::contents_ladder, &flt, &t );

		if ( t.m_start_solid )
			return FLT_MAX;
		if ( t.m_fraction < 1.f )
			top = std::max( top, t.m_end.m_z );
	}

	return top;
}

static void fireman_face_span( const c_vector& point, const c_vector& normal, const c_vector& tangent, float& out_min, float& out_max )
{
	constexpr float step = 8.f;
	constexpr int steps  = 6;

	out_min = 0.f;
	out_max = 0.f;

	for ( int side = 0; side < 2; ++side ) {
		const float sign = side == 0 ? 1.f : -1.f;

		for ( int i = 1; i <= steps; ++i ) {
			const float off = sign * step * static_cast< float >( i );

			const c_vector start( point.m_x + normal.m_x * 4.f + tangent.m_x * off,
			                      point.m_y + normal.m_y * 4.f + tangent.m_y * off, point.m_z );
			const c_vector end( start.m_x - normal.m_x * 12.f, start.m_y - normal.m_y * 12.f, start.m_z );

			trace_t t;
			c_trace_filter flt( g_ctx.m_local );
			ray_t r( start, end );
			g_interfaces.m_engine_trace->trace_ray( r, e_contents::contents_ladder, &flt, &t );

			if ( t.m_fraction >= 1.f )
				break;

			if ( sign > 0.f )
				out_max = off;
			else
				out_min = off;
		}
	}
}

static void fireman_hull_touch( const c_vector& maxs, const c_vector& normal, const c_vector& tangent, float& out_lo, float& out_hi )
{
	const float cx     = normal.m_x > 0.f ? -maxs.m_x : maxs.m_x;
	const float cy     = normal.m_y > 0.f ? -maxs.m_y : maxs.m_y;
	const float corner = cx * tangent.m_x + cy * tangent.m_y;

	const bool x_major = std::fabsf( normal.m_x ) >= std::fabsf( normal.m_y );
	const float minor  = x_major ? std::fabsf( normal.m_y ) : std::fabsf( normal.m_x );

	if ( minor > 0.26f ) {
		out_lo = out_hi = corner;
		return;
	}

	const float other = x_major ? cx * tangent.m_x - cy * tangent.m_y : cy * tangent.m_y - cx * tangent.m_x;

	out_lo = std::min( corner, other );
	out_hi = std::max( corner, other );
}

static bool fireman_can_latch( const c_vector& origin, const c_vector& mins, const c_vector& maxs, const float yaw, c_vector& out_normal,
                               bool* out_degenerate = nullptr, const float reach = 2.f )
{
	if ( out_degenerate )
		*out_degenerate = false;

	const float ladder_distance        = reach;
	constexpr unsigned int ladder_mask = mask_playersolid & ~e_contents::contents_playerclip;

	c_vector dir;
	g_math.angle_vectors( c_angle( 0.f, yaw, 0.f ), &dir );

	trace_t t;
	c_trace_filter flt( g_ctx.m_local );
	ray_t r( origin, origin + dir * ladder_distance, mins, maxs );
	g_interfaces.m_engine_trace->trace_ray( r, ladder_mask, &flt, &t );

	const c_vector& n = t.m_plane.m_normal;
	if ( t.m_start_solid || t.m_all_solid || ( n.m_x == 0.f && n.m_y == 0.f && n.m_z == 0.f ) ) {
		if ( fireman_face_gap( origin, mins, maxs, yaw, ladder_distance + 16.f ) > ladder_distance )
			return false;

		if ( out_degenerate )
			*out_degenerate = true;

		out_normal = c_vector( -dir.m_x, -dir.m_y, 0.f );
		return true;
	}

	if ( t.m_fraction >= 1.f )
		return false;
	if ( n.m_z == 1.f )
		return false;
	if ( !botox_is_ladder_trace( t ) )
		return false;

	out_normal = n;
	return true;
}

/* log only: LadderMove's own test (:3611 hull 2u along the press + OnLadder = ladder bit / climbable, nz != 1) beside ours */
static void fireman_latch_probe( const c_vector& origin, const c_vector& mins, const c_vector& maxs, const float yaw, char* out, const size_t size )
{
	constexpr unsigned int ladder_mask = mask_playersolid & ~e_contents::contents_playerclip;

	c_vector dir;
	g_math.angle_vectors( c_angle( 0.f, yaw, 0.f ), &dir );

	trace_t t;
	c_trace_filter flt( g_ctx.m_local );
	ray_t r( origin, origin + dir * 2.f, mins, maxs );
	g_interfaces.m_engine_trace->trace_ray( r, ladder_mask, &flt, &t );

	bool climb = false;
	if ( g_interfaces.m_physics_surface_props )
		if ( auto* data = g_interfaces.m_physics_surface_props->get_surface_data( t.surface.m_surface_props ) )
			climb = data->m_game.m_climbable != 0;

	c_vector n;
	bool flush      = false;
	const bool ours = fireman_can_latch( origin, mins, maxs, yaw, n, &flush );

	snprintf( out, size, "frac=%.2f ss=%d lad=%d climb=%d nz=%.2f nyaw=%.0f ours=%d flush=%d", t.m_fraction, ( int )t.m_start_solid,
	          ( t.m_contents & e_contents::contents_ladder ) != 0 ? 1 : 0, ( int )climb, t.m_plane.m_normal.m_z, t.m_plane.m_normal.to_angle( ).m_y,
	          ( int )ours, ( int )flush );
}

static float fireman_fling_clearance( const c_vector& origin, const c_vector& mins, const c_vector& maxs, const c_vector& normal )
{
	constexpr float probe = 64.f;

	const float len = sqrtf( normal.m_x * normal.m_x + normal.m_y * normal.m_y );
	if ( len < 0.01f )
		return 0.f;

	const c_vector dir( normal.m_x / len, normal.m_y / len, 0.f );

	trace_t t;
	c_trace_filter flt( g_ctx.m_local );
	ray_t r( origin, origin + dir * probe, mins, maxs );
	g_interfaces.m_engine_trace->trace_ray( r, mask_playersolid, &flt, &t );

	return t.m_fraction * probe;
}

static bool fireman_side_face( const c_vector& origin, const c_vector& mins, const c_vector& maxs, const c_vector& dir, const float reach,
                               c_vector& out_normal, float& out_gap )
{
	trace_t t;
	c_trace_filter flt( g_ctx.m_local );
	ray_t r( origin, origin + dir * reach, mins, maxs );
	g_interfaces.m_engine_trace->trace_ray( r, e_contents::contents_ladder, &flt, &t );

	if ( t.m_start_solid || t.m_all_solid || t.m_fraction >= 1.f )
		return false;

	const c_vector& n = t.m_plane.m_normal;
	if ( std::fabsf( n.m_z ) >= 0.3f || n.m_x * dir.m_x + n.m_y * dir.m_y > -0.7f )
		return false;

	out_gap = t.m_fraction * reach;

	trace_t solid;
	c_trace_filter solid_flt( g_ctx.m_local );
	ray_t solid_ray( origin, origin + dir * reach, mins, maxs );
	g_interfaces.m_engine_trace->trace_ray( solid_ray, mask_playersolid, &solid_flt, &solid );

	if ( solid.m_start_solid || solid.m_fraction * reach < out_gap - 2.f )
		return false;

	const float nlen = sqrtf( n.m_x * n.m_x + n.m_y * n.m_y );
	out_normal       = c_vector( n.m_x / nlen, n.m_y / nlen, 0.f );
	return true;
}

struct fireman_face_end_t {
	bool found = false;
	bool open  = false;
	float at   = 0.f;
	c_vector point{ };
	c_vector normal{ };
};

static fireman_face_end_t fireman_face_end( const c_vector& point, const c_vector& normal, const float sign )
{
	constexpr float step = 8.f;
	constexpr int steps  = 16;

	fireman_face_end_t out{ };
	const c_vector tangent( -normal.m_y * sign, normal.m_x * sign, 0.f );

	float miss = -1.f;

	for ( int i = 1; i <= steps && miss < 0.f; ++i ) {
		const float off = step * static_cast< float >( i );
		const c_vector start( point.m_x + normal.m_x * 4.f + tangent.m_x * off, point.m_y + normal.m_y * 4.f + tangent.m_y * off, point.m_z );
		const c_vector end( start.m_x - normal.m_x * 12.f, start.m_y - normal.m_y * 12.f, start.m_z );

		trace_t t;
		c_trace_filter flt( g_ctx.m_local );
		ray_t r( start, end );
		g_interfaces.m_engine_trace->trace_ray( r, e_contents::contents_ladder, &flt, &t );

		if ( t.m_fraction >= 1.f )
			miss = off;
	}

	if ( miss < 0.f )
		return out;

	// 0.1 in: a brush sticking < 0.5u out of its wall put a 0.5 probe inside the wall = closed = no side at all
	const c_vector corner( point.m_x - normal.m_x * 0.1f + tangent.m_x * miss, point.m_y - normal.m_y * 0.1f + tangent.m_y * miss, point.m_z );
	const c_vector back( corner.m_x - tangent.m_x * ( step + 1.f ), corner.m_y - tangent.m_y * ( step + 1.f ), corner.m_z );

	trace_t t;
	c_trace_filter flt( g_ctx.m_local );
	ray_t r( corner, back );
	g_interfaces.m_engine_trace->trace_ray( r, e_contents::contents_ladder, &flt, &t );

	const c_vector& n = t.m_plane.m_normal;
	if ( t.m_start_solid || t.m_fraction >= 1.f || std::fabsf( n.m_z ) >= 0.3f || n.m_x * tangent.m_x + n.m_y * tangent.m_y < 0.7f )
		return out;

	trace_t solid;
	c_trace_filter solid_flt( g_ctx.m_local );
	ray_t solid_ray( corner, corner, c_vector( -0.1f, -0.1f, -0.1f ), c_vector( 0.1f, 0.1f, 0.1f ) );
	g_interfaces.m_engine_trace->trace_ray( solid_ray, mask_playersolid, &solid_flt, &solid );

	const float nlen = sqrtf( n.m_x * n.m_x + n.m_y * n.m_y );

	out.found  = true;
	out.open   = !solid.m_start_solid && !solid.m_all_solid;
	out.point  = t.m_end;
	out.normal = c_vector( n.m_x / nlen, n.m_y / nlen, 0.f );
	out.at     = ( t.m_end.m_x - point.m_x ) * tangent.m_x + ( t.m_end.m_y - point.m_y ) * tangent.m_y;

	return out;
}

struct fireman_side_target_t {
	c_vector point{ };
	c_vector normal{ };
	c_vector u{ };
	float half  = 0.f;
	bool walled = false; // back end corner in solid; log only (8u-quantized, the park never trusts it)
};

static bool fireman_side_geometry( const c_vector& point, const c_vector& normal, const fireman_face_end_t ends[ 2 ], const c_vector* front,
                                   fireman_side_target_t& out )
{
	if ( !ends[ 0 ].found || !ends[ 1 ].found )
		return false;

	int front_end = 0;

	if ( front )
		front_end = ends[ 0 ].normal.m_x * front->m_x + ends[ 0 ].normal.m_y * front->m_y >= ends[ 1 ].normal.m_x * front->m_x + ends[ 1 ].normal.m_y * front->m_y ? 0 : 1;
	else if ( ends[ 0 ].open != ends[ 1 ].open )
		front_end = ends[ 0 ].open ? 0 : 1;
	else
		return false;

	const c_vector tangent( -normal.m_y, normal.m_x, 0.f );
	const float front_sign = front_end == 0 ? 1.f : -1.f;
	const float front_at = front_sign * ends[ front_end ].at;
	const float back_at  = -front_sign * ends[ 1 - front_end ].at;
	const float mid      = ( front_at + back_at ) * 0.5f;

	out.point  = c_vector( point.m_x + tangent.m_x * mid, point.m_y + tangent.m_y * mid, point.m_z );
	out.normal = normal;
	out.u      = c_vector( tangent.m_x * front_sign, tangent.m_y * front_sign, 0.f );
	out.half   = std::fabsf( front_at - back_at ) * 0.5f;
	out.walled = !ends[ 1 - front_end ].open;

	return out.half > 0.25f;
}

static int fireman_side_targets( const c_vector& point, const c_vector& normal, fireman_side_target_t out[ 2 ] )
{
	constexpr float side_max_width = 20.f;

	const fireman_face_end_t ends[ 2 ] = { fireman_face_end( point, normal, 1.f ), fireman_face_end( point, normal, -1.f ) };
	if ( !ends[ 0 ].found || !ends[ 1 ].found )
		return 0;

	if ( ends[ 0 ].at + ends[ 1 ].at < side_max_width ) {
		if ( fireman_side_geometry( point, normal, ends, nullptr, out[ 0 ] ) )
			return 1;

		if ( !ends[ 0 ].open || !ends[ 1 ].open )
			return 0;

		int count = 0;
		for ( const auto& end : ends )
			if ( fireman_side_geometry( point, normal, ends, &end.normal, out[ count ] ) )
				++count;

		return count;
	}

	int count = 0;

	for ( const auto& end : ends ) {
		if ( !end.open )
			continue;

		const fireman_face_end_t side_ends[ 2 ] = { fireman_face_end( end.point, end.normal, 1.f ), fireman_face_end( end.point, end.normal, -1.f ) };
		if ( fireman_side_geometry( end.point, end.normal, side_ends, &normal, out[ count ] ) )
			++count;
	}

	return count;
}

static c_vector fireman_side_park( const c_vector& origin, const c_vector& maxs, const c_vector& point, const c_vector& s, const c_vector& u, const float half,
                                   const float gap, const float back, bool& out_staged )
{
	const float sup_s = maxs.m_x * ( std::fabsf( s.m_x ) + std::fabsf( s.m_y ) );
	const float sup_u = maxs.m_x * ( std::fabsf( u.m_x ) + std::fabsf( u.m_y ) );

	const c_vector rel( origin.m_x - point.m_x, origin.m_y - point.m_y, 0.f );
	const float s_rel = rel.m_x * s.m_x + rel.m_y * s.m_y;
	const float u_rel = rel.m_x * u.m_x + rel.m_y * u.m_y;

	out_staged = u_rel - sup_u >= half - 0.25f && s_rel - sup_s < 0.25f;

	const float u_want = out_staged ? half + sup_u + 2.f : -half + 0.5f + sup_u - back;

	return c_vector( point.m_x + s.m_x * ( sup_s + gap ) + u.m_x * u_want, point.m_y + s.m_y * ( sup_s + gap ) + u.m_y * u_want, 0.f );
}

// straight line to the catch park clears the front face (hull past the side plane by the time it reaches the front plane):
// skip the staged waypoint, it is a full stop + crawl (log 10-05: front approach parked 2u off the strip until drop 11)
static bool fireman_side_line_clear( const c_vector& origin, const c_vector& park, const c_vector& maxs, const c_vector& point, const c_vector& s,
                                     const c_vector& u, const float half, const float margin )
{
	const float sup_s = maxs.m_x * ( std::fabsf( s.m_x ) + std::fabsf( s.m_y ) );
	const float sup_u = maxs.m_x * ( std::fabsf( u.m_x ) + std::fabsf( u.m_y ) );

	const float s0 = ( origin.m_x - point.m_x ) * s.m_x + ( origin.m_y - point.m_y ) * s.m_y;
	const float u0 = ( origin.m_x - point.m_x ) * u.m_x + ( origin.m_y - point.m_y ) * u.m_y;
	const float s1 = ( park.m_x - point.m_x ) * s.m_x + ( park.m_y - point.m_y ) * s.m_y;
	const float u1 = ( park.m_x - point.m_x ) * u.m_x + ( park.m_y - point.m_y ) * u.m_y;

	if ( u0 - u1 <= 1e-6f )
		return s0 - sup_s >= margin;

	const float t = std::clamp( ( u0 - ( half + sup_u ) ) / ( u0 - u1 ), 0.f, 1.f );
	return s0 + ( s1 - s0 ) * t - sup_s >= margin;
}

// slide in from the free staged spot along -u until the hull meets the wall (cap: hull centred on the side). a ladder brush
// sunk into its wall puts the old 0.5u park inside the wall; back < 0 = park forward of it. out_overlap -1 = staged spot solid.
// out_press: wall stopped us with < 1u of side exposed = park must lean on the wall, the steer band (0.5) is wider than the side
static bool fireman_side_back_room( const c_vector& mins, const c_vector& maxs, const c_vector& point, const c_vector& s, const c_vector& u,
                                    const float half, float& out_back, float& out_overlap, bool& out_press )
{
	constexpr float margin       = 0.05f;
	constexpr float min_overlap  = 0.1f;
	constexpr float thin_overlap = 1.f;

	out_overlap = -1.f;
	out_press   = false;

	const float sup_s  = maxs.m_x * ( std::fabsf( s.m_x ) + std::fabsf( s.m_y ) );
	const float sup_u  = maxs.m_x * ( std::fabsf( u.m_x ) + std::fabsf( u.m_y ) );
	const float u_base = -half + 0.5f + sup_u;
	const float u_free = half + sup_u + 2.f;
	const float u_cap  = std::min( 0.f, u_base );

	const auto at = [ & ]( const float uu ) {
		return c_vector( point.m_x + s.m_x * ( sup_s + 1.f ) + u.m_x * uu, point.m_y + s.m_y * ( sup_s + 1.f ) + u.m_y * uu, point.m_z );
	};

	trace_t t;
	c_trace_filter flt( g_ctx.m_local );
	ray_t r( at( u_free ), at( u_cap ), mins, maxs );
	g_interfaces.m_engine_trace->trace_ray( r, mask_playersolid, &flt, &t );

	if ( t.m_start_solid || t.m_all_solid )
		return false;

	float u_stop = t.m_fraction >= 1.f ? u_cap : u_free - t.m_fraction * ( u_free - u_cap ) + margin;

	out_overlap = half - std::max( u_stop - sup_u, -half );
	if ( out_overlap < min_overlap )
		return false;

	const auto floor_z = [ & ]( const c_vector& p ) {
		trace_t d;
		c_trace_filter d_flt( g_ctx.m_local );
		ray_t d_ray( p, c_vector( p.m_x, p.m_y, p.m_z - 1024.f ), mins, maxs );
		g_interfaces.m_engine_trace->trace_ray( d_ray, mask_playersolid, &d_flt, &d );
		return d.m_end.m_z;
	};

	if ( u_stop < u_base && floor_z( at( u_stop ) ) > floor_z( at( u_base ) ) + 8.f )
		u_stop = u_base;
	else
		out_press = t.m_fraction < 1.f && out_overlap < thin_overlap;

	out_back = u_base - u_stop;
	return true;
}

// hull overlap with the picked side's depth band (u), < 0 = beside it with nothing to grab
static float fireman_side_overlap( const c_vector& origin, const c_vector& maxs, const c_vector& point, const c_vector& u, const float half )
{
	const float pu    = ( origin.m_x - point.m_x ) * u.m_x + ( origin.m_y - point.m_y ) * u.m_y;
	const float sup_u = maxs.m_x * ( std::fabsf( u.m_x ) + std::fabsf( u.m_y ) );
	return std::min( pu + sup_u, half ) - std::max( pu - sup_u, -half );
}

static bool fireman_ride_holds( const c_vector& origin, const c_vector& mins, const c_vector& maxs, const c_vector& velocity,
                                const c_vector& latch_normal, const float ride_time )
{
	const float nlen = sqrtf( latch_normal.m_x * latch_normal.m_x + latch_normal.m_y * latch_normal.m_y );
	if ( nlen < 0.01f )
		return true;

	const c_vector n( latch_normal.m_x / nlen, latch_normal.m_y / nlen, 0.f );
	c_vector drift( velocity.m_x, velocity.m_y, 0.f );

	const float into = drift.m_x * n.m_x + drift.m_y * n.m_y;
	if ( into < 0.f ) {
		drift.m_x -= n.m_x * into;
		drift.m_y -= n.m_y * into;
	}

	trace_t slide;
	c_trace_filter slide_flt( g_ctx.m_local );
	ray_t slide_ray( origin, c_vector( origin.m_x + drift.m_x * ride_time, origin.m_y + drift.m_y * ride_time, origin.m_z ), mins, maxs );
	g_interfaces.m_engine_trace->trace_ray( slide_ray, mask_playersolid, &slide_flt, &slide );

	const c_vector end = slide.m_start_solid ? origin : slide.m_end;

	c_vector hold_normal;
	return fireman_can_latch( end, mins, maxs, c_vector( -n.m_x, -n.m_y, 0.f ).to_angle( ).m_y, hold_normal, nullptr, 10.f );
}

struct fireman_sim_t {
	bool valid  = false;
	bool ladder = false;
	bool ground = false;
	bool ducked = false;
	float z_in  = 0.f;
	float z     = 0.f;
	float vz    = 0.f;
	float h     = 0.f;
};

static fireman_sim_t fireman_sim( const c_user_cmd* cmd, const int buttons, const int next = -1 )
{
	fireman_sim_t out;
	auto* local = g_ctx.m_local;
	if ( !local || !g_interfaces.m_move_helper || !g_interfaces.m_prediction )
		return out;

	const int frame = g_interfaces.m_prediction->m_commands_predicted - 1;
	g_prediction.restore_entity_to_predicted_frame( frame );

	const int tick_base = local->get_tick_base( );
	c_user_cmd sim_cmd  = *cmd;
	out.z_in            = local->get_origin( ).m_z;

	for ( int i = 0; i < ( next >= 0 ? 2 : 1 ); ++i ) {
		sim_cmd.m_buttons       = i == 0 ? buttons : next;
		local->get_tick_base( ) = tick_base + i;
		g_prediction.begin( local, &sim_cmd );
		g_prediction.end( local );
	}

	const c_vector velocity = local->get_velocity( );
	out.valid  = true;
	out.ladder = local->get_move_type( ) == move_type_ladder;
	out.ground = ( local->get_flags( ) & fl_onground ) != 0;
	out.ducked = ( local->get_flags( ) & fl_ducking ) != 0;
	out.z      = local->get_origin( ).m_z;
	out.vz     = velocity.m_z;
	out.h      = velocity.length_2d( );

	g_prediction.restore_entity_to_predicted_frame( frame );
	return out;
}

static bool fireman_launched( const fireman_sim_t& s )
{
	return s.valid && !s.ladder && !s.ground && s.vz > 30.f;
}

static float fireman_launch_apex( const fireman_sim_t& s, const bool crouch )
{
	if ( !fireman_launched( s ) )
		return -FLT_MAX;

	const float gravity = g_convars[ HASH_BT( "sv_gravity" ) ] ? std::max( g_convars[ HASH_BT( "sv_gravity" ) ]->get_float( ), 1.f ) : 800.f;
	const float apex    = s.z + s.vz * s.vz / ( 2.f * gravity );

	if ( crouch )
		return apex + ( s.ducked ? 0.f : 9.f );

	return apex - ( s.ducked ? 18.f : 0.f );
}

struct fireman_drop_target_t {
	bool found    = false;
	bool overhead = false;
	float yaw     = 0.f;
	float lateral = 0.f;
	float depth   = 0.f;
	float floor_depth = 0.f;
	c_vector hit = c_vector( 0.f, 0.f, 0.f );
	bool deep = false;
};

// remembered: last column the scan hit (world xy). re-probed exactly before the ring: 24 line probes at fixed offsets land on a
// 1u strip only some ticks (log 10-05 08:59: drop_in t=1 once, then lock idle, ladder seen again at drop 9-34)
static bool fireman_find_drop_target( const c_vector& origin, const c_vector& mins, const c_vector& maxs, const float max_depth, const float max_lateral,
                                      fireman_drop_target_t& out, const c_vector* remembered = nullptr )
{
	out = fireman_drop_target_t{ };

	const float feet_z = origin.m_z + mins.m_z + 1.f;

	const auto probe = [ & ]( const float ox, const float oy ) -> float {
		const c_vector start( origin.m_x + ox, origin.m_y + oy, feet_z );

		trace_t t;
		c_trace_filter flt( g_ctx.m_local );
		ray_t r( start, c_vector( start.m_x, start.m_y, start.m_z - max_depth ) );
		g_interfaces.m_engine_trace->trace_ray( r, e_contents::contents_ladder, &flt, &t );

		return t.m_fraction >= 1.f ? -1.f : t.m_fraction * max_depth;
	};

	const auto floor_at = [ & ]( const float ox, const float oy ) -> float {
		const c_vector start( origin.m_x + ox, origin.m_y + oy, feet_z );

		trace_t t;
		c_trace_filter_trace_type_everything_filter_props flt;
		ray_t r( start, c_vector( start.m_x, start.m_y, start.m_z - max_depth ) );
		g_interfaces.m_engine_trace->trace_ray( r, mask_playersolid, &flt, &t );

		return t.m_fraction >= 1.f ? max_depth : t.m_fraction * max_depth;
	};

	const auto fill = [ & ]( const float ox, const float oy, const float d, const float f ) {
		constexpr float min_column = 64.f;

		out.found       = true;
		out.depth       = d;
		out.floor_depth = f;
		out.hit         = c_vector( origin.m_x + ox, origin.m_y + oy, feet_z - d );

		trace_t t;
		c_trace_filter flt( g_ctx.m_local );
		ray_t r( c_vector( out.hit.m_x, out.hit.m_y, out.hit.m_z - min_column ), c_vector( out.hit.m_x, out.hit.m_y, out.hit.m_z - min_column - 1.f ) );
		g_interfaces.m_engine_trace->trace_ray( r, e_contents::contents_ladder, &flt, &t );

		out.deep = t.m_start_solid;
	};

	constexpr float inset = 2.f;
	const float xs[ 5 ]   = { 0.f, maxs.m_x - inset, mins.m_x + inset, maxs.m_x - inset, mins.m_x + inset };
	const float ys[ 5 ]   = { 0.f, maxs.m_y - inset, mins.m_y + inset, mins.m_y + inset, maxs.m_y - inset };

	for ( int i = 0; i < 5; ++i ) {
		const float d = probe( xs[ i ], ys[ i ] );
		if ( d < 0.f )
			continue;
		if ( out.found && d >= out.depth )
			continue;

		const float f = floor_at( xs[ i ], ys[ i ] );
		if ( d > f + 8.f )
			continue;

		fill( xs[ i ], ys[ i ], d, f );
		out.overhead = true;
	}

	if ( out.found )
		return true;

	if ( remembered ) {
		const float ox  = remembered->m_x - origin.m_x;
		const float oy  = remembered->m_y - origin.m_y;
		const float lat = sqrtf( ox * ox + oy * oy );

		if ( lat <= max_lateral + 32.f ) {
			const float d = probe( ox, oy );

			if ( d >= 0.f ) {
				const float f = floor_at( ox, oy );

				if ( d <= f + 8.f ) {
					fill( ox, oy, d, f );
					out.overhead = std::fabsf( ox ) < maxs.m_x && std::fabsf( oy ) < maxs.m_y;
					out.yaw      = c_vector( ox, oy, 0.f ).to_angle( ).m_y;
					out.lateral  = lat;
					return true;
				}
			}
		}
	}

	{
		float best_lat = FLT_MAX, best_x = 0.f, best_y = 0.f, best_d = 0.f, best_f = 0.f;

		for ( const fireman_map_ladder_t& ladder : fireman_map_ladders( ) ) {
			if ( ladder.lo.m_z >= feet_z )
				continue;

			const float in_x = std::min( 2.f, ( ladder.hi.m_x - ladder.lo.m_x ) * 0.25f );
			const float in_y = std::min( 2.f, ( ladder.hi.m_y - ladder.lo.m_y ) * 0.25f );
			const float ox   = std::clamp( origin.m_x, ladder.lo.m_x + in_x, ladder.hi.m_x - in_x ) - origin.m_x;
			const float oy   = std::clamp( origin.m_y, ladder.lo.m_y + in_y, ladder.hi.m_y - in_y ) - origin.m_y;
			const float lat  = sqrtf( ox * ox + oy * oy );

			if ( lat > max_lateral || lat >= best_lat )
				continue;

			const float d = probe( ox, oy );
			if ( d < 0.f )
				continue;

			const float f = floor_at( ox, oy );
			if ( d > f + 8.f )
				continue;

			best_lat = lat, best_x = ox, best_y = oy, best_d = d, best_f = f;
		}

		if ( best_lat < FLT_MAX ) {
			fill( best_x, best_y, best_d, best_f );
			out.overhead = std::fabsf( best_x ) < maxs.m_x && std::fabsf( best_y ) < maxs.m_y;
			out.yaw      = c_vector( best_x, best_y, 0.f ).to_angle( ).m_y;
			out.lateral  = best_lat;
			return true;
		}
	}

	constexpr int dirs     = 8;
	const float radii[ 3 ] = { max_lateral * 0.34f, max_lateral * 0.67f, max_lateral };
	const float step       = std::numbers::pi_v< float > * 2.0f / static_cast< float >( dirs );

	for ( int ri = 0; ri < 3; ++ri ) {
		for ( int i = 0; i < dirs; ++i ) {
			const float a  = step * static_cast< float >( i );
			const float ox = cosf( a ) * radii[ ri ];
			const float oy = sinf( a ) * radii[ ri ];

			const float d = probe( ox, oy );
			if ( d < 0.f )
				continue;

			const float f = floor_at( ox, oy );
			if ( d > f + 8.f )
				continue;

			fill( ox, oy, d, f );
			out.yaw     = c_vector( cosf( a ), sinf( a ), 0.f ).to_angle( ).m_y;
			out.lateral = radii[ ri ];
			return true;
		}
	}

	return false;
}

static bool fireman_column_face( const c_vector& origin, const c_vector& mins, const c_vector& maxs, const c_vector& hit, fireman_ladder_hit_t& out )
{
	constexpr float under_top = 8.f;
	constexpr float from_out  = 48.f;
	constexpr int dirs        = 8;

	const float step = std::numbers::pi_v< float > * 2.0f / static_cast< float >( dirs );
	const c_vector end( hit.m_x, hit.m_y, hit.m_z - under_top );

	out = fireman_ladder_hit_t{ };

	// end strip of a thin ladder (nuke 24x1) is never caught: hull parks beside 1u (log 10-06 13:07 yaw=-180 misses)
	bool out_narrow = true;

	for ( int i = 0; i < dirs; ++i ) {
		const float a = step * static_cast< float >( i );
		const c_vector start( end.m_x + cosf( a ) * from_out, end.m_y + sinf( a ) * from_out, end.m_z );

		trace_t ladder_trace;
		c_trace_filter ladder_filter( g_ctx.m_local );
		ray_t ladder_ray( start, end );
		g_interfaces.m_engine_trace->trace_ray( ladder_ray, e_contents::contents_ladder, &ladder_filter, &ladder_trace );

		if ( ladder_trace.m_start_solid || ladder_trace.m_fraction >= 1.f )
			continue;

		const c_vector& normal = ladder_trace.m_plane.m_normal;
		if ( std::fabsf( normal.m_z ) >= 0.3f || ( normal.m_x == 0.f && normal.m_y == 0.f ) )
			continue;

		trace_t solid_trace;
		c_trace_filter_trace_type_everything_filter_props solid_filter;
		ray_t solid_ray( start, end );
		g_interfaces.m_engine_trace->trace_ray( solid_ray, mask_playersolid, &solid_filter, &solid_trace );

		if ( solid_trace.m_start_solid || solid_trace.m_fraction < ladder_trace.m_fraction - 2.f / from_out )
			continue;

		const float dist = c_vector( ladder_trace.m_end.m_x - origin.m_x, ladder_trace.m_end.m_y - origin.m_y, 0.f ).length_2d( );

		const float nlen = sqrtf( normal.m_x * normal.m_x + normal.m_y * normal.m_y );
		const c_vector n( normal.m_x / nlen, normal.m_y / nlen, 0.f );

		float span_lo, span_hi;
		fireman_face_span( ladder_trace.m_end, n, c_vector( -n.m_y, n.m_x, 0.f ), span_lo, span_hi );
		const bool narrow = span_hi - span_lo < 8.f;

		if ( out.found && ( ( narrow && !out_narrow ) || ( narrow == out_narrow && dist >= out.dist ) ) )
			continue;

		if ( fireman_face_is_stub( ladder_trace.m_end, n, mins, maxs, origin.m_z ) )
			continue;

		out_narrow    = narrow;
		out.found     = true;
		out.flat_face = true;
		out.nz        = normal.m_z;
		out.dist      = dist;
		out.point     = ladder_trace.m_end;
		out.normal    = c_vector( normal.m_x / nlen, normal.m_y / nlen, 0.f );
		out.yaw       = c_vector( -out.normal.m_x, -out.normal.m_y, 0.f ).to_angle( ).m_y;
	}

	return out.found;
}

static bool fireman_cliff_catch_risk( const c_vector& origin, const c_vector& mins, const c_vector& maxs, const c_vector& velocity, const bool on_ground )
{
	if ( on_ground || velocity.m_z > 0.f || velocity.m_z <= -50.f || velocity.m_x == 0.f || velocity.m_y == 0.f )
		return false;

	const float speed = velocity.length( );
	if ( speed < 0.01f )
		return false;

	constexpr float grab_distance      = 24.f;
	constexpr float grab_height_delta  = 6.f;
	constexpr unsigned int ladder_mask = mask_playersolid & ~e_contents::contents_playerclip;

	const c_vector start( origin.m_x, origin.m_y, origin.m_z - grab_height_delta );
	const c_vector end( origin.m_x - velocity.m_x / speed * grab_distance, origin.m_y - velocity.m_y / speed * grab_distance,
	                    origin.m_z - velocity.m_z / speed * grab_distance );

	trace_t t;
	c_trace_filter flt( g_ctx.m_local );
	ray_t r( start, end, mins, maxs );
	g_interfaces.m_engine_trace->trace_ray( r, ladder_mask, &flt, &t );

	if ( t.m_start_solid || t.m_all_solid )
		return true;
	if ( t.m_fraction >= 1.f )
		return false;
	if ( t.m_plane.m_normal.m_z == 1.f )
		return false;

	return botox_is_ladder_trace( t );
}

/* physics dt (engine interval, never n_tick::interval: the 128 fix would size it for a server that isn't there) */
static float fireman_phys_dt( )
{
	return g_interfaces.m_global_vars_base && g_interfaces.m_global_vars_base->m_interval_per_tick > 0.f
	         ? g_interfaces.m_global_vars_base->m_interval_per_tick
	         : 0.015625f;
}

static float fireman_gravity( )
{
	return g_convars[ HASH_BT( "sv_gravity" ) ] ? std::max( g_convars[ HASH_BT( "sv_gravity" ) ]->get_float( ), 1.f ) : 800.f;
}

static float fireman_air_friction( const float vz_start, const float g, const float dt )
{
	const float vz_cat = vz_start + g * dt * 0.5f;
	return vz_cat > 0.f && vz_cat <= 140.f ? 0.25f : 1.f;
}

// written wishspeed that lands a velocity delta this tick (n_air::exact_wish: friction, duck crop, stamina-cut maxspeed).
// engine caps (addspeed = 30 - dot) can only cut it, never overshoot. air ticks only (callers)
static float fireman_air_wish_speed( const float need )
{
	return n_air::exact_wish( need, g_prediction.backup_data.m_velocity.m_z, n_air::read_world( ) );
}

static int fireman_catch_ticks( )
{
	return static_cast< int >( std::ceilf( 0.2f / std::max( fireman_phys_dt( ), 0.001f ) ) );
}

constexpr float fireman_ride_floor_probe = 2.f;

static float fireman_grab_height( const float ride_speed )
{
	if ( ride_speed <= 0.f )
		return 0.f;

	return ride_speed * static_cast< float >( fireman_catch_ticks( ) ) * fireman_phys_dt( ) + fireman_ride_floor_probe - 0.5f;
}

static bool fireman_ride_sticks( const c_vector& origin, const c_vector& mins, const c_vector& maxs, const c_vector& normal )
{
	constexpr float stick_reach        = 10.f;
	constexpr unsigned int ladder_mask = mask_playersolid & ~e_contents::contents_playerclip;

	trace_t t;
	c_trace_filter flt( g_ctx.m_local );
	ray_t r( origin, origin - normal * stick_reach, mins, maxs );
	g_interfaces.m_engine_trace->trace_ray( r, ladder_mask, &flt, &t );

	if ( t.m_start_solid || t.m_all_solid )
		return true;

	constexpr float down_plane = -0.05f;

	const bool down_bevel = t.m_plane.m_normal.m_z < down_plane && t.m_plane.m_normal.dot_product( normal ) < 0.99f;

	return t.m_fraction < 1.f && t.m_plane.m_normal.m_z < 0.7f && !down_bevel && botox_is_ladder_trace( t );
}

static bool fireman_ride_next_sticks( const c_vector& origin, const c_vector& mins, const c_vector& maxs, c_vector velocity, const c_vector& normal )
{
	c_vector pos = origin;
	float left   = fireman_phys_dt( );

	for ( int bump = 0; bump < 2 && left > 0.f; ++bump ) {
		trace_t t;
		c_trace_filter flt( g_ctx.m_local );
		ray_t r( pos, pos + velocity * left, mins, maxs );
		g_interfaces.m_engine_trace->trace_ray( r, mask_playersolid, &flt, &t );

		if ( t.m_start_solid || t.m_all_solid )
			break;

		pos = t.m_end;
		if ( t.m_fraction >= 1.f )
			break;

		left -= left * t.m_fraction;
		velocity = velocity - t.m_plane.m_normal * velocity.dot_product( t.m_plane.m_normal );
	}

	return fireman_ride_sticks( pos, mins, maxs, normal );
}

// LadderMove airborne IN_BACK only (csgosrc2019 gamemovement.cpp:3718-3776), = tools/fr_fast_descend_check.py ladder_velocity
static c_vector fireman_ladder_back_velocity( const c_vector& n, const float pitch, const float yaw )
{
	c_vector fwd;
	g_math.angle_vectors( c_angle( pitch, yaw, 0.f ), &fwd );

	const c_vector velocity = fwd * -200.f;
	const c_vector perp     = c_vector( 0.f, 0.f, 1.f ).cross_product( n ).normalized( );
	const float into        = velocity.dot_product( n );
	const c_vector cross    = n * into;
	c_vector lateral        = velocity - cross;
	const c_vector tmp      = n.cross_product( perp );
	const float tmp_dist    = tmp.dot_product( lateral );
	const float perp_dist   = perp.dot_product( lateral );

	if ( ( perp * perp_dist + cross ).normalized( ).dot_product( n ) < -0.707f )
		lateral = tmp * tmp_dist + perp * ( 0.2f * perp_dist );

	return ( lateral - tmp * into ) * 0.78f;
}

struct fireman_ride_end_t {
	float room    = FLT_MAX;
	float grab_at = 0.f;
	float step    = 0.f;
	bool floor    = false;
};

static fireman_ride_end_t fireman_ride_end( const c_vector& origin, const c_vector& mins, const c_vector& maxs, const c_vector& velocity,
                                            const c_vector& normal, const float drop_left )
{
	fireman_ride_end_t out{ };

	c_vector vc      = velocity;
	const float into = vc.dot_product( normal );
	if ( into < 0.f )
		vc = vc - normal * into;

	// along-face drift is the approach's to brake: in the path a slide off a thin side reads as the ladder's end = grab at any height
	if ( const float nh = sqrtf( normal.m_x * normal.m_x + normal.m_y * normal.m_y ); nh > 0.01f ) {
		const c_vector tangent( -normal.m_y / nh, normal.m_x / nh, 0.f );
		vc = vc - tangent * vc.dot_product( tangent );
	}

	const float speed = vc.length( );
	if ( speed < 1.f || vc.m_z >= 0.f )
		return out;

	const float dt = fireman_phys_dt( );
	out.grab_at    = fireman_grab_height( -vc.m_z );
	out.step       = -vc.m_z * dt;

	const c_vector dir = vc * ( 1.f / speed );
	float len          = speed * static_cast< float >( fireman_catch_ticks( ) + 2 ) * dt + 4.f;

	trace_t path;
	c_trace_filter path_flt( g_ctx.m_local );
	ray_t path_ray( origin, origin + dir * len, mins, maxs );
	g_interfaces.m_engine_trace->trace_ray( path_ray, mask_playersolid, &path_flt, &path );

	const bool blocked = path.m_fraction < 1.f && !path.m_start_solid;
	out.floor          = blocked && path.m_plane.m_normal.m_z >= 0.7f;
	if ( blocked )
		len *= path.m_fraction;

	if ( fireman_ride_sticks( origin + dir * len, mins, maxs, normal ) ) {
		out.room = out.floor ? len * -dir.m_z : blocked && drop_left >= 0.f ? drop_left : FLT_MAX;
		return out;
	}

	if ( !fireman_ride_sticks( origin, mins, maxs, normal ) ) {
		out.room = 0.f;
		return out;
	}

	if ( normal.m_z < -0.02f )
		return out;

	float lo = 0.f, hi = len;
	for ( int i = 0; i < 8; ++i ) {
		const float mid = ( lo + hi ) * 0.5f;
		if ( fireman_ride_sticks( origin + dir * mid, mins, maxs, normal ) )
			lo = mid;
		else
			hi = mid;
	}

	out.room = lo * -dir.m_z;
	return out;
}

static float fireman_air_accel_speed( )
{
	return n_air::accel_speed( n_air::read_world( ) );
}

struct fireman_landing_t {
	bool valid  = false;
	bool hit    = false;
	bool ladder = false;
	float z     = 0.f;
	c_vector normal{ };
};

static fireman_landing_t fireman_predict_landing( const c_vector& origin, const c_vector& mins, const c_vector& maxs, const c_vector& velocity,
                                                  const c_vector* want, const float max_time )
{
	constexpr float coast_chord = 0.1f;
	constexpr float dead_band   = 2.f;

	n_air::state_t from;
	from.origin   = origin;
	from.velocity = velocity;

	const n_air::result_t r = n_air::fly( from, n_air::read_world( mins, maxs ), max_time, coast_chord, dead_band, nullptr,
	                                      [ & ]( const n_air::state_t&, c_vector& steer_to ) {
		                                      if ( !want )
			                                      return false;
		                                      steer_to = *want;
		                                      return true;
	                                      } );

	fireman_landing_t out;
	if ( !r.valid )
		return out;

	out.valid = true;
	out.hit   = r.hit;
	if ( r.hit ) {
		out.ladder = botox_is_ladder_trace( r.trace );
		out.z      = r.trace.m_end.m_z;
		out.normal = r.trace.m_plane.m_normal;
	}
	return out;
}

/* runs f when fire_man returns, whichever return (guards that must see the final cmd) */
template < class F >
struct fireman_scope_exit_t {
	F f;
	~fireman_scope_exit_t( ) { f( ); }
};
template < class F >
fireman_scope_exit_t( F ) -> fireman_scope_exit_t< F >;

/* player's view at fire_man entry: every write keeps it (tb style), forced view only when the engine would latch differently */
static c_angle s_fr_live_view{ };

/* LadderMove :3595-3614 off a written move: wishdir keeps pitch, hull 2u. 1 latch, 0 none, -1 start solid = can't tell */
static int fireman_move_latch( const c_angle& view, const float fwd, const float side, c_vector& out_normal )
{
	auto* col = g_ctx.m_local ? g_ctx.m_local->get_collideable( ) : nullptr;
	if ( !col )
		return -1;

	c_vector f, r;
	g_math.angle_vectors( view, &f, &r, nullptr );
	c_vector wish   = f * fwd + r * side;
	const float len = wish.length( );
	if ( len < 1e-6f )
		return 0;
	wish *= 1.f / len;

	const c_vector origin = g_prediction.backup_data.m_origin;
	trace_t t;
	c_trace_filter flt( g_ctx.m_local );
	ray_t ray( origin, origin + wish * 2.f, col->get_obb_mins( ), col->get_obb_maxs( ) );
	g_interfaces.m_engine_trace->trace_ray( ray, mask_playersolid & ~e_contents::contents_playerclip, &flt, &t );

	if ( t.m_start_solid || t.m_all_solid )
		return -1;
	if ( t.m_fraction >= 1.f || t.m_plane.m_normal.m_z == 1.f || !botox_is_ladder_trace( t ) )
		return 0;
	out_normal = t.m_plane.m_normal;
	return 1;
}

static bool fireman_same_latch( const int a, const c_vector& na, const int b, const c_vector& nb )
{
	return a >= 0 && a == b && ( a == 0 || na.dot_product( nb ) > 0.999f );
}

/* air/walk wishdir drops pitch, so the live yaw rotation is exact; only the latch trace sees pitch, re-asked */
static void fireman_write_wish( c_user_cmd* cmd, const float view_yaw, const float wish_yaw, const float speed )
{
	constexpr float deg_to_rad = std::numbers::pi_v< float > / 180.f;

	const c_angle forced( 0.f, view_yaw, 0.f );
	const float delta = g_math.normalize_angle( wish_yaw - view_yaw ) * deg_to_rad;
	const float fwd   = speed * cosf( delta );
	const float side  = -speed * sinf( delta );

	const float live_delta = g_math.normalize_angle( wish_yaw - s_fr_live_view.m_y ) * deg_to_rad;
	const float live_fwd   = speed * cosf( live_delta );
	const float live_side  = -speed * sinf( live_delta );

	c_vector n_forced{ }, n_live{ };
	const int l_forced = fireman_move_latch( forced, fwd, side, n_forced );
	const int l_live   = fireman_move_latch( s_fr_live_view, live_fwd, live_side, n_live );

	if ( fireman_same_latch( l_live, n_live, l_forced, n_forced ) ) {
		cmd->m_view_point   = s_fr_live_view;
		cmd->m_forward_move = live_fwd;
		cmd->m_side_move    = live_side;
		return;
	}

	cmd->m_view_point   = forced;
	cmd->m_forward_move = fwd;
	cmd->m_side_move    = side;
}

/* latch press in the player's view: ( f, s ) = ( F.m, R.m ) is the pitched wishdir with the most reach along m.
   kept only if the engine trace latches the same face, else false = caller writes its forced view */
static bool fireman_live_press( c_user_cmd* cmd, const float yaw, const float push, const c_vector& face_normal )
{
	c_vector f, r, m;
	g_math.angle_vectors( s_fr_live_view, &f, &r, nullptr );
	g_math.angle_vectors( c_angle( 0.f, yaw, 0.f ), &m );

	const float fm  = f.dot_product( m );
	const float rm  = r.dot_product( m );
	const float len = sqrtf( fm * fm + rm * rm );
	if ( len < 1e-3f )
		return false;

	const float fwd  = push * fm / len;
	const float side = push * rm / len;

	c_vector n{ };
	if ( fireman_move_latch( s_fr_live_view, fwd, side, n ) != 1 || n.dot_product( face_normal ) <= 0.999f )
		return false;

	cmd->m_view_point   = s_fr_live_view;
	cmd->m_forward_move = fwd;
	cmd->m_side_move    = side;
	return true;
}

static bool fireman_brake_drift( c_user_cmd* cmd, const c_vector& origin, const c_vector& mins, const c_vector& maxs,
                                 const c_vector& velocity, const float view_yaw )
{
	const float speed = sqrtf( velocity.m_x * velocity.m_x + velocity.m_y * velocity.m_y );
	if ( speed <= 2.f )
		return false;

	const float brake_yaw = c_vector( -velocity.m_x, -velocity.m_y, 0.f ).to_angle( ).m_y;

	c_vector brake_normal;
	if ( fireman_can_latch( origin, mins, maxs, brake_yaw, brake_normal ) )
		return false;

	fireman_write_wish( cmd, view_yaw, brake_yaw, fireman_air_wish_speed( speed ) );
	cmd->m_buttons &= ~( in_forward | in_back | in_moveleft | in_moveright );
	cmd->m_buttons |= in_jump;

	return true;
}

constexpr float fireman_brake_margin = 0.9f;

static float fireman_brake_travel( const float w, const float brake, const float dt, float vz, const float g )
{
	float v = w;
	float d = w * dt;

	for ( int k = 0; k < 256 && v > 0.f; ++k ) {
		vz -= g * dt;
		v = std::max( v - brake * fireman_air_friction( vz, g, dt ), 0.f );
		d += v * dt;
	}

	return d;
}

static float fireman_hold_speed( const float gap_error, const float decel, const float dt, const float closing_cap, const float vz )
{
	// never cap a want at the 30 veer cap: 30 limits gaining speed (addspeed = 30 - dot), not carried
	// speed; capping it brakes a useful 200 u/s drift. cap at what the brake can undo
	constexpr float profile_cap = 450.f;

	const float gap   = std::fabsf( gap_error );
	const float brake = fireman_brake_margin * std::max( decel, 1.f ) * dt;
	const float g     = fireman_gravity( );

	float lo = 0.f, hi = std::min( profile_cap, std::max( closing_cap, 1.f ) );

	if ( fireman_brake_travel( hi, brake, dt, vz, g ) <= gap )
		lo = hi;
	else
		for ( int i = 0; i < 12; ++i ) {
			const float mid = ( lo + hi ) * 0.5f;
			( fireman_brake_travel( mid, brake, dt, vz, g ) <= gap ? lo : hi ) = mid;
		}

	float toward = lo;

	/* anti-ringing for input error (stale backup, a latch-refused tick): the last units spread over >= 4 ticks.
	   Arrival only: binds under ~14u of error at 64 tick, ~3.5 at 128. */
	constexpr float min_ticks = 4.f;

	toward = std::min( toward, std::fabsf( gap_error ) / ( min_ticks * std::max( dt, 0.001f ) ) );

	return std::min( toward, std::max( closing_cap, 1.f ) );
}

static c_vector fireman_hold_delta( const c_vector& vel, const c_vector& to_target, const float gap_error, const float decel, const float dt,
                                    const float closing_cap, const float vz )
{
	const float toward = fireman_hold_speed( gap_error, decel, dt, closing_cap, vz );
	const float want   = gap_error >= 0.f ? toward : -toward;

	return c_vector( to_target.m_x * want - vel.m_x, to_target.m_y * want - vel.m_y, 0.f );
}

static c_vector fireman_steer_delta( const c_vector& vel, const c_vector& to_target, const float gap_error, const float decel,
                                     const float dt, const float closing_cap, const bool gain, const float vz, bool* out_perp = nullptr )
{
	const c_vector park = fireman_hold_delta( vel, to_target, gap_error, decel, dt, closing_cap, vz );

	constexpr float turn_min_speed = 60.f;
	constexpr float gain_min_speed = 28.f;
	constexpr float turn_min_cos   = 0.94f;
	constexpr float turn_max_cos   = -0.71f;

	if ( out_perp )
		*out_perp = false;

	const float speed = sqrtf( vel.m_x * vel.m_x + vel.m_y * vel.m_y );

	if ( speed <= ( gain ? gain_min_speed : turn_min_speed ) || closing_cap <= 31.f || gap_error <= 0.f )
		return park;

	const float inv = 1.f / speed;
	const c_vector v_hat( vel.m_x * inv, vel.m_y * inv, 0.f );

	const float aim = v_hat.m_x * to_target.m_x + v_hat.m_y * to_target.m_y;
	if ( aim <= turn_max_cos )
		return park;

	if ( gap_error <= fireman_brake_travel( speed, fireman_brake_margin * std::max( decel, 1.f ) * dt, dt, vz, fireman_gravity( ) ) + 8.f )
		return park;

	if ( aim >= turn_min_cos && ( !gain || speed >= fireman_hold_speed( gap_error, decel, dt, closing_cap, vz ) ) )
		return park;

	if ( out_perp )
		*out_perp = true;

	const float cross    = v_hat.m_x * to_target.m_y - v_hat.m_y * to_target.m_x;
	const c_vector perp  = cross >= 0.f ? c_vector( -v_hat.m_y, v_hat.m_x, 0.f ) : c_vector( v_hat.m_y, -v_hat.m_x, 0.f );

	// target inside the turning circle: a turn orbits it forever, brake instead
	if ( aim < turn_min_cos ) {
		const float turn   = std::min( std::max( decel, 1.f ) * dt * fireman_air_friction( vz, fireman_gravity( ), dt ), 30.f );
		const float radius = speed * speed * dt / turn;
		const float cx     = to_target.m_x * gap_error - perp.m_x * radius;
		const float cy     = to_target.m_y * gap_error - perp.m_y * radius;

		if ( cx * cx + cy * cy < radius * radius )
			return park;
	}

	return c_vector( perp.m_x * 450.f, perp.m_y * 450.f, 0.f );
}

static c_vector fireman_air_step( const c_vector& vel, const c_vector& wishdir, const float accel_speed )
{
	constexpr float air_veer_cap = 30.f;

	float add = air_veer_cap - ( vel.m_x * wishdir.m_x + vel.m_y * wishdir.m_y );
	if ( add <= 0.f )
		return vel;
	if ( add > accel_speed )
		add = accel_speed;

	return c_vector( vel.m_x + wishdir.m_x * add, vel.m_y + wishdir.m_y * add, 0.f );
}

// most speed along n this tick: a push straight down n adds 30 - v.n = nothing past 30 u/s, angled off it keeps gaining
static c_vector fireman_gain_dir( const c_vector& vel, const c_vector& n, const float side, const float accel_speed )
{
	const c_vector t( -n.m_y * side, n.m_x * side, 0.f );

	c_vector best_dir = n;
	float best        = -FLT_MAX;

	for ( int deg = 0; deg < 90; ++deg ) {
		const float a = static_cast< float >( deg ) * ( 3.14159265f / 180.f );
		const c_vector w( n.m_x * cosf( a ) + t.m_x * sinf( a ), n.m_y * cosf( a ) + t.m_y * sinf( a ), 0.f );
		const c_vector v = fireman_air_step( vel, w, accel_speed );
		const float along = v.m_x * n.m_x + v.m_y * n.m_y;

		if ( along > best ) {
			best     = along;
			best_dir = w;
		}
	}

	return best_dir;
}

// authority per tick off the world ( stamina bleeds during the approach ): the steer re-reads it every cmd, so must the sim
static int fireman_hold_ticks( c_vector vel, c_vector rel, const n_air::world_t& world, const float hold_dist, const int max_ticks,
                               const float dt, const bool gain, float vz, float& out_min_dist )
{
	const float g = fireman_gravity( );

	out_min_dist = sqrtf( rel.m_x * rel.m_x + rel.m_y * rel.m_y );

	for ( int t = 0; t < max_ticks; ++t, vz -= g * dt ) {
		const float dist = sqrtf( rel.m_x * rel.m_x + rel.m_y * rel.m_y );

		out_min_dist = std::min( out_min_dist, dist );

		if ( dist <= hold_dist + 2.f )
			return t;

		const float accel_speed = n_air::accel_speed( world, t );
		const float decel       = accel_speed / std::max( dt, 0.001f );

		const c_vector to_target = dist > 0.01f ? c_vector( rel.m_x / dist, rel.m_y / dist, 0.f ) : c_vector( 1.f, 0.f, 0.f );
		// 450 = approach cap (the window's 30 is outside catch_time). must fly the same controller as the
		// approach (steer_delta) or it refuses ladders the turn can reach
		const c_vector delta     = fireman_steer_delta( vel, to_target, dist - hold_dist, decel, dt, 450.f, gain, vz );
		const float need         = sqrtf( delta.m_x * delta.m_x + delta.m_y * delta.m_y );

		if ( need > 0.01f )
			vel = fireman_air_step( vel, c_vector( delta.m_x / need, delta.m_y / need, 0.f ),
			                        std::min( accel_speed * fireman_air_friction( vz, g, dt ), need ) );

		rel.m_x -= vel.m_x * dt;
		rel.m_y -= vel.m_y * dt;
	}

	return -1;
}

void n_movement::impl_t::fire_man( c_user_cmd* cmd )
{
	constexpr float ignore_jump_time = 0.2f;
	const float default_reach        = std::clamp( GET_VARIABLE( g_variables.m_fire_man_reach, float ), 64.f, 640.f ); // ladder search radius (menu "reach")
	constexpr float air_veer_cap     = 30.f;
	constexpr float max_catch_time   = 4.f;
	constexpr float grab_clearance   = 12.f;
	constexpr int launch_window      = 192;
	constexpr int move_buttons       = in_forward | in_back | in_moveleft | in_moveright;
	constexpr int recall_life        = 64;

	m_fireman_data.is_ladder = false;
	m_fireman_data.owns_cmd  = false;
	s_fr_live_view           = cmd->m_view_point;

	// log only: what PRE left in the cmd (autostrafe / keys) + view spin per tick (360s)
	static float last_view_yaw = 0.f;
	const float in_move        = sqrtf( cmd->m_forward_move * cmd->m_forward_move + cmd->m_side_move * cmd->m_side_move );
	const float view_spin      = g_math.normalize_angle( cmd->m_view_point.m_y - last_view_yaw );
	last_view_yaw              = cmd->m_view_point.m_y;

	const auto stand_down = [ & ]( ) {
		m_fireman_data.awall        = false;
		m_fireman_data.fr_hit       = false;
		m_fireman_data.was_ladder   = false;
		m_fireman_data.yaw_valid    = false;
		m_fireman_data.engaged      = false;
		m_fireman_data.fell_ready   = false;
		m_fireman_data.launch_ticks  = 0;
		m_fireman_data.launch_flung  = false;
		m_fireman_data.launch_jumped = false;
		m_fireman_data.backing_out   = false;
		m_fireman_data.steer_ticks   = 0;
		m_fireman_data.ride_ticks    = 0;
		m_fireman_data.ride_stall    = 0;
		m_fireman_data.brake_ticks   = 0;
		m_fireman_data.drop_in_lock    = false;
		m_fireman_data.lock_lost_ticks = 0;
		m_fireman_data.lock_ticks      = 0;
		m_fireman_data.ladder_lock       = false;
		m_fireman_data.ladder_lost_ticks = 0;
		m_fireman_data.lock_idle_ticks   = 0;
		m_fireman_data.lock_cooldown     = 0;
	};

	const auto lock_earned = [ & ]( ) { m_fireman_data.lock_idle_ticks = 0; };

	if ( !g_ctx.m_local )
		return;
	if ( !g_interfaces.m_engine_client->is_in_game( ) )
		return;
	if ( !g_ctx.m_local->is_alive( ) )
		return;

	// last ground z, tracked above every gate (gates return on key ticks, else stale floor refuses real drops)
	if ( ( g_prediction.backup_data.m_flags & fl_onground ) != 0 ) {
		m_fireman_data.last_ground_z  = g_prediction.backup_data.m_origin.m_z;
		m_fireman_data.ground_z_valid = true;
		m_fireman_data.recall_valid = false;
		m_fireman_data.drop_in_fall = false;
		m_fireman_data.drop_col_valid = false;
		m_fireman_data.lock_cooldown  = 0;

		// side pick is the ladder's, not the fall's: a hop touches down 1 tick and must not re-pick (flips sides)
		if ( m_fireman_data.side_valid && ++m_fireman_data.side_ground_ticks * fireman_phys_dt( ) > 0.25f )
			m_fireman_data.side_valid = false;

		if ( m_fireman_data.fall_seen && g_prediction.backup_data.m_move_type != move_type_ladder ) {
			botox_dbg_log( "[fr] missed seen_drop=%.0f min_gap=%.1f last_gap=%.1f last_dist=%.0f last_h=%.0f win=%d side=%d ov=%.2f/%.2f press=%d air=%d turned=%.0f",
			                   m_fireman_data.fall_seen_drop, m_fireman_data.fall_min_gap, m_fireman_data.fall_last_gap, m_fireman_data.fall_last_dist,
			                   m_fireman_data.fall_last_h, m_fireman_data.fall_win_ticks, ( int )m_fireman_data.fall_side, m_fireman_data.fall_last_ov,
			                   m_fireman_data.side_ov, ( int )m_fireman_data.side_press, m_fireman_data.air_ticks, m_fireman_data.air_turned );
			// last 24 hunt ticks, oldest first: where the approach stalled / overshot (veer logs only every 16th)
			const int hist_n = std::min( m_fireman_data.fall_hist_n, 24 );
			for ( int i = m_fireman_data.fall_hist_n - hist_n; i < m_fireman_data.fall_hist_n; ++i ) {
				const auto& h = m_fireman_data.fall_hist[ i % 24 ];
				botox_dbg_log( "[fr] hist %d drop=%.0f gap=%.1f dist=%.1f h=%.0f ov=%.2f err=%.1f need=%.0f along=%.0f w=%d in=%.0f spin=%.1f", i - m_fireman_data.fall_hist_n,
				                   h.drop, h.gap, h.dist, h.h, h.ov, h.err, h.need, h.along, h.wrote, h.in, h.spin );
			}

			m_fireman_data.fall_seen = false;
		}

		// left ladder is freed by standing 0.25s, not a landing: the launch chain touches down 1 tick per hop
		if ( m_fireman_data.left_valid && ++m_fireman_data.left_ground_ticks * fireman_phys_dt( ) > 0.25f ) {
			m_fireman_data.left_valid = false;
			botox_dbg_log( "[fr] left clear" );
		}
	} else {
		// takeoff back toward the left ladder after our launch = new attempt (quick repeats never stood 0.25s). our hops go away
		if ( m_fireman_data.left_valid && m_fireman_data.left_ground_ticks > 0 && m_fireman_data.launch_ticks <= 0 &&
		     g_prediction.backup_data.m_velocity.m_x * m_fireman_data.left_normal.m_x +
		             g_prediction.backup_data.m_velocity.m_y * m_fireman_data.left_normal.m_y <
		         0.f ) {
			m_fireman_data.left_valid = false;
			botox_dbg_log( "[fr] left clear back h=%.0f", g_prediction.backup_data.m_velocity.length_2d( ) );
		}

		m_fireman_data.left_ground_ticks = 0;
		m_fireman_data.side_ground_ticks = 0;
	}

	// after the landing's `missed` dump read them
	if ( ( g_prediction.backup_data.m_flags & fl_onground ) != 0 ) {
		m_fireman_data.air_ticks  = 0;
		m_fireman_data.air_turned = 0.f;
	} else {
		++m_fireman_data.air_ticks;
		m_fireman_data.air_turned += view_spin;
	}

	if ( m_edge_jump_data.m_ladder_detected )
		m_fireman_data.player_trick = true;
	else if ( ( g_prediction.backup_data.m_flags & fl_onground ) != 0 && g_prediction.backup_data.m_move_type != move_type_ladder )
		m_fireman_data.player_trick = false;

	if ( GET_VARIABLE( g_variables.m_air_stuck, bool ) && g_input.check_input( &GET_VARIABLE( g_variables.m_air_stuck_key, key_bind_t ) ) )
		return;
	if ( g_wall_climb.caught( cmd ) || g_wall_climb.active( cmd ) )
		return;

	if ( m_fireman_data.was_ladder && m_fireman_data.engaged && g_prediction.backup_data.m_move_type != move_type_ladder ) {
		const c_vector& fling = g_prediction.backup_data.m_velocity;
		const float fling_h   = fling.length_2d( );

		if ( fling_h > 64.f ) {
			m_fireman_data.left_valid        = true;
			m_fireman_data.left_normal       = c_vector( fling.m_x / fling_h, fling.m_y / fling_h, 0.f );
			m_fireman_data.left_ground_ticks = 0;
			m_fireman_data.recall_valid = false;
			m_fireman_data.side_valid   = false;
			m_fireman_data.steer_ticks  = 0;

			constexpr float left_probe_max = 1024.f;

			float left_drop   = 0.f;
			const c_vector lp = m_fireman_data.ride_point;

			m_fireman_data.left_point = lp;
			m_fireman_data.left_point.m_z = lp.m_z - 8192.f;

			if ( auto left_col = g_ctx.m_local->get_collideable( ); left_col && m_fireman_data.yaw_valid ) {
				const c_vector lmins = left_col->get_obb_mins( );
				const c_vector lmaxs = left_col->get_obb_maxs( );

				trace_t floor_trace;
				c_trace_filter_trace_type_everything_filter_props floor_flt;
				ray_t floor_ray( lp, c_vector( lp.m_x, lp.m_y, lp.m_z - left_probe_max ), lmins, lmaxs );
				g_interfaces.m_engine_trace->trace_ray( floor_ray, mask_playersolid, &floor_flt, &floor_trace );

				const float band_lo   = lmins.m_z + ( lmaxs.m_z - lmins.m_z ) * 0.3f;
				const float band_hi   = lmins.m_z + ( lmaxs.m_z - lmins.m_z ) * 0.7f;
				const float probe_cap = floor_trace.m_fraction * left_probe_max + band_lo - 1.f;
				const bool has_floor  = floor_trace.m_fraction < 1.f;

				left_drop = fireman_ladder_drop( lp, lmins, lmaxs, m_fireman_data.ladder_yaw, lmaxs.m_x * 1.415f + 16.f, probe_cap );

				const bool capped = left_drop >= probe_cap - 0.01f;

				if ( left_drop > 0.f && ( !capped || has_floor ) )
					m_fireman_data.left_point.m_z = lp.m_z - left_drop + ( capped ? band_lo : band_hi );
			}

			botox_dbg_log( "[fr] left armed h=%.0f nyaw=%.0f below=%.0f", fling_h, m_fireman_data.left_normal.to_angle( ).m_y, left_drop );
		}
	}

	if ( !GET_VARIABLE( g_variables.m_fire_man, bool ) || !g_input.check_input( &GET_VARIABLE( g_variables.m_fire_man_key, key_bind_t ) ) ) {
		m_fireman_data.key_held      = false;
		m_fireman_data.key_on_ladder = false;
		m_fireman_data.fall_seen = false;
		m_fireman_data.key_ticks = 0;
		stand_down( );
		return;
	}

	++m_fireman_data.key_ticks;

	if ( !m_fireman_data.key_held ) {
		m_fireman_data.key_held      = true;
		m_fireman_data.key_on_ladder = g_prediction.backup_data.m_move_type == move_type_ladder;
	}

	if ( m_fireman_data.key_on_ladder ) {
		const bool clear = ( g_prediction.backup_data.m_flags & fl_onground ) != 0 && g_prediction.backup_data.m_move_type != move_type_ladder;

		if ( !clear ) {
			stand_down( );
			return;
		}

		m_fireman_data.key_on_ladder = false;
	}

	// ladder edge jump: the player jumped off their own ladder; that airtime is theirs (the hunt / lock / latch guard
	// would steer them back onto the ladder they just left). never inside our own ride / landing jump
	static bool trick_logged = false;

	if ( m_fireman_data.player_trick && !( m_fireman_data.was_ladder && m_fireman_data.engaged ) && m_fireman_data.launch_ticks <= 0 ) {
		if ( !trick_logged )
			botox_dbg_log( "[fr] yield lej vz=%.0f h=%.0f", g_prediction.backup_data.m_velocity.m_z, g_prediction.backup_data.m_velocity.length_2d( ) );
		trick_logged = true;

		m_fireman_data.fall_seen = false;
		stand_down( );
		return;
	}
	trick_logged = false;

	const bool approach   = GET_VARIABLE( g_variables.m_fire_man_approach, bool );
	const bool drop_in_on = GET_VARIABLE( g_variables.m_fire_man_drop_in, bool );

	const bool user_steering = m_user_forward_move_raw != 0.f || m_user_side_move_raw != 0.f || ( m_user_buttons_raw & move_buttons ) != 0;
	const bool grounded_now  = ( g_prediction.backup_data.m_flags & fl_onground ) != 0;
	const bool airborne_now  = !grounded_now && g_prediction.backup_data.m_move_type == move_type_walk;

	const bool lock_armed = drop_in_on && GET_VARIABLE( g_variables.m_fire_man_drop_in_lock, bool );

	if ( !lock_armed || grounded_now ) {
		m_fireman_data.drop_in_lock    = false;
		m_fireman_data.lock_lost_ticks = 0;
		m_fireman_data.lock_ticks      = 0;
	}

	if ( grounded_now || !approach ) {
		m_fireman_data.ladder_lock      = false;
		m_fireman_data.ladder_lost_ticks = 0;
	}

	constexpr int lock_idle_limit = 8;
	constexpr int lock_recheck    = 64;

	if ( m_fireman_data.lock_cooldown > 0 )
		--m_fireman_data.lock_cooldown;

	if ( ( m_fireman_data.drop_in_lock || m_fireman_data.ladder_lock ) && m_fireman_data.lock_idle_ticks > lock_idle_limit ) {
		m_fireman_data.drop_in_lock  = false;
		m_fireman_data.ladder_lock   = false;
		m_fireman_data.lock_cooldown = lock_recheck;
		botox_dbg_log( "[fr] lock idle release" );
	}

	if ( m_fireman_data.drop_in_lock || m_fireman_data.ladder_lock )
		++m_fireman_data.lock_idle_ticks;

	bool locked = m_fireman_data.drop_in_lock || m_fireman_data.ladder_lock;

	const bool probe_only = user_steering && !locked && airborne_now;

	const bool on_ladder_now = g_prediction.backup_data.m_move_type == move_type_ladder;
	const int ride_grab_age  = cmd->m_tick_count - m_fireman_data.grab_tick;
	const bool ours_ladder   = m_fireman_data.was_ladder
	                             ? m_fireman_data.engaged
	                             : ( m_fireman_data.fell_ready && ride_grab_age >= 0 && ride_grab_age <= 2 );
	const bool riding_now    = on_ladder_now && ours_ladder;

	if ( on_ladder_now && !ours_ladder ) {
		m_fireman_data.drop_in_lock      = false;
		m_fireman_data.ladder_lock       = false;
		m_fireman_data.lock_lost_ticks   = 0;
		m_fireman_data.ladder_lost_ticks = 0;
		locked                           = false;
	}

	/* grounded with a key: stand down, unless our exit / landing jump is still owed (keys don't cancel the one jump) */
	if ( user_steering && !locked && !probe_only && !riding_now && !m_fireman_data.was_ladder && m_fireman_data.launch_ticks <= 0 ) {
		stand_down( );
		return;
	}

	if ( locked || riding_now ) {
		cmd->m_forward_move = 0.f;
		cmd->m_side_move    = 0.f;
		cmd->m_up_move      = 0.f;
		const bool keep_lj_duck = !riding_now && m_long_jump_data.m_ducking;
		cmd->m_buttons &= ~( move_buttons | ( keep_lj_duck ? 0 : in_duck ) );
		m_fireman_data.owns_cmd = true;

		if ( locked && ++m_fireman_data.lock_ticks > 512 ) {
			m_fireman_data.drop_in_lock = false;
			m_fireman_data.ladder_lock  = false;
			botox_dbg_log( "[fr] lock expired" );
		}
	}

	auto colidable = g_ctx.m_local->get_collideable( );
	if ( !colidable )
		return;

	const c_vector mins = colidable->get_obb_mins( );
	const c_vector maxs = colidable->get_obb_maxs( );
	const c_vector origin = g_prediction.backup_data.m_origin;

	// log only: last tick's press said latch, engine still WALK = our test looser than OnLadder, or a later writer changed the cmd
	if ( m_fireman_data.press_tick != -1000 && cmd->m_tick_count != m_fireman_data.press_tick ) {
		if ( cmd->m_tick_count - m_fireman_data.press_tick == 1 && g_prediction.backup_data.m_move_type != move_type_ladder ) {
			char at_press[ 128 ], at_now[ 128 ];
			fireman_latch_probe( m_fireman_data.press_origin, mins, maxs, m_fireman_data.press_yaw, at_press, sizeof( at_press ) );
			fireman_latch_probe( origin, mins, maxs, m_fireman_data.press_yaw, at_now, sizeof( at_now ) );
			const c_vector moved = origin - m_fireman_data.press_origin;
			botox_dbg_log( "[fr] nolatch by=%s yaw=%.0f moved=%.2f/%.2f/%.2f press{%s} now{%s}", m_fireman_data.press_by, m_fireman_data.press_yaw, moved.m_x,
			               moved.m_y, moved.m_z, at_press, at_now );
		}
		m_fireman_data.press_tick = -1000;
	}

	const auto drop_to_floor = [ & ]( ) -> float {
		c_vector from = origin;
		c_vector dir( 0.f, 0.f, -1.f );
		float left = 8192.f;

		for ( int bump = 0; bump < 3; ++bump ) {
			trace_t floor_trace;
			c_trace_filter_trace_type_everything_filter_props floor_flt;
			ray_t floor_ray( from, from + dir * left, mins, maxs );
			g_interfaces.m_engine_trace->trace_ray( floor_ray, mask_playersolid, &floor_flt, &floor_trace );

			if ( floor_trace.m_fraction >= 1.f )
				return -1.f;

			left -= floor_trace.m_fraction * left;
			from  = floor_trace.m_end;

			const c_vector& n = floor_trace.m_plane.m_normal;
			if ( floor_trace.m_start_solid || n.m_z >= 0.7f || n.m_z <= 0.f || !botox_is_ladder_trace( floor_trace ) )
				break;

			dir = c_vector( n.m_x * n.m_z, n.m_y * n.m_z, n.m_z * n.m_z - 1.f ).normalized( );
		}

		return origin.m_z - from.m_z;
	};

	const int move_type = g_prediction.backup_data.m_move_type;

	const unsigned int sims_before = g_prediction.m_sim_count;
	const auto sim_settle          = fireman_scope_exit_t{ [ & ]( ) {
		if ( g_prediction.m_sim_count == sims_before )
			return;

		c_user_cmd final_cmd = *cmd;
		g_prediction.restore_entity_to_predicted_frame( g_interfaces.m_prediction->m_commands_predicted - 1 );
		g_prediction.begin( g_ctx.m_local, &final_cmd );
		g_prediction.end( g_ctx.m_local );
	} };

	const auto latch_guard = fireman_scope_exit_t{ [ & ]( ) {
		if ( move_type != move_type_walk || grounded_now || m_fireman_data.owns_cmd || m_fireman_data.is_ladder )
			return;

		const float fwd  = cmd->m_forward_move;
		const float side = cmd->m_side_move;
		if ( std::fabsf( fwd ) < 1.f && std::fabsf( side ) < 1.f )
			return;

		const float wish_yaw = cmd->m_view_point.m_y + atan2f( -side, fwd ) * ( 180.f / std::numbers::pi_v< float > );

		c_vector guard_normal;
		if ( !fireman_can_latch( origin, mins, maxs, wish_yaw, guard_normal ) )
			return;

		// the ladder just left: never. others only while falling (catch window is ours; a rising jump onto a ladder to
		// climb it stays the player's, style 0 binds are always on)
		const bool left_face = fireman_latch_is_left( origin, maxs, guard_normal, m_fireman_data.left_valid, m_fireman_data.left_point,
		                                              m_fireman_data.left_normal );
		if ( !left_face && g_prediction.backup_data.m_velocity.m_z >= 0.f )
			return;

		cmd->m_forward_move = 0.f;
		cmd->m_side_move    = 0.f;
		cmd->m_buttons &= ~move_buttons;

		static int guard_log = 0;
		if ( ( guard_log++ % 16 ) == 0 )
			botox_dbg_log( "[fr] guard yaw=%.0f fwd=%.0f side=%.0f vz=%.0f launch=%d", wish_yaw, fwd, side, g_prediction.backup_data.m_velocity.m_z,
			                   m_fireman_data.launch_ticks );
	} };

	if ( move_type == move_type_ladder ) {
		lock_earned( );
		m_fireman_data.fall_seen = false;

		if ( !m_fireman_data.was_ladder ) {
			const int grab_age = cmd->m_tick_count - m_fireman_data.grab_tick;
			const bool ours    = m_fireman_data.fell_ready && grab_age >= 0 && grab_age <= 2;

			m_fireman_data.engaged    = ours;
			m_fireman_data.ride_ticks = 0;

			if ( !ours )
				botox_dbg_log( "[fr] alien vz=%.0f h=%.0f ready=%d age=%d", g_prediction.backup_data.m_velocity.m_z,
				                   g_prediction.backup_data.m_velocity.length_2d( ), ( int )m_fireman_data.fell_ready, grab_age );
		}

		m_fireman_data.was_ladder = true;

		if ( !m_fireman_data.engaged ) {
			if ( m_fireman_data.fell_ready && ( g_prediction.backup_data.m_flags & fl_onground ) == 0 ) {
				m_fireman_data.engaged = true;
				botox_dbg_log( "[fr] adopt vz=%.0f drop=%.0f", g_prediction.backup_data.m_velocity.m_z, drop_to_floor( ) );
			} else {
				if ( m_fireman_data.launch_ticks > 0 && ( g_prediction.backup_data.m_flags & fl_onground ) == 0 ) {
					cmd->m_forward_move = 0.f;
					cmd->m_side_move    = 0.f;
					cmd->m_up_move      = 0.f;
					cmd->m_buttons &= ~( move_buttons | in_duck );
					cmd->m_buttons |= in_jump;

					static int alien_log = 0;
					if ( ( alien_log++ % 8 ) == 0 )
						botox_dbg_log( "[fr] alien escape vz=%.0f drop=%.0f", g_prediction.backup_data.m_velocity.m_z, drop_to_floor( ) );
				}

				return;
			}
		}

		m_fireman_data.is_ladder    = true;
		m_fireman_data.fr_hit       = true;
		m_fireman_data.owns_cmd     = true;
		m_fireman_data.launch_ticks = 0;

		const c_vector ladder_normal = g_ctx.m_local->get_ladder_normal( );

		if ( ( ladder_normal.m_x != 0.f || ladder_normal.m_y != 0.f ) && ladder_normal.m_z > -0.3f && ladder_normal.m_z < fireman_slant_nz_max ) {
			m_fireman_data.ladder_yaw = c_vector( -ladder_normal.m_x, -ladder_normal.m_y, 0.f ).to_angle( ).m_y;
			m_fireman_data.yaw_valid  = true;
		}

		m_fireman_data.ride_point = origin;

		const float ride_drop = drop_to_floor( );

		if ( m_fireman_data.ride_ticks == 0 || std::fabsf( origin.m_z - m_fireman_data.ride_last_z ) > 1.f ) {
			m_fireman_data.ride_last_z = origin.m_z;
			m_fireman_data.ride_stall  = 0;
		} else if ( ++m_fireman_data.ride_stall == 8 )
			botox_dbg_log( "[fr] stuck t=%d vz=%.0f drop=%.0f z=%.1f", m_fireman_data.ride_ticks,
			                   g_prediction.backup_data.m_velocity.m_z, ride_drop, origin.m_z );

		const bool stalled = m_fireman_data.ride_stall >= 8;

		/* zero normal = flush latch, 270 * 0 = no fling. Log only. >= 2: networked, first tick may read stale zero */
		const bool dead_normal = m_fireman_data.ride_ticks >= 2 && std::fabsf( ladder_normal.m_x ) < 0.01f &&
		                         std::fabsf( ladder_normal.m_y ) < 0.01f;

		if ( dead_normal && m_fireman_data.ride_ticks == 2 )
			botox_dbg_log( "[fr] dead normal drop=%.0f vz=%.0f", ride_drop, g_prediction.backup_data.m_velocity.m_z );

		const int cooldown_ticks = fireman_catch_ticks( );

		if ( m_fireman_data.ride_ticks == 0 ) {
			m_fireman_data.ride_released    = false;
			m_fireman_data.ride_push        = 0;
			m_fireman_data.ride_floor_press = false;
			m_fireman_data.ride_descend     = false;
			m_fireman_data.ride_desc_ticks  = 0;
			m_fireman_data.ride_wait_ticks  = 0;
			m_fireman_data.ride_hover_ticks = 0;
		}

		cmd->m_forward_move = 0.f;
		cmd->m_side_move    = 0.f;
		cmd->m_up_move      = 0.f;
		cmd->m_buttons &= ~( move_buttons | in_duck );

		const int ride_base    = cmd->m_buttons & ~in_jump;
		const bool crouch_jump = GET_VARIABLE( g_variables.m_fire_man_crouch_jump, bool );

		const bool trace_grounded = ( g_prediction.backup_data.m_flags & fl_onground ) != 0 ||
		                            ( ride_drop >= 0.f && ride_drop < fireman_ride_floor_probe );
		constexpr float ground_sim_reach = 32.f; // > one tick of a 900 u/s frozen ride at 64 + one stale cmd

		bool ride_grounded = trace_grounded;

		if ( trace_grounded || ( ride_drop >= 0.f && ride_drop < ground_sim_reach ) ) {
			const fireman_sim_t still = fireman_sim( cmd, ride_base );

			if ( still.valid )
				ride_grounded = still.ground && still.z >= still.z_in - 0.5f;
		}

		const bool cooldown_done = m_fireman_data.ride_ticks >= cooldown_ticks;

		constexpr float push_speed    = 200.f;
		constexpr float push_detach   = 10.f;
		const int detach_ticks        = static_cast< int >( std::ceilf( push_detach / ( push_speed * fireman_phys_dt( ) ) ) );
		const int push_room           = cooldown_ticks - 2 - m_fireman_data.ride_ticks - detach_ticks;
		const float push_out          = g_prediction.backup_data.m_velocity.m_x * ladder_normal.m_x + g_prediction.backup_data.m_velocity.m_y * ladder_normal.m_y;
		// pressed once = never push again (a weak jump rising < 2u reads grounded; IN_BACK airborne means -156)
		const bool push_press = ride_grounded && m_fireman_data.ride_push == 1 && m_fireman_data.ride_released && push_out >= 100.f && push_room >= 0;
		const bool floor_detach = ride_grounded && !cooldown_done && ladder_normal.length_squared( ) > 0.25f &&
		                          !fireman_ride_sticks( origin, mins, maxs, ladder_normal );
		const auto push_keeps_floor = [ & ]( ) -> bool {
			const float n_len = sqrtf( ladder_normal.m_x * ladder_normal.m_x + ladder_normal.m_y * ladder_normal.m_y );
			if ( n_len < 0.5f )
				return false;
			const float out    = push_speed * fireman_phys_dt( ) + 1.f;
			const c_vector from( origin.m_x + ladder_normal.m_x / n_len * out, origin.m_y + ladder_normal.m_y / n_len * out, origin.m_z );
			trace_t t;
			c_trace_filter_trace_type_everything_filter_props flt;
			ray_t r( from, c_vector( from.m_x, from.m_y, from.m_z - std::max( ride_drop, 0.f ) - fireman_ride_floor_probe ), mins, maxs );
			g_interfaces.m_engine_trace->trace_ray( r, mask_playersolid, &flt, &t );
			return !t.m_start_solid && t.m_fraction < 1.f && t.m_plane.m_normal.m_z >= 0.7f;
		};
		const bool push_now = ride_grounded && !push_press && m_fireman_data.ride_push != 2 && !cooldown_done && m_fireman_data.yaw_valid &&
		                      ( floor_detach || ( GET_VARIABLE( g_variables.m_fire_man_push_off, bool ) && push_room >= 1 ) ) && push_keeps_floor( );

		bool floor_launch = false;
		int press_duck    = crouch_jump ? in_duck : 0;
		float press_apex  = -FLT_MAX;

		if ( ride_grounded && !push_press && !push_now && m_fireman_data.ride_released ) {
			const fireman_sim_t plain = fireman_sim( cmd, ride_base | in_jump );

			/* no sim = the count; 2 ticks past the count (it only over-estimates) = press blind, a clipped jump
			   (low ceiling) must not park us on the ladder forever */
			if ( !plain.valid || m_fireman_data.ride_ticks >= cooldown_ticks + 2 )
				floor_launch = cooldown_done;
			else {
				press_apex   = fireman_launch_apex( plain, crouch_jump );
				press_duck   = 0;
				floor_launch = press_apex > -FLT_MAX;

				if ( crouch_jump ) {
					const float ducked_apex = fireman_launch_apex( fireman_sim( cmd, ride_base | in_jump | in_duck ), true );

					if ( ducked_apex > -FLT_MAX && ducked_apex >= press_apex ) {
						press_apex   = ducked_apex;
						press_duck   = in_duck;
						floor_launch = true;
					}
				}
			}
		}

		const bool hold_press = ride_grounded && !push_press && !floor_launch;

		bool end_hang = !ride_grounded && !cooldown_done && m_fireman_data.ride_push == 0 && ladder_normal.length_squared( ) > 0.25f &&
		                !fireman_ride_next_sticks( origin, mins, maxs, g_prediction.backup_data.m_velocity, ladder_normal );

		constexpr float descend_speed = 156.f;
		const float phys_dt_ride = fireman_phys_dt( );
		const float face_max     = descend_speed * 1.41421356f;
		const float fast_vz      = -descend_speed * ( 1.f + sinf( 89.f * ( std::numbers::pi_v< float > / 180.f ) ) );
		float descend_vz         = -descend_speed;
		float descend_pitch      = 0.f;
		bool descend_fast        = false;

		if ( ride_drop > fireman_ride_floor_probe && std::fabsf( ladder_normal.m_z ) < 0.05f ) {
			const float room = ride_drop - 1.f;

			if ( -fast_vz * phys_dt_ride <= room ) {
				descend_vz   = fast_vz;
				descend_fast = true;
			} else {
				descend_vz    = -std::min( room / phys_dt_ride, face_max );
				descend_pitch = 45.f + asinf( std::clamp( descend_vz / face_max, -1.f, 1.f ) ) * ( 180.f / std::numbers::pi_v< float > );
			}
		}

		c_vector descend_step( 0.f, 0.f, descend_vz );

		/* slant: the stick trace runs along -normal, i.e. DOWN, so at floor+0 it hits the floor = :5075 WALK, no 270
		   (log 10-07 15:49 nz=0.31: pitch-0 step -188 landed on the floor, exit h=55). size by pitch to stop 1u over it */
		if ( std::fabsf( ladder_normal.m_z ) >= 0.05f && ladder_normal.length_squared( ) > 0.25f && m_fireman_data.yaw_valid &&
		     ( ride_drop < 0.f || ride_drop > fireman_ride_floor_probe ) ) {
			const float max_down = ride_drop < 0.f ? FLT_MAX : ( ride_drop - 1.f ) / phys_dt_ride;
			bool found           = false;

			for ( float p = -89.f; p <= 89.f; p += 0.5f ) {
				const c_vector v = fireman_ladder_back_velocity( ladder_normal, p, m_fireman_data.ladder_yaw );

				if ( v.m_z < 0.f && -v.m_z <= max_down && ( !found || v.m_z < descend_step.m_z ) ) {
					descend_step  = v;
					descend_pitch = p;
					found         = true;
				}
			}

			if ( found )
				descend_vz = descend_step.m_z;
		}

		if ( end_hang && m_fireman_data.yaw_valid && ride_drop > fireman_ride_floor_probe &&
		     ride_drop <= std::max( -g_prediction.backup_data.m_velocity.m_z, 0.f ) * fireman_phys_dt( ) + fireman_ride_floor_probe &&
		     fireman_ride_next_sticks( origin, mins, maxs, descend_step, ladder_normal ) ) {
			end_hang                    = false;
			m_fireman_data.ride_descend = true;
			botox_dbg_log( "[fr] floor end t=%d drop=%.1f vz=%.0f step=%.0f p=%.1f nz=%.2f", m_fireman_data.ride_ticks, ride_drop, g_prediction.backup_data.m_velocity.m_z,
			                   descend_step.m_z, descend_pitch, ladder_normal.m_z );
		}

		/* the frozen DRIFT ends the ride, not the ladder (log 10-05: h=16 off a 0.3u side at drop 34, then hung 7 ticks at 0 u/s
		   for the cooldown). straight down still sticks to the floor, or past the cooldown = climb down now: a jumpless move
		   replaces the drift, and the end jump comes from the floor press / the descend press after expiry */
		if ( end_hang && m_fireman_data.yaw_valid && ride_drop > fireman_ride_floor_probe ) {
			const float down_len = -descend_vz * static_cast< float >( cooldown_ticks + 2 ) * phys_dt_ride + 4.f;

			trace_t down;
			c_trace_filter down_flt( g_ctx.m_local );
			ray_t down_ray( origin, c_vector( origin.m_x, origin.m_y, origin.m_z - down_len ), mins, maxs );
			g_interfaces.m_engine_trace->trace_ray( down_ray, mask_playersolid, &down_flt, &down );

			const bool down_floor = down.m_fraction < 1.f && down.m_plane.m_normal.m_z >= 0.7f;
			// 1u over a floor hit: a prop step skin fails the stick ON it, holds over it (de_train cars)
			const c_vector down_end( down.m_end.m_x, down.m_end.m_y, down.m_end.m_z + ( down_floor ? 1.f : 0.f ) );
			const c_vector down_mid( origin.m_x, origin.m_y, ( origin.m_z + down_end.m_z ) * 0.5f );

			if ( !down.m_start_solid && ( down_floor || down.m_fraction >= 1.f ) && fireman_ride_sticks( down_end, mins, maxs, ladder_normal ) &&
			     fireman_ride_sticks( down_mid, mins, maxs, ladder_normal ) && fireman_ride_next_sticks( origin, mins, maxs, descend_step, ladder_normal ) ) {
				const float n_h = sqrtf( ladder_normal.m_x * ladder_normal.m_x + ladder_normal.m_y * ladder_normal.m_y );
				const c_vector& v_now = g_prediction.backup_data.m_velocity;

				end_hang                    = false;
				m_fireman_data.ride_descend = true;
				botox_dbg_log( "[fr] drift end t=%d drop=%.1f vz=%.0f floor=%d du=%.1f dn=%.1f", m_fireman_data.ride_ticks, ride_drop, v_now.m_z, ( int )down_floor,
				                   n_h > 0.01f ? ( -v_now.m_x * ladder_normal.m_y + v_now.m_y * ladder_normal.m_x ) / n_h : 0.f,
				                   n_h > 0.01f ? ( v_now.m_x * ladder_normal.m_x + v_now.m_y * ladder_normal.m_y ) / n_h : 0.f );
			}
		}

		const bool normal_flings = ladder_normal.m_x * ladder_normal.m_x + ladder_normal.m_y * ladder_normal.m_y > 0.25f;
		bool sim_release         = false;

		const float fling_ok = std::max( -g_prediction.backup_data.m_velocity.m_z, 0.f ) * fireman_phys_dt( ) * 4.f + 16.f;

		if ( !ride_grounded && !end_hang && m_fireman_data.ride_push == 0 && normal_flings && !m_fireman_data.ride_descend ) {
			const fireman_sim_t hold = fireman_sim( cmd, ride_base | in_jump );

			sim_release = hold.valid && ( ( hold.ladder && hold.vz > 30.f ) || ( !hold.ladder && hold.h < 64.f ) );

			if ( sim_release )
				botox_dbg_log( "[fr] hold refused t=%d ladder=%d vz=%.0f h=%.0f drop=%.0f", m_fireman_data.ride_ticks, ( int )hold.ladder, hold.vz, hold.h, ride_drop );

			const bool flings_now = hold.valid ? !hold.ladder && hold.h >= 64.f : cooldown_done;

			if ( flings_now && m_fireman_data.yaw_valid && ( ride_drop < 0.f || ride_drop > fling_ok ) &&
			     fireman_ride_next_sticks( origin, mins, maxs, descend_step, ladder_normal ) ) {
				m_fireman_data.ride_descend = true;
				botox_dbg_log( "[fr] descend t=%d drop=%.0f vz=%.0f ok=%.0f step=%.0f fast=%d", m_fireman_data.ride_ticks, ride_drop, g_prediction.backup_data.m_velocity.m_z,
				                   fling_ok, descend_vz, ( int )descend_fast );
			}
			else if ( flings_now && m_fireman_data.yaw_valid && ride_drop > fireman_ride_floor_probe && ride_drop <= fling_ok &&
			          fireman_ride_next_sticks( origin, mins, maxs, descend_step, ladder_normal ) ) {
				// :3713 fling is z 0: free fall from rest, then the landing press. climbing down + floor press jumps sooner
				const float descend_rate = std::fabsf( ladder_normal.m_z ) < 0.05f ? -fast_vz : descend_speed;
				const float fall_time    = sqrtf( 2.f * ride_drop / std::max( fireman_gravity( ), 1.f ) );
				bool falls_off           = ride_drop / descend_rate + phys_dt_ride <= fall_time;

				if ( !falls_off ) {
					const float n_len = sqrtf( ladder_normal.m_x * ladder_normal.m_x + ladder_normal.m_y * ladder_normal.m_y );
					const float out   = 270.f * sqrtf( 2.f * ride_drop / std::max( fireman_gravity( ), 1.f ) ) + 1.f;
					const c_vector at( origin.m_x + ladder_normal.m_x / n_len * out, origin.m_y + ladder_normal.m_y / n_len * out, origin.m_z );
					trace_t t;
					c_trace_filter_trace_type_everything_filter_props flt;
					ray_t r( at, c_vector( at.m_x, at.m_y, at.m_z - ride_drop - 18.f ), mins, maxs );
					g_interfaces.m_engine_trace->trace_ray( r, mask_playersolid, &flt, &t );
					falls_off = !t.m_start_solid && t.m_fraction >= 1.f;
				}

				if ( falls_off ) {
					m_fireman_data.ride_descend = true;
					botox_dbg_log( "[fr] floor end expiry t=%d drop=%.1f vz=%.0f", m_fireman_data.ride_ticks, ride_drop, g_prediction.backup_data.m_velocity.m_z );
				}
			}
		}

		bool descend_press = false;
		bool descend_wait  = false;

		if ( m_fireman_data.ride_descend && !ride_grounded ) {
			descend_press = !fireman_ride_next_sticks( origin, mins, maxs, descend_step, ladder_normal );
			// jump inside the cooldown is a no-op (:3708): we'd slide off the end with no 270 (log 10-05 08:41 flung=0).
			// hang (no keys = :3785 Init, 0 u/s, stays put) and press on expiry; a press a tick early just holds still
			descend_wait = descend_press && !cooldown_done;

			if ( descend_press )
				botox_dbg_log( "[fr] descend press t=%d drop=%.0f wait=%d", m_fireman_data.ride_ticks, ride_drop, ( int )descend_wait );
		}

		const bool descending = m_fireman_data.ride_descend && !ride_grounded && !descend_press;

		const bool release = hold_press || end_hang || sim_release || descending || descend_wait;

		if ( end_hang )
			botox_dbg_log( "[fr] end t=%d z=%.1f vz=%.0f drop=%.0f nz=%.2f h=%.0f", m_fireman_data.ride_ticks, origin.m_z,
			                   g_prediction.backup_data.m_velocity.m_z, ride_drop, ladder_normal.m_z, g_prediction.backup_data.m_velocity.length_2d( ) );

		if ( floor_launch )
			botox_dbg_log( "[fr] floor press t=%d apex=%.1f duck=%d cd=%d", m_fireman_data.ride_ticks, press_apex - origin.m_z, ( int )( press_duck != 0 ),
			                   cooldown_ticks - m_fireman_data.ride_ticks );

		if ( m_fireman_data.ride_ticks < 2 || ( m_fireman_data.ride_ticks % 8 ) == 0 || hold_press || end_hang )
			botox_dbg_log( "[fr] ride t=%d vz=%.0f h=%.0f drop=%.0f ground=%d/%d nz=%.2f nyaw=%.0f wait=%d stall=%d dead=%d push=%d room=%d", m_fireman_data.ride_ticks,
			                   g_prediction.backup_data.m_velocity.m_z, g_prediction.backup_data.m_velocity.length_2d( ), ride_drop,
			                   ( int )ride_grounded, ( int )trace_grounded, ladder_normal.m_z, c_vector( -ladder_normal.m_x, -ladder_normal.m_y, 0.f ).to_angle( ).m_y,
			                   ( int )hold_press, ( int )stalled, ( int )dead_normal, push_now ? 1 : push_press ? 2 : 0, push_room );

		++m_fireman_data.ride_ticks;

		if ( release )
			cmd->m_buttons &= ~in_jump;
		else
			cmd->m_buttons |= in_jump;

		if ( floor_launch || ( push_press && crouch_jump ) )
			cmd->m_buttons |= press_duck;

		const c_angle live_view = g_ctx.old_view_point;

		// IN_BACK at (p, y) == IN_FORWARD at (-p, y + 180) exactly (-forward). send the one nearer the player's view: no 180 snap on GOTV
		const auto write_back = [ & ]( const float pitch, const float yaw ) {
			const float yaw_f = g_math.normalize_angle( yaw + 180.f );
			const bool fwd    = std::fabsf( g_math.normalize_angle( live_view.m_y - yaw_f ) ) + std::fabsf( live_view.m_x + pitch ) <
			                 std::fabsf( g_math.normalize_angle( live_view.m_y - yaw ) ) + std::fabsf( live_view.m_x - pitch );
			cmd->m_view_point = fwd ? c_angle( -pitch, yaw_f, 0.f ) : c_angle( pitch, yaw, 0.f );
			cmd->m_buttons |= fwd ? in_forward : in_back;
		};

		if ( push_now ) {
			write_back( 0.f, m_fireman_data.ladder_yaw );
			m_fireman_data.ride_push = 1;
		} else if ( push_press )
			m_fireman_data.ride_push = 2;

		if ( descending ) {
			if ( descend_fast ) {
				// static view (old per-tick +-90 flip = 180 deg yaw jitter on GOTV): yaw 90 - 89 pitch off the face cancels the drift
				// (4e-4 u/s), vz -312. 4 mirrors, nearest the player's view
				const bool down  = live_view.m_x >= 0.f;
				const float off  = down ? 91.f : 89.f;
				const float yaw_l = g_math.normalize_angle( m_fireman_data.ladder_yaw + off );
				const float yaw_r = g_math.normalize_angle( m_fireman_data.ladder_yaw - off );
				const bool left   = std::fabsf( g_math.normalize_angle( live_view.m_y - yaw_l ) ) <=
				                  std::fabsf( g_math.normalize_angle( live_view.m_y - yaw_r ) );
				cmd->m_view_point = c_angle( down ? 89.f : -89.f, left ? yaw_l : yaw_r, 0.f );
				cmd->m_buttons |= ( down ? in_forward : in_back ) | ( left ? in_moveleft : in_moveright );
			} else
				write_back( descend_pitch, m_fireman_data.ladder_yaw );

			++m_fireman_data.ride_desc_ticks;
		}

		if ( hold_press )
			++m_fireman_data.ride_wait_ticks;
		else if ( !ride_grounded && std::fabsf( g_prediction.backup_data.m_velocity.m_z ) < 1.f )
			++m_fireman_data.ride_hover_ticks;

		m_fireman_data.ride_floor_press = m_fireman_data.ride_floor_press || ( ride_grounded && !hold_press );
		m_fireman_data.ride_released    = release;

		return;
	}

	m_fireman_data.fr_hit = false;

	const bool on_ground = ( g_prediction.backup_data.m_flags & fl_onground ) != 0;
	const bool cliff_risk = fireman_cliff_catch_risk( origin, mins, maxs, g_prediction.backup_data.m_velocity, on_ground );

	struct release_jump_t {
		c_user_cmd* cmd = nullptr;
		~release_jump_t( )
		{
			if ( cmd )
				cmd->m_buttons &= ~in_jump;
		}
	} landing_edge;

	if ( m_fireman_data.was_ladder ) {
		const c_vector& exit_velocity = g_prediction.backup_data.m_velocity;
		const float exit_speed = sqrtf( exit_velocity.m_x * exit_velocity.m_x + exit_velocity.m_y * exit_velocity.m_y );

		m_fireman_data.launch_flung = exit_speed > 64.f;

		m_fireman_data.launch_ticks  = m_fireman_data.engaged ? launch_window : 0;
		m_fireman_data.launch_jumped  = m_fireman_data.ride_floor_press && !on_ground;
		m_fireman_data.launch_pressed = false;
		m_fireman_data.launch_refused = 0;
		m_fireman_data.backing_out   = false;
		m_fireman_data.was_ladder    = false;
		m_fireman_data.engaged       = false;
		m_fireman_data.yaw_valid     = false;

		botox_dbg_log( "[fr] exit h=%.0f vz=%.0f flung=%d ground=%d drop=%.0f rode=%d desc=%d wait=%d hover=%d", exit_speed, exit_velocity.m_z,
		                   ( int )m_fireman_data.launch_flung, ( int )on_ground, drop_to_floor( ), m_fireman_data.ride_ticks,
		                   m_fireman_data.ride_desc_ticks, m_fireman_data.ride_wait_ticks, m_fireman_data.ride_hover_ticks );

		m_fireman_data.ride_ticks = 0;
		m_fireman_data.ride_stall = 0;
	}

	if ( m_fireman_data.launch_ticks > 0 ) {
		const bool fling_tick = m_fireman_data.launch_ticks == launch_window && !m_fireman_data.ride_released;
		--m_fireman_data.launch_ticks;

		if ( !on_ground && ( g_prediction.backup_data.m_velocity.m_z > 150.f || m_fireman_data.launch_pressed ) )
			m_fireman_data.launch_jumped = true;

		const bool rehunt = !m_fireman_data.launch_flung && !m_fireman_data.launch_jumped && !on_ground &&
		                    g_prediction.backup_data.m_velocity.m_z < -1.f && [ & ]( ) {
			                    const float left = drop_to_floor( );
			                    return left < 0.f || left > 96.f;
		                    }( );

		const bool crouch     = GET_VARIABLE( g_variables.m_fire_man_crouch_jump, bool );
		const bool ducked_now = ( g_prediction.backup_data.m_flags & fl_ducking ) != 0;
		const int jump_base   = cmd->m_buttons & ~( in_jump | in_duck );
		/* this tick sends a predicted launch press (the chain's landing edge must not strip it) */
		bool land_press = false;

		if ( !fling_tick && on_ground && m_fireman_data.launch_jumped ) {
			botox_dbg_log( "[fr] win done t=%d h=%.0f", launch_window - m_fireman_data.launch_ticks, g_prediction.backup_data.m_velocity.length_2d( ) );
			m_fireman_data.launch_ticks = 0;
		} else if ( !fling_tick && on_ground && m_fireman_data.launch_pressed ) {
			cmd->m_buttons &= ~in_jump;
			m_fireman_data.launch_pressed = false;
		} else if ( !fling_tick && on_ground ) {
			const int own_duck = crouch ? ( ducked_now ? 0 : in_duck ) : ( ducked_now ? in_duck : ( cmd->m_buttons & in_duck ) );

			int press                 = jump_base | in_jump | own_duck;
			const fireman_sim_t first = fireman_sim( cmd, press );
			float apex                = fireman_launch_apex( first, crouch );

			if ( crouch ) {
				const int other        = jump_base | in_jump | ( own_duck ? 0 : in_duck );
				const float other_apex = fireman_launch_apex( fireman_sim( cmd, other ), true );

				if ( other_apex > apex ) {
					apex  = other_apex;
					press = other;
				}
			}

			if ( !first.valid || apex > -FLT_MAX || m_fireman_data.launch_refused >= 2 ) {
				cmd->m_buttons                = ( cmd->m_buttons & ~( in_jump | in_duck ) ) | press;
				m_fireman_data.launch_pressed = true;
				m_fireman_data.launch_refused = 0;
				land_press                    = true;
			} else {
				cmd->m_buttons &= ~in_jump;
				++m_fireman_data.launch_refused;
			}

			botox_dbg_log( "[fr] press t=%d h=%.0f vz=%.0f ducked=%d sent=%d apex=%.1f duck=%d", launch_window - m_fireman_data.launch_ticks,
			                   g_prediction.backup_data.m_velocity.length_2d( ), g_prediction.backup_data.m_velocity.m_z, ( int )ducked_now,
			                   ( int )land_press, apex > -FLT_MAX ? apex - origin.m_z : -1.f, ( int )( ( press & in_duck ) != 0 ) );
		} else {
			cmd->m_buttons &= ~in_jump;
			m_fireman_data.launch_pressed = false;

			if ( !on_ground && !rehunt ) {
				const float vz = g_prediction.backup_data.m_velocity.m_z;

				const bool owes_jump = !m_fireman_data.launch_jumped;
				bool planned         = false;

				const float land_drop = owes_jump && !fling_tick && vz <= 0.f ? drop_to_floor( ) : -1.f;

				if ( land_drop >= 0.f && land_drop <= -vz * fireman_phys_dt( ) * 2.f + 12.f ) {
					const int now_opts[ 2 ] = { jump_base | in_jump, jump_base | in_jump | in_duck };

					float best_apex = -FLT_MAX;
					int best_press  = -1;

					for ( int i = 0; i < ( crouch ? 2 : 1 ); ++i ) {
						const float a = fireman_launch_apex( fireman_sim( cmd, now_opts[ i ] ), crouch );

						if ( a > best_apex ) {
							best_apex  = a;
							best_press = now_opts[ i ];
						}
					}

					if ( best_press >= 0 ) {
						cmd->m_buttons = ( cmd->m_buttons & ~( in_jump | in_duck ) ) | best_press;
						planned        = true;
						land_press     = true;
						m_fireman_data.launch_pressed = true;

						botox_dbg_log( "[fr] jb t=%d h=%.0f vz=%.0f ducked=%d apex=%.1f duck=%d", launch_window - m_fireman_data.launch_ticks,
						                   g_prediction.backup_data.m_velocity.length_2d( ), vz, ( int )ducked_now, best_apex - origin.m_z,
						                   ( int )( ( best_press & in_duck ) != 0 ) );
					} else {
						const int rule_duck     = crouch || ducked_now ? in_duck : 0;
						const int duck_opts[ 2 ] = { jump_base | rule_duck, jump_base | ( rule_duck ^ in_duck ) };
						int best_duck            = -1;

						for ( const int d : duck_opts ) {
							for ( int i = 0; i < ( crouch ? 2 : 1 ); ++i ) {
								const float a = fireman_launch_apex( fireman_sim( cmd, d, now_opts[ i ] ), crouch );

								if ( a > best_apex ) {
									best_apex = a;
									best_duck = d;
								}
							}
						}

						if ( best_duck >= 0 ) {
							cmd->m_buttons = ( cmd->m_buttons & ~( in_jump | in_duck ) ) | best_duck;
							planned        = true;
							landing_edge.cmd = cmd;

							botox_dbg_log( "[fr] land plan t=%d vz=%.0f drop=%.1f ducked=%d duck=%d apex=%.1f", launch_window - m_fireman_data.launch_ticks, vz,
							                   land_drop, ( int )ducked_now, ( int )( ( best_duck & in_duck ) != 0 ), best_apex - origin.m_z );
						}
					}
				}

				if ( !planned && ( crouch || ducked_now ) )
					cmd->m_buttons |= in_duck;
			}
		}

		if ( !rehunt && m_fireman_data.launch_ticks + 12 >= launch_window )
			botox_dbg_log( "[fr] win t=%d ground=%d h=%.0f vz=%.0f jump=%d duck=%d ducked=%d", launch_window - m_fireman_data.launch_ticks, ( int )on_ground,
			                   g_prediction.backup_data.m_velocity.length_2d( ), g_prediction.backup_data.m_velocity.m_z,
			                   ( int )( ( cmd->m_buttons & in_jump ) != 0 ), ( int )( ( cmd->m_buttons & in_duck ) != 0 ), ( int )ducked_now );

		const bool chain = m_fireman_data.launch_flung && !on_ground && !fling_tick && g_prediction.backup_data.m_velocity.m_z < 0.f;

		if ( chain && !m_fireman_data.launch_jumped && !land_press ) {
			const float land_drop = drop_to_floor( );

			if ( land_drop >= 0.f && land_drop <= -g_prediction.backup_data.m_velocity.m_z * fireman_phys_dt( ) + fireman_ride_floor_probe )
				landing_edge.cmd = cmd;
		}

		if ( !rehunt && !chain ) {
			m_fireman_data.fell_ready = false;
			m_fireman_data.awall      = false;
			return;
		}
	}

	m_fireman_data.fell_ready = false;

	if ( move_type != move_type_walk || on_ground ) {
		m_fireman_data.awall       = false;
		m_fireman_data.steer_ticks = 0;
		return;
	}

	const float fall_speed = -g_prediction.backup_data.m_velocity.m_z;

	const float drop_left = drop_to_floor( );

	constexpr float max_look_down = 512.f;

	constexpr float slice_floor_gap = 4.f;

	const float look_down = std::min( drop_left < 0.f ? max_look_down : drop_left + ( mins.m_z + maxs.m_z ) * 0.5f - slice_floor_gap, max_look_down );

	constexpr float drop_in_lateral = 72.f;
	constexpr float drop_in_depth   = 512.f;

	fireman_drop_target_t target;
	const bool drop_found = drop_in_on && fireman_find_drop_target( origin, mins, maxs, drop_in_depth, drop_in_lateral, target,
	                                                                m_fireman_data.drop_col_valid ? &m_fireman_data.drop_col : nullptr );

	float column_slice = -1.f;

	if ( drop_found ) {
		constexpr float under_top = 8.f;

		trace_t centre_trace;
		c_trace_filter_trace_type_everything_filter_props centre_flt;
		ray_t centre_ray( origin, c_vector( origin.m_x, origin.m_y, origin.m_z - 8192.f ) );
		g_interfaces.m_engine_trace->trace_ray( centre_ray, mask_playersolid, &centre_flt, &centre_trace );

		const float centre_drop = centre_trace.m_fraction >= 1.f ? -1.f : origin.m_z - centre_trace.m_end.m_z;

		if ( centre_drop < 0.f || target.depth + under_top + 16.f <= centre_drop )
			column_slice = target.depth + under_top;
	}

	constexpr float min_floor_drop = 18.f;

	fireman_ladder_hit_t ladder;
	/* the ladder we just flung off is not a target until we stand again (never strafe back into it) */
	const bool skip_left = m_fireman_data.left_valid;
	bool ladder_found    = fireman_find_ladder( origin, mins, maxs, default_reach, ladder, look_down, skip_left ? &m_fireman_data.left_point : nullptr,
	                                            skip_left ? &m_fireman_data.left_normal : nullptr, column_slice );
	bool ladder_recalled = false;
	bool column_hunt     = false;

	// beside too, not only overhead: ring brake parked us over the ledge 42u short of a ladder whose top is the ledge (log 10-06 12:42)
	if ( drop_found && ( !ladder_found || ladder.dist > 64.f ) ) {
		fireman_ladder_hit_t column;

		if ( fireman_column_face( origin, mins, maxs, target.hit, column ) &&
		     !( skip_left && fireman_is_left_ladder( column.point, m_fireman_data.left_point, m_fireman_data.left_normal ) ) ) {
			ladder       = column;
			ladder_found = true;
			column_hunt  = true;

			static int column_face_log = 0;
			if ( ( column_face_log++ % 16 ) == 0 )
				botox_dbg_log( "[fr] drop_in face dist=%.0f yaw=%.0f depth=%.0f deep=%d h=%.0f over=%d lat=%.0f", column.dist, column.yaw, target.depth,
				               ( int )target.deep, g_prediction.backup_data.m_velocity.length_2d( ), ( int )target.overhead, target.lateral );
		}
	}

	if ( ladder_found ) {
		m_fireman_data.recall_valid  = true;
		m_fireman_data.recall_flat   = ladder.flat_face;
		m_fireman_data.recall_tick   = cmd->m_tick_count;
		m_fireman_data.recall_point  = ladder.point;
		m_fireman_data.recall_normal = ladder.normal;
		m_fireman_data.recall_nz     = ladder.nz;
	} else if ( m_fireman_data.recall_valid ) {
		const c_vector to_point( m_fireman_data.recall_point.m_x - origin.m_x, m_fireman_data.recall_point.m_y - origin.m_y, 0.f );
		const float recall_dist = to_point.length_2d( );
		const int recall_age    = cmd->m_tick_count - m_fireman_data.recall_tick;

		if ( recall_age >= 0 && recall_age <= recall_life && recall_dist <= default_reach ) {
			ladder.found     = true;
			ladder.flat_face = m_fireman_data.recall_flat;
			ladder.dist      = recall_dist;
			ladder.point     = m_fireman_data.recall_point;
			ladder.normal    = m_fireman_data.recall_normal;
			ladder.nz        = m_fireman_data.recall_nz;
			/* yaw off the stored normal; a no-plane hit stored a stale ray dir, so aim at the point instead */
			ladder.yaw = m_fireman_data.recall_flat
			               ? c_vector( -ladder.normal.m_x, -ladder.normal.m_y, 0.f ).to_angle( ).m_y
			               : to_point.to_angle( ).m_y;

			ladder_found    = true;
			ladder_recalled = true;

			static int recall_log = 0;
			if ( ( recall_log++ % 16 ) == 0 )
				botox_dbg_log( "[fr] recall dist=%.0f age=%d yaw=%.0f drop=%.0f v=%.0f", recall_dist, recall_age, ladder.yaw, drop_left, fall_speed );
		}
	}

	bool side_face = false;

	const bool side_only = GET_VARIABLE( g_variables.m_fire_man_side_only, bool ) &&
	                       g_input.check_input( &GET_VARIABLE( g_variables.m_fire_man_side_only_key, key_bind_t ) );
	bool side_pick       = false;
	bool side_staged     = false;
	c_vector side_park_point{ };

	if ( !side_only )
		m_fireman_data.side_valid = false;

	if ( side_only && !ladder_found && m_fireman_data.side_valid &&
	     c_vector( m_fireman_data.side_point.m_x - origin.m_x, m_fireman_data.side_point.m_y - origin.m_y, 0.f ).length_2d( ) <= default_reach ) {
		ladder.found = true;
		ladder.point = m_fireman_data.side_point;
		ladder_found = true;
	}

	if ( side_only && ladder_found ) {
		constexpr float side_same_ladder = 160.f;

		const float from_stored = c_vector( ladder.point.m_x - m_fireman_data.side_point.m_x, ladder.point.m_y - m_fireman_data.side_point.m_y, 0.f ).length_2d( );

		if ( from_stored > side_same_ladder )
			m_fireman_data.side_valid = false;

		if ( !m_fireman_data.side_valid && ladder.flat_face && std::fabsf( ladder.nz ) < 0.1f ) {
			const c_vector catch_at = fireman_catch_point( ladder.point, ladder.normal, mins, maxs );

			fireman_side_target_t sides[ 2 ];
			const int side_count = fireman_side_targets( catch_at, ladder.normal, sides );

			float best       = FLT_MAX;
			int best_index   = -1;
			float best_back  = 0.f;
			bool best_press  = false;
			bool side_press[ 2 ] = { false, false };

			/* cost from where the drift stops if braked now (same brake profile the steer flies), not from origin:
			   the side the drift already carries us toward is the faster one, and the pick never changes after */
			const c_vector drift( g_prediction.backup_data.m_velocity.m_x, g_prediction.backup_data.m_velocity.m_y, 0.f );
			const float drift_h = drift.length_2d( );
			const float stop    = drift_h > 1.f ? fireman_brake_travel( drift_h, fireman_brake_margin * n_air::accel_speed( n_air::read_world( ) ), fireman_phys_dt( ),
			                                                            g_prediction.backup_data.m_velocity.m_z, fireman_gravity( ) )
			                                    : 0.f;
			const c_vector rest = drift_h > 1.f ? c_vector( origin.m_x + drift.m_x / drift_h * stop, origin.m_y + drift.m_y / drift_h * stop, 0.f )
			                                    : c_vector( origin.m_x, origin.m_y, 0.f );

			int buried = 0, blocked = 0;
			float side_ov[ 2 ] = { -2.f, -2.f };

			for ( int i = 0; i < side_count; ++i ) {
				float back = 0.f;
				if ( !fireman_side_back_room( mins, maxs, sides[ i ].point, sides[ i ].normal, sides[ i ].u, sides[ i ].half, back, side_ov[ i ], side_press[ i ] ) ) {
					++buried;
					continue;
				}

				bool staged;
				const c_vector park = fireman_side_park( origin, maxs, sides[ i ].point, sides[ i ].normal, sides[ i ].u, sides[ i ].half, 1.f, back, staged );
				/* the catch spot (origin = point: never staged). park may be the staged waypoint in front */
				bool unused;
				const c_vector final_park =
					fireman_side_park( sides[ i ].point, maxs, sides[ i ].point, sides[ i ].normal, sides[ i ].u, sides[ i ].half, 1.f, back, unused );

				/* hull must fit where it catches (pipe / wall beside the ladder), thin box at the ladder's height.
				   was tested at the waypoint, which sits in front of the ladder and fits almost always */
				trace_t fit;
				c_trace_filter fit_flt( g_ctx.m_local );
				const c_vector fit_at( final_park.m_x, final_park.m_y, sides[ i ].point.m_z );
				ray_t fit_ray( fit_at, fit_at, c_vector( mins.m_x, mins.m_y, -2.f ), c_vector( maxs.m_x, maxs.m_y, 2.f ) );
				g_interfaces.m_engine_trace->trace_ray( fit_ray, mask_playersolid, &fit_flt, &fit );

				if ( fit.m_start_solid || fit.m_all_solid ) {
					++blocked;
					continue;
				}

				float d = c_vector( park.m_x - rest.m_x, park.m_y - rest.m_y, 0.f ).length_2d( );
				if ( staged )
					d += c_vector( final_park.m_x - park.m_x, final_park.m_y - park.m_y, 0.f ).length_2d( );

				if ( d < best ) {
					best       = d;
					best_index = i;
					best_back  = back;
					best_press = side_press[ i ];
				}
			}

			if ( best_index >= 0 ) {
				const fireman_side_target_t& pick = sides[ best_index ];

				m_fireman_data.side_valid  = true;
				m_fireman_data.side_half   = pick.half;
				m_fireman_data.side_back   = best_back;
				m_fireman_data.side_press  = best_press;
				m_fireman_data.side_ov     = side_ov[ best_index ];
				m_fireman_data.side_point  = pick.point;
				m_fireman_data.side_normal = pick.normal;
				m_fireman_data.side_u      = pick.u;
			}

			static int pick_log = 0;
			if ( best_index >= 0 || ( pick_log++ % 16 ) == 0 )
				botox_dbg_log( "[fr] sidepick sides=%d pick=%d buried=%d blocked=%d ov=%.2f/%.2f halfs=%.1f/%.1f half=%.1f back=%.1f press=%d walled=%d nyaw=%.0f dist=%.0f cz=%.0f cost=%.0f stop=%.0f drop=%.0f h=%.0f air=%d key=%d turned=%.0f in=%.0f",
				               side_count, best_index, buried, blocked, side_ov[ 0 ], side_ov[ 1 ], side_count > 0 ? sides[ 0 ].half : 0.f, side_count > 1 ? sides[ 1 ].half : 0.f,
				                   best_index >= 0 ? sides[ best_index ].half : 0.f, best_back, ( int )best_press, best_index >= 0 ? ( int )sides[ best_index ].walled : 0,
				                   best_index >= 0 ? sides[ best_index ].normal.to_angle( ).m_y : 0.f, ladder.dist, catch_at.m_z - ladder.point.m_z,
				                   best_index >= 0 ? best : -1.f, stop, drop_left, drift_h, m_fireman_data.air_ticks, m_fireman_data.key_ticks,
				                   m_fireman_data.air_turned, in_move );
		}

		if ( m_fireman_data.side_valid ) {
			const c_vector& sp = m_fireman_data.side_point;
			const c_vector& sn = m_fireman_data.side_normal;

			// thin side: aim 0.5u INTO the wall, it clips us at max overlap (tools/fr_side_e2e_check.py wall_press).
			// not while over the strip's top: an into-wall wish there is clipped and steals the 30 u/s cap from crossing off it
			constexpr float wall_press = 0.5f;

			bool press = m_fireman_data.side_press;

			if ( press ) {
				const float mid_u = m_fireman_data.side_half - std::max( m_fireman_data.side_ov, 0.f ) * 0.5f;
				const c_vector top_at( sp.m_x - sn.m_x * 0.5f + m_fireman_data.side_u.m_x * mid_u, sp.m_y - sn.m_y * 0.5f + m_fireman_data.side_u.m_y * mid_u,
				                       origin.m_z + maxs.m_z );

				trace_t top;
				c_trace_filter top_flt( g_ctx.m_local );
				ray_t top_ray( top_at, c_vector( top_at.m_x, top_at.m_y, top_at.m_z - 512.f ) );
				g_interfaces.m_engine_trace->trace_ray( top_ray, e_contents::contents_ladder, &top_flt, &top );

				press = top.m_start_solid || top.m_fraction >= 1.f || origin.m_z + mins.m_z <= top.m_end.m_z;
			}

			// wall ends in a ledge (nuke -155): park hugs it by 0.05, band 0.5, drift lands us on it. low ledge = inside the grab window, keep
			constexpr float ledge_guard = 2.f;

			const c_vector& su = m_fireman_data.side_u;
			const float sup_s  = maxs.m_x * ( std::fabsf( sn.m_x ) + std::fabsf( sn.m_y ) );
			const float side_touch_u =
				-m_fireman_data.side_half + 0.5f + maxs.m_x * ( std::fabsf( su.m_x ) + std::fabsf( su.m_y ) ) - m_fireman_data.side_back - 0.05f;

			const c_vector ledge_at( sp.m_x + sn.m_x * ( sup_s + 1.f ) + su.m_x * ( side_touch_u - 0.5f ),
			                         sp.m_y + sn.m_y * ( sup_s + 1.f ) + su.m_y * ( side_touch_u - 0.5f ), origin.m_z );

			trace_t ledge;
			c_trace_filter ledge_flt( g_ctx.m_local );
			ray_t ledge_ray( ledge_at, c_vector( ledge_at.m_x, ledge_at.m_y, ledge_at.m_z - 1024.f ), mins, maxs );
			g_interfaces.m_engine_trace->trace_ray( ledge_ray, mask_playersolid, &ledge_flt, &ledge );

			const bool over_ledge = !ledge.m_start_solid && ledge.m_fraction < 1.f && ledge.m_end.m_z + mins.m_z > sp.m_z + maxs.m_z;

			float park_back = m_fireman_data.side_back + ( press && !over_ledge ? wall_press : 0.f );
			if ( over_ledge ) {
				park_back = std::min( park_back, m_fireman_data.side_back + 0.05f - ledge_guard );

				static int ledge_log = 0;
				if ( ( ledge_log++ % 8 ) == 0 )
					botox_dbg_log( "[fr] ledge z=%.0f feet=%.0f u=%.2f touch=%.2f drop=%.0f", ledge.m_end.m_z + mins.m_z, origin.m_z + mins.m_z,
					               ( origin.m_x - sp.m_x ) * su.m_x + ( origin.m_y - sp.m_y ) * su.m_y, side_touch_u, drop_left );
			}

			side_park_point = fireman_side_park( origin, maxs, sp, sn, m_fireman_data.side_u, m_fireman_data.side_half, 1.f, park_back, side_staged );

			if ( side_staged ) {
				bool unused;
				const c_vector final_park = fireman_side_park( sp, maxs, sp, sn, m_fireman_data.side_u, m_fireman_data.side_half, 1.f, park_back, unused );

				// 0.75 > 0.7u origin error: 0.25 caught more noiseless but latched the front face +1000 under noise (fr_side_e2e_check.py)
				if ( fireman_side_line_clear( origin, final_park, maxs, sp, sn, m_fireman_data.side_u, m_fireman_data.side_half, 0.75f ) ) {
					side_park_point = final_park;
					side_staged     = false;
				}
			}

			ladder.flat_face = true;
			ladder.nz        = 0.f;
			ladder.normal    = sn;
			ladder.point     = sp;
			ladder.yaw       = c_vector( -sn.m_x, -sn.m_y, 0.f ).to_angle( ).m_y;
			ladder.dist      = c_vector( sp.m_x - origin.m_x, sp.m_y - origin.m_y, 0.f ).length_2d( );

			side_pick = true;
			side_face = true;
		}
	}

	if ( !side_only && ladder_found && ( ladder.normal.m_x != 0.f || ladder.normal.m_y != 0.f ) ) {
		constexpr float side_reach = 64.f;

		const float front_probe = ladder.dist + 32.f;

		if ( fireman_face_gap( origin, mins, maxs, ladder.yaw, front_probe ) >= front_probe - 0.5f ) {
			const c_vector tangent( -ladder.normal.m_y, ladder.normal.m_x, 0.f );
			const float side_sign =
				( ladder.point.m_x - origin.m_x ) * tangent.m_x + ( ladder.point.m_y - origin.m_y ) * tangent.m_y >= 0.f ? 1.f : -1.f;
			const c_vector sweep_dir( tangent.m_x * side_sign, tangent.m_y * side_sign, 0.f );

			c_vector side_normal( 0.f, 0.f, 0.f );
			float side_gap = 0.f;

			const float reach_out = fireman_side_face( origin, mins, maxs, sweep_dir, side_reach, side_normal, side_gap )
			                          ? maxs.m_x * ( std::fabsf( side_normal.m_x ) + std::fabsf( side_normal.m_y ) )
			                          : -1.f;
			const c_vector side_point( origin.m_x + sweep_dir.m_x * side_gap - side_normal.m_x * reach_out,
			                           origin.m_y + sweep_dir.m_y * side_gap - side_normal.m_y * reach_out, origin.m_z );

			if ( reach_out >= 0.f && !fireman_face_is_stub( side_point, side_normal, mins, maxs, origin.m_z ) ) {
				const float from_yaw = ladder.yaw;

				ladder.flat_face = true;
				ladder.nz        = 0.f;
				ladder.normal    = side_normal;
				ladder.yaw       = c_vector( -side_normal.m_x, -side_normal.m_y, 0.f ).to_angle( ).m_y;
				ladder.dist      = side_gap + reach_out;
				ladder.point     = side_point;
				side_face        = true;

				static int side_log = 0;
				if ( ( side_log++ % 16 ) == 0 )
					botox_dbg_log( "[fr] side yaw=%.0f->%.0f gap=%.1f drop=%.0f v=%.0f h=%.0f", from_yaw, ladder.yaw, side_gap, drop_left, fall_speed,
					                   g_prediction.backup_data.m_velocity.length_2d( ) );
			}
		}
	}

	const auto side_ok = [ & ]( const c_vector& n ) {
		if ( !side_only || !m_fireman_data.side_valid )
			return true;

		const float len = sqrtf( n.m_x * n.m_x + n.m_y * n.m_y );
		return len > 0.01f && ( n.m_x * m_fireman_data.side_normal.m_x + n.m_y * m_fireman_data.side_normal.m_y ) / len > 0.9f;
	};

	const auto contact_ok = [ & ]( const c_vector& n ) {
		if ( fireman_latch_is_left( origin, maxs, n, m_fireman_data.left_valid, m_fireman_data.left_point, m_fireman_data.left_normal ) )
			return false;

		const float len = sqrtf( n.m_x * n.m_x + n.m_y * n.m_y );
		if ( len < 0.01f )
			return true;

		const c_vector nh( n.m_x / len, n.m_y / len, 0.f );
		const float reach    = maxs.m_x * ( std::fabsf( nh.m_x ) + std::fabsf( nh.m_y ) ) + 4.f;
		const float bands[ ] = { mins.m_z + 1.f, mins.m_z + ( maxs.m_z - mins.m_z ) * 0.3f };

		for ( const float band : bands ) {
			const c_vector from( origin.m_x, origin.m_y, origin.m_z + band );

			trace_t t;
			c_trace_filter flt( g_ctx.m_local );
			ray_t r( from, c_vector( from.m_x - nh.m_x * reach, from.m_y - nh.m_y * reach, from.m_z ) );
			g_interfaces.m_engine_trace->trace_ray( r, e_contents::contents_ladder, &flt, &t );

			if ( !t.m_start_solid && t.m_fraction < 1.f )
				return !fireman_face_is_stub( t.m_end, nh, mins, maxs, origin.m_z );
		}

		return true;
	};

	const float ride_scale = ladder_found ? 1.f - ladder.nz * ladder.nz : 1.f;
	const float grab_at    = fireman_grab_height( std::max( fall_speed, 0.f ) * ride_scale );

	const bool real_drop = !m_fireman_data.ground_z_valid || drop_left < 0.f ||
	                       ( m_fireman_data.last_ground_z - ( origin.m_z - drop_left ) ) > min_floor_drop;

	bool drop_on_top = false;

	if ( drop_in_on ) {
		const c_vector& drift = g_prediction.backup_data.m_velocity;

		bool usable = drop_found;

		if ( usable && !target.deep )
			usable = false;

		if ( usable && skip_left ) {
			c_vector column = origin;

			if ( !target.overhead ) {
				c_vector to_dir;
				g_math.angle_vectors( c_angle( 0.f, target.yaw, 0.f ), &to_dir );
				column = origin + to_dir * target.lateral;
			}

			if ( fireman_is_left_ladder( column, m_fireman_data.left_point, m_fireman_data.left_normal ) )
				usable = false;
		}

		/* ring hit through a wall: LOS at OUR height (the air the approach must cross) */
		if ( usable && !target.overhead ) {
			c_vector to_dir;
			g_math.angle_vectors( c_angle( 0.f, target.yaw, 0.f ), &to_dir );

			trace_t los;
			c_trace_filter_trace_type_everything_filter_props los_flt;
			ray_t los_ray( origin, origin + to_dir * target.lateral, mins, maxs );
			g_interfaces.m_engine_trace->trace_ray( los_ray, mask_playersolid, &los_flt, &los );

			usable = los.m_fraction >= 0.99f;
		}

		if ( usable ) {
			m_fireman_data.lock_lost_ticks = 0;
			m_fireman_data.drop_in_fall    = true;
			m_fireman_data.drop_col        = target.hit;
			m_fireman_data.drop_col_valid  = true;

			/* cooldown + idle reset like the hunt's arm: a lock the idle valve just dropped must not re-arm next tick */
			if ( lock_armed && !m_fireman_data.drop_in_lock && m_fireman_data.lock_cooldown <= 0 ) {
				m_fireman_data.drop_in_lock    = true;
				m_fireman_data.lock_ticks      = 0;
				m_fireman_data.lock_idle_ticks = 0;
				botox_dbg_log( "[fr] lock on over=%d lat=%.0f depth=%.0f", ( int )target.overhead, target.lateral, target.depth );
			}
		} else {
			m_fireman_data.drop_col_valid = false;

			if ( m_fireman_data.drop_in_lock && drop_left > grab_at && ++m_fireman_data.lock_lost_ticks > 16 ) {
				m_fireman_data.drop_in_lock = false;
				botox_dbg_log( "[fr] lock lost drop=%.0f", drop_left );
			}
		}

		/* never inside the catch window: the last 0.2s belong to the grab */
		const bool time_left = drop_left < 0.f || drop_left > grab_at;

		const bool hunt_owns = ladder_found && ( ladder.dist <= 64.f || column_hunt );

		const bool on_top = usable && target.overhead && !hunt_owns;

		drop_on_top = on_top;

		if ( on_top && time_left ) {
			lock_earned( );

			const float top_z        = target.hit.m_z;
			const auto lands_low     = [ & ]( const fireman_landing_t& l ) {
				return l.valid && ( !l.hit || ( l.normal.m_z >= 0.7f && l.z + mins.m_z < top_z - 4.f ) );
			};
			const auto lands_on_top  = [ & ]( const fireman_landing_t& l ) { return l.valid && l.hit && l.normal.m_z >= 0.7f && !lands_low( l ); };
			constexpr float pred_time = 2.f;

			const fireman_landing_t keep = fireman_predict_landing( origin, mins, maxs, drift, nullptr, pred_time );

			if ( lands_on_top( keep ) && !cliff_risk ) {
				const c_vector drift_h( drift.m_x, drift.m_y, 0.f );
				c_vector best_v{ };
				float best_cost = FLT_MAX;
				float best_z    = 0.f;

				for ( int i = 0; i < 8; ++i ) {
					const float a = static_cast< float >( i ) * ( std::numbers::pi_v< float > / 4.f );
					const c_vector dir( cosf( a ), sinf( a ), 0.f );
					const float along = std::max( drift_h.dot_product( dir ), air_veer_cap );
					const c_vector v( dir.m_x * along, dir.m_y * along, drift.m_z );

					const fireman_landing_t l = fireman_predict_landing( origin, mins, maxs, drift, &v, pred_time );
					if ( !lands_low( l ) )
						continue;

					const float cost = c_vector( v.m_x - drift_h.m_x, v.m_y - drift_h.m_y, 0.f ).length_2d( );
					if ( cost < best_cost ) {
						best_cost = cost;
						best_v    = v;
						best_z    = l.hit ? l.z : -1.f;
					}
				}

				const bool picked = best_cost < FLT_MAX;
				const float need  = picked ? best_cost : -1.f;

				static int pred_log = 0;
				if ( ( pred_log++ % 4 ) == 0 )
					botox_dbg_log( "[fr] drop_in pred top=%.0f land=%.0f ladder=%d h=%.0f vz=%.0f pick=%d need=%.0f to=%.0f", top_z, keep.z + mins.m_z,
					                   ( int )keep.ladder, drift.length_2d( ), drift.m_z, ( int )picked, need, best_z );

				if ( picked && need > 2.f ) {
					const float move_yaw = c_vector( best_v.m_x - drift_h.m_x, best_v.m_y - drift_h.m_y, 0.f ).to_angle( ).m_y;

					/* same write + guards as the ring brake below: never a yaw that grabs mid-shaft */
					c_vector move_normal;
					if ( !fireman_can_latch( origin, mins, maxs, move_yaw, move_normal ) ) {
						fireman_write_wish( cmd, move_yaw, move_yaw, fireman_air_wish_speed( need ) );
						cmd->m_buttons &= ~move_buttons;
						cmd->m_buttons |= in_jump;
						m_fireman_data.owns_cmd = true;
						++m_fireman_data.brake_ticks;
						++m_fireman_data.steer_ticks;
						return;
					}
				}
			}

			static int top_log = 0;
			if ( ( top_log++ % 16 ) == 0 )
				botox_dbg_log( "[fr] drop_in top depth=%.0f h=%.0f drop=%.0f slice=%.0f", target.depth, drift.length_2d( ), drop_left, column_slice );
		} else if ( usable && hunt_owns && time_left ) {
			static int hand_log = 0;
			if ( ( hand_log++ % 16 ) == 0 )
				botox_dbg_log( "[fr] drop_in hunt dist=%.0f depth=%.0f over=%d slice=%.0f", ladder.dist, target.depth, ( int )target.overhead, column_slice );
		}

		if ( usable && time_left && !cliff_risk && !on_top ) {
			lock_earned( );

			/* WANTED velocity: zero over the column; beside it the stopping profile (30 floor). A flat 30 braked a closing
			   drift that addspeed can never rebuild. Gap is conservative: ring hit = column inside (r - ring step, r]. */
			c_vector want( 0.f, 0.f, 0.f );

			if ( !target.overhead ) {
				c_vector to_dir;
				g_math.angle_vectors( c_angle( 0.f, target.yaw, 0.f ), &to_dir );

				const float dt_in    = fireman_phys_dt( );
				const float ring_gap = std::max( target.lateral - maxs.m_x - drop_in_lateral * 0.34f, 0.f );
				const float toward   = fireman_hold_speed( ring_gap, fireman_air_accel_speed( ) / std::max( dt_in, 0.001f ), dt_in, 450.f, drift.m_z );

				want = to_dir * std::max( toward, air_veer_cap );
			}

			const c_vector delta( want.m_x - drift.m_x, want.m_y - drift.m_y, 0.f );
			const float need = sqrtf( delta.m_x * delta.m_x + delta.m_y * delta.m_y );

			if ( need > 2.f && !hunt_owns ) {
				const float move_yaw = c_vector( delta.m_x, delta.m_y, 0.f ).to_angle( ).m_y;

				c_vector move_normal;
				if ( !fireman_can_latch( origin, mins, maxs, move_yaw, move_normal ) ) {
					const float wish = fireman_air_wish_speed( need );

					fireman_write_wish( cmd, target.yaw, move_yaw, wish );
					cmd->m_buttons &= ~move_buttons;
					cmd->m_buttons |= in_jump;
					m_fireman_data.owns_cmd = true;

					++m_fireman_data.brake_ticks;
					++m_fireman_data.steer_ticks;
					botox_dbg_log( "[fr] drop_in t=%d over=%d lat=%.0f v=%.0f need=%.0f wish=%.0f auth=%.1f stam=%.0f duck=%.2f vz=%.0f depth=%.0f floor=%.0f",
					                   m_fireman_data.brake_ticks, ( int )target.overhead, target.lateral,
					                   sqrtf( drift.m_x * drift.m_x + drift.m_y * drift.m_y ), need, wish, fireman_air_accel_speed( ),
					                   g_prediction.backup_data.m_stamina, g_prediction.backup_data.m_duck_amount, drift.m_z, target.depth,
					                   target.floor_depth );

					return;
				}
			}
		}
	}

	m_fireman_data.brake_ticks = 0;

	const bool steer_on = approach || ( drop_in_on && m_fireman_data.drop_in_fall && ( !ladder_found || ladder.dist <= 64.f || column_hunt ) );

	const bool hop_mode = !real_drop;

	if ( hop_mode ) {
		const bool hop_window = drop_left >= 0.f && drop_left <= grab_at && !cliff_risk;

		float hop_yaw   = 0.f;
		float hop_press = 0.f;
		bool hop_grab   = false;
		float hop_clear = -1.f;
		c_vector hop_face_normal{ };

		if ( hop_window ) {
			float hop_yaws[ 13 ];
			int hop_count = 0;

			if ( m_fireman_data.yaw_valid ) {
				constexpr float hop_fan[ 5 ] = { 0.f, 20.f, -20.f, 40.f, -40.f };

				for ( const float off : hop_fan )
					hop_yaws[ hop_count++ ] = g_math.normalize_angle( m_fireman_data.ladder_yaw + off );
			}

			for ( int i = 0; i < 8; ++i )
				hop_yaws[ hop_count++ ] = static_cast< float >( i ) * 45.f - 180.f;

			for ( int i = 0; i < hop_count; ++i ) {
				const float yaw = hop_yaws[ i ];

				c_vector hop_normal;
				bool hop_flush = false;
				if ( !fireman_can_latch( origin, mins, maxs, yaw, hop_normal, &hop_flush ) )
					continue;
				if ( hop_flush )
					continue;
				if ( !fireman_face_nz_ok( hop_normal.m_z, ladder_found ? ladder.nz : 0.f ) || !side_ok( hop_normal ) || !contact_ok(hop_normal ) )
					continue;

				const float clear = fireman_fling_clearance( origin, mins, maxs, hop_normal );
				if ( clear <= hop_clear )
					continue;

				hop_clear       = clear;
				hop_press       = yaw;
				hop_yaw         = c_vector( -hop_normal.m_x, -hop_normal.m_y, 0.f ).to_angle( ).m_y;
				hop_face_normal = hop_normal;
				hop_grab        = true;
			}

			if ( !hop_grab ) {
				static int gate_log = 0;
				if ( ( gate_log++ % 16 ) == 0 )
					botox_dbg_log( "[fr] gate z=%.0f floor=%.0f ground_z=%.0f drop=%.0f v=%.0f -> steer", origin.m_z, origin.m_z - drop_left,
					                   m_fireman_data.last_ground_z, drop_left, fall_speed );
			}
		} else {
			static int hop_gate_log = 0;
			if ( ( hop_gate_log++ % 16 ) == 0 )
				botox_dbg_log( "[fr] hopgate drop=%.0f v=%.0f grab_at=%.0f cliff=%d -> steer", drop_left, fall_speed, grab_at, ( int )cliff_risk );
		}

		if ( hop_grab ) {
			const bool hop_hang = !fireman_ride_next_sticks( origin, mins, maxs, g_prediction.backup_data.m_velocity, hop_face_normal );

			botox_dbg_log( "[fr] hop yaw=%.0f press=%.0f drop=%.0f v=%.0f ground_z=%.0f exit=%.0f hang=%d", hop_yaw, hop_press, drop_left, fall_speed,
			                   m_fireman_data.last_ground_z, hop_clear, ( int )hop_hang );

			m_fireman_data.fell_ready = true;
			m_fireman_data.awall      = true;
			m_fireman_data.ladder_yaw = hop_yaw;
			m_fireman_data.yaw_valid  = true;
			m_fireman_data.owns_cmd   = true;
			m_fireman_data.grab_tick  = cmd->m_tick_count;
			lock_earned( );

			m_fireman_data.press_tick   = cmd->m_tick_count;
			m_fireman_data.press_by     = "hop";
			m_fireman_data.press_yaw    = hop_yaw;
			m_fireman_data.press_origin = origin;

			// press -normal, not the fan yaw that hit: a failed straight press still drives in (fr_side_e2e_check.py hop_hit_yaw -284 under noise)
			if ( !fireman_live_press( cmd, hop_yaw, 30.f, hop_face_normal ) ) {
				cmd->m_view_point   = c_angle( 0.f, hop_yaw, 0.f );
				cmd->m_forward_move = 30.f;
				cmd->m_side_move    = 0.f;
			}
			cmd->m_buttons &= ~move_buttons;

			if ( hop_hang )
				cmd->m_buttons &= ~in_jump;
			else
				cmd->m_buttons |= in_jump;

			return;
		}

	}

	m_fireman_data.fell_ready = true;

	const float sv_gravity = g_convars[ HASH_BT( "sv_gravity" ) ] ? g_convars[ HASH_BT( "sv_gravity" ) ]->get_float( ) : 800.f;
	const float g_safe     = sv_gravity > 1.f ? sv_gravity : 1.f;

	const float fall_time = drop_left < 0.f
	                          ? max_catch_time
	                          : std::min( ( sqrtf( fall_speed * fall_speed + 2.f * g_safe * drop_left ) - fall_speed ) / g_safe, max_catch_time );

	if ( ladder_found ) {
		const float face_gap  = fireman_face_gap( origin, mins, maxs, ladder.yaw, ladder.dist + 32.f );
		const float clearance = std::min( face_gap, std::max( ladder.dist - maxs.m_x, 0.f ) );

		if ( real_drop && !m_fireman_data.fall_seen ) {
			m_fireman_data.fall_seen      = true;
			m_fireman_data.fall_seen_drop = drop_left;
			m_fireman_data.fall_min_gap   = face_gap;
			m_fireman_data.fall_win_ticks = 0;
			m_fireman_data.fall_hist_n    = 0;
		}

		m_fireman_data.fall_min_gap   = std::min( m_fireman_data.fall_min_gap, face_gap );
		m_fireman_data.fall_last_gap  = face_gap;
		m_fireman_data.fall_last_dist = ladder.dist;
		m_fireman_data.fall_last_h    = g_prediction.backup_data.m_velocity.length_2d( );
		m_fireman_data.fall_side      = side_pick;
		m_fireman_data.fall_last_ov   = side_pick ? fireman_side_overlap( origin, maxs, m_fireman_data.side_point, m_fireman_data.side_u, m_fireman_data.side_half ) : 0.f;

		auto& hist = m_fireman_data.fall_hist[ m_fireman_data.fall_hist_n++ % 24 ];
		hist       = { drop_left, face_gap, ladder.dist, m_fireman_data.fall_last_h, m_fireman_data.fall_last_ov };
		hist.in    = in_move;
		hist.spin  = view_spin;

		c_vector face_dir;
		g_math.angle_vectors( c_angle( 0.f, ladder.yaw, 0.f ), &face_dir );

		constexpr float park_gap = 1.f;

		const c_vector face_normal = ( ladder.normal.m_x != 0.f || ladder.normal.m_y != 0.f )
		                               ? ladder.normal
		                               : c_vector( -face_dir.m_x, -face_dir.m_y, 0.f );
		const c_vector face_tangent( -face_normal.m_y, face_normal.m_x, 0.f );

		const float hull_reach = maxs.m_x * ( std::fabsf( face_normal.m_x ) + std::fabsf( face_normal.m_y ) );

		/* NEAREST spot, never the middle: hold our place along the face while the hull still touches the known
		   span by edge_overlap, pull in only past that. Square on the hull may hang 12u off the edge. */
		float span_min = 0.f, span_max = 0.f;

		if ( !side_face && std::min( face_gap, ladder.dist ) <= 96.f )
			fireman_face_span( std::fabsf( ladder.nz ) < 0.1f ? fireman_catch_point( ladder.point, face_normal, mins, maxs ) : ladder.point, face_normal,
			                   face_tangent, span_min, span_max );

		float touch_lo = 0.f, touch_hi = 0.f;
		fireman_hull_touch( maxs, face_normal, face_tangent, touch_lo, touch_hi );

		constexpr float edge_overlap = 4.f;

		const c_vector to_point( ladder.point.m_x - origin.m_x, ladder.point.m_y - origin.m_y, 0.f );
		const float lateral_now = -( to_point.m_x * face_tangent.m_x + to_point.m_y * face_tangent.m_y );
		const float lateral_lo  = span_min + edge_overlap - touch_hi;
		const float lateral_hi  = span_max - edge_overlap - touch_lo;
		const float lateral_want = side_face                 ? lateral_now
		                           : lateral_lo > lateral_hi ? ( lateral_lo + lateral_hi ) * 0.5f
		                                                     : std::clamp( lateral_now, lateral_lo, lateral_hi );

		float slant_shift = 0.f;

		if ( std::fabsf( ladder.nz ) > 0.01f ) {
			const float v_catch  = sqrtf( std::max( fall_speed, 0.f ) * std::max( fall_speed, 0.f ) + 2.f * g_safe * std::max( drop_left, 0.f ) );
			const float z_catch  = drop_left >= 0.f ? std::min( origin.m_z, origin.m_z - drop_left + fireman_grab_height( v_catch * ride_scale ) ) : origin.m_z;
			const float z_touch  = z_catch + ( ladder.nz > 0.f ? mins.m_z : maxs.m_z );
			const float nz_slope = ladder.nz / std::max( sqrtf( 1.f - ladder.nz * ladder.nz ), 0.1f );

			slant_shift = -nz_slope * ( z_touch - ladder.point.m_z );
		}

		float park_out = hull_reach + park_gap + slant_shift;

		if ( !side_pick && face_gap >= ladder.dist + 32.f - 0.5f ) {
			const float lat_now = -( ( ladder.point.m_x - origin.m_x ) * face_tangent.m_x + ( ladder.point.m_y - origin.m_y ) * face_tangent.m_y );
			const auto floor_at = [ & ]( const float out ) -> float {
				const c_vector top( ladder.point.m_x + face_normal.m_x * out + face_tangent.m_x * lat_now,
				                    ladder.point.m_y + face_normal.m_y * out + face_tangent.m_y * lat_now, origin.m_z );
				trace_t t;
				c_trace_filter_trace_type_everything_filter_props flt;
				ray_t r( top, c_vector( top.m_x, top.m_y, top.m_z - 1024.f ), mins, maxs );
				g_interfaces.m_engine_trace->trace_ray( r, mask_playersolid, &flt, &t );
				if ( t.m_start_solid )
					return FLT_MAX;
				return t.m_fraction >= 1.f ? top.m_z - 1024.f : t.m_end.m_z;
			};

			const float here = floor_at( park_out );
			for ( const float extra : { 8.f, 16.f, 24.f } ) {
				if ( const float out_floor = floor_at( park_out + extra ); out_floor < here - 24.f ) {
					static int lip_log = 0;
					if ( ( lip_log++ % 16 ) == 0 )
						botox_dbg_log( "[fr] lip park out=%.0f+%.0f floor=%.0f -> %.0f drop=%.0f", park_out, extra, here, out_floor, drop_left );
					park_out += extra;
					break;
				}
			}
		}

		float lateral_park = lateral_want;

		if ( !side_face && !side_pick && span_max > span_min ) {
			const float lateral_mid = ( span_min + span_max ) * 0.5f;

			if ( std::fabsf( lateral_mid - lateral_want ) > 4.f ) {
				const auto column_floor = [ & ]( const float lat, bool& on_ladder ) -> float {
					const c_vector top( ladder.point.m_x + face_normal.m_x * park_out + face_tangent.m_x * lat,
					                    ladder.point.m_y + face_normal.m_y * park_out + face_tangent.m_y * lat, origin.m_z );

					trace_t t;
					c_trace_filter_trace_type_everything_filter_props flt;
					ray_t r( top, c_vector( top.m_x, top.m_y, top.m_z - 1024.f ), mins, maxs );
					g_interfaces.m_engine_trace->trace_ray( r, mask_playersolid, &flt, &t );

					on_ladder = t.m_fraction < 1.f && !t.m_start_solid && botox_is_ladder_trace( t );
					return t.m_fraction >= 1.f ? top.m_z - 1024.f : t.m_end.m_z;
				};

				bool park_ladder = false, mid_ladder = false;
				const float park_floor = column_floor( lateral_want, park_ladder );
				const float mid_floor  = column_floor( lateral_mid, mid_ladder );

				if ( !park_ladder && ( mid_ladder || mid_floor < park_floor - 8.f ) ) {
					lateral_park = lateral_mid;

					static int column_log = 0;
					if ( ( column_log++ % 16 ) == 0 )
						botox_dbg_log( "[fr] column park=%.0f floor=%.0f -> mid=%.0f floor=%.0f ladder=%d", lateral_want, park_floor, lateral_mid, mid_floor,
						                   ( int )mid_ladder );
				}
			}
		}

		const c_vector park_point = side_pick ? side_park_point
		                                      : c_vector( ladder.point.m_x + face_normal.m_x * park_out + face_tangent.m_x * lateral_park,
		                                                  ladder.point.m_y + face_normal.m_y * park_out + face_tangent.m_y * lateral_park, 0.f );

		const c_vector steer_flat( g_prediction.backup_data.m_velocity.m_x, g_prediction.backup_data.m_velocity.m_y, 0.f );

		const c_vector to_park( park_point.m_x - origin.m_x, park_point.m_y - origin.m_y, 0.f );

		const bool face_level = face_gap < ladder.dist + 32.f - 0.5f;
		/* staged side: the waypoint is OUT past the side plane, a one-sided half would never go there */
		const bool two_sided  = !face_level || ladder.nz >= 0.3f || side_staged;

		const float park_in_raw = to_park.m_x * face_dir.m_x + to_park.m_y * face_dir.m_y;
		const float park_in     = two_sided ? park_in_raw : std::max( park_in_raw, 0.f );
		const float park_lat = to_park.m_x * face_tangent.m_x + to_park.m_y * face_tangent.m_y;

		const c_vector rel( face_dir.m_x * park_in + face_tangent.m_x * park_lat,
		                    face_dir.m_y * park_in + face_tangent.m_y * park_lat, 0.f );

		const n_air::world_t air_world = n_air::read_world( );
		const float accel_speed        = n_air::accel_speed( air_world );
		const float phys_dt            = fireman_phys_dt( );
		const float decel              = accel_speed / std::max( phys_dt, 0.001f );
		const float vz_now      = g_prediction.backup_data.m_velocity.m_z;
		const float hold_dist = 8.f;
		const float catch_time = fall_time > ignore_jump_time ? fall_time - ignore_jump_time : 0.f;
		const int sim_ticks    = std::clamp( static_cast< int >( catch_time / std::max( phys_dt, 0.001f ) ), 1, 128 );

		const bool steer_gain = side_pick || approach || ( drop_in_on && m_fireman_data.drop_in_fall );

		float best_dist      = FLT_MAX;
		const int best_ticks = fireman_hold_ticks( steer_flat, rel, air_world, hold_dist, sim_ticks, phys_dt, steer_gain, vz_now, best_dist );

		const bool in_grab_range = clearance <= grab_clearance;

		/* beside the column the gate never refuses: the far gate is for DISTANT ladders, and a fast fall that
		   shrinks the sim budget also widens the catch window (grab_at ~ v * 0.216). 64 = a shaft's width. */
		constexpr float near_column = 64.f;

		const bool beside_column = std::min( face_gap, ladder.dist ) <= near_column;

		constexpr float hop_steer_reach = 128.f;

		if ( hop_mode && std::min( face_gap, ladder.dist ) > hop_steer_reach ) {
			m_fireman_data.awall = false;
			return;
		}

		if ( !in_grab_range && !beside_column && best_ticks < 0 ) {
			static int far_log = 0;
			if ( ( far_log++ % 16 ) == 0 )
				botox_dbg_log( "[fr] far clear=%.0f miss=%.0f dist=%.0f drop=%.0f v=%.0f sim=%d", clearance, best_dist, ladder.dist, drop_left, fall_speed, sim_ticks );

			/* refused mid-fall: unwind the approach we built, bounded by steer_ticks, only while the keys are
			   ours, and never while the drift still CLOSES on the column ("can't park" != "unreachable") */
			const bool far_closes = steer_flat.m_x * rel.m_x + steer_flat.m_y * rel.m_y > 0.f;

			if ( steer_on && !cliff_risk && !far_closes && m_fireman_data.steer_ticks > 0 && ( locked || !user_steering ) &&
			     fireman_brake_drift( cmd, origin, mins, maxs, steer_flat, ladder.yaw ) ) {
				m_fireman_data.owns_cmd = true;
				--m_fireman_data.steer_ticks;
			}

			m_fireman_data.awall = false;
			return;
		}

		m_fireman_data.awall      = in_grab_range;
		m_fireman_data.ladder_yaw = ladder.yaw;
		m_fireman_data.yaw_valid  = true;

		if ( approach && !m_fireman_data.ladder_lock && m_fireman_data.lock_cooldown <= 0 ) {
			m_fireman_data.ladder_lock     = true;
			m_fireman_data.lock_ticks      = 0;
			m_fireman_data.lock_idle_ticks = 0;
			botox_dbg_log( "[fr] ladder lock gap=%.1f dist=%.0f drop=%.0f v=%.0f", face_gap, ladder.dist, drop_left, fall_speed );
		}

		m_fireman_data.ladder_lost_ticks = 0;

		c_vector latch_normal;
		bool latch_flush      = false;
		const bool can_latch  = fireman_can_latch( origin, mins, maxs, ladder.yaw, latch_normal, &latch_flush );
		bool flat_latch = can_latch && !latch_flush && fireman_face_nz_ok( latch_normal.m_z, ladder.nz ) && side_ok( latch_normal ) && contact_ok(latch_normal );
		float latch_yaw = ladder.yaw;

		fireman_ride_end_t ride_end{ };
		ride_end.room    = drop_left >= 0.f ? drop_left : FLT_MAX;
		ride_end.grab_at = grab_at;
		ride_end.floor   = true;

		if ( face_gap < 24.f ) {
			const float nz_h       = sqrtf( std::max( 1.f - ladder.nz * ladder.nz, 0.f ) );
			const c_vector ride_n  = can_latch && !latch_flush ? latch_normal : c_vector( face_normal.m_x * nz_h, face_normal.m_y * nz_h, ladder.nz );
			const c_vector ride_at = can_latch ? origin : origin + face_dir * std::max( face_gap - 1.f, 0.f );

			ride_end = fireman_ride_end( ride_at, mins, maxs, g_prediction.backup_data.m_velocity, ride_n, drop_left );
		}

		const bool in_window = !hop_mode && ride_end.room >= 0.f && ride_end.room <= ride_end.grab_at;

		if ( in_window )
			++m_fireman_data.fall_win_ticks;

		if ( in_window && !flat_latch && face_gap < 24.f ) {
			constexpr float fan_offsets[ 12 ] = { 15.f, -15.f, 30.f, -30.f, 45.f, -45.f, 60.f, -60.f, 75.f, -75.f, 90.f, -90.f };

			float fan_clear = 12.f;

			for ( const float off : fan_offsets ) {
				const float yaw = g_math.normalize_angle( ladder.yaw + off );

				c_vector fan_normal;
				bool fan_flush = false;
				if ( !fireman_can_latch( origin, mins, maxs, yaw, fan_normal, &fan_flush ) )
					continue;
				if ( fan_flush || !fireman_face_nz_ok( fan_normal.m_z, ladder.nz ) || !side_ok( fan_normal ) || !contact_ok(fan_normal ) )
					continue;

				const float clear = fireman_fling_clearance( origin, mins, maxs, fan_normal );
				if ( clear <= fan_clear )
					continue;

				fan_clear    = clear;
				latch_normal = fan_normal;
				latch_yaw    = yaw;
				flat_latch   = true;
			}

			if ( flat_latch )
				botox_dbg_log( "[fr] edge yaw=%.0f->%.0f gap=%.1f exit=%.0f drop=%.0f", ladder.yaw, latch_yaw, face_gap, fan_clear, drop_left );
		}

		constexpr float straight_h = 8.f;

		const float steer_h = sqrtf( steer_flat.m_x * steer_flat.m_x + steer_flat.m_y * steer_flat.m_y );
		const bool window_spare = steer_on && flat_latch && in_window && ride_end.room > 0.5f * ride_end.grab_at;
		const bool edge_slide   = window_spare && !fireman_ride_holds( origin, mins, maxs, steer_flat, latch_normal,
		                                                             static_cast< float >( fireman_catch_ticks( ) ) * fireman_phys_dt( ) );
		const float latch_nh    = sqrtf( latch_normal.m_x * latch_normal.m_x + latch_normal.m_y * latch_normal.m_y );
		const float drift_out   = latch_nh > 0.01f ? ( steer_flat.m_x * latch_normal.m_x + steer_flat.m_y * latch_normal.m_y ) / latch_nh : 0.f;
		const float drift_kept  = sqrtf( std::max( steer_h * steer_h - drift_out * drift_out, 0.f ) + std::max( drift_out, 0.f ) * std::max( drift_out, 0.f ) );
		const bool straighten   = window_spare && ( drift_kept > straight_h || edge_slide );

		if ( straighten ) {
			static int straighten_log = 0;
			if ( ( straighten_log++ % 8 ) == 0 )
				botox_dbg_log( "[fr] straighten h=%.0f gap=%.1f drop=%.0f grab_at=%.0f v=%.0f edge=%d side=%d", steer_h, face_gap, drop_left, grab_at,
				                   fall_speed, ( int )edge_slide, ( int )side_face );
		}

		const bool grab_now = flat_latch && in_window && !straighten;

		if ( grab_now ) {
			float side_ov = 0.f, side_du = 0.f;
			if ( side_pick ) {
				side_ov = fireman_side_overlap( origin, maxs, m_fireman_data.side_point, m_fireman_data.side_u, m_fireman_data.side_half );
				side_du = steer_flat.m_x * m_fireman_data.side_u.m_x + steer_flat.m_y * m_fireman_data.side_u.m_y;
			}
			botox_dbg_log( "[fr] grab drop=%.0f room=%.0f v=%.0f grab_at=%.0f end=%s nz=%.2f side=%d h=%.0f hang=%d ov=%.2f du=%.1f air=%d turned=%.0f", drop_left,
			                   ride_end.room, fall_speed, ride_end.grab_at, ride_end.floor ? "floor" : "ladder", latch_normal.m_z, ( int )side_face, steer_h,
			                   ( int )( ride_end.room < ride_end.step ), side_ov, side_du, m_fireman_data.air_ticks, m_fireman_data.air_turned );
		} else if ( can_latch ) {
			static int hold_log = 0;
			if ( ( hold_log++ % 8 ) == 0 )
				botox_dbg_log( "[fr] hold drop=%.0f room=%.0f v=%.0f grab_at=%.0f end=%s nz=%.2f flat=%d flush=%d gap=%.1f", drop_left,
				                   ride_end.room < FLT_MAX ? ride_end.room : -1.f, fall_speed,
				                   ride_end.grab_at, ride_end.floor ? "floor" : "ladder", latch_normal.m_z, ( int )flat_latch, ( int )latch_flush, face_gap );
		} else {
			static int steer_log = 0;
			if ( ( steer_log++ % 16 ) == 0 )
				botox_dbg_log( "[fr] steer dist=%.0f gap=%.0f clear=%.0f drop=%.0f v=%.0f grab_at=%.0f rec=%d left=%d lat=%.1f", ladder.dist, face_gap, clearance, drop_left,
				                   fall_speed, grab_at, ( int )ladder_recalled, ( int )skip_left, park_lat );
		}

		if ( !grab_now ) {
			constexpr float back_out_start = 0.5f;
			constexpr float back_out_done  = 2.0f;

			const bool flush_now = latch_flush || ( can_latch && face_gap < back_out_start && !in_window );

			if ( flush_now )
				m_fireman_data.backing_out = true;
			else if ( m_fireman_data.backing_out && ( face_gap >= back_out_done || in_window ) )
				m_fireman_data.backing_out = false;

			if ( steer_on && m_fireman_data.backing_out && !cliff_risk ) {
				constexpr float out_offsets[ 5 ] = { 0.f, 30.f, -30.f, 60.f, -60.f };

				float out_yaw  = 0.f;
				bool out_found = false;

				for ( const float off : out_offsets ) {
					const float yaw = g_math.normalize_angle( ladder.yaw + 180.f + off );

					c_vector out_normal;
					if ( fireman_can_latch( origin, mins, maxs, yaw, out_normal ) )
						continue;

					c_vector out_dir;
					g_math.angle_vectors( c_angle( 0.f, yaw, 0.f ), &out_dir );

					if ( fireman_fling_clearance( origin, mins, maxs, out_dir ) < 12.f )
						continue;

					out_yaw   = yaw;
					out_found = true;
					break;
				}

				if ( out_found ) {
					fireman_write_wish( cmd, ladder.yaw, out_yaw, 450.f );
					lock_earned( );
					cmd->m_buttons &= ~move_buttons;
					cmd->m_buttons |= in_jump;
					m_fireman_data.owns_cmd = true;

					static int flush_log = 0;
					if ( ( flush_log++ % 8 ) == 0 )
						botox_dbg_log( "[fr] flush out=%.0f face=%.0f gap=%.1f dist=%.0f drop=%.0f v=%.0f", out_yaw, ladder.yaw, face_gap,
						                   ladder.dist, drop_left, fall_speed );

					return;
				}

				static int boxed_log = 0;
				if ( ( boxed_log++ % 16 ) == 0 )
					botox_dbg_log( "[fr] flush boxed face=%.0f gap=%.1f drop=%.0f v=%.0f", ladder.yaw, face_gap, drop_left, fall_speed );
			}

			const float drive_len = sqrtf( rel.m_x * rel.m_x + rel.m_y * rel.m_y );
			const c_vector drive_dir = drive_len > 0.01f ? c_vector( rel.m_x / drive_len, rel.m_y / drive_len, 0.f ) : face_dir;
			const float along        = steer_flat.m_x * drive_dir.m_x + steer_flat.m_y * drive_dir.m_y;

			// thin side: over the stop profile the station keeper brakes instead. nuke #3 log 10-07: 246 u/s in, slanted wall beside the 0.42u strip slid us off it
			const bool drive_hot = side_pick && along > std::max( fireman_hold_speed( drive_len, decel, phys_dt, 450.f, vz_now ), air_veer_cap );

			const bool drive_in = steer_on && in_window && !can_latch && !latch_flush && !straighten && !cliff_risk &&
			                      !m_fireman_data.backing_out && face_gap < 24.f && !drive_hot;

			if ( drive_hot && steer_on && in_window && face_gap < 24.f ) {
				static int hot_log = 0;
				if ( ( hot_log++ % 8 ) == 0 )
					botox_dbg_log( "[fr] drive hot along=%.0f err=%.1f gap=%.1f drop=%.0f", along, drive_len, face_gap, drop_left );
			}

			if ( drive_in ) {

				const float win_time  = drop_left > 0.f && fall_speed > 1.f ? drop_left / fall_speed : phys_dt;
				const float need_rate = drive_len / std::max( win_time, phys_dt );
				const float want_in   = std::max( std::max( air_veer_cap, std::min( need_rate, 450.f ) ) - along, 0.f );
				const float push = want_in > 1.f ? fireman_air_wish_speed( want_in ) : 30.f;

				fireman_write_wish( cmd, ladder.yaw, drive_dir.to_angle( ).m_y, push );
				lock_earned( );
				cmd->m_buttons &= ~move_buttons;
				/* jump held: an unpredicted latch keeps the fall (:3706) ...unless a ride from here ends inside its first
				   tick: that latch must hang (:3786) at the end */
				if ( ride_end.room >= ride_end.step )
					cmd->m_buttons |= in_jump;
				else
					cmd->m_buttons &= ~in_jump;

				m_fireman_data.owns_cmd = true;
				m_fireman_data.grab_tick = cmd->m_tick_count;
				++m_fireman_data.steer_ticks;

				static int drive_log = 0;
				if ( ( drive_log++ % 8 ) == 0 )
					botox_dbg_log( "[fr] drive gap=%.1f dist=%.0f err=%.1f lat=%.1f along=%.0f push=%.0f drop=%.0f v=%.0f", face_gap, ladder.dist,
					                   drive_len, park_lat, along, push, drop_left, fall_speed );

				hist.err   = drive_len;
				hist.along = along;
				hist.wrote = 2;
				return;
			}

			bool steer_wrote = false;
			float steer_need = -1.f;

			if ( steer_on && !cliff_risk ) {
				/* park, don't chase: the stopping profile is the anti-overshoot */
				const float rel_len      = sqrtf( rel.m_x * rel.m_x + rel.m_y * rel.m_y );
				const c_vector to_target = rel_len > 0.01f ? c_vector( rel.m_x / rel_len, rel.m_y / rel_len, 0.f ) : face_dir;

				const float raw_error = rel_len;

				// in window without a latch the band must not park us: 1u short of a thin side = zero overlap = no catch
				const float band_edge = in_window ? ( can_latch ? park_gap : 0.1f ) : ( can_latch ? 1.5f : back_out_start );
				const bool at_face      = !in_window && ( can_latch || face_gap <= 2.f );
				const float steer_error = at_face || rel_len <= band_edge ? 0.f : raw_error;

				const float closing_cap = in_window && straighten ? air_veer_cap : 450.f;
				bool gain_perp          = false;
				c_vector delta = fireman_steer_delta( steer_flat, to_target, std::max( steer_error, 0.f ), decel, phys_dt, closing_cap, steer_gain,
				                                      vz_now, &gain_perp );
				float need     = sqrtf( delta.m_x * delta.m_x + delta.m_y * delta.m_y );

				const float cross_s = ( origin.m_x - ladder.point.m_x ) * face_normal.m_x + ( origin.m_y - ladder.point.m_y ) * face_normal.m_y;
				const float cross_to = hull_reach + park_gap;

				if ( cross_s < cross_to ) {
					const float top_z  = fireman_column_top( origin, mins, maxs, ladder.point, face_normal );
					const float feet_z = origin.m_z + mins.m_z;

					if ( top_z > -FLT_MAX && top_z < FLT_MAX && feet_z > top_z ) {
						const float t_top   = ( vz_now + sqrtf( vz_now * vz_now + 2.f * g_safe * ( feet_z - top_z ) ) ) / g_safe;
						const float rate    = ( cross_to - cross_s ) / std::max( t_top - phys_dt, phys_dt );
						const float want_n  = ( steer_flat.m_x + delta.m_x ) * face_normal.m_x + ( steer_flat.m_y + delta.m_y ) * face_normal.m_y;
						const float vn      = steer_flat.m_x * face_normal.m_x + steer_flat.m_y * face_normal.m_y;
						const float target_n = std::max( want_n, rate );
						bool strafed        = false;

						if ( want_n < rate ) {
							delta.m_x += face_normal.m_x * ( rate - want_n );
							delta.m_y += face_normal.m_y * ( rate - want_n );
							need = sqrtf( delta.m_x * delta.m_x + delta.m_y * delta.m_y );
						}

						// slow run-up landed on the top: straight adds nothing past 30 u/s, strafe-gain from takeoff (gain integrates).
						// not side picks: e2e 128t +10 slid off thin sides for +2 ok
						if ( !side_pick && target_n - vn > 2.f ) {
							const float u_now  = steer_flat.m_x * face_tangent.m_x + steer_flat.m_y * face_tangent.m_y;
							const float u_want = ( steer_flat.m_x + delta.m_x ) * face_tangent.m_x + ( steer_flat.m_y + delta.m_y ) * face_tangent.m_y;
							const c_vector w   = fireman_gain_dir( steer_flat, face_normal, u_want >= u_now ? 1.f : -1.f,
							                                       accel_speed * fireman_air_friction( vz_now, fireman_gravity( ), phys_dt ) );

							delta     = c_vector( w.m_x * 450.f, w.m_y * 450.f, 0.f );
							need      = 450.f;
							gain_perp = false;
							strafed   = true;
						}

						botox_dbg_log( "[fr] cross s=%.1f to=%.1f top=%.0f feet=%.0f t=%.3f rate=%.0f want_n=%.0f vn=%.0f h=%.0f need=%.0f str=%d", cross_s, cross_to,
						               top_z, feet_z, t_top, rate, want_n, vn, steer_h, need, ( int )strafed );
					}
				}

				const bool turning = steer_h > 1.f && need > 1.f &&
				                     std::fabsf( ( steer_flat.m_x * delta.m_x + steer_flat.m_y * delta.m_y ) / ( steer_h * need ) ) < 0.2f;

				steer_need = need;
				hist.err   = raw_error;
				hist.need  = need;
				hist.along = steer_flat.m_x * to_target.m_x + steer_flat.m_y * to_target.m_y;

				if ( need > 2.f ) {
					float steer_yaw = c_vector( delta.m_x, delta.m_y, 0.f ).to_angle( ).m_y;

					c_vector steer_normal;
					bool steer_ok = !fireman_can_latch( origin, mins, maxs, steer_yaw, steer_normal );
					const bool back_contact = !steer_ok && steer_normal.m_x * face_normal.m_x + steer_normal.m_y * face_normal.m_y < -0.5f;

					if ( !steer_ok && gain_perp ) {
						const c_vector retry[ 2 ] = { c_vector( -delta.m_x, -delta.m_y, 0.f ),
						                              fireman_hold_delta( steer_flat, to_target, std::max( steer_error, 0.f ), decel, phys_dt, closing_cap, vz_now ) };

						for ( const c_vector& alt : retry ) {
							const float alt_need = sqrtf( alt.m_x * alt.m_x + alt.m_y * alt.m_y );
							const float alt_yaw  = c_vector( alt.m_x, alt.m_y, 0.f ).to_angle( ).m_y;

							if ( alt_need <= 2.f || fireman_can_latch( origin, mins, maxs, alt_yaw, steer_normal ) )
								continue;

							delta     = alt;
							need      = alt_need;
							steer_yaw = alt_yaw;
							steer_ok  = true;
							break;
						}
					}

					if ( steer_ok ) {
						const float steer_wish = fireman_air_wish_speed( need );

						fireman_write_wish( cmd, ladder.yaw, steer_yaw, steer_wish );
						lock_earned( );
						cmd->m_buttons &= ~move_buttons;

						cmd->m_buttons |= in_jump;
						m_fireman_data.owns_cmd = true;
						steer_wrote             = true;
						++m_fireman_data.steer_ticks;

						static int veer_log = 0;
						if ( ( veer_log++ % 16 ) == 0 )
							botox_dbg_log( "[fr] veer need=%.0f eta=%d dist=%.0f gap=%.1f err=%.1f latch=%d along=%.0f yaw=%.0f wish=%.0f h=%.0f sim=%d trn=%d gain=%d fr=%.2f vz=%.0f auth=%.1f in=%.0f spin=%.1f",
							                   need, best_ticks, rel_len, face_gap, steer_error, ( int )can_latch,
							                   steer_flat.m_x * to_target.m_x + steer_flat.m_y * to_target.m_y, steer_yaw, steer_wish,
							                   steer_h, sim_ticks, ( int )turning, ( int )gain_perp, fireman_air_friction( vz_now, fireman_gravity( ), phys_dt ), vz_now, accel_speed,
							                   in_move, view_spin );
					} else if ( back_contact ) {
						lock_earned( );

						static int back_coast_log = 0;
						if ( ( back_coast_log++ % 8 ) == 0 )
							botox_dbg_log( "[fr] back coast h=%.0f vz=%.0f s=%.1f", steer_h, vz_now, cross_s );
					} else if ( m_fireman_data.steer_ticks > 0 ) {
						if ( fireman_brake_drift( cmd, origin, mins, maxs, steer_flat, ladder.yaw ) ) {
							m_fireman_data.owns_cmd = true;
							steer_wrote             = true;
							lock_earned( );

							static int hold_brake_log = 0;
							if ( ( hold_brake_log++ % 16 ) == 0 )
								botox_dbg_log( "[fr] brake latch h=%.0f gap=%.1f dist=%.0f drop=%.0f", sqrtf( steer_flat.m_x * steer_flat.m_x + steer_flat.m_y * steer_flat.m_y ),
								                   face_gap, ladder.dist, drop_left );
						}
					}
				}
			}

			hist.wrote = steer_wrote ? 1 : 0;

			if ( steer_on && !steer_wrote && ( can_latch || face_gap < 24.f ) ) {
				cmd->m_forward_move = 0.f;
				cmd->m_side_move    = 0.f;
				cmd->m_buttons &= ~move_buttons;
				cmd->m_buttons |= in_jump;
				m_fireman_data.owns_cmd = true;
				lock_earned( );

				static int park_log = 0;
				if ( ( park_log++ % 16 ) == 0 )
					botox_dbg_log( "[fr] park gap=%.1f latch=%d dist=%.0f drop=%.0f v=%.0f", face_gap, ( int )can_latch, ladder.dist, drop_left, fall_speed );
			} else if ( !steer_wrote ) {
				/* seen, nothing written. need -1 = controller never ran (cliff band), <= 2 = parked too far out,
				   else its wish would latch and the brake fallback refused too */
				static int quiet_log = 0;
				if ( ( quiet_log++ % 16 ) == 0 )
					botox_dbg_log( "[fr] quiet need=%.0f cliff=%d gap=%.1f dist=%.0f back=%d win=%d drop=%.0f v=%.0f h=%.0f in=%.0f spin=%.1f side=%d", steer_need,
					                   ( int )cliff_risk, face_gap, ladder.dist, ( int )m_fireman_data.backing_out, ( int )in_window, drop_left, fall_speed,
					                   sqrtf( steer_flat.m_x * steer_flat.m_x + steer_flat.m_y * steer_flat.m_y ), in_move, view_spin, ( int )side_pick );
			}

			return;
		}

		float grab_yaw   = latch_yaw;
		float grab_clear     = fireman_fling_clearance( origin, mins, maxs, latch_normal );
		c_vector grab_normal = latch_normal;

		if ( grab_clear < 24.f ) {
			for ( int i = 0; i < 8; ++i ) {
				const float yaw = static_cast< float >( i ) * 45.f - 180.f;

				c_vector scan_normal;
				bool face_flush = false;
				if ( !fireman_can_latch( origin, mins, maxs, yaw, scan_normal, &face_flush ) )
					continue;
				if ( face_flush )
					continue;
				if ( !fireman_face_nz_ok( scan_normal.m_z, ladder.nz ) || !side_ok( scan_normal ) || !contact_ok(scan_normal ) )
					continue;

				const float clear = fireman_fling_clearance( origin, mins, maxs, scan_normal );
				if ( clear <= grab_clear )
					continue;

				grab_clear = clear;
				grab_yaw    = yaw;
				grab_normal = scan_normal;
			}

			botox_dbg_log( "[fr] face yaw=%.0f->%.0f exit=%.0f", ladder.yaw, grab_yaw, grab_clear );
		}

		m_fireman_data.owns_cmd    = true;
		m_fireman_data.grab_tick   = cmd->m_tick_count;
		m_fireman_data.press_tick   = cmd->m_tick_count;
		m_fireman_data.press_by     = "grab";
		m_fireman_data.press_yaw    = grab_yaw;
		m_fireman_data.press_origin = origin;
		m_fireman_data.ladder_yaw  = grab_yaw;
		m_fireman_data.backing_out = false;
		lock_earned( );

		constexpr float grab_push = 30.f;

		if ( !fireman_live_press( cmd, grab_yaw, grab_push, grab_normal ) ) {
			cmd->m_view_point   = c_angle( 0.f, grab_yaw, 0.f );
			cmd->m_forward_move = grab_push;
			cmd->m_side_move    = 0.f;
		}
		cmd->m_buttons &= ~move_buttons;

		if ( !fireman_ride_next_sticks( origin, mins, maxs, g_prediction.backup_data.m_velocity, grab_normal ) ) {
			cmd->m_buttons &= ~in_jump;
			botox_dbg_log( "[fr] grab hang drop=%.0f room=%.0f v=%.0f", drop_left, ride_end.room, fall_speed );
		} else
			cmd->m_buttons |= in_jump;

		return;
	}

	{
		static int miss_log = 0;
		if ( ( miss_log++ % 16 ) == 0 )
			botox_dbg_log( "[fr] miss reach=%.0f drop=%.0f v=%.0f down=%.0f left=%d", default_reach, drop_left, fall_speed, look_down, ( int )skip_left );
	}

	if ( m_fireman_data.ladder_lock && ++m_fireman_data.ladder_lost_ticks > 16 ) {
		m_fireman_data.ladder_lock = false;
		botox_dbg_log( "[fr] ladder lock lost drop=%.0f", drop_left );
	}

	bool drift_closes = false;

	if ( m_fireman_data.recall_valid && cmd->m_tick_count - m_fireman_data.recall_tick <= recall_life ) {
		const c_vector to_recall( m_fireman_data.recall_point.m_x - origin.m_x, m_fireman_data.recall_point.m_y - origin.m_y, 0.f );

		drift_closes = to_recall.m_x * g_prediction.backup_data.m_velocity.m_x + to_recall.m_y * g_prediction.backup_data.m_velocity.m_y > 0.f;
	}

	if ( drop_found && !target.overhead ) {
		c_vector to_column;
		g_math.angle_vectors( c_angle( 0.f, target.yaw, 0.f ), &to_column );

		drift_closes = drift_closes || to_column.m_x * g_prediction.backup_data.m_velocity.m_x + to_column.m_y * g_prediction.backup_data.m_velocity.m_y > 0.f;
	}

	if ( steer_on && !cliff_risk && !drift_closes && !drop_on_top && m_fireman_data.steer_ticks > 0 && ( locked || !user_steering ) &&
	     ( drop_left < 0.f || drop_left > grab_at ) ) {
		const c_vector drift_flat( g_prediction.backup_data.m_velocity.m_x, g_prediction.backup_data.m_velocity.m_y, 0.f );

		if ( fireman_brake_drift( cmd, origin, mins, maxs, drift_flat, cmd->m_view_point.m_y ) ) {
			m_fireman_data.owns_cmd = true;
			m_fireman_data.awall    = false;
			--m_fireman_data.steer_ticks;

			static int miss_brake_log = 0;
			if ( ( miss_brake_log++ % 16 ) == 0 )
				botox_dbg_log( "[fr] brake miss h=%.0f steer=%d drop=%.0f v=%.0f", drift_flat.length_2d( ), m_fireman_data.steer_ticks, drop_left, fall_speed );

			return;
		}
	}

	m_fireman_data.awall = false;

	if ( drop_left < 0.f || drop_left > grab_at )
		return;

	trace_t trace;
	float wall_yaw   = 0.f;
	c_vector wall_normal{ };
	const float step = std::numbers::pi_v< float > * 2.0f / 16.f;

	for ( int i = 0; i < 16; ++i ) {
		const float a = step * static_cast< float >( i );
		const c_vector wishdir( cosf( a ), sinf( a ), 0.f );
		c_trace_filter flt( g_ctx.m_local );
		ray_t ray( origin, origin + wishdir, mins, maxs );

		g_interfaces.m_engine_trace->trace_ray( ray, mask_playersolid, &flt, &trace );

		if ( trace.m_start_solid || trace.m_all_solid )
			continue;
		if ( trace.m_plane.m_normal.m_x == 0.f && trace.m_plane.m_normal.m_y == 0.f )
			continue;

		if ( trace.m_fraction < 1.f && fireman_face_nz_ok( trace.m_plane.m_normal.m_z, 0.f ) && side_ok( trace.m_plane.m_normal ) &&
		     contact_ok( trace.m_plane.m_normal ) ) {
			const c_vector normal_plane( -trace.m_plane.m_normal.m_x, -trace.m_plane.m_normal.m_y, 0.f );
			wall_yaw             = normal_plane.to_angle( ).m_y;
			wall_normal          = trace.m_plane.m_normal;
			m_fireman_data.awall = true;
			break;
		}
	}

	if ( !m_fireman_data.awall )
		return;

	const float backup_forward_move = cmd->m_forward_move;
	const float backup_side_move    = cmd->m_side_move;
	const int backup_buttons        = cmd->m_buttons;
	const c_angle backup_view       = cmd->m_view_point;

	cmd->m_buttons |= in_jump;
	cmd->m_buttons &= ~move_buttons;

	const int predicted_frame = g_interfaces.m_prediction->m_commands_predicted - 1;
	const auto sim_grabs      = [ & ]( ) {
		g_prediction.restore_entity_to_predicted_frame( predicted_frame );
		g_prediction.begin( g_ctx.m_local, cmd );
		g_prediction.end( g_ctx.m_local );
		const bool ladder = g_ctx.m_local->get_move_type( ) == move_type_ladder;
		g_prediction.restore_entity_to_predicted_frame( predicted_frame );
		return ladder;
	};

	bool grabbed = fireman_live_press( cmd, wall_yaw, 450.f, wall_normal ) && sim_grabs( );
	if ( !grabbed ) {
		cmd->m_view_point   = c_angle( 0.f, wall_yaw, 0.f );
		cmd->m_forward_move = 450.f;
		cmd->m_side_move    = 0.f;
		grabbed             = sim_grabs( );
	}

	if ( !grabbed ) {
		cmd->m_forward_move = backup_forward_move;
		cmd->m_side_move    = backup_side_move;
		cmd->m_buttons      = backup_buttons;
		cmd->m_view_point   = backup_view;
		return;
	}

	if ( !fireman_ride_next_sticks( origin, mins, maxs, g_prediction.backup_data.m_velocity, wall_normal ) )
		cmd->m_buttons &= ~in_jump;

	m_fireman_data.ladder_yaw = wall_yaw;
	m_fireman_data.yaw_valid  = true;
	m_fireman_data.owns_cmd   = true;
	m_fireman_data.is_ladder  = true;
	lock_earned( );
	m_fireman_data.grab_tick    = cmd->m_tick_count;
	m_fireman_data.press_tick   = cmd->m_tick_count;
	m_fireman_data.press_by     = "sim";
	m_fireman_data.press_yaw    = wall_yaw;
	m_fireman_data.press_origin = origin;
}
