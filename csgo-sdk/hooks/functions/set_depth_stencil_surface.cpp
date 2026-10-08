#include "../../globals/includes/includes.h"
#include "../../hacks/visuals/screen/depth_source.h"
#include "../hooks.h"

long __stdcall n_detoured_functions::set_depth_stencil_surface( IDirect3DDevice9* device, IDirect3DSurface9* surface )
{
	static auto original = g_hooks.m_set_depth_stencil_surface.get_original< decltype( &n_detoured_functions::set_depth_stencil_surface ) >( );

	HOOK_SCOPE_OR_BAIL( original( device, surface ) );

	return original( device, g_depth_source.map_depth_stencil( surface ) );
}
