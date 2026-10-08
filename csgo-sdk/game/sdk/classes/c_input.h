#pragma once
#include "c_vector.h"
#include "c_user_cmd.h"

#define MULTIPLAYER_BACKUP 150

class c_input
{
public:
	unsigned char pad0[ 0xc ];
	bool m_track_ir_available;
	bool m_mouse_initialized;
	bool m_mouse_active;
	unsigned char pad1[ 0x9a ];
	bool m_camera_in_third_person;
	unsigned char pad2[ 0x2 ];
	c_vector m_camera_offset;
	unsigned char pad3[ 0x38 ];
	c_user_cmd* m_commands;
	c_verified_user_cmd* m_verified_commands; 

	[[nodiscard]] c_user_cmd* get_user_cmd( const int sequence_number ) const
	{
		return &this->m_commands[ sequence_number % MULTIPLAYER_BACKUP ];
	}

	[[nodiscard]] c_verified_user_cmd* get_verified_cmd( const int sequence_number ) const
	{
		return &m_verified_commands[ sequence_number % MULTIPLAYER_BACKUP ];
	}
};
static_assert( sizeof( c_input ) == 0xF8 );
