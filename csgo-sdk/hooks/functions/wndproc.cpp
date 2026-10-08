#include "../hooks.h"

#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"
#include "../../hacks/chud_hud/chud_hud.h"
#include "../../hacks/mc_hud/mc_hud.h"
#include "../../hacks/movement/movement.h"
#include "../../hacks/web/websurface.h"

extern bool point_menu_is_opened( );

long __stdcall n_detoured_functions::wndproc( HWND window, unsigned int message, unsigned int wide_param, long long_param )
{
	/* read ONCE: on_release can restore the proc mid-message; CallWindowProcW( nullptr ) crashes */
	const WNDPROC old_wnd_proc = g_input.m_old_wnd_proc;

	const auto pass_to_game = [ & ]( ) -> long {
		return old_wnd_proc ? CallWindowProcW( old_wnd_proc, window, message, wide_param, long_param )
		                    : DefWindowProcW( window, message, wide_param, long_param );
	};

	const n_ctx::hook_scope_t hook_scope_guard;
	if ( g_ctx.m_unloading.load( std::memory_order_relaxed ) )
		return pass_to_game( );

	const bool chat_input = g_mc_hud.chat_on_key( message, wide_param, long_param, g_menu.m_opened || point_menu_is_opened( ) ) ||
	                        g_chud.chat_on_key( message, wide_param, long_param, g_menu.m_opened || point_menu_is_opened( ) );

	if ( !chat_input )
		g_input.on_wndproc( message, wide_param, long_param );

	if ( g_input.is_key_released( VK_INSERT ) )
		g_menu.m_opened = !g_menu.m_opened;

	const bool imgui_input = g_menu.m_opened || point_menu_is_opened( );

	g_interfaces.m_input_system->enable_input( !imgui_input );

	if ( g_render.m_initialised && imgui_input && ImGui_ImplWin32_WndProcHandler( window, message, wide_param, long_param ) )
		return 1L;

	if ( chat_input )
		return 1L;

	if ( !imgui_input && Web_OnInput( message, wide_param ) )
		return 0L;

	return pass_to_game( );
}