#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"
#include "../hooks.h"

#include <intrin.h>

bool __fastcall n_detoured_functions::is_hltv( void* ecx, [[maybe_unused]] void* edx )
{
	static auto original = g_hooks.m_is_hltv.get_original< bool( __thiscall* )( void* ) >( );
	HOOK_SCOPE_OR_BAIL( original( ecx ) );

	/* called in killer replay teardown with m_local freed: never read it, fetch local fresh */
	if ( original( ecx ) )
		return true;

	const auto local = g_interfaces.m_client_entity_list->get< c_base_entity >( g_interfaces.m_engine_client->get_local_player( ) );
	if ( !local || !local->is_alive( ) )
		return false;

	/* a miss on another build = null, not a read of address 0 */
	const auto read_site = []( const unsigned char* site ) -> void* { return site ? *reinterpret_cast< void* const* >( site ) : nullptr; };

	static const auto return_to_setup_velocity =
		read_site( g_modules[ CLIENT_DLL ].find_pattern( "84 C0 75 38 8B 0D ? ? ? ? 8B 01 8B 80 ? ? ? ? FF D0" ) );
	static const auto return_to_accumulate_layers = read_site( g_modules[ CLIENT_DLL ].find_pattern( "84 C0 75 0D F6 87" ) );

	if ( const auto return_address = _ReturnAddress( ); return_address == return_to_setup_velocity || return_address == return_to_accumulate_layers )
		return true;

	return original( ecx );
}