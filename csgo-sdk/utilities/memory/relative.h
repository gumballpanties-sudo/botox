#pragma once

namespace n_relative
{
	struct impl_t {
		unsigned int get( const unsigned int address )
		{
			return address + 0x4 + *reinterpret_cast< int* >( address );
		}
	};
}

inline n_relative::impl_t g_relative{ };