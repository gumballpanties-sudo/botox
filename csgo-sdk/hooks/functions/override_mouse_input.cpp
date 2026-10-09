#include "../../game/sdk/classes/c_engine_client.h"
#include "../../game/sdk/classes/c_global_vars_base.h"
#include "../../globals/includes/includes.h"
#include "../../hacks/movement/edge_skip.h"
#include "../../hacks/movement/edgebug.h"
#include "../../hacks/movement/movement.h"
#include "../../hacks/movement/movement_recorder.h"
#include "../../hacks/prediction/prediction.h"
#include "../../hacks/visuals/screen/flip_world.h"
#include "../hooks.h"

extern void botox_dbg_log( const char* fmt, ... );

void __fastcall n_detoured_functions::override_mouse_input( void* thisptr, int edx, float* x, float* y )
{
	static auto original = g_hooks.m_override_mouse_input.get_original< decltype( &n_detoured_functions::override_mouse_input ) >( );
	HOOK_SCOPE_OR_BAIL( original( thisptr, edx, x, y ) );

	if ( !g_interfaces.m_engine_client->is_in_game( ) )
		return original( thisptr, edx, x, y );

	// before ApplyMouse: yaw and cmd mousedx both follow the mirrored screen
	g_flip_world.on_mouse_input( x );

	if ( x )
		g_edgebug.add_mouse_x( *x );
	const bool eb_live      = GET_VARIABLE( g_variables.m_edgebug_style, int ) == 1 ? g_edgebug.donor_live( ) : g_edgebug.m_found;
	const bool edgebug_lock = eb_live && GET_VARIABLE( g_variables.m_edgebug_mouse_lock, bool );
	if ( edgebug_lock && x ) {
		const float strength = std::clamp( GET_VARIABLE( g_variables.m_edgebug_mouse_lock_strength, float ), 0.f, 100.f ) / 100.f;
		const int lock_type  = GET_VARIABLE( g_variables.m_edgebug_mouse_lock_type, int );
		if ( const int style = GET_VARIABLE( g_variables.m_edgebug_style, int ); style != 0 ) {
			const float keep = lock_type == 1 ? 0.f : 1.f - strength;
			*x *= keep;
			if ( style == 2 && lock_type == 0 && y )
				*y *= keep;
		} else
			*x *= 1.f - strength * ( lock_type == 0 ? g_edgebug.lock_progress( ) : 1.f );
	}
	g_edgebug.log_lock( edgebug_lock );

	const float skip_lock = g_edge_skip.lock_amount( );
	if ( skip_lock > 0.f && x )
		*x *= 1.f - skip_lock;
	if ( static bool skip_was = false; ( skip_lock > 0.f ) != skip_was ) {
		skip_was = !skip_was;
		botox_dbg_log( "ES: lock %s amount=%.2f\n", skip_was ? "on" : "off", skip_lock );
	}

	/* its pull would turn a locked camera; skipped, the owed yaw goes stale and drops */
	if ( !edgebug_lock && skip_lock <= 0.f )
		g_movement.strafe_optimizer_mouse( x );
	g_movement.auto_strafe_mouse( x, y );

	g_movement_recorder.camera_lock( x, y );

	original( thisptr, edx, x, y );
}
