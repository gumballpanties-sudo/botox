#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"
#include "../../hacks/visuals/screen/flip_world.h"
#include "../hooks.h"

void __fastcall n_detoured_functions::push_2d_view( void* ecx, void* edx, void* render_context, const c_view_setup& view, int flags,
                                                    void* render_target, void* frustum )
{
	static auto original = g_hooks.m_push_2d_view.get_original< decltype( &n_detoured_functions::push_2d_view ) >( );

	HOOK_SCOPE_OR_BAIL( original( ecx, edx, render_context, view, flags, render_target, frustum ) );

	g_flip_world.on_push_2d_view( );

	original( ecx, edx, render_context, view, flags, render_target, frustum );
}
