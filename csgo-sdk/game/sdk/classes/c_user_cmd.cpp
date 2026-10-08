#include "c_user_cmd.h"
#include "../../../dependencies/crc32/crc32.h"

[[nodiscard]] unsigned int c_user_cmd::get_check_sum( ) const
{
	unsigned int hash_crc = 0UL;

	crc32::init( &hash_crc );
	crc32::process_buffer( &hash_crc, &this->m_command_number, sizeof( this->m_command_number ) );
	crc32::process_buffer( &hash_crc, &this->m_tick_count, sizeof( this->m_tick_count ) );
	crc32::process_buffer( &hash_crc, &this->m_view_point, sizeof( this->m_view_point ) );
	crc32::process_buffer( &hash_crc, &this->m_aim_direction, sizeof( this->m_aim_direction ) );
	crc32::process_buffer( &hash_crc, &this->m_forward_move, sizeof( this->m_forward_move ) );
	crc32::process_buffer( &hash_crc, &this->m_side_move, sizeof( this->m_side_move ) );
	crc32::process_buffer( &hash_crc, &this->m_up_move, sizeof( this->m_up_move ) );
	crc32::process_buffer( &hash_crc, &this->m_buttons, sizeof( this->m_buttons ) );
	crc32::process_buffer( &hash_crc, &this->m_impulse, sizeof( this->m_impulse ) );
	crc32::process_buffer( &hash_crc, &this->m_weapon_select, sizeof( this->m_weapon_select ) );
	crc32::process_buffer( &hash_crc, &this->m_weapon_sub_type, sizeof( this->m_weapon_sub_type ) );
	crc32::process_buffer( &hash_crc, &this->m_random_seed, sizeof( this->m_random_seed ) );
	crc32::process_buffer( &hash_crc, &this->m_mouse_delta_x, sizeof( this->m_mouse_delta_x ) );
	crc32::process_buffer( &hash_crc, &this->m_mouse_delta_y, sizeof( this->m_mouse_delta_y ) );
	crc32::end( &hash_crc );

	return hash_crc;
}