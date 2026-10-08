#include "grenade_path.h"
#include "../../../game/sdk/includes/includes.h"
#include "../../../globals/includes/includes.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>

namespace
{
	constexpr float k_gravity_modifier = 0.4f;
	constexpr float k_elasticity       = 0.45f;
	constexpr float k_grenade_size     = 2.f;

	constexpr float k_underhand_dampening = 0.3f;
	constexpr float k_underhand_lower     = 12.f;

	constexpr float k_sleep_velocity_sq   = 400.f;
	constexpr float k_bounce_pad_speed_sq = 96000.f;

	constexpr float k_stop_epsilon = 0.1f;
	constexpr float k_max_time     = 5.f;

	constexpr unsigned int k_contents_grenadeclip = 0x80000;

	constexpr float k_fuse_time            = 1.5f;
	constexpr float k_molotov_rest_time    = 0.5f;
	constexpr float k_molotov_min_normal_z = 0.866f;

	constexpr float k_smoke_radius   = 166.f;
	constexpr float k_generic_radius = 115.f;

	bool is_grenade( const short item_index )
	{
		switch ( item_index ) {
		case e_item_definition_index::weapon_flashbang:
		case e_item_definition_index::weapon_hegrenade:
		case e_item_definition_index::weapon_smokegrenade:
		case e_item_definition_index::weapon_molotov:
		case e_item_definition_index::weapon_decoy:
		case e_item_definition_index::weapon_incgrenade:
		case e_item_definition_index::weapon_tagrenade:
			return true;
		default:
			return false;
		}
	}

	float effect_radius( const short item_index )
	{
		return item_index == e_item_definition_index::weapon_smokegrenade ? k_smoke_radius : k_generic_radius;
	}

	/* m_flThrowStrength: 1 = overhand, 0 = underhand. offset by hand: a missing prop (0) must fall
	   back to 1, not read the vtable pointer. */
	float throw_strength( c_base_entity* weapon )
	{
		static const unsigned int offset = g_netvars[ HASH_BT( "CBaseCSGrenade->m_flThrowStrength" ) ].m_offset;
		if ( !offset )
			return 1.f;

		return std::clamp( *reinterpret_cast< float* >( reinterpret_cast< std::uintptr_t >( weapon ) + offset ), 0.f, 1.f );
	}

	bool is_pin_pulled( c_base_entity* weapon )
	{
		static const unsigned int offset = g_netvars[ HASH_BT( "CBaseCSGrenade->m_bPinPulled" ) ].m_offset;
		if ( !offset )
			return true;

		return *reinterpret_cast< bool* >( reinterpret_cast< std::uintptr_t >( weapon ) + offset );
	}

	c_vector clip_velocity( const c_vector& in, const c_vector& normal )
	{
		const float backoff = in.dot_product( normal ) * 2.f;

		c_vector out = in - normal * backoff;

		if ( std::fabsf( out.m_x ) < k_stop_epsilon )
			out.m_x = 0.f;
		if ( std::fabsf( out.m_y ) < k_stop_epsilon )
			out.m_y = 0.f;
		if ( std::fabsf( out.m_z ) < k_stop_epsilon )
			out.m_z = 0.f;

		return out;
	}
}

bool n_grenade_path::impl_t::needs_rebuild( const c_angle& angles, const c_vector& origin, const float strength, const short item_index )
{
	const int tick = g_interfaces.m_global_vars_base->m_tick_count;

	if ( tick != this->m_last_tick || item_index != this->m_last_item_index || this->m_count == 0 )
		return true;

	if ( !angles.is_equal( this->m_last_angles, 0.01f ) )
		return true;

	if ( origin.dist_to_squared( this->m_last_origin ) > 0.01f )
		return true;

	return std::fabsf( strength - this->m_last_throw_strength ) > 0.001f;
}

bool n_grenade_path::impl_t::simulate( c_base_entity* weapon, const short item_index )
{
	this->m_count           = 0;
	this->m_detonated       = false;
	this->m_detonate_radius = 0.f;

	const auto weapon_data = g_interfaces.m_weapon_system->get_weapon_data( item_index );
	if ( !weapon_data )
		return false;

	static const auto sv_gravity = g_convars[ HASH_BT( "sv_gravity" ) ];

	const float step = g_interfaces.m_global_vars_base->m_interval_per_tick;
	if ( step <= 0.f )
		return false;

	const float gravity = ( sv_gravity ? sv_gravity->get_float( ) : 800.f ) * k_gravity_modifier;

	static const auto recoil_scale_convar = g_convars[ HASH_BT( "weapon_recoil_scale" ) ];

	c_angle view_angles = { };
	g_interfaces.m_engine_client->get_view_angles( view_angles );

	c_angle throw_angles = view_angles + g_ctx.m_local->get_punch( ) * ( recoil_scale_convar ? recoil_scale_convar->get_float( ) : 2.f );

	throw_angles.m_x = std::clamp( g_math.normalize_angle( throw_angles.m_x, -180.f, 180.f ), -90.f, 90.f );
	throw_angles.m_x -= 10.f * ( 90.f - std::fabsf( throw_angles.m_x ) ) / 90.f;
	throw_angles.m_y = g_math.normalize_angle( throw_angles.m_y, -180.f, 180.f );
	throw_angles.m_z = 0.f;

	const float strength = throw_strength( weapon );

	float velocity = std::clamp( weapon_data->m_throw_velocity * 0.9f, 15.f, 750.f );
	velocity *= k_underhand_dampening + ( 1.f - k_underhand_dampening ) * strength;

	c_vector forward = { };
	g_math.angle_vectors( throw_angles, &forward );

	c_vector position = g_ctx.m_local->get_origin( ) + g_ctx.m_local->get_view_offset( );
	position.m_z -= k_underhand_lower * ( 1.f - strength );

	const c_vector mins( -k_grenade_size, -k_grenade_size, -k_grenade_size );
	const c_vector maxs( k_grenade_size, k_grenade_size, k_grenade_size );

	const unsigned int mask = e_mask::mask_solid | k_contents_grenadeclip;

	c_trace_filter filter( g_ctx.m_local );

	{
		trace_t trace = { };
		g_interfaces.m_engine_trace->trace_ray( ray_t( position, position + forward * 22.f, mins, maxs ), mask, &filter, &trace );

		position = trace.m_end - forward * 6.f;
	}

	c_vector throw_velocity = forward * velocity + g_ctx.m_local->get_velocity( ) * 1.25f;

	this->m_points[ this->m_count++ ] = { position, false };

	float time      = 0.f;
	float rest_time = -1.f;

	while ( time < k_max_time && this->m_count < static_cast< int >( this->m_points.size( ) ) ) {
		time += step;

		const float new_velocity_z = throw_velocity.m_z - gravity * step;

		const c_vector move( throw_velocity.m_x * step, throw_velocity.m_y * step, ( ( throw_velocity.m_z + new_velocity_z ) * 0.5f ) * step );

		throw_velocity.m_z = new_velocity_z;

		trace_t trace = { };
		g_interfaces.m_engine_trace->trace_ray( ray_t( position, position + move, mins, maxs ), mask, &filter, &trace );

		position = trace.m_end;

		bool bounced   = false;
		bool detonated = false;

		if ( trace.m_fraction < 1.f ) {
			bounced = true;

			const c_vector normal = trace.m_plane.m_normal;

			if ( ( item_index == e_item_definition_index::weapon_molotov || item_index == e_item_definition_index::weapon_incgrenade ) &&
			     normal.m_z >= k_molotov_min_normal_z )
				detonated = true;

			throw_velocity = clip_velocity( throw_velocity, normal ) * k_elasticity;

			const float speed_squared = throw_velocity.length_squared( );

			if ( normal.m_z > 0.7f || ( normal.m_z > 0.1f && speed_squared < k_sleep_velocity_sq ) ) {
				if ( speed_squared > k_bounce_pad_speed_sq ) {
					const float along = throw_velocity.normalized( ).dot_product( normal );

					if ( along > 0.5f )
						throw_velocity = throw_velocity * ( ( 1.f - along ) + 0.5f );
				}

				if ( speed_squared < k_sleep_velocity_sq )
					throw_velocity = c_vector( );
			}

			if ( !detonated && !throw_velocity.is_zero( ) ) {
				const c_vector remainder = throw_velocity * ( ( 1.f - trace.m_fraction ) * step );

				trace_t bounce_trace = { };
				g_interfaces.m_engine_trace->trace_ray( ray_t( position, position + remainder, mins, maxs ), mask, &filter, &bounce_trace );

				position = bounce_trace.m_end;
			}
		}

		this->m_points[ this->m_count++ ] = { position, bounced };

		if ( !detonated ) {
			switch ( item_index ) {
			case e_item_definition_index::weapon_hegrenade:
			case e_item_definition_index::weapon_flashbang:
				detonated = time >= k_fuse_time;
				break;

			default: {
				if ( throw_velocity.is_zero( ) ) {
					if ( rest_time < 0.f )
						rest_time = time;

					const bool is_fire = item_index == e_item_definition_index::weapon_molotov ||
					                     item_index == e_item_definition_index::weapon_incgrenade;

					detonated = !is_fire || ( time - rest_time ) >= k_molotov_rest_time;
				} else
					rest_time = -1.f;

				break;
			}
			}
		}

		if ( detonated ) {
			this->m_detonated         = true;
			this->m_detonate_position = position;
			this->m_detonate_radius   = effect_radius( item_index );
			break;
		}
	}

	return this->m_count > 1;
}

void n_grenade_path::impl_t::draw( )
{
	const auto line_color     = GET_VARIABLE( g_variables.m_grenade_path_color, c_color ).get_u32( );
	const auto bounce_color   = GET_VARIABLE( g_variables.m_grenade_path_bounce_color, c_color ).get_u32( );
	const auto detonate_color = GET_VARIABLE( g_variables.m_grenade_path_detonate_color, c_color ).get_u32( );
	const float thickness     = GET_VARIABLE( g_variables.m_grenade_path_thickness, float );
	const bool draw_radius    = GET_VARIABLE( g_variables.m_grenade_path_radius, bool );

	c_vector_2d previous = { };
	bool has_previous    = false;

	for ( int i = 0; i < this->m_count; ++i ) {
		c_vector_2d screen = { };

		if ( !g_render.world_to_screen( this->m_points[ i ].m_position, screen ) ) {
			has_previous = false;
			continue;
		}

		if ( has_previous )
			g_render.m_draw_data.emplace_back( e_draw_type::draw_type_line,
			                                   std::make_any< line_draw_object_t >( line_draw_object_t{ previous, screen, line_color, thickness } ) );

		if ( this->m_points[ i ].m_bounce ) {
			const rect_draw_object_t marker{ c_vector_2d( screen.m_x - 2.f, screen.m_y - 2.f ), c_vector_2d( screen.m_x + 2.f, screen.m_y + 2.f ),
			                                 bounce_color, 0u, true };

			g_render.m_draw_data.emplace_back( e_draw_type::draw_type_rect, std::make_any< rect_draw_object_t >( marker ) );
		}

		previous     = screen;
		has_previous = true;
	}

	if ( !this->m_detonated )
		return;

	c_vector_2d detonate_screen = { };
	if ( g_render.world_to_screen( this->m_detonate_position, detonate_screen ) )
		g_render.m_draw_data.emplace_back( e_draw_type::draw_type_filled_circle,
		                                   std::make_any< filled_circle_draw_object_t >( detonate_screen, 3.f, detonate_color ) );

	if ( !draw_radius || this->m_detonate_radius <= 0.f )
		return;

	constexpr int segments       = 32;
	constexpr float step_radians = 2.f * std::numbers::pi_v< float > / segments;

	c_vector_2d last = { };
	bool has_last    = false;

	for ( int i = 0; i <= segments; ++i ) {
		const float radians = step_radians * i;

		const c_vector world( this->m_detonate_position.m_x + std::cosf( radians ) * this->m_detonate_radius,
		                      this->m_detonate_position.m_y + std::sinf( radians ) * this->m_detonate_radius, this->m_detonate_position.m_z );

		c_vector_2d screen = { };
		if ( !g_render.world_to_screen( world, screen ) ) {
			has_last = false;
			continue;
		}

		if ( has_last )
			g_render.m_draw_data.emplace_back(
				e_draw_type::draw_type_line, std::make_any< line_draw_object_t >( line_draw_object_t{ last, screen, detonate_color, thickness } ) );

		last     = screen;
		has_last = true;
	}
}

void n_grenade_path::impl_t::on_paint_traverse( )
{
	if ( !GET_VARIABLE( g_variables.m_grenade_path, bool ) )
		return;

	if ( !g_ctx.m_local || !g_ctx.m_local->is_alive( ) || g_ctx.m_local->get_observer_mode( ) != e_obs_mode::obs_mode_none ) {
		this->m_count = 0;
		return;
	}

	const auto weapon = g_interfaces.m_client_entity_list->get< c_base_entity >( g_ctx.m_local->get_active_weapon_handle( ) );
	if ( !weapon ) {
		this->m_count = 0;
		return;
	}

	const short item_index = weapon->get_item_definition_index( );
	if ( !is_grenade( item_index ) ) {
		this->m_count = 0;
		return;
	}

	if ( !is_pin_pulled( weapon ) ) {
		this->m_count = 0;
		return;
	}

	c_angle view_angles = { };
	g_interfaces.m_engine_client->get_view_angles( view_angles );

	const c_vector origin = g_ctx.m_local->get_origin( );
	const float strength  = throw_strength( weapon );

	if ( this->needs_rebuild( view_angles, origin, strength, item_index ) ) {
		this->m_last_tick           = g_interfaces.m_global_vars_base->m_tick_count;
		this->m_last_angles         = view_angles;
		this->m_last_origin         = origin;
		this->m_last_throw_strength = strength;
		this->m_last_item_index     = item_index;

		if ( !this->simulate( weapon, item_index ) )
			return;
	}

	this->draw( );
}
