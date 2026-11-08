#include "movement.h"
#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"
#include "../../dependencies/imgui/imgui.h"
#include "../visuals/screen/render_queue.h"
#include "../visuals/screen/stream_guard.h"
#include <algorithm>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

extern void botox_dbg_log( const char* fmt, ... );

constexpr float k_dist_calc_snap = 50.f;

enum e_dc_probe { dc_probe_floor = 0, dc_probe_drop, dc_probe_wall };

static int g_dc_traces = 0;

/* floor within 2u of at.z, a drop ( none / steep / step ), or inside a wall ( a wall is never an edge ) */
static e_dc_probe dc_probe( const c_vector& at, float& z_out )
{
	++g_dc_traces;
	trace_t tr;
	ray_t ray( c_vector( at.m_x, at.m_y, at.m_z + 4.f ), c_vector( at.m_x, at.m_y, at.m_z - 6.f ) );
	c_trace_filter fil( g_ctx.m_local );
	g_interfaces.m_engine_trace->trace_ray( ray, mask_playersolid, &fil, &tr );
	if ( tr.m_start_solid )
		return dc_probe_wall;
	if ( !tr.did_hit( ) || tr.m_plane.m_normal.m_z < 0.7f || std::fabsf( tr.m_end.m_z - at.m_z ) > 2.f )
		return dc_probe_drop;
	z_out = tr.m_end.m_z;
	return dc_probe_floor;
}

static float dc_edge_dist( const c_vector& p, const c_vector& dir, const float limit )
{
	float z = p.m_z, lo = 0.f, hi = -1.f;
	for ( float s = 2.f;; s += 2.f ) {
		s                     = std::min( s, limit );
		const e_dc_probe kind = dc_probe( p + dir * s, z );
		if ( kind == dc_probe_wall )
			return -1.f;
		if ( kind == dc_probe_drop ) {
			hi = s;
			break;
		}
		lo = s;
		if ( s >= limit )
			return -1.f;
	}
	for ( int i = 0; i < 7; i++ ) {
		const float mid = 0.5f * ( lo + hi );
		( dc_probe( p + dir * mid, z ) == dc_probe_floor ? lo : hi ) = mid;
	}
	return lo;
}

c_vector movement_nearest_edge( const c_vector& p, const float reach, const c_vector& facing, c_vector* normal_out )
{
	float z = p.m_z;
	if ( !g_ctx.m_local || dc_probe( p, z ) != dc_probe_floor )
		return p;

	const bool face    = facing.length_2d( ) > 0.5f;
	const auto dir_of  = []( const float yaw ) { return c_vector( std::cosf( yaw ), std::sinf( yaw ), 0.f ); };
	constexpr int dirs = 48;
	constexpr float step = 6.2831853f / dirs;

	float best = -1.f, best_yaw = 0.f, limit = reach;
	for ( int i = 0; i < dirs; i++ ) {
		const float yaw = i * step;
		const c_vector d = dir_of( yaw );
		if ( face && d.m_x * facing.m_x + d.m_y * facing.m_y <= 0.f )
			continue;
		const float dist = dc_edge_dist( p, d, limit );
		if ( dist >= 0.f && ( best < 0.f || dist < best ) ) {
			best     = dist;
			best_yaw = yaw;
			limit    = dist + 2.f;
		}
	}
	if ( best < 0.f )
		return p;

	const auto dist_at = [ & ]( const float yaw ) {
		const float dist = dc_edge_dist( p, dir_of( yaw ), limit );
		return dist < 0.f ? FLT_MAX : dist;
	};
	float lo = best_yaw - step, hi = best_yaw + step;
	for ( int i = 0; i < 10; i++ ) {
		const float m1 = lo + ( hi - lo ) / 3.f, m2 = hi - ( hi - lo ) / 3.f;
		if ( dist_at( m1 ) < dist_at( m2 ) )
			hi = m2;
		else
			lo = m1;
	}
	if ( const float yaw = 0.5f * ( lo + hi ), dist = dist_at( yaw ); dist <= best ) {
		best     = dist;
		best_yaw = yaw;
	}

	const c_vector dir = dir_of( best_yaw );
	c_vector edge      = p + dir * best;
	if ( dc_probe( edge, z ) == dc_probe_floor )
		edge.m_z = z;
	if ( normal_out )
		*normal_out = dir;
	return edge;
}

void n_movement::impl_t::dist_calc( c_user_cmd* cmd )
{
	auto& data        = m_dist_calc_data;
	const char* level = g_interfaces.m_engine_client->get_level_name_short( );
	if ( !level )
		return;
	if ( !data.map.empty( ) && data.map != level )
		data = { };

	static bool prev = false;
	const bool down  = GET_VARIABLE( g_variables.m_dist_calc, bool ) && g_input.check_input( &GET_VARIABLE( g_variables.m_dist_calc_key, key_bind_t ) );
	const bool press = down && !prev;
	prev             = down;
	if ( !press || !cmd || !g_ctx.m_local || !g_ctx.m_local->is_alive( ) )
		return;

	if ( data.count == 2 ) {
		data = { };
		movement_add_window( 3, "distance: cleared" );
		return;
	}

	c_angle cam{ };
	g_interfaces.m_engine_client->get_view_angles( cam );
	const c_vector eye     = g_ctx.m_local->get_eye_position( );
	const c_vector forward = c_vector::fromAngle( c_vector( cam.m_x, cam.m_y, 0.f ) );
	trace_t tr;
	ray_t ray( eye, eye + forward * 8192.f );
	c_trace_filter fil( g_ctx.m_local );
	g_interfaces.m_engine_trace->trace_ray( ray, mask_playersolid, &fil, &tr );
	if ( !tr.did_hit( ) || tr.m_plane.m_normal.m_z < 0.7f ) {
		movement_add_window( 3, "distance: aim at a floor" );
		return;
	}

	data.map          = level;
	g_dc_traces       = 0;
	const auto t_snap = std::chrono::steady_clock::now( );
	const auto log_cost = [ & ]( const char* pt ) {
		botox_dbg_log( "DC: %s traces=%d ms=%.2f\n", pt, g_dc_traces,
		               std::chrono::duration< double, std::milli >( std::chrono::steady_clock::now( ) - t_snap ).count( ) );
	};
	if ( data.count == 0 ) {
		data.a_aim = tr.m_end;
		data.a     = movement_nearest_edge( tr.m_end, k_dist_calc_snap );
		log_cost( "A" );
		data.count = 1;
		movement_add_window( 3, "distance: point A set, aim at the landing and press again" );
		return;
	}

	c_vector dir( tr.m_end.m_x - data.a_aim.m_x, tr.m_end.m_y - data.a_aim.m_y, 0.f );
	const float len = dir.length_2d( );
	if ( len < 1.f ) {
		movement_add_window( 3, "distance: B is on top of A" );
		return;
	}
	dir /= len;

	const auto snap = [ & ]( const c_vector& aim, const c_vector& facing, c_vector& normal ) {
		const c_vector edge = movement_nearest_edge( aim, k_dist_calc_snap, facing, &normal );
		return edge != aim ? edge : movement_nearest_edge( aim, k_dist_calc_snap, { }, &normal );
	};
	c_vector na = dir, nb = dir * -1.f;
	data.a      = snap( data.a_aim, dir, na );
	data.b      = snap( tr.m_end, dir * -1.f, nb );

	if ( na.m_x * nb.m_x + na.m_y * nb.m_y < -0.95f ) {
		const c_vector tangent( -nb.m_y, nb.m_x, 0.f );
		const float along = ( data.b.m_x - data.a.m_x ) * tangent.m_x + ( data.b.m_y - data.a.m_y ) * tangent.m_y;
		c_vector square   = data.b - tangent * along;
		square.m_z        = data.b.m_z;
		float z           = 0.f;
		if ( dc_probe( square - nb * 0.5f, z ) == dc_probe_floor )
			data.b = square;
	}
	log_cost( "B" );
	data.count  = 2;
	data.block  = c_vector( data.b.m_x - data.a.m_x, data.b.m_y - data.a.m_y, 0.f ).length_2d( );
	data.dz     = data.b.m_z - data.a.m_z;
	const float max_speed = g_ctx.m_local->get_max_speed( ) > 1.f ? g_ctx.m_local->get_max_speed( ) : 250.f;
	data.lj_max           = n_route::max_block( false, data.dz, max_speed );
	data.jb_max           = n_route::max_block( true, data.dz, max_speed );

	char buf[ 160 ]{ };
	sprintf_s( buf, "distance: %.1f block, dz %+.1f | lj %s | jb %s", data.block, data.dz,
	           data.lj_max < 0.f ? "too high" : data.block <= data.lj_max ? "yes" : "no", data.jb_max < 0.f ? "too high" : data.block <= data.jb_max ? "yes" : "no" );
	movement_add_window( 3, buf );
}

void n_movement::impl_t::dist_calc_render( )
{
	const auto& data = m_dist_calc_data;
	if ( !GET_VARIABLE( g_variables.m_dist_calc, bool ) || data.count == 0 || !g_ctx.m_local )
		return;
	auto* font = g_render.m_fonts[ e_font_names::font_name_verdana_11 ];
	if ( !font )
		return;

	const c_color accent = GET_VARIABLE( g_variables.m_accent, c_color );
	const unsigned int accent_u32 =
		c_color( static_cast< int >( accent.get< 0 >( ) ), static_cast< int >( accent.get< 1 >( ) ), static_cast< int >( accent.get< 2 >( ) ), 255 ).get_u32( );
	const unsigned int white = c_color( 255, 255, 255, 255 ).get_u32( );
	const unsigned int grey  = c_color( 128, 128, 128, 255 ).get_u32( );

	const auto add_text = [ & ]( float x, float y, const char* str, unsigned int color ) {
		text_draw_object_t text;
		text.m_font       = font;
		text.m_position   = c_vector_2d( x, y );
		text.m_text       = str;
		text.m_color      = color;
		text.m_draw_flags = text_flag_dropshadow;
		g_render.m_draw_data.emplace_back( e_draw_type::draw_type_text, std::make_any< text_draw_object_t >( text ) );
	};
	const auto text_w = [ & ]( const char* str ) { return font->CalcTextSizeA( font->FontSize, FLT_MAX, 0.f, str ).x; };

	c_vector_2d sa{ }, sb{ };
	const bool on_a = g_render.world_to_screen( data.a, sa );
	const bool on_b = data.count == 2 && g_render.world_to_screen( data.b, sb );

	if ( on_a && on_b ) {
		/* clip to screen: point near camera plane w2s = 1e6+ px, unclipped dashes ate RAM and crashed */
		const ImVec2 view = ImGui::GetIO( ).DisplaySize;
		const float dx = sb.m_x - sa.m_x, dy = sb.m_y - sa.m_y;
		const float p[ 4 ] = { -dx, dx, -dy, dy }, q[ 4 ] = { sa.m_x, view.x - sa.m_x, sa.m_y, view.y - sa.m_y };
		float t0 = 0.f, t1 = 1.f;
		for ( int i = 0; i < 4; i++ ) {
			if ( p[ i ] == 0.f ) {
				if ( q[ i ] < 0.f )
					t1 = -1.f;
			} else if ( p[ i ] < 0.f )
				t0 = std::max( t0, q[ i ] / p[ i ] );
			else
				t1 = std::min( t1, q[ i ] / p[ i ] );
		}
		const float l = sa.distance( sb );
		if ( t0 < t1 && l > 0.f ) {
			g_render.m_draw_data.emplace_back( e_draw_type::draw_type_line, std::make_any< line_draw_object_t >( sa + ( sb - sa ) * t0, sa + ( sb - sa ) * t1,
			                                                                                                    c_color( 0.f, 0.f, 0.f, 0.5f ).get_u32( ), 3.f ) );
			constexpr float s = 10.f;
			static float offset = 0.f;
			offset              = std::fmod( offset + 50.f * ImGui::GetIO( ).DeltaTime, 2.f * s );
			const float lo = t0 * l, hi = t1 * l;
			for ( long long m = static_cast< long long >( ( lo + offset ) / s );; m++ ) {
				const float d0 = std::max( m * s - offset, lo ), d1 = std::min( ( m + 1 ) * s - offset, hi );
				if ( d0 >= hi )
					break;
				if ( d1 > d0 )
					g_render.m_draw_data.emplace_back( e_draw_type::draw_type_line,
					                                   std::make_any< line_draw_object_t >( sa + ( sb - sa ) * ( d0 / l ), sa + ( sb - sa ) * ( d1 / l ),
					                                                                        m % 2 == 0 ? accent_u32 : grey, 1.f ) );
			}
		}
	}

	const auto marker = [ & ]( const c_vector_2d& p, const char* letter, const char* label ) {
		constexpr float radius = 11.f;
		filled_circle_draw_object_t filled;
		filled.m_center = p;
		filled.m_radius = radius;
		filled.m_color  = c_color( 0, 0, 0, 255 ).get_u32( );
		g_render.m_draw_data.emplace_back( e_draw_type::draw_type_filled_circle, std::make_any< filled_circle_draw_object_t >( filled ) );
		circle_draw_object_t outline;
		outline.m_center = p;
		outline.m_radius = radius;
		outline.m_color  = accent_u32;
		g_render.m_draw_data.emplace_back( e_draw_type::draw_type_circle, std::make_any< circle_draw_object_t >( outline ) );
		add_text( p.m_x - text_w( letter ) * 0.5f, p.m_y - font->FontSize * 0.5f - 1.f, letter, accent_u32 );
		add_text( p.m_x - text_w( label ) * 0.5f, p.m_y - radius - font->FontSize - 3.f, label, white );
	};
	if ( on_a )
		marker( sa, "A", "takeoff" );
	if ( data.count < 2 )
		return;
	if ( on_b )
		marker( sb, "B", "landing" );

	c_vector_2d mid{ };
	if ( !g_render.world_to_screen( ( data.a + data.b ) * 0.5f, mid ) )
		return;
	char lines[ 3 ][ 96 ]{ };
	sprintf_s( lines[ 0 ], "%.1f block   dz %+.1f", data.block, data.dz );
	const auto verdict = [ & ]( char* out, const char* name, const float max ) {
		if ( max < 0.f )
			sprintf_s( out, 96, "%s: no ( too high )", name );
		else
			sprintf_s( out, 96, "%.1f %s: %s ( max %.1f )", data.block, name, data.block <= max ? "yes" : "no", max );
	};
	verdict( lines[ 1 ], "lj", data.lj_max );
	verdict( lines[ 2 ], "jb", data.jb_max );
	for ( int i = 0; i < 3; i++ )
		add_text( mid.m_x - text_w( lines[ i ] ) * 0.5f, mid.m_y + 8.f + i * ( font->FontSize + 2.f ), lines[ i ],
		          i == 0 ? white : std::strstr( lines[ i ], ": yes" ) ? accent_u32 : grey );
}

/* ---- floor level ---- */

constexpr int k_floor_level_n = 49;

static std::atomic< bool > s_floor_level_click{ false }, s_floor_level_shown{ false };

void floor_level_toggle( )
{
	s_floor_level_click = true;
}

bool floor_level_shown( )
{
	return s_floor_level_shown;
}

static std::string floor_level_readout( const float lo, const float hi, const float mean )
{
	char buf[ 96 ]{ };
	sprintf_s( buf, "lowest: %.3f, highest: %.3f, mean: %.3f", lo, hi, mean );
	return buf;
}

void n_movement::impl_t::floor_level( c_user_cmd* cmd )
{
	auto& data        = m_floor_level_data;
	const char* level = g_interfaces.m_engine_client->get_level_name_short( );
	if ( !level )
		return;
	if ( !data.map.empty( ) && data.map != level )
		data = { };
	s_floor_level_shown = data.on;

	static bool prev = false;
	const bool down  = GET_VARIABLE( g_variables.m_floor_level, bool ) && g_input.check_input( &GET_VARIABLE( g_variables.m_floor_level_key, key_bind_t ) );
	const bool press = ( down && !prev ) | s_floor_level_click.exchange( false );
	prev             = down;
	if ( !press || !cmd || !g_ctx.m_local || !g_ctx.m_local->is_alive( ) )
		return;

	if ( data.on ) {
		data                = { };
		s_floor_level_shown = false;
		return;
	}

	c_angle cam{ };
	g_interfaces.m_engine_client->get_view_angles( cam );
	const c_vector eye     = g_ctx.m_local->get_eye_position( );
	const c_vector forward = c_vector::fromAngle( c_vector( cam.m_x, cam.m_y, 0.f ) );
	c_trace_filter fil( g_ctx.m_local );
	trace_t tr;
	ray_t ray( eye, eye + forward * 8192.f );
	g_interfaces.m_engine_trace->trace_ray( ray, mask_playersolid, &fil, &tr );
	if ( !tr.did_hit( ) || tr.m_plane.m_normal.m_z < 0.7f ) {
		movement_add_window( 3, "floor level: aim at a floor" );
		return;
	}

	constexpr int n    = k_floor_level_n;
	const float radius = std::clamp( GET_VARIABLE( g_variables.m_floor_level_size, float ), 16.f, 512.f );
	const float step   = 2.f * radius / ( n - 1 );
	const float x0 = tr.m_end.m_x - radius, y0 = tr.m_end.m_y - radius;
	std::vector< float > zs( n * n, NAN );
	data.center = tr.m_end;
	data.lo     = FLT_MAX;
	data.hi     = -FLT_MAX;
	double sum  = 0.0;
	int count   = 0;
	for ( int i = 0; i < n; i++ ) {
		for ( int j = 0; j < n; j++ ) {
			const float x = x0 + j * step, y = y0 + i * step;
			trace_t t;
			/* point rays slip through brush seams and hit the void below, a tiny hull does not */
			ray_t down_ray( c_vector( x, y, tr.m_end.m_z + 64.f ), c_vector( x, y, tr.m_end.m_z - 64.f ), c_vector( -0.25f, -0.25f, 0.f ),
			                c_vector( 0.25f, 0.25f, 0.f ) );
			g_interfaces.m_engine_trace->trace_ray( down_ray, mask_playersolid, &fil, &t );
			if ( t.m_start_solid || !t.did_hit( ) || t.m_plane.m_normal.m_z < 0.7f )
				continue;
			const float z     = t.m_end.m_z;
			zs[ i * n + j ] = z;
			data.lo         = std::min( data.lo, z );
			data.hi         = std::max( data.hi, z );
			sum += z;
			count++;
		}
	}
	if ( !count ) {
		data = { };
		movement_add_window( 3, "floor level: no floor under the aim" );
		return;
	}
	data.mean = static_cast< float >( sum / count );

	/* low red -> mid blue -> high green */
	const float span = data.hi - data.lo;
	const auto color = [ & ]( const float z ) {
		constexpr float k_stops[ 3 ][ 3 ] = { { 255.f, 40.f, 40.f }, { 40.f, 90.f, 255.f }, { 40.f, 230.f, 60.f } };
		const float t  = span < 0.01f ? 0.5f : ( z - data.lo ) / span;
		const int s    = t < 0.5f ? 0 : 1;
		const float a  = t < 0.5f ? t * 2.f : t * 2.f - 1.f;
		const auto mix = [ & ]( const int c ) { return static_cast< unsigned long >( k_stops[ s ][ c ] + ( k_stops[ s + 1 ][ c ] - k_stops[ s ][ c ] ) * a ); };
		return ( 170ul << 24 ) | ( mix( 0 ) << 16 ) | ( mix( 1 ) << 8 ) | mix( 2 );
	};
	auto mesh = std::make_shared< std::vector< floor_level_vertex_t > >( );
	mesh->reserve( ( n - 1 ) * ( n - 1 ) * 6 );
	for ( int i = 0; i + 1 < n; i++ ) {
		for ( int j = 0; j + 1 < n; j++ ) {
			const int idx[ 4 ] = { i * n + j, i * n + j + 1, ( i + 1 ) * n + j + 1, ( i + 1 ) * n + j };
			if ( std::isnan( zs[ idx[ 0 ] ] ) || std::isnan( zs[ idx[ 1 ] ] ) || std::isnan( zs[ idx[ 2 ] ] ) || std::isnan( zs[ idx[ 3 ] ] ) )
				continue;
			const auto [ lo, hi ] = std::minmax( { zs[ idx[ 0 ] ], zs[ idx[ 1 ] ], zs[ idx[ 2 ] ], zs[ idx[ 3 ] ] } );
			/* steeper than walkable = wall or step edge, leave a gap instead of a vertical sheet */
			if ( hi - lo > 2.f * step )
				continue;
			/* 0.25u lift: flush with the floor it z-fights */
			for ( const int k : { idx[ 0 ], idx[ 1 ], idx[ 2 ], idx[ 0 ], idx[ 2 ], idx[ 3 ] } )
				mesh->push_back( { x0 + ( k % n ) * step, y0 + ( k / n ) * step, zs[ k ] + 0.25f, color( zs[ k ] ) } );
		}
	}
	data.mesh           = std::move( mesh );
	data.on             = true;
	data.map            = level;
	s_floor_level_shown = true;
	movement_add_window( 3, floor_level_readout( data.lo, data.hi, data.mean ) );
}

void n_movement::impl_t::floor_level_render( )
{
	const auto& data = m_floor_level_data;
	if ( !GET_VARIABLE( g_variables.m_floor_level, bool ) || !data.on || !g_ctx.m_local )
		return;

	auto* font = g_render.m_fonts[ e_font_names::font_name_verdana_11 ];
	c_vector_2d mid{ };
	if ( !font || !g_render.world_to_screen( data.center, mid ) )
		return;
	text_draw_object_t text;
	text.m_text       = floor_level_readout( data.lo, data.hi, data.mean );
	text.m_font       = font;
	text.m_position   = c_vector_2d( mid.m_x - font->CalcTextSizeA( font->FontSize, FLT_MAX, 0.f, text.m_text.c_str( ) ).x * 0.5f, mid.m_y + 8.f );
	text.m_color      = c_color( 255, 255, 255, 255 ).get_u32( );
	text.m_draw_flags = text_flag_dropshadow;
	g_render.m_draw_data.emplace_back( e_draw_type::draw_type_text, std::make_any< text_draw_object_t >( text ) );
}

using floor_level_mesh_t = std::vector< n_movement::impl_t::floor_level_vertex_t >;
static_assert( sizeof( n_movement::impl_t::floor_level_vertex_t ) == 16, "D3DFVF_XYZ | D3DFVF_DIFFUSE layout" );

/* render thread, fixed function like web on surface: scene depth from DoPostScreenSpaceEffects hides it behind walls / viewmodel */
static void floor_level_draw( const floor_level_mesh_t& mesh, const D3DMATRIX& proj )
{
	IDirect3DDevice9* device = g_interfaces.m_direct_device;
	if ( !device || mesh.empty( ) )
		return;

	static constexpr D3DRENDERSTATETYPE k_states[] = { D3DRS_ZENABLE,         D3DRS_ZWRITEENABLE,     D3DRS_ZFUNC,           D3DRS_ALPHABLENDENABLE,
	                                                   D3DRS_SRCBLEND,        D3DRS_DESTBLEND,        D3DRS_ALPHATESTENABLE, D3DRS_CULLMODE,
	                                                   D3DRS_LIGHTING,        D3DRS_FOGENABLE,        D3DRS_STENCILENABLE,   D3DRS_COLORWRITEENABLE,
	                                                   D3DRS_SRGBWRITEENABLE, D3DRS_SCISSORTESTENABLE, D3DRS_CLIPPLANEENABLE, D3DRS_FILLMODE,
	                                                   D3DRS_SEPARATEALPHABLENDENABLE };
	static constexpr D3DTEXTURESTAGESTATETYPE k_stages[] = { D3DTSS_COLOROP, D3DTSS_COLORARG1, D3DTSS_ALPHAOP, D3DTSS_ALPHAARG1 };
	DWORD states[ std::size( k_states ) ]{ }, stages[ std::size( k_stages ) ]{ }, stage1[ 2 ]{ }, fvf = 0;
	D3DMATRIX world{ }, view{ }, old_proj{ };
	IDirect3DVertexShader9* vs        = nullptr;
	IDirect3DPixelShader9* ps         = nullptr;
	IDirect3DVertexDeclaration9* decl = nullptr;
	IDirect3DBaseTexture9* texture    = nullptr;
	n_stream_guard::state_t streams{ };

	for ( std::size_t i = 0; i < std::size( k_states ); i++ )
		device->GetRenderState( k_states[ i ], &states[ i ] );
	for ( std::size_t i = 0; i < std::size( k_stages ); i++ )
		device->GetTextureStageState( 0, k_stages[ i ], &stages[ i ] );
	device->GetTextureStageState( 1, D3DTSS_COLOROP, &stage1[ 0 ] );
	device->GetTextureStageState( 1, D3DTSS_ALPHAOP, &stage1[ 1 ] );
	device->GetTransform( D3DTS_WORLD, &world );
	device->GetTransform( D3DTS_VIEW, &view );
	device->GetTransform( D3DTS_PROJECTION, &old_proj );
	device->GetVertexShader( &vs );
	device->GetPixelShader( &ps );
	device->GetVertexDeclaration( &decl );
	device->GetFVF( &fvf );
	device->GetTexture( 0, &texture );
	streams.capture( device );

	static const D3DMATRIX k_identity = { 1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f };
	device->SetVertexShader( nullptr );
	device->SetPixelShader( nullptr );
	device->SetFVF( D3DFVF_XYZ | D3DFVF_DIFFUSE );
	device->SetTexture( 0, nullptr );
	device->SetTransform( D3DTS_WORLD, &k_identity );
	device->SetTransform( D3DTS_VIEW, &k_identity );
	device->SetTransform( D3DTS_PROJECTION, &proj );
	device->SetRenderState( D3DRS_ZENABLE, D3DZB_TRUE );
	device->SetRenderState( D3DRS_ZWRITEENABLE, FALSE );
	device->SetRenderState( D3DRS_ZFUNC, D3DCMP_LESSEQUAL );
	device->SetRenderState( D3DRS_ALPHABLENDENABLE, TRUE );
	device->SetRenderState( D3DRS_SRCBLEND, D3DBLEND_SRCALPHA );
	device->SetRenderState( D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA );
	device->SetRenderState( D3DRS_ALPHATESTENABLE, FALSE );
	device->SetRenderState( D3DRS_CULLMODE, D3DCULL_NONE );
	device->SetRenderState( D3DRS_LIGHTING, FALSE );
	device->SetRenderState( D3DRS_FOGENABLE, FALSE );
	device->SetRenderState( D3DRS_STENCILENABLE, FALSE );
	device->SetRenderState( D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE );
	device->SetRenderState( D3DRS_SRGBWRITEENABLE, FALSE );
	device->SetRenderState( D3DRS_SCISSORTESTENABLE, FALSE );
	device->SetRenderState( D3DRS_CLIPPLANEENABLE, 0 );
	device->SetRenderState( D3DRS_FILLMODE, D3DFILL_SOLID );
	device->SetRenderState( D3DRS_SEPARATEALPHABLENDENABLE, FALSE );
	device->SetTextureStageState( 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1 );
	device->SetTextureStageState( 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE );
	device->SetTextureStageState( 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1 );
	device->SetTextureStageState( 0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE );
	device->SetTextureStageState( 1, D3DTSS_COLOROP, D3DTOP_DISABLE );
	device->SetTextureStageState( 1, D3DTSS_ALPHAOP, D3DTOP_DISABLE );

	device->DrawPrimitiveUP( D3DPT_TRIANGLELIST, static_cast< UINT >( mesh.size( ) / 3 ), mesh.data( ), sizeof( mesh[ 0 ] ) );

	for ( std::size_t i = 0; i < std::size( k_states ); i++ )
		device->SetRenderState( k_states[ i ], states[ i ] );
	for ( std::size_t i = 0; i < std::size( k_stages ); i++ )
		device->SetTextureStageState( 0, k_stages[ i ], stages[ i ] );
	device->SetTextureStageState( 1, D3DTSS_COLOROP, stage1[ 0 ] );
	device->SetTextureStageState( 1, D3DTSS_ALPHAOP, stage1[ 1 ] );
	device->SetTransform( D3DTS_WORLD, &world );
	device->SetTransform( D3DTS_VIEW, &view );
	device->SetTransform( D3DTS_PROJECTION, &old_proj );
	device->SetTexture( 0, texture );
	device->SetVertexShader( vs );
	device->SetPixelShader( ps );
	if ( decl )
		device->SetVertexDeclaration( decl );
	else
		device->SetFVF( fvf );
	streams.restore( device );
	for ( IUnknown* held : std::initializer_list< IUnknown* >{ vs, ps, decl, texture } )
		if ( held )
			held->Release( );
}

/* DoPostScreenSpaceEffects ( main thread ): mesh is immutable once built, job owns a ref */
void floor_level_world( )
{
	const auto& data = g_movement.m_floor_level_data;
	if ( !GET_VARIABLE( g_variables.m_floor_level, bool ) || !data.on || !data.mesh || data.mesh->empty( ) || !g_interfaces.m_engine_client )
		return;

	const view_matrix_t& m = g_interfaces.m_engine_client->get_world_to_screen_matrix( );
	D3DMATRIX proj{ };
	for ( int r = 0; r < 4; r++ )
		for ( int c = 0; c < 4; c++ )
			proj.m[ r ][ c ] = m.data[ c ][ r ];

	auto job = [ mesh = data.mesh, proj ] { floor_level_draw( *mesh, proj ); };
	if ( !n_render_queue::submit( job ) )
		job( );
}
