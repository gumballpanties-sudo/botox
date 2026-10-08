#include "../movement/movement_internal.h"
#include "../../dependencies/imgui/imgui.h"
#include <algorithm>
#include <cmath>
#include <string>

static float s_hitmarker_timer = 0.0f;
static bool s_show_hitmarker  = false;
void on_hit_marker( )
{
	if ( !GET_VARIABLE( g_variables.m_hit_marker, bool ) )
		return;

	s_show_hitmarker  = true;
	s_hitmarker_timer = 0.5f / std::max( GET_VARIABLE( g_variables.m_hit_marker_speed, float ), 0.01f );
}
void on_hit_sound( )
{
	if ( !GET_VARIABLE( g_variables.m_hit_sound, bool ) )
		return;

	play_trick_sound( GET_VARIABLE( g_variables.m_hit_sound_type, int ) + 1, GET_VARIABLE( g_variables.m_hit_sound_volume, float ),
	                  GET_VARIABLE( g_variables.m_hit_sound_custom, std::string ).c_str( ) );
}
void RenderHitmarker( )
{
	if ( !GET_VARIABLE( g_variables.m_hit_marker, bool ) )
		return;
	const float speed              = std::max( GET_VARIABLE( g_variables.m_hit_marker_speed, float ), 0.01f );
	const float hitmarkerDuration  = 0.5f / speed;
	const float extensionTime      = 0.1f / speed;
	const float full_length        = GET_VARIABLE( g_variables.m_hit_marker_size, float );
	const float move_distance      = 5.0f;
	const float angle              = 45.0f;
	const float thickness          = GET_VARIABLE( g_variables.m_hit_marker_thickness, float );
	const bool  draw_outline       = GET_VARIABLE( g_variables.m_hit_marker_outline, bool );
	const float outline_thickness  = thickness + GET_VARIABLE( g_variables.m_hit_marker_outline_thickness, float ) * 2.f;
	if ( s_show_hitmarker ) {
		ImDrawList* drawList = ImGui::GetForegroundDrawList( );
		float elapsed_time   = hitmarkerDuration - s_hitmarker_timer;
		float current_length = 0.0f;
		float alpha          = 0.0f;
		float offset         = 0.0f;
		if ( elapsed_time < extensionTime ) {
			float progress = extensionTime > 0.f ? elapsed_time / extensionTime : 1.f;
			current_length = progress * full_length;
			alpha          = 1.0f;
		} else {
			current_length      = full_length;
			float fade_progress = ( elapsed_time - extensionTime ) / ( hitmarkerDuration - extensionTime );
			alpha               = 1.0f - fade_progress;
			offset              = fade_progress * move_distance;
		}
		const ImU32 color         = GET_VARIABLE( g_variables.m_hit_marker_color, c_color ).get_u32( alpha );
		const ImU32 outline_color = GET_VARIABLE( g_variables.m_hit_marker_outline_color, c_color ).get_u32( alpha );
		float centerX   = g_ctx.m_width / 2.0f;
		float centerY   = g_ctx.m_height / 2.0f;
		float rad_angle = angle * 3.1415926535f / 180.0f;
		float dx        = cos( rad_angle );
		float dy        = sin( rad_angle );

		const ImVec2 line_points[ 4 ][ 2 ] = {
			{ ImVec2( centerX + offset * dx, centerY + offset * dy ), ImVec2( centerX + offset * dx + current_length * dx, centerY + offset * dy + current_length * dy ) },
			{ ImVec2( centerX - offset * dx, centerY - offset * dy ), ImVec2( centerX - offset * dx - current_length * dx, centerY - offset * dy - current_length * dy ) },
			{ ImVec2( centerX + offset * dy, centerY - offset * dx ), ImVec2( centerX + offset * dy + current_length * dy, centerY - offset * dx - current_length * dx ) },
			{ ImVec2( centerX - offset * dy, centerY + offset * dx ), ImVec2( centerX - offset * dy - current_length * dy, centerY + offset * dx + current_length * dx ) },
		};

		const auto block = g_render.begin_stretch_block( drawList, false );

		if ( draw_outline )
			for ( const auto& line : line_points )
				drawList->AddLine( line[ 0 ], line[ 1 ], outline_color, outline_thickness );

		for ( const auto& line : line_points )
			drawList->AddLine( line[ 0 ], line[ 1 ], color, thickness );

		g_render.end_stretch_block( block );

		s_hitmarker_timer -= ImGui::GetIO( ).DeltaTime;
		if ( s_hitmarker_timer <= 0.0f ) {
			s_show_hitmarker = false;
		}
	}
}
