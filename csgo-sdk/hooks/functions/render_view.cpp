#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"
#include "../../hacks/visuals/screen/rear_mirror.h"
#include "../hooks.h"

void __fastcall n_detoured_functions::render_view( void* ecx, void* edx, c_view_setup& setup, c_view_setup& hud_setup, int clear_flags,
                                                   int what_to_draw )
{
	static auto original = g_hooks.m_render_view.get_original< decltype( &n_detoured_functions::render_view ) >( );
	HOOK_SCOPE_OR_BAIL( original( ecx, edx, setup, hud_setup, clear_flags, what_to_draw ) );

	/* before the main view: w2s matrix, current view, glow + post state the main pass leaves stay its own */
	alignas( 16 ) std::uint8_t mirror[ n_rear_mirror::k_view_size ];
	if ( g_rear_mirror.begin( ecx, setup, mirror ) ) {
		c_view_setup& view = *reinterpret_cast< c_view_setup* >( mirror );
		original( ecx, edx, view, view, n_rear_mirror::k_clear_flags, n_rear_mirror::k_what_to_draw );
		g_rear_mirror.end( );
	}

	original( ecx, edx, setup, hud_setup, clear_flags, what_to_draw );
}
