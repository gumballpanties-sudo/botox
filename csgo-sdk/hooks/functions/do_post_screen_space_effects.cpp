#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"
#include "../../hacks/misc/misc.h"
#include "../../hacks/visuals/screen/ambient_occlusion.h"
#include "../../hacks/visuals/screen/bloom.h"
#include "../../hacks/visuals/screen/depth_of_field.h"
#include "../../hacks/visuals/screen/depth_view.h"
#include "../../hacks/visuals/screen/flip_world.h"
#include "../../hacks/visuals/screen/player_stencil.h"
#include "../../hacks/visuals/screen/reflections.h"
#include "../../hacks/visuals/screen/resolution_spoof.h"
#include "../../hacks/visuals/screen/true_motion_blur.h"
#include "../../hacks/web/websurface.h"
#include "../../utilities/perf/perf_watch.h"
#include "../hooks.h"

void __fastcall n_detoured_functions::do_post_screen_space_effects( void* ecx, void* edx, c_view_setup* setup )
{
	static auto original = g_hooks.m_do_post_screen_space_effects.get_original< decltype( &n_detoured_functions::do_post_screen_space_effects ) >( );

	HOOK_SCOPE_OR_BAIL( original( ecx, edx, setup ) );

	original( ecx, edx, setup );

	if ( setup )
		kill_effects_world( setup->m_origin, setup->m_angles );

	g_player_stencil.after_glow( );

	PERF_ZONE( zone_screen_pass );

	g_depth_view.on_post_screen_space_effects( setup );

	g_reflections.on_post_screen_space_effects( setup );

	g_ambient_occlusion.on_post_screen_space_effects( setup );

	g_depth_of_field.on_post_screen_space_effects( setup );

	Web_OnPostScreenSpaceEffects( setup );

	g_true_motion_blur.on_post_screen_space_effects( setup );

	g_bloom.on_post_screen_space_effects( setup );

	g_resolution_spoof.on_post_screen_space_effects( setup );

	// last: DoPostScreenSpaceEffects runs after DrawViewModels in csgo, glow + every pass above must be in the mirror
	g_flip_world.on_post_screen_space_effects( setup );
}
