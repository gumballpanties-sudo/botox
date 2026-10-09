#include <mutex>
#include <shared_mutex>

#include "../../globals/includes/includes.h"
#include "../../hacks/chud_hud/chud_hud.h"
#include "../../hacks/entity_cache/entity_cache.h"
#include "../../hacks/mc_hud/mc_hud.h"
#include "../../hacks/misc/misc.h"
#include "../../hacks/misc/scaleform/image_cache.h"
#include "../../hacks/movement/movement.h"
#include "../../hacks/movement/movement_recorder.h"
#include "../../hacks/visuals/players/players.h"
#include "../../hacks/visuals/screen/color_correction.h"
#include "../../hacks/visuals/screen/fx_compat.h"
#include "../../hacks/visuals/screen/reflections.h"
#include "../../hacks/visuals/screen/resolution_spoof.h"
#include "../../hacks/web/websurface.h"
#include "../../utilities/perf/perf_watch.h"
#include "../hooks.h"
#include "flash_probe.h"

void n_detoured_functions::draw_overlay_frame( IDirect3DDevice9* device )
{
	PERF_ZONE( zone_overlay );

	g_render.on_end_scene(
		[ & ]( ) {
			g_movement.on_end_scene( );

			g_movement_recorder.on_end_scene( );

			Web_Present( device );

			g_mc_hud.on_end_scene( );

			g_menu.on_end_scene( );
			g_misc.on_end_scene( );
		},
		device );
}

long __stdcall n_detoured_functions::end_scene( IDirect3DDevice9* device )
{
	static auto original      = g_hooks.m_end_scene.get_original< decltype( &n_detoured_functions::end_scene ) >( );
	static void* used_address = nullptr;

	/* unload: no frame, no passes. this thread made the textures, so it releases them here */
	const n_ctx::hook_scope_t hook_scope_guard;
	if ( g_ctx.m_unloading.load( std::memory_order_relaxed ) ) {
		g_hooks.release_render_resources( );

		return original( device );
	}

	if ( !used_address ) {
		constexpr int k_rejected_max = 8;
		static void* rejected[ k_rejected_max ]{ };
		static int rejected_count = 0;

		void* const caller = _ReturnAddress( );

		bool known = false;
		for ( int i = 0; i < rejected_count; ++i ) {
			if ( rejected[ i ] == caller ) {
				known = true;
				break;
			}
		}

		if ( !known ) {
			MEMORY_BASIC_INFORMATION memory_basic_information = { };

			VirtualQuery( caller, &memory_basic_information, sizeof( MEMORY_BASIC_INFORMATION ) );

			char module_name[ MAX_PATH ] = { };
			GetModuleFileName( static_cast< HMODULE >( memory_basic_information.AllocationBase ), module_name, MAX_PATH );

			if ( strstr( module_name, ( "gameoverlayrenderer.dll" ) ) )
				used_address = caller;
			else if ( rejected_count < k_rejected_max )
				rejected[ rejected_count++ ] = caller;
		}
	}

	if ( _ReturnAddress( ) != used_address )
		return original( device );

	s_overlay_frame_seen = true;

	return overlay_end_scene( device, original );
}

long n_detoured_functions::overlay_end_scene( IDirect3DDevice9* device, long( __stdcall* end_scene_fn )( IDirect3DDevice9* ) )
{
	struct perf_frame_guard_t {
		~perf_frame_guard_t( ) { n_perf::frame_end( ); }
	} perf_frame_guard;

	kill_effects_capture( device );

	g_resolution_spoof.on_end_scene( );

	const bool overlay_pass = g_resolution_spoof.wants_overlay_pass( );

	const bool graded = g_color_correction.wants_pass( );

	if ( !overlay_pass && !graded )
		draw_overlay_frame( device );

	long result = 0l;
	{
		PERF_ZONE( zone_end_scene );
		result = end_scene_fn( device );
	}

	flash_probe_sample( device, 0 );

	{
		PERF_ZONE( zone_screen_pass );

		bool corrected = false;
		n_fx_compat::run( device, "cc", [ & ] { corrected = g_color_correction.on_end_scene_post( device ); } );

		if ( corrected )
			Web_RestoreOverColorCorr( device, g_color_correction.input_texture( ), g_color_correction.width( ),
			                          g_color_correction.height( ) );
	}

	flash_probe_sample( device, 1 );

	if ( graded && !overlay_pass && SUCCEEDED( device->BeginScene( ) ) ) {
		draw_overlay_frame( device );
		device->EndScene( );
	}

	/* leak watch: one line per 8 mb of new private bytes; mem_due gates the locked counts */
	if ( n_perf::mem_due( ) ) {
		n_perf::mem_counts_t counts{ };

		{
			// game thread swaps this queue
			std::shared_lock< std::shared_mutex > draw_lock( g_render.m_mutex );

			counts.m_draw_data = g_render.m_thread_safe_draw_data.size( );
		}

		counts.m_players        = g_entity_cache.count( e_enumeration_type::type_players );
		counts.m_edicts         = g_entity_cache.count( e_enumeration_type::type_edicts );
		counts.m_damage_numbers = g_players.m_damage_numbers.size( );
		counts.m_kill_rows      = g_chud.m_kill_rows.size( );
		counts.m_chat_rows      = g_chud.m_chat_rows.size( );

		{
			std::lock_guard< std::mutex > image_lock( g_image_cache.m_lock );

			counts.m_image_cache = g_image_cache.m_state.size( );
		}

		counts.m_cubes = g_reflections.cube_texture_count( );

		n_perf::mem_tick( counts );
	}

	return result;
}
