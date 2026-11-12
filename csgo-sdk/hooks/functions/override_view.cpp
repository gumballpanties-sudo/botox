#include "../../game/sdk/includes/includes.h"
#include "../../globals/globals.h"
#include "../../hacks/visuals/screen/mc_clouds.h"
#include "../../hacks/visuals/screen/screen.h"
#include "../../hacks/visuals/edicts/edicts.h"
#include "../hooks.h"

void __fastcall n_detoured_functions::override_view( void* ecx, void* edx, c_view_setup* setup )
{
	static auto original = g_hooks.m_override_view.get_original< decltype( &n_detoured_functions::override_view ) >( );
	HOOK_SCOPE_OR_BAIL( original( ecx, edx, setup ) );

	g_screen.on_override_view( setup );

	original( ecx, edx, setup );

	g_screen.on_override_view_post( setup );

	g_mc_clouds.on_override_view( setup );

	if ( setup ) {
		g_edicts.m_lamp_eye[ 0 ] = setup->m_origin.m_x;
		g_edicts.m_lamp_eye[ 1 ] = setup->m_origin.m_y;
		g_edicts.m_lamp_eye[ 2 ] = setup->m_origin.m_z;
	}
}
