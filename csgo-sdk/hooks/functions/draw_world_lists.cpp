#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"
#include "../../hacks/visuals/screen/mc_clouds.h"
#include "../hooks.h"

void __fastcall n_detoured_functions::draw_world_lists( void* ecx, void* edx, void* render_context, void* list, unsigned long flags,
                                                        float water_z_adjust )
{
	static auto original = g_hooks.m_draw_world_lists.get_original< decltype( &n_detoured_functions::draw_world_lists ) >( );

	HOOK_SCOPE_OR_BAIL( original( ecx, edx, render_context, list, flags, water_z_adjust ) );

	original( ecx, edx, render_context, list, flags, water_z_adjust );

	g_mc_clouds.on_draw_world_lists( flags );
}
