#pragma once
#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>


namespace n_chud
{
	struct kill_t {
		std::string m_attacker{ }, m_victim{ }, m_weapon{ };
		bool m_headshot{ }, m_local_involved{ };
		bool m_local_killer{ };
	};

	struct impl_t {
		void on_createmove( );
		void on_level_init( );

		// wndproc, every key msg. true = chat line took it, caller must swallow it
		bool chat_on_key( unsigned int msg, unsigned int wide_param, long long_param, bool ui_input );

		bool chat_reset( );

		void on_player_death( const std::string& attacker, const std::string& victim, const std::string& weapon, bool headshot,
		                      bool local_involved, bool local_killer );
		void on_chat( const std::string& text, bool stack = false, bool replace = false );

		bool resolve_hud( );
		void install( );
		void uninstall( );
		void tick( );
		void push_colors( );
		void hide_stock( );
		void register_system_fonts( );
		void restore_fonts_conf( );    // put valve's fonts.conf back, a bad one crashes the game

		void register_def_hud_font( );
		void clear_def_hud_font( );

		const std::vector< std::string >& system_fonts( );
		void expire_rows( );
		void run( const std::string& js );

		c_ui_engine* m_uiengine = nullptr;
		c_uipanel* m_hud_panel  = nullptr;
		bool m_inited           = false;

		bool m_built = false;

		// menu "force hud update" (render thread), consumed on the game thread
		bool m_should_force_update = false;

		// change detection, never push js unless the value actually moved
		int m_old_health   = -1;
		int m_old_armor    = -1;
		int m_old_clip     = -1;
		int m_old_reserve  = -1;
		int m_old_max_clip = -1;

		/* layout changes rebuild the tree, debounced (slider drag = rebuild per frame).
		   colour changes never rebuild, pushed immediately. */
		int m_old_layout          = -1;
		int m_pending_layout      = -1;
		float m_style_settle_time = 0.f;
		unsigned int m_old_colors = 0u;
		std::string m_old_weapons{ };
		int m_old_active_row = -1;
		std::string m_old_timer{ };
		std::string m_old_spectators{ };
		int m_old_killfeed_top                    = -1;
		unsigned long long m_next_killfeed_push = 0ull;

		// queues drained on the game thread
		std::deque< kill_t > m_pending_kills{ };
		struct chat_t {
			std::string m_text{ };
			bool m_stack{ }, m_replace{ };
		};
		std::deque< chat_t > m_pending_chat{ };

		float m_next_hide_pass = 0.f;
		float m_next_nuke_pass = 0.f;

		// row expiry driven from c++: $.Schedule never fired for killfeed rows
		struct row_t {
			int m_id{ };
			float m_death_time{ };
			bool m_keep{ };
		};

		int m_next_row_id = 0;
		std::deque< row_t > m_kill_rows{ };
		std::deque< row_t > m_chat_rows{ };
		int m_stack_row = -1;

		bool m_chat_open         = false;
		bool m_chat_team         = false;
		bool m_chat_input_dirty  = false;
		int  m_chat_result       = 0;
		/* the opening key still sends its own WM_CHAR: drop it, but only if it matches,
		   pumps that don't translate would lose the message's first letter. */
		wchar_t m_chat_open_char = 0;
		/* tick( ) stamps this (GetTickCount64). stale = createmove stopped (demo, killer replay):
		   nothing drains the line, so the key side never opens / drops it instead of eating keys */
		unsigned long long m_chat_drain_ms = 0ull;
		bool m_chat_stale_close            = false; // dropped while stale: tick( ) owes k_close_chat
		std::wstring m_chat_input{ };
		std::mutex m_chat_mutex{ };

		int m_old_team = -1;

		std::vector< std::string > m_system_fonts{ };

		std::vector< std::string > m_panorama_fonts{
			"Stratum2",         "Stratum2 Bold",    "Stratum2 Black",  "Stratum2 Light",     "Stratum2 Medium",
			"Stratum2 Regular", "Stratum2 Thin",    "Stratum2 Mono",   "Stratum2 Condensed", "Stratum2 Bold Monodigit",
			"Stratum2 Regular Monodigit",           "Noto Sans",       "Noto Sans Bold",     "Noto Sans SC",
			"Noto Sans TC",     "Noto Sans JP",     "Noto Sans KR",    "Noto Sans Thai",     "Noto Serif",
			"Noto Mono",        "Symbols"
		};
	};
}

inline n_chud::impl_t g_chud{ };
