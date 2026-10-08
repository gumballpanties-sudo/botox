#pragma once

#include "../../dependencies/imgui/imgui.h"

namespace n_menu
{
	struct impl_t {
		void on_end_scene( );

		void tab_visuals( );
		void tab_movement( );
		void tab_aimbot( );
		void tab_misc( );
		void tab_inventory( );
		void tab_fonts( );
		void tab_settings( );
		void route_calc_pinned( bool no_inputs );
		void player_list_window( bool no_inputs );

		float fade_draw_lists( ImVector< ImDrawList* >& out );

		bool m_opened             = false;
		bool m_player_list_opened = false;
		int m_route_calc_side     = 0;

		int m_tab    = 0;
		int m_subtab = 0;

		float m_anim_progress = 0.f;

		int m_selected_config    = 0;
		char m_config_file[ 32 ] = { };

		int m_selected_visual_config    = 0;
		char m_visual_config_file[ 32 ] = { };
	};
}

inline n_menu::impl_t g_menu{ };