#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"
#include "../../hacks/visuals/edicts/edicts.h"
#include "../hooks.h"

void __fastcall n_detoured_functions::draw_static_props( void* ecx, void* edx, void** props, const void* instances, int count, bool shadow_depth,
                                                         bool ssao_depth, bool wireframe )
{
	static auto original = g_hooks.m_draw_static_props.get_original< decltype( &n_detoured_functions::draw_static_props ) >( );
	HOOK_SCOPE_OR_BAIL( original( ecx, edx, props, instances, count, shadow_depth, ssao_depth, wireframe ) );

	if ( props && count > 0 && !shadow_depth && !ssao_depth && GET_VARIABLE( g_variables.m_lighting_modulation, bool ) &&
	     GET_VARIABLE( g_variables.m_lighting_modulation_lamps, bool ) )
		g_edicts.record_lamps( props, count );

	original( ecx, edx, props, instances, count, shadow_depth, ssao_depth, wireframe );
}
