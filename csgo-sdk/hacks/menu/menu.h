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
		void player_list_page( );

		struct faded_list_t {
			ImDrawList* m_list;
			float m_alpha;
		};

		/* confirm popups: live = drawn this frame, ghost = last frame copy drawn while fading out */
		struct popup_fade_t {
			ImGuiID m_window_id;
			float m_alpha;
			bool m_live;
			ImDrawList* m_ghost;
		};

		float fade_draw_lists( ImVector< ImDrawList* >& out, ImVector< faded_list_t >& popups );
		void popup_live( ImGuiID window_id );

		ImVector< popup_fade_t > m_popups;

		bool m_opened = false;

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