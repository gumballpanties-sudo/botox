#pragma once
#include "c_angle.h"
#include "c_vector.h"

class c_move_data
{
public:
	bool m_first_run_of_functions : 1;
	bool m_game_code_moved_player : 1;
	bool m_no_air_control         : 1;
	unsigned int m_player_handle; 
	int m_impulse_command;        
	c_angle m_view_angles;        
	c_angle m_abs_view_angles;    
	int m_buttons;                
	int m_old_buttons;            
	float m_forward_move;
	float m_side_move;
	float m_up_move;
	float m_max_speed;
	float m_client_max_speed;
	c_vector m_velocity;
	c_vector m_trailing_velocity;
	float m_trailing_velocity_time;
	c_vector m_angles;
	c_vector m_old_angles;
	float m_out_step_height; 
	c_vector m_out_wish_vel; 
	c_vector m_out_jump_vel; 
	c_vector m_constraint_center;
	float m_constraint_radius;
	float m_constraint_width;
	float m_constraint_speed_factor;
	bool m_constraint_past_radius;
	c_vector m_abs_origin;
};
