#include "serial_render.h"
#include "../../../game/sdk/includes/includes.h"
#include "../../../globals/includes/includes.h"

bool n_serial_render::impl_t::request( )
{
	this->m_wanted_frame = g_interfaces.m_global_vars_base ? g_interfaces.m_global_vars_base->m_frame_count : 0;

	if ( this->m_backup != k_untouched ) {
		if ( c_cconvar* queue_mode = g_convars[ HASH_BT( "mat_queue_mode" ) ] )
			queue_mode->set_value( this->m_backup );

		this->m_backup        = k_untouched;
		this->m_serial_frames = 0;
	}

	return true;
}

void n_serial_render::impl_t::idle( )
{
	if ( this->m_backup == k_untouched )
		return;

	const int frame = g_interfaces.m_global_vars_base ? g_interfaces.m_global_vars_base->m_frame_count : 0;

	if ( frame - this->m_wanted_frame < 2 )
		return;

	if ( c_cconvar* queue_mode = g_convars[ HASH_BT( "mat_queue_mode" ) ] )
		queue_mode->set_value( this->m_backup );

	this->m_backup        = k_untouched;
	this->m_serial_frames = 0;
}

void n_serial_render::impl_t::on_release( )
{
	if ( this->m_backup == k_untouched )
		return;

	if ( c_cconvar* queue_mode = g_convars[ HASH_BT( "mat_queue_mode" ) ] )
		queue_mode->set_value( this->m_backup );

	this->m_backup = k_untouched;
}
