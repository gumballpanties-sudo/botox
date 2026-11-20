#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"
#include "../../hacks/misc/misc.h"
#include "../../hacks/movement/movement.h"
#include "../../hacks/visuals/grenade/grenade_live.h"
#include "../../hacks/visuals/players/players.h"
#include "../../hacks/visuals/screen/ambient_occlusion.h"
#include "../../hacks/visuals/screen/bloom.h"
#include "../../hacks/visuals/screen/depth_of_field.h"
#include "../../hacks/visuals/screen/depth_source.h"
#include "../../hacks/visuals/screen/depth_view.h"
#include "../../hacks/visuals/screen/depth_warp.h"
#include "../../hacks/visuals/screen/flip_world.h"
#include "../../hacks/visuals/screen/player_stencil.h"
#include "../../hacks/visuals/screen/rear_mirror.h"
#include "../../hacks/visuals/screen/reflections.h"
#include "../../hacks/visuals/screen/render_queue.h"
#include "../../hacks/visuals/screen/true_motion_blur.h"
#include "../../hacks/web/websurface.h"
#include "../../utilities/perf/perf_watch.h"
#include "../hooks.h"

namespace
{
	enum : unsigned int { pass_view, pass_reflections, pass_ao, pass_fog, pass_warp, pass_dof };

	int g_pre_frame         = -1;
	unsigned int g_pre_mask = 0;

	// bit set = pass runs at DrawViewModels entry: gun drawn after it, never in its frame copy or depth
	unsigned int gun_free_mask( )
	{
		const bool gun_free[] = { !GET_VARIABLE( g_variables.m_emphasize_viewmodel, bool ) && !GET_VARIABLE( g_variables.m_show_depth, bool ),
			                      !GET_VARIABLE( g_variables.m_reflections_viewmodel, bool ),
			                      GET_VARIABLE( g_variables.m_ambient_occlusion_viewmodel, bool ),
			                      !GET_VARIABLE( g_variables.m_adaptive_fog_viewmodel, bool ),
			                      !GET_VARIABLE( g_variables.m_depth_distort_viewmodel, bool ),
			                      GET_VARIABLE( g_variables.m_depth_of_field_viewmodel_sharp, bool ) };

		unsigned int mask = 0;

		for ( unsigned int pass = 0; pass < std::size( gun_free ); ++pass )
			mask |= gun_free[ pass ] ? 1u << pass : 0u;

		// warp moves pixels: a live depth pass before it left at post would read z that no longer matches the frame ( ghost edges )
		const bool on_before_warp[] = { GET_VARIABLE( g_variables.m_emphasize, bool ) || GET_VARIABLE( g_variables.m_show_depth, bool ),
			                            GET_VARIABLE( g_variables.m_reflections, bool ), GET_VARIABLE( g_variables.m_ambient_occlusion, bool ),
			                            GET_VARIABLE( g_variables.m_adaptive_fog, bool ) };

		for ( unsigned int pass = pass_view; pass < pass_warp; ++pass )
			if ( on_before_warp[ pass ] && !( ( mask >> pass ) & 1u ) )
				mask &= ~( 1u << pass_warp );

		return mask;
	}
}

void n_detoured_functions::screen_passes( c_view_setup* setup, const bool before_viewmodel )
{
	if ( !setup )
		return;

	const int frame = g_interfaces.m_global_vars_base ? g_interfaces.m_global_vars_base->m_frame_count : 0;

	// one snapshot per frame: a menu flip between the two calls can't run a pass twice or skip it
	if ( before_viewmodel ) {
		g_pre_frame = frame;
		g_pre_mask  = gun_free_mask( );
	}

	// DrawViewModels missed this frame = everything at DoPostScreenSpaceEffects
	const bool pre_ran      = g_pre_frame == frame;
	const unsigned int here = before_viewmodel ? g_pre_mask : ( pre_ran ? ~g_pre_mask : ~0u );

	// z tested world draws stay under every pass
	if ( before_viewmodel || !pre_ran ) {
		kill_effects_world( setup->m_origin, setup->m_angles );
		floor_level_world( );
		g_grenade_live.on_post_screen_space_effects( setup );
	}

	// inject splash up: no screen pass (their first shader builds stall frames)
	if ( boot_screen_active( ) )
		return;

	PERF_ZONE( zone_screen_pass );

	const auto run = [ here ]( const unsigned int pass ) { return ( ( here >> pass ) & 1u ) != 0; };

	if ( run( pass_view ) )
		g_depth_view.on_post_screen_space_effects( setup );

	if ( run( pass_reflections ) )
		g_reflections.on_post_screen_space_effects( setup );

	if ( run( pass_ao ) )
		g_ambient_occlusion.on_post_screen_space_effects( setup );

	// under dof ( dof blurs the fog ) and under depth warp ( fog warps with the frame its depth belongs to )
	if ( run( pass_fog ) )
		g_depth_view.on_post_screen_space_effects_fog( setup );

	// before dof so the blur lands on the distorted frame
	if ( run( pass_warp ) )
		g_depth_warp.on_post_screen_space_effects( setup );

	if ( run( pass_dof ) )
		g_depth_of_field.on_post_screen_space_effects( setup );

	// copy routes ( resz / nvapi / matsys ) hold gun-free z now: post passes ( motion blur gun fence ) must copy again
	if ( before_viewmodel && here ) {
		const auto stale = [ ] { g_depth_source.invalidate_resolve( ); };

		if ( !n_render_queue::submit( stale ) )
			stale( );
	}
}

void __fastcall n_detoured_functions::do_post_screen_space_effects( void* ecx, void* edx, c_view_setup* setup )
{
	static auto original = g_hooks.m_do_post_screen_space_effects.get_original< decltype( &n_detoured_functions::do_post_screen_space_effects ) >( );

	HOOK_SCOPE_OR_BAIL( original( ecx, edx, setup ) );

	/* ClientModeCSNormal::DoPostScreenSpaceEffects = glow only: a second glow pass a frame doubles its pulse / health fades */
	if ( g_rear_mirror.drawing( ) )
		return;

	screen_passes( setup, false );

	const bool passes = !boot_screen_active( );

	PERF_ZONE( zone_screen_pass );

	Web_OnPostScreenSpaceEffects( setup );

	if ( passes ) {
		g_true_motion_blur.on_post_screen_space_effects( setup );

		g_bloom.on_post_screen_space_effects( setup );
	}

	/* glow + stencil over every pass above ( no blur / bloom / warp on them ). passes restore the depth-stencil, so the
	   engine glow still depth tests against the scene and writes the stencil refs player_stencil reads */
	original( ecx, edx, setup );

	if ( passes && n_players::esp_on( ) )
		g_player_stencil.after_glow( );

	// last: DoPostScreenSpaceEffects runs after DrawViewModels in csgo, glow + every pass above must be in the mirror
	g_flip_world.on_post_screen_space_effects( setup );
}
