#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"
#include "../hooks.h"

#include "../../hacks/entity_cache/entity_cache.h"
#include "../../hacks/avatar_cache/avatar_cache.h"
#include "../../hacks/misc/scaleform/scaleform.h"
#include "../../hacks/skins/skins.h"
#include "../../hacks/visuals/edicts/edicts.h"

void __stdcall n_detoured_functions::level_init_pre_entity( const char* map_name )
{
	static auto original = g_hooks.m_level_init_pre_entity.get_original< decltype( &n_detoured_functions::level_init_pre_entity ) >( );
	HOOK_SCOPE_OR_BAIL( original( map_name ) );

	g_edicts.reset( );
	g_avatar_cache.reset( );

	/* pairs with level_shutdown: first map after a disconnect never shut one down */
	g_entity_cache.reset( );

	g_skins.on_level_pre_load( );

	g_convars.rescan( );

	const float rate = 1.f / g_interfaces.m_global_vars_base->m_interval_per_tick;
	g_convars.set_if_present( HASH_BT( "cl_updaterate" ), rate );
	g_convars.set_if_present( HASH_BT( "cl_cmdrate" ), rate );

	return original( map_name );
}
