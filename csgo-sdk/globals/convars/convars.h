#pragma once

class c_cconvar;

namespace n_convars
{
	struct impl_t {
		bool on_attach( );

		void rescan( );

		c_cconvar* operator[]( unsigned int hash );

		int int_or( unsigned int hash, int fallback );
		float float_or( unsigned int hash, float fallback );

		void set_if_present( unsigned int hash, float value );
	};
}

inline n_convars::impl_t g_convars{ };