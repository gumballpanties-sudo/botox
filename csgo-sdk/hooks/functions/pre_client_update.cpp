#include "../../globals/globals.h"
#include "../hooks.h"

void bot_names_server_write( );

/* listen server IServerGameDLL::PreClientUpdate: after entity thinks, right before snapshots go out */
void __fastcall n_detoured_functions::pre_client_update( void* ecx, void* edx, bool simulating )
{
	static auto original = g_hooks.m_pre_client_update.get_original< decltype( &n_detoured_functions::pre_client_update ) >( );
	HOOK_SCOPE_OR_BAIL( original( ecx, edx, simulating ) );

	original( ecx, edx, simulating );
	bot_names_server_write( );
}
