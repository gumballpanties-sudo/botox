#include "../../game/sdk/classes/c_move_data.h"
#include "../../globals/globals.h"
#include "../hooks.h"

bool tung_surf_apply( c_base_entity* player, c_move_data* mv );
void tung_surf_apply_sv( c_move_data* mv );

void __fastcall n_detoured_functions::process_movement( void* thisptr, void* edx, c_base_entity* player, c_move_data* move_data )
{
	static auto original = g_hooks.m_process_movement.get_original< decltype( &n_detoured_functions::process_movement ) >( );
	HOOK_SCOPE_OR_BAIL( original( thisptr, edx, player, move_data ) );

	move_data->m_game_code_moved_player = false;
	tung_surf_apply( player, move_data );

	return original( thisptr, edx, player, move_data );
}

void __fastcall n_detoured_functions::process_movement_sv( void* thisptr, void* edx, void* player, c_move_data* move_data )
{
	static auto original = g_hooks.m_process_movement_sv.get_original< decltype( &n_detoured_functions::process_movement_sv ) >( );
	HOOK_SCOPE_OR_BAIL( original( thisptr, edx, player, move_data ) );

	tung_surf_apply_sv( move_data );

	return original( thisptr, edx, player, move_data );
}
