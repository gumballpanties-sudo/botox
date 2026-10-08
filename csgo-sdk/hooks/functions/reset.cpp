#include "../../globals/includes/includes.h"
#include "../../hacks/avatar_cache/avatar_cache.h"
#include "../../hacks/indicators/indicators.h"
#include "../../hacks/mc_hud/mc_hud.h"
#include "../../hacks/misc/misc.h"
#include "../../hacks/visuals/screen/ambient_occlusion.h"
#include "../../hacks/visuals/screen/bloom.h"
#include "../../hacks/visuals/screen/color_correction.h"
#include "../../hacks/visuals/screen/depth_of_field.h"
#include "../../hacks/visuals/screen/depth_source.h"
#include "../../hacks/visuals/screen/depth_view.h"
#include "../../hacks/visuals/screen/flip_world.h"
#include "../../hacks/visuals/screen/fx_compat.h"
#include "../../hacks/visuals/screen/mc_clouds.h"
#include "../../hacks/visuals/screen/gpu_timer.h"
#include "../../hacks/visuals/screen/player_stencil.h"
#include "../../hacks/visuals/screen/reflections.h"
#include "../../hacks/visuals/screen/resolution_spoof.h"
#include "../../hacks/visuals/screen/true_motion_blur.h"
#include "../../hacks/visuals/weapon_sheen.h"
#include "../../hacks/web/websurface.h"
#include "../hooks.h"

long __stdcall n_detoured_functions::reset( IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* presentation_parameters )
{
	static auto original = g_hooks.m_reset.get_original< decltype( &n_detoured_functions::reset ) >( );

	/* unload: release our DEFAULT-pool targets (they'd fail the Reset), never rebuild (imgui
	   CreateDeviceObjects on a dying context crashes) */
	const n_ctx::hook_scope_t hook_scope_guard;
	if ( g_ctx.m_unloading.load( std::memory_order_relaxed ) ) {
		g_hooks.release_render_resources( );

		return original( device, presentation_parameters );
	}

	g_resolution_spoof.on_device_lost( );
	g_color_correction.on_device_lost( );
	g_depth_of_field.on_device_lost( );
	g_depth_view.on_device_lost( );
	g_reflections.on_device_lost( );
	g_ambient_occlusion.on_device_lost( );
	g_true_motion_blur.on_device_lost( );
	g_bloom.on_device_lost( );
	g_player_stencil.on_device_lost( );

	g_depth_source.on_device_lost( );
	g_weapon_sheen.on_device_lost( );
	g_flip_world.on_device_lost( );
	g_mc_clouds.on_device_lost( );
	n_gpu_timer::on_device_lost( );
	n_fx_compat::on_device_lost( );

	g_indicators.release_textures( );
	g_mc_hud.release_textures( );
	kill_effects_release_textures( );
	Web_InvalidateTexture( );

	if ( !g_render.m_initialised )
		return original( device, presentation_parameters );

	ImGui_ImplDX9_InvalidateDeviceObjects( );

	/* same thread that created them; think( ) refetches within a second */
	g_avatar_cache.release_all( );

	g_misc.release_spectator_textures( true );

	const long reset_result = original( device, presentation_parameters );

	if ( reset_result == ( ( long )0l ) )
		ImGui_ImplDX9_CreateDeviceObjects( );

	return reset_result;
}