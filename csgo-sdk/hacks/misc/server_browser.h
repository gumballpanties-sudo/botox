#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

/* misc > servers: steam master server list for both csgo app ids + steam.inf version; the menu hides vac servers while
   the client can't join them. every steam call on the main thread ( frame start ), menu only reads snapshots */
namespace n_server_browser
{
	struct server_t {
		std::string m_name, m_map, m_address; /* address = ip:connection port */
		std::uint32_t m_app_id = 0;             /* the app the server registered under */
		int m_players = 0, m_max_players = 0, m_bots = 0, m_ping = 0;
		bool m_password = false, m_secure = false, m_cybershoke = false;
	};

	struct status_t {
		std::string m_version; /* steam.inf PatchVersion, "" = not found -> no version filter */
		std::uint32_t m_app_id = 0; /* the running game's */
		bool m_secure_allowed  = true; /* live every frame: insecure bypass toggle re-filters without a new query */
		bool m_refreshing      = true; /* menu asks on first open, main thread starts it next frame */
		bool m_no_steam        = false;
	};

	struct impl_t {
		void on_frame( );
		void on_eject( );

		void refresh( ) { m_want_refresh = true; }
		void join( const std::string& address );

		/* copies only when the list changed since `generation`; true = out / status written */
		bool snapshot( std::vector< server_t >& out, status_t& status, unsigned& generation );

		/* steam callbacks ( file-local response object ) */
		void add( const server_t& server );
		void finished( );

	private:
		void start( );
		void release( );

		std::atomic< bool > m_want_refresh{ false };
		std::atomic< bool > m_finished{ false };
		std::vector< void* > m_requests; /* HServerListRequest, one per app id, main thread only */

		std::mutex m_mutex;
		std::vector< server_t > m_servers;
		status_t m_status;
		int m_pending = 0; /* requests without RefreshComplete yet */
		unsigned m_generation = 1;
		std::string m_join;
	};
} // namespace n_server_browser

inline n_server_browser::impl_t g_server_browser{ };
