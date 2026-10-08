#pragma once
#include "key_bind_t.h"

#include <Windows.h>

#include <array>
#include <cstdint>

enum class e_key_state : int {
	none = 0,
	down,
	up,
	released
};

namespace n_input
{
	struct impl_t {
		HWND m_window          = nullptr;
		WNDPROC m_old_wnd_proc = nullptr;

		std::array< e_key_state, 256U > m_key_state = { };
		std::array< std::uint32_t, 256U > m_release_seq = { };

		bool on_attach( );
		void on_release( );
		bool on_wndproc( unsigned int msg, unsigned int wide_param, long long_param );

		bool is_key_down( const std::uint32_t button_code )
		{
			return this->m_key_state[ button_code ] == e_key_state::down;
		}

		bool is_key_released( const std::uint32_t button_code )
		{
			if ( this->m_key_state[ button_code ] == e_key_state::released ) {
				this->m_key_state[ button_code ] = e_key_state::up;
				return true;
			}

			return false;
		}

		/* keys belong to a menu / chat / console, not binds. any thread: plain reads only */
		bool keys_blocked( );

		bool check_input( key_bind_t* key_data );
	};
}

inline n_input::impl_t g_input{ };