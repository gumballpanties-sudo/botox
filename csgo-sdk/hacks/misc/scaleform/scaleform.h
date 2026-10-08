#pragma once
#include "../../../game/sdk/includes/includes.h"
#include "../../../globals/includes/includes.h"

#include <cstdint>
#include <string>

template< auto V >
struct ct_data {
	constexpr static auto value = V;
};

namespace n_scaleform
{
	struct impl_t {
		char*( CDECL* compare_extension )( const char* lhs, const char* rhs );

		void on_level_init( );
		void on_createmove( );
		bool get_replacement_icon( const char* name, const uint8_t*& data, size_t& len, int& w, int& h );

		void modify_all( );
		void tick( );
		void classic_off( );
		void restore_showloadout( );
		void on_release( ) { restore_showloadout( ); }
		void dump_panel_tree( );

		void hud_removal( );

		void hud_aspect( );

		// crosshair half, styles 0 / 1 - panorama, the ISurface rect hooks never see them
		void crosshair_aspect( );

		void force_crosshair_panorama( );

		void crosshair_color( );
		void hud_color( );

		void ammo_pip_color( );

		void weapon_icon_color( );

		void keep_killfeed( );

		void square_radar( );

		// per-pass CSGOHud roots, never m_hud_panel (scaleform's): passes must not switch each other off
		c_uipanel* m_removal_root = nullptr;
		c_uipanel* m_aspect_root = nullptr;
		c_uipanel* m_xhair_root  = nullptr;
		c_uipanel* m_force_xhair_root = nullptr;
		c_uipanel* m_xhair_color_root = nullptr;
		c_uipanel* m_hud_color_root   = nullptr;
		c_uipanel* m_pip_color_root   = nullptr;
		c_uipanel* m_killfeed_root    = nullptr;
		c_uipanel* m_moi_root         = nullptr;
		c_uipanel* m_radar_root       = nullptr;

		std::string m_icon_wash{ };

		c_ui_engine* m_uiengine   = nullptr;
		c_uipanel* m_hud_panel    = nullptr;
		c_uipanel* m_menu_panel   = nullptr;
		c_uipanel* m_weap_sel     = nullptr;
		c_uipanel* m_weap_pan_bg  = nullptr;

		bool m_inited = false;
		bool m_classic_applied = false;
		int m_saved_showloadout = -1;
		int m_icon_src = 0;

		int m_old_color            = -1;
		float m_old_alpha          = -1.f;
		int m_old_healthammo_style = -1;
		int m_old_in_buyzone       = -1;
		short m_old_weapon_def     = -1;

		bool m_pending_mvp = false;
		int m_winpanel_team = 0;

		bool m_should_force_update            = false;
		bool m_should_force_reload            = false; // menu "force hud update" (render thread): undo / reload, then reinstall
		bool m_should_update_teamcount_avatar = false;
		bool m_should_update_winpanel         = false;
		bool m_should_update_deathnotices     = false;
		bool m_killfeed_round_reset           = false;

		// set by menu (render thread), consumed in on_createmove: panorama is game thread only
		bool m_should_dump_panels = false;
	};
}

inline n_scaleform::impl_t g_scaleform{ };
