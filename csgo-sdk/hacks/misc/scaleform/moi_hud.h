#pragma once

#include <atomic>
#include <string>
#include <unordered_map>
#include <unordered_set>

class c_uipanel;

namespace n_moi_hud
{
	struct impl_t {
		void update( c_uipanel* hud_root, bool wanted );

		void on_level_init( );

		bool wants_root( ) const { return m_serving || m_js_off_pending; }

		bool serving( ) const { return m_serving; }

		void reload( c_uipanel* hud_root ) { reload_all( hud_root ); }

		void on_release( );

		/* load_file_into_buffer detour: served copy (game-relative path, loose file) for a stock panorama path,
		   nullptr = stock. panorama main thread */
		const char* redirect( const char* path );

		void redirect_failed( const char* path, const char* served );

		/* set_image_data detour, any decode thread: moi svg for icons/equipment/<name>.vsvg, nullptr = stock */
		const std::string* select_icon( const char* name );

		/* panorama caches a decoded svg per url for good: after a style switch the old style's weapon icons stay.
		   re-decodes every equipment icon a style swaps (classic's set is inside this one), game thread */
		void reload_icons( );

		const char* killfeed_format( const char* stock_format, const char* weapon );

		std::atomic< bool > m_shutdown{ false };
		std::atomic< int > m_workers{ 0 };

		std::atomic< int > m_done{ 0 };
		std::atomic< int > m_failed{ 0 };
		int m_total = 0;

	private:
		void start_downloads( );
		bool build_served( );
		void reload_all( c_uipanel* hud_root );
		void apply_js( c_uipanel* hud_root, bool on );
		void push_cvars( c_uipanel* hud_root, bool force );
		bool patch_killfeed( bool on );

		bool m_started = false;
		std::atomic< bool > m_serving{ false }; // redirect( ) live + hud reloaded with moi files; decode threads read it
		bool m_js_applied = false;
		bool m_js_off_pending = false;
		int m_checked_at = -1;

		float m_old_alpha = -1.f;
		int m_old_simple  = -1;

		std::unordered_map< std::string, std::string > m_served;

		// killfeed pngs on disk, by weapon name. rebuilt with m_served (game thread, where the call site runs)
		std::unordered_set< std::string > m_killfeed;

		/* weapon select svgs by name: read on decode threads, so built ONCE (all downloads done) and published
		   through an atomic pointer, never mutated after */
		std::atomic< const std::unordered_map< std::string, std::string >* > m_select{ nullptr };
	};
}

inline n_moi_hud::impl_t g_moi_hud{ };
