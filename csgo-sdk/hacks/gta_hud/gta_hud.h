#pragma once
#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"

#include <mutex>
#include <string>
#include <vector>

/* gta san andreas hud, 1:1 off gta_sa.exe 1.0 US (CHud / CFont / CRadar, gta-reversed for names) adapted to csgo:
   clock = round clock, money / health / armor = csgo values, wanted stars = round kills, zone name = place name,
   wasted on death, mission passed / failed on round win / loss, radar = csgo overview with sa blips.
   art + font widths come out of the sa install (tools/gta_hud_gen.py -> gta_data.h, botox-assets gta/).
   game thread (paint_traverse / events) fills the snapshot, render thread (end_scene) draws.
   csgo chat stays (typing works), the rest of the stock hud is hidden by scaleform hud_removal */
namespace n_gta_hud
{
	struct blip_t {
		float m_pos[ 3 ]{ };
		int m_team   = 0;     // csgo team
		bool m_enemy = false;
	};

	struct snapshot_t {
		bool m_in_game = false;
		bool m_alive   = false; // subject alive
		bool m_wasted  = false; // local dead, nobody followed yet (death / freeze cam)
		int m_team     = 0;     // local csgo team

		int m_health = 0, m_armor = 0, m_money = 0, m_kills = 0;
		int m_def         = 0;  // active weapon item definition index
		bool m_knife      = false;
		int m_clip = 0, m_reserve = 0, m_clip_max = 0;
		bool m_scoped     = false;
		bool m_ducking    = false;
		float m_counter   = 0.f; // m_fAttackButtonCounter stand-in
		int m_clock_ms    = -1;  // round / bomb / warmup clock, -1 = local time
		std::string m_zone{ };

		std::string m_map{ };
		float m_origin[ 3 ]{ };
		float m_yaw = 0.f; // camera heading, degrees
		std::vector< blip_t > m_blips{ };
		bool m_bomb = false, m_bomb_planted = false;
		float m_bomb_pos[ 3 ]{ };
	};

	struct impl_t {
		// game thread
		void on_paint_traverse( );
		void on_event( game_event_t* event );
		void on_release( );

		// render thread
		void on_end_scene( );
		void release_textures( );

		snapshot_t m_snapshot{ };
		std::mutex m_lock{ }; // m_snapshot + the event queue in gta_hud.cpp
	};

	bool enabled( );

	// visuals tab popup (render thread)
	void draw_options( );

	// game thread (HudZoom paint): sa's sniper scope replaces csgo's
	bool hides_scope( );
}

inline n_gta_hud::impl_t g_gta_hud{ };
