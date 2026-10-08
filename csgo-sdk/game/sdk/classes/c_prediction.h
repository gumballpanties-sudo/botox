#pragma once
#include "../../../utilities/memory/virtual.h"
#include "c_angle.h"
#include "c_utl_vector.h"

class c_base_entity;
class c_user_cmd;
class c_move_data;

class c_prediction
{
public:
	char pad0[ 0x4 ];
	unsigned int m_last_ground;
	bool m_in_prediction;
	bool m_is_first_time_predicted;
	bool m_engine_paused;
	bool m_old_cl_predict_value;
	int i_previous_start_frame;
	int n_incoming_packet_number;
	float fl_last_server_world_time_stamp;
	bool sp_is_first_time_predicted;
	char pad1[ 0x3 ];
	int m_commands_predicted;
	int m_server_commands_acknowledged;
	int m_previous_ack_had_errors;
	float m_ideal_pitch;
	int m_last_command_acknowledged;
	bool m_previous_ack_error_triggers_full_latch_reset;
	c_utl_vector< unsigned int > m_entities_with_prediction_errors_in_last_ack;
	bool m_performed_tick_shift;

public:
	void update( int start_frame, bool valid_frame, int incoming_acknowledged, int outgoing_command )
	{
		g_virtual.call< void >( this, 3, start_frame, valid_frame, incoming_acknowledged, outgoing_command );
	}

	void get_local_view_angles( c_angle& view_angles )
	{
		g_virtual.call< void >( this, 12, std::ref( view_angles ) );
	}

	void set_local_view_angles( c_angle& view_angles )
	{
		g_virtual.call< void >( this, 13, std::ref( view_angles ) );
	}

	void check_moving_ground( c_base_entity* entity, double frametime )
	{
		g_virtual.call< void >( this, 18, entity, frametime );
	}

	void setup_move( c_base_entity* entity, c_user_cmd* cmd, c_move_helper* p_helper, c_move_data* move_data )
	{
		g_virtual.call< void >( this, 20, entity, cmd, p_helper, move_data );
	}

	void finish_move( c_base_entity* entity, c_user_cmd* cmd, c_move_data* move_data )
	{
		g_virtual.call< void >( this, 21, entity, cmd, move_data );
	}
};
