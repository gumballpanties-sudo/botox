#include "menu_internal.h"
#include "../misc/misc.h"
#include "../cod4_hud/cod4_hud.h"
#include "../urt_hud/urt_hud.h"
#include "../web/websurf_math.h"
#include "../web/websurface.h"
#include "../visuals/screen/depth_source.h"
#include "../visuals/crosshair/custom_crosshair.h"
#include "../visuals/edicts/edicts.h"
#include "../visuals/old_mods.h"

static void bool_combo( const char* label, std::initializer_list< std::pair< const char*, bool* > > items )
{
	std::vector< const char* > names;
	std::vector< bool > flags;
	for ( const auto& [ name, var ] : items ) {
		names.push_back( name );
		flags.push_back( *var );
	}
	if ( !ImGui::MultiCombo( label, flags, names, static_cast< int >( names.size( ) ) ) )
		return;
	int i = 0;
	for ( const auto& item : items )
		*item.second = flags[ i++ ];
}

void n_menu::impl_t::tab_visuals( )
{
	const bool players = GET_VARIABLE( g_variables.m_players, bool );

	switch ( this->m_subtab ) {
	case 0: {
		menu_columns_begin( );

		if ( menu_group_begin( "players" ) ) {
			ImGui::Checkbox( "esp preview", &GET_VARIABLE( g_variables.m_players_preview, bool ) );
			ImGui::Checkbox( "players", &GET_VARIABLE( g_variables.m_players, bool ) );
			if ( GET_VARIABLE( g_variables.m_players, bool ) ) {
				ImGui::OptionPopup(
					"esp settings##player esp settings",
					[ & ]( ) {
						ImGui::Checkbox( "include teammates", &GET_VARIABLE( g_variables.m_players_teammates, bool ) );

						ImGui::SliderFloat( "max distance##player esp", &GET_VARIABLE( g_variables.m_players_max_distance, float ), 0.f,
					                        6000.f, "%.0f u", ImGuiSliderFlags_AlwaysClamp );

						if ( GET_VARIABLE( g_variables.m_players_max_distance, float ) > 0.f )
							ImGui::Checkbox( "fade with distance", &GET_VARIABLE( g_variables.m_players_distance_fade, bool ) );

						ImGui::Checkbox( "only on sound", &GET_VARIABLE( g_variables.m_players_sound_only, bool ) );

						if ( GET_VARIABLE( g_variables.m_players_sound_only, bool ) ) {
							ImGui::SliderFloat( "sound duration##player esp", &GET_VARIABLE( g_variables.m_players_sound_duration, float ), 0.2f,
						                        5.f, "%.1f s" );
							ImGui::Checkbox( "sound fade##player esp", &GET_VARIABLE( g_variables.m_players_sound_fade, bool ) );

							ImGui::SliderFloat( "min volume##sound esp players", &GET_VARIABLE( g_variables.m_sound_esp_min_volume, float ), 0.f,
						                        1.f, "%.2f", ImGuiSliderFlags_AlwaysClamp );
							ImGui::SliderFloat( "sound max distance##sound esp players",
						                        &GET_VARIABLE( g_variables.m_sound_esp_max_distance, float ), 0.f, 6000.f, "%.0f u",
						                        ImGuiSliderFlags_AlwaysClamp );
						}
					},
					ImVec2( 220.f, -1.f ) );

				ImGui::Checkbox( "bounding box##player", &GET_VARIABLE( g_variables.m_players_box, bool ) );
				if ( GET_VARIABLE( g_variables.m_players_box, bool ) ) {
					ImGui::OptionPopup(
						"bounding box settings##player bounding box settings",
						[ & ]( ) {
							ImGui::Checkbox( "visibility colors##player box",
						                     &GET_VARIABLE( g_variables.m_players_box_visibility_colors, bool ) );

							if ( GET_VARIABLE( g_variables.m_players_box_visibility_colors, bool ) ) {
								ImGui::ColorEdit4( "visible##player box visible color",
							                       &GET_VARIABLE( g_variables.m_players_box_visible_color, c_color ),
							                       color_picker_alpha_flags );
								ImGui::ColorEdit4( "invisible##player box invisible color",
							                       &GET_VARIABLE( g_variables.m_players_box_invisible_color, c_color ),
							                       color_picker_alpha_flags );
							} else {
								ImGui::ColorEdit4( "box color##player bounding box color",
							                       &GET_VARIABLE( g_variables.m_players_box_color, c_color ), color_picker_alpha_flags );
							}

							ImGui::Checkbox( "3d bounding box##player", &GET_VARIABLE( g_variables.m_players_box_3d, bool ) );

							if ( !GET_VARIABLE( g_variables.m_players_box_3d, bool ) )
								ImGui::Checkbox( "corner bounding box##player", &GET_VARIABLE( g_variables.m_players_box_corner, bool ) );

							ImGui::SliderFloat( "thickness##player box thickness", &GET_VARIABLE( g_variables.m_players_box_thickness, float ), 1.f, 5.f,
							                    "%.0f px", ImGuiSliderFlags_AlwaysClamp );

							ImGui::SliderFloat( "rounding##player box rounding", &GET_VARIABLE( g_variables.m_players_box_rounding, float ), 0.f, 20.f,
							                    "%.0f px", ImGuiSliderFlags_AlwaysClamp );

							ImGui::Checkbox( "bounding box outline##player", &GET_VARIABLE( g_variables.m_players_box_outline, bool ) );

							if ( GET_VARIABLE( g_variables.m_players_box_outline, bool ) ) {
								ImGui::ColorEdit4( "outline color##player bounding box outline color",
							                       &GET_VARIABLE( g_variables.m_players_box_outline_color, c_color ),
							                       color_picker_alpha_flags );

								ImGui::SliderFloat( "outline thickness##player box outline thickness",
							                        &GET_VARIABLE( g_variables.m_players_box_outline_thickness, float ), 1.f, 5.f, "%.0f px",
							                        ImGuiSliderFlags_AlwaysClamp );

								if ( !GET_VARIABLE( g_variables.m_players_box_corner, bool ) && !GET_VARIABLE( g_variables.m_players_box_3d, bool ) )
									ImGui::Combo( "outline side##player box outline side",
								                  &GET_VARIABLE( g_variables.m_players_box_outline_side, int ),
								                  "inside\0outside\0both\0" );
							}
						},
						ImVec2( 220.f, -1.f ) );
				}

				/* own row: fill without a frame is a valid look, box toggle must not gate it */
				ImGui::Checkbox( "box fill##player", &GET_VARIABLE( g_variables.m_players_box_fill, bool ) );
				if ( GET_VARIABLE( g_variables.m_players_box_fill, bool ) ) {
					ImGui::OptionPopup(
						"box fill settings##player box fill settings",
						[ & ]( ) {
							ImGui::ColorEdit4( GET_VARIABLE( g_variables.m_players_box_fill_gradient, bool ) ? "top color##player box fill color"
							                                                                                : "fill color##player box fill color",
							                   &GET_VARIABLE( g_variables.m_players_box_fill_color, c_color ), color_picker_alpha_flags );

							ImGui::Checkbox( "gradient##player box fill", &GET_VARIABLE( g_variables.m_players_box_fill_gradient, bool ) );

							if ( GET_VARIABLE( g_variables.m_players_box_fill_gradient, bool ) )
								ImGui::ColorEdit4( "bottom color##player box fill bottom color",
							                       &GET_VARIABLE( g_variables.m_players_box_fill_bottom_color, c_color ),
							                       color_picker_alpha_flags );
						},
						ImVec2( 220.f, -1.f ) );
				}

				ImGui::Checkbox( "name##player", &GET_VARIABLE( g_variables.m_players_name, bool ) );
				if ( GET_VARIABLE( g_variables.m_players_name, bool ) ) {
					ImGui::OptionPopup(
						"name settings##player name settings",
						[ & ]( ) {
							ImGui::Combo( "position##player name", &GET_VARIABLE( g_variables.m_players_name_position, int ),
						                  "above box\0below box\0inside top\0inside bottom\0" );

							ImGui::Checkbox( "visibility colors##player name",
						                     &GET_VARIABLE( g_variables.m_players_name_visibility_colors, bool ) );

							if ( GET_VARIABLE( g_variables.m_players_name_visibility_colors, bool ) ) {
								ImGui::ColorEdit4( "visible##player name visible color",
							                       &GET_VARIABLE( g_variables.m_players_name_visible_color, c_color ),
							                       color_picker_alpha_flags );
								ImGui::ColorEdit4( "invisible##player name invisible color",
							                       &GET_VARIABLE( g_variables.m_players_name_invisible_color, c_color ),
							                       color_picker_alpha_flags );
							} else {
								ImGui::ColorEdit4( "name color##player name color",
							                       &GET_VARIABLE( g_variables.m_players_name_color, c_color ), color_picker_alpha_flags );
							}
						},
						ImVec2( 220.f, -1.f ) );
				}

				ImGui::Checkbox( "health bar##player", &GET_VARIABLE( g_variables.m_players_health_bar, bool ) );
				if ( GET_VARIABLE( g_variables.m_players_health_bar, bool ) ) {
					ImGui::OptionPopup(
						"health bar settings##player health bar settings",
						[ & ]( ) {
							ImGui::Combo( "bar side##player health bar", &GET_VARIABLE( g_variables.m_players_health_bar_side, int ),
						                  "left\0right\0top\0bottom\0corner\0" );

							if ( GET_VARIABLE( g_variables.m_players_health_bar_side, int ) == e_bar_side::bar_side_corner )
								ImGui::Combo( "which corner##player health bar", &GET_VARIABLE( g_variables.m_players_health_bar_corner_pick, int ),
								              "bottom left\0bottom right\0top left\0top right\0" );

							if ( GET_VARIABLE( g_variables.m_players_health_bar_side, int ) == e_bar_side::bar_side_corner &&
							     !GET_VARIABLE( g_variables.m_players_box, bool ) ) {
								ImGui::SliderFloat( "corner rounding##player health bar",
								                    &GET_VARIABLE( g_variables.m_players_health_bar_corner_rounding, float ), 0.f, 20.f, "%.0f px",
								                    ImGuiSliderFlags_AlwaysClamp );
								ImGui::SliderFloat( "corner size##player health bar",
								                    &GET_VARIABLE( g_variables.m_players_health_bar_corner_size, float ), 0.1f, 1.f, "%.2f",
								                    ImGuiSliderFlags_AlwaysClamp );
							}

							ImGui::SliderFloat( "bar thickness##player health bar",
						                        &GET_VARIABLE( g_variables.m_players_health_bar_thickness, float ), 1.f, 10.f, "%.0f px",
						                        ImGuiSliderFlags_AlwaysClamp );

							ImGui::Checkbox( "health bar outline", &GET_VARIABLE( g_variables.m_players_health_bar_outline, bool ) );
							ImGui::Checkbox( "custom health bar color",
						                     &GET_VARIABLE( g_variables.m_players_health_bar_custom_color, bool ) );

							if ( GET_VARIABLE( g_variables.m_players_health_bar_custom_color, bool ) ) {
								ImGui::ColorEdit4( GET_VARIABLE( g_variables.m_players_health_bar_gradient, bool ) ? "top color##health bar color"
								                                                                                   : "bar color##health bar color",
								                   &GET_VARIABLE( g_variables.m_players_health_bar_color, c_color ), color_picker_alpha_flags );

								ImGui::Checkbox( "gradient##health bar", &GET_VARIABLE( g_variables.m_players_health_bar_gradient, bool ) );

								if ( GET_VARIABLE( g_variables.m_players_health_bar_gradient, bool ) )
									ImGui::ColorEdit4( "bottom color##health bar bottom color",
									                   &GET_VARIABLE( g_variables.m_players_health_bar_bottom_color, c_color ),
									                   color_picker_alpha_flags );
							}

							ImGui::ColorEdit4( "background color##health bar background color",
						                       &GET_VARIABLE( g_variables.m_players_health_bar_bg_color, c_color ),
						                       color_picker_alpha_flags );
						},
						ImVec2( 220.f, -1.f ) );
				}

				ImGui::Checkbox( "health number##player", &GET_VARIABLE( g_variables.m_players_health_text, bool ) );
				if ( GET_VARIABLE( g_variables.m_players_health_text, bool ) ) {
					ImGui::OptionPopup(
						"health number settings##player health number settings",
						[ & ]( ) {
							ImGui::Checkbox( "health number suffix", &GET_VARIABLE( g_variables.m_players_health_suffix, bool ) );
							ImGui::Combo( "health number style", &GET_VARIABLE( g_variables.m_players_health_text_style, int ),
						                  "standalone bottom\0follow health bar\0" );

							ImGui::Checkbox( "custom health number color",
						                     &GET_VARIABLE( g_variables.m_players_health_text_custom_color, bool ) );

							if ( GET_VARIABLE( g_variables.m_players_health_text_custom_color, bool ) )
								ImGui::ColorEdit4( "number color##player health number color",
							                       &GET_VARIABLE( g_variables.m_players_health_text_color, c_color ),
							                       color_picker_alpha_flags );
						},
						ImVec2( 220.f, -1.f ) );
				}

				ImGui::Checkbox( "weapon name##player", &GET_VARIABLE( g_variables.m_weapon_name, bool ) );
				if ( GET_VARIABLE( g_variables.m_weapon_name, bool ) ) {
					ImGui::SameLine( );
					ImGui::ColorEdit4( "##weapon name color", &GET_VARIABLE( g_variables.m_weapon_name_color, c_color ),
					                   color_picker_alpha_flags );
				}

				ImGui::Checkbox( "weapon icon##player", &GET_VARIABLE( g_variables.m_weapon_icon, bool ) );
				if ( GET_VARIABLE( g_variables.m_weapon_icon, bool ) ) {
					ImGui::SameLine( );
					ImGui::ColorEdit4( "##weapon icon color", &GET_VARIABLE( g_variables.m_weapon_icon_color, c_color ),
					                   color_picker_alpha_flags );
				}

				ImGui::Checkbox( "weapon ammo bar##player", &GET_VARIABLE( g_variables.m_player_ammo_bar, bool ) );
				if ( GET_VARIABLE( g_variables.m_player_ammo_bar, bool ) ) {
					ImGui::SameLine( );
					ImGui::ColorEdit4( "##player ammo bar color", &GET_VARIABLE( g_variables.m_player_ammo_bar_color, c_color ),
					                   color_picker_alpha_flags );
				}

				ImGui::Checkbox( "distance##player", &GET_VARIABLE( g_variables.m_players_distance, bool ) );
				if ( GET_VARIABLE( g_variables.m_players_distance, bool ) ) {
					ImGui::OptionPopup(
						"distance settings##player distance settings",
						[ & ]( ) {
							ImGui::Combo( "unit##player distance", &GET_VARIABLE( g_variables.m_players_distance_unit, int ),
						                  "meters\0units\0feet\0" );

							ImGui::ColorEdit4( "distance color##player distance color",
						                       &GET_VARIABLE( g_variables.m_players_distance_color, c_color ), color_picker_alpha_flags );
						},
						ImVec2( 220.f, -1.f ) );
				}

				ImGui::Checkbox( "flags##player", &GET_VARIABLE( g_variables.m_players_flags, bool ) );
				if ( GET_VARIABLE( g_variables.m_players_flags, bool ) ) {
					ImGui::OptionPopup(
						"flag settings##player flag settings",
						[ & ]( ) {
							ImGui::MultiCombo( "flags##player flag list", GET_VARIABLE( g_variables.m_player_flags, std::vector< bool > ),
						                       { "money", "armor", "helmet", "defuse kit", "defusing", "walking", "scoped", "flashed",
						                         "carrying bomb", "grabbing hostage", "immune", "wallbangable" },
						                       GET_VARIABLE( g_variables.m_player_flags, std::vector< bool > ).size( ) );

							ImGui::ColorEdit4( "flag color##player flag color", &GET_VARIABLE( g_variables.m_players_flags_color, c_color ),
						                       color_picker_alpha_flags );

							ImGui::ColorEdit4( "alert color##player flag alert color",
						                       &GET_VARIABLE( g_variables.m_players_flags_alert_color, c_color ), color_picker_alpha_flags );
						},
						ImVec2( 220.f, -1.f ) );
				}
			}

			ImGui::BeginDisabled( !players );

			ImGui::Checkbox( "player skeleton", &GET_VARIABLE( g_variables.m_players_skeleton, bool ) );
			if ( GET_VARIABLE( g_variables.m_players_skeleton, bool ) ) {
				ImGui::OptionPopup(
					"skeleton settings##player skeleton settings",
					[ & ]( ) {
						ImGui::Checkbox( "visibility colors##player skeleton",
					                     &GET_VARIABLE( g_variables.m_players_skeleton_visibility_colors, bool ) );

						if ( GET_VARIABLE( g_variables.m_players_skeleton_visibility_colors, bool ) ) {
							ImGui::ColorEdit4( "visible##player skeleton visible color",
						                       &GET_VARIABLE( g_variables.m_players_skeleton_visible_color, c_color ),
						                       color_picker_alpha_flags );
							ImGui::ColorEdit4( "invisible##player skeleton invisible color",
						                       &GET_VARIABLE( g_variables.m_players_skeleton_invisible_color, c_color ),
						                       color_picker_alpha_flags );
						} else {
							ImGui::ColorEdit4( "skeleton color##player skeleton color", &GET_VARIABLE( g_variables.m_players_skeleton_color, c_color ),
						                       color_picker_alpha_flags );
						}

						ImGui::Combo( "skeleton type##player skeleton", &GET_VARIABLE( g_variables.m_players_skeleton_type, int ),
					                  "normal\0lag compensated\0smooth\0" );
						ImGui::SliderFloat( "thickness##player skeleton",
					                        &GET_VARIABLE( g_variables.m_players_skeleton_thickness, float ), 0.5f, 5.f, "%.1f px",
					                        ImGuiSliderFlags_AlwaysClamp );
					},
					ImVec2( 220.f, -1.f ) );
			}

			ImGui::Checkbox( "show hitboxes", &GET_VARIABLE( g_variables.m_players_hitboxes, bool ) );
			if ( GET_VARIABLE( g_variables.m_players_hitboxes, bool ) ) {
				ImGui::OptionPopup(
					"hitboxes settings##player hitboxes settings",
					[ & ]( ) {
						ImGui::Combo( "show##player hitboxes", &GET_VARIABLE( g_variables.m_players_hitboxes_mode, int ), "always\0on hit\0" );
						ImGui::ColorEdit4( "color##player hitboxes color", &GET_VARIABLE( g_variables.m_players_hitboxes_color, c_color ),
					                       color_picker_alpha_flags );
						ImGui::SliderFloat( "thickness##player hitboxes", &GET_VARIABLE( g_variables.m_players_hitboxes_thickness, float ), 0.5f,
					                        5.f, "%.1f px", ImGuiSliderFlags_AlwaysClamp );
						if ( GET_VARIABLE( g_variables.m_players_hitboxes_mode, int ) == 1 )
							ImGui::SliderFloat( "duration##player hitboxes", &GET_VARIABLE( g_variables.m_players_hitboxes_duration, float ), 0.1f,
						                        10.f, "%.1f s", ImGuiSliderFlags_AlwaysClamp );
					},
					ImVec2( 220.f, -1.f ) );
			}

			ImGui::Checkbox( "player glow", &GET_VARIABLE( g_variables.m_glow_enable, bool ) );
			if ( GET_VARIABLE( g_variables.m_glow_enable, bool ) ) {
				ImGui::OptionPopup(
					"player glow settings",
					[ & ]( ) {
						const bool legacy   = GET_VARIABLE( g_variables.m_glow_legacy, bool );
						const bool gradient = !legacy && GET_VARIABLE( g_variables.m_glow_gradient, bool );

						ImGui::ColorEdit4( gradient ? "visible inner##player vis glow color" : "player visible##player vis glow color",
					                       &GET_VARIABLE( g_variables.m_glow_vis_color, c_color ), color_picker_alpha_flags );

						if ( gradient )
							ImGui::ColorEdit4( "visible outer##player vis glow outer color",
						                       &GET_VARIABLE( g_variables.m_glow_vis_outer_color, c_color ), color_picker_alpha_flags );

						ImGui::ColorEdit4( gradient ? "invisible inner##player invis glow color" : "player invisible##player invis glow color",
					                       &GET_VARIABLE( g_variables.m_glow_invis_color, c_color ), color_picker_alpha_flags );

						if ( gradient )
							ImGui::ColorEdit4( "invisible outer##player invis glow outer color",
						                       &GET_VARIABLE( g_variables.m_glow_invis_outer_color, c_color ), color_picker_alpha_flags );

						ImGui::Checkbox( "accent wave##player glow", &GET_VARIABLE( g_variables.m_glow_wave, bool ) );
						if ( GET_VARIABLE( g_variables.m_glow_wave, bool ) ) {
							ImGui::ColorEdit4( "wave color##player glow wave", &GET_VARIABLE( g_variables.m_glow_wave_color, c_color ),
							                   color_picker_no_alpha_flags );
							ImGui::SliderFloat( "wave speed##player glow wave", &GET_VARIABLE( g_variables.m_glow_wave_speed, float ), 0.05f, 4.f,
							                    "%.2fx", ImGuiSliderFlags_AlwaysClamp );
							if ( !legacy )
								ImGui::SliderFloat( "wave size##player glow wave", &GET_VARIABLE( g_variables.m_glow_wave_size, float ), 0.05f, 2.f,
								                    "%.2f", ImGuiSliderFlags_AlwaysClamp );
							ImGui::SliderFloat( "wave strength##player glow wave", &GET_VARIABLE( g_variables.m_glow_wave_strength, float ), 0.f,
							                    1.f, "%.2f", ImGuiSliderFlags_AlwaysClamp );
							if ( !legacy )
								ImGui::Checkbox( "follow player##player glow wave", &GET_VARIABLE( g_variables.m_glow_wave_player, bool ) );
						}

						ImGui::Checkbox( "edge pulse##player glow", &GET_VARIABLE( g_variables.m_glow_pulse, bool ) );
						if ( GET_VARIABLE( g_variables.m_glow_pulse, bool ) ) {
							ImGui::SliderFloat( "pulse speed##player glow pulse", &GET_VARIABLE( g_variables.m_glow_pulse_speed, float ), 0.1f, 4.f,
							                    "%.1fx", ImGuiSliderFlags_AlwaysClamp );
							ImGui::SliderFloat( "pulse min##player glow pulse", &GET_VARIABLE( g_variables.m_glow_pulse_min, float ), 0.f, 95.f,
							                    "%.0f%%", ImGuiSliderFlags_AlwaysClamp );
						}

						ImGui::Checkbox( "legacy##player glow", &GET_VARIABLE( g_variables.m_glow_legacy, bool ) );
						if ( legacy ) {
							ImGui::Combo( "style##player glow legacy", &GET_VARIABLE( g_variables.m_glow_legacy_style, int ),
							              "default\0rim\0outline\0pulse\0" );
							return;
						}

						ImGui::Checkbox( "normal blending##player glow", &GET_VARIABLE( g_variables.m_glow_normal_blend, bool ) );

						if ( GET_VARIABLE( g_variables.m_glow_normal_blend, bool ) )
							ImGui::SliderFloat( "thickness##player glow", &GET_VARIABLE( g_variables.m_glow_thickness, float ), 2.f, 30.f,
							                    "%.0f px", ImGuiSliderFlags_AlwaysClamp );

						ImGui::Checkbox( "gradient##player glow", &GET_VARIABLE( g_variables.m_glow_gradient, bool ) );

						if ( gradient )
							ImGui::SliderFloat( "inner size##player glow gradient", &GET_VARIABLE( g_variables.m_glow_gradient_inner, float ), 0.f,
						                        100.f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp );
					},
					ImVec2( 220.f, -1.f ) );
			}

			ImGui::Checkbox( "player stencil", &GET_VARIABLE( g_variables.m_players_stencil, bool ) );
			if ( GET_VARIABLE( g_variables.m_players_stencil, bool ) ) {
				ImGui::OptionPopup(
					"player stencil settings",
					[ & ]( ) {
						ImGui::ColorEdit4( "player visible##player stencil vis color", &GET_VARIABLE( g_variables.m_players_stencil_vis_color, c_color ),
					                       color_picker_alpha_flags );
						ImGui::ColorEdit4( "player invisible##player stencil invis color",
					                       &GET_VARIABLE( g_variables.m_players_stencil_invis_color, c_color ), color_picker_alpha_flags );
						ImGui::SliderFloat( "thickness##player stencil", &GET_VARIABLE( g_variables.m_players_stencil_thickness, float ), 0.5f, 8.f,
					                        "%.1f px", ImGuiSliderFlags_AlwaysClamp );

						ImGui::Checkbox( "accent wave##player stencil", &GET_VARIABLE( g_variables.m_players_stencil_wave, bool ) );
						if ( GET_VARIABLE( g_variables.m_players_stencil_wave, bool ) ) {
							ImGui::ColorEdit4( "wave color##player stencil wave", &GET_VARIABLE( g_variables.m_players_stencil_wave_color, c_color ),
							                   color_picker_no_alpha_flags );
							ImGui::SliderFloat( "wave speed##player stencil wave", &GET_VARIABLE( g_variables.m_players_stencil_wave_speed, float ), 0.05f,
							                    4.f, "%.2fx", ImGuiSliderFlags_AlwaysClamp );
							ImGui::SliderFloat( "wave size##player stencil wave", &GET_VARIABLE( g_variables.m_players_stencil_wave_size, float ), 0.05f,
							                    2.f, "%.2f", ImGuiSliderFlags_AlwaysClamp );
							ImGui::SliderFloat( "wave strength##player stencil wave", &GET_VARIABLE( g_variables.m_players_stencil_wave_strength, float ),
							                    0.f, 1.f, "%.2f", ImGuiSliderFlags_AlwaysClamp );
							ImGui::Checkbox( "follow player##player stencil wave", &GET_VARIABLE( g_variables.m_players_stencil_wave_player, bool ) );
						}
					},
					ImVec2( 220.f, -1.f ) );
			}

			ImGui::Checkbox( "internal player glow", &GET_VARIABLE( g_variables.m_inner_glow, bool ) );
			if ( GET_VARIABLE( g_variables.m_inner_glow, bool ) ) {
				ImGui::OptionPopup(
					"internal player glow settings",
					[ & ]( ) {
						ImGui::ColorEdit4( "player visible##inner glow vis color", &GET_VARIABLE( g_variables.m_inner_glow_vis_color, c_color ),
					                       color_picker_alpha_flags );
						ImGui::ColorEdit4( "player invisible##inner glow invis color", &GET_VARIABLE( g_variables.m_inner_glow_invis_color, c_color ),
					                       color_picker_alpha_flags );
						ImGui::Checkbox( "follow bones##inner glow", &GET_VARIABLE( g_variables.m_inner_glow_bones, bool ) );
						if ( GET_VARIABLE( g_variables.m_inner_glow_bones, bool ) )
							ImGui::SliderFloat( "width##inner glow", &GET_VARIABLE( g_variables.m_inner_glow_bone_width, float ), 10.f, 150.f, "%.0f%%",
						                        ImGuiSliderFlags_AlwaysClamp );
						else
							ImGui::SliderFloat( "size##inner glow", &GET_VARIABLE( g_variables.m_inner_glow_size, float ), 2.f, 30.f, "%.0f px",
						                        ImGuiSliderFlags_AlwaysClamp );
						ImGui::SliderFloat( "intensity##inner glow", &GET_VARIABLE( g_variables.m_inner_glow_intensity, float ), 0.25f, 4.f, "%.2fx",
					                        ImGuiSliderFlags_AlwaysClamp );
					},
					ImVec2( 220.f, -1.f ) );
			}

			ImGui::EndDisabled( );

			ImGui::BeginDisabled( !players );

			ImGui::Checkbox( "lag compensated trail", &GET_VARIABLE( g_variables.m_players_backtrack_trail, bool ) );
			ImGui::Checkbox( "player avatar", &GET_VARIABLE( g_variables.m_players_avatar, bool ) );
			if ( GET_VARIABLE( g_variables.m_players_avatar, bool ) ) {
				ImGui::OptionPopup(
					"avatar settings##player avatar settings",
					[ & ]( ) {
						ImGui::Combo( "position##player avatar", &GET_VARIABLE( g_variables.m_players_avatar_position, int ),
					                  "next to name\0above box\0below box\0cover head\0" );

						if ( GET_VARIABLE( g_variables.m_players_avatar_position, int ) == e_avatar_position::avatar_position_head )
							ImGui::SliderFloat( "head scale##player avatar",
						                        &GET_VARIABLE( g_variables.m_players_avatar_head_scale, float ), 0.25f, 3.f, "%.2fx",
						                        ImGuiSliderFlags_AlwaysClamp );
						else
							ImGui::SliderFloat( "avatar size##player avatar", &GET_VARIABLE( g_variables.m_players_avatar_size, float ),
						                        6.f, 40.f, "%.0f px", ImGuiSliderFlags_AlwaysClamp );
					},
					ImVec2( 220.f, -1.f ) );
			}

			ImGui::Checkbox( "damage numbers", &GET_VARIABLE( g_variables.m_damage_numbers, bool ) );
			if ( GET_VARIABLE( g_variables.m_damage_numbers, bool ) ) {
				ImGui::OptionPopup(
					"damage numbers settings",
					[ & ]( ) {
						ImGui::ColorEdit4( "damage numbers color##damage numbers color", &GET_VARIABLE( g_variables.m_damage_numbers_color, c_color ),
					                       color_picker_alpha_flags );
						ImGui::SliderFloat( "duration", &GET_VARIABLE( g_variables.m_damage_numbers_duration, float ), 0.2f, 5.f,
					                        "%.1f s" );
						ImGui::SliderFloat( "rise speed", &GET_VARIABLE( g_variables.m_damage_numbers_speed, float ), 0.f, 100.f,
					                        "%.0f u/s" );
						ImGui::Checkbox( "stack fast hits", &GET_VARIABLE( g_variables.m_damage_numbers_stack, bool ) );
					},
					ImVec2( 220.f, -1.f ) );
			}

			ImGui::Checkbox( ( "out of fov arrows" ), &GET_VARIABLE( g_variables.m_out_of_fov_arrows, bool ) );
			if ( GET_VARIABLE( g_variables.m_out_of_fov_arrows, bool ) ) {
				ImGui::OptionPopup(
					"fov arrows settings",
					[ & ]( ) {
						ImGui::Combo( "arrows show##out of fov arrows type", &GET_VARIABLE( g_variables.m_out_of_fov_arrows_type, int ),
					                  "players\0enemy sounds\0players + sounds\0" );

						const int arrows_type = GET_VARIABLE( g_variables.m_out_of_fov_arrows_type, int );

						if ( arrows_type != 1 ) {
							ImGui::ColorEdit4( "visible color##out of fov arrows color",
						                       &GET_VARIABLE( g_variables.m_out_of_fov_arrows_color, c_color ),
						                       color_picker_alpha_flags );
							ImGui::ColorEdit4( "behind wall color##out of fov arrows wall color",
						                       &GET_VARIABLE( g_variables.m_out_of_fov_arrows_wall_color, c_color ),
						                       color_picker_alpha_flags );
						}

						if ( arrows_type != 0 ) {
							ImGui::ColorEdit4( "sound color##out of fov arrows sound color",
						                       &GET_VARIABLE( g_variables.m_out_of_fov_arrows_sound_color, c_color ),
						                       color_picker_alpha_flags );
							ImGui::SliderFloat( "sound arrow duration",
						                        &GET_VARIABLE( g_variables.m_out_of_fov_arrows_sound_duration, float ), 0.2f, 5.f,
						                        "%.1f s" );
							ImGui::Checkbox( "sound arrow fade", &GET_VARIABLE( g_variables.m_out_of_fov_arrows_sound_fade, bool ) );

							ImGui::SliderFloat( "min volume##sound esp arrows",
						                        &GET_VARIABLE( g_variables.m_sound_esp_min_volume, float ), 0.f, 1.f, "%.2f",
						                        ImGuiSliderFlags_AlwaysClamp );
							ImGui::SliderFloat( "max distance##sound esp arrows",
						                        &GET_VARIABLE( g_variables.m_sound_esp_max_distance, float ), 0.f, 6000.f, "%.0f u",
						                        ImGuiSliderFlags_AlwaysClamp );
						}

						ImGui::Combo( "arrows style##out of fov arrows style", &GET_VARIABLE( g_variables.m_out_of_fov_arrows_style, int ),
					                  "default\0interium\0" );
						ImGui::SliderFloat( "arrows width", &GET_VARIABLE( g_variables.m_out_of_fov_arrows_width, float ), 1.f, 60.f,
					                        "%.1f px" );
						ImGui::SliderFloat( "arrows height", &GET_VARIABLE( g_variables.m_out_of_fov_arrows_height, float ), 1.f, 60.f,
					                        "%.1f px" );
						ImGui::SliderInt( "arrows distance", &GET_VARIABLE( g_variables.m_out_of_fov_arrows_distance, int ), 10, 500,
					                      "%d px" );
						if ( arrows_type != 1 )
							ImGui::Checkbox( "player avatars", &GET_VARIABLE( g_variables.m_out_of_fov_arrows_avatar, bool ) );

						ImGui::Checkbox( "follow aspect ratio", &GET_VARIABLE( g_variables.m_out_of_fov_arrows_aspect, bool ) );
					},
					ImVec2( 220.f, -1.f ) );
			}

			ImGui::EndDisabled( );
		}
		menu_group_end( );

		if ( menu_group_begin( "edicts" ) ) {
			ImGui::Checkbox( "dropped weapons", &GET_VARIABLE( g_variables.m_dropped_weapons, bool ) );
			if ( GET_VARIABLE( g_variables.m_dropped_weapons, bool ) ) {
				ImGui::OptionPopup(
					"dropped weapons settings",
					[ & ]( ) {
						ImGui::Checkbox( "bounding box##dropped weapons", &GET_VARIABLE( g_variables.m_dropped_weapons_box, bool ) );
						if ( GET_VARIABLE( g_variables.m_dropped_weapons_box, bool ) ) {
							ImGui::ColorEdit4( "box color##dropped weapons bounding box color",
						                       &GET_VARIABLE( g_variables.m_dropped_weapons_box_color, c_color ), color_picker_alpha_flags );

							ImGui::Checkbox( "corner bounding box##dropped weapons",
						                     &GET_VARIABLE( g_variables.m_dropped_weapons_box_corner, bool ) );
							ImGui::Checkbox( "bounding box outline##dropped weapons",
						                     &GET_VARIABLE( g_variables.m_dropped_weapons_box_outline, bool ) );

							if ( GET_VARIABLE( g_variables.m_dropped_weapons_box_outline, bool ) )
								ImGui::ColorEdit4( "outline color##dropped weapons bounding box outline color",
							                       &GET_VARIABLE( g_variables.m_dropped_weapons_box_outline_color, c_color ),
							                       color_picker_alpha_flags );
						}

						ImGui::Checkbox( "name##dropped weapons", &GET_VARIABLE( g_variables.m_dropped_weapons_name, bool ) );
						if ( GET_VARIABLE( g_variables.m_dropped_weapons_name, bool ) ) {
							ImGui::SameLine( );
							ImGui::ColorEdit4( "##dropped weapons name color",
						                       &GET_VARIABLE( g_variables.m_dropped_weapons_name_color, c_color ), color_picker_alpha_flags );
						}

						ImGui::Checkbox( "icons##dropped weapons icon", &GET_VARIABLE( g_variables.m_dropped_weapons_icon, bool ) );
						if ( GET_VARIABLE( g_variables.m_dropped_weapons_icon, bool ) ) {
							ImGui::SameLine( );
							ImGui::ColorEdit4( "##dropped weapons icon color",
						                       &GET_VARIABLE( g_variables.m_dropped_weapons_icon_color, c_color ), color_picker_alpha_flags );
						}
					},
					ImVec2( 220.f, -1.f ) );
			}

			ImGui::Checkbox( "planted bomb", &GET_VARIABLE( g_variables.m_bomb_esp, bool ) );
			if ( GET_VARIABLE( g_variables.m_bomb_esp, bool ) ) {
				ImGui::OptionPopup(
					"planted bomb settings",
					[ & ]( ) {
						ImGui::Checkbox( "icon##bomb esp", &GET_VARIABLE( g_variables.m_bomb_esp_icon, bool ) );
						ImGui::Checkbox( "countdown##bomb esp", &GET_VARIABLE( g_variables.m_bomb_esp_timer, bool ) );

						ImGui::ColorEdit4( "ticking##bomb esp color", &GET_VARIABLE( g_variables.m_bomb_esp_color, c_color ),
					                       color_picker_alpha_flags );

						ImGui::ColorEdit4( "defuse in time##bomb esp defuse color",
					                       &GET_VARIABLE( g_variables.m_bomb_esp_defuse_color, c_color ), color_picker_alpha_flags );

						ImGui::ColorEdit4( "defuse too late##bomb esp fail color",
					                       &GET_VARIABLE( g_variables.m_bomb_esp_fail_color, c_color ), color_picker_alpha_flags );
					},
					ImVec2( 220.f, -1.f ) );
			}

			ImGui::Checkbox( "bomb timer bar", &GET_VARIABLE( g_variables.m_bomb_timer_bar, bool ) );
			if ( GET_VARIABLE( g_variables.m_bomb_timer_bar, bool ) ) {
				ImGui::OptionPopup(
					"bomb timer bar settings",
					[ & ]( ) {
						ImGui::SliderInt( "height##bomb timer bar", &GET_VARIABLE( g_variables.m_bomb_timer_bar_position, int ), 0, 1000,
					                      "%d px" );
					},
					ImVec2( 220.f, -1.f ) );
			}

			ImGui::Checkbox( "thrown projectiles", &GET_VARIABLE( g_variables.m_thrown_objects, bool ) );
			if ( GET_VARIABLE( g_variables.m_thrown_objects, bool ) ) {
				ImGui::OptionPopup(
					"thrown projectiles settings",
					[ & ]( ) {
						ImGui::Checkbox( "thrown objects name", &GET_VARIABLE( g_variables.m_thrown_objects_name, bool ) );
						if ( GET_VARIABLE( g_variables.m_thrown_objects_name, bool ) ) {
							ImGui::SameLine( );
							ImGui::ColorEdit4( "##thrown objects name color",
						                       &GET_VARIABLE( g_variables.m_thrown_objects_name_color, c_color ), color_picker_alpha_flags );
						}
						ImGui::Checkbox( "thrown objects icon", &GET_VARIABLE( g_variables.m_thrown_objects_icon, bool ) );
						if ( GET_VARIABLE( g_variables.m_thrown_objects_icon, bool ) ) {
							ImGui::SameLine( );
							ImGui::ColorEdit4( "##thrown objects icon color",
						                       &GET_VARIABLE( g_variables.m_thrown_objects_icon_color, c_color ), color_picker_alpha_flags );
						}
					},
					ImVec2( 220.f, -1.f ) );
			}

			ImGui::Checkbox( "grenade path", &GET_VARIABLE( g_variables.m_grenade_path, bool ) );
			if ( GET_VARIABLE( g_variables.m_grenade_path, bool ) ) {
				ImGui::OptionPopup(
					"grenade path settings",
					[ & ]( ) {
						ImGui::ColorEdit4( "path##grenade path color", &GET_VARIABLE( g_variables.m_grenade_path_color, c_color ),
					                       color_picker_alpha_flags );
						ImGui::ColorEdit4( "bounce##grenade path bounce color",
					                       &GET_VARIABLE( g_variables.m_grenade_path_bounce_color, c_color ), color_picker_alpha_flags );
						ImGui::ColorEdit4( "detonation##grenade path detonate color",
					                       &GET_VARIABLE( g_variables.m_grenade_path_detonate_color, c_color ), color_picker_alpha_flags );

						ImGui::Checkbox( "effect radius##grenade path", &GET_VARIABLE( g_variables.m_grenade_path_radius, bool ) );

						ImGui::SliderFloat( "thickness##grenade path", &GET_VARIABLE( g_variables.m_grenade_path_thickness, float ), 1.f, 5.f,
					                        "%.1f px" );
					},
					ImVec2( 220.f, -1.f ) );
			}
		}
		menu_group_end( );

		menu_columns_next( );

		if ( menu_group_begin( "chams" ) ) {
			ImGui::Checkbox( "enable enemy chams", &GET_VARIABLE( g_variables.m_chams_enable, bool ) );

			ImGui::BeginDisabled( !GET_VARIABLE( g_variables.m_chams_enable, bool ) );

			chams_dropdown( "visible material", g_variables.m_chams_visible_layers, g_variables.m_chams_visible_colors,
			                g_variables.m_chams_visible_dark_colors );
			chams_dropdown( "occluded material", g_variables.m_chams_occluded_layers, g_variables.m_chams_occluded_colors,
			                g_variables.m_chams_occluded_dark_colors );

			ImGui::Checkbox( "ragdolls##chams ragdolls", &GET_VARIABLE( g_variables.m_chams_ragdolls, bool ) );

			chams_dropdown( "backtrack material", g_variables.m_chams_backtrack_layers, g_variables.m_chams_backtrack_colors,
			                g_variables.m_chams_backtrack_dark_colors );

			chams_dropdown( "sound material", g_variables.m_chams_sound_layers, g_variables.m_chams_sound_colors,
			                g_variables.m_chams_sound_dark_colors );

			if ( n_chams::impl_t::has_layers( GET_VARIABLE( g_variables.m_chams_sound_layers, std::vector< int > ) ) ) {
				ImGui::SliderFloat( "sound duration", &GET_VARIABLE( g_variables.m_chams_sound_duration, float ), 0.2f, 5.f, "%.1f s" );
				ImGui::Checkbox( "sound fade", &GET_VARIABLE( g_variables.m_chams_sound_fade, bool ) );

				ImGui::SliderFloat( "sound min volume##sound esp chams", &GET_VARIABLE( g_variables.m_sound_esp_min_volume, float ), 0.f, 1.f,
				                    "%.2f", ImGuiSliderFlags_AlwaysClamp );
				ImGui::SliderFloat( "sound max distance##sound esp chams", &GET_VARIABLE( g_variables.m_sound_esp_max_distance, float ), 0.f,
				                    6000.f, "%.0f u", ImGuiSliderFlags_AlwaysClamp );
			}

			ImGui::Checkbox( "backtrack xqz", &GET_VARIABLE( g_variables.m_chams_backtrack_xqz, bool ) );
			ImGui::Combo( "backtrack type", &GET_VARIABLE( g_variables.m_chams_backtrack_type, int ),
			              "oldest record\0all records\0aimbot target record\0" );

			if ( GET_VARIABLE( g_variables.m_chams_backtrack_type, int ) == 1 )
				ImGui::SliderInt( "backtrack max ghosts", &GET_VARIABLE( g_variables.m_chams_backtrack_max_ghosts, int ), 2, 64 );

			ImGui::Checkbox( "backtrack tick gradient", &GET_VARIABLE( g_variables.m_chams_backtrack_gradient, bool ) );

			ImGui::EndDisabled( );

			ImGui::Checkbox( "enable local chams", &GET_VARIABLE( g_variables.m_chams_local_enable, bool ) );

			ImGui::BeginDisabled( !GET_VARIABLE( g_variables.m_chams_local_enable, bool ) );

			chams_dropdown( "weapon material", g_variables.m_chams_weapon_layers, g_variables.m_chams_weapon_colors,
			                g_variables.m_chams_weapon_dark_colors );
			chams_dropdown( "gloves material", g_variables.m_chams_arms_layers, g_variables.m_chams_arms_colors, g_variables.m_chams_arms_dark_colors );
			chams_dropdown( "sleeve material", g_variables.m_chams_sleeve_layers, g_variables.m_chams_sleeve_colors,
			                g_variables.m_chams_sleeve_dark_colors );

			ImGui::EndDisabled( );

			bool two_tone_used = false;
			for ( const std::uint32_t layers : { g_variables.m_chams_visible_layers, g_variables.m_chams_occluded_layers, g_variables.m_chams_backtrack_layers,
			                                     g_variables.m_chams_sound_layers, g_variables.m_chams_weapon_layers, g_variables.m_chams_arms_layers,
			                                     g_variables.m_chams_sleeve_layers } )
				for ( const int slot : GET_VARIABLE( layers, std::vector< int > ) )
					two_tone_used |= slot == n_chams::chams_material_two_tone;

			if ( two_tone_used ) {
				ImGui::SliderFloat( "two tone threshold", &GET_VARIABLE( g_variables.m_chams_two_tone_threshold, float ), 0.f, 1.f, "%.2f",
				                    ImGuiSliderFlags_AlwaysClamp );
				ImGui::SliderFloat( "two tone edge", &GET_VARIABLE( g_variables.m_chams_two_tone_edge, float ), 0.f, 0.5f, "%.2f",
				                    ImGuiSliderFlags_AlwaysClamp );
			}
		}
		menu_group_end( );

		menu_columns_end( );
		break;
	}
	case 1: {
		menu_columns_begin( );

		if ( menu_group_begin( "world" ) ) {
			ImGui::Checkbox( "world fov", &GET_VARIABLE( g_variables.m_world_fov_enable, bool ) );
			if ( GET_VARIABLE( g_variables.m_world_fov_enable, bool ) ) {
				ImGui::SliderFloat( "fov##world", &GET_VARIABLE( g_variables.m_world_fov, float ), -30.f, 60.f, "%.1f" );
			}

			ImGui::Checkbox( "precipitation", &GET_VARIABLE( g_variables.m_precipitation, bool ) );
			if ( GET_VARIABLE( g_variables.m_precipitation, bool ) )
				ImGui::Combo( "type##precipitation", &GET_VARIABLE( g_variables.m_precipitation_type, int ), "rain\0ash\0rain storm\0snow\0" );

			{
				// built ins ( index = e_skybox_type ), then sets found in csgo\materials\skybox ( rescanned on open )
				static const char* const k_skyboxes[] = { "off",        "cloudy",     "night",     "night (blue)", "baggage",    "tibet",
				                                          "vietnam",    "lunacy",     "embassy",   "italy",        "jungle",     "office",
				                                          "daylight 1", "daylight 2", "daylight 3", "daylight 4",  "day 02_05",  "nuke blank",
				                                          "dust blank", "venice",     "vertigo",   "vertigo blue", "dust",       "aztec",
				                                          "minecraft" };
				static_assert( IM_ARRAYSIZE( k_skyboxes ) == e_skybox_type::skybox_custom );

				int& skybox         = GET_VARIABLE( g_variables.m_skybox, int );
				std::string& custom = GET_VARIABLE( g_variables.m_skybox_custom, std::string );
				const char* preview = skybox == e_skybox_type::skybox_custom ? custom.c_str( )
				                      : skybox >= 0 && skybox < e_skybox_type::skybox_custom ? k_skyboxes[ skybox ]
				                                                                             : "off";
				if ( ImGui::BeginCombo( "skybox", preview ) ) {
					if ( ImGui::IsWindowAppearing( ) )
						g_edicts.scan_custom_skyboxes( );

					for ( int i = 0; i < e_skybox_type::skybox_custom; i++ )
						if ( ImGui::Selectable( k_skyboxes[ i ], skybox == i ) )
							skybox = i;

					for ( const std::string& name : g_edicts.m_custom_skyboxes )
						if ( ImGui::Selectable( name.c_str( ), skybox == e_skybox_type::skybox_custom && custom == name ) ) {
							skybox = e_skybox_type::skybox_custom;
							custom = name;
						}

					ImGui::EndCombo( );
				}
			}

			ImGui::Checkbox( "sun angle", &GET_VARIABLE( g_variables.m_sun_angle, bool ) );
			if ( GET_VARIABLE( g_variables.m_sun_angle, bool ) ) {
				ImGui::OptionPopup(
					"sun angle settings",
					[ & ]( ) {
						ImGui::SliderFloat( "pitch##sun angle", &GET_VARIABLE( g_variables.m_sun_angle_pitch, float ), -180.f, 180.f, "%.1f" );
						ImGui::SliderFloat( "yaw##sun angle", &GET_VARIABLE( g_variables.m_sun_angle_yaw, float ), -180.f, 180.f, "%.1f" );
						ImGui::SliderFloat( "roll##sun angle", &GET_VARIABLE( g_variables.m_sun_angle_roll, float ), -180.f, 180.f, "%.1f" );

						ImGui::SliderFloat( "rotation speed##sun angle", &GET_VARIABLE( g_variables.m_sun_angle_speed, float ), 0.f, 90.f,
						                    "%.1f deg/s" );

						ImGui::SliderFloat( "shadow distance##sun angle", &GET_VARIABLE( g_variables.m_sun_angle_shadow_distance, float ), 0.f,
						                    3000.f, "%.0f u" );
					},
					ImVec2( 220.f, -1.f ) );
			}

			ImGui::Checkbox( "fog", &GET_VARIABLE( g_variables.m_fog, bool ) );
			if ( GET_VARIABLE( g_variables.m_fog, bool ) ) {
				ImGui::OptionPopup(
					"fog settings",
					[ & ]( ) {
						ImGui::ColorEdit4( "color##fog color picker", &GET_VARIABLE( g_variables.m_fog_color, c_color ), color_picker_alpha_flags );

						ImGui::SliderFloat( "start##fog", &GET_VARIABLE( g_variables.m_fog_start, float ), -5000.f, 5000.f, "%.0f u" );
						ImGui::SliderFloat( "end##fog", &GET_VARIABLE( g_variables.m_fog_end, float ), -5000.f, 5000.f, "%.0f u" );
						ImGui::SliderFloat( "density##fog", &GET_VARIABLE( g_variables.m_fog_density, float ), 0.f, 100.f, "%.0f%%",
						                    ImGuiSliderFlags_AlwaysClamp );
					},
					ImVec2( 220.f, -1.f ) );
			}

			ImGui::Checkbox( "fullbright", &GET_VARIABLE( g_variables.m_fullbright, bool ) );

			ImGui::Checkbox( "flip world", &GET_VARIABLE( g_variables.m_flip_world, bool ) );
			if ( GET_VARIABLE( g_variables.m_flip_world, bool ) )
				ImGui::Checkbox( "gun other side##flip world", &GET_VARIABLE( g_variables.m_flip_world_mirror_hand, bool ) );

			ImGui::Checkbox( "dlight", &GET_VARIABLE( g_variables.m_dlight, bool ) );
			if ( GET_VARIABLE( g_variables.m_dlight, bool ) ) {
				ImGui::OptionPopup(
					"dlight settings",
					[ & ]( ) {
						ImGui::ColorEdit4( "enemies##dlight", &GET_VARIABLE( g_variables.m_dlight_color, c_color ), color_picker_no_alpha_flags );
						ImGui::Checkbox( "local player##dlight", &GET_VARIABLE( g_variables.m_dlight_local, bool ) );
						if ( GET_VARIABLE( g_variables.m_dlight_local, bool ) )
							ImGui::ColorEdit4( "local##dlight local color", &GET_VARIABLE( g_variables.m_dlight_local_color, c_color ),
							                   color_picker_no_alpha_flags );
						ImGui::SliderFloat( "radius##dlight", &GET_VARIABLE( g_variables.m_dlight_radius, float ), 25.f, 1000.f, "%.0f u",
						                    ImGuiSliderFlags_AlwaysClamp );
						ImGui::SliderInt( "brightness##dlight", &GET_VARIABLE( g_variables.m_dlight_brightness, int ), 0, 10, "%d",
						                  ImGuiSliderFlags_AlwaysClamp );
					},
					ImVec2( 220.f, -1.f ) );
			}

			ImGui::Combo( "ragdoll gravity", &GET_VARIABLE( g_variables.m_ragdoll_gravity, int ), "off\0away\0fly up\0" );
			if ( GET_VARIABLE( g_variables.m_ragdoll_gravity, int ) > 0 )
				ImGui::SliderFloat( "strength##ragdoll gravity", &GET_VARIABLE( g_variables.m_ragdoll_gravity_strength, float ), 0.1f, 5.f, "%.1fx",
				                    ImGuiSliderFlags_AlwaysClamp );

			ImGui::Checkbox( "custom smoke color", &GET_VARIABLE( g_variables.m_custom_smoke, bool ) );
			if ( GET_VARIABLE( g_variables.m_custom_smoke, bool ) ) {
				ImGui::SameLine( );
				ImGui::ColorEdit4( "##custom smoke color picker", &GET_VARIABLE( g_variables.m_custom_smoke_color, c_color ),
				                   color_picker_alpha_flags );
			}

			ImGui::Checkbox( "custom molotov color", &GET_VARIABLE( g_variables.m_custom_molotov, bool ) );
			if ( GET_VARIABLE( g_variables.m_custom_molotov, bool ) ) {
				ImGui::SameLine( );
				ImGui::ColorEdit4( "##custom molotov color picker", &GET_VARIABLE( g_variables.m_custom_molotov_color, c_color ),
				                   color_picker_alpha_flags );
			}

			ImGui::Checkbox( "custom blood color", &GET_VARIABLE( g_variables.m_custom_blood, bool ) );
			if ( GET_VARIABLE( g_variables.m_custom_blood, bool ) ) {
				ImGui::SameLine( );
				ImGui::ColorEdit4( "##custom blood color picker", &GET_VARIABLE( g_variables.m_custom_blood_color, c_color ),
				                   color_picker_alpha_flags );
			}

			ImGui::Checkbox( "custom precipitation color", &GET_VARIABLE( g_variables.m_custom_precipitation, bool ) );
			if ( GET_VARIABLE( g_variables.m_custom_precipitation, bool ) ) {
				ImGui::SameLine( );
				ImGui::ColorEdit4( "##custom precipitation color picker", &GET_VARIABLE( g_variables.m_custom_precipitation_color, c_color ),
				                   color_picker_alpha_flags );
			}

			ImGui::Checkbox( "footstep fx", &GET_VARIABLE( g_variables.m_footstep_fx, bool ) );
			if ( GET_VARIABLE( g_variables.m_footstep_fx, bool ) ) {
				ImGui::Combo( "type##footstep fx", &GET_VARIABLE( g_variables.m_footstep_fx_type, int ),
				              "blood\0feathers\0fireball\0slime\0confetti\0splash\0ring trail\0" );
				ImGui::Checkbox( "custom color##footstep fx", &GET_VARIABLE( g_variables.m_footstep_fx_custom_color, bool ) );
				if ( GET_VARIABLE( g_variables.m_footstep_fx_custom_color, bool ) ) {
					ImGui::SameLine( );
					ImGui::ColorEdit4( "##footstep fx color picker", &GET_VARIABLE( g_variables.m_footstep_fx_color, c_color ),
					                   color_picker_alpha_flags );
				}
			}

			modulation_row( "world modulation", g_variables.m_world_modulation, g_variables.m_world_modulation_color,
			                g_variables.m_world_modulation_brightness );
			modulation_row( "prop modulation", g_variables.m_prop_modulation, g_variables.m_prop_modulation_color,
			                g_variables.m_prop_modulation_brightness );
			modulation_row( "skybox modulation", g_variables.m_skybox_modulation, g_variables.m_skybox_modulation_color,
			                g_variables.m_skybox_modulation_brightness );

			ImGui::Checkbox( "lighting modulation", &GET_VARIABLE( g_variables.m_lighting_modulation, bool ) );
			if ( GET_VARIABLE( g_variables.m_lighting_modulation, bool ) ) {
				ImGui::OptionPopup(
					"lighting modulation settings",
					[ & ]( ) {
						ImGui::ColorEdit4( "color##lighting modulation", &GET_VARIABLE( g_variables.m_lighting_modulation_color, c_color ),
						                   color_picker_no_alpha_flags );
						ImGui::Checkbox( "lamp lights##lighting modulation", &GET_VARIABLE( g_variables.m_lighting_modulation_lamps, bool ) );
						if ( GET_VARIABLE( g_variables.m_lighting_modulation_lamps, bool ) ) {
							ImGui::SliderInt( "intensity##lighting modulation", &GET_VARIABLE( g_variables.m_lighting_modulation_intensity, int ), 0,
							                  10, "%d", ImGuiSliderFlags_AlwaysClamp );
							ImGui::SliderFloat( "radius##lighting modulation", &GET_VARIABLE( g_variables.m_lighting_modulation_radius, float ), 25.f,
							                    1000.f, "%.0f u", ImGuiSliderFlags_AlwaysClamp );
							ImGui::SliderInt( "max lights##lighting modulation", &GET_VARIABLE( g_variables.m_lighting_modulation_max, int ), 1, 16,
							                  "%d", ImGuiSliderFlags_AlwaysClamp );
							ImGui::SliderFloat( "max distance##lighting modulation",
							                    &GET_VARIABLE( g_variables.m_lighting_modulation_distance, float ), 100.f, 8000.f, "%.0f u",
							                    ImGuiSliderFlags_AlwaysClamp );
						}
						ImGui::Checkbox( "light glows##lighting modulation", &GET_VARIABLE( g_variables.m_lighting_modulation_glows, bool ) );
						if ( GET_VARIABLE( g_variables.m_lighting_modulation_glows, bool ) )
							ImGui::SliderFloat( "glow brightness##lighting modulation",
							                    &GET_VARIABLE( g_variables.m_lighting_modulation_glow_brightness, float ), 0.f, 400.f, "%.0f%%",
							                    ImGuiSliderFlags_AlwaysClamp );
					},
					ImVec2( 220.f, -1.f ) );
			}

			ImGui::Checkbox( "old shaders", &GET_VARIABLE( g_variables.m_old_shaders, bool ) );
			if ( GET_VARIABLE( g_variables.m_old_shaders, bool ) ) {
				ImGui::OptionPopup(
					"old shaders settings",
					[ & ]( ) {
						ImGui::Checkbox( "weapons", &GET_VARIABLE( g_variables.m_old_shaders_weapons, bool ) );
						ImGui::Checkbox( "arms", &GET_VARIABLE( g_variables.m_old_shaders_arms, bool ) );
						ImGui::Checkbox( "players", &GET_VARIABLE( g_variables.m_old_shaders_players, bool ) );
						ImGui::Checkbox( "world", &GET_VARIABLE( g_variables.m_old_shaders_world, bool ) );
						ImGui::Checkbox( "props", &GET_VARIABLE( g_variables.m_old_shaders_props, bool ) );
					},
					ImVec2( 220.f, -1.f ) );
			}

			ImGui::Checkbox( "old csgo", &GET_VARIABLE( g_variables.m_old_csgo, bool ) );
			if ( GET_VARIABLE( g_variables.m_old_csgo, bool ) ) {
				ImGui::OptionPopup(
					"old csgo settings",
					[ & ]( ) {
						// same order as n_old_mods::impl_t::toggle
						bool* const toggles[ n_old_mods::k_max_packs ] = {
							&GET_VARIABLE( g_variables.m_old_csgo_players, bool ), &GET_VARIABLE( g_variables.m_old_csgo_sounds, bool ),
							&GET_VARIABLE( g_variables.m_old_csgo_ranks, bool ), &GET_VARIABLE( g_variables.m_old_csgo_misc, bool ),
							&GET_VARIABLE( g_variables.m_old_csgo_inspect, bool ) };

						for ( int i = 0; i < n_old_mods::impl_t::count( ); ++i ) {
							ImGui::Checkbox( std::format( "{}##old csgo", n_old_mods::impl_t::label( i ) ).c_str( ), toggles[ i ] );

							const auto& pack = g_old_mods.m_packs[ i ];
							const int state  = pack.m_state;
							if ( !*toggles[ i ] || ( state != n_old_mods::state_downloading && state != n_old_mods::state_failed ) )
								continue;

							const std::string overlay = state == n_old_mods::state_failed
							                                ? std::format( "{} failed, re-tick", pack.m_missing.load( ) )
							                                : std::string( "downloading" );
							ImGui::ProgressBar( g_old_mods.progress( i ), ImVec2( -1.f, 0.f ), overlay.c_str( ) );
						}
					},
					ImVec2( 220.f, -1.f ) );
			}

			ImGui::Checkbox( "world texture", &GET_VARIABLE( g_variables.m_world_texture, bool ) );
			if ( GET_VARIABLE( g_variables.m_world_texture, bool ) ) {
				ImGui::OptionPopup(
					"world texture settings",
					[ & ]( ) {
						constexpr auto preset_label = []( void*, int index, const char** out ) {
							*out = n_misc::world_texture_presets[ index ].m_label;
							return true;
						};

						auto& presets = GET_VARIABLE( g_variables.m_world_texture_preset, std::vector< int > );
						auto& customs = GET_VARIABLE( g_variables.m_world_texture_custom, std::vector< std::string > );
						constexpr std::size_t category_count = n_misc::world_texture_category_count;
						if ( presets.size( ) < category_count )
							presets.resize( category_count, 0 );
						if ( customs.size( ) < category_count )
							customs.resize( category_count );

						int all = presets[ 0 ];
						for ( int category = 1; category < n_misc::world_texture_category_count; category++ )
							all = presets[ category ] == all ? all : -1;

						if ( ImGui::Combo( "all##world texture", &all, preset_label, nullptr, n_misc::world_texture_preset_count ) )
							std::fill( presets.begin( ), presets.begin( ) + n_misc::world_texture_category_count, all );

						ImGui::Checkbox( "remove decals", &GET_VARIABLE( g_variables.m_world_texture_remove_decals, bool ) );

						static char custom_buffers[ n_misc::world_texture_category_count ][ 128 ] = { };

						for ( int category = 0; category < n_misc::world_texture_category_count; category++ ) {
							ImGui::PushID( category );
							ImGui::Combo( n_misc::world_texture_categories[ category ], &presets[ category ], preset_label, nullptr,
							              n_misc::world_texture_preset_count );

							if ( presets[ category ] == n_misc::world_texture_preset_custom ) {
								char* buffer = custom_buffers[ category ];
								ImGui::InputTextWithHint( "##custom", "materials/ path, no .vtf", buffer, sizeof( custom_buffers[ category ] ) );
								if ( ImGui::IsItemDeactivatedAfterEdit( ) )
									customs[ category ] = buffer;
								else if ( !ImGui::IsItemActive( ) && customs[ category ] != buffer ) {
									std::strncpy( buffer, customs[ category ].c_str( ), sizeof( custom_buffers[ category ] ) - 1 );
									buffer[ sizeof( custom_buffers[ category ] ) - 1 ] = '\0';
								}
							}
							ImGui::PopID( );
						}
					},
					ImVec2( 260.f, -1.f ) );
			}

			ImGui::Checkbox( "prop texture", &GET_VARIABLE( g_variables.m_prop_texture, bool ) );
			if ( GET_VARIABLE( g_variables.m_prop_texture, bool ) ) {
				ImGui::OptionPopup(
					"prop texture settings",
					[ & ]( ) {
						constexpr auto preset_label = []( void*, int index, const char** out ) {
							*out = n_misc::world_texture_presets[ index ].m_label;
							return true;
						};

						auto& preset = GET_VARIABLE( g_variables.m_prop_texture_preset, int );
						auto& custom = GET_VARIABLE( g_variables.m_prop_texture_custom, std::string );

						ImGui::Combo( "texture##prop texture", &preset, preset_label, nullptr, n_misc::world_texture_preset_count );

						if ( preset == n_misc::world_texture_preset_custom ) {
							static char buffer[ 128 ] = { };
							ImGui::InputTextWithHint( "##prop custom", "materials/ path, no .vtf", buffer, sizeof( buffer ) );
							if ( ImGui::IsItemDeactivatedAfterEdit( ) )
								custom = buffer;
							else if ( !ImGui::IsItemActive( ) && custom != buffer ) {
								std::strncpy( buffer, custom.c_str( ), sizeof( buffer ) - 1 );
								buffer[ sizeof( buffer ) - 1 ] = '\0';
							}
						}
					},
					ImVec2( 220.f, -1.f ) );
			}

			/* every world material's base texture -> one pixel of its own colour ( wins over world texture ) */
			ImGui::Checkbox( "flat textures", &GET_VARIABLE( g_variables.m_flat_textures, bool ) );

			ImGui::Checkbox( "bullet tracers", &GET_VARIABLE( g_variables.m_bullet_tracers, bool ) );
			if ( GET_VARIABLE( g_variables.m_bullet_tracers, bool ) ) {
				ImGui::OptionPopup(
					"bullet tracers settings",
					[ & ]( ) {
						ImGui::Checkbox( "local##bullet tracers", &GET_VARIABLE( g_variables.m_bullet_tracers_local, bool ) );
						ImGui::SameLine( );
						ImGui::ColorEdit4( "##bullet tracers local color", &GET_VARIABLE( g_variables.m_bullet_tracers_local_color, c_color ),
						                   color_picker_alpha_flags );

						ImGui::Checkbox( "enemies##bullet tracers", &GET_VARIABLE( g_variables.m_bullet_tracers_enemy, bool ) );
						ImGui::SameLine( );
						ImGui::ColorEdit4( "##bullet tracers enemy color", &GET_VARIABLE( g_variables.m_bullet_tracers_enemy_color, c_color ),
						                   color_picker_alpha_flags );

						ImGui::Checkbox( "teammates##bullet tracers", &GET_VARIABLE( g_variables.m_bullet_tracers_team, bool ) );
						ImGui::SameLine( );
						ImGui::ColorEdit4( "##bullet tracers team color", &GET_VARIABLE( g_variables.m_bullet_tracers_team_color, c_color ),
						                   color_picker_alpha_flags );

						ImGui::Combo( "sprite##bullet tracers", &GET_VARIABLE( g_variables.m_bullet_tracers_sprite, int ),
						              "purple laser\0phys beam\0laser beam\0white\0" );
						ImGui::SliderFloat( "life##bullet tracers", &GET_VARIABLE( g_variables.m_bullet_tracers_life, float ), 0.1f, 10.f, "%.1f s",
						                    ImGuiSliderFlags_AlwaysClamp );
						ImGui::SliderFloat( "width##bullet tracers", &GET_VARIABLE( g_variables.m_bullet_tracers_width, float ), 0.5f, 10.f, "%.1f",
						                    ImGuiSliderFlags_AlwaysClamp );
					},
					ImVec2( 220.f, -1.f ) );
			}

			ImGui::Checkbox( "movement trail", &GET_VARIABLE( g_variables.m_movement_trail, bool ) );
			if ( GET_VARIABLE( g_variables.m_movement_trail, bool ) ) {
				ImGui::OptionPopup(
					"movement trail settings",
					[ & ]( ) {
						ImGui::ColorEdit4( "color##movement trail", &GET_VARIABLE( g_variables.m_movement_trail_color, c_color ), color_picker_alpha_flags );
						ImGui::ColorEdit4( "crouch color##movement trail", &GET_VARIABLE( g_variables.m_movement_trail_crouch_color, c_color ),
						                   color_picker_alpha_flags );
						ImGui::Checkbox( "rainbow##movement trail", &GET_VARIABLE( g_variables.m_movement_trail_rainbow, bool ) );
						ImGui::Checkbox( "air only##movement trail", &GET_VARIABLE( g_variables.m_movement_trail_air_only, bool ) );
						ImGui::Combo( "sprite##movement trail", &GET_VARIABLE( g_variables.m_movement_trail_sprite, int ),
						              "purple laser\0phys beam\0laser beam\0white\0" );
						ImGui::SliderFloat( "life##movement trail", &GET_VARIABLE( g_variables.m_movement_trail_life, float ), 0.1f, 10.f, "%.1f s",
						                    ImGuiSliderFlags_AlwaysClamp );
						ImGui::SliderFloat( "width##movement trail", &GET_VARIABLE( g_variables.m_movement_trail_width, float ), 0.5f, 20.f, "%.1f",
						                    ImGuiSliderFlags_AlwaysClamp );
					},
					ImVec2( 220.f, -1.f ) );
			}

			ImGui::Checkbox( "bullet impacts", &GET_VARIABLE( g_variables.m_bullet_impacts, bool ) );
			if ( GET_VARIABLE( g_variables.m_bullet_impacts, bool ) ) {
				ImGui::OptionPopup(
					"bullet impacts settings",
					[ & ]( ) {
						ImGui::ColorEdit4( "client color##bullet impacts", &GET_VARIABLE( g_variables.m_bullet_impacts_client_color, c_color ),
						                   color_picker_alpha_flags );
						ImGui::ColorEdit4( "server color##bullet impacts", &GET_VARIABLE( g_variables.m_bullet_impacts_server_color, c_color ),
						                   color_picker_alpha_flags );
						ImGui::SliderFloat( "size##bullet impacts", &GET_VARIABLE( g_variables.m_bullet_impacts_size, float ), 0.5f, 10.f, "%.1f u",
						                    ImGuiSliderFlags_AlwaysClamp );
						ImGui::SliderFloat( "duration##bullet impacts", &GET_VARIABLE( g_variables.m_bullet_impacts_duration, float ), 0.1f, 30.f,
						                    "%.1f s", ImGuiSliderFlags_AlwaysClamp );
					},
					ImVec2( 220.f, -1.f ) );
			}

			ImGui::Checkbox( "world hit marker", &GET_VARIABLE( g_variables.m_world_hit_marker, bool ) );
			if ( GET_VARIABLE( g_variables.m_world_hit_marker, bool ) ) {
				ImGui::OptionPopup(
					"world hit marker settings",
					[ & ]( ) {
						ImGui::ColorEdit4( "color##world hit marker", &GET_VARIABLE( g_variables.m_world_hit_marker_color, c_color ),
						                   color_picker_alpha_flags );
						ImGui::SliderFloat( "size##world hit marker", &GET_VARIABLE( g_variables.m_world_hit_marker_size, float ), 2.f, 20.f, "%.0f px",
						                    ImGuiSliderFlags_AlwaysClamp );
						ImGui::SliderFloat( "duration##world hit marker", &GET_VARIABLE( g_variables.m_world_hit_marker_duration, float ), 0.1f, 5.f,
						                    "%.1f s", ImGuiSliderFlags_AlwaysClamp );
					},
					ImVec2( 220.f, -1.f ) );
			}
		}
		menu_group_end( );

		menu_columns_next( );

		if ( menu_group_begin( "removals" ) ) {
			safe_checkbox( "disable post processing", &GET_VARIABLE( g_variables.m_disable_post_processing, bool ) );
			ImGui::Checkbox( "remove panorama blur", &GET_VARIABLE( g_variables.m_remove_panorama_blur, bool ) );

			ImGui::Checkbox( "remove 3d skybox", &GET_VARIABLE( g_variables.m_remove_3d_skybox, bool ) );

			ImGui::Checkbox( "remove hud elements", &GET_VARIABLE( g_variables.m_remove_hud_elements, bool ) );
			if ( GET_VARIABLE( g_variables.m_remove_hud_elements, bool ) ) {
				ImGui::MultiCombo( "elements##remove hud", GET_VARIABLE( g_variables.m_removed_hud_elements, std::vector< bool > ),
			                       { "health & armor", "ammo", "weapon selection", "killfeed", "radar", "money", "timer & team counter", "chat",
			                         "alerts", "hint text", "radio", "vote", "spectator panel", "death cam", "win panel", "c4 icon (under radar)" },
			                       GET_VARIABLE( g_variables.m_removed_hud_elements, std::vector< bool > ).size( ) );
			}

			ImGui::Checkbox( "remove ragdolls", &GET_VARIABLE( g_variables.m_remove_ragdolls, bool ) );
			ImGui::Checkbox( "remove smoke", &GET_VARIABLE( g_variables.m_remove_smoke, bool ) );
			if ( GET_VARIABLE( g_variables.m_remove_smoke, bool ) ) {
				ImGui::OptionPopup(
					"remove smoke settings",
					[]( ) {
						ImGui::Combo( "smoke##remove smoke mode", &GET_VARIABLE( g_variables.m_remove_smoke_mode, int ),
					                  "wireframe\0disabled\0" );
					},
					ImVec2( 220.f, -1.f ) );
			}

			ImGui::Checkbox( "remove flash", &GET_VARIABLE( g_variables.m_remove_flash, bool ) );
			if ( GET_VARIABLE( g_variables.m_remove_flash, bool ) ) {
				ImGui::OptionPopup(
					"remove flash settings",
					[]( ) {
						ImGui::SliderFloat( "opacity##remove flash", &GET_VARIABLE( g_variables.m_remove_flash_opacity, float ), 0.f,
					                        100.f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp );
					},
					ImVec2( 220.f, -1.f ) );
			}

			ImGui::Checkbox( "remove visual recoil", &GET_VARIABLE( g_variables.m_remove_visual_recoil, bool ) );
#ifdef _DEBUG
			ImGui::Checkbox( "disable interp", &GET_VARIABLE( g_variables.m_disable_interp, bool ) );
#endif
		}
		menu_group_end( );

		if ( menu_group_begin( "viewmodel" ) ) {
			ImGui::Checkbox( "viewmodel offset", &GET_VARIABLE( g_variables.m_viewmodel_offset_enable, bool ) );
			if ( GET_VARIABLE( g_variables.m_viewmodel_offset_enable, bool ) ) {
				ImGui::OptionPopup(
					"viewmodel offset settings",
					[ & ]( ) {
						ImGui::SliderFloat( "x##viewmodel", &GET_VARIABLE( g_variables.m_viewmodel_x, float ), -30.f, 30.f, "%.2f" );
						ImGui::SliderFloat( "y##viewmodel", &GET_VARIABLE( g_variables.m_viewmodel_y, float ), -30.f, 30.f, "%.2f" );
						ImGui::SliderFloat( "z##viewmodel", &GET_VARIABLE( g_variables.m_viewmodel_z, float ), -30.f, 30.f, "%.2f" );
						ImGui::SliderFloat( "roll##viewmodel", &GET_VARIABLE( g_variables.m_viewmodel_roll, float ), -180.f, 180.f, "%.1f" );
					},
					ImVec2( 220.f, -1.f ) );
			}

			ImGui::Checkbox( "viewmodel fov", &GET_VARIABLE( g_variables.m_viewmodel_fov_enable, bool ) );
			if ( GET_VARIABLE( g_variables.m_viewmodel_fov_enable, bool ) ) {
				ImGui::OptionPopup(
					"viewmodel fov settings",
					[ & ]( ) { ImGui::SliderFloat( "fov##viewmodel", &GET_VARIABLE( g_variables.m_viewmodel_fov, float ), 1.f, 179.f, "%.1f" ); },
					ImVec2( 220.f, -1.f ) );
			}

			ImGui::Checkbox( "weapon sway", &GET_VARIABLE( g_variables.m_weapon_sway, bool ) );
			if ( GET_VARIABLE( g_variables.m_weapon_sway, bool ) ) {
				ImGui::OptionPopup(
					"weapon sway settings",
					[ & ]( ) {
						ImGui::SliderFloat( "scale##weapon sway", &GET_VARIABLE( g_variables.m_weapon_sway_scale, float ), 0.f, 50.f, "%.2f" );
					},
					ImVec2( 220.f, -1.f ) );
			}

			ImGui::Checkbox( "weapon sheen", &GET_VARIABLE( g_variables.m_weapon_sheen, bool ) );
			if ( GET_VARIABLE( g_variables.m_weapon_sheen, bool ) ) {
				ImGui::OptionPopup(
					"weapon sheen settings",
					[ & ]( ) {
						ImGui::ColorEdit4( "color##weapon sheen color", &GET_VARIABLE( g_variables.m_weapon_sheen_color, c_color ),
						                   color_picker_alpha_flags );
						ImGui::SliderFloat( "delay##weapon sheen", &GET_VARIABLE( g_variables.m_weapon_sheen_delay, float ), 0.f, 10.f, "%.1f s",
						                    ImGuiSliderFlags_AlwaysClamp );
						ImGui::SliderFloat( "width##weapon sheen", &GET_VARIABLE( g_variables.m_weapon_sheen_width, float ), 0.1f, 1.f, "%.2f",
						                    ImGuiSliderFlags_AlwaysClamp );
						ImGui::Checkbox( "normal blend##weapon sheen", &GET_VARIABLE( g_variables.m_weapon_sheen_normal, bool ) );
						if ( GET_VARIABLE( g_variables.m_weapon_sheen_normal, bool ) )
							ImGui::SliderFloat( "env strength##weapon sheen", &GET_VARIABLE( g_variables.m_weapon_sheen_env, float ), 0.f, 10.f, "%.2f",
							                    ImGuiSliderFlags_AlwaysClamp );
					},
					ImVec2( 220.f, -1.f ) );
			}

			ImGui::Checkbox( "bob changer", &GET_VARIABLE( g_variables.m_viewmodel_bob, bool ) );
			if ( GET_VARIABLE( g_variables.m_viewmodel_bob, bool ) ) {
				ImGui::OptionPopup(
					"bob changer settings",
					[ & ]( ) {
						ImGui::SliderFloat( "cycle", &GET_VARIABLE( g_variables.m_viewmodel_bob_cycle, float ), 0.f, 5.f, "%.2f" );
						ImGui::SliderFloat( "vertical", &GET_VARIABLE( g_variables.m_viewmodel_bob_vertical, float ), 0.f, 10.f, "%.2f" );
						ImGui::SliderFloat( "lateral", &GET_VARIABLE( g_variables.m_viewmodel_bob_lateral, float ), 0.f, 10.f, "%.2f" );
						ImGui::SliderFloat( "lower amount", &GET_VARIABLE( g_variables.m_viewmodel_bob_lower, float ), 0.f, 100.f, "%.1f" );

						ImGui::SliderFloat( "shift left", &GET_VARIABLE( g_variables.m_viewmodel_shift_left, float ), 0.f, 10.f, "%.2f" );
						ImGui::SliderFloat( "shift right", &GET_VARIABLE( g_variables.m_viewmodel_shift_right, float ), 0.f, 10.f, "%.2f" );
					},
					ImVec2( 220.f, -1.f ) );
			}
		}
		menu_group_end( );

		menu_columns_end( );
		break;
	}
	case 2: {
		menu_columns_begin( );

		if ( menu_group_begin( "screen" ) ) {
			ImGui::Checkbox( "aspect ratio", &GET_VARIABLE( g_variables.m_aspect_ratio_enable, bool ) );
			if ( GET_VARIABLE( g_variables.m_aspect_ratio_enable, bool ) ) {
				ImGui::OptionPopup(
					"aspect ratio settings",
					[ & ]( ) {
						auto& live_ratio = GET_VARIABLE( g_variables.m_aspect_ratio, float );

						static float pending_ratio = 0.f;
						static float applied_ratio = 0.f;

						if ( pending_ratio <= 0.f || applied_ratio != live_ratio ) {
							pending_ratio = live_ratio;
							applied_ratio = live_ratio;
						}

						ImGui::SliderFloat( "ratio##aspect ratio", &pending_ratio, 0.5f, 4.f, "%.3f", ImGuiSliderFlags_AlwaysClamp );

						if ( ImGui::Button( "apply##aspect ratio", ImVec2( -1.f, 15.f ) ) ) {
							live_ratio    = pending_ratio;
							applied_ratio = pending_ratio;
						}

						ImGui::Checkbox( "stretch hud##aspect ratio", &GET_VARIABLE( g_variables.m_aspect_ratio_hud, bool ) );

						ImGui::Checkbox( "stretch crosshair##aspect ratio", &GET_VARIABLE( g_variables.m_aspect_ratio_overlay, bool ) );

						ImGui::Checkbox( "stretch cheat overlays##aspect ratio",
						                 &GET_VARIABLE( g_variables.m_aspect_ratio_cheat_overlay, bool ) );
					},
					ImVec2( 220.f, -1.f ) );
			}

			/* real res untouched: finished frame (world + hud + menus + overlays) crunched to the res and scaled back up */
			ImGui::Checkbox( "resolution spoofer", &GET_VARIABLE( g_variables.m_resolution_spoof, bool ) );
			if ( GET_VARIABLE( g_variables.m_resolution_spoof, bool ) ) {
				ImGui::OptionPopup(
					"resolution spoofer settings",
					[ & ]( ) {
						struct res_t {
							int m_width, m_height;
							const char* m_aspect;
						};

						static constexpr res_t k_resolutions[] = {
							{ 1920, 1440, "4:3" },  { 1920, 1080, "16:9" }, { 1680, 1050, "16:10" }, { 1600, 1200, "4:3" },
							{ 1600, 900, "16:9" },  { 1440, 1080, "4:3" },  { 1440, 900, "16:10" },  { 1366, 768, "16:9" },
							{ 1280, 1024, "5:4" },  { 1280, 960, "4:3" },   { 1280, 800, "16:10" },  { 1280, 720, "16:9" },
							{ 1200, 900, "4:3" },   { 1152, 864, "4:3" },   { 1024, 768, "4:3" },    { 1024, 576, "16:9" },
							{ 960, 720, "4:3" },    { 960, 540, "16:9" },   { 800, 600, "4:3" },     { 640, 480, "4:3" },
						};

						int& width  = GET_VARIABLE( g_variables.m_resolution_spoof_width, int );
						int& height = GET_VARIABLE( g_variables.m_resolution_spoof_height, int );

						// real back buffer from Present; 0 before the first frame = list everything
						const int screen_width  = g_resolution_spoof.m_screen_width;
						const int screen_height = g_resolution_spoof.m_screen_height;

						const std::string preview = std::to_string( width ) + "x" + std::to_string( height );

						if ( ImGui::BeginCombo( "resolution##res spoof", preview.c_str( ) ) ) {
							for ( const res_t& res : k_resolutions ) {
								if ( screen_width > 0 && ( res.m_width > screen_width || res.m_height > screen_height ||
								                           ( res.m_width == screen_width && res.m_height == screen_height ) ) )
									continue;

								const std::string label = std::to_string( res.m_width ) + "x" + std::to_string( res.m_height ) + "  " + res.m_aspect;

								if ( ImGui::Selectable( label.c_str( ), width == res.m_width && height == res.m_height ) ) {
									width  = res.m_width;
									height = res.m_height;
								}
							}

							ImGui::EndCombo( );
						}

						// off: square pixels at the res's line count, world keeps the screen shape
						ImGui::Checkbox( "stretch to fill##res spoof", &GET_VARIABLE( g_variables.m_resolution_spoof_stretch, bool ) );

						ImGui::Checkbox( "scale net graph##res spoof", &GET_VARIABLE( g_variables.m_resolution_spoof_net_graph, bool ) );
					},
					ImVec2( 220.f, -1.f ) );
			}

			/* one shader pass over the finished back buffer in end_scene; menu drawn after, never graded */
			ImGui::Checkbox( "color correction", &GET_VARIABLE( g_variables.m_color_correction, bool ) );
			if ( GET_VARIABLE( g_variables.m_color_correction, bool ) ) {
				ImGui::OptionPopup(
					"color correction settings",
					[ & ]( ) {
						ImGui::SliderFloat( "exposure##color correction", &GET_VARIABLE( g_variables.m_color_correction_exposure, float ), -3.f,
						                    3.f, "%+.2f ev" );

						ImGui::Label( "levels" );
						ImGui::SliderFloat( "in black##color correction", &GET_VARIABLE( g_variables.m_color_correction_levels_in_black, float ),
						                    0.f, 0.99f, "%.2f", ImGuiSliderFlags_AlwaysClamp );
						ImGui::SliderFloat( "in white##color correction", &GET_VARIABLE( g_variables.m_color_correction_levels_in_white, float ),
						                    0.01f, 1.f, "%.2f", ImGuiSliderFlags_AlwaysClamp );
						ImGui::SliderFloat( "gamma##color correction", &GET_VARIABLE( g_variables.m_color_correction_levels_gamma, float ), 0.1f,
						                    10.f, "%.2f", ImGuiSliderFlags_AlwaysClamp );
						ImGui::SliderFloat( "out black##color correction",
						                    &GET_VARIABLE( g_variables.m_color_correction_levels_out_black, float ), 0.f, 1.f, "%.2f",
						                    ImGuiSliderFlags_AlwaysClamp );
						ImGui::SliderFloat( "out white##color correction",
						                    &GET_VARIABLE( g_variables.m_color_correction_levels_out_white, float ), 0.f, 1.f, "%.2f",
						                    ImGuiSliderFlags_AlwaysClamp );

						// the remap divides by ( in white - in black ), so they can never meet
						if ( GET_VARIABLE( g_variables.m_color_correction_levels_in_black, float ) >=
						     GET_VARIABLE( g_variables.m_color_correction_levels_in_white, float ) )
							GET_VARIABLE( g_variables.m_color_correction_levels_in_white, float ) =
								GET_VARIABLE( g_variables.m_color_correction_levels_in_black, float ) + 0.01f;

						draw_color_correction_curves( );

						ImGui::Label( "tone" );
						ImGui::SliderFloat( "highlights##color correction", &GET_VARIABLE( g_variables.m_color_correction_highlights, float ),
						                    -1.f, 1.f, "%+.2f" );
						ImGui::SliderFloat( "shadows##color correction", &GET_VARIABLE( g_variables.m_color_correction_shadows, float ), -1.f,
						                    1.f, "%+.2f" );

						ImGui::Label( "color" );
						ImGui::SliderFloat( "red##color correction", &GET_VARIABLE( g_variables.m_color_correction_red, float ), 0.f, 2.f,
						                    "%.2f" );
						ImGui::SliderFloat( "green##color correction", &GET_VARIABLE( g_variables.m_color_correction_green, float ), 0.f, 2.f,
						                    "%.2f" );
						ImGui::SliderFloat( "blue##color correction", &GET_VARIABLE( g_variables.m_color_correction_blue, float ), 0.f, 2.f,
						                    "%.2f" );
						ImGui::SliderFloat( "hue##color correction", &GET_VARIABLE( g_variables.m_color_correction_hue, float ), -180.f, 180.f,
						                    "%.1f" );
						ImGui::SliderFloat( "saturation##color correction", &GET_VARIABLE( g_variables.m_color_correction_saturation, float ),
						                    0.f, 3.f, "%.2f" );
						ImGui::SliderFloat( "contrast##color correction", &GET_VARIABLE( g_variables.m_color_correction_contrast, float ), 0.5f,
						                    1.5f, "%.3f" );
						ImGui::SliderFloat( "temperature##color correction", &GET_VARIABLE( g_variables.m_color_correction_temperature, float ),
						                    -1.f, 1.f, "%+.2f" );

						if ( ImGui::Button( "reset##color correction", ImVec2( -1.f, 15.f ) ) )
							reset_color_correction( );
					},
					ImVec2( 260.f, -1.f ) );
			}

			ImGui::Checkbox( "sharpen", &GET_VARIABLE( g_variables.m_sharpen, bool ) );
			if ( GET_VARIABLE( g_variables.m_sharpen, bool ) ) {
				ImGui::OptionPopup(
					"sharpen settings",
					[ & ]( ) {
						ImGui::SliderFloat( "strength##sharpen", &GET_VARIABLE( g_variables.m_sharpen_strength, float ), 0.f, 5.f, "%.2f",
						                    ImGuiSliderFlags_AlwaysClamp );

						ImGui::SliderFloat( "threshold##sharpen", &GET_VARIABLE( g_variables.m_sharpen_threshold, float ), 0.f, 0.2f, "%.3f",
						                    ImGuiSliderFlags_AlwaysClamp );
					},
					ImVec2( 220.f, -1.f ) );
			}

			ImGui::Checkbox( "film grain", &GET_VARIABLE( g_variables.m_film_grain, bool ) );
			if ( GET_VARIABLE( g_variables.m_film_grain, bool ) ) {
				ImGui::OptionPopup(
					"film grain settings",
					[ & ]( ) {
						ImGui::SliderFloat( "intensity##film grain", &GET_VARIABLE( g_variables.m_film_grain_intensity, float ), 0.f, 0.3f,
						                    "%.3f", ImGuiSliderFlags_AlwaysClamp );

						ImGui::SliderFloat( "size##film grain", &GET_VARIABLE( g_variables.m_film_grain_size, float ), 1.f, 4.f, "%.2f",
						                    ImGuiSliderFlags_AlwaysClamp );

						ImGui::SliderFloat( "response position##film grain", &GET_VARIABLE( g_variables.m_film_grain_response_position, float ),
						                    0.f, 1.f, "%.2f", ImGuiSliderFlags_AlwaysClamp );
						ImGui::SliderFloat( "response range##film grain", &GET_VARIABLE( g_variables.m_film_grain_response_range, float ), 0.05f,
						                    1.f, "%.2f", ImGuiSliderFlags_AlwaysClamp );
						ImGui::SliderFloat( "minimum response##film grain", &GET_VARIABLE( g_variables.m_film_grain_response_minimum, float ),
						                    0.f, 1.f, "%.2f", ImGuiSliderFlags_AlwaysClamp );
					},
					ImVec2( 220.f, -1.f ) );
			}

			ImGui::Checkbox( "vignette", &GET_VARIABLE( g_variables.m_vignette, bool ) );
			if ( GET_VARIABLE( g_variables.m_vignette, bool ) ) {
				ImGui::OptionPopup(
					"vignette settings",
					[ & ]( ) {
						ImGui::ColorEdit4( "color##vignette", &GET_VARIABLE( g_variables.m_vignette_color, c_color ), color_picker_alpha_flags );

						ImGui::SliderFloat( "radius##vignette", &GET_VARIABLE( g_variables.m_vignette_radius, float ), 0.f, 1.f, "%.2f",
						                    ImGuiSliderFlags_AlwaysClamp );
						ImGui::SliderFloat( "feather##vignette", &GET_VARIABLE( g_variables.m_vignette_feather, float ), 0.f, 1.f, "%.2f",
						                    ImGuiSliderFlags_AlwaysClamp );

						ImGui::SliderFloat( "aspect ratio##vignette", &GET_VARIABLE( g_variables.m_vignette_aspect, float ), 0.25f, 4.f, "%.2f",
						                    ImGuiSliderFlags_AlwaysClamp );

						ImGui::SliderFloat( "blur##vignette", &GET_VARIABLE( g_variables.m_vignette_blur, float ), 0.f, 32.f, "%.1f",
						                    ImGuiSliderFlags_AlwaysClamp );

						ImGui::Checkbox( "invert##vignette", &GET_VARIABLE( g_variables.m_vignette_invert, bool ) );
					},
					ImVec2( 220.f, -1.f ) );
			}

			ImGui::Checkbox( "invert", &GET_VARIABLE( g_variables.m_invert_filter, bool ) );
			if ( GET_VARIABLE( g_variables.m_invert_filter, bool ) ) {
				ImGui::OptionPopup(
					"invert settings",
					[ & ]( ) {
						ImGui::Combo( "mode##invert", &GET_VARIABLE( g_variables.m_invert_filter_mode, int ), "color\0" "brightness\0" );
						ImGui::SliderFloat( "strength##invert", &GET_VARIABLE( g_variables.m_invert_filter_strength, float ), 0.f, 1.f, "%.2f",
						                    ImGuiSliderFlags_AlwaysClamp );
					},
					ImVec2( 220.f, -1.f ) );
			}

			ImGui::Checkbox( "posterize", &GET_VARIABLE( g_variables.m_posterize, bool ) );
			if ( GET_VARIABLE( g_variables.m_posterize, bool ) ) {
				ImGui::OptionPopup(
					"posterize settings",
					[ & ]( ) {
						ImGui::SliderInt( "levels##posterize", &GET_VARIABLE( g_variables.m_posterize_levels, int ), 2, 64, "%d",
						                  ImGuiSliderFlags_AlwaysClamp );
						ImGui::SliderFloat( "dither##posterize", &GET_VARIABLE( g_variables.m_posterize_dither, float ), 0.f, 1.f, "%.2f",
						                    ImGuiSliderFlags_AlwaysClamp );
						ImGui::SliderFloat( "strength##posterize", &GET_VARIABLE( g_variables.m_posterize_strength, float ), 0.f, 1.f, "%.2f",
						                    ImGuiSliderFlags_AlwaysClamp );
					},
					ImVec2( 220.f, -1.f ) );
			}

			ImGui::Checkbox( "deband", &GET_VARIABLE( g_variables.m_deband, bool ) );
			if ( GET_VARIABLE( g_variables.m_deband, bool ) ) {
				ImGui::OptionPopup(
					"deband settings",
					[ & ]( ) {
						ImGui::SliderInt( "quality##deband", &GET_VARIABLE( g_variables.m_deband_iterations, int ), 1, 4, "%d",
						                  ImGuiSliderFlags_AlwaysClamp );
						ImGui::SliderFloat( "strength##deband", &GET_VARIABLE( g_variables.m_deband_strength, float ), 1.f, 50.f, "%.0f",
						                    ImGuiSliderFlags_AlwaysClamp );
						ImGui::SliderFloat( "radius##deband", &GET_VARIABLE( g_variables.m_deband_radius, float ), 4.f, 48.f, "%.0f px",
						                    ImGuiSliderFlags_AlwaysClamp );
						ImGui::SliderFloat( "dither##deband", &GET_VARIABLE( g_variables.m_deband_grain, float ), 0.f, 64.f, "%.0f",
						                    ImGuiSliderFlags_AlwaysClamp );
					},
					ImVec2( 220.f, -1.f ) );
			}

			ImGui::Checkbox( "reflections", &GET_VARIABLE( g_variables.m_reflections, bool ) );
			if ( GET_VARIABLE( g_variables.m_reflections, bool ) ) {
				ImGui::OptionPopup(
					"reflections settings",
					[ & ]( ) {
						ImGui::SliderFloat( "intensity##reflections", &GET_VARIABLE( g_variables.m_reflections_intensity, float ), 0.f, 1.f,
						                    "%.2f", ImGuiSliderFlags_AlwaysClamp );
						ImGui::SliderFloat( "distance##reflections", &GET_VARIABLE( g_variables.m_reflections_distance, float ), 0.05f, 1.f,
						                    "%.2f", ImGuiSliderFlags_AlwaysClamp );
						ImGui::SliderFloat( "blur##reflections", &GET_VARIABLE( g_variables.m_reflections_blur, float ), 0.f, 5.f, "%.1f",
						                    ImGuiSliderFlags_AlwaysClamp );
						ImGui::SliderFloat( "angle falloff##reflections", &GET_VARIABLE( g_variables.m_reflections_falloff, float ), 0.f, 1.f,
						                    "%.2f", ImGuiSliderFlags_AlwaysClamp );

						ImGui::Label( "performance" );

						ImGui::SliderFloat( "render size##reflections", &GET_VARIABLE( g_variables.m_reflections_scale, float ), 0.25f, 1.f,
						                    "%.2f", ImGuiSliderFlags_AlwaysClamp );
						ImGui::Combo( "ray quality##reflections", &GET_VARIABLE( g_variables.m_reflections_quality, int ),
						              "low\0" "medium\0" "high\0" );
						ImGui::Checkbox( "smooth normals##reflections", &GET_VARIABLE( g_variables.m_reflections_smooth_normals, bool ) );
					},
					ImVec2( 220.f, -1.f ) );
			}

			ImGui::Checkbox( "emphasize", &GET_VARIABLE( g_variables.m_emphasize, bool ) );
			if ( GET_VARIABLE( g_variables.m_emphasize, bool ) ) {
				ImGui::OptionPopup(
					"emphasize settings",
					[ & ]( ) {
						ImGui::SliderFloat( "focus depth##emphasize", &GET_VARIABLE( g_variables.m_emphasize_focus_depth, float ), 0.f, 1.f, "%.3f",
						                    ImGuiSliderFlags_AlwaysClamp );
						ImGui::SliderFloat( "focus range##emphasize", &GET_VARIABLE( g_variables.m_emphasize_focus_range, float ), 0.f, 1.f, "%.3f",
						                    ImGuiSliderFlags_AlwaysClamp );
						ImGui::SliderFloat( "focus edge##emphasize", &GET_VARIABLE( g_variables.m_emphasize_focus_edge, float ), 0.f, 1.f, "%.3f",
						                    ImGuiSliderFlags_AlwaysClamp );

						ImGui::Checkbox( "spherical##emphasize", &GET_VARIABLE( g_variables.m_emphasize_spherical, bool ) );
						if ( GET_VARIABLE( g_variables.m_emphasize_spherical, bool ) ) {
							ImGui::SliderInt( "sphere fov##emphasize", &GET_VARIABLE( g_variables.m_emphasize_sphere_fov, int ), 1, 180, "%d deg",
							                  ImGuiSliderFlags_AlwaysClamp );
							ImGui::SliderFloat( "focus x##emphasize", &GET_VARIABLE( g_variables.m_emphasize_sphere_x, float ), 0.f, 1.f, "%.3f",
							                    ImGuiSliderFlags_AlwaysClamp );
							ImGui::SliderFloat( "focus y##emphasize", &GET_VARIABLE( g_variables.m_emphasize_sphere_y, float ), 0.f, 1.f, "%.3f",
							                    ImGuiSliderFlags_AlwaysClamp );
						}

						ImGui::ColorEdit4( "blend color##emphasize", &GET_VARIABLE( g_variables.m_emphasize_blend_color, c_color ),
						                   color_picker_no_alpha_flags );
						ImGui::SliderFloat( "blend factor##emphasize", &GET_VARIABLE( g_variables.m_emphasize_blend_factor, float ), 0.f, 1.f,
						                    "%.3f", ImGuiSliderFlags_AlwaysClamp );
						ImGui::SliderFloat( "effect factor##emphasize", &GET_VARIABLE( g_variables.m_emphasize_effect_factor, float ), 0.f, 1.f,
						                    "%.3f", ImGuiSliderFlags_AlwaysClamp );
					},
					ImVec2( 220.f, -1.f ) );
			}

			ImGui::Checkbox( "show depth", &GET_VARIABLE( g_variables.m_show_depth, bool ) );
			if ( GET_VARIABLE( g_variables.m_show_depth, bool ) ) {
				ImGui::OptionPopup(
					"show depth settings",
					[ & ]( ) {
						ImGui::Combo( "present type##show depth", &GET_VARIABLE( g_variables.m_show_depth_present_type, int ),
						              "depth map\0" "normal map\0" "show both\0" );
						ImGui::Checkbox( "blend into image##show depth", &GET_VARIABLE( g_variables.m_show_depth_blend, bool ) );

						ImGui::Checkbox( "live preview##show depth", &GET_VARIABLE( g_variables.m_show_depth_live_preview, bool ) );
						if ( GET_VARIABLE( g_variables.m_show_depth_live_preview, bool ) ) {
							ImGui::Checkbox( "upside down##show depth", &GET_VARIABLE( g_variables.m_show_depth_upside_down, bool ) );
							ImGui::Checkbox( "reversed##show depth", &GET_VARIABLE( g_variables.m_show_depth_reversed, bool ) );
							ImGui::Checkbox( "logarithmic##show depth", &GET_VARIABLE( g_variables.m_show_depth_logarithmic, bool ) );
							ImGui::SliderFloat( "scale x##show depth", &GET_VARIABLE( g_variables.m_show_depth_scale_x, float ), 0.f, 2.f, "%.3f",
							                    ImGuiSliderFlags_AlwaysClamp );
							ImGui::SliderFloat( "scale y##show depth", &GET_VARIABLE( g_variables.m_show_depth_scale_y, float ), 0.f, 2.f, "%.3f",
							                    ImGuiSliderFlags_AlwaysClamp );

							const ImVec2 screen = ImVec2( g_ctx.m_width, g_ctx.m_height );
							ImGui::SliderInt( "offset x##show depth", &GET_VARIABLE( g_variables.m_show_depth_offset_x, int ),
							                  -static_cast< int >( screen.x ), static_cast< int >( screen.x ), "%d px" );
							ImGui::SliderInt( "offset y##show depth", &GET_VARIABLE( g_variables.m_show_depth_offset_y, int ),
							                  -static_cast< int >( screen.y ), static_cast< int >( screen.y ), "%d px" );

							ImGui::SliderFloat( "far plane##show depth", &GET_VARIABLE( g_variables.m_show_depth_far_plane, float ), 0.f, 1000.f,
							                    "%.1f", ImGuiSliderFlags_AlwaysClamp );
							ImGui::SliderFloat( "multiplier##show depth", &GET_VARIABLE( g_variables.m_show_depth_multiplier, float ), 0.f, 1000.f,
							                    "%.3f", ImGuiSliderFlags_AlwaysClamp );
						}
					},
					ImVec2( 220.f, -1.f ) );
			}

			ImGui::Checkbox( "channel shift", &GET_VARIABLE( g_variables.m_channel_shift, bool ) );
			if ( GET_VARIABLE( g_variables.m_channel_shift, bool ) ) {
				ImGui::OptionPopup(
					"channel shift settings",
					[ & ]( ) {
						ImGui::SliderFloat( "red x##channel shift", &GET_VARIABLE( g_variables.m_channel_shift_red_x, float ), -32.f, 32.f, "%.1f px" );
						ImGui::SliderFloat( "red y##channel shift", &GET_VARIABLE( g_variables.m_channel_shift_red_y, float ), -32.f, 32.f, "%.1f px" );
						ImGui::SliderFloat( "green x##channel shift", &GET_VARIABLE( g_variables.m_channel_shift_green_x, float ), -32.f, 32.f,
						                    "%.1f px" );
						ImGui::SliderFloat( "green y##channel shift", &GET_VARIABLE( g_variables.m_channel_shift_green_y, float ), -32.f, 32.f,
						                    "%.1f px" );
						ImGui::SliderFloat( "blue x##channel shift", &GET_VARIABLE( g_variables.m_channel_shift_blue_x, float ), -32.f, 32.f,
						                    "%.1f px" );
						ImGui::SliderFloat( "blue y##channel shift", &GET_VARIABLE( g_variables.m_channel_shift_blue_y, float ), -32.f, 32.f,
						                    "%.1f px" );

						ImGui::Checkbox( "depth based##channel shift", &GET_VARIABLE( g_variables.m_channel_shift_depth, bool ) );
						if ( GET_VARIABLE( g_variables.m_channel_shift_depth, bool ) ) {
							ImGui::Combo( "depth side##channel shift", &GET_VARIABLE( g_variables.m_channel_shift_depth_mode, int ),
							              "background\0" "foreground\0" "both sides\0" );
							ImGui::SliderFloat( "distance##channel shift", &GET_VARIABLE( g_variables.m_channel_shift_depth_distance, float ), 0.f, 4000.f,
							                    "%.0f u", ImGuiSliderFlags_AlwaysClamp );
							ImGui::SliderFloat( "range##channel shift", &GET_VARIABLE( g_variables.m_channel_shift_depth_range, float ), 0.f, 2000.f,
							                    "%.0f u", ImGuiSliderFlags_AlwaysClamp );
							ImGui::SliderFloat( "ramp##channel shift", &GET_VARIABLE( g_variables.m_channel_shift_depth_ramp, float ), 1.f, 4000.f,
							                    "%.0f u", ImGuiSliderFlags_AlwaysClamp );
						}
					},
					ImVec2( 220.f, -1.f ) );
			}

			ImGui::Checkbox( "depth distort", &GET_VARIABLE( g_variables.m_depth_distort, bool ) );
			if ( GET_VARIABLE( g_variables.m_depth_distort, bool ) ) {
				ImGui::OptionPopup(
					"depth distort settings",
					[ & ]( ) {
						int& effect = GET_VARIABLE( g_variables.m_depth_distort_effect, int );
						ImGui::Combo( "effect##depth distort", &effect,
						              "bulge / pinch\0" "swirl\0" "spliced radials\0" "zigzag\0" "ripple\0" "wave\0" "drunk\0" "tiny planet\0"
						              "slit scan\0" "radial slit scan\0" );
						const bool radial = effect <= 4;

						int& animate = GET_VARIABLE( g_variables.m_depth_distort_animate, int );
						constexpr const char* k_animate[] = { "no\0" "yes\0", "no\0" "amplitude\0" "phase\0", "no\0" "amplitude\0" "phase\0" "angle\0" };
						const int anim_kind = ( effect == 3 || effect == 4 ) ? 1 : ( effect == 5 ? 2 : 0 );
						if ( effect != 6 && effect != 7 ) {
							if ( animate < 0 || animate > anim_kind + 1 )
								animate = 0;
							ImGui::Combo( "animate##depth distort", &animate, k_animate[ anim_kind ] );
							if ( animate )
								ImGui::SliderFloat( "animation speed##depth distort", &GET_VARIABLE( g_variables.m_depth_distort_anim_speed, float ), 0.05f,
								                    10.f, "%.2f", ImGuiSliderFlags_AlwaysClamp );
						}

						ImGui::Label( "properties" );

						switch ( effect ) {
						case 0:
							ImGui::SliderFloat( "magnitude##depth distort", &GET_VARIABLE( g_variables.m_depth_distort_magnitude, float ), -1.f, 1.f, "%.2f",
							                    ImGuiSliderFlags_AlwaysClamp );
							break;
						case 1:
						case 2:
							ImGui::SliderFloat( "angle##depth distort", &GET_VARIABLE( g_variables.m_depth_distort_angle, float ), -1800.f, 1800.f,
							                    "%.0f deg", ImGuiSliderFlags_AlwaysClamp );
							ImGui::SliderFloat( "inner radius##depth distort", &GET_VARIABLE( g_variables.m_depth_distort_inner_radius, float ), 0.f, 1.f,
							                    "%.3f", ImGuiSliderFlags_AlwaysClamp );
							if ( effect == 1 )
								ImGui::Checkbox( "inverse angle##depth distort", &GET_VARIABLE( g_variables.m_depth_distort_inverse, bool ) );
							else
								ImGui::SliderInt( "splices##depth distort", &GET_VARIABLE( g_variables.m_depth_distort_splices, int ), 1, 50, "%d",
								                  ImGuiSliderFlags_AlwaysClamp );
							break;
						case 3:
						case 4:
							ImGui::SliderFloat( "angle##depth distort", &GET_VARIABLE( g_variables.m_depth_distort_angle, float ), -999.f, 999.f, "%.0f",
							                    ImGuiSliderFlags_AlwaysClamp );
							ImGui::SliderFloat( "period##depth distort", &GET_VARIABLE( g_variables.m_depth_distort_period, float ), 0.1f, 10.f, "%.2f",
							                    ImGuiSliderFlags_AlwaysClamp );
							ImGui::SliderFloat( "amplitude##depth distort", &GET_VARIABLE( g_variables.m_depth_distort_amplitude, float ), -10.f, 10.f,
							                    "%.2f", ImGuiSliderFlags_AlwaysClamp );
							ImGui::SliderFloat( "phase##depth distort", &GET_VARIABLE( g_variables.m_depth_distort_phase, float ), -5.f, 5.f, "%.2f",
							                    ImGuiSliderFlags_AlwaysClamp );
							break;
						case 5:
							ImGui::Combo( "wave type##depth distort", &GET_VARIABLE( g_variables.m_depth_distort_wave_type, int ), "x/x\0" "x/y\0" );
							ImGui::SliderFloat( "angle##depth distort", &GET_VARIABLE( g_variables.m_depth_distort_wave_angle, float ), -360.f, 360.f,
							                    "%.0f deg", ImGuiSliderFlags_AlwaysClamp );
							ImGui::SliderFloat( "period##depth distort", &GET_VARIABLE( g_variables.m_depth_distort_wave_period, float ), 0.1f, 10.f,
							                    "%.2f", ImGuiSliderFlags_AlwaysClamp );
							ImGui::SliderFloat( "amplitude##depth distort", &GET_VARIABLE( g_variables.m_depth_distort_wave_amplitude, float ), -1.f, 1.f,
							                    "%.3f", ImGuiSliderFlags_AlwaysClamp );
							ImGui::SliderFloat( "phase##depth distort", &GET_VARIABLE( g_variables.m_depth_distort_phase, float ), -5.f, 5.f, "%.2f",
							                    ImGuiSliderFlags_AlwaysClamp );
							break;
						case 6:
							ImGui::SliderFloat( "strength x##depth distort", &GET_VARIABLE( g_variables.m_depth_distort_drunk_strength_x, float ), 0.f,
							                    100.f, "%.1f", ImGuiSliderFlags_AlwaysClamp );
							ImGui::SliderFloat( "strength y##depth distort", &GET_VARIABLE( g_variables.m_depth_distort_drunk_strength_y, float ), 0.f,
							                    100.f, "%.1f", ImGuiSliderFlags_AlwaysClamp );
							ImGui::SliderFloat( "speed##depth distort", &GET_VARIABLE( g_variables.m_depth_distort_drunk_speed, float ), 0.f, 10.f, "%.1f",
							                    ImGuiSliderFlags_AlwaysClamp );
							ImGui::SliderFloat( "bending angle##depth distort", &GET_VARIABLE( g_variables.m_depth_distort_drunk_angle, float ), -1800.f,
							                    1800.f, "%.0f deg", ImGuiSliderFlags_AlwaysClamp );
							ImGui::SliderFloat( "bending speed##depth distort", &GET_VARIABLE( g_variables.m_depth_distort_drunk_angle_speed, float ), 0.f,
							                    10.f, "%.1f", ImGuiSliderFlags_AlwaysClamp );
							break;
						case 7:
							ImGui::SliderFloat( "scale##depth distort", &GET_VARIABLE( g_variables.m_depth_distort_planet_scale, float ), 0.f, 10.f, "%.2f",
							                    ImGuiSliderFlags_AlwaysClamp );
							ImGui::SliderFloat( "rotation x##depth distort", &GET_VARIABLE( g_variables.m_depth_distort_planet_rot_x, float ), 0.f, 1.f,
							                    "%.3f", ImGuiSliderFlags_AlwaysClamp );
							ImGui::SliderFloat( "rotation y##depth distort", &GET_VARIABLE( g_variables.m_depth_distort_planet_rot_y, float ), 0.f, 1.f,
							                    "%.3f", ImGuiSliderFlags_AlwaysClamp );
							ImGui::SliderFloat( "rotation z##depth distort", &GET_VARIABLE( g_variables.m_depth_distort_planet_rot_z, float ), 0.f, 360.f,
							                    "%.1f deg", ImGuiSliderFlags_AlwaysClamp );
							ImGui::SliderFloat( "offset x##depth distort", &GET_VARIABLE( g_variables.m_depth_distort_planet_offset_x, float ), -0.5f, 0.5f,
							                    "%.3f", ImGuiSliderFlags_AlwaysClamp );
							ImGui::SliderFloat( "offset y##depth distort", &GET_VARIABLE( g_variables.m_depth_distort_planet_offset_y, float ), -0.5f, 0.5f,
							                    "%.3f", ImGuiSliderFlags_AlwaysClamp );
							ImGui::SliderFloat( "seam blending##depth distort", &GET_VARIABLE( g_variables.m_depth_distort_planet_seam, float ), 0.5f, 1.f,
							                    "%.3f", ImGuiSliderFlags_AlwaysClamp );
							break;
						case 8:
							ImGui::Combo( "direction##depth distort", &GET_VARIABLE( g_variables.m_depth_distort_slit_direction, int ),
							              "left\0" "right\0" "up\0" "down\0" );
							if ( !animate )
								ImGui::SliderFloat( "position##depth distort", &GET_VARIABLE( g_variables.m_depth_distort_slit_position, float ), 0.f, 1.f,
								                    "%.3f", ImGuiSliderFlags_AlwaysClamp );
							ImGui::SliderFloat( "scan speed##depth distort", &GET_VARIABLE( g_variables.m_depth_distort_slit_speed, float ), 0.f, 3.f,
							                    "%.2f", ImGuiSliderFlags_AlwaysClamp );
							break;
						case 9:
							if ( !animate )
								ImGui::SliderFloat( "position##depth distort", &GET_VARIABLE( g_variables.m_depth_distort_slit_position, float ), 0.f, 1.f,
								                    "%.3f", ImGuiSliderFlags_AlwaysClamp );
							ImGui::ColorEdit4( "border color##depth distort", &GET_VARIABLE( g_variables.m_depth_distort_border_color, c_color ) );
							break;
						default:
							break;
						}

						if ( radial || effect == 9 ) {
							ImGui::SliderFloat( "center x##depth distort", &GET_VARIABLE( g_variables.m_depth_distort_center_x, float ), 0.f, 1.f, "%.3f",
							                    ImGuiSliderFlags_AlwaysClamp );
							ImGui::SliderFloat( "center y##depth distort", &GET_VARIABLE( g_variables.m_depth_distort_center_y, float ), 0.f, 1.f, "%.3f",
							                    ImGuiSliderFlags_AlwaysClamp );
						}

						if ( radial ) {
							ImGui::SliderFloat( "radius##depth distort", &GET_VARIABLE( g_variables.m_depth_distort_radius, float ), 0.f, 1.f, "%.3f",
							                    ImGuiSliderFlags_AlwaysClamp );
							ImGui::SliderFloat( "tension##depth distort", &GET_VARIABLE( g_variables.m_depth_distort_tension, float ), 0.f, 10.f, "%.3f",
							                    ImGuiSliderFlags_AlwaysClamp );
							ImGui::SliderFloat( "aspect ratio##depth distort", &GET_VARIABLE( g_variables.m_depth_distort_aspect, float ), -100.f, 100.f,
							                    "%.1f", ImGuiSliderFlags_AlwaysClamp );
							ImGui::SliderFloat( "aspect angle##depth distort", &GET_VARIABLE( g_variables.m_depth_distort_aspect_angle, float ), -180.f,
							                    180.f, "%.1f deg", ImGuiSliderFlags_AlwaysClamp );
							ImGui::Checkbox( "use offset coordinates##depth distort", &GET_VARIABLE( g_variables.m_depth_distort_use_offset, bool ) );
							if ( GET_VARIABLE( g_variables.m_depth_distort_use_offset, bool ) ) {
								ImGui::SliderFloat( "offset x##depth distort", &GET_VARIABLE( g_variables.m_depth_distort_offset_x, float ), 0.f, 1.f, "%.3f",
								                    ImGuiSliderFlags_AlwaysClamp );
								ImGui::SliderFloat( "offset y##depth distort", &GET_VARIABLE( g_variables.m_depth_distort_offset_y, float ), 0.f, 1.f, "%.3f",
								                    ImGuiSliderFlags_AlwaysClamp );
							}
						}

						ImGui::Label( "blending" );
						ImGui::Combo( "blending mode##depth distort", &GET_VARIABLE( g_variables.m_depth_distort_blend, int ),
						              "normal\0" "darken\0" "  multiply\0" "  color burn\0" "  linear burn\0" "lighten\0" "  screen\0"
						              "  color dodge\0" "  linear dodge\0" "  addition\0" "  glow\0" "overlay\0" "  soft light\0"
						              "  hard light\0" "  vivid light\0" "  linear light\0" "  pin light\0" "  hard mix\0" "difference\0"
						              "  exclusion\0" "subtract\0" "  divide\0" "  divide (alternative)\0" "  divide (photoshop)\0"
						              "  reflect\0" "  grain extract\0" "  grain merge\0" "hue\0" "  saturation\0" "  color\0"
						              "  luminosity\0" );
						ImGui::SliderFloat( "opacity##depth distort", &GET_VARIABLE( g_variables.m_depth_distort_opacity, float ), 0.f, 1.f, "%.2f",
						                    ImGuiSliderFlags_AlwaysClamp );

						ImGui::Label( "depth" );
						ImGui::Combo( "depth side##depth distort", &GET_VARIABLE( g_variables.m_depth_distort_depth_mode, int ),
						              "background\0" "foreground\0" "both sides\0" );
						ImGui::SliderFloat( "distance##depth distort", &GET_VARIABLE( g_variables.m_depth_distort_depth_distance, float ), 0.f, 4000.f,
						                    "%.0f u", ImGuiSliderFlags_AlwaysClamp );
						ImGui::SliderFloat( "range##depth distort", &GET_VARIABLE( g_variables.m_depth_distort_depth_range, float ), 0.f, 2000.f,
						                    "%.0f u", ImGuiSliderFlags_AlwaysClamp );
						ImGui::SliderFloat( "ramp##depth distort", &GET_VARIABLE( g_variables.m_depth_distort_depth_ramp, float ), 1.f, 4000.f,
						                    "%.0f u", ImGuiSliderFlags_AlwaysClamp );
						if ( effect <= 7 ) {
							ImGui::Checkbox( "fade transition##depth distort", &GET_VARIABLE( g_variables.m_depth_distort_fade, bool ) );
							ImGui::Checkbox( "protect edges##depth distort", &GET_VARIABLE( g_variables.m_depth_distort_protect, bool ) );
						}
					},
					ImVec2( 230.f, -1.f ) );
			}

			ImGui::Checkbox( "depth of field", &GET_VARIABLE( g_variables.m_depth_of_field, bool ) );
			if ( GET_VARIABLE( g_variables.m_depth_of_field, bool ) ) {
				ImGui::OptionPopup(
					"depth of field settings",
					[ & ]( ) {
						ImGui::Label( "focus" );

						ImGui::Checkbox( "auto focus##depth of field", &GET_VARIABLE( g_variables.m_depth_of_field_auto_focus, bool ) );

						if ( !GET_VARIABLE( g_variables.m_depth_of_field_auto_focus, bool ) ) {
							ImGui::SliderFloat( "distance##depth of field", &GET_VARIABLE( g_variables.m_depth_of_field_distance, float ), 16.f,
							                    4000.f, "%.0f u", ImGuiSliderFlags_AlwaysClamp );
						} else {
							ImGui::SliderFloat( "focus point x##depth of field", &GET_VARIABLE( g_variables.m_depth_of_field_focus_x, float ), 0.f,
							                    1.f, "%.2f", ImGuiSliderFlags_AlwaysClamp );
							ImGui::SliderFloat( "focus point y##depth of field", &GET_VARIABLE( g_variables.m_depth_of_field_focus_y, float ), 0.f,
							                    1.f, "%.2f", ImGuiSliderFlags_AlwaysClamp );

							ImGui::SliderFloat( "focus speed##depth of field", &GET_VARIABLE( g_variables.m_depth_of_field_focus_speed, float ),
							                    0.5f, 40.f, "%.1f", ImGuiSliderFlags_AlwaysClamp );

							ImGui::SliderFloat( "focus limit##depth of field", &GET_VARIABLE( g_variables.m_depth_of_field_focus_limit, float ),
							                    256.f, 8192.f, "%.0f u", ImGuiSliderFlags_AlwaysClamp );

							// glass, grates, debris don't steal focus
							ImGui::Checkbox( "see through glass##depth of field",
							                 &GET_VARIABLE( g_variables.m_depth_of_field_focus_see_through, bool ) );
						}

						ImGui::Label( "range" );

						ImGui::SliderFloat( "sharp range##depth of field", &GET_VARIABLE( g_variables.m_depth_of_field_range, float ), 0.f,
						                    3000.f, "%.0f u", ImGuiSliderFlags_AlwaysClamp );

						ImGui::SliderFloat( "blur ramp##depth of field", &GET_VARIABLE( g_variables.m_depth_of_field_ramp, float ), 1.f, 4000.f,
						                    "%.0f u", ImGuiSliderFlags_AlwaysClamp );

						ImGui::Label( "blur" );
						ImGui::SliderFloat( "max blur##depth of field", &GET_VARIABLE( g_variables.m_depth_of_field_max_blur, float ), 0.f,
						                    60.f, "%.1f px", ImGuiSliderFlags_AlwaysClamp );

						ImGui::SliderFloat( "highlights##depth of field",
						                    &GET_VARIABLE( g_variables.m_depth_of_field_bokeh_boost, float ), 0.f, 16.f, "%.1f",
						                    ImGuiSliderFlags_AlwaysClamp );
						ImGui::SliderFloat( "highlight cutoff##depth of field",
						                    &GET_VARIABLE( g_variables.m_depth_of_field_bokeh_threshold, float ), 0.f, 1.f, "%.2f",
						                    ImGuiSliderFlags_AlwaysClamp );

						ImGui::Combo( "quality##depth of field", &GET_VARIABLE( g_variables.m_depth_of_field_quality, int ),
						              "16 taps\0" "32 taps\0" "64 taps\0" "128 taps\0" );

						ImGui::Checkbox( "smooth blur##depth of field", &GET_VARIABLE( g_variables.m_depth_of_field_smooth, bool ) );

						ImGui::Checkbox( "blur near side##depth of field", &GET_VARIABLE( g_variables.m_depth_of_field_near_blur, bool ) );

						if ( GET_VARIABLE( g_variables.m_depth_of_field_near_blur, bool ) )
							ImGui::Checkbox( "show near bleed##depth of field",
							                 &GET_VARIABLE( g_variables.m_depth_of_field_debug_near, bool ) );

						ImGui::Checkbox( "keep viewmodel sharp##depth of field",
						                 &GET_VARIABLE( g_variables.m_depth_of_field_viewmodel_sharp, bool ) );

						ImGui::Label( "debug" );
						ImGui::Checkbox( "show depth##depth of field", &GET_VARIABLE( g_variables.m_depth_of_field_debug_depth, bool ) );

						if ( GET_VARIABLE( g_variables.m_depth_of_field_debug_depth, bool ) )
							ImGui::SliderFloat( "white at##depth of field", &GET_VARIABLE( g_variables.m_depth_of_field_debug_white, float ),
							                    100.f, 8000.f, "%.0f u", ImGuiSliderFlags_AlwaysClamp );

						ImGui::Checkbox( "universal depth##depth of field", &GET_VARIABLE( g_variables.m_depth_force_live, bool ) );

						ImGui::TextDisabled( "depth: %s %s", g_depth_source.route_name( ), g_depth_source.verify_name( ) );
					},
					ImVec2( 220.f, -1.f ) );
			}

			ImGui::Checkbox( "ambient occlusion", &GET_VARIABLE( g_variables.m_ambient_occlusion, bool ) );
			if ( GET_VARIABLE( g_variables.m_ambient_occlusion, bool ) ) {
				ImGui::OptionPopup(
					"ambient occlusion settings",
					[ & ]( ) {
						ImGui::Label( "shading" );

						ImGui::SliderFloat( "radius##ambient occlusion", &GET_VARIABLE( g_variables.m_ambient_occlusion_radius, float ), 4.f,
						                    300.f, "%.0f u", ImGuiSliderFlags_AlwaysClamp );

						ImGui::SliderFloat( "strength##ambient occlusion", &GET_VARIABLE( g_variables.m_ambient_occlusion_intensity, float ),
						                    0.f, 4.f, "%.2f", ImGuiSliderFlags_AlwaysClamp );

						ImGui::SliderFloat( "bias##ambient occlusion", &GET_VARIABLE( g_variables.m_ambient_occlusion_bias, float ), 0.f, 0.8f,
						                    "%.2f", ImGuiSliderFlags_AlwaysClamp );

						ImGui::SliderFloat( "blur##ambient occlusion", &GET_VARIABLE( g_variables.m_ambient_occlusion_blur, float ), 0.f, 3.f,
						                    "%.2f", ImGuiSliderFlags_AlwaysClamp );

						ImGui::Label( "range" );

						ImGui::SliderFloat( "fade from##ambient occlusion", &GET_VARIABLE( g_variables.m_ambient_occlusion_fade_start, float ),
						                    0.f, 8000.f, "%.0f u", ImGuiSliderFlags_AlwaysClamp );
						ImGui::SliderFloat( "fade to##ambient occlusion", &GET_VARIABLE( g_variables.m_ambient_occlusion_fade_end, float ),
						                    0.f, 16000.f, "%.0f u", ImGuiSliderFlags_AlwaysClamp );

						ImGui::Checkbox( "fade under fog##ambient occlusion",
						                 &GET_VARIABLE( g_variables.m_ambient_occlusion_fog_fade, bool ) );

						ImGui::Checkbox( "fade under smoke##ambient occlusion",
						                 &GET_VARIABLE( g_variables.m_ambient_occlusion_smoke_fade, bool ) );

						ImGui::Checkbox( "skip viewmodel##ambient occlusion",
						                 &GET_VARIABLE( g_variables.m_ambient_occlusion_viewmodel, bool ) );

						ImGui::Label( "performance" );

						ImGui::SliderInt( "samples##ambient occlusion", &GET_VARIABLE( g_variables.m_ambient_occlusion_samples, int ), 4, 64,
						                  "%d", ImGuiSliderFlags_AlwaysClamp );
						ImGui::SliderFloat( "render size##ambient occlusion", &GET_VARIABLE( g_variables.m_ambient_occlusion_scale, float ),
						                    0.25f, 1.f, "%.2f", ImGuiSliderFlags_AlwaysClamp );

						ImGui::Label( "debug" );

						ImGui::Checkbox( "show occlusion##ambient occlusion", &GET_VARIABLE( g_variables.m_ambient_occlusion_debug, bool ) );
					},
					ImVec2( 220.f, -1.f ) );
			}

			ImGui::Checkbox( "motion blur", &GET_VARIABLE( g_variables.m_motion_blur, bool ) );

			if ( GET_VARIABLE( g_variables.m_motion_blur, bool ) ) {
				ImGui::OptionPopup(
					"motion blur settings",
					[ & ]( ) {
						ImGui::Checkbox( "blur forward motion", &GET_VARIABLE( g_variables.m_motion_blur_forward, bool ) );

						ImGui::SliderFloat( "blur strength", &GET_VARIABLE( g_variables.m_motion_blur_strength, float ), 0.f, 10.f, "%.2f" );
						ImGui::SliderFloat( "rotation intensity", &GET_VARIABLE( g_variables.m_motion_blur_rotation_intensity, float ), 0.f,
					                        8.f, "%.2f" );
						ImGui::SliderFloat( "falling intensity", &GET_VARIABLE( g_variables.m_motion_blur_falling_intensity, float ), 0.f, 8.f,
					                        "%.2f" );
						ImGui::SliderFloat( "roll intensity", &GET_VARIABLE( g_variables.m_motion_blur_roll_intensity, float ), 0.f, 8.f,
					                        "%.2f" );

						ImGui::SliderFloat( "falling min", &GET_VARIABLE( g_variables.m_motion_blur_falling_min, float ), 0.f, 50.f, "%.2f" );
						ImGui::SliderFloat( "falling max", &GET_VARIABLE( g_variables.m_motion_blur_falling_max, float ), 0.f, 50.f, "%.2f" );
					},
					ImVec2( 220.f, -1.f ) );
			}

			ImGui::Checkbox( "true motion blur", &GET_VARIABLE( g_variables.m_true_motion_blur, bool ) );

			if ( GET_VARIABLE( g_variables.m_true_motion_blur, bool ) ) {
				ImGui::OptionPopup(
					"true motion blur settings",
					[ & ]( ) {
						ImGui::SliderFloat( "shutter##true motion blur", &GET_VARIABLE( g_variables.m_true_motion_blur_shutter, float ), 0.f,
						                    40.f, "%.1f ms", ImGuiSliderFlags_AlwaysClamp );

						ImGui::Label( "amount" );

						ImGui::SliderFloat( "strength##true motion blur", &GET_VARIABLE( g_variables.m_true_motion_blur_camera, float ), 0.f, 3.f,
						                    "%.2f", ImGuiSliderFlags_AlwaysClamp );

						ImGui::Checkbox( "blur players##true motion blur", &GET_VARIABLE( g_variables.m_true_motion_blur_players, bool ) );

						ImGui::SliderFloat( "player trail##true motion blur", &GET_VARIABLE( g_variables.m_true_motion_blur_object, float ), 0.f,
						                    12.f, "%.2f", ImGuiSliderFlags_AlwaysClamp );

						ImGui::Checkbox( "blur viewmodel##true motion blur", &GET_VARIABLE( g_variables.m_true_motion_blur_viewmodel, bool ) );

						/* 1 = true streak (leave it there with bones on). gun only, never the world */
						if ( GET_VARIABLE( g_variables.m_true_motion_blur_viewmodel, bool ) ) {
							ImGui::SliderFloat( "viewmodel amount##true motion blur",
							                    &GET_VARIABLE( g_variables.m_true_motion_blur_viewmodel_strength, float ), 0.f, 32.f, "%.2f",
							                    ImGuiSliderFlags_AlwaysClamp );

							ImGui::SliderFloat( "viewmodel sway##true motion blur",
							                    &GET_VARIABLE( g_variables.m_true_motion_blur_viewmodel_sway, float ), 0.f, 8.f, "%.2f",
							                    ImGuiSliderFlags_AlwaysClamp );
						}

						ImGui::Label( "quality" );

						ImGui::SliderInt( "max samples##true motion blur", &GET_VARIABLE( g_variables.m_true_motion_blur_samples, int ), 4, 64,
						                  "%d", ImGuiSliderFlags_AlwaysClamp );

						ImGui::SliderFloat( "max streak##true motion blur", &GET_VARIABLE( g_variables.m_true_motion_blur_max, float ), 0.005f,
						                    0.1f, "%.3f", ImGuiSliderFlags_AlwaysClamp );

						/* 1 = blur/smoothie frame blend, 2.2 = bright blend (linear light). higher = brights win */
						ImGui::SliderFloat( "blur gamma##true motion blur", &GET_VARIABLE( g_variables.m_true_motion_blur_gamma, float ), 1.f,
						                    10.f, "%.2f", ImGuiSliderFlags_AlwaysClamp );

						/* amd + msaa off = none ( z swap hides the arm ), msaa on = resz */
						ImGui::Checkbox( "universal depth##true motion blur", &GET_VARIABLE( g_variables.m_depth_force_live, bool ) );
						ImGui::TextDisabled( "depth: %s %s", g_depth_source.route_name( ), g_depth_source.verify_name( ) );
					},
					ImVec2( 220.f, -1.f ) );
			}

			ImGui::Checkbox( "bloom", &GET_VARIABLE( g_variables.m_bloom, bool ) );

			if ( GET_VARIABLE( g_variables.m_bloom, bool ) ) {
				ImGui::OptionPopup(
					"bloom settings",
					[ & ]( ) {
						ImGui::SliderFloat( "intensity##bloom", &GET_VARIABLE( g_variables.m_bloom_intensity, float ), 0.f, 3.f, "%.2f",
						                    ImGuiSliderFlags_AlwaysClamp );

						ImGui::SliderFloat( "threshold##bloom", &GET_VARIABLE( g_variables.m_bloom_curve, float ), 0.f, 10.f, "%.2f",
						                    ImGuiSliderFlags_AlwaysClamp );

						ImGui::SliderFloat( "radius##bloom", &GET_VARIABLE( g_variables.m_bloom_radius, float ), 0.f, 1.f, "%.2f",
						                    ImGuiSliderFlags_AlwaysClamp );

						ImGui::SliderFloat( "saturation##bloom", &GET_VARIABLE( g_variables.m_bloom_saturation, float ), 0.f, 3.f, "%.2f",
						                    ImGuiSliderFlags_AlwaysClamp );

						ImGui::SliderFloat( "aspect ratio##bloom", &GET_VARIABLE( g_variables.m_bloom_aspect, float ), -1.f, 1.f, "%.2f",
						                    ImGuiSliderFlags_AlwaysClamp );
					},
					ImVec2( 220.f, -1.f ) );
			}

			ImGui::Checkbox( "low fps viewmodel", &GET_VARIABLE( g_variables.m_viewmodel_low_fps, bool ) );

			if ( GET_VARIABLE( g_variables.m_viewmodel_low_fps, bool ) ) {
				ImGui::OptionPopup(
					"low fps viewmodel settings",
					[ & ]( ) {
						ImGui::SliderFloat( "fps##low fps viewmodel", &GET_VARIABLE( g_variables.m_viewmodel_low_fps_rate, float ), 15.f, 60.f,
						                    "%.0f fps", ImGuiSliderFlags_AlwaysClamp );
					},
					ImVec2( 220.f, -1.f ) );
			}
		}
		menu_group_end( );

		menu_columns_next( );

		if ( menu_group_begin( "hud" ) ) {
			ImGui::Checkbox( "sniper crosshair", &GET_VARIABLE( g_variables.m_sniper_crosshair, bool ) );

			if ( GET_VARIABLE( g_variables.m_sniper_crosshair, bool ) ) {
				ImGui::OptionPopup(
					"sniper crosshair settings",
					[ & ]( ) {
						ImGui::Combo( "style##sniper crosshair style", &GET_VARIABLE( g_variables.m_sniper_crosshair_style, int ),
					                  "clarity\0kamidere\0interwebz\0custom\0" );

						const int style = GET_VARIABLE( g_variables.m_sniper_crosshair_style, int );

						ImGui::ColorEdit4( "color##sniper crosshair color", &GET_VARIABLE( g_variables.m_sniper_crosshair_color, c_color ),
						                   color_picker_alpha_flags );

						if ( style == 3 ) {
							ImGui::SliderFloat( "size##sniper crosshair", &GET_VARIABLE( g_variables.m_sniper_crosshair_size, float ), 0.f, 10.f,
							                    "%.1f" );
							ImGui::SliderFloat( "thickness##sniper crosshair", &GET_VARIABLE( g_variables.m_sniper_crosshair_thickness, float ),
							                    0.f, 3.f, "%.1f" );
							ImGui::SliderFloat( "gap##sniper crosshair", &GET_VARIABLE( g_variables.m_sniper_crosshair_gap, float ), -5.f, 5.f,
							                    "%.1f" );

							ImGui::Checkbox( "dot##sniper crosshair", &GET_VARIABLE( g_variables.m_sniper_crosshair_dot, bool ) );

							ImGui::Checkbox( "t style##sniper crosshair", &GET_VARIABLE( g_variables.m_sniper_crosshair_t, bool ) );

							ImGui::Checkbox( "outline##sniper crosshair", &GET_VARIABLE( g_variables.m_sniper_crosshair_outline, bool ) );
						}

						if ( style == 2 ) {
							ImGui::ColorEdit4( "dot color##sniper crosshair dot color",
							                   &GET_VARIABLE( g_variables.m_sniper_crosshair_dot_color, c_color ), color_picker_alpha_flags );

							ImGui::SliderFloat( "size##sniper crosshair interwebz size",
							                    &GET_VARIABLE( g_variables.m_sniper_crosshair_interwebz_size, float ), 0.5f, 4.f, "%.1fx" );

							ImGui::SliderInt( "thickness##sniper crosshair interwebz thickness",
							                  &GET_VARIABLE( g_variables.m_sniper_crosshair_interwebz_thickness, int ), 1, 8, "%d px" );

							ImGui::SliderInt( "gap##sniper crosshair interwebz gap",
							                  &GET_VARIABLE( g_variables.m_sniper_crosshair_interwebz_gap, int ), 0, 20, "%d px" );

							ImGui::Checkbox( "arm corner outline##sniper crosshair interwebz arm corners",
							                 &GET_VARIABLE( g_variables.m_sniper_crosshair_interwebz_arm_corners, bool ) );

							ImGui::Checkbox( "dot corner outline##sniper crosshair interwebz dot corners",
							                 &GET_VARIABLE( g_variables.m_sniper_crosshair_interwebz_dot_corners, bool ) );
						}

						if ( style == 1 || style == 2 || ( style == 3 && GET_VARIABLE( g_variables.m_sniper_crosshair_outline, bool ) ) ) {
							ImGui::ColorEdit4( "outline color##sniper crosshair outline color",
							                   &GET_VARIABLE( g_variables.m_sniper_crosshair_outline_color, c_color ),
							                   color_picker_alpha_flags );
						}

						if ( style == 3 && GET_VARIABLE( g_variables.m_sniper_crosshair_outline, bool ) ) {
							ImGui::SliderFloat( "outline thickness##sniper crosshair",
							                    &GET_VARIABLE( g_variables.m_sniper_crosshair_outline_thickness, float ), 0.1f, 3.f, "%.1f" );
						}
					},
					ImVec2( 220.f, -1.f ) );
			}

			ImGui::Checkbox( "custom crosshair", &GET_VARIABLE( g_variables.m_custom_crosshair, bool ) );

			if ( GET_VARIABLE( g_variables.m_custom_crosshair, bool ) ) {
				ImGui::OptionPopup(
					"custom crosshair settings",
					[ & ]( ) {
						static bool s_test_running = false;
						static float s_test_speed  = 0.f;
						static double s_test_t0    = 0.0;
						static bool s_preview_bg   = false;

						const auto sides = []( const char* id, bool* left, bool* right, bool* top, bool* bottom ) {
							ImGui::PushID( id );
							ImGui::Checkbox( "left", left );
							ImGui::Checkbox( "right", right );
							ImGui::Checkbox( "top", top );
							ImGui::Checkbox( "bottom", bottom );
							ImGui::PopID( );
						};

						/* popup = AlwaysAutoResize -> default table sizing is FixedFit, force stretch */
						if ( !ImGui::BeginTable( "##custom crosshair columns", 2, ImGuiTableFlags_SizingStretchSame ) )
							return;
						ImGui::TableNextColumn( );

						ImGui::Checkbox( "antialiased##custom crosshair", &GET_VARIABLE( g_variables.m_custom_crosshair_antialias, bool ) );
						ImGui::Checkbox( "dot##custom crosshair", &GET_VARIABLE( g_variables.m_custom_crosshair_dot, bool ) );
						ImGui::Checkbox( "outline##custom crosshair", &GET_VARIABLE( g_variables.m_custom_crosshair_outline, bool ) );
						ImGui::Checkbox( "only some sides##custom crosshair", &GET_VARIABLE( g_variables.m_custom_crosshair_sides_only, bool ) );
						if ( GET_VARIABLE( g_variables.m_custom_crosshair_sides_only, bool ) )
							sides( "custom crosshair sides", &GET_VARIABLE( g_variables.m_custom_crosshair_side_left, bool ),
							       &GET_VARIABLE( g_variables.m_custom_crosshair_side_right, bool ),
							       &GET_VARIABLE( g_variables.m_custom_crosshair_side_top, bool ),
							       &GET_VARIABLE( g_variables.m_custom_crosshair_side_bottom, bool ) );

						if ( n_custom_crosshair::blend_combo( "blend##custom crosshair", &GET_VARIABLE( g_variables.m_custom_crosshair_blend, int ) ) )
							ImGui::SetTooltip( "how the arms + dot mix with the frame under them.\n"
							                   "additive adds light; the rest work like photoshop layer modes.\n"
							                   "alpha is the opacity. the outline always draws normal." );

						ImGui::SliderFloat( "thickness##custom crosshair", &GET_VARIABLE( g_variables.m_custom_crosshair_thickness, float ), 0.5f, 5.f,
						                    "%.1f" );
						ImGui::SliderFloat( "length##custom crosshair", &GET_VARIABLE( g_variables.m_custom_crosshair_length, float ), 0.5f, 20.f,
						                    "%.1f" );
						ImGui::SliderFloat( "gap##custom crosshair", &GET_VARIABLE( g_variables.m_custom_crosshair_gap, float ), -10.f, 10.f, "%.1f" );
						ImGui::SliderFloat( "aspect##custom crosshair", &GET_VARIABLE( g_variables.m_custom_crosshair_aspect, float ), 0.1f, 10.f,
						                    "%.2f" );
						ImGui::SliderFloat( "gap aspect##custom crosshair", &GET_VARIABLE( g_variables.m_custom_crosshair_gap_aspect, float ), 0.1f,
						                    5.f, "%.2f" );
						ImGui::SliderFloat( "rotation##custom crosshair", &GET_VARIABLE( g_variables.m_custom_crosshair_rotation, float ), 0.f, 360.f,
						                    "%.0f deg" );
						ImGui::Checkbox( "taper##custom crosshair", &GET_VARIABLE( g_variables.m_custom_crosshair_taper, bool ) );
						if ( GET_VARIABLE( g_variables.m_custom_crosshair_taper, bool ) ) {
							ImGui::SliderFloat( "near width##custom crosshair", &GET_VARIABLE( g_variables.m_custom_crosshair_taper_near, float ), 0.f,
							                    10.f, "%.2f" );
							ImGui::SliderFloat( "far width##custom crosshair", &GET_VARIABLE( g_variables.m_custom_crosshair_taper_far, float ), 0.f,
							                    10.f, "%.2f" );
						}

						ImGui::ColorEdit4( "color##custom crosshair", &GET_VARIABLE( g_variables.m_custom_crosshair_color, c_color ),
						                   color_picker_alpha_flags );

						ImGui::Checkbox( "gradient##custom crosshair", &GET_VARIABLE( g_variables.m_custom_crosshair_gradient, bool ) );
						if ( GET_VARIABLE( g_variables.m_custom_crosshair_gradient, bool ) ) {
							ImGui::ColorEdit4( "center / outer##custom crosshair gradient",
							                   &GET_VARIABLE( g_variables.m_custom_crosshair_gradient_center, c_color ), color_picker_alpha_flags );
							ImGui::SameLine( );
							ImGui::ColorEdit4( "##custom crosshair gradient outer", &GET_VARIABLE( g_variables.m_custom_crosshair_gradient_outer, c_color ),
							                   color_picker_alpha_flags, 1 );
							ImGui::SliderFloat( "gradient start##custom crosshair", &GET_VARIABLE( g_variables.m_custom_crosshair_gradient_start, float ),
							                    0.f, 1.f, "%.2f" );
							ImGui::SliderFloat( "gradient end##custom crosshair", &GET_VARIABLE( g_variables.m_custom_crosshair_gradient_end, float ), 0.f,
							                    1.f, "%.2f" );
							float& grad_end = GET_VARIABLE( g_variables.m_custom_crosshair_gradient_end, float );
							grad_end        = ImMax( grad_end, GET_VARIABLE( g_variables.m_custom_crosshair_gradient_start, float ) );
						}

						ImGui::TableNextColumn( );

						if ( GET_VARIABLE( g_variables.m_custom_crosshair_dot, bool ) ) {
							ImGui::Checkbox( "round dot##custom crosshair", &GET_VARIABLE( g_variables.m_custom_crosshair_dot_circle, bool ) );
							ImGui::Checkbox( "custom dot color##custom crosshair", &GET_VARIABLE( g_variables.m_custom_crosshair_dot_custom_color, bool ) );
							if ( GET_VARIABLE( g_variables.m_custom_crosshair_dot_custom_color, bool ) ) {
								ImGui::SameLine( );
								ImGui::ColorEdit4( "##custom crosshair dot color", &GET_VARIABLE( g_variables.m_custom_crosshair_dot_color, c_color ),
								                   color_picker_alpha_flags );
							}
						}

						if ( GET_VARIABLE( g_variables.m_custom_crosshair_outline, bool ) ) {
							ImGui::Checkbox( "outline on lines##custom crosshair", &GET_VARIABLE( g_variables.m_custom_crosshair_outline_on_lines, bool ) );
							ImGui::Checkbox( "outline on dot##custom crosshair", &GET_VARIABLE( g_variables.m_custom_crosshair_outline_on_dot, bool ) );
							float& outline_thickness = GET_VARIABLE( g_variables.m_custom_crosshair_outline_thickness, float );
							ImGui::SliderFloat( "outline thickness##custom crosshair", &outline_thickness, 0.1f, 5.f, "%.1f" );
							outline_thickness = roundf( outline_thickness * 10.f ) / 10.f;
							ImGui::ColorEdit4( "outline color##custom crosshair", &GET_VARIABLE( g_variables.m_custom_crosshair_outline_color, c_color ),
							                   color_picker_alpha_flags );
							ImGui::Checkbox( "soft outline (drop shadow)##custom crosshair", &GET_VARIABLE( g_variables.m_custom_crosshair_outline_blur, bool ) );
							if ( GET_VARIABLE( g_variables.m_custom_crosshair_outline_blur, bool ) )
								ImGui::SliderFloat( "softness##custom crosshair", &GET_VARIABLE( g_variables.m_custom_crosshair_outline_blur_radius, float ),
								                    0.5f, 10.f, "%.1f" );
							ImGui::Checkbox( "outline only some sides##custom crosshair",
							                 &GET_VARIABLE( g_variables.m_custom_crosshair_outline_sides_only, bool ) );
							if ( GET_VARIABLE( g_variables.m_custom_crosshair_outline_sides_only, bool ) )
								sides( "custom crosshair outline sides", &GET_VARIABLE( g_variables.m_custom_crosshair_outline_side_left, bool ),
								       &GET_VARIABLE( g_variables.m_custom_crosshair_outline_side_right, bool ),
								       &GET_VARIABLE( g_variables.m_custom_crosshair_outline_side_top, bool ),
								       &GET_VARIABLE( g_variables.m_custom_crosshair_outline_side_bottom, bool ) );
						}

						ImGui::Checkbox( "dynamic spread##custom crosshair", &GET_VARIABLE( g_variables.m_custom_crosshair_dynamic, bool ) );
						if ( GET_VARIABLE( g_variables.m_custom_crosshair_dynamic, bool ) ) {
							float& dyn_min = GET_VARIABLE( g_variables.m_custom_crosshair_dynamic_min, float );
							float& dyn_ref = GET_VARIABLE( g_variables.m_custom_crosshair_dynamic_ref, float );
							ImGui::SliderFloat( "min speed##custom crosshair", &dyn_min, 0.f, 900.f, "%.0f u/s" );
							ImGui::SliderFloat( "max spread##custom crosshair", &GET_VARIABLE( g_variables.m_custom_crosshair_dynamic_max, float ), 0.f,
							                    20.f, "%.1f" );
							ImGui::SliderFloat( "full at##custom crosshair", &dyn_ref, 1.f, 1000.f, "%.0f u/s" );
							if ( dyn_min >= dyn_ref )
								dyn_min = ImMax( 0.f, dyn_ref - 1.f );

							if ( ImGui::Button( s_test_running ? "stop test##custom crosshair" : "test##custom crosshair", ImVec2( -1.f, 15.f ) ) ) {
								s_test_running = !s_test_running;
								s_test_t0      = ImGui::GetTime( );
							}
						} else
							s_test_running = false;

						/* 0 -> "full at" over 3 s, then restart */
						s_test_speed = 0.f;
						if ( s_test_running ) {
							const float dyn_ref = GET_VARIABLE( g_variables.m_custom_crosshair_dynamic_ref, float );
							s_test_speed        = static_cast< float >( ImGui::GetTime( ) - s_test_t0 ) / 3.f * dyn_ref;
							if ( s_test_speed >= dyn_ref ) {
								s_test_speed = 0.f;
								s_test_t0    = ImGui::GetTime( );
							}
						}

						ImGui::Checkbox( "moving preview background##custom crosshair", &s_preview_bg );

						const float side = ImGui::GetContentRegionAvail( ).x;
						if ( side > 4.f ) {
							ImDrawList* const list = ImGui::GetWindowDrawList( );
							const ImVec2 p0        = ImGui::GetCursorScreenPos( );
							const ImVec2 p1( p0.x + side, p0.y + side );
							list->AddRectFilled( p0, p1, IM_COL32( 20, 20, 20, 220 ), 4.f );

							if ( s_preview_bg )
								n_custom_crosshair::draw_preview_scene( list, p0, side );

							n_custom_crosshair::draw_preview( list, p0, p1, s_test_running ? s_test_speed : -1.f );

							if ( s_test_running ) {
								char speed_text[ 32 ];
								snprintf( speed_text, sizeof( speed_text ), "%.0f u/s", s_test_speed );
								const ImVec2 ts = ImGui::CalcTextSize( speed_text );
								list->AddText( ImVec2( p0.x + ( side - ts.x ) * 0.5f, p1.y - ts.y - 4.f ), IM_COL32( 200, 200, 200, 210 ), speed_text );
							}

							ImGui::Dummy( ImVec2( side, side ) );
						}

						ImGui::EndTable( );
					},
					ImVec2( 440.f, -1.f ) );
			}

			ImGui::Checkbox( "aimbot fov circle", &GET_VARIABLE( g_variables.m_aimbot_fov_circle, bool ) );

			if ( GET_VARIABLE( g_variables.m_aimbot_fov_circle, bool ) ) {
				ImGui::OptionPopup(
					"aimbot fov circle settings",
					[ & ]( ) {
						ImGui::ColorEdit4( "color##aimbot fov circle", &GET_VARIABLE( g_variables.m_aimbot_fov_circle_color, c_color ),
						                   color_picker_alpha_flags );

						ImGui::SliderFloat( "thickness##aimbot fov circle",
						                    &GET_VARIABLE( g_variables.m_aimbot_fov_circle_thickness, float ), 0.5f, 5.f, "%.1f" );

						ImGui::Checkbox( "only while aim key held##aimbot fov circle",
						                 &GET_VARIABLE( g_variables.m_aimbot_fov_circle_key_only, bool ) );
					},
					ImVec2( 220.f, -1.f ) );
			}

			ImGui::Checkbox( "custom net graph", &GET_VARIABLE( g_variables.m_net_graph, bool ) );

			if ( GET_VARIABLE( g_variables.m_net_graph, bool ) ) {
				ImGui::OptionPopup(
					"net graph settings",
					[ & ]( ) {
						ImGui::TextDisabled( "shown with net_graph 1" );
						ImGui::TextDisabled( "font: fonts > net graph" );

						ImGui::MultiCombo( "shown##net graph", GET_VARIABLE( g_variables.m_net_graph_items, std::vector< bool > ),
						                   { "fps", "frame var", "ping", "up rate", "loss", "choke", "cmd rate", "tick", "sv time", "sv var", "server type" },
						                   GET_VARIABLE( g_variables.m_net_graph_items, std::vector< bool > ).size( ) );
						ImGui::Checkbox( "lerp##net graph", &GET_VARIABLE( g_variables.m_net_graph_lerp, bool ) );
						ImGui::Checkbox( "in / out rows##net graph", &GET_VARIABLE( g_variables.m_net_graph_in_out, bool ) );

						ImGui::ColorEdit4( "text color##net graph", &GET_VARIABLE( g_variables.m_net_graph_color, c_color ), color_picker_alpha_flags );
						ImGui::Checkbox( "warning colors##net graph", &GET_VARIABLE( g_variables.m_net_graph_warn_colors, bool ) );
						if ( GET_VARIABLE( g_variables.m_net_graph_warn_colors, bool ) ) {
							ImGui::ColorEdit4( "low / mid / high##net graph warn3", &GET_VARIABLE( g_variables.m_net_graph_warn3, c_color ),
							                   color_picker_alpha_flags );
							ImGui::SameLine( );
							ImGui::ColorEdit4( "##net graph warn2", &GET_VARIABLE( g_variables.m_net_graph_warn2, c_color ), color_picker_alpha_flags, 1 );
							ImGui::SameLine( );
							ImGui::ColorEdit4( "##net graph warn1", &GET_VARIABLE( g_variables.m_net_graph_warn1, c_color ), color_picker_alpha_flags, 2 );
						}

						ImGui::Combo( "text style##net graph", &GET_VARIABLE( g_variables.m_net_graph_text_style, int ), "none\0shadow\0outline\0" );
						if ( GET_VARIABLE( g_variables.m_net_graph_text_style, int ) != 0 )
							ImGui::ColorEdit4( "outline color##net graph", &GET_VARIABLE( g_variables.m_net_graph_outline_color, c_color ),
							                   color_picker_alpha_flags );

						ImGui::Checkbox( "background##net graph", &GET_VARIABLE( g_variables.m_net_graph_background, bool ) );
						if ( GET_VARIABLE( g_variables.m_net_graph_background, bool ) ) {
							ImGui::SameLine( );
							ImGui::ColorEdit4( "##net graph background color", &GET_VARIABLE( g_variables.m_net_graph_background_color, c_color ),
							                   color_picker_alpha_flags );
						}

						ImGui::SliderInt( "x##net graph", &GET_VARIABLE( g_variables.m_net_graph_x, int ), -1, g_ctx.m_width,
						                  GET_VARIABLE( g_variables.m_net_graph_x, int ) < 0 ? "stock" : "%d px" );
						ImGui::SliderInt( "y##net graph", &GET_VARIABLE( g_variables.m_net_graph_y, int ), -1, g_ctx.m_height,
						                  GET_VARIABLE( g_variables.m_net_graph_y, int ) < 0 ? "stock" : "%d px" );
					},
					ImVec2( 220.f, -1.f ) );
			}

			safe_checkbox( "force crosshair", &GET_VARIABLE( g_variables.m_force_crosshair, bool ) );

			ImGui::Checkbox( "default crosshair color", &GET_VARIABLE( g_variables.m_panorama_crosshair_color, bool ) );

			if ( GET_VARIABLE( g_variables.m_panorama_crosshair_color, bool ) ) {
				ImGui::SameLine( );
				ImGui::ColorEdit4( "##default crosshair color picker",
				                   &GET_VARIABLE( g_variables.m_panorama_crosshair_color_value, c_color ), color_picker_alpha_flags );
			}

			ImGui::Checkbox( "classic crosshair color", &GET_VARIABLE( g_variables.m_classic_crosshair_color, bool ) );

			if ( GET_VARIABLE( g_variables.m_classic_crosshair_color, bool ) ) {
				ImGui::SameLine( );
				ImGui::ColorEdit4( "##classic crosshair color picker",
				                   &GET_VARIABLE( g_variables.m_classic_crosshair_color_value, c_color ), color_picker_alpha_flags );
			}

			ImGui::Checkbox( "classic crosshair outline color", &GET_VARIABLE( g_variables.m_classic_crosshair_outline_color, bool ) );

			if ( GET_VARIABLE( g_variables.m_classic_crosshair_outline_color, bool ) ) {
				ImGui::SameLine( );
				ImGui::ColorEdit4( "##classic crosshair outline color picker",
				                   &GET_VARIABLE( g_variables.m_classic_crosshair_outline_color_value, c_color ), color_picker_alpha_flags );
			}

			ImGui::Checkbox( "square radar", &GET_VARIABLE( g_variables.m_square_radar, bool ) );

			ImGui::Checkbox( "hud color", &GET_VARIABLE( g_variables.m_panorama_hud_color, bool ) );

			if ( GET_VARIABLE( g_variables.m_panorama_hud_color, bool ) ) {
				ImGui::SameLine( );
				ImGui::ColorEdit4( "##hud color picker", &GET_VARIABLE( g_variables.m_panorama_hud_color_value, c_color ),
				                   color_picker_alpha_flags );
			}

			ImGui::Checkbox( "keep killfeed", &GET_VARIABLE( g_variables.m_keep_killfeed, bool ) );

			ImGui::Checkbox( "scaleform", &GET_VARIABLE( g_variables.m_scaleform, bool ) );

			if ( GET_VARIABLE( g_variables.m_scaleform, bool ) )
				ImGui::Combo( "style##scaleform style", &GET_VARIABLE( g_variables.m_scaleform_style, int ), "moi v2\0classic\0" );

			ImGui::Checkbox( "chud hud", &GET_VARIABLE( g_variables.m_chud_hud, bool ) );

			if ( GET_VARIABLE( g_variables.m_chud_hud, bool ) ) {
				ImGui::OptionPopup(
					"chud hud settings",
					[ & ]( ) {
						ImGui::ColorEdit4( "primary color##chud hud color", &GET_VARIABLE( g_variables.m_chud_hud_color, c_color ),
						                   color_picker_alpha_flags );

						ImGui::ColorEdit4( "secondary color##chud hud secondary color", &GET_VARIABLE( g_variables.m_chud_hud_secondary, c_color ),
						                   color_picker_alpha_flags );

						ImGui::SliderFloat( "hud scale", &GET_VARIABLE( g_variables.m_chud_hud_scale, float ), 50.f, 150.f, "%.0f%%" );

						ImGui::SliderFloat( "hud padding", &GET_VARIABLE( g_variables.m_chud_hud_padding, float ), 0.f, 120.f, "%.0f" );

						ImGui::Checkbox( "background blur", &GET_VARIABLE( g_variables.m_chud_hud_blur, bool ) );
						if ( ImGui::IsItemHovered( ) )
							ImGui::SetTooltip( "lower background alpha to see it" );

						ImGui::Checkbox( "watermark background", &GET_VARIABLE( g_variables.m_chud_hud_watermark_bg, bool ) );

						ImGui::Checkbox( "spectator list", &GET_VARIABLE( g_variables.m_chud_hud_spectators, bool ) );

						if ( GET_VARIABLE( g_variables.m_chud_hud_spectators, bool ) ) {
							ImGui::Checkbox( "spectator avatars##chud",
							                 &GET_VARIABLE( g_variables.m_chud_hud_spectator_avatars, bool ) );

							ImGui::SliderInt( "spectator max width##chud",
							                  &GET_VARIABLE( g_variables.m_chud_hud_spectator_max_width, int ), 60, 600, "%d px" );
						}

						ImGui::ColorEdit4( "background color##chud hud bg color", &GET_VARIABLE( g_variables.m_chud_hud_bg, c_color ),
						                   color_picker_alpha_flags );

						ImGui::Checkbox( "dropshadow", &GET_VARIABLE( g_variables.m_chud_hud_shadow, bool ) );

						if ( GET_VARIABLE( g_variables.m_chud_hud_shadow, bool ) ) {
							ImGui::SliderFloat( "shadow blur", &GET_VARIABLE( g_variables.m_chud_hud_shadow_blur, float ), 0.f, 32.f,
							                    "%.0f" );
							ImGui::SliderFloat( "shadow offset x", &GET_VARIABLE( g_variables.m_chud_hud_shadow_x, float ), -20.f, 20.f,
							                    "%.0f" );
							ImGui::SliderFloat( "shadow offset y", &GET_VARIABLE( g_variables.m_chud_hud_shadow_y, float ), -20.f, 20.f,
							                    "%.0f" );

							ImGui::SliderFloat( "shadow spread (bars)", &GET_VARIABLE( g_variables.m_chud_hud_shadow_spread, float ), 0.f,
							                    20.f, "%.0f" );

							ImGui::ColorEdit4( "shadow color##chud hud shadow color", &GET_VARIABLE( g_variables.m_chud_hud_shadow_color, c_color ),
							                   color_picker_alpha_flags );
						}
					},
					ImVec2( 220.f, -1.f ) );
			}

			ImGui::Checkbox( "minecraft hud", &GET_VARIABLE( g_variables.m_mc_hud, bool ) );

			if ( GET_VARIABLE( g_variables.m_mc_hud, bool ) ) {
				ImGui::OptionPopup(
					"minecraft hud settings",
					[ & ]( ) {
						ImGui::Checkbox( "minecraft crosshair", &GET_VARIABLE( g_variables.m_mc_hud_crosshair, bool ) );

						int& gui_scale = GET_VARIABLE( g_variables.m_mc_hud_gui_scale, int );
						ImGui::SliderInt( "gui scale##mc hud", &gui_scale, 0, 6, gui_scale == 0 ? "auto" : "%d" );
					},
					ImVec2( 220.f, -1.f ) );
			}

			ImGui::Checkbox( "call of duty 4 hud", &GET_VARIABLE( g_variables.m_cod4_hud, bool ) );

			if ( GET_VARIABLE( g_variables.m_cod4_hud, bool ) ) {
				ImGui::OptionPopup(
					"call of duty 4 hud settings",
					[ & ]( ) {
						if ( !ImGui::BeginTable( "##cod4 columns", 2, ImGuiTableFlags_SizingStretchSame ) )
							return;
						ImGui::TableNextColumn( );
						ImGui::Label( "hardpoint key" );
						ImGui::Keybind( "hardpoint key##cod4", &GET_VARIABLE( g_variables.m_cod4_hud_hardpoint_key, key_bind_t ), false );

						ImGui::TableNextColumn( );
						n_cod4_hud::draw_options( );
						ImGui::EndTable( );
					},
					ImVec2( 440.f, -1.f ) );
			}

			ImGui::Checkbox( "urban terror hud", &GET_VARIABLE( g_variables.m_urt_hud, bool ) );

			if ( GET_VARIABLE( g_variables.m_urt_hud, bool ) ) {
				ImGui::OptionPopup(
					"urban terror hud settings",
					[ & ]( ) {
						ImGui::Label( "minimap toggle key" );
						ImGui::Keybind( "minimap toggle key##urt", &GET_VARIABLE( g_variables.m_urt_hud_map_key, key_bind_t ), false );

						n_urt_hud::draw_options( );
					},
					ImVec2( 640.f, -1.f ) );
			}

			ImGui::Checkbox( "web on surface", &GET_VARIABLE( g_variables.m_web, bool ) );
			if ( GET_VARIABLE( g_variables.m_web, bool ) ) {
				ImGui::OptionPopup(
					"web settings",
					[]( ) {
						ImGui::TextWrapped( "%s", Web_Status( ) );

						static char url_buffer[ 512 ] = { };
						static std::string url_seen   = { };
						auto& url_var                 = GET_VARIABLE( g_variables.m_web_url, std::string );

						if ( url_var != url_seen ) {
							strncpy_s( url_buffer, url_var.c_str( ), _TRUNCATE );
							url_seen = url_var;
						}

						const bool url_enter = ImGui::InputTextWithHint( "address##web url", "address or search", url_buffer, sizeof( url_buffer ),
						                                                 ImGuiInputTextFlags_EnterReturnsTrue );

						if ( ImGui::Button( "go##web url", ImVec2( -1.f, 15.f ) ) || url_enter ) {
							url_var  = url_buffer;
							url_seen = url_var;
							Web_Navigate( url_var.c_str( ) );
						}

						auto& resolution = GET_VARIABLE( g_variables.m_web_resolution, int );
						int resolution_index = resolution >= 1080 ? 2 : ( resolution >= 720 ? 1 : 0 );

						if ( ImGui::Combo( "resolution##web", &resolution_index, "480p\0" "720p\0" "1080p\0" ) ) {
							constexpr int heights[ 3 ] = { 480, 720, 1080 };
							resolution                 = heights[ resolution_index ];
						}

						auto& aspect = GET_VARIABLE( g_variables.m_web_aspect, int );
						if ( aspect < 0 || aspect >= k_wsAspectCount )
							aspect = 0;

						static const char* aspect_names[ k_wsAspectCount ] = { };
						if ( !aspect_names[ 0 ] ) {
							for ( int i = 0; i < k_wsAspectCount; ++i )
								aspect_names[ i ] = k_wsAspects[ i ].name;
						}

						ImGui::Combo( "shape##web", &aspect, aspect_names, k_wsAspectCount );

						if ( ImGui::Button( "place at crosshair##web", ImVec2( -1.f, 15.f ) ) )
							Web_PlaceAtCrosshair( );

						if ( ImGui::Button( "remove##web", ImVec2( -1.f, 15.f ) ) )
							Web_Unplace( );

						ImGui::SliderFloat( "size##web", &GET_VARIABLE( g_variables.m_web_scale, float ), 0.1f, 8.f, "%.2fx" );
						ImGui::SliderFloat( "corner rounding##web", &GET_VARIABLE( g_variables.m_web_round, float ), 0.f, 128.f, "%.0f" );
						ImGui::SliderFloat( "opacity##web world", &GET_VARIABLE( g_variables.m_web_world_opacity, float ), 0.f, 1.f, "%.2f" );
						ImGui::Combo( "draw##web", &GET_VARIABLE( g_variables.m_web_depth_mode, int ), "decal\0under hud\0on top\0" );
						ImGui::Combo( "back side##web", &GET_VARIABLE( g_variables.m_web_back_mode, int ), "mirror\0hidden\0readable\0" );

						ImGui::SliderFloat( "volume##web", &GET_VARIABLE( g_variables.m_web_volume, float ), 0.f, 1.f, "%.2f" );
						ImGui::Checkbox( "fade with distance##web", &GET_VARIABLE( g_variables.m_web_volume_distance, bool ) );
						if ( GET_VARIABLE( g_variables.m_web_volume_distance, bool ) ) {
							ImGui::SliderFloat( "full volume within##web", &GET_VARIABLE( g_variables.m_web_volume_near, float ), 0.f, 2048.f, "%.0f" );
							ImGui::SliderFloat( "silent past##web", &GET_VARIABLE( g_variables.m_web_volume_far, float ), 16.f, 4096.f, "%.0f" );
						}

						bool_combo( "playback##web", { { "play when placed", &GET_VARIABLE( g_variables.m_web_auto_play, bool ) },
						                               { "from the beginning", &GET_VARIABLE( g_variables.m_web_play_from_start, bool ) },
						                               { "remove when video ends", &GET_VARIABLE( g_variables.m_web_auto_remove, bool ) } } );

						bool_combo( "controls##web", { { "crosshair is the mouse", &GET_VARIABLE( g_variables.m_web_crosshair_pointer, bool ) },
						                               { "hover only on click", &GET_VARIABLE( g_variables.m_web_click_only, bool ) },
						                               { "don't shoot at the screen", &GET_VARIABLE( g_variables.m_web_block_fire, bool ) },
						                               { "wheel / arrows scroll page", &GET_VARIABLE( g_variables.m_web_scroll_input, bool ) } } );

						ImGui::Label( "place key" );
						ImGui::Keybind( "place key##web", &GET_VARIABLE( g_variables.m_web_place_key, key_bind_t ), false );
						ImGui::Label( "remove key" );
						ImGui::Keybind( "remove key##web", &GET_VARIABLE( g_variables.m_web_remove_key, key_bind_t ), false );
						ImGui::Label( "click key" );
						ImGui::Keybind( "click key##web", &GET_VARIABLE( g_variables.m_web_click_key, key_bind_t ), false );
						ImGui::Label( "right click key" );
						ImGui::Keybind( "right click key##web", &GET_VARIABLE( g_variables.m_web_right_click_key, key_bind_t ), false );

						bool_combo( "browser window##web", { { "show", &GET_VARIABLE( g_variables.m_web_panel, bool ) },
						                                     { "keep with menu closed", &GET_VARIABLE( g_variables.m_web_panel_always, bool ) } } );
						if ( GET_VARIABLE( g_variables.m_web_panel, bool ) )
							ImGui::SliderFloat( "window opacity##web", &GET_VARIABLE( g_variables.m_web_panel_opacity, float ), 0.05f, 1.f, "%.2f" );

						if ( ImGui::Button( "clear cache##web", ImVec2( -1.f, 15.f ) ) )
							Web_ClearCache( );

						if ( ImGui::Button( "clear memory##web", ImVec2( -1.f, 15.f ) ) )
							Web_ClearMemory( );
					},
					ImVec2( 220.f, -1.f ) );
			}

			if ( ImGui::Button( "force hud update", ImVec2( -1.f, 15.f ) ) ) {
				g_scaleform.m_should_force_reload = true;
				g_scaleform.m_should_force_update = true;
				g_chud.m_should_force_update      = true;
			}

#ifdef _DEBUG

			static int ctx_panel = 0;
			static char buffer[ 1024 * 16 ];
			ImGui::InputText( "panorama script text", buffer, IM_ARRAYSIZE( buffer ) );

			ImGui::Combo( "context panel##panel", &ctx_panel, "CSGOHud\0CSGOMainMenu\0" );

			if ( ImGui::Button( "run panorama script", ImVec2( -1.f, 15.f ) ) )
				g_scaleform.m_uiengine->run_script( ctx_panel == 0 ? g_scaleform.m_hud_panel : g_scaleform.m_menu_panel, buffer,
				                                    ctx_panel == 0 ? "panorama/layout/hud/hud.xml" : "panorama/layout/mainmenu.xml", 8, 10,
				                                    false, false );

#endif
		}
		menu_group_end( );

		menu_columns_end( );
		break;
	}
	}
}
