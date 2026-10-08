#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"
#include "../../hacks/movement/movement.h"
#include "../hooks.h"

extern bool point_menu_is_opened( );

void __fastcall n_detoured_functions::lock_cursor( void* ecx, void* edx )
{
	static auto original = g_hooks.m_lock_cursor.get_original< decltype( &n_detoured_functions::lock_cursor ) >( );
	HOOK_SCOPE_OR_BAIL( original( ecx, edx ) );

	if ( g_menu.m_opened ) {
		g_interfaces.m_surface->unlock_cursor( );
		return;
	}

	if ( point_menu_is_opened( ) ) {
		g_interfaces.m_surface->unlock_cursor( );
		return;
	}

	original( ecx, edx );
}