#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"
#include "../../hacks/visuals/screen/flip_world.h"
#include "../hooks.h"

const view_matrix_t& __fastcall n_detoured_functions::world_to_screen_matrix( void* ecx, [[maybe_unused]] void* edx )
{
	static auto original = g_hooks.m_world_to_screen_matrix.get_original< const view_matrix_t&( __thiscall* )( void* ) >( );

	HOOK_SCOPE_OR_BAIL( original( ecx ) );

	return g_flip_world.on_world_to_screen_matrix( original( ecx ) );
}
