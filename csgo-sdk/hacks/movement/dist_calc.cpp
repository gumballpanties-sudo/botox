#include "movement.h"
#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"
#include "../../dependencies/imgui/imgui.h"
#include <algorithm>
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
