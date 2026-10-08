#include "../hooks.h"
#include "../../globals/globals.h"
#include "../../hacks/skins/skins.h"

unsigned short __fastcall n_detoured_functions::find_mdl( void* ecx, void* edx, const char* path )
{
	static auto original = g_hooks.m_find_mdl.get_original< decltype( &n_detoured_functions::find_mdl ) >( );
	HOOK_SCOPE_OR_BAIL( original( ecx, edx, path ) );

	/* never store the handle: the cache recycles it once the last ref drops */
	if ( const char* redirect = n_skins::knife_anim_redirect( path ) )
		return original( ecx, edx, redirect );

	return original( ecx, edx, path );
}
