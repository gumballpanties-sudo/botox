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

void poop_surf_demo_origin( c_vector& org );

/* IPrediction slot 8, callers = demo recorder ( cl_demo.cpp:376 ) + demo ui panel only */
void __fastcall n_detoured_functions::get_view_origin( void* ecx, void* edx, c_vector& origin )
{
	static auto original = g_hooks.m_get_view_origin.get_original< decltype( &n_detoured_functions::get_view_origin ) >( );
	HOOK_SCOPE_OR_BAIL( original( ecx, edx, origin ) );

	original( ecx, edx, origin );

	poop_surf_demo_origin( origin );
}
