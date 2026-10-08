#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"
#include "../hooks.h"

bool __fastcall n_detoured_functions::is_playing_demo( void* ecx, [[maybe_unused]] void* edx )
{
	static auto original = g_hooks.m_is_playing_demo.get_original< bool( __thiscall* )( void* ) >( );
	HOOK_SCOPE_OR_BAIL( original( ecx ) );

	return original( ecx );
}
