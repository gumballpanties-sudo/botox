#pragma once

namespace n_virtual
{
	struct impl_t {
		template< typename T = void* >
		constexpr T get( void* thisptr, unsigned int index )
		{
			return ( *static_cast< T** >( thisptr ) )[ index ];
		}

		template< typename T, typename... args_t >
		constexpr T call( void* thisptr, unsigned int index, args_t... args )
		{
			using fn = T( __thiscall* )( void*, decltype( args )... );

			return ( *static_cast< fn** >( thisptr ) )[ index ]( thisptr, args... );
		}
	};
}

inline n_virtual::impl_t g_virtual{ };