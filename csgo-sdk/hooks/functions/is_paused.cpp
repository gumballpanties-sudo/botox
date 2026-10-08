#include "../../globals/includes/includes.h"
#include "../../game/sdk/includes/includes.h"
#include "../hooks.h"

#include <intrin.h>

bool __fastcall n_detoured_functions::is_paused( void* ecx, [[maybe_unused]] void* edx )
{
	static auto original = g_hooks.m_is_paused.get_original< bool( __thiscall* )( void* ) >( );
	HOOK_SCOPE_OR_BAIL( original( ecx ) );

	const auto local = g_interfaces.m_client_entity_list->get< c_base_entity >( g_interfaces.m_engine_client->get_local_player( ) );
	if ( !local || !local->is_alive( ) )
		return original( ecx );

	static const auto is_paused_extrapolate =
		reinterpret_cast< unsigned int >( g_modules[ CLIENT_DLL ].find_pattern( "0F B6 0D ? ? ? ? 84 C0 0F 44" ) );

	if ( const auto return_address = reinterpret_cast< unsigned int >( _ReturnAddress( ) ); return_address == is_paused_extrapolate )
		return true;

	return original( ecx );
}