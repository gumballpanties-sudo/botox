#include "sound_esp.h"

void n_sound_esp::impl_t::think( )
{
	if ( !g_interfaces.m_engine_sound || !g_interfaces.m_global_vars_base || !g_ctx.m_local )
		return;

	this->m_sounds.remove_all( );
	g_interfaces.m_engine_sound->get_active_sounds( this->m_sounds );

	if ( !this->m_sounds.count( ) )
		return;

	const float now      = g_interfaces.m_global_vars_base->m_real_time;
	const int local_slot = g_interfaces.m_engine_client->get_local_player( );

	const float min_volume      = GET_VARIABLE( g_variables.m_sound_esp_min_volume, float );
	const float max_distance    = GET_VARIABLE( g_variables.m_sound_esp_max_distance, float );
	const c_vector local_origin = g_ctx.m_local->get_abs_origin( );

	for ( int i = 0; i < this->m_sounds.count( ); i++ ) {
		const auto& sound = this->m_sounds[ i ];

		if ( sound.m_sound_source <= 0 || sound.m_sound_source > 64 || sound.m_sound_source == local_slot )
			continue;

		if ( sound.m_volume < min_volume )
			continue;

		if ( !sound.m_origin )
			continue;

		if ( max_distance > 0.f && local_origin.dist_to( *sound.m_origin ) > max_distance )
			continue;

		const auto player = g_interfaces.m_client_entity_list->get< c_base_entity >( sound.m_sound_source );
		if ( !player || !g_ctx.m_local->is_enemy( player ) )
			continue;

		this->m_last_sound[ sound.m_sound_source ] = now;

		this->m_origin[ sound.m_sound_source ] = *sound.m_origin;
	}
}

void n_sound_esp::impl_t::reset( )
{
	for ( int i = 0; i < 65; i++ ) {
		this->m_last_sound[ i ] = 0.f;
		this->m_origin[ i ]     = c_vector( 0.f, 0.f, 0.f );
	}
}

float n_sound_esp::impl_t::alpha( const int index, const float duration, const bool fade ) const
{
	if ( index <= 0 || index > 64 || duration <= 0.f )
		return 0.f;

	const float last = this->m_last_sound[ index ];
	if ( last <= 0.f )
		return 0.f;

	const float elapsed = g_interfaces.m_global_vars_base->m_real_time - last;
	if ( elapsed < 0.f || elapsed > duration )
		return 0.f;

	if ( !fade )
		return 1.f;

	const float hold = duration * 0.5f;

	return elapsed <= hold ? 1.f : 1.f - ( elapsed - hold ) / ( duration - hold );
}

float n_sound_esp::impl_t::player_gate( c_base_entity* player ) const
{
	// same gate as frame_stage_notify's think(): esp off = option dead, stamps would be stale
	if ( !GET_VARIABLE( g_variables.m_players, bool ) || !GET_VARIABLE( g_variables.m_players_sound_only, bool ) )
		return 1.f;

	if ( !player || !g_ctx.m_local || player == g_ctx.m_local || !g_ctx.m_local->is_enemy( player ) )
		return 1.f;

	return this->alpha( player->get_index( ), GET_VARIABLE( g_variables.m_players_sound_duration, float ),
	                    GET_VARIABLE( g_variables.m_players_sound_fade, bool ) );
}
