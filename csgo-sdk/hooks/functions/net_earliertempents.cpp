#include "../../globals/globals.h"
#include "../../globals/macros/macros.h"
#include "../../utilities/memory/memory.h"
#include "../../utilities/modules/modules.h"
#include "../hooks.h"

#include <intrin.h>

bool _fastcall n_detoured_functions::net_earliertempents( void* ecx, void* edx )
{
	static auto original = g_hooks.m_net_earliertempents.get_original< decltype( &n_detoured_functions::net_earliertempents ) >( );
	HOOK_SCOPE_OR_BAIL( original( ecx, edx ) );

	static auto cl_readpackets = reinterpret_cast< void* >( g_modules[ ENGINE_DLL ].find_pattern( "53 8A D9 8B 0D ? ? ? ? 56 57 8B B9 ? ? ? ?" ) );

	if ( _ReturnAddress( ) == cl_readpackets )
		return true;

	return original( ecx, edx );
}
