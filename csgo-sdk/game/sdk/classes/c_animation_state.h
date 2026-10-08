#pragma once
#include "c_vector.h"

class c_base_entity;

class c_animation_state
{
public:
	unsigned char pad0[ 0x60 ];          
	c_base_entity* m_entity;
	c_base_entity* m_active_weapon;
	c_base_entity* m_last_active_weapon;
	float m_last_update_time;
	int m_last_update_frame;
	float m_last_update_increment;
	float m_eye_yaw;
	float m_eye_pitch;
	float m_goal_feet_yaw;
	float m_last_feet_yaw;
	float m_move_yaw;
	float m_last_move_yaw;
	float m_lean_amount;
	float m_lower_body_yaw_align_time;
	float m_feet_cycle;
	float m_move_weight;
	float m_move_weight_smoothed;
	float m_duck_amount;
	float m_hit_ground_cycle;
	float m_recrouch_weight;
	c_vector m_origin;
	c_vector m_last_origin;
	c_vector m_velocity;
	c_vector m_velocity_normalized;
	c_vector m_velocity_normalized_non_zero;
	float m_velocity_lenght2d;
	float m_jump_fall_velocity;
	float m_speed_normalized;
	float m_running_speed;
	float m_ducking_speed;
	float m_duration_moving;
	float m_duration_still;
	bool m_on_ground;
	bool m_hit_ground_animation;
	unsigned char pad2[ 0x2 ];
	float m_next_lower_body_yaw_update_time;
	float m_duration_in_air;
	float m_left_ground_height;
	float m_hit_ground_weight;
	float m_walk_to_run_transition;
	unsigned char pad3[ 0x4 ];
	float m_affected_fraction;
	unsigned char pad4[ 0x208 ];
	float m_min_body_yaw;
	float m_max_body_yaw;
	float m_min_pitch;
	float m_max_pitch;
	int m_animset_version;
};                                       
