#include <shared_mutex>

#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"
#include "../../globals/logger/logger.h"
#include "../hooks.h"

#include "../../hacks/debug/debug.h"
#include "../../hacks/indicators/indicators.h"
#include "../../hacks/mc_hud/mc_hud.h"
#include "../../hacks/misc/misc.h"
#include "../../hacks/movement/movement.h"
#include "../../hacks/movement/movement_recorder.h"
#include "../../hacks/visuals/bullets/bullets.h"
#include "../../hacks/visuals/edicts/edicts.h"
#include "../../hacks/visuals/grenade/grenade_path.h"
#include "../../hacks/visuals/players/dormancy/dormancy.h"
#include "../../hacks/visuals/players/players.h"
#include "../../utilities/perf/perf_watch.h"

void __fastcall n_detoured_functions::paint_traverse( void* ecx, void* edx, unsigned int panel, bool force_repaint, bool force )
{
	static auto original = g_hooks.m_paint_traverse.get_original< decltype( &n_detoured_functions::paint_traverse ) >( );

	HOOK_SCOPE_OR_BAIL( original( ecx, edx, panel, force_repaint, force ) );

	const char* panel_name = g_interfaces.m_panel->get_panel_name( panel );

	const unsigned int panel_hash = HASH_RT( panel_name );

	if ( panel_hash == HASH_BT( "GameConsole" ) || panel_hash == HASH_BT( "CompletionList" ) ) {
		g_ctx.m_is_console_being_drawn = true;
		original( ecx, edx, panel, force_repaint, force );
		g_ctx.m_is_console_being_drawn = false;
		return;
	}

	/* retail CHudScope panel name, 2019 source says "HudScope" */
	if ( panel_hash == HASH_BT( "HudZoom" ) ) {
		g_ctx.m_is_scope_being_drawn = true;
		original( ecx, edx, panel, force_repaint, force );
		g_ctx.m_is_scope_being_drawn = false;
		return;
	}

	if ( panel_hash == HASH_BT( "HudWeapon" ) ) {
		g_ctx.m_is_hud_weapon_being_drawn = true;
		g_ctx.m_crosshair_outline_next    = true;
		original( ecx, edx, panel, force_repaint, force );
		g_ctx.m_is_hud_weapon_being_drawn = false;
		return;
	}

	original( ecx, edx, panel, force_repaint, force );

	static HWND foreground_window = LI_FN( GetForegroundWindow )( );

	if ( static float last_checked = g_interfaces.m_global_vars_base->m_real_time;
	     fabs( last_checked - g_interfaces.m_global_vars_base->m_real_time ) > 2.0f ) {
		foreground_window = LI_FN( GetForegroundWindow )( );
		last_checked = g_interfaces.m_global_vars_base->m_real_time;
	}

	g_ctx.m_is_window_focused = foreground_window == g_input.m_window;

	switch ( panel_hash ) {
	case HASH_BT( "MatSystemTopPanel" ): {
		PERF_ZONE( zone_paint );

		if ( g_ctx.m_world_restore_requested.load( std::memory_order_acquire ) &&
		     !g_ctx.m_world_restore_done.load( std::memory_order_acquire ) ) {
			g_misc.on_level_shutdown( );
			g_edicts.fog( true );
			g_edicts.skybox_fog( true );
			g_edicts.skybox( true );
			g_edicts.fullbright( true );
			g_ctx.m_world_restore_done = true;
		}

		std::shared_lock< std::shared_mutex > font_lock( g_render.m_font_mutex );

		g_render.clear_draw_data( );

		/* main-thread matrix copy: end_scene must not read the live (torn) one */
		g_render.cache_view_matrix( );

		if ( g_render.m_initialised ) {
			g_ctx.m_low_fps = static_cast< int >( ImGui::GetIO( ).Framerate + 0.5f ) <
			                  static_cast< int >( 1.f / g_interfaces.m_global_vars_base->m_interval_per_tick );

			{
				PERF_ZONE( zone_paint_misc );
				g_misc.on_paint_traverse( );
			}

			{
				PERF_ZONE( zone_paint_movement );
				g_movement.on_paint_traverse( );
			}

			g_movement_recorder.on_paint_traverse( );

			g_logger.on_paint_traverse( );

			// every frame, on or off: off is what puts the crosshair back and drops an open chat line
			g_mc_hud.on_paint_traverse( );

			kill_effects_paint( );

#ifdef _DEBUG
			g_debugger.on_paint_traverse( );
#endif

			if ( g_ctx.m_local ) {
				g_indicators.on_paint_traverse( );
				g_edicts.on_paint_traverse( );
				g_grenade_path.on_paint_traverse( );
				g_bullets.on_paint_traverse( );

				{
					PERF_ZONE( zone_paint_players );
					g_players.on_paint_traverse( );
				}
			} else
				g_indicators.m_indicator_data.reset( );
		}

		g_render.swap_draw_data( );
		break;
	}
	}
}
