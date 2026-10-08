#include "lagcomp.h"
#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"
#include "../../globals/logger/logger.h"
#include "../entity_cache/entity_cache.h"
#include "../chams/chams.h"

#include <algorithm>
#include <cmath>

extern void botox_dbg_log( const char* fmt, ... );

float n_lagcomp::impl_t::lerp_time( )
{
	const auto cl_updaterate    = g_convars[ HASH_BT( "cl_updaterate" ) ];
	const auto sv_minupdaterate = g_convars[ HASH_BT( "sv_minupdaterate" ) ];
	const auto sv_maxupdaterate = g_convars[ HASH_BT( "sv_maxupdaterate" ) ];
	const auto cl_interp        = g_convars[ HASH_BT( "cl_interp" ) ];
	const auto cl_interp_ratio  = g_convars[ HASH_BT( "cl_interp_ratio" ) ];
	const auto min_ratio        = g_convars[ HASH_BT( "sv_client_min_interp_ratio" ) ];
	const auto max_ratio        = g_convars[ HASH_BT( "sv_client_max_interp_ratio" ) ];

	if ( !cl_interp || !cl_interp_ratio )
		return 0.f;

	float ratio = cl_interp_ratio->get_float( );
	if ( ratio == 0.f )
		ratio = 1.f;

	if ( min_ratio && max_ratio && min_ratio->get_float( ) != -1.f )
		ratio = std::clamp( ratio, min_ratio->get_float( ), std::max( min_ratio->get_float( ), max_ratio->get_float( ) ) );

	/* max then min = clamp( rate, min, max ) when min <= max, and never UB when a server sets them crossed */
	float rate = cl_updaterate ? cl_updaterate->get_float( ) : 64.f;
	if ( sv_maxupdaterate )
		rate = std::min( rate, sv_maxupdaterate->get_float( ) );
	if ( sv_minupdaterate )
		rate = std::max( rate, sv_minupdaterate->get_float( ) );

	if ( rate <= 0.f )
		return cl_interp->get_float( );

	return std::max( cl_interp->get_float( ), ratio / rate );
}

float n_lagcomp::impl_t::max_unlag( )
{
	/* remote sv_maxunlag is never sent to clients; local copy is server.dll's, not theirs. csgo default 0.2 (player_lagcompensation.cpp:36) */
	if ( !is_hosting( ) )
		return 0.2f;

	const auto unlag = g_convars[ HASH_BT( "sv_maxunlag" ) ];
	return unlag ? unlag->get_float( ) : 0.2f;
}

float n_lagcomp::impl_t::real_latency( )
{
	const auto net_channel = g_interfaces.m_engine_client->get_net_channel_info( );
	if ( !net_channel )
		return 0.f;

	return net_channel->get_latency( FLOW_OUTGOING ) + net_channel->get_latency( FLOW_INCOMING );
}

bool n_lagcomp::impl_t::is_hosting( )
{
	if ( !g_convars[ HASH_BT( "sv_lagpushticks" ) ] )
		return false;

	const auto nci = g_interfaces.m_engine_client->get_net_channel_info( );

	return !nci || nci->is_loopback( );
}

float n_lagcomp::impl_t::extend( )
{
	if ( !GET_VARIABLE( g_variables.m_backtrack_enable, bool ) || !GET_VARIABLE( g_variables.m_backtrack_extend, bool ) )
		return 0.f;

	const float wanted = static_cast< float >( GET_VARIABLE( g_variables.m_backtrack_extend_amount, int ) ) / 1000.f;

	return std::clamp( wanted, 0.f, std::min( max_unlag( ), k_max_backtrack ) );
}

float n_lagcomp::impl_t::time_limit( )
{
	const float wanted = static_cast< float >( GET_VARIABLE( g_variables.m_backtrack_time_limit, int ) ) / 1000.f;

	return std::clamp( wanted, 0.f, k_max_backtrack ) + extend( );
}

float n_lagcomp::impl_t::correct_time( )
{
	return std::clamp( real_latency( ) + lerp_time( ), 0.f, max_unlag( ) );
}

float n_lagcomp::impl_t::window_center( )
{
	return correct_time( ) + ( is_hosting( ) ? extend( ) : 0.f );
}

bool n_lagcomp::impl_t::live_accepted( )
{
	return window_center( ) - lerp_time( ) <= 0.2f;
}

float n_lagcomp::impl_t::server_time( )
{
	return static_cast< float >( m_server_tick ) * g_interfaces.m_global_vars_base->m_interval_per_tick;
}

void n_lagcomp::impl_t::begin_command( c_user_cmd* cmd )
{
	if ( g_ctx.m_local && ( !m_last_cmd || m_last_cmd->m_has_been_predicted ) )
		m_server_tick = g_ctx.m_local->get_tick_base( );
	else
		m_server_tick++;

	m_last_cmd    = cmd;
	m_shot        = e_shot::none;
	m_record_push = 0;
}

void n_lagcomp::impl_t::refresh_window( )
{
	if ( const int now_frame = g_interfaces.m_global_vars_base->m_frame_count; now_frame != m_window_frame ) {
		m_window_frame  = now_frame;
		m_window_center = window_center( );
		m_window_unlag  = max_unlag( );
		m_window_limit  = time_limit( );
	}
}

bool n_lagcomp::impl_t::is_valid( const float sim_time, const float margin )
{
	refresh_window( );

	const float now = server_time( );
	if ( sim_time <= 0.f || sim_time < std::floor( now + margin - m_window_unlag ) )
		return false;

	return std::fabsf( m_window_center - ( now - sim_time ) ) <= 0.2f - margin;
}

bool n_lagcomp::impl_t::usable( c_base_entity* entity, const float sim_time )
{
	if ( !entity || !is_valid( sim_time ) )
		return false;

	return entity->get_simulation_time( ) - sim_time <= m_window_limit + 1e-3f;
}

void n_lagcomp::impl_t::commit_shot( record_t* record )
{
	g_ctx.m_record = record;

	if ( !record ) {
		m_shot = e_shot::live;
		return;
	}

	m_shot        = e_shot::record;
	m_record_push = 0;

	if ( is_hosting( ) ) {
		const float interval = g_interfaces.m_global_vars_base->m_interval_per_tick;
		m_record_push        = static_cast< int >( std::floor( ( server_time( ) - record->m_sim_time - correct_time( ) ) / interval + 0.5f ) );
	}

	g_ctx.m_cmd->m_tick_count = g_math.time_to_ticks( record->m_sim_time + lerp_time( ) ) + m_record_push;
}

int n_lagcomp::impl_t::push_ticks( )
{
	if ( !is_hosting( ) )
		return 0;

	switch ( m_shot ) {
	case e_shot::record:
		return m_record_push;
	case e_shot::live:
		return 0;
	default:
		return live_accepted( ) ? 0 : g_math.time_to_ticks( std::min( window_center( ), max_unlag( ) ) - lerp_time( ) );
	}
}

void n_lagcomp::impl_t::on_frame_stage_notify( )
{
	bool build = true;
	if ( GET_VARIABLE( g_variables.m_performance, bool ) &&GET_VARIABLE( g_variables.m_performance_skip_unused_records, bool ) )
		build = GET_VARIABLE( g_variables.m_backtrack_enable, bool ) || GET_VARIABLE( g_variables.m_players_backtrack_trail, bool ) ||
		        ( GET_VARIABLE( g_variables.m_chams_enable, bool ) &&
		          n_chams::impl_t::has_layers( GET_VARIABLE( g_variables.m_chams_backtrack_layers, std::vector< int > ) ) );

	g_entity_cache.enumerate( e_enumeration_type::type_players, [ & ]( c_base_entity* entity ) {
		if ( !entity || !entity->is_valid_aim_target( ) )
			return;

		const auto index = entity->get_index( );

		auto& record_list = this->m_records[ index ];
		auto& location    = this->m_record_location[ index ];

		if ( record_list ) {
			for ( int j = 0; j < g_ctx.m_max_allocations; j++ ) {
				auto& record = record_list[ j ];

				if ( record.m_player != index && record.m_player != -1 ) {
					if ( g_ctx.m_record >= record_list && g_ctx.m_record < record_list + max_records )
						g_ctx.m_record = nullptr;

					for ( int k = 0; k < max_records; k++ )
						record_list[ k ] = record_t{ };

					location = 0;
					break;
				}

				record.m_valid = usable( entity, record.m_sim_time );
			}
		}

		if ( !build )
			return;

		if ( !record_list ) {
			record_list = new record_t[ max_records ];
			location    = 0;
		}

		if ( location >= g_ctx.m_max_allocations )
			location = 0;

		const int previous_slot = ( location > 0 ? location : std::max( 1, g_ctx.m_max_allocations ) ) - 1;

		if ( record_list[ previous_slot ].m_sim_time == entity->get_simulation_time( ) )
			return;

		record_t new_record{ };

		new_record.m_player   = index;
		new_record.m_sim_time = entity->get_simulation_time( );
		new_record.m_valid    = usable( entity, new_record.m_sim_time );
		new_record.m_vec_origin = entity->get_origin( );

		if ( const auto& previous = record_list[ previous_slot ];
		     previous.m_player == index && previous.m_sim_time > 0.f &&
		     ( new_record.m_vec_origin - previous.m_vec_origin ).length_squared( ) > 64.f * 64.f ) {
			if ( g_ctx.m_record >= record_list && g_ctx.m_record < record_list + max_records )
				g_ctx.m_record = nullptr;

			for ( int k = 0; k < max_records; k++ )
				record_list[ k ] = record_t{ };

			location = 0;
		}

		const auto saved_origin  = entity->get_abs_origin( );
		const int saved_effects  = entity->get_effects( );
		const auto entity_address = reinterpret_cast< std::uintptr_t >( entity );

		*reinterpret_cast< int* >( entity_address + 0xA30 ) = g_interfaces.m_global_vars_base->m_frame_count;
		*reinterpret_cast< int* >( entity_address + 0xA28 ) = 0;

		*reinterpret_cast< c_vector* >( entity_address + 0xA0 ) = entity->get_origin( );

		*reinterpret_cast< int* >( entity_address + 0xA68 ) = 0;

		entity->invalidate_bone_cache( );
		entity->get_effects( ) |= 8;

		const bool built = entity->setup_bones( new_record.m_matrix, 128, 0x7FF00, g_interfaces.m_global_vars_base->m_current_time );

		entity->set_abs_origin( saved_origin );
		entity->get_effects( ) = saved_effects;

		if ( !built )
			return;

		memcpy( &record_list[ location ], &new_record, sizeof( record_t ) );

		location++;
	} );
}

n_lagcomp::impl_t::record_t* n_lagcomp::impl_t::oldest_record( const int index )
{
	if ( index < 0 || index >= static_cast< int >( m_records.size( ) ) || !g_lagcomp.m_records[ index ] )
		return nullptr;

	record_t* oldest = nullptr;

	for ( int i = 0; i < g_ctx.m_max_allocations; i++ ) {
		auto* current_record = &g_lagcomp.m_records[ index ][ i ];

		if ( current_record->m_valid && ( !oldest || current_record->m_sim_time < oldest->m_sim_time ) )
			oldest = current_record;
	}

	return oldest;
}

void n_lagcomp::impl_t::update_incoming_sequences( c_net_channel* net_channel )
{
	if ( !net_channel )
		return;

	if ( net_channel->m_in_sequence_nr < m_last_incoming_sequence )
		clear_incoming_sequences( );

	if ( m_last_incoming_sequence == 0 ) {
		m_last_incoming_sequence = net_channel->m_in_sequence_nr;
		m_last_reliable_change   = net_channel->m_in_sequence_nr;
	}

	if ( net_channel->m_in_sequence_nr > m_last_incoming_sequence ) {
		if ( !m_sequences.empty( ) && m_sequences.front( ).m_in_reliable_state != net_channel->m_in_reliable_state )
			m_last_reliable_change = net_channel->m_in_sequence_nr;

		m_last_incoming_sequence = net_channel->m_in_sequence_nr;
		m_sequences.emplace_front( sequence_object_t( net_channel->m_in_reliable_state, net_channel->m_out_reliable_state,
		                                              net_channel->m_in_sequence_nr, g_interfaces.m_global_vars_base->m_real_time ) );
	}

	if ( m_sequences.size( ) > 2048U )
		m_sequences.pop_back( );
}

void n_lagcomp::impl_t::on_create_move_update( c_net_channel* net_channel )
{
	if ( extend( ) > 0.f && !is_hosting( ) )
		update_incoming_sequences( net_channel );
	else
		clear_incoming_sequences( );
}

void n_lagcomp::impl_t::clear_incoming_sequences( )
{
	if ( !m_sequences.empty( ) ) {
		m_last_incoming_sequence = 0;
		m_last_reliable_change   = 0;
		m_sequences.clear( );
	}

	m_last_spoofed = sequence_object_t{ 0, 0, 0, 0.f };

	g_ctx.m_extend_applied = 0.f;
}

void n_lagcomp::impl_t::add_latency_to_net_channel( c_net_channel* net_channel, float latency )
{
	const sequence_object_t* target = nullptr;

	for ( const auto& sequence : m_sequences ) {
		target = &sequence;

		if ( g_interfaces.m_global_vars_base->m_real_time - sequence.m_current_time >= latency )
			break;
	}

	if ( !target ) {
		g_ctx.m_extend_applied = 0.f;
		return;
	}

	bool frozen = false;
	if ( m_last_spoofed.m_sequence_nr != 0 && target->m_sequence_nr < m_last_spoofed.m_sequence_nr ) {
		target = &m_last_spoofed;
		frozen = true;
	} else
		m_last_spoofed = *target;

	net_channel->m_in_reliable_state = target->m_in_reliable_state;
	net_channel->m_in_sequence_nr    = target->m_sequence_nr;

	g_ctx.m_extend_applied = g_interfaces.m_global_vars_base->m_real_time - target->m_current_time;

	{
		static int last_bucket = -1;

		if ( const int bucket = static_cast< int >( g_ctx.m_extend_applied * 40.f ) + 4096 * static_cast< int >( window_center( ) * 40.f );
		     bucket != last_bucket ) {
			last_bucket = bucket;

			botox_dbg_log( "EXTEND: applied=%.0fms want=%.0fms centre=%.0fms in=%.0fms deque=%d seq=%d frozen=%d relchg=%d",
			                   g_ctx.m_extend_applied * 1000.f, latency * 1000.f, window_center( ) * 1000.f,
			                   ( g_interfaces.m_engine_client->get_net_channel_info( )
			                         ? g_interfaces.m_engine_client->get_net_channel_info( )->get_latency( FLOW_INCOMING )
			                         : 0.f ) *
			                       1000.f,
			                   ( int )m_sequences.size( ), target->m_sequence_nr, frozen ? 1 : 0, m_last_reliable_change );
		}
	}
}

void n_lagcomp::impl_t::on_release( )
{
	g_ctx.m_record = nullptr;

	for ( std::size_t index = 0U; index < this->m_records.size( ); index++ ) {
		delete[] this->m_records[ index ];

		this->m_records[ index ]         = nullptr;
		this->m_record_location[ index ] = 0;
	}

	this->m_sequences.clear( );
}
