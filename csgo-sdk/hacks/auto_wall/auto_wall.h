#pragma once
#include "../../game/sdk/classes/c_engine_trace.h"
#include "../../game/sdk/classes/c_physics_surface_props.h"
#include "../../game/sdk/classes/c_vector.h"

class c_weapon_data;

struct fire_bullet_data_t {
	c_vector m_position    = { };
	c_vector m_direction   = { };
	trace_t m_enter_trace  = { };
	float m_current_damage = 0.0f;
	int m_penetrate_count  = 0;
};

namespace n_auto_wall
{
	struct impl_t {
	public:
		static float get_damage(const c_vector& point, fire_bullet_data_t* data_out = nullptr );

		static void scale_damage( const int hit_group, c_base_entity* entity, const float weapon_armor_ratio, const float weapon_head_shot_multiplier,
		                          float& damage );

		/* backtrack record point: bullet only has to reach it, target's live model skipped */
		static float get_record_damage( const c_vector& point, c_base_entity* target, int hit_group );

		static bool simulate_fire_bullet( c_base_entity* weapon, fire_bullet_data_t& data, c_base_entity* record_target = nullptr, int hit_group = 0,
		                                  const c_vector* record_point = nullptr );

	private:
		static void clip_trace_to_players( const c_vector& abs_start, const c_vector& abs_end, const unsigned int mask, c_trace_filter* filter,
		                                   trace_t* exit_trace, const float min_range = 0.0f );

		static bool trace_to_exit( trace_t& enter_trace, trace_t& exit_trace, const c_vector& position, const c_vector& direction,
		                           const c_base_entity* clip_player );

		static bool handle_bullet_penetration(  const c_weapon_data* weapon_data, const surfacedata_t* enter_surface_data,
		                                       fire_bullet_data_t& data );
	};
}

inline n_auto_wall::impl_t g_auto_wall{ };