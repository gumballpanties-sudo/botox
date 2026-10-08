#pragma once
#include "../classes/c_angle.h"
#include "../classes/c_vector.h"

enum e_command_buttons;

#pragma pack( push, 4 )
class c_user_cmd
{
public:
	virtual ~c_user_cmd( ) { }
	int m_command_number;
	int m_tick_count;
	c_angle m_view_point;
	c_vector m_aim_direction;
	float m_forward_move;
	float m_side_move;
	float m_up_move;
	int m_buttons;
	unsigned char m_impulse;
	int m_weapon_select;
	int m_weapon_sub_type;
	int m_random_seed;
	short m_mouse_delta_x;
	short m_mouse_delta_y;
	bool m_has_been_predicted;
	c_vector m_head_angles;
	c_vector m_head_offset;

	[[nodiscard]] unsigned int get_check_sum( ) const;
};
static_assert( sizeof( c_user_cmd ) == 0x64 );

class c_verified_user_cmd
{
public:
	c_user_cmd m_user_cmd;
	unsigned int m_hash_crc;
};
static_assert( sizeof( c_verified_user_cmd ) == 0x68 );
#pragma pack( pop )
