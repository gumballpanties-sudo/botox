#include "../../globals/globals.h"
#include "../../hacks/visuals/screen/mc_clouds.h"
#include "../../hacks/visuals/screen/screen.h"
#include "../hooks.h"

void __fastcall n_detoured_functions::override_view( void* ecx, void* edx, c_view_setup* setup )
{
	static auto original = g_hooks.m_override_view.get_original< decltype( &n_detoured_functions::override_view ) >( );
	HOOK_SCOPE_OR_BAIL( original( ecx, edx, setup ) );

	g_screen.on_override_view( setup );

	original( ecx, edx, setup );

	g_mc_clouds.on_override_view( setup );
}
