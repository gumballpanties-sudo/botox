#include "c_color.h"
#include "../../../dependencies/imgui/imgui.h"

[[nodiscard]] unsigned int c_color::get_u32( const float alpha_multiplier ) const
{
	// game thread can ask before the render thread made the imgui context ( first create_move after inject = null GImGui crash )
	if ( !ImGui::GetCurrentContext( ) )
		return ImGui::ColorConvertFloat4ToU32( this->get_vec4( alpha_multiplier ) );
	return ImGui::GetColorU32( this->get_vec4( alpha_multiplier ) );
}

[[nodiscard]] ImVec4 c_color::get_vec4( const float alpha_multiplier ) const
{
	return ImVec4( this->base< e_color_type::color_type_r >( ), this->base< e_color_type::color_type_g >( ),
	               this->base< e_color_type::color_type_b >( ), this->base< e_color_type::color_type_a >( ) * alpha_multiplier );
}