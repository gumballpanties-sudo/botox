#pragma once
#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"

#include <cstdint>
#include <mutex>
#include <string>

/* minecraft 1.21.11 survival hud, 1:1 off the decompiled Gui / ChatComponent / ChatScreen (tools/mc_hud_check.py).
   game thread (paint_traverse) snapshots + runs the chat line, render thread (end_scene) draws.
   stock / chud hud hidden by scaleform hud_removal while on, so off = nothing left behind */
namespace n_mc_hud
{
	struct slot_t {
		int m_sprite         = -1; // n_mc_assets::e_sprite, -1 = empty
		int m_count          = 0;
		std::uint32_t m_tint = 0;  // potion liquid rgb
	};

	struct snapshot_t {
		bool m_in_game = false;
		bool m_alive   = false;
		bool m_scoped  = false;
		int m_health   = 0; // MC 0..20
		int m_armor    = 0; // MC 0..20
		int m_money    = 0;
		int m_selected = 0;
		unsigned int m_weapon_handle = 0;
		slot_t m_slots[ 9 ]{ };
		slot_t m_offhand{ };
		std::string m_item_name{ };
	};

	struct impl_t {
		// game thread
		void on_paint_traverse( );
		void on_player_death( int attacker_index, int victim_index, const char* weapon );
		void on_chat( const std::string& html, bool replace_last = false );
		void on_notice( const char* text, int client, bool formatted );
		void on_release( );

		// window thread, true = chat line took the key
		bool chat_on_key( unsigned int msg, unsigned int wide_param, long long_param, bool ui_input );

		// render thread
		void on_end_scene( );
		void release_textures( );

		snapshot_t m_snapshot{ };
		std::mutex m_lock{ }; // m_snapshot + chat history (mc_hud.cpp)

		bool m_was_enabled      = false;
		bool m_crosshair_hidden = false;
		int m_saved_crosshair   = 1;

		// chat line, chud's contract: wndproc writes, paint_traverse drains + sends
		std::mutex m_chat_mutex{ };
		bool m_chat_open                   = false;
		bool m_chat_team                   = false;
		int m_chat_result                  = 0; // 1 send, 2 cancel
		wchar_t m_chat_open_char           = 0;
		unsigned long long m_chat_open_ms  = 0ull;
		unsigned long long m_chat_drain_ms = 0ull;
		std::wstring m_chat_input{ };

		void drain_chat( bool active );
	};

	bool enabled( );
}

inline n_mc_hud::impl_t g_mc_hud{ };
