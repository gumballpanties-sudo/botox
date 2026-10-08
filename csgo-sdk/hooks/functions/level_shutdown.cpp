#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"
#include "../hooks.h"

#include "../../hacks/avatar_cache/avatar_cache.h"
#include "../../hacks/entity_cache/entity_cache.h"
#include "../../hacks/visuals/edicts/edicts.h"
#include "../../hacks/misc/misc.h"
#include "../../hacks/chams/chams.h"
#include "../../hacks/visuals/players/sound_esp/sound_esp.h"
#include "../../hacks/web/websurface.h"

void __fastcall n_detoured_functions::level_shutdown( void* thisptr )
{
	static auto original = g_hooks.m_level_shutdown.get_original< decltype( &n_detoured_functions::level_shutdown ) >( );
	HOOK_SCOPE_OR_BAIL( original( thisptr ) );

	g_edicts.reset( );

	g_entity_cache.reset( );

	g_misc.on_level_shutdown( );
	g_chams.reset_color_cache( );

	g_sound_esp.reset( );

	Web_LevelShutdown( );

	g_ctx.m_local = nullptr;

	return original( thisptr );
}
