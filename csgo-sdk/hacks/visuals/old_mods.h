#pragma once

#include <atomic>
#include <string>
#include <vector>

namespace n_old_mods
{
	inline constexpr int k_max_packs = 5;

	enum e_state : int { state_idle, state_downloading, state_ready, state_failed };

	struct pack_state_t {
		std::atomic< int > m_state{ state_idle };
		std::atomic< unsigned long long > m_bytes_done{ 0 };
		std::atomic< int > m_missing{ 0 };
		bool m_mounted = false; // game thread only
	};

	struct impl_t {
		// game thread only, fsn start: model flushes free studio data, every studiohdr reader must be on this thread too
		void on_frame( );

		static int count( );
		static const char* label( int pack );
		static bool& toggle( int pack );
		float progress( int pack ) const;

		// game thread: esp preview root model + its standing sequence while the old player pack is mounted, nullptr = stock setup
		const char* preview_model( int& sequence ) const;

		// game thread: old player pack mounted ( skins: gloves changer off, arms fallback picks sleeves = the old hands )
		bool players_mounted( ) const;

		pack_state_t m_packs[ k_max_packs ]{ };
		std::atomic< bool > m_any_mounted{ false }; // sv_pure.cpp: pure mode 0 rejects every loose pack file
		std::atomic< bool > m_serial{ false };      // misc performance(): leave mat_queue_mode alone while set

	private:
		void serial_step( );
		void fix_arms( );
		void hide_gloves( );

		static constexpr int k_untouched = -99;
		int m_serial_phase = 0; // 1 queue draining, 2 reloaded, restoring
		int m_serial_frame = 0;
		int m_queue_backup = k_untouched;
		std::string m_serial_cmd{ };
		std::vector< std::string > m_serial_particles{ }, m_serial_images{ }, m_serial_models{ };
		std::vector< int > m_serial_remove{ };
		std::string m_arms_backup{ }, m_arms_pick{ }; // local m_szArmsModel before / after fix_arms wrote it
		std::vector< unsigned int > m_hidden{ };      // entity handles hide_gloves set EF_NODRAW on
	};
}

inline n_old_mods::impl_t g_old_mods{ };
