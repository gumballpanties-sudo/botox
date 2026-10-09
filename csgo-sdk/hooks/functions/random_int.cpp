#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"
#include "../../globals/logger/logger.h"
#include "../hooks.h"

#include "../../hacks/skins/skins.h"

#include <intrin.h>

int __cdecl n_detoured_functions::random_int( int min, int max )
{
	static auto original = g_hooks.m_random_int.get_original< decltype( &n_detoured_functions::random_int ) >( );
	HOOK_SCOPE_OR_BAIL( original( min, max ) );

	unsigned long long until = n_skins::g_rig_lookat_until.load( std::memory_order_relaxed );
	if ( !until || min != 0 || max != n_skins::k_deagle_lookat_weights - 1 )
		return original( min, max );

	HMODULE caller = nullptr;
	GetModuleHandleExA( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                    static_cast< const char* >( _ReturnAddress( ) ), &caller );
	if ( !caller || caller != GetModuleHandleA( "server.dll" ) )
		return original( min, max );

	const bool live = GetTickCount64( ) < until;
	if ( !n_skins::g_rig_lookat_until.compare_exchange_strong( until, 0 ) || !live )
		return original( min, max );

	original( min, max ); // stream advances like a real roll
	botox_dbg_log( "[spin] local roll rigged -> lookat02" );
	return max;
}
