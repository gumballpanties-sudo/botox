#include "dormancy.h"
#include "../players.h"

void n_dormancy::impl_t::think( )
{
	m_cur_sound_list.remove_all( );
	g_interfaces.m_engine_sound->get_active_sounds( m_cur_sound_list );

	if ( !m_cur_sound_list.count( ) )
		return;

	for ( int i = 0; i < m_cur_sound_list.count( ); i++ ) {
		auto& sound = m_cur_sound_list[ i ];

		if ( sound.m_sound_source <= 0 || sound.m_sound_source > 64 || sound.m_origin->is_zero( ) )
			continue;

		if ( !is_valid_sound( sound ) )
			continue;

		auto player = g_interfaces.m_client_entity_list->get< c_base_entity >( sound.m_sound_source );

		if ( !player->is_valid_enemy( ) )
			continue;

		setup_adjust_player( player, sound );

		m_sound_players[ sound.m_sound_source ].override_info( sound );
	}

	m_vec_sound_buffer = m_cur_sound_list;
}

void n_dormancy::impl_t::setup_adjust_player( c_base_entity* player, sound_info_t& sound )
{
	c_vector source{ }, dest{ };
	trace_t trace;
	c_trace_filter filter( player );

	source = *sound.m_origin + c_vector( 0.0f, 0.0f, 1.0f );
	dest   = source - c_vector( 0.0f, 0.0f, 100.0f );

	ray_t ray( source, dest );

	g_interfaces.m_engine_trace->trace_ray( ray, mask_playersolid, &filter, &trace );
	if ( trace.m_all_solid )
		m_sound_players[ sound.m_sound_source ].m_receive_time = -1;

	*sound.m_origin = trace.m_fraction <= 0.97f ? trace.m_end : *sound.m_origin;

	m_sound_players[ sound.m_sound_source ].m_flags = player->get_flags( );
	m_sound_players[ sound.m_sound_source ].m_flags |= ( trace.m_fraction < 0.50f ? fl_ducking : 0 ) | ( trace.m_fraction < 1.0f ? fl_onground : 0 );
	m_sound_players[ sound.m_sound_source ].m_flags &=
		( trace.m_fraction >= 0.50f ? ~fl_ducking : 0 ) | ( trace.m_fraction >= 1.0f ? ~fl_onground : 0 );
}

bool n_dormancy::impl_t::adjust_player_sound( c_base_entity* entity )
{
	const auto i            = entity->get_index( );
	const auto sound_player = m_sound_players[ i ];

	auto expired = false;

	if ( fabs( g_interfaces.m_global_vars_base->m_real_time - sound_player.m_receive_time ) > 10.0f )
		expired = true;

	entity->is_spotted( ) = true;
	entity->get_flags( )  = sound_player.m_flags;
	entity->set_abs_origin( sound_player.m_abs_origin );

	return !expired;
}

bool n_dormancy::impl_t::is_valid_sound( sound_info_t& sound )
{
	for ( auto i = 0; i < m_vec_sound_buffer.count( ); i++ )
		if ( m_vec_sound_buffer[ i ].m_uid == sound.m_uid )
			return false;

	return true;
}
