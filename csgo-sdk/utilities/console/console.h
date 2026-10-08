#pragma once

namespace n_console
{
	enum class log_level {
		NORMAL = 0,
		DEBUG,
		SUCCESS,
		WARNING,
		FATAL
	};

	enum log_colors : int {
		BLACK     = 0,
		DARK_BLUE = 1,
		DARK_GREEN,
		DARK_CYAN,
		DARK_RED,
		DARK_PURPLE,
		DARK_YELLOW,
		DARK_WHITE,
		GRAY,
		BLUE,
		GREEN,
		CYAN,
		RED,
		PURPLE,
		YELLOW,
		WHITE
	};

	struct impl_t {
		void on_attach( const char* console_title );
		void on_release( );

		void emit( log_level level, const char* text );

		template< n_console::log_level lev = n_console::log_level::NORMAL >
		void print( const char* text );
	};
}

inline n_console::impl_t g_console{ };

#include "console.inl"
