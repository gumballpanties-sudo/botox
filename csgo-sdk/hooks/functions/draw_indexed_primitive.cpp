#include "../../globals/includes/includes.h"
#include "../../hacks/visuals/lsd_chams.h"
#include "../../hacks/visuals/two_tone_chams.h"
#include "../../hacks/visuals/weapon_sheen.h"
#include "../hooks.h"

long __stdcall n_detoured_functions::draw_indexed_primitive( IDirect3DDevice9* device, D3DPRIMITIVETYPE type, int base_vertex, unsigned int min_vertex,
                                                             unsigned int vertices, unsigned int start_index, unsigned int primitives )
{
	static auto original = g_hooks.m_draw_indexed_primitive.get_original< decltype( &n_detoured_functions::draw_indexed_primitive ) >( );

	// every draw in the game lands here: no scope ticket unless armed
	const bool two_tone = g_two_tone_chams.armed( );
	const bool lsd      = g_lsd_chams.armed( );
	if ( !two_tone && !lsd && !g_weapon_sheen.armed( ) )
		return original( device, type, base_vertex, min_vertex, vertices, start_index, primitives );

	HOOK_SCOPE_OR_BAIL( original( device, type, base_vertex, min_vertex, vertices, start_index, primitives ) );

	if ( two_tone )
		return g_two_tone_chams.draw( original, device, type, base_vertex, min_vertex, vertices, start_index, primitives );

	if ( lsd )
		return g_lsd_chams.draw( original, device, type, base_vertex, min_vertex, vertices, start_index, primitives );

	return g_weapon_sheen.draw( original, device, type, base_vertex, min_vertex, vertices, start_index, primitives );
}
