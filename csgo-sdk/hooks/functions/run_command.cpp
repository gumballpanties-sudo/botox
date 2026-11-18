#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"
#include "../hooks.h"

void pred_fix_base_velocity( c_base_entity* e );

void __fastcall n_detoured_functions::run_command( void* ecx, void* edx, void* entity, void* cmd, void* move_helper )
{
	static auto original = g_hooks.m_run_command.get_original< decltype( &n_detoured_functions::run_command ) >( );
	HOOK_SCOPE_OR_BAIL( original( ecx, edx, entity, cmd, move_helper ) );

	/* the engine's own prediction must not add the base velocity decode garbage either ( prediction.cpp ) */
	if ( entity && entity == g_ctx.m_local )
		pred_fix_base_velocity( static_cast< c_base_entity* >( entity ) );
	original( ecx, edx, entity, cmd, move_helper );

	g_interfaces.m_move_helper = reinterpret_cast< c_move_helper* >( move_helper );
}