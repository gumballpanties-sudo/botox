#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"
#include "../../hacks/misc/scaleform/scaleform.h"
#include "../../hacks/chud_hud/chud_hud.h"
#include "../../hacks/avatar_cache/avatar_cache.h"
#include "../../hacks/skins/skins.h"
#include "../../hacks/aimbot/aimbot.h"
#include "../../hacks/movement/movement.h"
#include "../../hacks/visuals/bullets/bullets.h"
#include "../hooks.h"

void __fastcall n_detoured_functions::level_init_post_entity( void* ecx, void* edx )
{
	static auto original = g_hooks.m_level_init_post_entity.get_original< decltype( &n_detoured_functions::level_init_post_entity ) >( );
	HOOK_SCOPE_OR_BAIL( original( ecx, edx ) );

	g_scaleform.on_level_init( );
	g_chud.on_level_init( );
	g_skins.on_level_init( );
	g_aimbot.on_level_init( );
	g_movement.on_level_init( );
	g_bullets.on_level_init( );

	original( ecx, edx );
}
