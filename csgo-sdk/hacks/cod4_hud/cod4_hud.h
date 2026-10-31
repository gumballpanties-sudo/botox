#pragma once
#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"

#include <array>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

/* call of duty 4 (1.7) multiplayer hud, 1:1: the stock ui_mp/hud.txt menus copied item for item (cod4_data.h,
   tools/cod4_hud_gen.py), their menu expressions evaluated live (cod4_expr.h), the ownerdraws ported from the
   decompiled iw3mp.exe (cod4_logic.h + this file). art and fonts come out of the user's cod4 install at runtime.
   game thread (paint_traverse / events) fills snapshot + queues, render thread (end_scene) draws.
   stock / chud hud hidden by scaleform hud_removal while on */
namespace n_cod4_hud
{
	struct actor_t {
		int m_index        = 0;
		bool m_enemy       = false;
		float m_pos[ 2 ]{ };
		float m_yaw        = 0.f;
	};

	struct snapshot_t {
		bool m_in_game = false;
		bool m_alive   = false;
		bool m_scoped  = false;
		bool m_sniper  = false;
		bool m_flashed = false;
		bool m_console = false;
		int m_team     = 0; // 2 T = axis, 3 CT = allies
		int m_health = 0, m_max_health = 100, m_armor = 0;
		float m_origin[ 3 ]{ };
		float m_yaw = 0.f, m_tan_half_fov_y = 1.f;

		// active weapon
		int m_weapon      = -1; // k_weapons index
		unsigned int m_weapon_handle = 0;
		bool m_knife      = false;
		int m_clip = 0, m_clip_max = 0, m_reserve = 0, m_reserve_max = 0;
		float m_spread_scale = 0.f; // aimSpreadScale 0..255, PM_UpdateAimSpread run per frame on the game thread
		float m_spread_min = 0.f, m_spread_max = 0.f; // hipSpread min / max, degrees
		bool m_ducked     = false;
		std::string m_weapon_name{ };

		// offhand slots: frag = he / molotov, smoke = flash / smoke / decoy
		int m_frag_weapon = -1, m_frag_count = 0;
		int m_smoke_weapon = -1, m_smoke_count = 0;
		bool m_has_c4 = false;
		std::uint32_t m_frag_glyph = 0, m_smoke_glyph = 0, m_c4_glyph = 0; // csgo weapon icon font codepoints

		// score
		int m_team_score = 0, m_enemy_score = 0, m_ct_score = 0, m_t_score = 0;
		int m_player_score = 0, m_score_rank1 = 0, m_score_rank2 = 0;
		int m_score_limit = 0;

		bool m_warmup    = false;
		float m_timeleft = -1.f; // round seconds left (frozen at the round time in freeze time), -1 = no game rules

		std::vector< actor_t > m_actors{ };
		std::string m_map{ };
	};

	struct impl_t {
		// game thread
		void on_paint_traverse( );
		void on_event( game_event_t* event );
		void on_chat( const char* text, int client, bool html );
		void on_release( );

		// render thread
		void on_end_scene( );
		void release_textures( );

		snapshot_t m_snapshot{ };
		std::mutex m_lock{ }; // m_snapshot + the event state in cod4_hud.cpp

		// window thread, true = chat line took the key (csgo's chat never opens)
		bool chat_on_key( unsigned int msg, unsigned int wide_param, long long_param, bool ui_input );

		// chat line, mc_hud's contract: wndproc writes, paint_traverse drains + sends, end_scene draws
		std::mutex m_chat_mutex{ };
		bool m_chat_open                   = false;
		bool m_chat_team                   = false;
		int m_chat_result                  = 0; // 1 send, 2 cancel
		wchar_t m_chat_open_char           = 0;
		unsigned long long m_chat_drain_ms = 0ull;
		std::wstring m_chat_input{ };

		void drain_chat( bool active );
	};

	bool enabled( );

	// visuals tab popup: the hud switches cod4's own menus have (render thread)
	void draw_options( );

	// window thread: the hardpoint key. never eats the key
	bool on_key( unsigned int msg, unsigned int vk );

	// game thread (HudZoom paint): cod4's ads overlay replaces csgo's scope
	bool hides_scope( );
}

inline n_cod4_hud::impl_t g_cod4_hud{ };
