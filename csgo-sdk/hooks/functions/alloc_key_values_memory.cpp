#include "../../globals/includes/includes.h"
#include "../../utilities/memory/relative.h"
#include "../hooks.h"

void* __fastcall n_detoured_functions::alloc_key_values_memory( void* ecx, void* edx, int size )
{
	static auto original = g_hooks.m_alloc_key_values_memory.get_original< decltype( &n_detoured_functions::alloc_key_values_memory ) >( );
	HOOK_SCOPE_OR_BAIL( original( ecx, edx, size ) );

	/* a miss on another build = 0 (never a return address), not a rel32 read at address 1 */
	const auto call_target = []( const unsigned char* site, const unsigned int offset ) -> unsigned int {
		return site ? g_relative.get( reinterpret_cast< unsigned int >( site + 0x1 ) ) + offset : 0u;
	};

	static const auto key_values_alloc_engine =
		call_target( g_modules[ ENGINE_DLL ].find_pattern( "E8 ? ? ? ? 83 C4 08 84 C0 75 10 FF 75 0C" ), 0x4A );
	static const auto key_values_alloc_client = call_target( g_modules[ CLIENT_DLL ].find_pattern( "E8 ? ? ? ? 83 C4 08 84 C0 75 10" ), 0x3E );

	if ( const unsigned int return_address = reinterpret_cast< unsigned int >( _ReturnAddress( ) );
	     return_address == key_values_alloc_engine || return_address == key_values_alloc_client )
		return nullptr;

	return original( ecx, edx, size );
}
