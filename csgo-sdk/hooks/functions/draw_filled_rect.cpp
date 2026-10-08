#include "../../globals/includes/includes.h"
#include "../../game/sdk/classes/c_global_vars_base.h"
#include "../../hacks/visuals/screen/resolution_spoof.h"
#include "../hooks.h"

#include <algorithm>
#include <cmath>

static bool crosshair_rect_gate( const int x0, const int y0, const int x1, const int y1 )
{
	const int screen_width  = g_resolution_spoof.m_screen_width;
	const int screen_height = g_resolution_spoof.m_screen_height;

	if ( screen_width <= 0 || screen_height <= 0 )
		return false;

	const int width  = abs( x1 - x0 );
	const int height = abs( y1 - y0 );

	/* thin on one axis, never large, near centre (gap + length can reach 256) */
	if ( width > 256 || height > 256 || ( width > 32 && height > 32 ) )
		return false;

	return fabsf( ( x0 + x1 ) * 0.5f - screen_width * 0.5f ) <= 256.f &&
	       fabsf( ( y0 + y1 ) * 0.5f - screen_height * 0.5f ) <= 256.f;
}

static void snap_span( int& low, int& high, const float grid )
{
	if ( grid <= 1.001f )
		return;

	int snapped_low  = static_cast< int >( roundf( floorf( low / grid ) * grid ) );
	int snapped_high = static_cast< int >( roundf( ceilf( high / grid ) * grid ) );

	if ( snapped_high <= snapped_low )
		snapped_high = static_cast< int >( roundf( snapped_low + grid ) );

	low  = snapped_low;
	high = snapped_high;
}

/* CHudScope::Paint draws for a square screen; widen x round centre like a real stretched res */
static float scope_scale( )
{
	if ( !g_ctx.m_is_scope_being_drawn || !GET_VARIABLE( g_variables.m_aspect_ratio_hud, bool ) || g_resolution_spoof.m_screen_width <= 0 )
		return 1.f;

	return g_resolution_spoof.aspect_scale( );
}

static float scope_x( const float x, const float scale )
{
	const float center_x = g_resolution_spoof.m_screen_width * 0.5f;
	return center_x + ( x - center_x ) * scale;
}

static bool stretch_scope_x( int& x0, int& x1, const bool rect = false )
{
	const float scale = scope_scale( );
	if ( fabsf( scale - 1.f ) < 0.001f )
		return g_ctx.m_is_scope_being_drawn;

	const float a = scope_x( static_cast< float >( std::min( x0, x1 ) ), scale );
	const float b = scope_x( static_cast< float >( std::max( x0, x1 ) ), scale );

	/* black bars end on a float arc edge now: grow 1 px past it or the world shows as a seam */
	if ( rect && b - a > 64.f ) {
		x0 = static_cast< int >( floorf( a ) ) - 1;
		x1 = static_cast< int >( ceilf( b ) ) + 1;
		return true;
	}

	x0 = static_cast< int >( roundf( scope_x( static_cast< float >( x0 ), scale ) ) );
	x1 = static_cast< int >( roundf( scope_x( static_cast< float >( x1 ), scale ) ) );
	return true;
}

static void stretch_crosshair_rect( int& x0, int& y0, int& x1, int& y1 )
{
	if ( stretch_scope_x( x0, x1, true ) )
		return;

	if ( !crosshair_rect_gate( x0, y0, x1, y1 ) )
		return;

	const float scale = g_resolution_spoof.overlay_aspect_scale( );

	if ( fabsf( scale - 1.f ) >= 0.001f ) {
		const float center_x = g_resolution_spoof.m_screen_width * 0.5f;

		x0 = static_cast< int >( roundf( center_x + ( x0 - center_x ) * scale ) );
		x1 = static_cast< int >( roundf( center_x + ( x1 - center_x ) * scale ) );

		if ( x1 == x0 )
			++x1;
	}

	float grid_x = 0.f, grid_y = 0.f;

	if ( g_resolution_spoof.low_res_grid( grid_x, grid_y ) ) {
		int low_x = std::min( x0, x1 ), high_x = std::max( x0, x1 );
		int low_y = std::min( y0, y1 ), high_y = std::max( y0, y1 );

		snap_span( low_x, high_x, grid_x );
		snap_span( low_y, high_y, grid_y );

		x0 = low_x;
		x1 = high_x;
		y0 = low_y;
		y1 = high_y;
	}
}

void __stdcall n_detoured_functions::draw_filled_rect( int x0, int y0, int x1, int y1 )
{
	static auto original = g_hooks.m_draw_filled_rect.get_original< void( __thiscall* )( c_surface*, int, int, int, int ) >( );
	HOOK_SCOPE_OR_BAIL( original( g_interfaces.m_surface, x0, y0, x1, y1 ) );

	stretch_crosshair_rect( x0, y0, x1, y1 );

	original( g_interfaces.m_surface, x0, y0, x1, y1 );
}

void __stdcall n_detoured_functions::draw_textured_rect( int x0, int y0, int x1, int y1 )
{
	static auto original = g_hooks.m_draw_textured_rect.get_original< void( __thiscall* )( c_surface*, int, int, int, int ) >( );
	HOOK_SCOPE_OR_BAIL( original( g_interfaces.m_surface, x0, y0, x1, y1 ) );

	stretch_crosshair_rect( x0, y0, x1, y1 );

	original( g_interfaces.m_surface, x0, y0, x1, y1 );
}

void __stdcall n_detoured_functions::draw_line( int x0, int y0, int x1, int y1 )
{
	static auto original = g_hooks.m_draw_line.get_original< void( __thiscall* )( c_surface*, int, int, int, int ) >( );
	HOOK_SCOPE_OR_BAIL( original( g_interfaces.m_surface, x0, y0, x1, y1 ) );

	stretch_scope_x( x0, x1 );

	original( g_interfaces.m_surface, x0, y0, x1, y1 );
}

void __fastcall n_detoured_functions::draw_textured_polygon( void* ecx, void* edx, int count, float* vertices, bool clip_vertices )
{
	static auto original = g_hooks.m_draw_textured_polygon.get_original< decltype( &n_detoured_functions::draw_textured_polygon ) >( );
	HOOK_SCOPE_OR_BAIL( original( ecx, edx, count, vertices, clip_vertices ) );

	/* vgui Vertex_t = position xy + texcoord uv */
	float stretched[ 8 * 4 ];
	const float scale = scope_scale( );

	if ( fabsf( scale - 1.f ) < 0.001f || !vertices || count <= 0 || count > 8 )
		return original( ecx, edx, count, vertices, clip_vertices );

	for ( int i = 0; i < count * 4; ++i )
		stretched[ i ] = vertices[ i ];

	for ( int i = 0; i < count; ++i )
		stretched[ i * 4 ] = scope_x( stretched[ i * 4 ], scale );

	original( ecx, edx, count, stretched, clip_vertices );
}
