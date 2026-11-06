#pragma once

#include <atomic>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "../../utilities/input/key_bind_t.h"

struct lua_State;
struct game_event_t;
class c_user_cmd;

namespace n_lua
{
	enum e_element_type : int {
		element_checkbox = 0,
		element_slider_int,
		element_slider_float,
		element_combo,
		element_color,
		element_keybind,
		element_button,
		element_text,
		element_input
	};

	struct element_t {
		e_element_type m_type = element_text;
		std::string m_label   = { };
		std::string m_format  = { };
		std::vector< std::string > m_items = { };

		bool m_bool  = false;
		int m_int    = 0, m_int_min = 0, m_int_max = 100;
		float m_float = 0.f, m_float_min = 0.f, m_float_max = 1.f;
		float m_color[ 4 ] = { 1.f, 1.f, 1.f, 1.f };
		key_bind_t m_bind  = { };
		char m_text[ 256 ] = { };

		bool m_visible = true;
		bool m_clicked = false;
		int m_callback = -2;
	};

	struct script_t {
		std::string m_name;
		std::filesystem::path m_path;
		/* game thread only, never touched by the menu */
		lua_State* m_state = nullptr;
		/* deque: keybind toggles key on the element address, push_back must not move old ones */
		std::deque< element_t > m_elements;
		std::string m_error;
		std::vector< void* > m_allocations;
		bool m_unload_requested = false;
		bool m_dirty            = false;
	};

	enum e_request : int {
		request_load = 0,
		request_unload,
		request_reload,
		request_refresh
	};

	struct impl_t {
		/* game thread. on_paint = request queue + paint callbacks, MatSystemTopPanel before swap_draw_data */
		void on_paint( );
		void on_create_move( c_user_cmd* cmd, bool* send_packet, bool pre_prediction );
		void on_frame_stage( int stage );
		void on_event( game_event_t* event );
		void on_level_init( );
		void on_level_shutdown( );
		/* eject: runs every unload callback and closes every state, game thread */
		void shutdown( );

		/* any thread */
		void request( e_request type, const std::string& name = { } );

		std::filesystem::path folder( ) const;

		/* menu reads/writes scripts + elements only under m_mutex; game thread changes the list only under it.
		   never hold it while lua runs (render thread waits on it) */
		std::mutex m_mutex;
		std::vector< std::unique_ptr< script_t > > m_scripts;
		std::vector< std::pair< e_request, std::string > > m_requests;

		bool m_initialised = false;
		std::atomic< bool > m_shutdown{ false };

		/* lua_api.cpp side */
		void load( script_t& script );
		void unload( script_t& script, bool run_callback );
		void refresh( );
		void save_settings( script_t& script );
		void apply_saved( script_t& script, element_t& element );
		void write_autoload( );
		void fail( script_t& script, const char* where, const char* message );
	};
}

inline n_lua::impl_t g_lua{ };
