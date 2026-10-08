#include "movement.h"
#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"
#include "../../dependencies/imgui/imgui.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

extern void botox_dbg_log( const char* fmt, ... );

void n_movement::impl_t::pixel_calc_render( )
{
	if ( !g_ctx.m_local || !g_ctx.m_local->is_alive( ) )
		return;
	if ( !GET_VARIABLE( g_variables.m_pixel_calc, bool ) || !GET_VARIABLE( g_variables.m_pixel_calc_show_point, bool ) )
		return;

	auto& data = m_pixel_calc_data;

	/* map changed = stale world point, drop it */
	if ( !data.map.empty( ) && data.map != g_interfaces.m_engine_client->get_level_name_short( ) )
		data.clear( );

	if ( data.point_vec.is_zero( ) )
		return;

	c_vector_2d screen_pos;
	if ( !g_render.world_to_screen( data.point_vec, screen_pos ) )
		return;

	auto accent  = GET_VARIABLE( g_variables.m_accent, c_color );
	float radius = 12.f;

	filled_circle_draw_object_t filled;
	filled.m_center = screen_pos;
	filled.m_radius = radius;
	filled.m_color  = c_color( 0, 0, 0, 255 ).get_u32( );
	g_render.m_draw_data.emplace_back( e_draw_type::draw_type_filled_circle, std::make_any< filled_circle_draw_object_t >( filled ) );

	circle_draw_object_t outline;
	outline.m_center = screen_pos;
	outline.m_radius = radius;
	outline.m_color =
		c_color( static_cast< int >( accent.get< 0 >( ) ), static_cast< int >( accent.get< 1 >( ) ), static_cast< int >( accent.get< 2 >( ) ), 255 )
			.get_u32( );
	g_render.m_draw_data.emplace_back( e_draw_type::draw_type_circle, std::make_any< circle_draw_object_t >( outline ) );

	auto* font = g_render.m_fonts[ e_font_names::font_name_verdana_11 ];
	if ( font ) {
		const char* label = "px";
		ImVec2 text_size  = font->CalcTextSizeA( font->FontSize, FLT_MAX, 0.0f, label );
		text_draw_object_t text;
		text.m_position = c_vector_2d( screen_pos.m_x - text_size.x * 0.5f, screen_pos.m_y - text_size.y * 0.5f - 1.f );
		text.m_color    = c_color( static_cast< int >( accent.get< 0 >( ) ), static_cast< int >( accent.get< 1 >( ) ),
		                           static_cast< int >( accent.get< 2 >( ) ), 255 )
		                   .get_u32( );
		text.m_text = label;
		text.m_font = font;
		g_render.m_draw_data.emplace_back( e_draw_type::draw_type_text, std::make_any< text_draw_object_t >( text ) );
	}
}

void n_movement::impl_t::pixel_calc( c_user_cmd* cmd )
{
	if ( !g_ctx.m_local || !g_ctx.m_local->is_alive( ) )
		return;
	if ( !GET_VARIABLE( g_variables.m_pixel_calc, bool ) )
		return;

	auto& data = m_pixel_calc_data;

	/* same stale-point guard as pixel_calc_render, create_move can run before the next paint */
	if ( !data.map.empty( ) && data.map != g_interfaces.m_engine_client->get_level_name_short( ) )
		data.clear( );

	const bool advanced = GET_VARIABLE( g_variables.m_pixel_calc_advanced_readout, bool );
	const auto added    = [ & ]( float z ) {
		char buf[ 64 ]{ };
		sprintf_s( buf, "added %s point at %.2f", GET_VARIABLE( g_variables.m_pixel_calc_type, int ) == 1 ? "pixeljump" : "pixelsurf", z );
		movement_add_window( 3, buf );
	};

	if ( cmd ) {
		static bool set_point_prev = false;
		bool set_point_pressed     = g_input.check_input( &GET_VARIABLE( g_variables.m_pixel_calc_aim_key, key_bind_t ) );
		if ( set_point_pressed && !set_point_prev ) {
			c_vector eye = g_ctx.m_local->get_eye_position( );
			c_angle cam{ };
			g_interfaces.m_engine_client->get_view_angles( cam );
			c_vector view_angle( cam.m_x, cam.m_y, 0.f );
			c_vector forward = c_vector::fromAngle( view_angle );

			float best_screen_dist = FLT_MAX;
			c_vector best_point{ };
			bool snap_found                = false;
			const float snap_screen_radius = 30.f;

			auto try_snap = [ & ]( const std::vector< AnimatedPoint >& points ) {
				for ( const auto& ap : points ) {
					if ( ap.is_removing )
						continue;
					c_vector_2d screen_pos;
					if ( !pf_dot_screen( ap, screen_pos ) )
						continue;
					float dx          = screen_pos.m_x - g_ctx.m_width * 0.5f;
					float dy          = screen_pos.m_y - g_ctx.m_height * 0.5f;
					float screen_dist = sqrtf( dx * dx + dy * dy );
					if ( screen_dist < snap_screen_radius && screen_dist < best_screen_dist ) {
						best_screen_dist = screen_dist;
						best_point       = ap.position;
						snap_found       = true;
					}
				}
			};

			try_snap( GET_VARIABLE( g_variables.m_pixel_calc_type, int ) == 1 ? animated_points4 : animated_points );

			if ( snap_found ) {
				data.point_vec      = best_point;
				data.normal_pos_vec = c_vector( 0, 0, 1 );
				data.snapped        = true;
				data.map            = g_interfaces.m_engine_client->get_level_name_short( );
				if ( advanced )
					movement_add_window( 3, "calc: snapped to finder point (z: " + std::to_string( best_point.m_z ) + ")" );
				else
					added( best_point.m_z );
			} else {
				const auto end_pos = eye + forward * 6000.f;
				trace_t trace;
				ray_t ray( eye, end_pos );
				c_trace_filter flt( g_ctx.m_local );
				g_interfaces.m_engine_trace->trace_ray( ray, mask_playersolid, &flt, &trace );
				if ( trace.m_start_solid || trace.m_all_solid ) {
					constexpr unsigned int mask_noclip =
						mask_playersolid & ~static_cast< unsigned int >( contents_playerclip );
					ray_t retry( eye, end_pos );
					c_trace_filter retry_flt( g_ctx.m_local );
					g_interfaces.m_engine_trace->trace_ray( retry, mask_noclip, &retry_flt, &trace );
				}
				c_vector hit_point  = eye + ( end_pos - eye ) * trace.m_fraction;
				data.point_vec      = hit_point;
				data.normal_pos_vec = trace.m_plane.m_normal;
				data.snapped        = false;
				data.map            = g_interfaces.m_engine_client->get_level_name_short( );
				if ( advanced )
					movement_add_window( 3, "calc: point set (z: " + std::to_string( hit_point.m_z ) + ")" );
				else
					added( hit_point.m_z );
			}
		}
		set_point_prev = set_point_pressed;
	}

	static bool calc_prev_pressed = false;
	bool calc_pressed             = g_input.check_input( &GET_VARIABLE( g_variables.m_pixel_calc_solve_key, key_bind_t ) );
	if ( !calc_pressed ) {
		calc_prev_pressed = false;
		return;
	}
	if ( calc_prev_pressed )
		return;
	calc_prev_pressed = true;

	auto& pv = data.point_vec;
	if ( pv.is_zero( ) || !cmd )
		return;

	/* every exit is the popup ( combos or one error line ). built locally, published in one assign:
	   end_scene reads data.rows on the d3d thread */
	std::vector< n_route::popup_row_t > rows;
	const auto publish = [ & ]( ) {
		data.rows          = std::move( rows );
		data.popup_started = GetTickCount64( ) / 1000.f;
	};
	const auto fail = [ & ]( const std::string& why ) {
		rows = { { why, n_route::popup_row_error } };
		publish( );
	};

	const unsigned int moves =
		static_cast< unsigned int >( GET_VARIABLE( g_variables.m_pixel_calc_moves, int ) ) & n_route::k_all_moves;
	if ( moves == 0u ) {
		fail( "no jumps allowed - tick some in pixel calc settings" );
		return;
	}
	/* shares the engine numbers ( impulse, travel ) with the route calc worker */
	if ( n_route::solve_busy( ) ) {
		fail( "route calculator is still calculating - try again when it's done" );
		return;
	}
	if ( !( g_ctx.m_local->get_flags( ) & fl_onground ) ) {
		fail( "stand on the ground to calculate" );
		return;
	}
	if ( !n_route::measure_launch( cmd ) ) {
		fail( "could not measure the jump - try again on flat ground" );
		return;
	}

	route_point_t pt;
	pt.pos         = pv;
	pt.wall_normal = data.normal_pos_vec;
	const bool pixeljump = GET_VARIABLE( g_variables.m_pixel_calc_type, int ) == 1;
	pt.type              = pixeljump ? route_pt_pixeljump : route_pt_pixelsurf;
	pt.snapped     = data.snapped;
	pt.quantized_z = n_route::target_z( pt );
	if ( pixeljump ) {
		float gap  = -1.f;
		pt.pj_skin = n_route::lip_kind( pt.pos, pt.quantized_z, gap ) == 1;
	}

	const c_vector origin = g_ctx.m_local->get_origin( );
	const int on_player = n_route::ground_player( );

	const float floor_nz = n_route::floor_nz( );
	const bool sloped    = floor_nz < n_route::k_flat_nz;
	std::string slope_note;
	if ( sloped ) {
		char buf[ 160 ]{ };
		sprintf_s( buf, "standing on a slope ( n.z %.3f ): hop combos skipped - landings slide here, stand on flat ground for them",
		           floor_nz );
		slope_note = buf;
		botox_dbg_log( "[px] %s\n", buf );
	}

	const int max_hops = sloped ? 0 : 3;
	const int shown    = std::clamp( GET_VARIABLE( g_variables.m_route_calc_max_results, int ), 1, 64 );
	float best_gap           = FLT_MAX;
	float closest_z          = 0.f;
	std::vector< n_route::solve_input_t > failed;
	unsigned int refused = 0u;

	for ( int hops = 0; hops <= max_hops; ++hops ) {
		n_route::solve_input_t in;
		for ( int i = 0; i < hops; ++i ) {
			route_point_t floor_pt;
			floor_pt.pos         = origin;
			floor_pt.type        = route_pt_ground;
			floor_pt.quantized_z = origin.m_z;
			floor_pt.ent         = on_player;
			in.points.emplace_back( floor_pt );
		}
		in.points.emplace_back( pt );
		in.start_z         = origin.m_z;
		in.start_on_ground = true;
		in.start_on_player = on_player > 0;
		in.start_styles = n_route::k_all_styles;
		in.global_moves = moves;
		in.delay_ticks  = GET_VARIABLE( g_variables.m_route_calc_delay_ticks, int );
		in.travel       = 0.f;
		in.max_results  = hops == 0 ? shown : ( std::max )( 2, shown / 2 );

		n_route::solve_output_t result;
		const auto clock_start = std::chrono::steady_clock::now( );
		n_route::solve( in, result );
		const double seconds = std::chrono::duration< double >( std::chrono::steady_clock::now( ) - clock_start ).count( );

		botox_dbg_log( "[px] %s z %9.4f ( %s%s ) +%d hops -> %lld routes in %.3f s, closest %.4f\n",
		                   pixeljump ? "pixeljump" : "pixelsurf", pt.quantized_z, data.snapped ? "snapped" : "aimed",
		                   pt.pj_skin ? ", skin" : "", hops, result.total, seconds,
		                   result.best_gap < 3.4e38f ? result.best_gap : -1.f );

		if ( result.routes.empty( ) ) {
			if ( result.failed_at == hops && result.best_gap < best_gap ) {
				best_gap  = result.best_gap;
				closest_z = result.closest_z;
			}
			refused |= result.refused_moves;
			failed.emplace_back( std::move( in ) );
			continue;
		}
		for ( const auto& s : result.routes )
			rows.push_back( { n_route::route_line( s ), n_route::popup_row_combo } );
	}

	if ( !rows.empty( ) ) {
		if ( sloped )
			rows.push_back( { slope_note, n_route::popup_row_note } );
		publish( );
	} else if ( !advanced ) {
		rows = { { "no solutions found", n_route::popup_row_error } };
		if ( sloped )
			rows.push_back( { slope_note, n_route::popup_row_note } );
		publish( );
	} else {
		char buf[ 160 ]{ };
		if ( best_gap < FLT_MAX )
			sprintf_s( buf, "no combo from here hits it - wants z %.4f, closest %.4f (off by %.3f)", pt.quantized_z,
			           closest_z, best_gap );
		else
			sprintf_s( buf, "no combo from here hits it - no arc got near z %.4f", pt.quantized_z );
		rows = { { buf, n_route::popup_row_error } };
		if ( sloped )
			rows.push_back( { slope_note, n_route::popup_row_note } );
		const auto hint_start = std::chrono::steady_clock::now( );
		for ( const auto& pass : failed ) {
			const int left = 250 - static_cast< int >( std::chrono::duration_cast< std::chrono::milliseconds >(
			                                               std::chrono::steady_clock::now( ) - hint_start ).count( ) );
			if ( left <= 0 )
				break;
			std::string example;
			if ( const std::string helps = n_route::solve_hint( pass, refused, example, left ); !helps.empty( ) ) {
				const std::string note = "with " + helps + " on ( off in pixel calc jumps ): " + example;
				rows.push_back( { note, n_route::popup_row_note } );
				botox_dbg_log( "[px] %s\n", note.c_str( ) );
				break;
			}
		}
		publish( );
	}
}

void n_movement::impl_t::pixel_calc_ui( )
{
	if ( !g_ctx.m_local || !g_ctx.m_local->is_alive( ) )
		return;
	if ( !GET_VARIABLE( g_variables.m_pixel_calc, bool ) )
		return;
	n_route::draw_popup( "pixel calculator", m_pixel_calc_data.rows, m_pixel_calc_data.popup_started );
}
