#pragma once
#include "menu.h"
#include "../../globals/fonts/fonts.h"
#include "../../globals/includes/includes.h"
#include "../../globals/logger/logger.h"
#include "../misc/scaleform/scaleform.h"
#include "../chams/chams.h"
#include "../chud_hud/chud_hud.h"
#include "../movement/movement.h"
#include "../skins/skins.h"
#include "../visuals/screen/resolution_spoof.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <format>
#include <functional>
#include <string>
#include <vector>

constexpr int color_picker_alpha_flags = ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf | ImGuiColorEditFlags_NoOptions |
                                         ImGuiColorEditFlags_InputRGB | ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoTooltip |
                                         ImGuiColorEditFlags_NoDragDrop | ImGuiColorEditFlags_PickerHueBar | ImGuiColorEditFlags_NoBorder;

constexpr int color_picker_no_alpha_flags = ImGuiColorEditFlags_NoOptions | ImGuiColorEditFlags_InputRGB | ImGuiColorEditFlags_NoInputs |
                                            ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop | ImGuiColorEditFlags_PickerHueBar |
                                            ImGuiColorEditFlags_NoBorder;

constexpr float menu_band_height        = 25.f;
constexpr float menu_bottom_band_height = 12.f;

void modulation_row( const char* label, const std::uint32_t enable_variable, const std::uint32_t color_variable,
                     const std::uint32_t brightness_variable );
void draw_color_correction_curves( );
void reset_color_correction( );
void chams_dropdown( const char* label, const std::uint32_t layers_variable, const std::uint32_t colors_variable, const std::uint32_t dark_colors_variable );
void save_popup( const char* str_id, bool& open, const ImVec2& window_size, const std::function< void( ) >& fn );
/* render thread, every frame: lands a finished image box pick even when its box is hidden */
void image_pick_apply( );

inline void text_buffer_sync( const char* widget_id, std::string& var, char* buffer, const std::size_t buffer_size, const char* hint = nullptr )
{
	const bool changed = hint ? ImGui::InputTextWithHint( widget_id, hint, buffer, buffer_size ) : ImGui::InputText( widget_id, buffer, buffer_size );
	if ( changed )
		var = buffer;
	else if ( !ImGui::IsItemActive( ) && var != buffer ) {
		const std::size_t len = var.size( ) < buffer_size - 1 ? var.size( ) : buffer_size - 1;
		std::memcpy( buffer, var.c_str( ), len );
		buffer[ len ] = '\0';
	}
}

/* safe mode: greyed + shown off, feature code skips it too */
inline void safe_checkbox( const char* label, bool* v )
{
	const bool safe = GET_VARIABLE( g_variables.m_safe_mode, bool );
	bool off        = false;

	ImGui::BeginDisabled( safe );
	ImGui::Checkbox( label, safe ? &off : v );
	ImGui::EndDisabled( );

	if ( safe && ImGui::IsItemHovered( ImGuiHoveredFlags_AllowWhenDisabled ) )
		ImGui::SetTooltip( "safe mode enabled" );
}

struct menu_style_t {
	const char* m_name;
	ImU32 m_window, m_band, m_child, m_frame, m_outline, m_window_outline, m_popup, m_text, m_text_dim;
	float m_window_rounding, m_frame_rounding, m_popup_rounding;
	int m_font, m_font_bold;
	c_color m_accent;
};

extern const menu_style_t k_menu_styles[ ];
extern const int k_menu_style_count;

/* page = two scrolling columns of groups; group = header child sized to last frame's content */
void menu_columns_begin( );
void menu_columns_next( );
void menu_columns_end( );
bool menu_group_begin( const char* name );
void menu_group_end( );
