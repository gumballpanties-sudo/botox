#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"
#include "../hooks.h"

bool __fastcall n_detoured_functions::loose_files_allowed( void* ecx, void* edx )
{
	static auto original = g_hooks.m_loose_files_allowed.get_original< decltype( &n_detoured_functions::loose_files_allowed ) >( );
	HOOK_SCOPE_OR_BAIL( original( ecx, edx ) );

	if ( GET_VARIABLE( g_variables.m_pure_bypass, bool ) )
		return true;

	return original( ecx, edx );
}

void __cdecl n_detoured_functions::check_for_pure_server_whitelist( )
{
	static auto original = g_hooks.m_check_for_pure_server_whitelist.get_original< decltype( &n_detoured_functions::check_for_pure_server_whitelist ) >( );
	HOOK_SCOPE_OR_BAIL( original( ) );

	if ( GET_VARIABLE( g_variables.m_pure_bypass, bool ) )
		return;

	original( );
}
