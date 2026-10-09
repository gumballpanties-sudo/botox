#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"
#include "../hooks.h"

#include "../../hacks/aimbot/aimbot.h"
#include "../../hacks/animations/animations.h"
#include "../../hacks/debug/debug.h"
#include "../../hacks/entity_cache/entity_cache.h"
#include "../../hacks/misc/misc.h"
#include "../../hacks/movement/edgebug.h"
#include "../../hacks/movement/movement.h"
#include "../../hacks/movement/movement_recorder.h"
#include "../../hacks/skins/skins.h"
#include "../../hacks/chams/chams.h"
#include "../../hacks/visuals/bullets/bullets.h"
#include "../../hacks/visuals/edicts/edicts.h"
#include "../../hacks/visuals/players/sound_esp/sound_esp.h"
#include "../../hacks/visuals/screen/flip_world.h"
#include "../../hacks/visuals/screen/screen.h"
#include "../../hacks/web/websurface.h"
#include "../../utilities/perf/perf_watch.h"

void __fastcall n_detoured_functions::frame_stage_notify( void* ecx, void* edx, int stage )
{
	static auto original = g_hooks.m_frame_stage_notify.get_original< decltype( &n_detoured_functions::frame_stage_notify ) >( );

	/* chams / skins / smoke rebuild here: must not run while eject releases materials */
	HOOK_SCOPE_OR_BAIL( original( ecx, edx, stage ) );

	if ( stage == render_start )
		g_flip_world.on_render_start( );

	g_misc.on_frame_stage_notify( stage );

	g_aimbot.nospread_frame_stage( stage );

	g_movement_recorder.on_frame_stage( stage );

	g_skins.publish_anim_donor( );

	/* web browser tick, every frame from the main menu; main thread (steam html surface wants it) */
	if ( stage == e_client_frame_stage::start ) {
		Web_MainTick( );
		discord_rpc_frame( );
	}

	if ( !g_interfaces.m_engine_client->is_connected_safe( ) ) {
		/* killer replay rebuilds the entity list and create_move doesn't tick: drop the dangling m_local */
		g_ctx.m_local = nullptr;

		g_edicts.reset( );
		g_lagcomp.clear_incoming_sequences( );
		return original( ecx, edx, stage );
	}

	g_ctx.m_local = g_interfaces.m_client_entity_list->get< c_base_entity >( g_interfaces.m_engine_client->get_local_player( ) );
	if ( !g_ctx.m_local )
		return original( ecx, edx, stage );

	{
		PERF_ZONE( zone_frame_stage );

		g_edicts.on_frame_stage_notify( stage );
		g_screen.on_frame_stage_notify( stage );
		g_movement.on_frame_stage_notify( stage );
		g_animations.on_frame_stage_notify( stage );
		g_skins.on_frame_stage_notify( stage );
		g_bullets.on_frame_stage_notify( stage );
	}

	if ( stage == e_client_frame_stage::start )
		g_edgebug.donor_frame_start( );

	if ( stage == render_start ) {
		const bool sound_chams = GET_VARIABLE( g_variables.m_chams_enable, bool ) &&
		                         n_chams::impl_t::has_layers( GET_VARIABLE( g_variables.m_chams_sound_layers, std::vector< int > ) );

		const bool sound_arrows = GET_VARIABLE( g_variables.m_players, bool ) && GET_VARIABLE( g_variables.m_out_of_fov_arrows, bool ) &&
		                          GET_VARIABLE( g_variables.m_out_of_fov_arrows_type, int ) != 0;

		const bool sound_players = GET_VARIABLE( g_variables.m_players, bool ) && GET_VARIABLE( g_variables.m_players_sound_only, bool );

		if ( sound_chams || sound_arrows || sound_players )
			g_sound_esp.think( );
	}

#ifdef _DEBUG
	g_debugger.on_frame_stage_notify( stage );
#endif
	if ( stage == net_update_end ) {
		for ( auto events = g_interfaces.m_client_state->m_events; events; events = events->m_next ) {
			if ( events->m_class_id )
				break;

			events->m_fire_delay = 0.f;
		}

		g_interfaces.m_engine_client->fire_events( );

		[ & ]( ) {
#ifdef _DEBUG
			if ( !GET_VARIABLE( g_variables.m_disable_interp, bool ) )
				return;
#else
			return;
#endif

			if ( g_ctx.m_local->get_observer_mode( ) != e_obs_mode::obs_mode_none )
				return;

			g_entity_cache.enumerate( e_enumeration_type::type_players, [ & ]( c_base_entity* entity ) {
				if ( !entity->is_valid_enemy( ) )
					return;

				const auto var_map = entity->get_var_map( );
				if ( !var_map )
					return;

				if ( var_map->interpolated_entries > 20108 )
					return;

				for ( int i = 0; i < var_map->interpolated_entries; i++ )
					var_map->entries[ i ].needs_to_interpolate = false;
			} );
		}( );
	}

	/* records at render_start: at net_update_end bone access is still pushed off
	   and interpolation unresolved, so setup_bones returns stale matrices */
	if ( stage == render_start )
		g_lagcomp.on_frame_stage_notify( );

	if ( stage == render_start &&
	     ( ( GET_VARIABLE( g_variables.m_chams_enable, bool ) && GET_VARIABLE( g_variables.m_chams_ragdolls, bool ) ) ||
	       GET_VARIABLE( g_variables.m_remove_ragdolls, bool ) || kill_effects_wants_ragdoll_dme( ) ) ) {
		for ( int i = 1; i <= g_interfaces.m_client_entity_list->get_highest_entity_index( ); ++i ) {
			const auto entity = g_interfaces.m_client_entity_list->get< c_base_entity >( i );
			if ( !entity )
				continue;

			const auto client_class = static_cast< c_client_networkable* >( entity )->get_client_class( );
			if ( client_class && client_class->m_class_id == e_class_ids::ccs_ragdoll )
				entity->can_use_fast_path( ) = false;
		}
	}

	{
		static auto ragdoll_physics = g_interfaces.m_convar->find_var( "cl_ragdoll_physics_enable" );
		if ( ragdoll_physics ) {
			static bool forced = false;

			if ( const bool want = GET_VARIABLE( g_variables.m_remove_ragdolls, bool ); want && ragdoll_physics->get_int( ) != 0 ) {
				ragdoll_physics->set_value( static_cast< int >( 0 ) );
				forced = true;
			}
			else if ( !want && forced ) {
				ragdoll_physics->set_value( static_cast< int >( 1 ) );
				forced = false;
			}
		}
	}

	original( ecx, edx, stage );
}
