#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"
#include "../hooks.h"

void __fastcall n_detoured_functions::run_command( void* ecx, void* edx, void* entity, void* cmd, void* move_helper )
{
	static auto original = g_hooks.m_run_command.get_original< decltype( &n_detoured_functions::run_command ) >( );
	HOOK_SCOPE_OR_BAIL( original( ecx, edx, entity, cmd, move_helper ) );

	original( ecx, edx, entity, cmd, move_helper );

	g_interfaces.m_move_helper = reinterpret_cast< c_move_helper* >( move_helper );
}