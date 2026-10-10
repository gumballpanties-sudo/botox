#pragma once

#pragma comment( lib, "windowsapp" )

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/base.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <d3d9.h>

using namespace winrt;
using namespace Windows::Graphics::Imaging;
using namespace Windows::Media::Control;
using namespace Windows::Storage::Streams;
using namespace Windows::Foundation::Collections;
using namespace winrt::Windows::Media::Control;

namespace n_media_player
{
	struct impl_t {
		/* background thread only: winrt calls block for ms, never on the render thread */
		bool init( );
		void on_update( );
		void shutdown( ); // eject: drop the proxy on its own MTA thread while the process is healthy

		/* game quit: a static dtor releasing the out-of-proc proxy does RPC after the thread pool is gone
		   (ntdll+0x10ff85 c000000d on every exit). leak the ref, the process is ending anyway */
		~impl_t( )
		{
			if ( this->m_session_manager.has_value( ) )
				winrt::detach_abi( *this->m_session_manager );
		}

		/* render thread only: D3DCREATE_MULTITHREADED depends on mat_queue_mode, so touch the device from end_scene */
		void update_texture( IDirect3DDevice9* device );
		void on_release( );

		std::string get_title( );
		std::string get_artist( );
		std::wstring get_source_app( );

		float get_progress( );
		double get_position_ms( );
		long long get_total_ms( );

		enum e_command : int { command_none, command_previous, command_toggle, command_next };
		// any thread: queued, the media thread runs it on the shown source
		void request( const e_command command ) { this->m_command = command; }
		// media thread: true = ran one
		bool run_command( );

		/* synced lyrics from lrclib, fetched on the lyrics thread when the track changes. empty = none / not found */
		struct lyric_line_t {
			double m_start_ms;
			std::string m_text;
		};
		void update_lyrics( bool wanted );
		// render thread: copies only when the generation moved. returns generation
		int get_lyrics( std::vector< lyric_line_t >& out, int have_generation );

		/* visualizer: own thread. WASAPI process loopback of the media session's app tree ( browser / spotify ) only.
		   app not found = every app but csgo. no session = silent. open only while the option is on */
		void visualizer_thread( const std::atomic< bool >& stop, bool ( *wanted )( ) );
		// render thread: k_bands levels 0..1, decayed toward 0 when nothing plays
		void get_bands( float* out );

		bool m_has_media{ };
		bool m_is_playing{ };
		IDirect3DTexture9* m_image{ };

	private:
		std::mutex m_bands_mutex{ };
		float m_bands[ 64 ]{ };
		std::mutex m_mutex{ };
		std::string m_title{ }, m_artist{ };
		std::wstring m_source_app{ }; // session's AppUserModelId ( no session: audible app's exe path ), empty = nothing
		enum e_source : int { source_none, source_session, source_kopuz, source_app } m_source_kind{ };
		std::atomic< int > m_command{ };
		// media thread only. an audible-only app ( no SMTC, e.g. opera ) goes silent once paused, keep it shown as paused
		bool m_app_paused{ };
		std::chrono::steady_clock::time_point m_app_paused_at{ };
		std::vector< unsigned char > m_thumbnail{ };
		bool m_thumbnail_dirty{ };
		std::wstring m_thumbnail_key{ }; // media thread only
		int m_thumbnail_wait{ };
		long long m_total_ms{ }, m_current_ms{ };
		double m_rate{ 1.0 };
		std::chrono::steady_clock::time_point m_position_stamp{ };

		std::mutex m_lyrics_mutex{ };
		std::vector< lyric_line_t > m_lyrics{ };
		std::string m_lyrics_key{ }; // media thread only
		int m_lyrics_generation{ };

		std::optional< GlobalSystemMediaTransportControlsSessionManager > m_session_manager{ };
	};
}

inline n_media_player::impl_t g_media_player{ };
