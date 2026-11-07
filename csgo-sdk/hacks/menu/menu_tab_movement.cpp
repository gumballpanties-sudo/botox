#include "menu_internal.h"
#include "../movement/movement_recorder.h"

static bool move_mask_combo( const char* label, int& stored )
{
	static const std::vector< const char* > names( std::begin( n_route::k_move_names ), std::end( n_route::k_move_names ) );
	std::vector< bool > flags( n_route::move_count, true );
	for ( int f = 0; f < n_route::move_count; ++f )
		flags[ f ] = ( static_cast< unsigned int >( stored ) & ( 1u << f ) ) != 0u;
	if ( !ImGui::MultiCombo( label, flags, names, n_route::move_count ) )
		return false;
	unsigned int built = 0u;
	for ( int f = 0; f < n_route::move_count; ++f )
		if ( flags[ f ] )
			built |= 1u << f;
	stored = static_cast< int >( built );
	return true;
}

static void route_calc_page( )
{
	std::lock_guard< std::recursive_mutex > route_lock( n_route::g_route_lock );
	auto& route = g_movement.m_route_calc_data;

	static const std::vector< const char* > style_names( std::begin( n_route::k_style_names ),
	                                                     std::end( n_route::k_style_names ) );
	static const std::vector< unsigned int > style_bits = { 1u << n_route::style_jump, 1u << n_route::style_minijump,
	                                                        1u << n_route::style_longjump, 1u << n_route::style_jumpbug,
	                                                        1u << n_route::style_walk_off };
	static const std::vector< const char* > point_names = { n_route::k_style_names[ n_route::style_jump ], "jump (ducked)",
	                                                        "delay hop", "delay hop (ducked)",
	                                                        n_route::k_style_names[ n_route::style_minijump ], "delay mj",
	                                                        n_route::k_style_names[ n_route::style_longjump ], "delay lj",
	                                                        n_route::k_style_names[ n_route::style_jumpbug ] };
	static const std::vector< unsigned int > point_bits = { 1u << n_route::style_jump, n_route::k_pt_duck_jump,
	                                                        n_route::k_pt_delay_hop, n_route::k_pt_delay_duck,
	                                                        1u << n_route::style_minijump, n_route::k_pt_delay_mj,
	                                                        1u << n_route::style_longjump, n_route::k_pt_delay_lj,
	                                                        1u << n_route::style_jumpbug };

	const auto style_combo = []( const char* label, unsigned int& mask, const std::vector< const char* >& names,
	                             const std::vector< unsigned int >& bits ) {
		const int count = static_cast< int >( names.size( ) );
		std::vector< bool > flags( count, true );
		for ( int f = 0; f < count; ++f )
			flags[ f ] = ( mask & bits[ f ] ) != 0u;
		if ( !ImGui::MultiCombo( label, flags, names, count ) )
			return false;
		/* rewrite only drawn bits: a row without "walk off" must not clear it */
		unsigned int drawn = 0u, built = 0u;
		for ( int f = 0; f < count; ++f ) {
			drawn |= bits[ f ];
			if ( flags[ f ] )
				built |= bits[ f ];
		}
		mask = ( mask & ~drawn ) | built;
		return true;
	};

	menu_columns_begin( );

	if ( menu_group_begin( "route calculator" ) ) {
		ImGui::Checkbox( "route calculator", &GET_VARIABLE( g_variables.m_route_calc, bool ) );

		if ( ImGui::Checkbox( "use current pos for start jump", &GET_VARIABLE( g_variables.m_route_calc_start_here, bool ) ) )
			route.drop_solutions( );

		if ( ImGui::Checkbox( "allow selecting invalid pixels", &GET_VARIABLE( g_variables.m_route_calc_allow_invalid, bool ) ) )
			route.drop_solutions( );

		ImGui::Checkbox( "snap points to nearest edge", &GET_VARIABLE( g_variables.m_route_calc_snap_edge, bool ) );
		ImGui::Checkbox( "show key list", &GET_VARIABLE( g_variables.m_route_calc_show_keys, bool ) );
		ImGui::Checkbox( "show path", &GET_VARIABLE( g_variables.m_route_calc_show_path, bool ) );
		if ( GET_VARIABLE( g_variables.m_route_calc_show_path, bool ) ) {
			ImGui::SliderInt( "path alpha", &GET_VARIABLE( g_variables.m_route_calc_path_alpha, int ), 0, 255, "%d" );
			ImGui::SliderFloat( "path thickness", &GET_VARIABLE( g_variables.m_route_calc_path_thickness, float ), 1.f, 8.f, "%.1f px" );
		}
		ImGui::Checkbox( "show combos on screen", &GET_VARIABLE( g_variables.m_route_calc_show_bar, bool ) );
		ImGui::Checkbox( "advanced readout##route calc", &GET_VARIABLE( g_variables.m_route_calc_advanced_readout, bool ) );

		ImGui::SliderInt( "popup duration", &GET_VARIABLE( g_variables.m_route_calc_popup_time, int ), 1, 30, "%d s" );
		ImGui::SliderInt( "max displayed combos", &GET_VARIABLE( g_variables.m_route_calc_max_results, int ), 1, 64, "%d combos" );

		if ( ImGui::SliderInt( "delay hop ticks", &GET_VARIABLE( g_variables.m_route_calc_delay_ticks, int ), 0, 8, "%d ticks" ) )
			route.drop_solutions( );

		ImGui::Label( "jump types to calculate" );
		if ( move_mask_combo( "##global jump types", GET_VARIABLE( g_variables.m_route_calc_global_moves, int ) ) )
			route.drop_solutions( );
	}
	menu_group_end( );

	if ( menu_group_begin( "keys" ) ) {
		ImGui::Label( "add point" );
		ImGui::Keybind( "route calc add key", &GET_VARIABLE( g_variables.m_route_calc_add_key, key_bind_t ) );
		ImGui::Label( "calculate combos" );
		ImGui::Keybind( "route calc solve key", &GET_VARIABLE( g_variables.m_route_calc_solve_key, key_bind_t ) );
		ImGui::Label( "delete point" );
		ImGui::Keybind( "route calc delete key", &GET_VARIABLE( g_variables.m_route_calc_delete_key, key_bind_t ) );
		ImGui::Label( "clear all points" );
		ImGui::Keybind( "route calc clear key", &GET_VARIABLE( g_variables.m_route_calc_clear_key, key_bind_t ) );
	}
	menu_group_end( );

	menu_columns_next( );

	if ( menu_group_begin( "points" ) ) {
		if ( ImGui::BeginListBox( "##route points", ImVec2( -1.f, 160.f ) ) ) {
			const bool start_here = GET_VARIABLE( g_variables.m_route_calc_start_here, bool );
			if ( ImGui::Selectable( start_here ? "starting jump (current position)" : "starting jump (point 1)", route.selected_row == 0 ) )
				route.selected_row = 0;

			for ( std::size_t i = 0; i < route.points.size( ); ++i ) {
				const std::string row = std::to_string( static_cast< int >( i ) + 1 ) + ". " +
				                        route_point_type_name( route.points[ i ].type ) + "   z " +
				                        std::to_string( static_cast< int >( route.points[ i ].pos.m_z ) ) +
				                        ( route.points[ i ].enabled ? "" : "   (off)" );
				if ( ImGui::Selectable( row.c_str( ), route.selected_row == static_cast< int >( i ) + 1 ) )
					route.selected_row = static_cast< int >( i ) + 1;
			}
			ImGui::EndListBox( );
		}

		if ( ImGui::Button( "clear all points", ImVec2( -1.f, 15.f ) ) )
			route.clear( );

		if ( route.selected_row > static_cast< int >( route.points.size( ) ) )
			route.selected_row = 0;

		if ( route.selected_row == 0 ) {
			ImGui::Label( "types to calculate" );
			int& stored       = GET_VARIABLE( g_variables.m_route_calc_start_styles, int );
			unsigned int mask = static_cast< unsigned int >( stored );
			if ( style_combo( "##types to calculate", mask, style_names, style_bits ) ) {
				stored = static_cast< int >( mask & n_route::k_all_styles );
				route.drop_solutions( );
			}
		} else {
			auto& point = route.points[ route.selected_row - 1 ];

			if ( ImGui::Checkbox( "point enabled", &point.enabled ) )
				route.drop_solutions( );

			const int row = route.selected_row - 1;
			if ( ImGui::Button( "move up", ImVec2( -1.f, 15.f ) ) && row > 0 ) {
				std::swap( route.points[ row ], route.points[ row - 1 ] );
				route.selected_row -= 1;
				route.drop_solutions( );
			}
			if ( ImGui::Button( "move down", ImVec2( -1.f, 15.f ) ) && row + 1 < static_cast< int >( route.points.size( ) ) ) {
				std::swap( route.points[ row ], route.points[ row + 1 ] );
				route.selected_row += 1;
				route.drop_solutions( );
			}
			if ( ImGui::Button( "delete point", ImVec2( -1.f, 15.f ) ) ) {
				route.points.erase( route.points.begin( ) + row );
				if ( route.selected_row > static_cast< int >( route.points.size( ) ) )
					route.selected_row = static_cast< int >( route.points.size( ) );
				route.drop_solutions( );
			} else {
				static const char* const type_names[ route_pt_count ] = { "ground",   "pixelsurf",
				                                                         "edgebug",  "headbang",
				                                                         "texturebug", "pixeljump",
				                                                         "headbounce" };
				int shown                                             = point.type;
				if ( ImGui::Combo( "point type", &shown, type_names, route_pt_count ) && shown != point.type ) {
					const bool needs_dot = shown == route_pt_pixelsurf || shown == route_pt_texturebug || shown == route_pt_pixeljump ||
					                       shown == route_pt_headbounce;
					const bool dot_ok    = point.snap_type == shown ||
					                    ( shown == route_pt_pixelsurf && point.snap_type == route_pt_texturebug );
					if ( needs_dot && !dot_ok &&
					     !GET_VARIABLE( g_variables.m_route_calc_allow_invalid, bool ) ) {
						movement_add_window( 3, std::string( "route: a " ) + type_names[ shown ] +
						                            " point must be placed on a " +
						                            ( shown == route_pt_pixelsurf    ? "green pixelsurf"
						                              : shown == route_pt_pixeljump  ? "yellow pixeljump"
						                              : shown == route_pt_headbounce ? "red headbounce"
						                                                             : "cyan texturebug" ) +
						                            " finder dot" );
					} else {
						if ( point.type != shown )
							point.has_measured = false;
						point.type = shown;
						route.drop_solutions( );
					}
				}

				ImGui::Label( "types to calculate" );
				if ( point.type == route_pt_ground || point.type == route_pt_pixeljump ) {
					if ( style_combo( "##types to calculate", point.styles, point_names, point_bits ) )
						route.drop_solutions( );
				} else {
					if ( ImGui::Checkbox( "arrive standing", &point.allow_stand ) )
						route.drop_solutions( );
					if ( ImGui::Checkbox( "arrive ducked", &point.allow_duck ) )
						route.drop_solutions( );
				}
			}
		}
	}
	menu_group_end( );

	menu_columns_end( );
}

/* lumi points page: global look | per kind design, preview right */
static void points_page( )
{
	static int selected = point_style_ps;

	menu_columns_begin( );

	if ( menu_group_begin( "points" ) ) {
		ImGui::Checkbox( "render ps assist points", &GET_VARIABLE( g_variables.m_points_render_ps, bool ) );
		ImGui::Checkbox( "render bounce assist points", &GET_VARIABLE( g_variables.m_points_render_bounce, bool ) );
		ImGui::Checkbox( "render calculator points", &GET_VARIABLE( g_variables.m_points_render_calc, bool ) );
		ImGui::Checkbox( "render finder points", &GET_VARIABLE( g_variables.m_points_render_finder, bool ) );
		ImGui::SliderFloat( "points opacity", &GET_VARIABLE( g_variables.m_points_opacity, float ), 0.1f, 1.f, "%.1f", ImGuiSliderFlags_AlwaysClamp );
		ImGui::SliderFloat( "max render distance", &GET_VARIABLE( g_variables.m_points_max_dist, float ), 200.f, 4000.f, "%.0f u",
		                    ImGuiSliderFlags_AlwaysClamp );
		ImGui::Checkbox( "distance fade", &GET_VARIABLE( g_variables.m_points_distance_fade, bool ) );
		ImGui::Checkbox( "distance shrink", &GET_VARIABLE( g_variables.m_points_distance_shrink, bool ) );
		ImGui::Checkbox( "radius color", &GET_VARIABLE( g_variables.m_points_radius_color, bool ) );
		if ( GET_VARIABLE( g_variables.m_points_radius_color, bool ) ) {
			ImGui::SameLine( );
			ImGui::ColorEdit4( "##points radius color", &GET_VARIABLE( g_variables.m_points_radius_clr, c_color ), color_picker_alpha_flags );
		}
		ImGui::Checkbox( "delete points", &GET_VARIABLE( g_variables.m_points_delete, bool ) );
		if ( GET_VARIABLE( g_variables.m_points_delete, bool ) )
			ImGui::Keybind( "delete points key", &GET_VARIABLE( g_variables.m_points_delete_key, key_bind_t ) );
	}
	menu_group_end( );

	if ( menu_group_begin( "appearance" ) ) {
		ImGui::SliderFloat( "point size", &GET_VARIABLE( g_variables.m_points_size, float ), 0.5f, 3.f, "%.2fx", ImGuiSliderFlags_AlwaysClamp );
		ImGui::SliderFloat( "outline thickness", &GET_VARIABLE( g_variables.m_points_thickness, float ), 0.5f, 4.f, "%.2fx",
		                    ImGuiSliderFlags_AlwaysClamp );
		ImGui::SliderFloat( "background opacity", &GET_VARIABLE( g_variables.m_points_fill, float ), 0.f, 1.f, "%.2f", ImGuiSliderFlags_AlwaysClamp );
		ImGui::SliderFloat( "hover size", &GET_VARIABLE( g_variables.m_points_hover_scale, float ), 1.f, 2.5f, "%.2fx", ImGuiSliderFlags_AlwaysClamp );
		ImGui::SliderFloat( "design size", &GET_VARIABLE( g_variables.m_points_design_size, float ), 0.2f, 1.2f, "%.2fx",
		                    ImGuiSliderFlags_AlwaysClamp );
		ImGui::Checkbox( "glow", &GET_VARIABLE( g_variables.m_points_glow, bool ) );
		ImGui::Checkbox( "pulse in radius", &GET_VARIABLE( g_variables.m_points_pulse, bool ) );
		ImGui::Checkbox( "point labels", &GET_VARIABLE( g_variables.m_points_labels, bool ) );
	}
	menu_group_end( );

	menu_columns_next( );

	if ( menu_group_begin( "point design" ) ) {
		static const std::vector< const char* > names = [ ] {
			std::vector< const char* > out;
			for ( int s = 0; s < point_style_count; ++s )
				out.push_back( point_style( s ).name );
			return out;
		}( );
		ImGui::Combo( "point type", &selected, names.data( ), point_style_count );
		const auto& style = point_style( selected );

		/* assist + calculator points follow the accent */
		if ( style.has_color )
			ImGui::ColorEdit4( "point color", &GET_VARIABLE( style.color, c_color ), color_picker_alpha_flags );

		if ( style.has_gaps ) {
			ImGui::Combo( "outline shape", &GET_VARIABLE( style.shape, int ), k_point_shape_names, k_point_shape_count );
			const int stored = GET_VARIABLE( style.gaps, int );
			std::vector< bool > gaps( 4 );
			for ( int g = 0; g < 4; ++g )
				gaps[ g ] = ( stored & ( 1 << g ) ) != 0;
			if ( ImGui::MultiCombo( "outline gaps", gaps, { "top", "bottom", "left", "right" }, 4 ) ) {
				int built = 0;
				for ( int g = 0; g < 4; ++g )
					if ( gaps[ g ] )
						built |= 1 << g;
				GET_VARIABLE( style.gaps, int ) = built;
			}
		}

		if ( style.has_design ) {
			ImGui::Checkbox( "draw design", &GET_VARIABLE( style.design, bool ) );
			if ( GET_VARIABLE( style.design, bool ) )
				ImGui::Combo( "design type", &GET_VARIABLE( style.design_type, int ), k_point_design_names, k_point_design_count );
		}
	}
	menu_group_end( );

	if ( menu_group_begin( "preview" ) ) {
		ImGui::Dummy( ImVec2( ImGui::GetContentRegionAvail( ).x, 220.f ) );
		const ImVec2 lo = ImGui::GetItemRectMin( ), hi = ImGui::GetItemRectMax( );
		const float cx = ( lo.x + hi.x ) * 0.5f, cy = ( lo.y + hi.y ) * 0.5f - 8.f;
		const float r  = std::clamp( ( std::min )( hi.x - lo.x, hi.y - lo.y ) * 0.15f * GET_VARIABLE( g_variables.m_points_size, float ), 2.5f,
		                             ( hi.y - lo.y ) * 0.35f );
		point_style_preview( selected, cx, cy, r );
		const char* label   = point_style( selected ).name;
		const ImVec2 text_s = ImGui::CalcTextSize( label );
		ImGui::GetWindowDrawList( )->AddText( ImVec2( std::floor( cx - text_s.x * 0.5f ), cy + r + 10.f ), ImGui::GetColorU32( ImGuiCol_Text ), label );
	}
	menu_group_end( );

	menu_columns_end( );
}

void n_menu::impl_t::tab_movement( )
{
	switch ( this->m_subtab ) {
	case 0: {
		menu_columns_begin( );

		if ( menu_group_begin( "movement" ) ) {
			ImGui::Checkbox( "bunny hop", &GET_VARIABLE( g_variables.m_bunny_hop, bool ) );
			if ( GET_VARIABLE( g_variables.m_bunny_hop, bool ) ) {
				ImGui::OptionPopup(
					"bunny hop settings",
					[ & ]( ) {
						ImGui::SliderInt( "success rate##bunny hop chance", &GET_VARIABLE( g_variables.m_bunny_hop_chance, int ), 0, 100, "%d%%" );
					},
					ImVec2( 220.f, -1.f ) );
			}

			ImGui::Checkbox( "delay hop", &GET_VARIABLE( g_variables.m_delay_hop, bool ) );
			if ( GET_VARIABLE( g_variables.m_delay_hop, bool ) ) {
				ImGui::OptionPopup(
					"delay hop settings",
					[ & ]( ) {
						ImGui::Label( "delay hop keybind" );
						ImGui::Keybind( "delay hop key", &GET_VARIABLE( g_variables.m_delay_hop_key, key_bind_t ) );
						ImGui::SliderInt( "ticks to wait##delay hop ticks", &GET_VARIABLE( g_variables.m_delay_hop_ticks, int ), 1, 8,
						                  "%d ticks" );
					},
					ImVec2( 220.f, -1.f ) );
			}

			ImGui::Checkbox( "edge jump", &GET_VARIABLE( g_variables.m_edge_jump, bool ) );
			if ( GET_VARIABLE( g_variables.m_edge_jump, bool ) ) {
				ImGui::OptionPopup(
					"edge jump settings",
					[ & ]( ) {
						ImGui::Label( "edge jump keybind" );
						ImGui::Keybind( "edge jump key", &GET_VARIABLE( g_variables.m_edge_jump_key, key_bind_t ) );
						ImGui::Checkbox( "on ladders##edge jump ladder", &GET_VARIABLE( g_variables.m_edge_jump_ladder, bool ) );
					},
					ImVec2( 220.f, -1.f ) );
			}

			ImGui::Checkbox( "long jump", &GET_VARIABLE( g_variables.m_long_jump, bool ) );
			if ( GET_VARIABLE( g_variables.m_long_jump, bool ) ) {
				ImGui::OptionPopup(
					"long jump settings",
					[ & ]( ) {
						ImGui::Label( "long jump keybind" );
						ImGui::Keybind( "long jump key", &GET_VARIABLE( g_variables.m_long_jump_key, key_bind_t ) );
						ImGui::Checkbox( "adaptive key cancelling", &GET_VARIABLE( g_variables.m_long_jump_adaptive, bool ) );
						if ( GET_VARIABLE( g_variables.m_long_jump_adaptive, bool ) ) {
							auto& stop = GET_VARIABLE( g_variables.m_long_jump_adaptive_stop, std::vector< bool > );
							ImGui::MultiCombo( "stop cancelling when##ljadstop", stop, { "let go", "landed", "time passes" }, stop.size( ) );
							if ( stop.size( ) > 2 && stop[ 2 ] )
								ImGui::SliderFloat( "time after jump##ljadtime", &GET_VARIABLE( g_variables.m_long_jump_adaptive_time, float ), 0.02f,
								                    1.f, "%.2f s" );
						}
						ImGui::Checkbox( "edge jump##ljej", &GET_VARIABLE( g_variables.m_long_jump_edge_jump, bool ) );
						if ( GET_VARIABLE( g_variables.m_long_jump_edge_jump, bool ) )
							ImGui::Checkbox( "on ladders##ljejladder", &GET_VARIABLE( g_variables.m_long_jump_edge_jump_ladder, bool ) );
					},
					ImVec2( 220.f, -1.f ) );
			}

			ImGui::Checkbox( "mini jump", &GET_VARIABLE( g_variables.m_mini_jump, bool ) );
			if ( GET_VARIABLE( g_variables.m_mini_jump, bool ) ) {
				ImGui::OptionPopup(
					"mini jump settings",
					[ & ]( ) {
						ImGui::Label( "mini jump keybind" );
						ImGui::Keybind( "mini jump key", &GET_VARIABLE( g_variables.m_mini_jump_key, key_bind_t ) );
						ImGui::Checkbox( "edge jump", &GET_VARIABLE( g_variables.m_mini_jump_edge_jump, bool ) );
						ImGui::Checkbox( "hold crouch", &GET_VARIABLE( g_variables.m_mini_jump_hold_duck, bool ) );
					},
					ImVec2( 220.f, -1.f ) );
			}

			ImGui::Checkbox( "jump bug", &GET_VARIABLE( g_variables.m_jump_bug, bool ) );
			if ( GET_VARIABLE( g_variables.m_jump_bug, bool ) ) {
				ImGui::OptionPopup(
					"jump bug settings",
					[ & ]( ) {
						ImGui::Label( "jump bug keybind" );
						ImGui::Keybind( "jump bug key", &GET_VARIABLE( g_variables.m_jump_bug_key, key_bind_t ) );
						ImGui::Checkbox( "while crouching", &GET_VARIABLE( g_variables.m_jump_bug_crouch, bool ) );
						ImGui::Checkbox( "kangaroo", &GET_VARIABLE( g_variables.m_jump_bug_kangaroo, bool ) );
						ImGui::Checkbox( "jb only", &GET_VARIABLE( g_variables.m_jump_bug_only, bool ) );
						ImGui::Checkbox( "height assist", &GET_VARIABLE( g_variables.m_jump_bug_assist, bool ) );
						if ( GET_VARIABLE( g_variables.m_jump_bug_assist, bool ) )
							ImGui::SliderInt( "assist ticks##jba", &GET_VARIABLE( g_variables.m_jump_bug_assist_ticks, int ), 1, 8 );
					},
					ImVec2( 220.f, -1.f ) );
			}

			ImGui::Checkbox( "auto crouch", &GET_VARIABLE( g_variables.m_auto_crouch, bool ) );
			if ( GET_VARIABLE( g_variables.m_auto_crouch, bool ) ) {
				ImGui::OptionPopup(
					"auto crouch settings",
					[ & ]( ) {
						ImGui::Label( "auto crouch keybind" );
						ImGui::Keybind( "auto crouch key", &GET_VARIABLE( g_variables.m_auto_crouch_key, key_bind_t ) );
						ImGui::SliderInt( "ticks##autocrouch", &GET_VARIABLE( g_variables.m_auto_crouch_ticks, int ), 1, 32 );
					},
					ImVec2( 220.f, -1.f ) );
			}

			ImGui::Checkbox( "auto strafe", &GET_VARIABLE( g_variables.m_auto_strafe, bool ) );
			if ( GET_VARIABLE( g_variables.m_auto_strafe, bool ) ) {
				ImGui::OptionPopup(
					"auto strafe settings",
					[ & ]( ) {
						ImGui::Label( "auto strafe keybind" );
						ImGui::Keybind( "auto strafe key", &GET_VARIABLE( g_variables.m_auto_strafe_key, key_bind_t ) );

						ImGui::Combo( "type##astype", &GET_VARIABLE( g_variables.m_auto_strafe_type, int ), "standard\0WASD\0circle (mouse)\0" );
						if ( GET_VARIABLE( g_variables.m_auto_strafe_type, int ) == 1 )
							ImGui::Checkbox( "strafe in last pressed direction", &GET_VARIABLE( g_variables.m_auto_strafe_last_keys, bool ) );
						if ( GET_VARIABLE( g_variables.m_auto_strafe_type, int ) == 2 )
							ImGui::Checkbox( "strafe in last mouse direction", &GET_VARIABLE( g_variables.m_auto_strafe_last_mouse, bool ) );
						ImGui::Checkbox( "avoid wall collisions", &GET_VARIABLE( g_variables.m_auto_strafe_avoid_walls, bool ) );
						if ( GET_VARIABLE( g_variables.m_auto_strafe_avoid_walls, bool ) )
							ImGui::SliderFloat( "avoid distance", &GET_VARIABLE( g_variables.m_auto_strafe_avoid_dist, float ), 0.f, 200.f, "%.0f u" );
					},
					ImVec2( 220.f, -1.f ) );
			}

			ImGui::Checkbox( "strafe optimizer", &GET_VARIABLE( g_variables.m_strafe_optimizer, bool ) );
			if ( GET_VARIABLE( g_variables.m_strafe_optimizer, bool ) ) {
				ImGui::OptionPopup(
					"strafe optimizer settings",
					[ & ]( ) {
						ImGui::SliderFloat( "gain", &GET_VARIABLE( g_variables.m_strafe_optimizer_gain, float ), 0.f, 100.f, "%.0f%%" );
						ImGui::SliderFloat( "min speed", &GET_VARIABLE( g_variables.m_strafe_optimizer_min_speed, float ), 0.f, 250.f, "%.0f" );
					},
					ImVec2( 220.f, -1.f ) );
			}

			ImGui::Checkbox( "ladder bug", &GET_VARIABLE( g_variables.m_ladder_bug, bool ) );
			ImGui::Keybind( "ladder bug key", &GET_VARIABLE( g_variables.m_ladder_bug_key, key_bind_t ) );

			ImGui::Checkbox( "ladder glide", &GET_VARIABLE( g_variables.m_ladder_glide, bool ) );
			ImGui::Keybind( "ladder glide key", &GET_VARIABLE( g_variables.m_ladder_glide_key, key_bind_t ) );

			ImGui::Checkbox( "ladder freelook climb", &GET_VARIABLE( g_variables.m_ladder_freelook_climb, bool ) );
			ImGui::Keybind( "ladder freelook climb key", &GET_VARIABLE( g_variables.m_ladder_freelook_climb_key, key_bind_t ) );

			ImGui::Checkbox( "fast ladder", &GET_VARIABLE( g_variables.m_fast_ladder, bool ) );
			ImGui::Keybind( "fast ladder key", &GET_VARIABLE( g_variables.m_fast_ladder_key, key_bind_t ) );

			ImGui::Checkbox( "edge bug", &GET_VARIABLE( g_variables.edge_bug, bool ) );

			if ( GET_VARIABLE( g_variables.edge_bug, bool ) ) {
				ImGui::OptionPopup(
					"edge bug settings",
					[ & ]( ) {
						ImGui::Label( "edgebug keybind" );
						ImGui::Keybind( "edge bug key", &GET_VARIABLE( g_variables.edge_bug_key, key_bind_t ) );
						ImGui::Combo( "style##ebstyle", &GET_VARIABLE( g_variables.m_edgebug_style, int ), "botox\0delusional\0dna\0hotwheels\0" );
						const int style = GET_VARIABLE( g_variables.m_edgebug_style, int );
						if ( style == 1 ) {
							ImGui::Checkbox( "mouse fix##ebdelmousefix", &GET_VARIABLE( g_variables.m_edgebug_del_mouse_fix, bool ) );
							ImGui::SliderInt( "ticks to predict##ebdelticks", &GET_VARIABLE( g_variables.m_edgebug_del_ticks, int ), 0, 128 );
							ImGui::Checkbox( "advanced detection##ebdeladv", &GET_VARIABLE( g_variables.m_edgebug_del_advanced, bool ) );
							if ( GET_VARIABLE( g_variables.m_edgebug_del_advanced, bool ) ) {
								ImGui::SliderFloat( "angle limit##ebdelangle", &GET_VARIABLE( g_variables.m_edgebug_del_angle_limit, float ), 0.f, 180.f, "%.2f" );
								if ( GET_VARIABLE( g_variables.m_edgebug_del_angle_limit, float ) > 0.f ) {
									ImGui::SliderInt( "search amount##ebdelsearch", &GET_VARIABLE( g_variables.m_edgebug_del_search, int ), 1, 15 );
									ImGui::Checkbox( "silent edgebug##ebdelsilent", &GET_VARIABLE( g_variables.m_edgebug_del_silent, bool ) );
								}
							}
						} else if ( style == 2 ) {
							ImGui::SliderFloat( "scan time##ebdnascan", &GET_VARIABLE( g_variables.m_edgebug_dna_scan, float ), 0.f, 1.f, "%.2f s" );
							ImGui::Checkbox( "advanced mode##ebdnaadv", &GET_VARIABLE( g_variables.m_edgebug_dna_advanced, bool ) );
							if ( GET_VARIABLE( g_variables.m_edgebug_dna_advanced, bool ) ) {
								ImGui::SliderInt( "advanced range##ebdnarange", &GET_VARIABLE( g_variables.m_edgebug_dna_range, int ), 1, 10 );
								ImGui::Checkbox( "strafe custom angle##ebdnacustom", &GET_VARIABLE( g_variables.m_edgebug_dna_custom_angle, bool ) );
								ImGui::SliderFloat( "angle limit##ebdnaangle", &GET_VARIABLE( g_variables.m_edgebug_dna_angle_limit, float ), 0.f, 45.f, "%.1f" );
							}
						} else if ( style == 3 ) {
							ImGui::SliderInt( "edge bug predicted ticks##ebhwticks", &GET_VARIABLE( g_variables.m_edgebug_hw_ticks, int ), 0, 32 );
							ImGui::SliderFloat( "mouse lock scale##ebhwlock", &GET_VARIABLE( g_variables.m_edgebug_hw_lock_amt, float ), 0.f, 1.f, "%.2f" );
							ImGui::Checkbox( "advanced detection##ebhwadv", &GET_VARIABLE( g_variables.m_edgebug_hw_advanced, bool ) );
							if ( GET_VARIABLE( g_variables.m_edgebug_hw_advanced, bool ) )
								ImGui::SliderFloat( "max strafe delta##ebhwdelta", &GET_VARIABLE( g_variables.m_edgebug_hw_strafe_delta_max, float ), 0.1f, 100.f,
								                    "%.1f%%" );
						}
						if ( style == 0 ) {
							ImGui::SliderFloat( "edge bug predict time", &GET_VARIABLE( g_variables.m_edgebug_predict_time, float ),
							                    0.02f, 2.f, "%.2f s" );
							/* none picked = both */
							ImGui::MultiCombo( "stance##ebtypes", GET_VARIABLE( g_variables.m_edgebug_types, std::vector< bool > ),
							                   { "ducked", "standing" }, GET_VARIABLE( g_variables.m_edgebug_types, std::vector< bool > ).size( ) );

							ImGui::Checkbox( "autostrafe to edge", &GET_VARIABLE( g_variables.m_edgebug_autostrafe_to_edge, bool ) );
							if ( GET_VARIABLE( g_variables.m_edgebug_autostrafe_to_edge, bool ) ) {
								/* cone on the PATH, not the push: max bend off the starting course; lips
								   outside it are never picked. 180 = unclamped */
								ImGui::SliderFloat( "max path bend##ebstrafemax",
								                    &GET_VARIABLE( g_variables.m_edgebug_autostrafe_max_angle, float ), 0.f, 180.f, "%.0f deg" );
								/* bisect a sideways push between a run that lands on the lip and one that misses it */
								ImGui::Checkbox( "lip refine##ebrefine", &GET_VARIABLE( g_variables.m_edgebug_lip_refine, bool ) );
								if ( GET_VARIABLE( g_variables.m_edgebug_lip_refine, bool ) )
									ImGui::SliderInt( "refine steps##ebrefsteps", &GET_VARIABLE( g_variables.m_edgebug_refine_steps, int ), 1, 12 );
								ImGui::Checkbox( "path bend##ebbend", &GET_VARIABLE( g_variables.m_edgebug_path_bend, bool ) );
								ImGui::Checkbox( "ebanat shapes##ebshapes", &GET_VARIABLE( g_variables.m_edgebug_ebanat_shapes, bool ) );
							}
							ImGui::Checkbox( "duck timing##ebducktiming", &GET_VARIABLE( g_variables.m_edgebug_duck_timing, bool ) );
							if ( GET_VARIABLE( g_variables.m_edgebug_duck_timing, bool ) )
								ImGui::Checkbox( "full window##ebfullwin", &GET_VARIABLE( g_variables.m_edgebug_full_window, bool ) );
							/* sim budget per search: 1x = old cost, higher = more finds + more cpu per tick near lips */
							ImGui::SliderFloat( "search effort##ebeffort", &GET_VARIABLE( g_variables.m_edgebug_search_effort, float ), 0.25f, 3.f, "%.2fx" );

							ImGui::Checkbox( "mouse steer##ebsteer", &GET_VARIABLE( g_variables.m_edgebug_mouse_steer, bool ) );
							if ( GET_VARIABLE( g_variables.m_edgebug_mouse_steer, bool ) )
								ImGui::SliderInt( "paths##ebpaths", &GET_VARIABLE( g_variables.m_edgebug_paths, int ), 1, 4 );
						}

						if ( style != 3 )
							ImGui::Checkbox( "lock mouse on find", &GET_VARIABLE( g_variables.m_edgebug_mouse_lock, bool ) );
						if ( style != 3 && GET_VARIABLE( g_variables.m_edgebug_mouse_lock, bool ) ) {
							ImGui::Combo( "lock type##eblocktype", &GET_VARIABLE( g_variables.m_edgebug_mouse_lock_type, int ),
							              style == 0 ? "smooth\0full\0" : "static\0full\0" );
							ImGui::SliderFloat( "lock strength##eblockstr", &GET_VARIABLE( g_variables.m_edgebug_mouse_lock_strength, float ), 0.f,
							                    100.f, "%.0f%%" );
						}

						ImGui::Combo( "detection sound##ebsound", &GET_VARIABLE( g_variables.m_edge_bug_sound, int ),
					                  "none\0arena switch\0button\0money\0beep\0custom\0" );

						if ( GET_VARIABLE( g_variables.m_edge_bug_sound, int ) == 5 ) {
							auto&       eb_snd_var = GET_VARIABLE( g_variables.m_edge_bug_sound_custom, std::string );
							static char eb_snd_buffer[ 128 ] = { };
							text_buffer_sync( "sound path##ebsndpath", eb_snd_var, eb_snd_buffer, sizeof( eb_snd_buffer ) );
						}

						if ( GET_VARIABLE( g_variables.m_edge_bug_sound, int ) > 0 )
							ImGui::SliderFloat( "sound volume##ebsndvol", &GET_VARIABLE( g_variables.m_edge_bug_sound_volume, float ), 0.f, 2.f, "%.2f" );

						if ( style == 0 ) {
							ImGui::Combo( "detection mode##ebdetectmode", &GET_VARIABLE( g_variables.m_edgebug_detection_mode, int ),
							              "normal\0enhanced\0ebanat\0" );
							if ( GET_VARIABLE( g_variables.m_edgebug_detection_mode, int ) == 1 )
								ImGui::SliderInt( "search amount##ebsearch", &GET_VARIABLE( g_variables.m_edgebug_search_amount, int ), 1, 4 );
						}
					},
					ImVec2( 220.f, -1.f ) );
			}

			/* edge skip: walk off / bug on a lip, strafe back + air duck onto it. own key, never while holding duck */
			ImGui::Checkbox( "edge skip", &GET_VARIABLE( g_variables.m_edgebug_edge_skip, bool ) );
			if ( GET_VARIABLE( g_variables.m_edgebug_edge_skip, bool ) ) {
				ImGui::OptionPopup(
					"edge skip settings",
					[ & ]( ) {
						ImGui::Label( "edge skip keybind" );
						ImGui::Keybind( "edge skip key", &GET_VARIABLE( g_variables.m_edge_skip_key, key_bind_t ) );
						ImGui::Checkbox( "use edgebug settings##esuseeb", &GET_VARIABLE( g_variables.m_edge_skip_use_eb, bool ) );
						if ( GET_VARIABLE( g_variables.m_edge_skip_use_eb, bool ) )
							return;
						ImGui::SliderFloat( "predict time##esptime", &GET_VARIABLE( g_variables.m_edge_skip_predict_time, float ), 0.02f, 2.f, "%.2f s" );
						ImGui::SliderFloat( "max path bend##esbend", &GET_VARIABLE( g_variables.m_edge_skip_max_angle, float ), 0.f, 180.f, "%.0f deg" );
						ImGui::SliderInt( "search amount##essearch", &GET_VARIABLE( g_variables.m_edge_skip_search_amount, int ), 1, 4 );

						ImGui::Checkbox( "lock mouse on find##eslock", &GET_VARIABLE( g_variables.m_edge_skip_mouse_lock, bool ) );
						if ( GET_VARIABLE( g_variables.m_edge_skip_mouse_lock, bool ) ) {
							ImGui::Combo( "lock type##eslocktype", &GET_VARIABLE( g_variables.m_edge_skip_mouse_lock_type, int ), "smooth\0full\0" );
							ImGui::SliderFloat( "lock strength##eslockstr", &GET_VARIABLE( g_variables.m_edge_skip_mouse_lock_strength, float ), 0.f,
							                    100.f, "%.0f%%" );
						}

						ImGui::Combo( "detection sound##essound", &GET_VARIABLE( g_variables.m_edge_skip_sound, int ),
						              "none\0arena switch\0button\0money\0beep\0custom\0" );
						if ( GET_VARIABLE( g_variables.m_edge_skip_sound, int ) == 5 ) {
							auto&       es_snd_var = GET_VARIABLE( g_variables.m_edge_skip_sound_custom, std::string );
							static char es_snd_buffer[ 128 ] = { };
							text_buffer_sync( "sound path##essndpath", es_snd_var, es_snd_buffer, sizeof( es_snd_buffer ) );
						}
						if ( GET_VARIABLE( g_variables.m_edge_skip_sound, int ) > 0 )
							ImGui::SliderFloat( "sound volume##essndvol", &GET_VARIABLE( g_variables.m_edge_skip_sound_volume, float ), 0.f, 2.f, "%.2f" );

						ImGui::Checkbox( "visualize##esviz", &GET_VARIABLE( g_variables.m_edge_skip_visualize, bool ) );
					},
					ImVec2( 220.f, -1.f ) );
			}

			ImGui::Checkbox( "pixel surf", &GET_VARIABLE( g_variables.m_pixel_surf, bool ) );
			if ( GET_VARIABLE( g_variables.m_pixel_surf, bool ) ) {
				ImGui::OptionPopup(
					"pixel surf settings",
					[ & ]( ) {
						ImGui::Label( "pixel surf keybind" );
						ImGui::Keybind( "pixel surf key", &GET_VARIABLE( g_variables.m_pixel_surf_key, key_bind_t ) );

						ImGui::Combo( "detection sound##pssound", &GET_VARIABLE( g_variables.m_pixel_surf_sound, int ),
					                  "none\0arena switch\0button\0money\0beep\0custom\0" );

						if ( GET_VARIABLE( g_variables.m_pixel_surf_sound, int ) == 5 ) {
							auto&       ps_snd_var = GET_VARIABLE( g_variables.m_pixel_surf_sound_custom, std::string );
							static char ps_snd_buffer[ 128 ] = { };
							text_buffer_sync( "sound path##pssndpath", ps_snd_var, ps_snd_buffer, sizeof( ps_snd_buffer ) );
						}

						if ( GET_VARIABLE( g_variables.m_pixel_surf_sound, int ) > 0 )
							ImGui::SliderFloat( "sound volume##pssndvol", &GET_VARIABLE( g_variables.m_pixel_surf_sound_volume, float ), 0.f, 2.f, "%.2f" );
					},
					ImVec2( 220.f, -1.f ) );
			}

			ImGui::Checkbox( "pixelsurf assist", &GET_VARIABLE( g_variables.m_pixel_surf_assist, bool ) );
			if ( GET_VARIABLE( g_variables.m_pixel_surf_assist, bool ) ) {
				ImGui::OptionPopup(
					"pixelsurf assist settings",
					[ & ]( ) {
						ImGui::Label( "assist keybind" );
						ImGui::Keybind( "pixelsurf assist key", &GET_VARIABLE( g_variables.m_pixel_surf_assist_key, key_bind_t ) );

						ImGui::Label( "set pixelsurf point key" );
						ImGui::Keybind( "set pixelsurf point key",
						                &GET_VARIABLE( g_variables.m_pixel_surf_assist_point_key, key_bind_t ) );

						ImGui::SliderInt( "assist ticks##pxticks", &GET_VARIABLE( g_variables.m_pixel_surf_assist_ticks, int ), 0, 128, "%d",
						                  ImGuiSliderFlags_AlwaysClamp );

						ImGui::Combo( "assist type##pxtyppee", &GET_VARIABLE( g_variables.m_pixel_surf_assist_type, int ),
					                  "standard\0delta strafe\0" );

						ImGui::Checkbox( "stamina hops", &GET_VARIABLE( g_variables.m_pixel_surf_assist_brokehop, bool ) );
						ImGui::Checkbox( "ground help", &GET_VARIABLE( g_variables.m_pixel_surf_assist_ground_help, bool ) );
						if ( GET_VARIABLE( g_variables.m_pixel_surf_assist_ground_help, bool ) )
							ImGui::SliderFloat( "ground angle limit",
							                    &GET_VARIABLE( g_variables.m_pixel_surf_assist_ground_angle_limit, float ), 0.f, 45.f, "%.1f" );

						ImGui::Checkbox( "pixel surf assist render", &GET_VARIABLE( g_variables.m_pixel_surf_assist_render, bool ) );
						if ( GET_VARIABLE( g_variables.m_pixel_surf_assist_render, bool ) )
							ImGui::SliderFloat( "render height", &GET_VARIABLE( g_variables.m_pixel_surf_assist_render_height, float ), 0.f,
							                    100.f, "%.1f" );
					},
					ImVec2( 220.f, -1.f ) );
			}

			ImGui::Checkbox( "bounce assist", &GET_VARIABLE( g_variables.m_bouncee_assist, bool ) );
			if ( GET_VARIABLE( g_variables.m_bouncee_assist, bool ) ) {
				ImGui::OptionPopup(
					"bounce assist settings",
					[ & ]( ) {
						ImGui::Label( "assist keybind" );
						ImGui::Keybind( "bounce assist key", &GET_VARIABLE( g_variables.m_bounce_assist_key, key_bind_t ) );

						ImGui::Label( "set bounce points key" );
						ImGui::Keybind( "set bounce points key", &GET_VARIABLE( g_variables.m_bounce_assist_point_key, key_bind_t ) );
						ImGui::Checkbox( "stamina hops", &GET_VARIABLE( g_variables.m_bouncee_assist_brokehop, bool ) );
						ImGui::Checkbox( "render", &GET_VARIABLE( g_variables.m_bouncee_assist_render, bool ) );
					},
					ImVec2( 220.f, -1.f ) );
			}

			ImGui::Checkbox( "pixeljump assist", &GET_VARIABLE( g_variables.m_pixel_jump_assist, bool ) );
			if ( GET_VARIABLE( g_variables.m_pixel_jump_assist, bool ) ) {
				ImGui::OptionPopup(
					"pixeljump assist settings",
					[ & ]( ) {
						ImGui::Label( "assist keybind" );
						ImGui::Keybind( "pixeljump assist key", &GET_VARIABLE( g_variables.m_pixel_jump_assist_key, key_bind_t ) );

						ImGui::Label( "set pixeljump point key" );
						ImGui::Keybind( "set pixeljump point key", &GET_VARIABLE( g_variables.m_pixel_jump_assist_point_key, key_bind_t ) );

						ImGui::SliderInt( "assist ticks##pjticks", &GET_VARIABLE( g_variables.m_pixel_jump_assist_ticks, int ), 0, 128, "%d",
						                  ImGuiSliderFlags_AlwaysClamp );
						ImGui::SliderFloat( "landing radius##pjradius", &GET_VARIABLE( g_variables.m_pixel_jump_assist_radius, float ), 1.f, 48.f, "%.0f u",
						                    ImGuiSliderFlags_AlwaysClamp );
						ImGui::Checkbox( "render##pjrender", &GET_VARIABLE( g_variables.m_pixel_jump_assist_render, bool ) );
					},
					ImVec2( 220.f, -1.f ) );
			}

			ImGui::Checkbox( "auto bounce", &GET_VARIABLE( g_variables.m_auto_bounce, bool ) );
			if ( GET_VARIABLE( g_variables.m_auto_bounce, bool ) ) {
				ImGui::OptionPopup(
					"auto bounce settings",
					[ & ]( ) {
						ImGui::Label( "auto bounce keybind" );
						ImGui::Keybind( "auto bounce key", &GET_VARIABLE( g_variables.m_auto_bounce_key, key_bind_t ) );

						ImGui::Combo( "mode##abmode", &GET_VARIABLE( g_variables.m_auto_bounce_mode, int ), "drop in\0keep speed\0stop\0lumi\0" );
						if ( GET_VARIABLE( g_variables.m_auto_bounce_mode, int ) == 3 ) {
							ImGui::SliderInt( "rays##ablrays", &GET_VARIABLE( g_variables.m_auto_bounce_lumi_rays, int ), 4, 64 );
							ImGui::SliderFloat( "radius##ablradius", &GET_VARIABLE( g_variables.m_auto_bounce_lumi_radius, float ), 5.f, 100.f, "%.0f u" );
							ImGui::SliderFloat( "depth##abldepth", &GET_VARIABLE( g_variables.m_auto_bounce_lumi_depth, float ), 50.f, 4096.f, "%.0f u",
							                    ImGuiSliderFlags_Logarithmic );
							ImGui::SliderFloat( "min angle##ablmin", &GET_VARIABLE( g_variables.m_auto_bounce_lumi_min_angle, float ), 5.f, 60.f, "%.0f deg" );
							ImGui::SliderFloat( "max angle##ablmax", &GET_VARIABLE( g_variables.m_auto_bounce_lumi_max_angle, float ), 20.f, 90.f, "%.0f deg" );
							ImGui::SliderFloat( "smoothing##ablsmooth", &GET_VARIABLE( g_variables.m_auto_bounce_lumi_smooth, float ), 0.f, 1.f, "%.2f" );
							ImGui::Checkbox( "auto jump", &GET_VARIABLE( g_variables.m_auto_bounce_jump, bool ) );
							ImGui::Checkbox( "crouch near face##abcrouch", &GET_VARIABLE( g_variables.m_auto_bounce_crouch, bool ) );
						} else {
						ImGui::SliderFloat( "min speed gain", &GET_VARIABLE( g_variables.m_auto_bounce_min_gain, float ), 0.f, 500.f, "%.0f" );
						ImGui::SliderFloat( "max sim reach##abreach", &GET_VARIABLE( g_variables.m_auto_bounce_reach, float ), 16.f, 512.f, "%.0f u" );
						ImGui::Checkbox( "auto jump", &GET_VARIABLE( g_variables.m_auto_bounce_jump, bool ) );
						ImGui::Checkbox( "crouch jump##abcrouch", &GET_VARIABLE( g_variables.m_auto_bounce_crouch, bool ) );
						ImGui::Checkbox( "auto align##abalign", &GET_VARIABLE( g_variables.m_auto_bounce_align, bool ) );
						ImGui::Checkbox( "visualize##abviz", &GET_VARIABLE( g_variables.m_auto_bounce_visualize, bool ) );
						ImGui::ColorEdit4( "visualize color##abvizcolor", &GET_VARIABLE( g_variables.m_auto_bounce_visualize_color, c_color ),
						                   color_picker_alpha_flags );
						}
					},
					ImVec2( 220.f, -1.f ) );
			}

			ImGui::Checkbox( "texture bug", &GET_VARIABLE( g_variables.m_texture_bug, bool ) );
			if ( GET_VARIABLE( g_variables.m_texture_bug, bool ) ) {
				ImGui::OptionPopup(
					"texture bug settings",
					[ & ]( ) {
						ImGui::Label( "texture bug keybind" );
						ImGui::Keybind( "texture bug key", &GET_VARIABLE( g_variables.m_texture_bug_key, key_bind_t ) );
						ImGui::SliderFloat( "reach##tbreach", &GET_VARIABLE( g_variables.m_texture_bug_reach, float ), 0.f, 48.f, "%.1f u" );
						/* stance tb pins in: an unticked one is swapped for the other, never tried */
						ImGui::MultiCombo( "types allowed##tbtypes", GET_VARIABLE( g_variables.m_texture_bug_types, std::vector< bool > ),
						                   { "ducking", "standing" }, GET_VARIABLE( g_variables.m_texture_bug_types, std::vector< bool > ).size( ) );
						ImGui::Checkbox( "auto crouch##tbautoduck", &GET_VARIABLE( g_variables.m_texture_bug_auto_duck, bool ) );
						ImGui::Checkbox( "wallstrafe##tbws", &GET_VARIABLE( g_variables.m_texture_bug_wallstrafe, bool ) );

						ImGui::Combo( "detection sound##tbsound", &GET_VARIABLE( g_variables.m_texture_bug_sound, int ),
						              "none\0arena switch\0button\0money\0beep\0custom\0" );
						if ( GET_VARIABLE( g_variables.m_texture_bug_sound, int ) == 5 ) {
							auto&       tb_snd_var = GET_VARIABLE( g_variables.m_texture_bug_sound_custom, std::string );
							static char tb_snd_buffer[ 128 ] = { };
							text_buffer_sync( "sound path##tbsndpath", tb_snd_var, tb_snd_buffer, sizeof( tb_snd_buffer ) );
						}
						if ( GET_VARIABLE( g_variables.m_texture_bug_sound, int ) > 0 )
							ImGui::SliderFloat( "sound volume##tbsndvol", &GET_VARIABLE( g_variables.m_texture_bug_sound_volume, float ), 0.f, 2.f, "%.2f" );
					},
					ImVec2( 220.f, -1.f ) );
			}

			ImGui::Checkbox( "air stuck", &GET_VARIABLE( g_variables.m_air_stuck, bool ) );
			if ( GET_VARIABLE( g_variables.m_air_stuck, bool ) ) {
				ImGui::OptionPopup(
					"air stuck settings",
					[ & ]( ) {
						ImGui::Label( "air stuck keybind" );
						ImGui::Keybind( "air stuck key", &GET_VARIABLE( g_variables.m_air_stuck_key, key_bind_t ) );

						ImGui::Checkbox( "fix air stucks", &GET_VARIABLE( g_variables.m_fix_air_stucks, bool ) );
						ImGui::Checkbox( "use movement fix", &GET_VARIABLE( g_variables.m_movement_fix, bool ) );
						ImGui::Checkbox( "screen message", &GET_VARIABLE( g_variables.m_air_stuck_display_screen, bool ) );
						ImGui::SliderFloat( "extra reach", &GET_VARIABLE( g_variables.m_air_stuck_extra_reach, float ), 0.f, 32.f, "%.2f" );
						ImGui::SliderFloat( "fan budget", &GET_VARIABLE( g_variables.m_air_stuck_budget, float ), 5.f, 90.f, "%.0f%%" );
					},
					ImVec2( 220.f, -1.f ) );
			}

			ImGui::Checkbox( "wall climb", &GET_VARIABLE( g_variables.m_wall_climb, bool ) );
			if ( GET_VARIABLE( g_variables.m_wall_climb, bool ) ) {
				ImGui::OptionPopup(
					"wall climb settings",
					[ & ]( ) {
						ImGui::Label( "wall climb keybind" );
						ImGui::Keybind( "wall climb key", &GET_VARIABLE( g_variables.m_wall_climb_key, key_bind_t ) );
						ImGui::Checkbox( "prevent slow", &GET_VARIABLE( g_variables.m_wall_climb_prevent_slow, bool ) );
						ImGui::Checkbox( "crouch catches", &GET_VARIABLE( g_variables.m_wall_climb_duck, bool ) );
						ImGui::Checkbox( "pin catch ( stops on seams )", &GET_VARIABLE( g_variables.m_wall_climb_pin_catch, bool ) );
						ImGui::SliderInt( "spot reach", &GET_VARIABLE( g_variables.m_wall_climb_spot_reach, int ), 0, 32, "%d u" );
						ImGui::Checkbox( "steer to seam", &GET_VARIABLE( g_variables.m_wall_climb_seam_steer, bool ) );
						if ( GET_VARIABLE( g_variables.m_wall_climb_seam_steer, bool ) )
							ImGui::SliderFloat( "delta strafe scale", &GET_VARIABLE( g_variables.m_wall_climb_steer_scale, float ), 0.25f, 2.f, "%.2fx" );
						ImGui::Checkbox( "visualize##wcviz", &GET_VARIABLE( g_variables.m_wall_climb_visualize, bool ) );
						ImGui::ColorEdit4( "visualize color##wcvizcolor", &GET_VARIABLE( g_variables.m_wall_climb_visualize_color, c_color ),
						                   color_picker_alpha_flags );
						ImGui::Checkbox( "show wall slope", &GET_VARIABLE( g_variables.m_show_wall_slope, bool ) );
						if ( GET_VARIABLE( g_variables.m_show_wall_slope, bool ) ) {
							ImGui::Label( "show wall slope key" );
							ImGui::Keybind( "show wall slope key", &GET_VARIABLE( g_variables.m_show_wall_slope_key, key_bind_t ) );
						}
					},
					ImVec2( 220.f, -1.f ) );
			}

			ImGui::Checkbox( "fire man", &GET_VARIABLE( g_variables.m_fire_man, bool ) );
			if ( GET_VARIABLE( g_variables.m_fire_man, bool ) ) {
				ImGui::OptionPopup(
					"fire man settings",
					[ & ]( ) {
						ImGui::Label( "fire man keybind" );
						ImGui::Keybind( "fire man key", &GET_VARIABLE( g_variables.m_fire_man_key, key_bind_t ) );

						ImGui::Checkbox( "approach", &GET_VARIABLE( g_variables.m_fire_man_approach, bool ) );

						ImGui::SliderFloat( "reach##frreach", &GET_VARIABLE( g_variables.m_fire_man_reach, float ), 64.f, 640.f, "%.0f u" );

						ImGui::Checkbox( "drop in", &GET_VARIABLE( g_variables.m_fire_man_drop_in, bool ) );
						/* stops stray keys strafing you out of the shaft; eats fwd/side/duck, never jump */
						if ( GET_VARIABLE( g_variables.m_fire_man_drop_in, bool ) )
							ImGui::Checkbox( "lock movement", &GET_VARIABLE( g_variables.m_fire_man_drop_in_lock, bool ) );

						ImGui::Checkbox( "crouch jump", &GET_VARIABLE( g_variables.m_fire_man_crouch_jump, bool ) );

						ImGui::Checkbox( "sides only", &GET_VARIABLE( g_variables.m_fire_man_side_only, bool ) );
						if ( GET_VARIABLE( g_variables.m_fire_man_side_only, bool ) )
							ImGui::Keybind( "sides only key", &GET_VARIABLE( g_variables.m_fire_man_side_only_key, key_bind_t ) );

						ImGui::Checkbox( "push off early", &GET_VARIABLE( g_variables.m_fire_man_push_off, bool ) );
					},
					ImVec2( 220.f, -1.f ) );
			}

			ImGui::Checkbox( "air freeze", &GET_VARIABLE( g_variables.m_air_freeze, bool ) );
			if ( GET_VARIABLE( g_variables.m_air_freeze, bool ) ) {
				ImGui::OptionPopup(
					"air freeze settings",
					[ & ]( ) {
						ImGui::Label( "air freeze keybind" );
						ImGui::Keybind( "air freeze key", &GET_VARIABLE( g_variables.m_air_freeze_key, key_bind_t ) );
					},
					ImVec2( 220.f, -1.f ) );
			}
		}
		menu_group_end( );

		menu_columns_next( );

		if ( menu_group_begin( "other" ) ) {
			ImGui::Checkbox( "128 tick fix", &GET_VARIABLE( g_variables.m_tick_fix_128, bool ) );

			ImGui::Checkbox( "smooth demo view", &GET_VARIABLE( g_variables.m_silent_view, bool ) );

			ImGui::Checkbox( "pixel surf fix", &GET_VARIABLE( g_variables.m_pixel_surf_fix, bool ) );

			ImGui::Checkbox( "auto align", &GET_VARIABLE( g_variables.m_auto_align, bool ) );
			if ( GET_VARIABLE( g_variables.m_auto_align, bool ) ) {
				ImGui::OptionPopup(
					"auto align settings",
					[ & ]( ) { ImGui::Checkbox( "pixel jumps over pixelsurfs", &GET_VARIABLE( g_variables.m_auto_align_pixel_jump, bool ) ); },
					ImVec2( 220.f, -1.f ) );
			}

			ImGui::Checkbox( "nulls", &GET_VARIABLE( g_variables.m_nulls, bool ) );

			ImGui::Checkbox( "half sideways bind", &GET_VARIABLE( g_variables.m_hsw, bool ) );
			if ( GET_VARIABLE( g_variables.m_hsw, bool ) ) {
				ImGui::OptionPopup(
					"half sideways settings",
					[ & ]( ) {
						ImGui::Label( "half sideways keybind" );
						ImGui::Keybind( "half sideways key", &GET_VARIABLE( g_variables.m_hsw_key, key_bind_t ) );
						ImGui::Combo( "side##hsw", &GET_VARIABLE( g_variables.m_hsw_side, int ), "left\0right\0" );
						ImGui::Checkbox( "allow W / S inputs in air", &GET_VARIABLE( g_variables.m_hsw_allow_w_s, bool ) );
						ImGui::Checkbox( "pre on ground", &GET_VARIABLE( g_variables.m_hsw_pre_onground, bool ) );
						if ( GET_VARIABLE( g_variables.m_hsw_pre_onground, bool ) )
							ImGui::Checkbox( "allow A / D inputs on ground", &GET_VARIABLE( g_variables.m_hsw_allow_a_d, bool ) );
					},
					ImVec2( 220.f, -1.f ) );
			}

			ImGui::Checkbox( "auto one hop", &GET_VARIABLE( g_variables.m_auto_one_hop, bool ) );
			ImGui::Keybind( "auto one hop key", &GET_VARIABLE( g_variables.m_auto_one_hop_key, key_bind_t ) );

			ImGui::Checkbox( "blockbot", &GET_VARIABLE( g_variables.m_blockbot, bool ) );
			ImGui::Keybind( "blockbot key", &GET_VARIABLE( g_variables.m_blockbot_key, key_bind_t ) );

			safe_checkbox( "no crouch cooldown", &GET_VARIABLE( g_variables.m_no_crouch_cooldown, bool ) );

			ImGui::Checkbox( "fast stop", &GET_VARIABLE( g_variables.m_fast_stop, bool ) );

			ImGui::Checkbox( "pixel finder", &GET_VARIABLE( g_variables.m_pixel_finder, bool ) );
			if ( GET_VARIABLE( g_variables.m_pixel_finder, bool ) ) {
				ImGui::OptionPopup(
					"pixel finder settings",
					[ & ]( ) {
						ImGui::Label( "scan key" );
						ImGui::Keybind( "pixel finder key", &GET_VARIABLE( g_variables.m_pixel_finder_key, key_bind_t ) );
						ImGui::Checkbox( "pixelsurfs", &GET_VARIABLE( g_variables.m_pixel_finder_pixelsurfs, bool ) );
						ImGui::Checkbox( "texturebugs", &GET_VARIABLE( g_variables.m_pixel_finder_texturebugs, bool ) );
						ImGui::Checkbox( "headbounces", &GET_VARIABLE( g_variables.m_pixel_finder_headbounces, bool ) );
						ImGui::Checkbox( "pixeljumps", &GET_VARIABLE( g_variables.m_pixel_finder_pixeljumps, bool ) );
						const pf_map_state_t ms = pf_map_scan_get( );
						const std::string pm_label =
							ms.busy ? std::format( "cancel scan  {}%###pf map scan", ms.total > 0 ? ms.done * 100 / ms.total : 0 ) : std::string( "search whole map###pf map scan" );
						if ( ImGui::Button( pm_label.c_str( ), ImVec2( -1.f, 15.f ) ) )
							pf_map_scan_toggle( );
						ImGui::Checkbox( "map texturebugs", &GET_VARIABLE( g_variables.m_pixel_finder_map_texturebugs, bool ) );
						ImGui::Checkbox( "show map lines", &GET_VARIABLE( g_variables.m_pixel_finder_map_lines, bool ) );
						if ( ms.busy || ms.lines > 0 || ms.tb > 0 )
							ImGui::TextDisabled( "seam %d / %d   %d surf%s   %d tb line%s", ms.done, ms.total, ms.lines, ms.lines == 1 ? "" : "s", ms.tb,
							                     ms.tb == 1 ? "" : "s" );
					},
					ImVec2( 220.f, -1.f ) );
			}

			ImGui::Checkbox( "pixel calculator", &GET_VARIABLE( g_variables.m_pixel_calc, bool ) );
			if ( GET_VARIABLE( g_variables.m_pixel_calc, bool ) ) {
				ImGui::OptionPopup(
					"pixel calculator settings",
					[ & ]( ) {
						ImGui::Label( "aim key" );
						ImGui::Keybind( "pixel calculator place key", &GET_VARIABLE( g_variables.m_pixel_calc_aim_key, key_bind_t ) );
						ImGui::Label( "solve key" );
						ImGui::Keybind( "pixel calculator solve key", &GET_VARIABLE( g_variables.m_pixel_calc_solve_key, key_bind_t ) );
						ImGui::Checkbox( "show point", &GET_VARIABLE( g_variables.m_pixel_calc_show_point, bool ) );
						ImGui::Combo( "type##pixel calculator type", &GET_VARIABLE( g_variables.m_pixel_calc_type, int ), "pixelsurf\0pixeljump\0headbounce\0" );
						ImGui::Checkbox( "advanced readout##pixel calculator", &GET_VARIABLE( g_variables.m_pixel_calc_advanced_readout, bool ) );
						ImGui::Label( "allowed jumps" );
						move_mask_combo( "##pixel calculator jump types", GET_VARIABLE( g_variables.m_pixel_calc_moves, int ) );
					},
					ImVec2( 220.f, -1.f ) );
			}

			ImGui::Checkbox( "distance calculator", &GET_VARIABLE( g_variables.m_dist_calc, bool ) );
			if ( GET_VARIABLE( g_variables.m_dist_calc, bool ) ) {
				ImGui::OptionPopup(
					"distance calculator settings",
					[ & ]( ) {
						ImGui::Label( "point key" );
						ImGui::Keybind( "distance calculator key", &GET_VARIABLE( g_variables.m_dist_calc_key, key_bind_t ) );
					},
					ImVec2( 220.f, -1.f ) );
			}

			ImGui::Checkbox( "practice window", &GET_VARIABLE( g_variables.m_practice_window, bool ) );
			if ( GET_VARIABLE( g_variables.m_practice_window, bool ) ) {
				ImGui::OptionPopup(
					"practice window settings",
					[ & ]( ) {
						ImGui::Label( "practice checkpoint key" );
						ImGui::Keybind( "practice cp key", &GET_VARIABLE( g_variables.m_practice_cp_key, key_bind_t ) );
						ImGui::Label( "practice teleport key" );
						ImGui::Keybind( "practice tp key", &GET_VARIABLE( g_variables.m_practice_tp_key, key_bind_t ) );
						ImGui::Checkbox( "show window", &GET_VARIABLE( g_variables.m_practice_window_show, bool ) );
					},
					ImVec2( 220.f, -1.f ) );
			}

			ImGui::Checkbox( "fake pov in demos", &GET_VARIABLE( g_variables.m_fake_pov, bool ) );
			if ( GET_VARIABLE( g_variables.m_fake_pov, bool ) ) {
				ImGui::OptionPopup(
					"fake pov settings",
					[ & ]( ) {
						ImGui::Label( "fake pov keybind" );
						ImGui::Keybind( "fake pov key", &GET_VARIABLE( g_variables.m_fake_pov_key, key_bind_t ) );
						ImGui::Combo( "angle##fake pov", &GET_VARIABLE( g_variables.m_fake_pov_mode, int ),
						              "right\0left\0up\0bottom\0backwards\0spinning\0" );
						if ( GET_VARIABLE( g_variables.m_fake_pov_mode, int ) == 5 )
							ImGui::SliderFloat( "spin speed##fake pov", &GET_VARIABLE( g_variables.m_fake_pov_spin_speed, float ), 10.f, 540.f, "%.0f deg/s" );
						ImGui::SliderFloat( "smoothing##fake pov", &GET_VARIABLE( g_variables.m_fake_pov_smooth, float ), 0.5f, 20.f, "%.1f" );
						ImGui::Checkbox( "snap view on disable##fake pov", &GET_VARIABLE( g_variables.m_fake_pov_snap_view, bool ) );

						ImGui::Checkbox( "indicator arrow##fake pov", &GET_VARIABLE( g_variables.m_fake_pov_arrow, bool ) );
						if ( GET_VARIABLE( g_variables.m_fake_pov_arrow, bool ) ) {
							ImGui::ColorEdit4( "arrow color##fake pov", &GET_VARIABLE( g_variables.m_fake_pov_arrow_color, c_color ), color_picker_alpha_flags );
							ImGui::SliderFloat( "arrow size##fake pov", &GET_VARIABLE( g_variables.m_fake_pov_arrow_size, float ), 4.f, 64.f, "%.0f" );
							ImGui::SliderFloat( "arrow distance##fake pov", &GET_VARIABLE( g_variables.m_fake_pov_arrow_dist, float ), 0.f, 400.f, "%.0f" );
						}
					},
					ImVec2( 220.f, -1.f ) );
			}

			ImGui::Checkbox( "jumpstats", &GET_VARIABLE( g_variables.m_jump_stats, bool ) );
			if ( GET_VARIABLE( g_variables.m_jump_stats, bool ) ) {
				ImGui::OptionPopup(
					"jumpstats settings",
					[ & ]( ) {
						ImGui::Checkbox( "show fails##jumpstats", &GET_VARIABLE( g_variables.m_jump_stats_show_fails, bool ) );
					},
					ImVec2( 220.f, -1.f ) );
			}
		}
		menu_group_end( );

		/* not real movement: local server only, client side fakes, +jetpack or y6_ast online */
		if ( menu_group_begin( "fake movement" ) ) {
			ImGui::Checkbox( "tung surf (local y6 ast)", &GET_VARIABLE( g_variables.m_tung_surf, bool ) );
			if ( GET_VARIABLE( g_variables.m_tung_surf, bool ) ) {
				ImGui::OptionPopup(
					"tung surf settings",
					[ & ]( ) {
						ImGui::Label( "tung surf keybind" );
						ImGui::Keybind( "tung surf key", &GET_VARIABLE( g_variables.m_tung_surf_key, key_bind_t ) );
						/* online the server never sees the snap: this assist makes it real there */
						ImGui::Checkbox( "y6 assist", &GET_VARIABLE( g_variables.m_tung_surf_y6, bool ) );
						ImGui::SliderFloat( "snap to pixelsurfs##tungsnap", &GET_VARIABLE( g_variables.m_tung_surf_snap, float ), 0.f, 32.f, "%.0f u" );
						/* a catch / ride needs a move INTO the wall: presses when your keys don't, never when they point away */
						ImGui::Checkbox( "press into wall##tungpress", &GET_VARIABLE( g_variables.m_tung_surf_press, bool ) );
					},
					ImVec2( 220.f, -1.f ) );
			}

			ImGui::Checkbox( "y6 ast bind", &GET_VARIABLE( g_variables.m_lirili_larila, bool ) );
			if ( GET_VARIABLE( g_variables.m_lirili_larila, bool ) ) {
				ImGui::OptionPopup(
					"y6 ast bind settings",
					[ & ]( ) {
						ImGui::Label( "y6 ast keybind" );
						ImGui::Keybind( "y6 ast key", &GET_VARIABLE( g_variables.m_lirili_larila_key, key_bind_t ) );
					},
					ImVec2( 220.f, -1.f ) );
			}

			ImGui::Checkbox( "hide y6o ps", &GET_VARIABLE( g_variables.m_hide_y6o_ps, bool ) );

			ImGui::Checkbox( "edge surf", &GET_VARIABLE( g_variables.m_edge_surf, bool ) );
			if ( GET_VARIABLE( g_variables.m_edge_surf, bool ) ) {
				ImGui::OptionPopup(
					"edge surf settings",
					[ & ]( ) {
						ImGui::Label( "edge surf keybind" );
						ImGui::Keybind( "edge surf key", &GET_VARIABLE( g_variables.m_edge_surf_key, key_bind_t ) );
						ImGui::SliderFloat( "catch reach (local)##esurfreach", &GET_VARIABLE( g_variables.m_edge_surf_reach, float ), 0.1f, 32.f, "%.1f u" );
						ImGui::SliderFloat( "lead range##esurfrange", &GET_VARIABLE( g_variables.m_edge_surf_range, float ), 8.f, 512.f, "%.0f u" );
					},
					ImVec2( 220.f, -1.f ) );
			}

			ImGui::Checkbox( "poop joomp", &GET_VARIABLE( g_variables.m_poop_ladder_jump, bool ) );
			if ( GET_VARIABLE( g_variables.m_poop_ladder_jump, bool ) ) {
				ImGui::OptionPopup(
					"poop joomp settings",
					[ & ]( ) {
						ImGui::Label( "poop joomp keybind" );
						ImGui::Keybind( "poop joomp key", &GET_VARIABLE( g_variables.m_poop_ladder_jump_key, key_bind_t ) );
						ImGui::SliderFloat( "jump strength (local)", &GET_VARIABLE( g_variables.m_poop_ladder_jump_strength, float ), 100.f, 500.f, "%.0f%%" );
						ImGui::SliderFloat( "jetpack height (online)", &GET_VARIABLE( g_variables.m_poop_ladder_jump_height, float ), 10.f, 2000.f, "%.0f u" );
					},
					ImVec2( 220.f, -1.f ) );
			}
		}
		menu_group_end( );

		menu_columns_end( );
		break;
	}
	case 1: {
		menu_columns_begin( );

		if ( menu_group_begin( "indicators" ) ) {
			ImGui::Checkbox( "velocity indicator", &GET_VARIABLE( g_variables.m_velocity_indicator, bool ) );
			if ( GET_VARIABLE( g_variables.m_velocity_indicator, bool ) ) {
				ImGui::OptionPopup(
					"velocity indicator settings",
					[ & ]( ) {
						ImGui::Combo( "style##velocity indicator style", &GET_VARIABLE( g_variables.m_velocity_indicator_style, int ),
						              k_indicator_style_names );

						if ( GET_VARIABLE( g_variables.m_velocity_indicator_style, int ) == indicator_style_smooth )
							ImGui::Combo( "comes from##velocity indicator", &GET_VARIABLE( g_variables.m_velocity_indicator_smooth_origin, int ),
							              k_smooth_origin_names );

						if ( GET_VARIABLE( g_variables.m_velocity_indicator_style, int ) == indicator_style_kamidere )
							ImGui::ColorEdit4( "kami shadow color##velocity indicator", &GET_VARIABLE( g_variables.m_indicator_kami_shadow_color, c_color ),
							                   color_picker_alpha_flags );

						if ( GET_VARIABLE( g_variables.m_velocity_indicator_custom_color, bool ) ) {
							ImGui::ColorEdit4( "velocity colors##velocity indicator color 1",
						                       &GET_VARIABLE( g_variables.m_velocity_indicator_color1, c_color ), color_picker_alpha_flags );

							ImGui::SameLine( );
							ImGui::ColorEdit4( "##velocity indicator color 2", &GET_VARIABLE( g_variables.m_velocity_indicator_color2, c_color ),
						                       color_picker_alpha_flags, 1 );
						} else {
							ImGui::ColorEdit4( "velocity colors##velocity indicator color 3",
						                       &GET_VARIABLE( g_variables.m_velocity_indicator_color3, c_color ), color_picker_alpha_flags );

							ImGui::SameLine( );
							ImGui::ColorEdit4( "##velocity indicator color 4", &GET_VARIABLE( g_variables.m_velocity_indicator_color4, c_color ),
						                       color_picker_alpha_flags, 1 );

							ImGui::SameLine( );
							ImGui::ColorEdit4( "##velocity indicator color 5", &GET_VARIABLE( g_variables.m_velocity_indicator_color5, c_color ),
						                       color_picker_alpha_flags, 2 );
						}

						ImGui::Checkbox( "show pre speed##velocity indicator",
					                     &GET_VARIABLE( g_variables.m_velocity_indicator_show_pre_speed, bool ) );
						if ( GET_VARIABLE( g_variables.m_velocity_indicator_show_pre_speed, bool ) &&
						     GET_VARIABLE( g_variables.m_velocity_indicator_style, int ) != indicator_style_kamibebra ) {
							ImGui::Combo( "pre speed position##velocity indicator", &GET_VARIABLE( g_variables.m_velocity_indicator_pre_layout, int ),
							              k_pre_layout_names );
							ImGui::SliderFloat( "pre speed scale##velocity indicator", &GET_VARIABLE( g_variables.m_velocity_indicator_pre_scale, float ), 0.25f,
							                    2.f, "%.2fx" );
							if ( GET_VARIABLE( g_variables.m_velocity_indicator_pre_layout, int ) == pre_layout_right )
								ImGui::Combo( "pre speed align##velocity indicator", &GET_VARIABLE( g_variables.m_velocity_indicator_pre_align, int ),
								              k_pre_align_names );
						}

						ImGui::Checkbox( "fade alpha##velocity indicator", &GET_VARIABLE( g_variables.m_velocity_indicator_fade_alpha, bool ) );

						ImGui::Checkbox( "custom color##velocity indicator",
					                     &GET_VARIABLE( g_variables.m_velocity_indicator_custom_color, bool ) );

						if ( GET_VARIABLE( g_variables.m_velocity_indicator_style, int ) != indicator_style_kamibebra )
							ImGui::Checkbox( "shadow##velocity indicator", &GET_VARIABLE( g_variables.m_velocity_indicator_shadow, bool ) );
						if ( GET_VARIABLE( g_variables.m_velocity_indicator_shadow, bool ) &&
						     GET_VARIABLE( g_variables.m_velocity_indicator_style, int ) != indicator_style_kamibebra ) {
							ImGui::SliderFloat( "shadow blur##velocity indicator", &GET_VARIABLE( g_variables.m_velocity_indicator_shadow_blur, float ), 0.f,
							                    20.f, "%.1f" );
							if ( GET_VARIABLE( g_variables.m_velocity_indicator_style, int ) != indicator_style_kamidere )
								ImGui::ColorEdit4( "shadow color##velocity indicator", &GET_VARIABLE( g_variables.m_velocity_indicator_shadow_color, c_color ),
								                   color_picker_alpha_flags );
							ImGui::SliderInt( "shadow x##velocity indicator", &GET_VARIABLE( g_variables.m_velocity_indicator_shadow_x, int ), -10, 10, "%d px" );
							ImGui::SliderInt( "shadow y##velocity indicator", &GET_VARIABLE( g_variables.m_velocity_indicator_shadow_y, int ), -10, 10, "%d px" );
						}

						if ( GET_VARIABLE( g_variables.m_velocity_indicator_style, int ) != indicator_style_kamibebra )
							ImGui::Checkbox( "outline##velocity indicator", &GET_VARIABLE( g_variables.m_velocity_indicator_outline, bool ) );
						if ( GET_VARIABLE( g_variables.m_velocity_indicator_outline, bool ) &&
						     GET_VARIABLE( g_variables.m_velocity_indicator_style, int ) != indicator_style_kamibebra ) {
							ImGui::ColorEdit4( "outline color##velocity indicator", &GET_VARIABLE( g_variables.m_velocity_indicator_outline_color, c_color ),
							                   color_picker_alpha_flags );
							ImGui::SliderInt( "outline thickness##velocity indicator", &GET_VARIABLE( g_variables.m_velocity_indicator_outline_px, int ), 1, 5,
							                  "%d px" );
						}

						ImGui::SliderFloat( "scale##velocity indicator", &GET_VARIABLE( g_variables.m_velocity_indicator_scale, float ), k_indicator_scale_min, 3.f, "%.2fx" );

						ImGui::SliderInt( "position##velocity indicator", &GET_VARIABLE( g_variables.m_velocity_indicator_padding, int ), 30,
					                      g_ctx.m_height );
					},
					ImVec2( 220.f, -1.f ) );
			}

			ImGui::Checkbox( "stamina indicator", &GET_VARIABLE( g_variables.m_stamina_indicator, bool ) );
			if ( GET_VARIABLE( g_variables.m_stamina_indicator, bool ) ) {
				ImGui::OptionPopup(
					"stamina indicator settings",
					[ & ]( ) {
						ImGui::Combo( "style##stamina indicator style", &GET_VARIABLE( g_variables.m_stamina_indicator_style, int ),
						              k_indicator_style_names );

						if ( GET_VARIABLE( g_variables.m_stamina_indicator_style, int ) == indicator_style_smooth )
							ImGui::Combo( "comes from##stamina indicator", &GET_VARIABLE( g_variables.m_stamina_indicator_smooth_origin, int ),
							              k_smooth_origin_names );

						if ( GET_VARIABLE( g_variables.m_stamina_indicator_style, int ) == indicator_style_kamidere )
							ImGui::ColorEdit4( "kami shadow color##stamina indicator", &GET_VARIABLE( g_variables.m_indicator_kami_shadow_color, c_color ),
							                   color_picker_alpha_flags );

						ImGui::ColorEdit4( "stamina colors##stamina indicator color 1",
					                       &GET_VARIABLE( g_variables.m_stamina_indicator_color1, c_color ), color_picker_alpha_flags );

						ImGui::SameLine( );
						ImGui::ColorEdit4( "##stamina indicator color 2", &GET_VARIABLE( g_variables.m_stamina_indicator_color2, c_color ),
					                       color_picker_alpha_flags, 1 );

						ImGui::Checkbox( "show pre speed##stamina indicator",
					                     &GET_VARIABLE( g_variables.m_stamina_indicator_show_pre_speed, bool ) );
						if ( GET_VARIABLE( g_variables.m_stamina_indicator_show_pre_speed, bool ) &&
						     GET_VARIABLE( g_variables.m_stamina_indicator_style, int ) != indicator_style_kamibebra ) {
							ImGui::Combo( "pre speed position##stamina indicator", &GET_VARIABLE( g_variables.m_stamina_indicator_pre_layout, int ),
							              k_pre_layout_names );
							ImGui::SliderFloat( "pre speed scale##stamina indicator", &GET_VARIABLE( g_variables.m_stamina_indicator_pre_scale, float ), 0.25f,
							                    2.f, "%.2fx" );
							if ( GET_VARIABLE( g_variables.m_stamina_indicator_pre_layout, int ) == pre_layout_right )
								ImGui::Combo( "pre speed align##stamina indicator", &GET_VARIABLE( g_variables.m_stamina_indicator_pre_align, int ),
								              k_pre_align_names );
						}

						ImGui::Checkbox( "fade alpha##stamina indicator", &GET_VARIABLE( g_variables.m_stamina_indicator_fade_alpha, bool ) );

						if ( GET_VARIABLE( g_variables.m_stamina_indicator_style, int ) != indicator_style_kamibebra )
							ImGui::Checkbox( "shadow##stamina indicator", &GET_VARIABLE( g_variables.m_stamina_indicator_shadow, bool ) );
						if ( GET_VARIABLE( g_variables.m_stamina_indicator_shadow, bool ) &&
						     GET_VARIABLE( g_variables.m_stamina_indicator_style, int ) != indicator_style_kamibebra ) {
							ImGui::SliderFloat( "shadow blur##stamina indicator", &GET_VARIABLE( g_variables.m_stamina_indicator_shadow_blur, float ), 0.f,
							                    20.f, "%.1f" );
							if ( GET_VARIABLE( g_variables.m_stamina_indicator_style, int ) != indicator_style_kamidere )
								ImGui::ColorEdit4( "shadow color##stamina indicator", &GET_VARIABLE( g_variables.m_stamina_indicator_shadow_color, c_color ),
								                   color_picker_alpha_flags );
							ImGui::SliderInt( "shadow x##stamina indicator", &GET_VARIABLE( g_variables.m_stamina_indicator_shadow_x, int ), -10, 10, "%d px" );
							ImGui::SliderInt( "shadow y##stamina indicator", &GET_VARIABLE( g_variables.m_stamina_indicator_shadow_y, int ), -10, 10, "%d px" );
						}

						if ( GET_VARIABLE( g_variables.m_stamina_indicator_style, int ) != indicator_style_kamibebra )
							ImGui::Checkbox( "outline##stamina indicator", &GET_VARIABLE( g_variables.m_stamina_indicator_outline, bool ) );
						if ( GET_VARIABLE( g_variables.m_stamina_indicator_outline, bool ) &&
						     GET_VARIABLE( g_variables.m_stamina_indicator_style, int ) != indicator_style_kamibebra ) {
							ImGui::ColorEdit4( "outline color##stamina indicator", &GET_VARIABLE( g_variables.m_stamina_indicator_outline_color, c_color ),
							                   color_picker_alpha_flags );
							ImGui::SliderInt( "outline thickness##stamina indicator", &GET_VARIABLE( g_variables.m_stamina_indicator_outline_px, int ), 1, 5,
							                  "%d px" );
						}

						ImGui::SliderFloat( "scale##stamina indicator", &GET_VARIABLE( g_variables.m_stamina_indicator_scale, float ), k_indicator_scale_min, 3.f, "%.2fx" );

						ImGui::SliderInt( "position##stamina indicator", &GET_VARIABLE( g_variables.m_stamina_indicator_padding, int ), 30,
					                      g_ctx.m_height );
					},
					ImVec2( 220.f, -1.f ) );
			}

			ImGui::Checkbox( "velocity graph", &GET_VARIABLE( g_variables.m_velocity_graph, bool ) );
			if ( GET_VARIABLE( g_variables.m_velocity_graph, bool ) ) {
				ImGui::OptionPopup(
					"velocity graph settings",
					[ & ]( ) {
						ImGui::ColorEdit4( "line color##velocity graph", &GET_VARIABLE( g_variables.m_velocity_graph_color, c_color ), color_picker_alpha_flags );
						ImGui::SliderFloat( "thickness##velocity graph", &GET_VARIABLE( g_variables.m_velocity_graph_thickness, float ), 0.5f, 6.f, "%.1f" );

						ImGui::Checkbox( "gain colors##velocity graph", &GET_VARIABLE( g_variables.m_velocity_graph_gain_colors, bool ) );
						if ( GET_VARIABLE( g_variables.m_velocity_graph_gain_colors, bool ) ) {
							ImGui::ColorEdit4( "gain / loss##velocity graph gain", &GET_VARIABLE( g_variables.m_velocity_graph_color_gain, c_color ),
							                   color_picker_alpha_flags );
							ImGui::SameLine( );
							ImGui::ColorEdit4( "##velocity graph loss", &GET_VARIABLE( g_variables.m_velocity_graph_color_loss, c_color ), color_picker_alpha_flags,
							                   1 );
						}

						ImGui::Checkbox( "fill##velocity graph", &GET_VARIABLE( g_variables.m_velocity_graph_fill, bool ) );
						if ( GET_VARIABLE( g_variables.m_velocity_graph_fill, bool ) )
							ImGui::ColorEdit4( "fill color##velocity graph", &GET_VARIABLE( g_variables.m_velocity_graph_fill_color, c_color ),
							                   color_picker_alpha_flags );

						ImGui::Checkbox( "fade sides##velocity graph", &GET_VARIABLE( g_variables.m_velocity_graph_fade_sides, bool ) );
						if ( GET_VARIABLE( g_variables.m_velocity_graph_fade_sides, bool ) )
							ImGui::SliderFloat( "fade width##velocity graph", &GET_VARIABLE( g_variables.m_velocity_graph_fade, float ), 0.01f, 0.5f, "%.2f" );

						ImGui::Checkbox( "jump speeds##velocity graph", &GET_VARIABLE( g_variables.m_velocity_graph_jump_speeds, bool ) );
						if ( GET_VARIABLE( g_variables.m_velocity_graph_jump_speeds, bool ) ) {
							ImGui::ColorEdit4( "text color##velocity graph", &GET_VARIABLE( g_variables.m_velocity_graph_text_color, c_color ),
							                   color_picker_alpha_flags );
							ImGui::Checkbox( "text shadow##velocity graph", &GET_VARIABLE( g_variables.m_velocity_graph_text_shadow, bool ) );
							ImGui::SliderFloat( "text scale##velocity graph", &GET_VARIABLE( g_variables.m_velocity_graph_text_scale, float ), k_indicator_scale_min,
							                    3.f, "%.2fx" );
						}

						ImGui::SliderInt( "length##velocity graph", &GET_VARIABLE( g_variables.m_velocity_graph_length, int ), 16, 1024 );
						ImGui::SliderFloat( "max speed##velocity graph", &GET_VARIABLE( g_variables.m_velocity_graph_max, float ), 0.f, 1000.f,
						                    GET_VARIABLE( g_variables.m_velocity_graph_max, float ) <= 0.f ? "auto" : "%.0f" );
						ImGui::SliderInt( "width##velocity graph", &GET_VARIABLE( g_variables.m_velocity_graph_width, int ), 50, g_ctx.m_width );
						ImGui::SliderInt( "height##velocity graph", &GET_VARIABLE( g_variables.m_velocity_graph_height, int ), 10, 400 );
						ImGui::SliderInt( "offset x##velocity graph", &GET_VARIABLE( g_variables.m_velocity_graph_offset_x, int ), -g_ctx.m_width / 2,
						                  g_ctx.m_width / 2 );
						ImGui::SliderInt( "position##velocity graph", &GET_VARIABLE( g_variables.m_velocity_graph_position, int ), 0, g_ctx.m_height );
					},
					ImVec2( 220.f, -1.f ) );
			}

			ImGui::Checkbox( "keybind indicators", &GET_VARIABLE( g_variables.m_key_indicators_enable, bool ) );
			if ( GET_VARIABLE( g_variables.m_key_indicators_enable, bool ) ) {
				ImGui::OptionPopup(
					"keybind indicator settings",
					[ & ]( ) {
						ImGui::Combo( "style##keybind indicator style", &GET_VARIABLE( g_variables.m_key_indicators_style, int ),
						              k_indicator_style_names );

						if ( GET_VARIABLE( g_variables.m_key_indicators_style, int ) == indicator_style_smooth )
							ImGui::Combo( "comes from##keybind indicators", &GET_VARIABLE( g_variables.m_key_indicators_smooth_origin, int ),
							              k_smooth_origin_names );

						if ( GET_VARIABLE( g_variables.m_key_indicators_style, int ) == indicator_style_static )
							ImGui::SliderFloat( "fade in##keybind indicators", &GET_VARIABLE( g_variables.m_key_indicators_static_fade, float ), 0.f, 1.f,
							                    "%.2f s" );

						if ( GET_VARIABLE( g_variables.m_key_indicators_style, int ) == indicator_style_kamidere )
							ImGui::ColorEdit4( "kami shadow color##keybind indicator", &GET_VARIABLE( g_variables.m_indicator_kami_shadow_color, c_color ),
							                   color_picker_alpha_flags );

						const bool kamibebra_keys = GET_VARIABLE( g_variables.m_key_indicators_style, int ) == indicator_style_kamibebra;
						if ( kamibebra_keys )
							ImGui::ColorEdit4( "detect accent##keybind indicator", &GET_VARIABLE( g_variables.m_indicator_kamibebra_accent, c_color ),
							                   color_picker_alpha_flags );

						ImGui::ColorEdit4( "keybind colors##keybind color", &GET_VARIABLE( g_variables.m_key_color, c_color ),
					                       color_picker_alpha_flags );

						ImGui::SameLine( );
						ImGui::ColorEdit4( "##keybind success color", &GET_VARIABLE( g_variables.m_key_color_success, c_color ),
					                       color_picker_alpha_flags, 1 );

						ImGui::ColorEdit4( "in prediction color##keybind predict color", &GET_VARIABLE( g_variables.m_key_color_predict, c_color ),
						                   color_picker_alpha_flags );

						if ( !kamibebra_keys )
							ImGui::Checkbox( "horizontal layout", &GET_VARIABLE( g_variables.m_key_indicators_horizontal, bool ) );

						ImGui::SliderInt( "spacing##keybind indicators", &GET_VARIABLE( g_variables.m_key_indicators_spacing, int ), -10, 40, "%d px" );

						if ( !kamibebra_keys )
							ImGui::Checkbox( "shadow##keybind indicators", &GET_VARIABLE( g_variables.m_key_indicators_shadow, bool ) );
						if ( GET_VARIABLE( g_variables.m_key_indicators_shadow, bool ) && !kamibebra_keys ) {
							ImGui::SliderFloat( "shadow blur##keybind indicators", &GET_VARIABLE( g_variables.m_key_indicators_shadow_blur, float ), 0.f, 20.f,
							                    "%.1f" );

							if ( GET_VARIABLE( g_variables.m_key_indicators_style, int ) != indicator_style_kamidere )
								ImGui::ColorEdit4( "shadow color##keybind indicators", &GET_VARIABLE( g_variables.m_key_indicators_shadow_color, c_color ),
								                   color_picker_alpha_flags );

							ImGui::Checkbox( "detect on shadow", &GET_VARIABLE( g_variables.m_key_indicators_shadow_detect, bool ) );
							ImGui::SliderInt( "shadow x##keybind indicators", &GET_VARIABLE( g_variables.m_key_indicators_shadow_x, int ), -10, 10, "%d px" );
							ImGui::SliderInt( "shadow y##keybind indicators", &GET_VARIABLE( g_variables.m_key_indicators_shadow_y, int ), -10, 10, "%d px" );
						}

						if ( !kamibebra_keys )
							ImGui::Checkbox( "outline##keybind indicators", &GET_VARIABLE( g_variables.m_key_indicators_outline, bool ) );
						if ( GET_VARIABLE( g_variables.m_key_indicators_outline, bool ) && !kamibebra_keys ) {
							ImGui::ColorEdit4( "outline color##keybind indicators", &GET_VARIABLE( g_variables.m_key_indicators_outline_color, c_color ),
							                   color_picker_alpha_flags );
							ImGui::SliderInt( "outline thickness##keybind indicators", &GET_VARIABLE( g_variables.m_key_indicators_outline_px, int ), 1, 5,
							                  "%d px" );
						}

						ImGui::SliderFloat( "scale##keybind indicators", &GET_VARIABLE( g_variables.m_key_indicators_scale, float ), k_indicator_scale_min, 3.f, "%.2fx" );

						ImGui::Checkbox( "detection particles", &GET_VARIABLE( g_variables.m_key_indicators_particles, bool ) );
						if ( GET_VARIABLE( g_variables.m_key_indicators_particles, bool ) ) {
							ImGui::Checkbox( "random colors##key particles", &GET_VARIABLE( g_variables.m_key_indicators_particle_random, bool ) );
							if ( !GET_VARIABLE( g_variables.m_key_indicators_particle_random, bool ) )
								ImGui::ColorEdit4( "particle color##key particles", &GET_VARIABLE( g_variables.m_key_indicators_particle_color, c_color ),
								                   color_picker_alpha_flags );
							ImGui::SliderInt( "amount##key particles", &GET_VARIABLE( g_variables.m_key_indicators_particle_amount, int ), 1, 60 );
							ImGui::SliderFloat( "size##key particles", &GET_VARIABLE( g_variables.m_key_indicators_particle_size, float ), 1.f, 12.f, "%.1f" );
							ImGui::SliderFloat( "speed##key particles", &GET_VARIABLE( g_variables.m_key_indicators_particle_speed, float ), 10.f, 600.f, "%.0f" );
							ImGui::SliderFloat( "gravity##key particles", &GET_VARIABLE( g_variables.m_key_indicators_particle_gravity, float ), -300.f, 800.f,
							                    "%.0f" );
							ImGui::SliderFloat( "life##key particles", &GET_VARIABLE( g_variables.m_key_indicators_particle_life, float ), 0.2f, 3.f, "%.1f s" );
							ImGui::SliderFloat( "cooldown##key particles", &GET_VARIABLE( g_variables.m_key_indicators_particle_cooldown, float ), 0.f, 2.f,
							                    "%.2f s" );
						}

						ImGui::SliderInt( "position##sub indicators", &GET_VARIABLE( g_variables.m_key_indicators_position, int ), 30,
					                      g_ctx.m_height );

						ImGui::MultiCombo( "displayed keybinds", GET_VARIABLE( g_variables.m_key_indicators, std::vector< bool > ),
					                       { "edgebug", "pixelsurf", "edgejump", "longjump", "delayhop", "minijump", "jumpbug", "texturebug",
					                         "airstuck", "wallclimb", "autostrafe", "ps assist", "fireman", "ladder bug", "ladder glide",
					                         "freelook climb", "fast ladder", "one hop", "auto crouch", "auto bounce", "tung surf", "edge skip", "half sideways",
					                         "zeus bug", "air freeze", "edge surf" },
					                       GET_VARIABLE( g_variables.m_key_indicators, std::vector< bool > ).size( ) );

						/* per-bind label override; empty = stock label (hint). buffer 16 = 15 chars keeps the
						   string in MSVC's SSO buffer, so paint_traverse copying mid-edit never hits freed heap */
						ImGui::Checkbox( "custom labels", &GET_VARIABLE( g_variables.m_key_indicators_custom_labels, bool ) );
						if ( GET_VARIABLE( g_variables.m_key_indicators_custom_labels, bool ) ) {
							auto&       labels                    = GET_VARIABLE( g_variables.m_key_indicators_labels, std::vector< std::string > );
							static char buffers[ label_max ][ 16 ] = { };

							float name_width = 0.f;
							for ( int i = 0; i < label_max; i++ )
								name_width = ImMax( name_width, ImGui::CalcTextSize( k_keybind_labels[ i ][ 0 ] ).x );
							name_width += 8.f;

							for ( int i = 0; i < label_max && i < static_cast< int >( labels.size( ) ); i++ ) {
								ImGui::PushID( i );
								ImGui::SetCursorPosY( ImGui::GetCursorPosY( ) - 7.f - ( i > 0 ? ImGui::GetStyle( ).ItemSpacing.y : 0.f ) );
								ImGui::SetCursorPosX( ImGui::GetCursorPosX( ) + name_width );
								text_buffer_sync( "##label", labels[ i ], buffers[ i ], sizeof( buffers[ i ] ), k_keybind_labels[ i ][ 1 ] );
								const ImVec2 box = ImGui::GetItemRectMin( );
								ImGui::GetWindowDrawList( )->AddText( ImVec2( box.x - name_width, box.y + ( 17.f - ImGui::GetFontSize( ) ) * 0.5f ),
								                                      ImGui::GetColorU32( ImGuiCol_Text ), k_keybind_labels[ i ][ 0 ] );
								ImGui::PopID( );
							}
						}
					},
					ImVec2( 220.f, -1.f ) );
			}

			ImGui::Checkbox( "keystroke indicators", &GET_VARIABLE( g_variables.m_key_press_indicator, bool ) );
			if ( GET_VARIABLE( g_variables.m_key_press_indicator, bool ) ) {
				ImGui::OptionPopup(
					"keystroke indicator settings",
					[ & ]( ) {
						ImGui::Combo( "layout##key press", &GET_VARIABLE( g_variables.m_key_press_layout, int ),
						              "clarity (a w d / c s j)\0keyboard (c w j / a s d)\0" );

						ImGui::Combo( "released##key press", &GET_VARIABLE( g_variables.m_key_press_released, int ), "underscore\0dim letter\0hidden\0" );

						ImGui::ColorEdit4( "key colors##key press color", &GET_VARIABLE( g_variables.m_key_press_color, c_color ), color_picker_alpha_flags );

						ImGui::SameLine( );
						ImGui::ColorEdit4( "##key press released color", &GET_VARIABLE( g_variables.m_key_press_color_released, c_color ),
						                   color_picker_alpha_flags, 1 );

						ImGui::Checkbox( "duck / jump##key press", &GET_VARIABLE( g_variables.m_key_press_duck_jump, bool ) );
						ImGui::Checkbox( "mouse direction##key press", &GET_VARIABLE( g_variables.m_key_press_mouse, bool ) );

						ImGui::Checkbox( "shadow##key press", &GET_VARIABLE( g_variables.m_key_press_shadow, bool ) );
						if ( GET_VARIABLE( g_variables.m_key_press_shadow, bool ) ) {
							ImGui::SliderFloat( "shadow blur##key press", &GET_VARIABLE( g_variables.m_key_press_shadow_blur, float ), 0.f, 20.f, "%.1f" );
							ImGui::SliderInt( "shadow x##key press", &GET_VARIABLE( g_variables.m_key_press_shadow_x, int ), -10, 10, "%d px" );
							ImGui::SliderInt( "shadow y##key press", &GET_VARIABLE( g_variables.m_key_press_shadow_y, int ), -10, 10, "%d px" );
						}

						ImGui::Checkbox( "outline##key press", &GET_VARIABLE( g_variables.m_key_press_outline, bool ) );
						if ( GET_VARIABLE( g_variables.m_key_press_outline, bool ) ) {
							ImGui::ColorEdit4( "outline color##key press", &GET_VARIABLE( g_variables.m_key_press_outline_color, c_color ), color_picker_alpha_flags );
							ImGui::SliderInt( "outline thickness##key press", &GET_VARIABLE( g_variables.m_key_press_outline_px, int ), 1, 5, "%d px" );
						}

						ImGui::SliderFloat( "fade##key press", &GET_VARIABLE( g_variables.m_key_press_fade, float ), 0.f, 0.3f, "%.2f s" );

						ImGui::SliderInt( "key gap##key press", &GET_VARIABLE( g_variables.m_key_press_gap_x, int ), 0, 60 );
						ImGui::SliderInt( "row gap##key press", &GET_VARIABLE( g_variables.m_key_press_gap_y, int ), -10, 60 );

						ImGui::SliderFloat( "scale##key press", &GET_VARIABLE( g_variables.m_key_press_scale, float ), k_indicator_scale_min, 3.f, "%.2fx" );

						ImGui::SliderInt( "position##key press", &GET_VARIABLE( g_variables.m_key_press_position, int ), 30,
						                  static_cast< int >( g_ctx.m_height ) );
						ImGui::SliderInt( "x offset##key press", &GET_VARIABLE( g_variables.m_key_press_offset_x, int ),
						                  -static_cast< int >( g_ctx.m_width / 2.f ), static_cast< int >( g_ctx.m_width / 2.f ) );
					},
					ImVec2( 220.f, -1.f ) );
			}
		}
		menu_group_end( );

		menu_columns_next( );

		if ( menu_group_begin( "detections" ) ) {
			/* chat detections: label list order must match e_detection_types */
			ImGui::Checkbox( "detections", &GET_VARIABLE( g_variables.m_detections, bool ) );
			if ( GET_VARIABLE( g_variables.m_detections, bool ) ) {
				ImGui::OptionPopup(
					"detection settings",
					[ & ]( ) {
						ImGui::MultiCombo( "detections", GET_VARIABLE( g_variables.m_detection_types, std::vector< bool > ),
					                       { "edgebug", "texturebug", "airstuck", "pixelsurf", "wallclimb" },
					                       GET_VARIABLE( g_variables.m_detection_types, std::vector< bool > ).size( ) );
						ImGui::Checkbox( "stack edgebugs##detstack", &GET_VARIABLE( g_variables.m_detection_stack_eb, bool ) );

						auto&       label_var = GET_VARIABLE( g_variables.m_detection_label, std::string );
						static char label_buffer[ 64 ] = { };
						text_buffer_sync( "chat label##detlabel", label_var, label_buffer, sizeof( label_buffer ) );
					},
					ImVec2( 220.f, -1.f ) );
			}

			ImGui::Checkbox( "edgebug visualize", &GET_VARIABLE( g_variables.m_edgebug_visualize, bool ) );
			if ( GET_VARIABLE( g_variables.m_edgebug_visualize, bool ) ) {
				ImGui::OptionPopup(
					"edgebug visualize settings",
					[ & ]( ) {
						ImGui::ColorEdit4( "outline color##ebvizcolor", &GET_VARIABLE( g_variables.m_edgebug_visualize_color, c_color ),
						                   color_picker_alpha_flags );
						ImGui::SliderFloat( "visualize thickness##ebvizthick",
						                    &GET_VARIABLE( g_variables.m_edgebug_visualize_thickness, float ), 0.5f, 10.f, "%.1f" );
						ImGui::Combo( "pad shape##ebvizshape", &GET_VARIABLE( g_variables.m_edgebug_visualize_shape, int ),
						              "square\0circle\0" );
						ImGui::Combo( "pad fill##ebvizfill", &GET_VARIABLE( g_variables.m_edgebug_visualize_fill, int ),
						              "outline\0filled\0filled + outline\0" );
						if ( GET_VARIABLE( g_variables.m_edgebug_visualize_fill, int ) != 0 ) {
							ImGui::ColorEdit4( "pad color##ebvizpadcolor", &GET_VARIABLE( g_variables.m_edgebug_visualize_pad_color, c_color ),
							                   color_picker_alpha_flags );
							ImGui::SliderFloat( "fill opacity##ebvizfilla",
							                    &GET_VARIABLE( g_variables.m_edgebug_visualize_fill_alpha, float ), 0.05f, 1.f, "%.2f" );
						}
						ImGui::SliderFloat( "pad size##ebvizsize", &GET_VARIABLE( g_variables.m_edgebug_visualize_size, float ),
						                    4.f, 64.f, "%.0f" );
						ImGui::Checkbox( "project onto geometry", &GET_VARIABLE( g_variables.m_edgebug_visualize_projected, bool ) );
					},
					ImVec2( 220.f, -1.f ) );
			}

			ImGui::Checkbox( "pixelsurf lines", &GET_VARIABLE( g_variables.m_pixel_surf_line_render, bool ) );
			if ( GET_VARIABLE( g_variables.m_pixel_surf_line_render, bool ) ) {
				ImGui::OptionPopup(
					"pixelsurf lines settings",
					[ & ]( ) {
						ImGui::ColorEdit4( "line color##pslinecolor", &GET_VARIABLE( g_variables.m_pixel_surf_line_color, c_color ),
						                   color_picker_alpha_flags );
						ImGui::Combo( "line style##pslinestyle", &GET_VARIABLE( g_variables.m_pixel_surf_line_style, int ),
						              "full lines\0dotted lines\0dots\0" );
						ImGui::SliderFloat( "line thickness##pslinethick", &GET_VARIABLE( g_variables.m_pixel_surf_line_thickness, float ), 0.5f,
						                    10.f, "%.1f" );
						ImGui::Checkbox( "full trajectory", &GET_VARIABLE( g_variables.m_pixel_surf_line_full_trajectory, bool ) );
					},
					ImVec2( 220.f, -1.f ) );
			}
		}
		menu_group_end( );

		menu_columns_end( );
		break;
	}
	case 2: {
		route_calc_page( );
		break;
	}
	case 3: {
		auto& mr = g_movement_recorder;

		static bool first_list = true;
		if ( first_list ) {
			mr.refresh_clips( );
			first_list = false;
		}

		menu_columns_begin( );

		if ( menu_group_begin( "recorder" ) ) {
			ImGui::Checkbox( "movement recorder", &GET_VARIABLE( g_variables.m_movement_rec, bool ) );
			ImGui::Checkbox( "show route lines", &GET_VARIABLE( g_variables.m_movement_rec_show_line, bool ) );
			ImGui::Checkbox( "route lines near start", &GET_VARIABLE( g_variables.m_movement_rec_preview_line, bool ) );
			ImGui::Checkbox( "show recorder timer", &GET_VARIABLE( g_variables.m_movement_rec_render, bool ) );
			if ( GET_VARIABLE( g_variables.m_movement_rec_render, bool ) ) {
				ImGui::OptionPopup(
					"recorder timer settings",
					[ & ]( ) {
						ImGui::Combo( "position##recorder timer", &GET_VARIABLE( g_variables.m_movement_rec_position, int ),
						              "top left\0bottom left\0bottom right\0" );
					},
					ImVec2( 220.f, -1.f ) );
			}
			ImGui::Checkbox( "clipper", &GET_VARIABLE( g_variables.m_movement_rec_clipper, bool ) );
			if ( GET_VARIABLE( g_variables.m_movement_rec_clipper, bool ) ) {
				ImGui::Checkbox( "show clipper box", &GET_VARIABLE( g_variables.m_movement_rec_clipper_box, bool ) );
				ImGui::SliderFloat( "clipper duration##recorder", &GET_VARIABLE( g_variables.m_movement_rec_clip_seconds, float ), 1.f, 120.f, "%.0f s" );
			}
			ImGui::Checkbox( "original playback viewangles", &GET_VARIABLE( g_variables.m_movement_rec_lockva, bool ) );
			ImGui::Combo( "playback yaw", &GET_VARIABLE( g_variables.m_movement_rec_yaw, int ), "original\0left\0right\0backwards\0spinning\0" );
			if ( GET_VARIABLE( g_variables.m_movement_rec_yaw, int ) == 4 )
				ImGui::SliderFloat( "spin speed##recorder", &GET_VARIABLE( g_variables.m_movement_rec_spin_speed, float ), -1800.f, 1800.f, "%.0f deg/s" );
			ImGui::SliderFloat( "smooth start##recorder", &GET_VARIABLE( g_variables.m_movement_rec_smooth_start, float ), 0.f, 3.f,
			                    GET_VARIABLE( g_variables.m_movement_rec_smooth_start, float ) > 0.01f ? "%.2f s" : "off" );
			ImGui::SliderFloat( "smooth end##recorder", &GET_VARIABLE( g_variables.m_movement_rec_smooth_end, float ), 0.f, 3.f,
			                    GET_VARIABLE( g_variables.m_movement_rec_smooth_end, float ) > 0.01f ? "%.2f s" : "off" );
			ImGui::Checkbox( "lock while aiming to position", &GET_VARIABLE( g_variables.m_movement_rec_lockgoingtostart, bool ) );
			ImGui::Checkbox( "stop playback on movement", &GET_VARIABLE( g_variables.m_movement_rec_stop_on_move, bool ) );
			ImGui::Checkbox( "force same weapons", &GET_VARIABLE( g_variables.m_movement_rec_force_weapon, bool ) );

			/* press actions: a style ( always on / toggle ) would fire them on every menu close */
			ImGui::Label( "start recording" );
			ImGui::Keybind( "recorder start rec key", &GET_VARIABLE( g_variables.m_movement_rec_keystartrecord, key_bind_t ), false );
			ImGui::Label( "stop recording" );
			ImGui::Keybind( "recorder stop rec key", &GET_VARIABLE( g_variables.m_movement_rec_keystoprecord, key_bind_t ), false );
			ImGui::Label( "save route" );
			ImGui::Keybind( "recorder save key", &GET_VARIABLE( g_variables.m_movement_rec_keysaveroute, key_bind_t ), false );
			ImGui::Label( "start playback" );
			ImGui::Keybind( "recorder start play key", &GET_VARIABLE( g_variables.m_movement_rec_keystartplay, key_bind_t ), false );
			ImGui::Label( "stop playback" );
			ImGui::Keybind( "recorder stop play key", &GET_VARIABLE( g_variables.m_movement_rec_keystopplay, key_bind_t ), false );
			ImGui::Label( "clear route" );
			ImGui::Keybind( "recorder clear key", &GET_VARIABLE( g_variables.m_movement_rec_keyclearrecord, key_bind_t ), false );
			if ( GET_VARIABLE( g_variables.m_movement_rec_clipper, bool ) ) {
				ImGui::Label( "clip route" );
				ImGui::Keybind( "recorder clip key", &GET_VARIABLE( g_variables.m_movement_rec_keyclip, key_bind_t ), false );
			}
		}
		menu_group_end( );

		if ( menu_group_begin( "actions" ) ) {
			using namespace n_movement_recorder;

			static constexpr std::pair< const char*, e_action > k_actions[ ] = {
				{ "start recording", action_start_rec }, { "stop recording", action_stop_rec }, { "save route", action_save },
				{ "start playback", action_start_play }, { "stop playback", action_stop_play }, { "clear route", action_clear },
				{ "clip route", action_clip },
			};

			ImGui::BeginDisabled( !GET_VARIABLE( g_variables.m_movement_rec, bool ) );
			for ( const auto& [ label, action ] : k_actions ) {
				if ( action == action_clip && !GET_VARIABLE( g_variables.m_movement_rec_clipper, bool ) )
					continue;
				if ( ImGui::Button( std::format( "{}##recorder action", label ).c_str( ), ImVec2( -1.f, 15.f ) ) )
					mr.request( action );
			}
			ImGui::EndDisabled( );
		}
		menu_group_end( );

		menu_columns_next( );

		if ( menu_group_begin( "routes" ) ) {
			std::scoped_lock lock( mr.m_clips_mutex );

			if ( mr.m_current_map.empty( ) )
				ImGui::TextDisabled( "join a map to see its routes" );
			else
				ImGui::TextDisabled( "%s   %d routes   server %d tick", mr.m_current_map.c_str( ), static_cast< int >( mr.m_clips.size( ) ),
				                     mr.server_tickrate( ) );

			if ( ImGui::BeginListBox( "##recorder routes", ImVec2( -1.f, 220.f ) ) ) {
				for ( std::size_t i = 0; i < mr.m_clips.size( ); ++i ) {
					const auto& clip      = mr.m_clips[ i ];
					const float seconds   = static_cast< float >( clip.frames.size( ) ) / static_cast< float >( std::max( clip.tickrate, 1 ) );
					const std::string row = std::format( "{}   {} ticks  {:.1f} s##route {}", clip.label( ), clip.frames.size( ), seconds, i );
					if ( ImGui::Selectable( row.c_str( ), mr.m_selected_clip == static_cast< int >( i ), ImGuiSelectableFlags_AllowDoubleClick ) ) {
						mr.m_selected_clip = static_cast< int >( i );
						if ( ImGui::IsMouseDoubleClicked( ImGuiMouseButton_Left ) )
							mr.open_clip_editor( i );
					}
				}
				ImGui::EndListBox( );
			}

			const bool has_selection = mr.m_selected_clip >= 0 && mr.m_selected_clip < static_cast< int >( mr.m_clips.size( ) );

			ImGui::BeginDisabled( !has_selection );
			if ( ImGui::Button( "edit route", ImVec2( -1.f, 15.f ) ) && has_selection )
				mr.open_clip_editor( static_cast< std::size_t >( mr.m_selected_clip ) );
			if ( ImGui::Button( "delete route", ImVec2( -1.f, 15.f ) ) && has_selection )
				mr.delete_clip( static_cast< std::size_t >( mr.m_selected_clip ) );
			ImGui::EndDisabled( );

			if ( ImGui::Button( "refresh routes", ImVec2( -1.f, 15.f ) ) )
				mr.refresh_clips( );
		}
		menu_group_end( );

		menu_columns_end( );
		break;
	}
	case 4: {
		points_page( );
		break;
	}
	}
}
