#include "../../game/sdk/includes/includes.h"
#include "../hooks.h"

#include "../../dependencies/imgui/imgui.h"
#include "../../globals/config/variables.h"
#include "../../globals/globals.h"
#include "../../globals/interfaces/interfaces.h"
#include "../../hacks/chams/chams.h"
#include "../../hacks/lagcomp/lagcomp.h"
#include "../../hacks/misc/misc.h"
#include "../../hacks/visuals/players/sound_esp/sound_esp.h"
#include "../../hacks/visuals/viewmodel_bones.h"
#include "../../hacks/visuals/weapon_sheen.h"
#include "../../utilities/console/console.h"
#include <algorithm>
#include <string>
#include <vector>

static bool is_body_model( const char* name )
{
	if ( !name )
		return false;

	// not ragdolls, must keep drawing
	if ( strstr( name, "player/contactshadow" ) || strstr( name, "hostage_carry" ) )
		return false;

	return strstr( name, "models/player" ) || strstr( name, "/hostage" ) || strstr( name, "/chicken" ) || strstr( name, "ragdoll" );
}

static c_base_entity* cs_ragdoll( const int entity_index )
{
	if ( entity_index <= 0 )
		return nullptr;

	const auto entity = g_interfaces.m_client_entity_list->get< c_base_entity >( entity_index );
	if ( !entity )
		return nullptr;

	const auto client_class = static_cast< c_client_networkable* >( entity )->get_client_class( );
	return client_class && client_class->m_class_id == e_class_ids::ccs_ragdoll ? entity : nullptr;
}

static bool is_enemy_ragdoll( const int entity_index )
{
	const auto ragdoll = cs_ragdoll( entity_index );
	if ( !ragdoll )
		return false;

	const auto owner = reinterpret_cast< c_base_entity* >(
		g_interfaces.m_client_entity_list->get_client_entity_from_handle( ragdoll->get_ragdoll_player_handle( ) ) );

	return owner && owner != g_ctx.m_local && owner->is_player( ) && owner->is_enemy( g_ctx.m_local );
}

static bool should_skip_ragdoll( const model_render_info_t& info )
{
	if ( !info.model )
		return false;

	if ( info.entity_index > 0 )
		return cs_ragdoll( info.entity_index ) != nullptr;

	return is_body_model( info.model->m_name );
}

static constexpr int studio_depth_pass_flags = 0x08000000  | 0x20000000
                                             | 0x40000000 ;

enum e_viewmodel_part : int {
	viewmodel_part_none = -1,
	viewmodel_part_arms = 0,
	viewmodel_part_sleeve,
	viewmodel_part_weapon
};

static int viewmodel_part( const char* name )
{
	if ( strstr( name, "v_models/arms/" ) )
		return ( strstr( name, "sleeve" ) || strstr( name, "watch" ) ) ? viewmodel_part_sleeve : viewmodel_part_arms;

	if ( strstr( name, "weapons/v_" ) )
		return viewmodel_part_weapon;

	return viewmodel_part_none;
}

void __fastcall n_detoured_functions::draw_model_execute( void* ecx, void* edx, void* context, void* state, model_render_info_t& info,
                                                          matrix3x4_t* custom_bone_to_world )
{
	static auto original = g_hooks.m_draw_model_execute.get_original< decltype( &n_detoured_functions::draw_model_execute ) >( );

	// eject frees chams materials; drawing mid release crashes
	HOOK_SCOPE_OR_BAIL( original( ecx, edx, context, state, info, custom_bone_to_world ) );

	if ( ( GET_VARIABLE( g_variables.m_remove_ragdolls, bool ) && should_skip_ragdoll( info ) ) || kill_effects_hides_ragdoll( info.entity_index ) )
		return;

	/* viewmodel bone velocity for true motion blur. destructor: bones must be read after whichever
	   return path drew the model. skipped on depth-only passes and inside glow */
	struct viewmodel_bones_t {
		~viewmodel_bones_t( )
		{
			g_viewmodel_bones.collect( m_state, m_info, m_bones, m_allowed );
		}

		void* m_state;
		model_render_info_t& m_info;
		matrix3x4_t* m_bones;
		bool m_allowed;
	} const viewmodel_bones{ state, info, custom_bone_to_world,
		                     !( info.flags & studio_depth_pass_flags ) && !g_ctx.m_is_glow_being_drawn };

	// destructor: sheen redraw goes on top of whichever path drew the weapon
	struct weapon_sheen_t {
		~weapon_sheen_t( )
		{
			if ( m_model && g_weapon_sheen.begin( m_model ) ) {
				m_original( m_ecx, m_edx, m_context, m_state, m_info, m_bones );
				g_weapon_sheen.end( );
			}
		}

		decltype( original ) m_original;
		void* m_ecx;
		void* m_edx;
		void* m_context;
		void* m_state;
		model_render_info_t& m_info;
		matrix3x4_t* m_bones;
		const char* m_model;
	} const weapon_sheen{ original, ecx, edx, context, state, info, custom_bone_to_world,
		                  !( info.flags & studio_depth_pass_flags ) && !g_ctx.m_is_glow_being_drawn && info.model && info.model->m_name &&
		                          viewmodel_part( info.model->m_name ) == viewmodel_part_weapon
		                      ? info.model->m_name
		                      : nullptr };

	const bool enemy_chams = GET_VARIABLE( g_variables.m_chams_enable, bool );
	const bool local_chams = GET_VARIABLE( g_variables.m_chams_local_enable, bool );

	if ( ( !enemy_chams && !local_chams ) || !context || !custom_bone_to_world || !info.model ||
	     ( info.flags & studio_depth_pass_flags ) || g_ctx.m_is_glow_being_drawn || !g_ctx.m_local ||
	     !g_interfaces.m_engine_client->is_connected_safe( ) )
		return original( ecx, edx, context, state, info, custom_bone_to_world );

	const char* const model_name = info.model->m_name;
	if ( !model_name )
		return original( ecx, edx, context, state, info, custom_bone_to_world );

	const bool enemy_ragdoll = enemy_chams && GET_VARIABLE( g_variables.m_chams_ragdolls, bool ) && is_enemy_ragdoll( info.entity_index );

	if ( !enemy_ragdoll && ( info.entity_index <= 0 || info.entity_index >= 64 ) ) {
		const int part = viewmodel_part( model_name );
		if ( !local_chams || part == viewmodel_part_none )
			return original( ecx, edx, context, state, info, custom_bone_to_world );

		std::uint32_t layers_variable = g_variables.m_chams_weapon_layers;
		std::uint32_t colors_variable = g_variables.m_chams_weapon_colors;

		if ( part == viewmodel_part_arms ) {
			layers_variable = g_variables.m_chams_arms_layers;
			colors_variable = g_variables.m_chams_arms_colors;
		} else if ( part == viewmodel_part_sleeve ) {
			layers_variable = g_variables.m_chams_sleeve_layers;
			colors_variable = g_variables.m_chams_sleeve_colors;
		}

		const auto& layers = GET_VARIABLE( layers_variable, std::vector< int > );
		const auto& colors = GET_VARIABLE( colors_variable, std::vector< c_color > );

		if ( !n_chams::impl_t::has_layers( layers ) )
			return original( ecx, edx, context, state, info, custom_bone_to_world );

		g_chams.draw_layers( layers, colors, false, 1.f, [ & ]( ) {
			original( ecx, edx, context, state, info, custom_bone_to_world );
		} );

		g_interfaces.m_model_render->forced_material_override( nullptr );
		n_chams::impl_t::reset_original_color( );
		return;
	}

	if ( !enemy_chams || !strstr( model_name, "player" ) || strstr( model_name, "player/contactshadow" ) ||
	     g_interfaces.m_model_render->is_forced_material_override( ) )
		return original( ecx, edx, context, state, info, custom_bone_to_world );

	const auto& visible_layers   = GET_VARIABLE( g_variables.m_chams_visible_layers, std::vector< int > );
	const auto& visible_colors   = GET_VARIABLE( g_variables.m_chams_visible_colors, std::vector< c_color > );
	const auto& occluded_layers  = GET_VARIABLE( g_variables.m_chams_occluded_layers, std::vector< int > );
	const auto& occluded_colors  = GET_VARIABLE( g_variables.m_chams_occluded_colors, std::vector< c_color > );
	const auto& backtrack_layers = GET_VARIABLE( g_variables.m_chams_backtrack_layers, std::vector< int > );
	const auto& backtrack_colors = GET_VARIABLE( g_variables.m_chams_backtrack_colors, std::vector< c_color > );
	const auto& sound_layers     = GET_VARIABLE( g_variables.m_chams_sound_layers, std::vector< int > );
	const auto& sound_colors     = GET_VARIABLE( g_variables.m_chams_sound_colors, std::vector< c_color > );

	const bool draw_visible   = n_chams::impl_t::has_layers( visible_layers );
	const bool draw_occluded  = n_chams::impl_t::has_layers( occluded_layers );
	const bool draw_backtrack = !enemy_ragdoll && n_chams::impl_t::has_layers( backtrack_layers );
	const bool draw_sound     = !enemy_ragdoll && n_chams::impl_t::has_layers( sound_layers );

	if ( !draw_visible && !draw_occluded && !draw_backtrack && !draw_sound )
		return original( ecx, edx, context, state, info, custom_bone_to_world );

	const auto player = g_interfaces.m_client_entity_list->get< c_base_entity >( info.entity_index );
	if ( !enemy_ragdoll && ( !player || !player->is_valid_enemy( ) ) )
		return original( ecx, edx, context, state, info, custom_bone_to_world );

	const auto draw_live = [ & ]( ) {
		original( ecx, edx, context, state, info, custom_bone_to_world );
	};

	const float esp_gate = enemy_ragdoll ? 1.f : g_sound_esp.player_gate( player );

	if ( draw_sound ) {
		const float sound_alpha = g_sound_esp.alpha( info.entity_index, GET_VARIABLE( g_variables.m_chams_sound_duration, float ),
		                                             GET_VARIABLE( g_variables.m_chams_sound_fade, bool ) );

		if ( sound_alpha > 0.f )
			g_chams.draw_layers( sound_layers, sound_colors, true, sound_alpha, draw_live );
	}

	if ( draw_backtrack ) {
		const int backtrack_type = GET_VARIABLE( g_variables.m_chams_backtrack_type, int );

		/* not const: m_matrix must decay to a mutable matrix3x4_t*. skipped in aimbot-record mode */
		auto* oldest_record = backtrack_type == 2 ? nullptr : g_lagcomp.oldest_record( info.entity_index );
		const bool xqz      = GET_VARIABLE( g_variables.m_chams_backtrack_xqz, bool );
		const unsigned int anim_key = HASH_RT( model_name ) + static_cast< unsigned int >( info.entity_index ) * 0x9E3779B9U;

		const bool gradient = GET_VARIABLE( g_variables.m_chams_backtrack_gradient, bool );

		/* records are network pose, model is interpolated behind it: newer ones would draw in front */
		const float model_time = g_lagcomp.model_time( );

		const auto draw_record = [ & ]( matrix3x4_t* record_matrix, float alpha_scale ) {
			alpha_scale *= esp_gate;

			if ( !record_matrix || alpha_scale <= 0.f )
				return;

			g_chams.draw_layers( backtrack_layers, backtrack_colors, xqz, alpha_scale, [ & ]( ) {
				original( ecx, edx, context, state, info, record_matrix );
			} );
		};

		switch ( backtrack_type ) {
		case 0:
			if ( oldest_record && oldest_record->m_valid && oldest_record->m_sim_time <= model_time ) {
				const float distance = oldest_record->m_vec_origin.dist_to_squared( player->get_abs_origin( ) );

				ImAnimationHelper alpha_animation = ImAnimationHelper( anim_key, ImGui::GetIO( ).DeltaTime );
				alpha_animation.Update( 2.f, distance > 9.f ? 2.f : -2.f );

				draw_record( oldest_record->m_matrix, alpha_animation.AnimationData->second );
			}
			break;
		case 1:
			if ( oldest_record && oldest_record->m_valid ) {
				const float distance = oldest_record->m_vec_origin.dist_to_squared( player->get_abs_origin( ) );

				const int capacity = std::clamp( g_ctx.m_max_allocations, 0, n_lagcomp::impl_t::max_records );

				if ( const auto record_list = g_lagcomp.m_records[ info.entity_index ]; record_list && capacity > 0 ) {
					ImAnimationHelper alpha_animation = ImAnimationHelper( anim_key, ImGui::GetIO( ).DeltaTime );
					alpha_animation.Update( 2.f, distance > 9.f ? 2.f : -2.f );

					n_lagcomp::impl_t::record_t* ranked[ n_lagcomp::impl_t::max_records ]{ };
					int record_count = 0;
					bool ordered     = true;

					for ( int step = 1; step <= capacity; step++ ) {
						const int slot = ( g_lagcomp.m_record_location[ info.entity_index ] - step + capacity * 2 ) % capacity;

						if ( !record_list[ slot ].m_valid || record_list[ slot ].m_sim_time > model_time )
							continue;

						if ( record_count > 0 && record_list[ slot ].m_sim_time > ranked[ record_count - 1 ]->m_sim_time )
							ordered = false;

						ranked[ record_count++ ] = &record_list[ slot ];
					}

					if ( !ordered )
						std::sort( ranked, ranked + record_count,
						           []( const n_lagcomp::impl_t::record_t* a, const n_lagcomp::impl_t::record_t* b ) {
							           return a->m_sim_time > b->m_sim_time;
						           } );

					/* ghost cap thins, never shortens: newest + oldest always, rest evenly spaced */
					const int max_ghosts =
						std::clamp( GET_VARIABLE( g_variables.m_chams_backtrack_max_ghosts, int ), 2, n_lagcomp::impl_t::max_records );
					const int drawn_count = record_count < max_ghosts ? record_count : max_ghosts;

					for ( int pick = drawn_count - 1; pick >= 0; pick-- ) {
						const int i = drawn_count > 1 ? static_cast< int >( static_cast< float >( pick ) *
						                                                        static_cast< float >( record_count - 1 ) /
						                                                        static_cast< float >( drawn_count - 1 ) +
						                                                    0.5f )
						                              : 0;

						float tick_scale = 1.f;

						if ( gradient && record_count > 1 )
							tick_scale = 1.f - static_cast< float >( i ) / static_cast< float >( record_count - 1 );

						draw_record( ranked[ i ]->m_matrix, alpha_animation.AnimationData->second * tick_scale );
					}
				}
			}
			break;
		case 2:
			if ( g_ctx.m_record && g_ctx.m_cmd ) {
				ImAnimationHelper alpha_animation = ImAnimationHelper( anim_key, ImGui::GetIO( ).DeltaTime );
				alpha_animation.Update( 2.f, g_ctx.m_cmd->m_buttons & in_attack ? 2.f : -2.f );

				draw_record( g_ctx.m_record->m_matrix, alpha_animation.AnimationData->second );
			}
			break;
		default:
			break;
		}
	}

	if ( draw_occluded && esp_gate > 0.f )
		g_chams.draw_layers( occluded_layers, occluded_colors, true, esp_gate, draw_live );

	if ( draw_visible && esp_gate > 0.f )
		g_chams.draw_layers( visible_layers, visible_colors, false, 1.f, draw_live );
	else
		draw_live( );

	g_interfaces.m_model_render->forced_material_override( nullptr );

	n_chams::impl_t::reset_original_color( );
}
