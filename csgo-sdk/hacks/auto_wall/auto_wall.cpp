#include "auto_wall.h"
#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"
#include "../entity_cache/entity_cache.h"

#include <algorithm>

float n_auto_wall::impl_t::get_damage( const c_vector& point, fire_bullet_data_t* data_out )
{
	const auto position = g_ctx.m_local->get_eye_position( );

	fire_bullet_data_t data = { };
	data.m_position         = position;
	data.m_direction        = ( point - position ).normalized( );

	if ( const auto weapon = g_interfaces.m_client_entity_list->get< c_base_entity >( g_ctx.m_local->get_active_weapon_handle( ) );
	     !weapon || !simulate_fire_bullet( weapon, data ) )
		return -1.0f;

	if ( data_out )
		*data_out = data;

	return data.m_current_damage;
}

void n_auto_wall::impl_t::scale_damage( const int hit_group, c_base_entity* entity, const float weapon_armor_ratio,
                                        const float weapon_head_shot_multiplier, float& damage )
{
	const auto has_heavy_armor = entity->has_heavy_armor( );
	const auto team            = entity->get_team( );

	static auto mp_damage_scale_ct_head = g_convars[ HASH_BT( "mp_damage_scale_ct_head" ) ];
	static auto mp_damage_scale_t_head  = g_convars[ HASH_BT( "mp_damage_scale_t_head" ) ];

	float head_damage_scale = team == e_team_id::team_ct   ? mp_damage_scale_ct_head->get_float( )
	                          : team == e_team_id::team_tt ? mp_damage_scale_t_head->get_float( )
	                                                       : 1.0f;

	static auto mp_damage_scale_ct_body = g_convars[ HASH_BT( "mp_damage_scale_ct_body" ) ];
	static auto mp_damage_scale_t_body  = g_convars[ HASH_BT( "mp_damage_scale_t_body" ) ];

	const float body_damage_scale = team == e_team_id::team_ct   ? mp_damage_scale_ct_body->get_float( )
	                                : team == e_team_id::team_tt ? mp_damage_scale_t_body->get_float( )
	                                                             : 1.0f;

	if ( has_heavy_armor )
		head_damage_scale *= 0.5f;

	switch ( hit_group ) {
	case e_hitgroup::hitgroup_head:
		damage *= weapon_head_shot_multiplier * head_damage_scale;
		break;
	case e_hitgroup::hitgroup_chest:
	case e_hitgroup::hitgroup_leftarm:
	case e_hitgroup::hitgroup_rightarm:
	case e_hitgroup::hitgroup_neck:
		damage *= body_damage_scale;
		break;
	case e_hitgroup::hitgroup_stomach:
		damage *= 1.25f * body_damage_scale;
		break;
	case e_hitgroup::hitgroup_leftleg:
	case e_hitgroup::hitgroup_rightleg:
		damage *= 0.75f * body_damage_scale;
		break;
	default:
		break;
	}

	if ( entity->is_armored( hit_group ) ) {
		const int armor         = entity->get_armor( );
		float heavy_armor_bonus = 1.0f, armor_bonus = 0.5f, armor_ratio = weapon_armor_ratio * 0.5f;

		if ( has_heavy_armor ) {
			heavy_armor_bonus = 0.25f;
			armor_bonus       = 0.33f;
			armor_ratio *= 0.20f;
		}

		float damage_to_health = damage * armor_ratio;
		if ( const float damage_to_armor = ( damage - damage_to_health ) * ( heavy_armor_bonus * armor_bonus );
		     damage_to_armor > static_cast< float >( armor ) )
			damage_to_health = damage - static_cast< float >( armor ) / armor_bonus;

		damage = damage_to_health;
	}
}

void n_auto_wall::impl_t::clip_trace_to_players( const c_vector& abs_start, const c_vector& abs_end, const unsigned int mask, c_trace_filter* filter,
                                                 trace_t* exit_trace, const float min_range )
{
	trace_t trace           = { };
	float smallest_fraction = exit_trace->m_fraction;

	const ray_t ray( abs_start, abs_end );

	g_entity_cache.enumerate( e_enumeration_type::type_players, [ & ]( c_base_entity* entity ) {
		if ( !entity || !entity->is_valid_aim_target( ) )
			return;

		if ( filter && !filter->should_hit_entity( entity, mask ) )
			return;

		const auto client_renderable = entity->get_client_renderable( );
		if ( !client_renderable )
			return;

		const auto client_unknown = client_renderable->get_client_unknown( );
		if ( !client_unknown )
			return;

		const auto collideable = client_unknown->get_collideable( );
		if ( !collideable )
			return;

		const c_vector obb_mins = collideable->get_obb_mins( );
		const c_vector obb_maxs = collideable->get_obb_maxs( );

		const c_vector center   = ( obb_maxs + obb_mins ) * 0.5f;
		const c_vector position = center + entity->get_origin( );

		const c_vector position_difference = position - abs_start;
		c_vector abs_difference            = abs_end - abs_start;
		const float flLength               = abs_difference.normalize_in_place( );

		const float range_along = abs_difference.dot_product( position_difference );
		float range             = 0.0f;

		if ( range_along < 0.0f )
			range = -position_difference.length( );
		else if ( range_along > flLength )
			range = -( position - abs_end ).length( );
		else
			range = ( position - ( abs_difference * range_along + abs_start ) ).length( );

		constexpr float max_range = 60.f;
		if ( range < min_range || range > max_range )
			return;

		g_interfaces.m_engine_trace->clip_ray_to_entity( ray, mask | e_contents::contents_hitbox, entity, &trace );

		if ( trace.m_fraction < smallest_fraction ) {
			*exit_trace       = trace;
			smallest_fraction = trace.m_fraction;
		}
	} );
}

bool n_auto_wall::impl_t::trace_to_exit( trace_t& enter_trace, trace_t& exit_trace, const c_vector& position, const c_vector& direction,
                                         const c_base_entity* clip_player )
{
	static auto sv_clip_penetration_traces_to_players = g_convars[ HASH_BT( "sv_clip_penetration_traces_to_players" ) ];

	float distance     = 0.0f;
	int start_contents = 0;

	while ( distance <= 90.0f ) {
		distance += 4.0f;

		c_vector start = position + direction * distance;

		if ( !start_contents )
			start_contents = g_interfaces.m_engine_trace->get_point_contents( start, e_mask::mask_shot_hull | e_contents::contents_hitbox, nullptr );

		const int current_contents =
			g_interfaces.m_engine_trace->get_point_contents( start, e_mask::mask_shot_hull | e_contents::contents_hitbox, nullptr );

		if ( !( current_contents & e_mask::mask_shot_hull ) ||
		     ( current_contents & e_contents::contents_hitbox && current_contents != start_contents ) ) {
			const c_vector end = start - ( direction * 4.0f );

			ray_t world_ray( start, end );
			g_interfaces.m_engine_trace->trace_ray( world_ray, e_mask::mask_shot_hull | e_contents::contents_hitbox, nullptr, &exit_trace );

			if ( sv_clip_penetration_traces_to_players->get_bool( ) ) {
				c_trace_filter filter( clip_player );
				clip_trace_to_players( end, start, e_mask::mask_shot_hull | e_contents::contents_hitbox, &filter, &exit_trace, -60.f );
			}

			if ( exit_trace.m_start_solid && exit_trace.surface.m_flags & e_surf_type::surf_hitbox ) {
				ray_t ray( start, position );
				c_trace_filter filter( exit_trace.m_hit_entity );

				g_interfaces.m_engine_trace->trace_ray( ray, e_mask::mask_shot_hull, &filter, &exit_trace );

				if ( exit_trace.did_hit( ) && !exit_trace.m_start_solid ) {
					start = exit_trace.m_end;
					return true;
				}

				continue;
			}

			if ( exit_trace.did_hit( ) && !exit_trace.m_start_solid ) {
				if ( enter_trace.m_hit_entity->is_breakable( ) && exit_trace.m_hit_entity->is_breakable( ) )
					return true;

				if ( enter_trace.surface.m_flags & e_surf_type::surf_nodraw || ( !( exit_trace.surface.m_flags & e_surf_type::surf_nodraw ) &&
				                                                                 exit_trace.m_plane.m_normal.dot_product( direction ) <= 1.0f ) ) {
					const float multiplier = exit_trace.m_fraction * 4.0f;
					start -= direction * multiplier;
					return true;
				}

				continue;
			}

			if ( !exit_trace.did_hit( ) || exit_trace.m_start_solid ) {
				if ( !enter_trace.m_hit_entity )
					return false;

				if ( enter_trace.m_hit_entity->get_index( ) != 0 && enter_trace.m_hit_entity->is_breakable( ) ) {
					exit_trace       = enter_trace;
					exit_trace.m_end = start + direction;
					return true;
				}

				continue;
			}
		}
	}

	return false;
}

bool n_auto_wall::impl_t::handle_bullet_penetration( const c_weapon_data* weapon_data, const surfacedata_t* enter_surface_data,
                                                     fire_bullet_data_t& data )
{
	static auto ff_damage_reduction_bullets  = g_convars[ HASH_BT( "ff_damage_reduction_bullets" ) ];
	static auto ff_damage_bullet_penetration = g_convars[ HASH_BT( "ff_damage_bullet_penetration" ) ];

	const float reducation_damage = ff_damage_reduction_bullets->get_float( );
	const float penetrate_damage  = ff_damage_bullet_penetration->get_float( );

	const unsigned short enter_material = enter_surface_data->m_game.m_material;

	if ( data.m_penetrate_count == 0 && enter_material != e_valve_decals::char_tex_grate && enter_material != e_valve_decals::char_tex_glass &&
	     !( data.m_enter_trace.surface.m_flags & e_surf_type::surf_nodraw ) )
		return false;

	if ( weapon_data->m_penetration <= 0.0f || data.m_penetrate_count <= 0 )
		return false;

	trace_t exit_trace = { };
	if ( !trace_to_exit( data.m_enter_trace, exit_trace, data.m_enter_trace.m_end, data.m_direction, g_ctx.m_local ) &&
	     !( g_interfaces.m_engine_trace->get_point_contents( data.m_enter_trace.m_end, e_mask::mask_shot_hull, nullptr ) & e_mask::mask_shot_hull ) )
		return false;

	const surfacedata_t* exit_surface_data = g_interfaces.m_physics_surface_props->get_surface_data( exit_trace.surface.m_surface_props );
	const unsigned short exit_material     = exit_surface_data->m_game.m_material;

	const float enter_penetration_modifier = enter_surface_data->m_game.m_penetration_modifier;
	const float exit_penetration_modifier  = exit_surface_data->m_game.m_penetration_modifier;

	float damage_lost_modifier = 0.16f;
	float penetration_modifier = 0.0f;

	if ( enter_material == e_valve_decals::char_tex_grate || enter_material == e_valve_decals::char_tex_glass ) {
		damage_lost_modifier = 0.05f;
		penetration_modifier = 3.0f;
	} else if ( ( ( data.m_enter_trace.m_contents >> 3 ) & e_contents::contents_solid ) ||
	            ( ( data.m_enter_trace.surface.m_flags >> 7 ) & e_surf_type::surf_light ) ) {
		damage_lost_modifier = 0.16f;
		penetration_modifier = 1.0f;
	} else if ( enter_material == e_valve_decals::char_tex_flesh && reducation_damage == 0.0f && data.m_enter_trace.m_hit_entity != nullptr &&
	            data.m_enter_trace.m_hit_entity->is_player( ) && ( g_ctx.m_local->get_team( ) == data.m_enter_trace.m_hit_entity->get_team( ) ) ) {
		if ( penetrate_damage == 0.0f )
			return false;

		damage_lost_modifier = penetrate_damage;
		penetration_modifier = penetrate_damage;
	} else {
		damage_lost_modifier = 0.16f;
		penetration_modifier = ( enter_penetration_modifier + exit_penetration_modifier ) * 0.5f;
	}

	if ( enter_material == exit_material ) {
		if ( exit_material == e_valve_decals::char_tex_cardboard || exit_material == e_valve_decals::char_tex_wood )
			penetration_modifier = 3.0f;
		else if ( exit_material == e_valve_decals::char_tex_plastic )
			penetration_modifier = 2.0f;
	}

	const float trace_distance = ( exit_trace.m_end - data.m_enter_trace.m_end ).length_squared( );

	const float modifier = ( penetration_modifier > 0.0f ? 1.0f / penetration_modifier : 0.0f );

	const float lost_damage = ( data.m_current_damage * damage_lost_modifier +
	                            ( weapon_data->m_penetration > 0.0f ? 3.75f / weapon_data->m_penetration : 0.0f ) * ( modifier * 3.0f ) ) +
	                          ( ( modifier * trace_distance ) / 24.0f );

	if ( lost_damage > data.m_current_damage )
		return false;

	if ( lost_damage > 0.0f )
		data.m_current_damage -= lost_damage;

	if ( data.m_current_damage < 1.0f )
		return false;

	data.m_position = exit_trace.m_end;
	--data.m_penetrate_count;
	return true;
}

float n_auto_wall::impl_t::get_record_damage( const c_vector& point, c_base_entity* target, const int hit_group )
{
	const auto position = g_ctx.m_local->get_eye_position( );

	fire_bullet_data_t data = { };
	data.m_position         = position;
	data.m_direction        = ( point - position ).normalized( );

	if ( const auto weapon = g_interfaces.m_client_entity_list->get< c_base_entity >( g_ctx.m_local->get_active_weapon_handle( ) );
	     !weapon || !simulate_fire_bullet( weapon, data, target, hit_group, &point ) )
		return -1.0f;

	return data.m_current_damage;
}

bool n_auto_wall::impl_t::simulate_fire_bullet( c_base_entity* weapon, fire_bullet_data_t& data, c_base_entity* record_target, const int hit_group,
                                                const c_vector* record_point )
{
	const auto weapon_data = g_interfaces.m_weapon_system->get_weapon_data( weapon->get_item_definition_index( ) );
	if ( !weapon_data )
		return false;

	const bool record = record_target && record_point;

	data.m_penetrate_count = 4;
	data.m_current_damage  = static_cast< float >( weapon_data->m_damage );

	float trace_length = 0.0f;

	/* record: server moves the target there, so its live model must not block */
	c_trace_filter filter( [ record_target ]( c_base_entity* entity, int ) { return entity != g_ctx.m_local && entity != record_target; } );

	while ( data.m_penetrate_count > 0 && data.m_current_damage >= 1.0f ) {
		const float segment = record ? std::max( 0.f, ( *record_point - data.m_position ).dot_product( data.m_direction ) )
		                             : weapon_data->m_range - trace_length;
		if ( !record && segment <= 0.f )
			break;

		const c_vector end = data.m_position + data.m_direction * segment;

		ray_t ray( data.m_position, end );
		g_interfaces.m_engine_trace->trace_ray( ray, e_mask::mask_shot_hull | e_contents::contents_hitbox, &filter, &data.m_enter_trace );

		clip_trace_to_players( data.m_position, record ? end : end + data.m_direction * 40.0f, e_mask::mask_shot_hull | e_contents::contents_hitbox,
		                       &filter, &data.m_enter_trace );

		const surfacedata_t* enter_surface_data =
			g_interfaces.m_physics_surface_props->get_surface_data( data.m_enter_trace.surface.m_surface_props );
		const float enter_penetration_modifier = enter_surface_data->m_game.m_penetration_modifier;

		if ( data.m_enter_trace.m_fraction == 1.0f ) {
			if ( !record )
				break;

			trace_length += segment;
			data.m_current_damage *= std::powf( weapon_data->m_range_modifier, trace_length / max_weapon_damage );
			scale_damage( hit_group, record_target, weapon_data->m_armor_ratio, weapon_data->m_head_shot_multiplier, data.m_current_damage );
			return data.m_current_damage >= 1.0f;
		}

		trace_length += data.m_enter_trace.m_fraction * segment;
		data.m_current_damage *= std::powf( weapon_data->m_range_modifier, trace_length / max_weapon_damage );

		/* CCSPlayer::FireBullet: the hit lands first, the 3000u / modifier cut only stops further penetration */
		if ( data.m_enter_trace.m_hit_group != hitgroup_generic && data.m_enter_trace.m_hit_group != hitgroup_gear &&
		     data.m_enter_trace.m_hit_entity && data.m_enter_trace.m_hit_entity->is_valid_aim_target( ) ) {
			scale_damage( data.m_enter_trace.m_hit_group, data.m_enter_trace.m_hit_entity, weapon_data->m_armor_ratio,
			              weapon_data->m_head_shot_multiplier, data.m_current_damage );
			return true;
		}

		if ( trace_length > 3000.f || enter_penetration_modifier < 0.1f )
			break;

		if ( !handle_bullet_penetration( weapon_data, enter_surface_data, data ) )
			break;
	}

	return false;
}
