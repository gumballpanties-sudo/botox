#include "../../game/sdk/includes/includes.h"
#include "../../game/sdk/classes/c_angle.h"
#include "../../globals/math/math.h"
#include "../hooks.h"

#include "../../dependencies/imgui/imgui.h"
#include "../../globals/config/variables.h"
#include "../../globals/globals.h"
#include "../../globals/interfaces/interfaces.h"
#include "../../hacks/chams/chams.h"
#include "../../hacks/chams/shot_chams.h"
#include "../../hacks/lagcomp/lagcomp.h"
#include "../../hacks/lua/lua.h"
#include "../../hacks/lua/lua_api.h"
#include "../../hacks/misc/misc.h"
#include "../../hacks/visuals/players/sound_esp/sound_esp.h"
#include "../../hacks/visuals/screen/rear_mirror.h"
#include "../../hacks/visuals/viewmodel_bones.h"
#include "../../hacks/visuals/viewmodel_fps.h"
#include "../../hacks/visuals/viewmodel_trail.h"
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

static int shot_owner( const int entity_index )
{
	if ( entity_index >= 1 && entity_index <= 64 )
		return entity_index;

	const auto ragdoll = cs_ragdoll( entity_index );
	if ( !ragdoll )
		return 0;

	const auto owner = reinterpret_cast< c_base_entity* >(
		g_interfaces.m_client_entity_list->get_client_entity_from_handle( ragdoll->get_ragdoll_player_handle( ) ) );
	return owner && owner->is_player( ) ? static_cast< int >( owner->get_index( ) ) : 0;
}

/* RenderGlowEffects redraws glowers under pMatGlowColor ( type 0 ) + stencil refs 1 / 2. type 3 = skin composites, real draw */
static bool game_forced_material( )
{
	const auto& override_state = g_interfaces.m_studio_render->material_override( );
	return override_state.m_materials[ 0 ] && override_state.m_type != 3;
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

/* DrawModelState_t, Kisak public/engine/ivmodelrender.h:40 */
struct draw_model_state_t {
	const void* m_studio_hdr;
	const void* m_studio_hw_data;
	void* m_renderable;
	const matrix3x4_t* m_model_to_world;
	void* m_decals;
	int m_draw_flags;
	int m_lod;
};

/* scripts see each draw once: a hide returns, a tint / swap redraws through this hook ( chams, blur, ragdolls still apply )
   with s_lua_redraw up so the scripts skip it. glow silhouettes + the mirror pass keep the real model */
static bool s_lua_redraw = false;

static bool lua_draw_model( void* ecx, void* edx, void* context, void* state, model_render_info_t& info, matrix3x4_t* bones )
{
	if ( s_lua_redraw || !state || !info.model || g_ctx.m_is_glow_being_drawn || g_rear_mirror.drawing( ) || !g_lua.draw_model_wanted( ) )
		return false;

	n_lua::draw_model_t draw{ };
	draw.m_name   = info.model->m_name;
	draw.m_index  = info.entity_index;
	draw.m_flags  = info.flags;
	draw.m_origin = &info.origin.m_x;
	draw.m_info   = &info;
	draw.m_state  = state;
	g_lua.on_draw_model( draw );

	if ( draw.m_hide )
		return true;

	const bool swap = draw.m_swap_model && bones;

	/* model:animate: the drawn ( or swapped ) mesh on the script's pose, root placed where the entity draws */
	matrix3x4_t* anim_bones = nullptr;
	if ( draw.m_anim_root && bones ) {
		const auto* const drawn_state = static_cast< const draw_model_state_t* >( state );
		const auto* const target      = static_cast< const studiohdr_t* >( swap ? draw.m_swap_hdr : drawn_state->m_studio_hdr );

		matrix3x4_t to_world{ };
		if ( drawn_state->m_model_to_world )
			to_world = *drawn_state->m_model_to_world;
		else {
			g_math.angle_matrix( c_angle( info.angles.m_x, info.angles.m_y, info.angles.m_z ), to_world );
			to_world.set_origin( info.origin );
		}

		anim_bones = const_cast< matrix3x4_t* >( n_lua_api::animate_bones( draw, target, to_world ) );
	}

	if ( !draw.m_tint && !swap && !anim_bones )
		return false;

	matrix3x4_t* const draw_bones = anim_bones ? anim_bones : bones;

	float color[ 3 ]{ };
	g_interfaces.m_render_view->get_color_modulation( color );
	const float blend = g_interfaces.m_render_view->get_blend( );

	if ( draw.m_tint ) {
		g_interfaces.m_render_view->set_color_modulation( ( draw.m_color & 0xFF ) / 255.f, ( ( draw.m_color >> 8 ) & 0xFF ) / 255.f,
		                                                  ( ( draw.m_color >> 16 ) & 0xFF ) / 255.f );
		g_interfaces.m_render_view->set_blend( blend * ( ( draw.m_color >> 24 ) & 0xFF ) / 255.f );
	}

	s_lua_redraw = true;
	if ( swap ) {
		/* decals are vertex indices into the drawn mesh, lod is re-clamped by studiorender from the new hw data */
		draw_model_state_t swap_state = *static_cast< const draw_model_state_t* >( state );
		swap_state.m_studio_hdr       = draw.m_swap_hdr;
		swap_state.m_studio_hw_data   = draw.m_swap_hw;
		swap_state.m_decals           = nullptr;

		model_render_info_t swap_info = info;
		swap_info.model               = const_cast< model_t* >( static_cast< const model_t* >( draw.m_swap_model ) );
		n_detoured_functions::draw_model_execute( ecx, edx, context, &swap_state, swap_info, draw_bones );
	} else
		n_detoured_functions::draw_model_execute( ecx, edx, context, state, info, draw_bones );
	s_lua_redraw = false;

	g_interfaces.m_render_view->set_color_modulation( color );
	g_interfaces.m_render_view->set_blend( blend );
	return true;
}

void __fastcall n_detoured_functions::draw_model_execute( void* ecx, void* edx, void* context, void* state, model_render_info_t& info,
                                                          matrix3x4_t* custom_bone_to_world )
{
	static auto original = g_hooks.m_draw_model_execute.get_original< decltype( &n_detoured_functions::draw_model_execute ) >( );

	// eject frees chams materials; drawing mid release crashes
	HOOK_SCOPE_OR_BAIL( original( ecx, edx, context, state, info, custom_bone_to_world ) );

	if ( lua_draw_model( ecx, edx, context, state, info, custom_bone_to_world ) )
		return;

	// first: chams, sheen + motion blur bones all see the held pose
	custom_bone_to_world = g_viewmodel_fps.apply( state, custom_bone_to_world, info.model );

	/* destructor: snapshot draws after whichever path drew the player / its ragdoll, hidden ragdoll included.
	   not in a lua redraw: a swapped mesh has another skeleton. not in glow: snapshot would write glow rt + stencil */
	struct shot_chams_t {
		~shot_chams_t( )
		{
			if ( m_owner > 0 )
				g_shot_chams.draw( m_owner, [ this ]( matrix3x4_t* bones ) { m_original( m_ecx, m_edx, m_context, m_state, m_info, bones ); } );
		}

		decltype( original ) m_original;
		void* m_ecx;
		void* m_edx;
		void* m_context;
		void* m_state;
		model_render_info_t& m_info;
		int m_owner;
	} const shot_chams{ original, ecx, edx, context, state, info,
		                !s_lua_redraw && !g_rear_mirror.drawing( ) && GET_VARIABLE( g_variables.m_chams_enable, bool ) && context && state &&
		                        info.model && info.model->m_name && !( info.flags & studio_depth_pass_flags ) && !g_ctx.m_is_glow_being_drawn &&
		                        !game_forced_material( ) && strstr( info.model->m_name, "models/player" ) && !strstr( info.model->m_name, "contactshadow" ) &&
		                        n_chams::impl_t::has_layers( GET_VARIABLE( g_variables.m_chams_shot_layers, std::vector< int > ) )
		                    ? shot_owner( info.entity_index )
		                    : 0 };

	if ( ( GET_VARIABLE( g_variables.m_remove_ragdolls, bool ) && should_skip_ragdoll( info ) && !kill_effects_shows_ragdoll( info.entity_index ) ) ||
	     kill_effects_hides_ragdoll( info.entity_index ) )
		return;

	if ( g_rear_mirror.drawing( ) )
		return original( ecx, edx, context, state, info, custom_bone_to_world );

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
		std::uint32_t darks_variable  = g_variables.m_chams_weapon_dark_colors;

		if ( part == viewmodel_part_arms ) {
			layers_variable = g_variables.m_chams_arms_layers;
			colors_variable = g_variables.m_chams_arms_colors;
			darks_variable  = g_variables.m_chams_arms_dark_colors;
		} else if ( part == viewmodel_part_sleeve ) {
			layers_variable = g_variables.m_chams_sleeve_layers;
			colors_variable = g_variables.m_chams_sleeve_colors;
			darks_variable  = g_variables.m_chams_sleeve_dark_colors;
		}

		const auto& layers       = GET_VARIABLE( layers_variable, std::vector< int > );
		const auto& colors       = GET_VARIABLE( colors_variable, std::vector< c_color > );
		const auto& darks        = GET_VARIABLE( darks_variable, std::vector< c_color > );
		const auto& trail_layers = GET_VARIABLE( g_variables.m_chams_trail_layers, std::vector< int > );

		// skin composites are a selective override set before dme: every forced pass wipes them, each pass starts from the game's
		auto& override_state                            = g_interfaces.m_studio_render->material_override( );
		const studio_material_override_t saved_override = override_state;

		// game forced material ( scope lens mask stencil, C_BaseViewModel::DrawModel ) = not a real viewmodel draw
		const bool game_forced = saved_override.m_materials[ 0 ] && saved_override.m_type != 3;

		const bool live_layers = n_chams::impl_t::has_layers( layers );
		const bool trail       = !game_forced && n_chams::impl_t::has_layers( trail_layers );

		if ( !live_layers && !trail )
			return original( ecx, edx, context, state, info, custom_bone_to_world );

		float entry_color[ 3 ]{ };
		g_interfaces.m_render_view->get_color_modulation( entry_color );
		const float entry_blend = g_interfaces.m_render_view->get_blend( );

		if ( trail ) {
			const auto* const header = state ? static_cast< const studiohdr_t* >( static_cast< const draw_model_state_t* >( state )->m_studio_hdr ) : nullptr;
			const int bone_count     = header ? header->n_bones : 0;

			g_viewmodel_trail.record( part, info.model, bone_count, custom_bone_to_world );

			// ghosts in their own depth range behind the live viewmodel, no depth write: never over a live part
			g_viewmodel_trail.for_each_ghost( context, part, info.model, bone_count, [ & ]( matrix3x4_t* ghost_bones, const float alpha ) {
				override_state = saved_override;
				g_chams.draw_layers( trail_layers, GET_VARIABLE( g_variables.m_chams_trail_colors, std::vector< c_color > ),
				                     GET_VARIABLE( g_variables.m_chams_trail_dark_colors, std::vector< c_color > ), false, alpha,
				                     [ & ]( ) { original( ecx, edx, context, state, info, ghost_bones ); }, nullptr, false );
			} );
		}

		override_state = saved_override;

		// weapon draws before arms (C_BaseViewModel::DrawModel): translucent layers must lay depth
		if ( live_layers )
			g_chams.draw_layers(
				layers, colors, darks, false, 1.f, [ & ]( ) { original( ecx, edx, context, state, info, custom_bone_to_world ); },
				static_cast< c_material_render_context* >( context ) );
		else {
			g_interfaces.m_render_view->set_color_modulation( entry_color );
			g_interfaces.m_render_view->set_blend( entry_blend );
			original( ecx, edx, context, state, info, custom_bone_to_world );
		}

		n_chams::impl_t::reset_original_color( );
		return;
	}

	if ( !enemy_chams || !strstr( model_name, "player" ) || strstr( model_name, "player/contactshadow" ) ||
	     g_interfaces.m_model_render->is_forced_material_override( ) )
		return original( ecx, edx, context, state, info, custom_bone_to_world );

	const bool draw_visible   = n_chams::impl_t::has_layers( GET_VARIABLE( g_variables.m_chams_visible_layers, std::vector< int > ) );
	const bool draw_occluded  = n_chams::impl_t::has_layers( GET_VARIABLE( g_variables.m_chams_occluded_layers, std::vector< int > ) );
	const bool draw_backtrack = !enemy_ragdoll && n_chams::impl_t::has_layers( GET_VARIABLE( g_variables.m_chams_backtrack_layers, std::vector< int > ) );
	const bool draw_sound     = !enemy_ragdoll && n_chams::impl_t::has_layers( GET_VARIABLE( g_variables.m_chams_sound_layers, std::vector< int > ) );

	if ( !draw_visible && !draw_occluded && !draw_backtrack && !draw_sound )
		return original( ecx, edx, context, state, info, custom_bone_to_world );

	const auto player = g_interfaces.m_client_entity_list->get< c_base_entity >( info.entity_index );
	if ( !enemy_ragdoll && ( !player || !player->is_valid_enemy( ) ) )
		return original( ecx, edx, context, state, info, custom_bone_to_world );

	const auto draw_live = [ & ]( ) {
		original( ecx, edx, context, state, info, custom_bone_to_world );
	};

	const float esp_gate = enemy_ragdoll ? 1.f : g_sound_esp.player_gate( player );

	const float sound_alpha = draw_sound ? g_sound_esp.alpha( info.entity_index, GET_VARIABLE( g_variables.m_chams_sound_duration, float ),
	                                                          GET_VARIABLE( g_variables.m_chams_sound_fade, bool ) )
	                                     : 0.f;

	const auto draw_backtrack_records = [ & ]( ) {
		const int backtrack_type = GET_VARIABLE( g_variables.m_chams_backtrack_type, int );

		/* not const: m_matrix must decay to a mutable matrix3x4_t*. skipped in aimbot-record mode */
		auto* oldest_record = backtrack_type == 2 ? nullptr : g_lagcomp.oldest_record( info.entity_index );
		const unsigned int anim_key = HASH_RT( model_name ) + static_cast< unsigned int >( info.entity_index ) * 0x9E3779B9U;

		/* records are network pose, model is interpolated behind it: newer ones would draw in front */
		const float model_time = g_lagcomp.model_time( );

		const auto draw_record = [ & ]( matrix3x4_t* record_matrix, const float alpha_scale ) {
			if ( record_matrix )
				g_chams.draw_backtrack_ghost( [ & ]( ) { original( ecx, edx, context, state, info, record_matrix ); }, alpha_scale * esp_gate );
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

					n_chams::impl_t::for_each_ghost( record_count, n_lagcomp::impl_t::max_records, [ & ]( const int i, const float tick_scale ) {
						draw_record( ranked[ i ]->m_matrix, alpha_animation.AnimationData->second * tick_scale );
					} );
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
	};

	g_chams.draw_player( draw_live, draw_backtrack_records, draw_backtrack, esp_gate, sound_alpha );
}
