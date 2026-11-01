#pragma once
#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"

#include <mutex>
#include <string>
#include <vector>

/* urban terror 4.3 hud, 1:1 off the decompiled cgame.qvm (CG_Draw2D and everything under it). art, fonts, sounds, weapon
   and team tables come out of the q3ut4 pk3s (tools/urt_hud_gen.py -> urt_data.h, botox-assets urt/), every cg_ hud cvar
   is an option with its stock default. csgo maps onto urt bomb mode: T = red, CT = blue.
   game thread (paint_traverse / events) fills the snapshot + queues, render thread (end_scene) draws.
   stock / chud hud hidden by scaleform hud_removal while on */
namespace n_urt_hud
{
	struct player_t {
		int m_index  = 0;
		int m_team   = 0; // urt: 1 red, 2 blue, 3 spectator, 0 free
		bool m_alive = false;
		bool m_c4    = false;
		bool m_dormant = false;
		int m_kills = 0, m_deaths = 0, m_score = 0;
		float m_pos[ 3 ]{ };
		float m_yaw = 0.f;
		std::string m_name{ };
	};

	struct snapshot_t {
		bool m_in_game   = false;
		bool m_alive     = false; // the drawn player (local, or the one followed in eye)
		bool m_free_spec = false; // spectator with nobody followed
		bool m_following = false;
		int m_me         = 0;     // local index
		int m_subject    = 0;     // drawn player index
		int m_team       = 0;     // local urt team
		int m_subject_team = 0;
		std::string m_follow_name{ };

		int m_health    = 0;
		float m_stamina = 1.f; // 0..1
		int m_ping      = 0;
		float m_origin[ 3 ]{ };
		float m_velocity[ 3 ]{ };
		float m_yaw      = 0.f;
		bool m_ducked    = false;
		bool m_on_ground = true;
		int m_keys       = 0; // sub_32550 bits: 1 jump 2 fwd 4 back 8 left 16 right 32 sprint 64 walk 128 crouch

		// active weapon
		int m_weapon = 0; // urt id
		int m_clip = 0, m_clip_max = 0, m_clips = 0;
		std::string m_mode_name{ };
		float m_pulse  = 0.f; // ps.stats[1] * 3.3e-05 stand-in
		bool m_scoped  = false;
		int m_mode     = 0;
		std::vector< int > m_inventory{ }; // urt ids, slot order
		unsigned int m_weapon_handle = 0;

		// round
		int m_red_score = 0, m_blue_score = 0;
		int m_round = 0, m_max_rounds = 0;
		int m_round_ms = -1;    // -1 = no clock
		int m_warmup_ms = -1;   // warmup countdown, -1 = none
		int m_strat_ms = -1;    // freeze time left
		bool m_bomb_planted = false, m_bomb_seen = false;
		float m_bomb_pos[ 2 ]{ };
		bool m_have_sites = false;
		float m_sites[ 2 ][ 2 ]{ };
		float m_defuse = -1.f;  // local defuse progress 0..1
		bool m_carry_c4 = false, m_in_bomb_zone = false;

		// crosshair target (CG_ScanForCrosshairEntity)
		int m_target = 0;
		int m_target_team = 0, m_target_health = 0;
		bool m_target_c4 = false;
		float m_target_pos[ 3 ]{ };
		float m_eye[ 3 ]{ };
		float m_view[ 3 ]{ }; // pitch yaw roll
		float m_fov = 90.f;

		std::string m_location{ };
		std::string m_map{ };
		float m_world_min[ 3 ]{ }, m_world_max[ 3 ]{ };
		bool m_have_world = false;
		float m_since_received = 0.f;
		std::vector< player_t > m_players{ };
	};

	struct impl_t {
		// game thread
		void on_paint_traverse( );
		void on_event( game_event_t* event );
		void on_chat( const char* text, int client, bool html );
		void on_release( );

		// window thread, true = chat line took the key (csgo's chat never opens)
		bool chat_on_key( unsigned int msg, unsigned int wide_param, long long_param, bool ui_input );

		// render thread
		void on_end_scene( );
		void release_textures( );

		snapshot_t m_snapshot{ };
		std::mutex m_lock{ }; // m_snapshot + the event queues in urt_hud.cpp

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

	// visuals tab popup: every cg_ hud cvar (render thread)
	void draw_options( );

	// window thread: the +maptoggle bind (m key by default in urt, here the visuals popup key). never eats the key
	bool on_key( unsigned int msg, unsigned int vk );

	// game thread (HudZoom paint): urt's scope replaces csgo's
	bool hides_scope( );
}

inline n_urt_hud::impl_t g_urt_hud{ };
