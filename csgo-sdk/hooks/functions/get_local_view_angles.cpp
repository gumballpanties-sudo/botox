#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"
#include "../../hacks/misc/misc.h"
#include "../hooks.h"

void __fastcall n_detoured_functions::get_local_view_angles( void* ecx, void* edx, c_angle& angles )
{
	static auto original = g_hooks.m_get_local_view_angles.get_original< decltype( &n_detoured_functions::get_local_view_angles ) >( );
	HOOK_SCOPE_OR_BAIL( original( ecx, edx, angles ) );

	original( ecx, edx, angles );

	fake_pov_demo_angles( angles );
}
