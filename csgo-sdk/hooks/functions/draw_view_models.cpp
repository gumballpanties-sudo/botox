#include "../../globals/globals.h"
#include "../../hacks/visuals/screen/screen.h"
#include "../hooks.h"

void __fastcall n_detoured_functions::draw_view_models( void* ecx, void* edx, c_view_setup& setup, bool draw_view_model, bool draw_scope_lens_mask )
{
	static auto original = g_hooks.m_draw_view_models.get_original< decltype( &n_detoured_functions::draw_view_models ) >( );
	HOOK_SCOPE_OR_BAIL( original( ecx, edx, setup, draw_view_model, draw_scope_lens_mask ) );

	g_screen.on_draw_view_models( setup );

	original( ecx, edx, setup, draw_view_model, draw_scope_lens_mask );
}
