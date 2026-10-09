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

static const ImVec2 k_route_calc_panel_size( 500.f, 364.f );

static void route_calc_settings_body( )
{
	auto& route = g_movement.m_route_calc_data;

	static const std::vector< const char* > style_names( std::begin( n_route::k_style_names ),
	                                                     std::end( n_route::k_style_names ) );

	const auto style_combo = []( const char* label, unsigned int& mask, int count ) {
		std::vector< bool > flags( count, true );
		for ( int f = 0; f < count; ++f )
			flags[ f ] = ( mask & ( 1u << f ) ) != 0u;
		if ( !ImGui::MultiCombo( label, flags, style_names, count ) )
			return false;
		/* rewrite only drawn bits: a row without "walk off" must not clear it */
		unsigned int drawn = 0u, built = 0u;
		for ( int f = 0; f < count; ++f ) {
			drawn |= 1u << f;
			if ( flags[ f ] )
				built |= 1u << f;
		}
		mask = ( mask & ~drawn ) | built;
		return true;
	};

	/* two even columns; lower boxes take what's left so nothing runs past the panel */
	const float column_width = ImFloor( ( ImGui::GetContentRegionAvail( ).x - ImGui::GetStyle( ).ItemSpacing.x ) / 2.f );

	ImGui::BeginGroup( );

	if ( ImGui::BeginChild( ( "master" ), ImVec2( column_width, 66.f ), true, 0, true ) ) {
		ImGui::Checkbox( "enable pixel finder", &GET_VARIABLE( g_variables.m_pixel_finder, bool ) );
		ImGui::OptionPopup(
			"page pixel finder settings",
			[ & ]( ) {
				ImGui::Label( "scan key" );
				ImGui::Keybind( "page pixel finder key", &GET_VARIABLE( g_variables.m_pixel_finder_key, key_bind_t ) );
				ImGui::Checkbox( "pixelsurfs##page", &GET_VARIABLE( g_variables.m_pixel_finder_pixelsurfs, bool ) );
				ImGui::Checkbox( "texturebugs##page", &GET_VARIABLE( g_variables.m_pixel_finder_texturebugs, bool ) );
				ImGui::Checkbox( "headbounces##page", &GET_VARIABLE( g_variables.m_pixel_finder_headbounces, bool ) );
				ImGui::Checkbox( "pixeljumps##page", &GET_VARIABLE( g_variables.m_pixel_finder_pixeljumps, bool ) );
			},
			ImVec2( 200.f, -1 ) );

		ImGui::Checkbox( "enable pixel calculator", &GET_VARIABLE( g_variables.m_pixel_calc, bool ) );
		ImGui::OptionPopup(
			"page pixel calculator settings",
			[ & ]( ) {
				ImGui::Label( "aim key" );
				ImGui::Keybind( "page pixel aim key", &GET_VARIABLE( g_variables.m_pixel_calc_aim_key, key_bind_t ) );
				ImGui::Label( "solve key" );
				ImGui::Keybind( "page pixel solve key", &GET_VARIABLE( g_variables.m_pixel_calc_solve_key, key_bind_t ) );
				ImGui::Checkbox( "show point", &GET_VARIABLE( g_variables.m_pixel_calc_show_point, bool ) );
				ImGui::Combo( "type##page pixel calculator type", &GET_VARIABLE( g_variables.m_pixel_calc_type, int ),
				              "pixelsurf\0pixeljump\0" );
				ImGui::Checkbox( "advanced readout##page pixel calculator",
				                 &GET_VARIABLE( g_variables.m_pixel_calc_advanced_readout, bool ) );
				ImGui::Label( "allowed jumps" );
				move_mask_combo( "##page pixel calculator jump types", GET_VARIABLE( g_variables.m_pixel_calc_moves, int ) );
			},
			ImVec2( 200.f, -1 ) );

		ImGui::Checkbox( "enable route calculator", &GET_VARIABLE( g_variables.m_route_calc, bool ) );
		ImGui::OptionPopup(
			"route calculator keys",
			[ & ]( ) {
				ImGui::Label( "add point" );
				ImGui::Keybind( "route calc add key", &GET_VARIABLE( g_variables.m_route_calc_add_key, key_bind_t ) );
				ImGui::Label( "calculate combos" );
				ImGui::Keybind( "route calc solve key", &GET_VARIABLE( g_variables.m_route_calc_solve_key, key_bind_t ) );
				ImGui::Label( "delete point" );
				ImGui::Keybind( "route calc delete key", &GET_VARIABLE( g_variables.m_route_calc_delete_key, key_bind_t ) );
				ImGui::Label( "clear all points" );
				ImGui::Keybind( "route calc clear key", &GET_VARIABLE( g_variables.m_route_calc_clear_key, key_bind_t ) );
				ImGui::Checkbox( "show key list", &GET_VARIABLE( g_variables.m_route_calc_show_keys, bool ) );
				ImGui::Checkbox( "show combos on screen", &GET_VARIABLE( g_variables.m_route_calc_show_bar, bool ) );
				ImGui::Checkbox( "advanced readout##route calc",
				                 &GET_VARIABLE( g_variables.m_route_calc_advanced_readout, bool ) );
				ImGui::SliderInt( "popup duration", &GET_VARIABLE( g_variables.m_route_calc_popup_time, int ), 1, 30,
				                  "%d sec" );
			},
			ImVec2( 210.f, -1 ) );
	}
	ImGui::EndChild( );

	if ( ImGui::BeginChild( ( "preferences" ), ImVec2( column_width, 0.f ), true, 0, true ) ) {
		ImGui::Checkbox( "pin to menu", &GET_VARIABLE( g_variables.m_route_calc_pinned, bool ) );

		if ( ImGui::Checkbox( "use current pos for start jump",
		                      &GET_VARIABLE( g_variables.m_route_calc_start_here, bool ) ) )
			route.drop_solutions( );

		if ( ImGui::Checkbox( "allow selecting invalid pixels",
		                      &GET_VARIABLE( g_variables.m_route_calc_allow_invalid, bool ) ) )
			route.drop_solutions( );

		ImGui::Checkbox( "snap points to nearest edge", &GET_VARIABLE( g_variables.m_route_calc_snap_edge, bool ) );

		ImGui::SliderInt( "max displayed combos", &GET_VARIABLE( g_variables.m_route_calc_max_results, int ), 1, 64,
		                  "%d combos" );

		if ( ImGui::SliderInt( "delay hop ticks", &GET_VARIABLE( g_variables.m_route_calc_delay_ticks, int ), 0, 8,
		                       "%d ticks" ) )
			route.drop_solutions( );

		ImGui::Label( "jump types to calculate" );
		if ( move_mask_combo( "##global jump types", GET_VARIABLE( g_variables.m_route_calc_global_moves, int ) ) )
			route.drop_solutions( );
	}
	ImGui::EndChild( );

	ImGui::EndGroup( );

	ImGui::SameLine( );

	if ( ImGui::BeginChild( ( "added points" ), ImVec2( 0.f, 0.f ), true, 0, true ) ) {
		if ( ImGui::BeginListBox( ( "##route points" ), ImVec2( 0.f, 92.f ) ) ) {
			const bool start_here = GET_VARIABLE( g_variables.m_route_calc_start_here, bool );
			if ( ImGui::Selectable( start_here ? "starting jump (current position)" : "starting jump (point 1)",
			                        route.selected_row == 0, ImGuiSelectableFlags_DontClosePopups ) )
				route.selected_row = 0;

			for ( std::size_t i = 0; i < route.points.size( ); ++i ) {
				const std::string row = std::to_string( static_cast< int >( i ) + 1 ) + ". " +
				                        route_point_type_name( route_point_shown_type( route.points[ i ] ) ) + "   z " +
				                        std::to_string( static_cast< int >( route_point_mark( route.points[ i ] ).m_z ) ) +
				                        ( route.points[ i ].enabled ? "" : "   (off)" );
				if ( ImGui::Selectable( row.c_str( ), route.selected_row == static_cast< int >( i ) + 1,
				                        ImGuiSelectableFlags_DontClosePopups ) )
					route.selected_row = static_cast< int >( i ) + 1;
			}
			ImGui::EndListBox( );
		}

		if ( ImGui::Button( "clear all points" ) )
			route.clear( );

		if ( route.selected_row > static_cast< int >( route.points.size( ) ) )
			route.selected_row = 0;

		if ( route.selected_row == 0 ) {
			ImGui::Label( "types to calculate" );
			int& stored       = GET_VARIABLE( g_variables.m_route_calc_start_styles, int );
			unsigned int mask = static_cast< unsigned int >( stored );
			if ( style_combo( "##types to calculate", mask, n_route::style_count ) ) {
				stored = static_cast< int >( mask & n_route::k_all_styles );
				route.drop_solutions( );
			}
		} else {
			auto& point = route.points[ route.selected_row - 1 ];

			if ( ImGui::Checkbox( "point enabled", &point.enabled ) )
				route.drop_solutions( );

			const int row = route.selected_row - 1;
			if ( ImGui::Button( "move up" ) && row > 0 ) {
				std::swap( route.points[ row ], route.points[ row - 1 ] );
				route.selected_row -= 1;
				route.drop_solutions( );
			}
			ImGui::SameLine( );
			if ( ImGui::Button( "move down" ) && row + 1 < static_cast< int >( route.points.size( ) ) ) {
				std::swap( route.points[ row ], route.points[ row + 1 ] );
				route.selected_row += 1;
				route.drop_solutions( );
			}
			ImGui::SameLine( );
			if ( ImGui::Button( "delete point" ) ) {
				route.points.erase( route.points.begin( ) + row );
				if ( route.selected_row > static_cast< int >( route.points.size( ) ) )
					route.selected_row = static_cast< int >( route.points.size( ) );
				route.drop_solutions( );
			} else {
				static const char* const type_names[ route_pt_count ] = { "ground",   "pixelsurf",
				                                                         "edgebug",  "headbang",
				                                                         "texturebug", "pixeljump",
				                                                         "headbounce" };
				int shown                                             = route_point_shown_type( point );
				if ( ImGui::Combo( "point type", &shown, type_names, route_pt_count ) && shown != route_point_shown_type( point ) ) {
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
						point.type    = shown;
						point.head_tb = false;
						route.drop_solutions( );
					}
				}

				if ( point.type == route_pt_ground || point.type == route_pt_pixeljump ) {
					ImGui::Label( "types to calculate" );
					if ( style_combo( "##types to calculate", point.styles, n_route::style_count - 1 ) )
						route.drop_solutions( );
				} else {
					ImGui::Label( "types to calculate" );
					if ( ImGui::Checkbox( "arrive standing", &point.allow_stand ) )
						route.drop_solutions( );
					if ( ImGui::Checkbox( "arrive ducked", &point.allow_duck ) )
						route.drop_solutions( );
				}
			}
		}
	}
	ImGui::EndChild( );
}

void n_menu::impl_t::route_calc_pinned( const bool no_inputs )
{
	this->m_route_calc_side = 0;
	if ( !GET_VARIABLE( g_variables.m_route_calc_pinned, bool ) )
		return;

	const ImGuiWindow* menu = ImGui::FindWindowByName( "botox-ui" );
	if ( !menu )
		return;

	constexpr float k_gap = 8.f;
	const ImVec2 display  = ImGui::GetIO( ).DisplaySize;
	float x               = menu->Pos.x + menu->Size.x + k_gap;
	this->m_route_calc_side = 1;
	if ( x + k_route_calc_panel_size.x > display.x ) {
		x                       = menu->Pos.x - k_gap - k_route_calc_panel_size.x;
		this->m_route_calc_side = -1;
	}
	ImGui::SetNextWindowPos( ImVec2( x, menu->Pos.y ), ImGuiCond_Always );
	ImGui::SetNextWindowSize( k_route_calc_panel_size, ImGuiCond_Always );

	ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoCollapse |
	                         ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
	if ( no_inputs )
		flags |= ImGuiWindowFlags_NoInputs;

	if ( ImGui::Begin( "route calculator##pinned", nullptr, flags ) ) {
		const auto draw_list = ImGui::GetWindowDrawList( );
		const ImVec2 pos = ImGui::GetWindowPos( ), size = ImGui::GetWindowSize( );
		const auto bold  = g_render.m_fonts[ e_font_names::font_name_verdana_bd_11 ];
		const char* title = "route calculator settings";
		draw_list->AddRectFilled( pos, ImVec2( pos.x + size.x, pos.y + 20.f ), ImColor( 25 / 255.f, 25 / 255.f, 25 / 255.f ),
		                          ImGui::GetStyle( ).WindowRounding - 2.f, ImDrawFlags_RoundCornersTop );
		draw_list->AddRect( pos, ImVec2( pos.x + size.x, pos.y + size.y ), ImColor( 50, 50, 50, 100 ), ImGui::GetStyle( ).WindowRounding - 2.f );
		const ImVec2 title_size = bold->CalcTextSizeA( bold->FontSize, FLT_MAX, 0.f, title );
		draw_list->AddText( bold, bold->FontSize, ImVec2( pos.x + ( size.x - title_size.x ) / 2.f, pos.y + ( 20.f - title_size.y ) / 2.f ),
		                    ImGui::GetColorU32( ImGuiCol_Text ), title );
		RenderFadedGradientLine( draw_list, ImVec2( pos.x, pos.y + 20.f ), ImVec2( size.x, 1.f ), ImGui::GetColorU32( ImGuiCol_::ImGuiCol_Accent ) );
		ImGui::SetCursorPosY( ImGui::GetCursorPosY( ) + 20.f );

		route_calc_settings_body( );
	}
	ImGui::End( );
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
					ImVec2( 200.f, -1 ) );
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
					ImVec2( 200.f, -1 ) );
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
					ImVec2( 200.f, -1 ) );
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
					ImVec2( 200.f, -1 ) );
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
					ImVec2( 200.f, -1 ) );
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
					},
					ImVec2( 200.f, -1 ) );
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
					ImVec2( 200.f, -1 ) );
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
							ImGui::SliderFloat( "avoid distance", &GET_VARIABLE( g_variables.m_auto_strafe_avoid_dist, float ), 0.f, 200.f, "%.0f units" );
					},
					ImVec2( 220.f, -1 ) );
			}

			ImGui::Checkbox( "strafe optimizer", &GET_VARIABLE( g_variables.m_strafe_optimizer, bool ) );
			if ( GET_VARIABLE( g_variables.m_strafe_optimizer, bool ) ) {
				ImGui::OptionPopup(
					"strafe optimizer settings",
					[ & ]( ) {
						ImGui::SliderFloat( "gain", &GET_VARIABLE( g_variables.m_strafe_optimizer_gain, float ), 0.f, 100.f, "%.0f%%" );
						ImGui::SliderFloat( "min speed", &GET_VARIABLE( g_variables.m_strafe_optimizer_min_speed, float ), 0.f, 250.f, "%.0f" );
					},
					ImVec2( 220.f, -1 ) );
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
					"edgebug advanced settings",
					[ & ]( ) {
						ImGui::Label( "edgebug keybind" );
						ImGui::Keybind( "edge bug key", &GET_VARIABLE( g_variables.edge_bug_key, key_bind_t ) );
						ImGui::Combo( "style##ebstyle", &GET_VARIABLE( g_variables.m_edgebug_style, int ), "botox\0delusional\0dna\0" );
						const int style = GET_VARIABLE( g_variables.m_edgebug_style, int );
						if ( style == 1 ) {
							ImGui::SliderInt( "ticks to predict##ebdelticks", &GET_VARIABLE( g_variables.m_edgebug_del_ticks, int ), 0, 128 );
							ImGui::Checkbox( "mouse fix##ebdelmousefix", &GET_VARIABLE( g_variables.m_edgebug_del_mouse_fix, bool ) );
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
						}
						if ( style == 0 ) {
							ImGui::SliderFloat( "edge bug predict time", &GET_VARIABLE( g_variables.m_edgebug_predict_time, float ),
							                    0.02f, 2.f, "%.2f s" );

							ImGui::Checkbox( "autostrafe to edge", &GET_VARIABLE( g_variables.m_edgebug_autostrafe_to_edge, bool ) );
							if ( GET_VARIABLE( g_variables.m_edgebug_autostrafe_to_edge, bool ) ) {
								/* cone on the PATH, not the push: max bend off the starting course; lips
								   outside it are never picked. 180 = unclamped */
								ImGui::SliderFloat( "max path bend##ebstrafemax",
								                    &GET_VARIABLE( g_variables.m_edgebug_autostrafe_max_angle, float ), 0.f, 180.f, "%.0f deg" );
							}

							ImGui::Checkbox( "mouse steer##ebsteer", &GET_VARIABLE( g_variables.m_edgebug_mouse_steer, bool ) );
							if ( GET_VARIABLE( g_variables.m_edgebug_mouse_steer, bool ) )
								ImGui::SliderInt( "paths##ebpaths", &GET_VARIABLE( g_variables.m_edgebug_paths, int ), 1, 4 );

							ImGui::MultiCombo( "types allowed##ebtypes", GET_VARIABLE( g_variables.m_edgebug_types, std::vector< bool > ),
							                   { "ducking", "standing" }, GET_VARIABLE( g_variables.m_edgebug_types, std::vector< bool > ).size( ) );
						}

						ImGui::Checkbox( "lock mouse on find", &GET_VARIABLE( g_variables.m_edgebug_mouse_lock, bool ) );
						if ( GET_VARIABLE( g_variables.m_edgebug_mouse_lock, bool ) ) {
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
							if ( ImGui::InputText( "sound path##ebsndpath", eb_snd_buffer, sizeof( eb_snd_buffer ) ) )
								eb_snd_var = eb_snd_buffer;
							else if ( !ImGui::IsItemActive( ) && eb_snd_var != eb_snd_buffer ) {
								const std::size_t len = eb_snd_var.size( ) < sizeof( eb_snd_buffer ) - 1 ? eb_snd_var.size( ) : sizeof( eb_snd_buffer ) - 1;
								std::memcpy( eb_snd_buffer, eb_snd_var.c_str( ), len );
								eb_snd_buffer[ len ] = '\0';
							}
						}

						if ( GET_VARIABLE( g_variables.m_edge_bug_sound, int ) > 0 )
							ImGui::SliderFloat( "sound volume##ebsndvol", &GET_VARIABLE( g_variables.m_edge_bug_sound_volume, float ), 0.f, 2.f, "%.2f" );

						if ( style == 0 ) {
							ImGui::Combo( "detection mode##ebdetectmode", &GET_VARIABLE( g_variables.m_edgebug_detection_mode, int ),
							              "normal\0enhanced\0" );
							if ( GET_VARIABLE( g_variables.m_edgebug_detection_mode, int ) == 1 )
								ImGui::SliderInt( "search amount##ebsearch", &GET_VARIABLE( g_variables.m_edgebug_search_amount, int ), 1, 4 );
						}
					},
					ImVec2( 200.f, -1 ) );
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
							if ( ImGui::InputText( "sound path##essndpath", es_snd_buffer, sizeof( es_snd_buffer ) ) )
								es_snd_var = es_snd_buffer;
							else if ( !ImGui::IsItemActive( ) && es_snd_var != es_snd_buffer ) {
								const std::size_t len = es_snd_var.size( ) < sizeof( es_snd_buffer ) - 1 ? es_snd_var.size( ) : sizeof( es_snd_buffer ) - 1;
								std::memcpy( es_snd_buffer, es_snd_var.c_str( ), len );
								es_snd_buffer[ len ] = '\0';
							}
						}
						if ( GET_VARIABLE( g_variables.m_edge_skip_sound, int ) > 0 )
							ImGui::SliderFloat( "sound volume##essndvol", &GET_VARIABLE( g_variables.m_edge_skip_sound_volume, float ), 0.f, 2.f, "%.2f" );

						ImGui::Checkbox( "visualize##esviz", &GET_VARIABLE( g_variables.m_edge_skip_visualize, bool ) );
					},
					ImVec2( 200.f, -1 ) );
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
							if ( ImGui::InputText( "sound path##pssndpath", ps_snd_buffer, sizeof( ps_snd_buffer ) ) )
								ps_snd_var = ps_snd_buffer;
							else if ( !ImGui::IsItemActive( ) && ps_snd_var != ps_snd_buffer ) {
								const std::size_t len = ps_snd_var.size( ) < sizeof( ps_snd_buffer ) - 1 ? ps_snd_var.size( ) : sizeof( ps_snd_buffer ) - 1;
								std::memcpy( ps_snd_buffer, ps_snd_var.c_str( ), len );
								ps_snd_buffer[ len ] = '\0';
							}
						}

						if ( GET_VARIABLE( g_variables.m_pixel_surf_sound, int ) > 0 )
							ImGui::SliderFloat( "sound volume##pssndvol", &GET_VARIABLE( g_variables.m_pixel_surf_sound_volume, float ), 0.f, 2.f, "%.2f" );
					},
					ImVec2( 200.f, -1 ) );
			}

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
					ImVec2( 200.f, -1 ) );
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

						ImGui::Checkbox( "show points", &GET_VARIABLE( g_variables.m_pixel_surf_assist_show_points, bool ) );
						if ( GET_VARIABLE( g_variables.m_pixel_surf_assist_show_points, bool ) )
							ImGui::SliderFloat( "point draw distance",
							                    &GET_VARIABLE( g_variables.m_pixel_surf_assist_point_dist, float ), 200.f, 4000.f, "%.0f",
							                    ImGuiSliderFlags_AlwaysClamp );

						ImGui::Checkbox( "pixel surf assist render", &GET_VARIABLE( g_variables.m_pixel_surf_assist_render, bool ) );
						if ( GET_VARIABLE( g_variables.m_pixel_surf_assist_render, bool ) )
							ImGui::SliderFloat( "render height", &GET_VARIABLE( g_variables.m_pixel_surf_assist_render_height, float ), 0.f,
							                    100.f, "%.1f" );
					},
					ImVec2( 220.f, -1 ) );
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
					ImVec2( 220.f, -1 ) );
			}

			ImGui::Checkbox( "auto bounce", &GET_VARIABLE( g_variables.m_auto_bounce, bool ) );
			if ( GET_VARIABLE( g_variables.m_auto_bounce, bool ) ) {
				ImGui::OptionPopup(
					"auto bounce settings",
					[ & ]( ) {
						ImGui::Label( "auto bounce keybind" );
						ImGui::Keybind( "auto bounce key", &GET_VARIABLE( g_variables.m_auto_bounce_key, key_bind_t ) );

						ImGui::Combo( "mode##abmode", &GET_VARIABLE( g_variables.m_auto_bounce_mode, int ), "drop in\0keep speed\0stop\0" );
						ImGui::SliderFloat( "min speed gain", &GET_VARIABLE( g_variables.m_auto_bounce_min_gain, float ), 0.f, 500.f, "%.0f" );
						ImGui::SliderFloat( "max sim reach##abreach", &GET_VARIABLE( g_variables.m_auto_bounce_reach, float ), 16.f, 512.f, "%.0fu" );
						ImGui::Checkbox( "auto jump", &GET_VARIABLE( g_variables.m_auto_bounce_jump, bool ) );
						ImGui::Checkbox( "crouch jump##abcrouch", &GET_VARIABLE( g_variables.m_auto_bounce_crouch, bool ) );
						ImGui::Checkbox( "auto align##abalign", &GET_VARIABLE( g_variables.m_auto_bounce_align, bool ) );
						ImGui::Checkbox( "visualize##abviz", &GET_VARIABLE( g_variables.m_auto_bounce_visualize, bool ) );
						ImGui::ColorEdit4( "visualize color##abvizcolor", &GET_VARIABLE( g_variables.m_auto_bounce_visualize_color, c_color ),
						                   color_picker_alpha_flags );
					},
					ImVec2( 220.f, -1 ) );
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
						ImGui::Checkbox( "wallstrafe##tbws", &GET_VARIABLE( g_variables.m_texture_bug_wallstrafe, bool ) );

						ImGui::Combo( "detection sound##tbsound", &GET_VARIABLE( g_variables.m_texture_bug_sound, int ),
						              "none\0arena switch\0button\0money\0beep\0custom\0" );
						if ( GET_VARIABLE( g_variables.m_texture_bug_sound, int ) == 5 ) {
							auto&       tb_snd_var = GET_VARIABLE( g_variables.m_texture_bug_sound_custom, std::string );
							static char tb_snd_buffer[ 128 ] = { };
							if ( ImGui::InputText( "sound path##tbsndpath", tb_snd_buffer, sizeof( tb_snd_buffer ) ) )
								tb_snd_var = tb_snd_buffer;
							else if ( !ImGui::IsItemActive( ) && tb_snd_var != tb_snd_buffer ) {
								const std::size_t len = tb_snd_var.size( ) < sizeof( tb_snd_buffer ) - 1 ? tb_snd_var.size( ) : sizeof( tb_snd_buffer ) - 1;
								std::memcpy( tb_snd_buffer, tb_snd_var.c_str( ), len );
								tb_snd_buffer[ len ] = '\0';
							}
						}
						if ( GET_VARIABLE( g_variables.m_texture_bug_sound, int ) > 0 )
							ImGui::SliderFloat( "sound volume##tbsndvol", &GET_VARIABLE( g_variables.m_texture_bug_sound_volume, float ), 0.f, 2.f, "%.2f" );
					},
					ImVec2( 220.f, -1 ) );
			}

			ImGui::Checkbox( "air stuck", &GET_VARIABLE( g_variables.m_air_stuck, bool ) );
			if ( GET_VARIABLE( g_variables.m_air_stuck, bool ) ) {
				ImGui::OptionPopup(
					"air stuck settings",
					[ & ]( ) {
						ImGui::Label( "air stuck keybind" );
						ImGui::Keybind( "air stuck key", &GET_VARIABLE( g_variables.m_air_stuck_key, key_bind_t ) );

						ImGui::Checkbox( "fix air stucks", &GET_VARIABLE( g_variables.m_fix_air_stucks, bool ) );
						ImGui::Checkbox( "screen message", &GET_VARIABLE( g_variables.m_air_stuck_display_screen, bool ) );
						ImGui::SliderFloat( "extra reach", &GET_VARIABLE( g_variables.m_air_stuck_extra_reach, float ), 0.f, 32.f, "%.2f" );
						ImGui::SliderFloat( "fan budget", &GET_VARIABLE( g_variables.m_air_stuck_budget, float ), 5.f, 90.f, "%.0f%%" );
					},
					ImVec2( 220.f, -1 ) );
			}

			ImGui::Checkbox( "wall climb", &GET_VARIABLE( g_variables.m_wall_climb, bool ) );
			if ( GET_VARIABLE( g_variables.m_wall_climb, bool ) ) {
				ImGui::OptionPopup(
					"wall climb settings",
					[ & ]( ) {
						ImGui::Label( "wall climb keybind" );
						ImGui::Keybind( "wall climb key", &GET_VARIABLE( g_variables.m_wall_climb_key, key_bind_t ) );
						ImGui::Checkbox( "prevent slow", &GET_VARIABLE( g_variables.m_wall_climb_prevent_slow, bool ) );
					},
					ImVec2( 220.f, -1 ) );
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
					ImVec2( 220.f, -1 ) );
			}

			ImGui::Checkbox( "air freeze", &GET_VARIABLE( g_variables.m_air_freeze, bool ) );
			if ( GET_VARIABLE( g_variables.m_air_freeze, bool ) ) {
				ImGui::OptionPopup(
					"air freeze settings",
					[ & ]( ) {
						ImGui::Label( "air freeze keybind" );
						ImGui::Keybind( "air freeze key", &GET_VARIABLE( g_variables.m_air_freeze_key, key_bind_t ) );
					},
					ImVec2( 220.f, -1 ) );
			}
		}
		menu_group_end( );

		menu_columns_next( );

		if ( menu_group_begin( "other" ) ) {
			ImGui::Checkbox( "128 tick fix", &GET_VARIABLE( g_variables.m_tick_fix_128, bool ) );

			ImGui::Checkbox( "movement fix", &GET_VARIABLE( g_variables.m_movement_fix, bool ) );

			ImGui::Checkbox( "smooth demo view", &GET_VARIABLE( g_variables.m_silent_view, bool ) );

			ImGui::Checkbox( "pixel surf fix", &GET_VARIABLE( g_variables.m_pixel_surf_fix, bool ) );

			ImGui::Checkbox( "auto align", &GET_VARIABLE( g_variables.m_auto_align, bool ) );

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
					ImVec2( 220.f, -1 ) );
			}

			ImGui::Checkbox( "auto one hop", &GET_VARIABLE( g_variables.m_auto_one_hop, bool ) );
			ImGui::Keybind( "auto one hop key", &GET_VARIABLE( g_variables.m_auto_one_hop_key, key_bind_t ) );

			ImGui::Checkbox( "blockbot", &GET_VARIABLE( g_variables.m_blockbot, bool ) );
			ImGui::Keybind( "blockbot key", &GET_VARIABLE( g_variables.m_blockbot_key, key_bind_t ) );

			safe_checkbox( "no crouch cooldown",&GET_VARIABLE( g_variables.m_no_crouch_cooldown, bool ) );

			ImGui::Checkbox( "fast stop", &GET_VARIABLE( g_variables.m_fast_stop, bool ) );
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
						if ( GET_VARIABLE( g_variables.m_velocity_indicator_show_pre_speed, bool ) ) {
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

						ImGui::Checkbox( "shadow##velocity indicator", &GET_VARIABLE( g_variables.m_velocity_indicator_shadow, bool ) );
						if ( GET_VARIABLE( g_variables.m_velocity_indicator_shadow, bool ) ) {
							ImGui::SliderFloat( "shadow blur##velocity indicator", &GET_VARIABLE( g_variables.m_velocity_indicator_shadow_blur, float ), 0.f,
							                    20.f, "%.1f" );
							if ( GET_VARIABLE( g_variables.m_velocity_indicator_style, int ) != indicator_style_kamidere )
								ImGui::ColorEdit4( "shadow color##velocity indicator", &GET_VARIABLE( g_variables.m_velocity_indicator_shadow_color, c_color ),
								                   color_picker_alpha_flags );
							ImGui::SliderInt( "shadow x##velocity indicator", &GET_VARIABLE( g_variables.m_velocity_indicator_shadow_x, int ), -10, 10, "%d px" );
							ImGui::SliderInt( "shadow y##velocity indicator", &GET_VARIABLE( g_variables.m_velocity_indicator_shadow_y, int ), -10, 10, "%d px" );
						}

						ImGui::Checkbox( "outline##velocity indicator", &GET_VARIABLE( g_variables.m_velocity_indicator_outline, bool ) );
						if ( GET_VARIABLE( g_variables.m_velocity_indicator_outline, bool ) ) {
							ImGui::ColorEdit4( "outline color##velocity indicator", &GET_VARIABLE( g_variables.m_velocity_indicator_outline_color, c_color ),
							                   color_picker_alpha_flags );
							ImGui::SliderInt( "outline thickness##velocity indicator", &GET_VARIABLE( g_variables.m_velocity_indicator_outline_px, int ), 1, 5,
							                  "%d px" );
						}

						ImGui::SliderFloat( "scale##velocity indicator", &GET_VARIABLE( g_variables.m_velocity_indicator_scale, float ), k_indicator_scale_min, 3.f, "%.2fx" );

						ImGui::SliderInt( "position##velocity indicator", &GET_VARIABLE( g_variables.m_velocity_indicator_padding, int ), 30,
					                      g_ctx.m_height );
					},
					ImVec2( 200.f, -1 ) );
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
						if ( GET_VARIABLE( g_variables.m_stamina_indicator_show_pre_speed, bool ) ) {
							ImGui::Combo( "pre speed position##stamina indicator", &GET_VARIABLE( g_variables.m_stamina_indicator_pre_layout, int ),
							              k_pre_layout_names );
							ImGui::SliderFloat( "pre speed scale##stamina indicator", &GET_VARIABLE( g_variables.m_stamina_indicator_pre_scale, float ), 0.25f,
							                    2.f, "%.2fx" );
							if ( GET_VARIABLE( g_variables.m_stamina_indicator_pre_layout, int ) == pre_layout_right )
								ImGui::Combo( "pre speed align##stamina indicator", &GET_VARIABLE( g_variables.m_stamina_indicator_pre_align, int ),
								              k_pre_align_names );
						}

						ImGui::Checkbox( "fade alpha##stamina indicator", &GET_VARIABLE( g_variables.m_stamina_indicator_fade_alpha, bool ) );

						ImGui::Checkbox( "shadow##stamina indicator", &GET_VARIABLE( g_variables.m_stamina_indicator_shadow, bool ) );
						if ( GET_VARIABLE( g_variables.m_stamina_indicator_shadow, bool ) ) {
							ImGui::SliderFloat( "shadow blur##stamina indicator", &GET_VARIABLE( g_variables.m_stamina_indicator_shadow_blur, float ), 0.f,
							                    20.f, "%.1f" );
							if ( GET_VARIABLE( g_variables.m_stamina_indicator_style, int ) != indicator_style_kamidere )
								ImGui::ColorEdit4( "shadow color##stamina indicator", &GET_VARIABLE( g_variables.m_stamina_indicator_shadow_color, c_color ),
								                   color_picker_alpha_flags );
							ImGui::SliderInt( "shadow x##stamina indicator", &GET_VARIABLE( g_variables.m_stamina_indicator_shadow_x, int ), -10, 10, "%d px" );
							ImGui::SliderInt( "shadow y##stamina indicator", &GET_VARIABLE( g_variables.m_stamina_indicator_shadow_y, int ), -10, 10, "%d px" );
						}

						ImGui::Checkbox( "outline##stamina indicator", &GET_VARIABLE( g_variables.m_stamina_indicator_outline, bool ) );
						if ( GET_VARIABLE( g_variables.m_stamina_indicator_outline, bool ) ) {
							ImGui::ColorEdit4( "outline color##stamina indicator", &GET_VARIABLE( g_variables.m_stamina_indicator_outline_color, c_color ),
							                   color_picker_alpha_flags );
							ImGui::SliderInt( "outline thickness##stamina indicator", &GET_VARIABLE( g_variables.m_stamina_indicator_outline_px, int ), 1, 5,
							                  "%d px" );
						}

						ImGui::SliderFloat( "scale##stamina indicator", &GET_VARIABLE( g_variables.m_stamina_indicator_scale, float ), k_indicator_scale_min, 3.f, "%.2fx" );

						ImGui::SliderInt( "position##stamina indicator", &GET_VARIABLE( g_variables.m_stamina_indicator_padding, int ), 30,
					                      g_ctx.m_height );
					},
					ImVec2( 200.f, -1 ) );
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
					ImVec2( 200.f, -1 ) );
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

						ImGui::ColorEdit4( "keybind colors##keybind color", &GET_VARIABLE( g_variables.m_key_color, c_color ),
					                       color_picker_alpha_flags );

						ImGui::SameLine( );
						ImGui::ColorEdit4( "##keybind success color", &GET_VARIABLE( g_variables.m_key_color_success, c_color ),
					                       color_picker_alpha_flags, 1 );

						ImGui::ColorEdit4( "in prediction color##keybind predict color", &GET_VARIABLE( g_variables.m_key_color_predict, c_color ),
						                   color_picker_alpha_flags );

						ImGui::Checkbox( "horizontal layout", &GET_VARIABLE( g_variables.m_key_indicators_horizontal, bool ) );

						ImGui::SliderInt( "spacing##keybind indicators", &GET_VARIABLE( g_variables.m_key_indicators_spacing, int ), -10, 40, "%d px" );

						ImGui::Checkbox( "shadow##keybind indicators", &GET_VARIABLE( g_variables.m_key_indicators_shadow, bool ) );
						if ( GET_VARIABLE( g_variables.m_key_indicators_shadow, bool ) ) {
							ImGui::SliderFloat( "shadow blur##keybind indicators", &GET_VARIABLE( g_variables.m_key_indicators_shadow_blur, float ), 0.f, 20.f,
							                    "%.1f" );

							if ( GET_VARIABLE( g_variables.m_key_indicators_style, int ) != indicator_style_kamidere )
								ImGui::ColorEdit4( "shadow color##keybind indicators", &GET_VARIABLE( g_variables.m_key_indicators_shadow_color, c_color ),
								                   color_picker_alpha_flags );

							ImGui::Checkbox( "detect on shadow", &GET_VARIABLE( g_variables.m_key_indicators_shadow_detect, bool ) );
							ImGui::SliderInt( "shadow x##keybind indicators", &GET_VARIABLE( g_variables.m_key_indicators_shadow_x, int ), -10, 10, "%d px" );
							ImGui::SliderInt( "shadow y##keybind indicators", &GET_VARIABLE( g_variables.m_key_indicators_shadow_y, int ), -10, 10, "%d px" );
						}

						ImGui::Checkbox( "outline##keybind indicators", &GET_VARIABLE( g_variables.m_key_indicators_outline, bool ) );
						if ( GET_VARIABLE( g_variables.m_key_indicators_outline, bool ) ) {
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
					                         "zeus bug", "air freeze" },
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
								const bool edited = ImGui::InputTextWithHint( "##label", k_keybind_labels[ i ][ 1 ], buffers[ i ], sizeof( buffers[ i ] ) );
								const ImVec2 box  = ImGui::GetItemRectMin( );
								ImGui::GetWindowDrawList( )->AddText( ImVec2( box.x - name_width, box.y + ( 17.f - ImGui::GetFontSize( ) ) * 0.5f ),
								                                      ImGui::GetColorU32( ImGuiCol_Text ), k_keybind_labels[ i ][ 0 ] );
								if ( edited )
									labels[ i ] = buffers[ i ];
								else if ( !ImGui::IsItemActive( ) && labels[ i ] != buffers[ i ] ) {
									const std::size_t len = labels[ i ].size( ) < sizeof( buffers[ i ] ) - 1 ? labels[ i ].size( ) : sizeof( buffers[ i ] ) - 1;
									std::memcpy( buffers[ i ], labels[ i ].c_str( ), len );
									buffers[ i ][ len ] = '\0';
								}
								ImGui::PopID( );
							}
						}
					},
					ImVec2( 220.f, -1 ) );
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

						ImGui::SliderFloat( "fade##key press", &GET_VARIABLE( g_variables.m_key_press_fade, float ), 0.f, 0.3f, "%.2fs" );

						ImGui::SliderInt( "key gap##key press", &GET_VARIABLE( g_variables.m_key_press_gap_x, int ), 0, 60 );
						ImGui::SliderInt( "row gap##key press", &GET_VARIABLE( g_variables.m_key_press_gap_y, int ), -10, 60 );

						ImGui::SliderFloat( "scale##key press", &GET_VARIABLE( g_variables.m_key_press_scale, float ), k_indicator_scale_min, 3.f, "%.2fx" );

						ImGui::SliderInt( "position##key press", &GET_VARIABLE( g_variables.m_key_press_position, int ), 30,
						                  static_cast< int >( g_ctx.m_height ) );
						ImGui::SliderInt( "x offset##key press", &GET_VARIABLE( g_variables.m_key_press_offset_x, int ),
						                  -static_cast< int >( g_ctx.m_width / 2.f ), static_cast< int >( g_ctx.m_width / 2.f ) );
					},
					ImVec2( 220.f, -1 ) );
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
						if ( ImGui::InputText( "chat label##detlabel", label_buffer, sizeof( label_buffer ) ) )
							label_var = label_buffer;
						else if ( !ImGui::IsItemActive( ) && label_var != label_buffer ) {
							const std::size_t len = label_var.size( ) < sizeof( label_buffer ) - 1 ? label_var.size( ) : sizeof( label_buffer ) - 1;
							std::memcpy( label_buffer, label_var.c_str( ), len );
							label_buffer[ len ] = '\0';
						}
					},
					ImVec2( 220.f, -1 ) );
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
					ImVec2( 220.f, -1 ) );
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
					ImVec2( 220.f, -1 ) );
			}
		}
		menu_group_end( );

		menu_columns_end( );
		break;
	}
	case 2: {
		menu_columns_begin( );

		if ( menu_group_begin( "calculators" ) ) {
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
					},
					ImVec2( 200.f, -1 ) );
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
						ImGui::Combo( "type##pixel calculator type", &GET_VARIABLE( g_variables.m_pixel_calc_type, int ), "pixelsurf\0pixeljump\0" );
						ImGui::Checkbox( "advanced readout##pixel calculator", &GET_VARIABLE( g_variables.m_pixel_calc_advanced_readout, bool ) );
						ImGui::Label( "allowed jumps" );
						move_mask_combo( "##pixel calculator jump types", GET_VARIABLE( g_variables.m_pixel_calc_moves, int ) );
					},
					ImVec2( 200.f, -1 ) );
			}

			ImGui::Checkbox( "route calculator", &GET_VARIABLE( g_variables.m_route_calc, bool ) );
			if ( !GET_VARIABLE( g_variables.m_route_calc_pinned, bool ) )
				ImGui::OptionPopup( "route calculator settings", route_calc_settings_body, k_route_calc_panel_size );

			ImGui::Checkbox( "distance calculator", &GET_VARIABLE( g_variables.m_dist_calc, bool ) );
			if ( GET_VARIABLE( g_variables.m_dist_calc, bool ) ) {
				ImGui::OptionPopup(
					"distance calculator settings",
					[ & ]( ) {
						ImGui::Label( "point key" );
						ImGui::Keybind( "distance calculator key", &GET_VARIABLE( g_variables.m_dist_calc_key, key_bind_t ) );
					},
					ImVec2( 200.f, -1 ) );
			}
		}
		menu_group_end( );

		menu_columns_next( );

		if ( menu_group_begin( "practice" ) ) {
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
					ImVec2( 220.f, -1 ) );
			}

			ImGui::Checkbox( "fake pov in demos", &GET_VARIABLE( g_variables.m_fake_pov, bool ) );
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
				ImVec2( 220.f, -1 ) );

			ImGui::Checkbox( "hide y6o ps", &GET_VARIABLE( g_variables.m_hide_y6o_ps, bool ) );

			ImGui::Checkbox( "jumpstats", &GET_VARIABLE( g_variables.m_jump_stats, bool ) );
			if ( GET_VARIABLE( g_variables.m_jump_stats, bool ) ) {
				ImGui::OptionPopup(
					"jumpstats settings",
					[ & ]( ) {
						ImGui::Checkbox( "show fails##jumpstats", &GET_VARIABLE( g_variables.m_jump_stats_show_fails, bool ) );
					},
					ImVec2( 200.f, -1 ) );
			}
		}
		menu_group_end( );

		menu_columns_end( );
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
			ImGui::Checkbox( "show recording line", &GET_VARIABLE( g_variables.m_movement_rec_show_line, bool ) );
			ImGui::Checkbox( "show recorder timer", &GET_VARIABLE( g_variables.m_movement_rec_render, bool ) );
			if ( GET_VARIABLE( g_variables.m_movement_rec_render, bool ) ) {
				ImGui::OptionPopup(
					"recorder timer settings",
					[ & ]( ) {
						ImGui::Combo( "position##recorder timer", &GET_VARIABLE( g_variables.m_movement_rec_position, int ),
						              "top left\0bottom left\0bottom right\0" );
					},
					ImVec2( 200.f, -1 ) );
			}
			ImGui::Checkbox( "show clipper box", &GET_VARIABLE( g_variables.m_movement_rec_clipper_box, bool ) );
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
			ImGui::SliderFloat( "clipper duration##recorder", &GET_VARIABLE( g_variables.m_movement_rec_clip_seconds, float ), 1.f, 120.f, "%.0f s" );

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
			ImGui::Label( "clip route" );
			ImGui::Keybind( "recorder clip key", &GET_VARIABLE( g_variables.m_movement_rec_keyclip, key_bind_t ), false );
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
					const std::string row = std::format( "{}   {} ticks  {:.1f}s##route {}", clip.label( ), clip.frames.size( ), seconds, i );
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
	}
}
