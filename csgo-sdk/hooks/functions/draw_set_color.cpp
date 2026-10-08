#include "../../globals/globals.h"
#include "../../globals/includes/includes.h"
#include "../../globals/interfaces/interfaces.h"
#include "../../utilities/console/console.h"
#include "../hooks.h"

/* DrawCrosshairRect (weapon_csbase.cpp:407): outline = DrawSetColor( 0, 0, 0, a ) right before the line's own color */
static void classic_crosshair_color( int& r, int& g, int& b, int& a )
{
	const bool outline_on = g_convars.int_or( HASH_BT( "cl_crosshair_drawoutline" ), 0 ) != 0;
	const bool is_outline = outline_on && g_ctx.m_crosshair_outline_next && !r && !g && !b;

	g_ctx.m_crosshair_outline_next = !is_outline;

	const bool want = is_outline ? GET_VARIABLE( g_variables.m_classic_crosshair_outline_color, bool )
	                             : GET_VARIABLE( g_variables.m_classic_crosshair_color, bool );
	if ( !want )
		return;

	const c_color color = is_outline ? GET_VARIABLE( g_variables.m_classic_crosshair_outline_color_value, c_color )
	                                 : GET_VARIABLE( g_variables.m_classic_crosshair_color_value, c_color );

	r = color[ color_type_r ];
	g = color[ color_type_g ];
	b = color[ color_type_b ];
	a = a * color[ color_type_a ] / 255;
}

void __stdcall n_detoured_functions::draw_set_color( int r, int g, int b, int a )
{
	static auto original = g_hooks.m_draw_set_color.get_original< void( __thiscall* )( c_surface*, int, int, int, int ) >( );
	HOOK_SCOPE_OR_BAIL( original( g_interfaces.m_surface, r, g, b, a ) );

	if ( g_ctx.m_is_console_being_drawn && GET_VARIABLE( g_variables.m_console_color_enable, bool ) ) {
		const c_color tint = GET_VARIABLE( g_variables.m_console_color, c_color );
		original( g_interfaces.m_surface, r * tint[ color_type_r ] / 255, g * tint[ color_type_g ] / 255, b * tint[ color_type_b ] / 255,
		          a * tint[ color_type_a ] / 255 );
		return;
	}

	if ( g_ctx.m_is_hud_weapon_being_drawn )
		classic_crosshair_color( r, g, b, a );

	original( g_interfaces.m_surface, r, g, b, a );
}
