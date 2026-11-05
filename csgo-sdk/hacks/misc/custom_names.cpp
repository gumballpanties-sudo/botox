#include "misc.h"
#include "names_pure.h"
#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"
#include "../../hooks/hooks.h"
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

/* custom names, fpsoverlay names.cpp + alias list port (botox-tf2 hacks/misc/names.cpp is the same feature):
     player list name  per-player name (misc > player list "custom name", saved by xuid), anyone, you too. wins
     custom name       your own name, optional cycle list
     streamer mode     others as team / enemy label or one label, optional cycle lists
   IVEngineClient::GetPlayerInfo slot 8, name patched after the original: hud, scoreboard, killfeed, chat, esp all read
   it (C_PlayerResource copies it once a second). client side only: a gotv / server-side demo keeps the real names */
namespace
{
	/* fpsoverlay g_nameIdx / g_teamLabelIdx / ... + their last-change times */
	n_names::Cycle s_name_cycle, s_team_cycle, s_enemy_cycle, s_everyone_cycle;

	/* last unpatched name per slot (player list's name column) */
	char s_real[ 65 ][ 128 ] = { };

	int team_of( int index )
	{
		auto* entity = index > 0 ? g_interfaces.m_client_entity_list->get< c_base_entity >( index ) : nullptr;
		return entity ? entity->get_team( ) : 0;
	}

	/* fpsoverlay: the timer ticks a list only when it is read */
	const char* from_list( const std::string& fixed, bool cycle, std::uint32_t rows_var, std::uint32_t count_var, n_names::Cycle& c )
	{
		const auto& rows = GET_VARIABLE( rows_var, std::vector< std::string > );
		const int count  = std::clamp( GET_VARIABLE( count_var, int ), 0, ( std::min )( n_names::kMaxNames, static_cast< int >( rows.size( ) ) ) );

		if ( cycle && count > 0 && GET_VARIABLE( g_variables.m_name_trigger, int ) == 0 && g_interfaces.m_global_vars_base )
			n_names::Tick( c, count, g_interfaces.m_global_vars_base->m_current_time, GET_VARIABLE( g_variables.m_name_interval, float ) );

		return n_names::Choose( fixed.c_str( ), cycle, c, count, [ &rows ]( int i ) { return rows[ i ].c_str( ); } );
	}

	/* fpsoverlay GetStreamedPlayerName. nullptr / "" = real name */
	const char* streamed( int index )
	{
		const int local = g_interfaces.m_engine_client->get_local_player( );
		if ( index == local ) /* streamer mode never masks you; custom name still applies */
			return GET_VARIABLE( g_variables.m_use_custom_username, bool )
			           ? from_list( GET_VARIABLE( g_variables.m_custom_username, std::string ), GET_VARIABLE( g_variables.m_enable_name_cycle, bool ),
			                        g_variables.m_name_cycle, g_variables.m_name_cycle_count, s_name_cycle )
			           : nullptr;

		switch ( GET_VARIABLE( g_variables.m_streamer_mode, int ) ) {
		case 1: {
			if ( local < 1 )
				return "player";
			const int local_team = team_of( local ), team = team_of( index );
			if ( local_team == 0 || team == 0 )
				return "player";
			if ( team == local_team )
				return from_list( GET_VARIABLE( g_variables.m_streamer_team_label, std::string ), GET_VARIABLE( g_variables.m_enable_team_label_cycle, bool ),
				                  g_variables.m_team_label_cycle, g_variables.m_team_label_cycle_count, s_team_cycle );
			return from_list( GET_VARIABLE( g_variables.m_streamer_enemy_label, std::string ), GET_VARIABLE( g_variables.m_enable_enemy_label_cycle, bool ),
			                  g_variables.m_enemy_label_cycle, g_variables.m_enemy_label_cycle_count, s_enemy_cycle );
		}
		case 2:
			return from_list( GET_VARIABLE( g_variables.m_streamer_everyone_label, std::string ), GET_VARIABLE( g_variables.m_enable_everyone_label_cycle, bool ),
			                  g_variables.m_everyone_label_cycle, g_variables.m_everyone_label_cycle_count, s_everyone_cycle );
		default:
			return nullptr;
		}
	}

	/* fpsoverlay GetPlayerName_Masked order: player list name, then streamer / custom name. false = real */
	bool resolve( int index, const player_info_t& info, char* out, std::size_t size )
	{
		if ( !g_interfaces.m_engine_client || !g_interfaces.m_client_entity_list )
			return false;

		/* never player_list_get here: it calls get_player_info = this hook again. stamp only reads info */
		if ( index >= 1 && index <= 64 && !info.m_is_hltv ) {
			player_list_stamp( index, info );
			const auto& entry = g_player_list[ index ];
			if ( entry.m_user_id == info.m_user_id && entry.m_name[ 0 ] ) {
				strncpy_s( out, size, entry.m_name, _TRUNCATE );
				return true;
			}
		}

		if ( !GET_VARIABLE( g_variables.m_use_custom_username, bool ) && GET_VARIABLE( g_variables.m_streamer_mode, int ) == 0 )
			return false;

		const char* name = streamed( index );
		if ( !name || !name[ 0 ] )
			return false;
		strncpy_s( out, size, name, _TRUNCATE );
		return true;
	}
}

bool __fastcall n_detoured_functions::get_player_info( void* ecx, [[maybe_unused]] void* edx, int index, player_info_t* info )
{
	static auto original = g_hooks.m_get_player_info.get_original< bool( __thiscall* )( void*, int, player_info_t* ) >( );
	HOOK_SCOPE_OR_BAIL( original( ecx, index, info ) );

	const bool ok = original( ecx, index, info );
	if ( ok && info && index >= 1 && index <= 64 )
		strncpy_s( s_real[ index ], info->m_name, _TRUNCATE );

	char name[ 128 ];
	if ( ok && info && resolve( index, *info, name, sizeof( name ) ) ) {
		std::memset( info->m_name, 0, sizeof( info->m_name ) );
		std::memcpy( info->m_name, name, strnlen( name, sizeof( info->m_name ) - 1 ) );
	}
	return ok;
}

const char* player_list_real_name( int index )
{
	return ( index >= 1 && index <= 64 ) ? s_real[ index ] : "";
}

namespace
{
	bool read_varint( const std::uint8_t*& at, const std::uint8_t* end, std::uint64_t& value )
	{
		value = 0;
		for ( int shift = 0; at < end && shift < 64; shift += 7 ) {
			const std::uint8_t byte = *at++;
			value |= static_cast< std::uint64_t >( byte & 0x7f ) << shift;
			if ( !( byte & 0x80 ) )
				return true;
		}
		return false;
	}

	void write_varint( std::string& out, std::uint64_t value )
	{
		do {
			const auto byte = static_cast< std::uint8_t >( value & 0x7f );
			value >>= 7;
			out += static_cast< char >( value ? byte | 0x80 : byte );
		} while ( value );
	}
}

/* SayText: 1 ent_idx, 2 text (plugin line). SayText2: 1 ent_idx, 3 msg_name (raw = plugin line), 4 params, [0] = name.
   ent_idx is serialized first, so it is known before any string. other fields copied byte for byte */
bool custom_names_rewrite_chat( const int type, const void* msg, const int size, std::string& out )
{
	if ( !msg || size <= 0 || !g_interfaces.m_engine_client )
		return false;

	const auto* at  = static_cast< const std::uint8_t* >( msg );
	const auto* end = at + size;
	std::string real, fake;
	bool param_seen = false, changed = false;
	out.clear( );

	while ( at < end ) {
		const auto* start = at;
		std::uint64_t key = 0;
		if ( !read_varint( at, end, key ) )
			return false;
		const auto field = key >> 3, wire = key & 7;

		if ( wire == 0 ) {
			std::uint64_t value = 0;
			if ( !read_varint( at, end, value ) )
				return false;
			player_info_t info{ };
			if ( field == 1 && value >= 1 && value <= 64 && g_interfaces.m_engine_client->get_player_info( static_cast< int >( value ), &info ) ) {
				real = s_real[ value ]; /* hook just refreshed it */
				fake = info.m_name;
				if ( real == fake )
					real.clear( );
			}
			out.append( reinterpret_cast< const char* >( start ), at - start );
			continue;
		}
		if ( wire != 2 )
			return false;

		std::uint64_t length = 0;
		if ( !read_varint( at, end, length ) || length > static_cast< std::uint64_t >( end - at ) )
			return false;
		std::string value( reinterpret_cast< const char* >( at ), static_cast< std::size_t >( length ) );
		at += length;

		const bool name_field = ( type == 5 && field == 2 ) || ( type == 6 && field == 3 && !value.empty( ) && value[ 0 ] != '#' ) ||
		                        ( type == 6 && field == 4 && !param_seen );
		if ( type == 6 && field == 4 )
			param_seen = true;

		if ( name_field && !real.empty( ) ) {
			if ( const auto pos = value.find( real ); pos != std::string::npos ) {
				value.replace( pos, real.size( ), fake );
				changed = true;
			}
		}
		write_varint( out, key );
		write_varint( out, value.size( ) );
		out += value;
	}
	return changed;
}

/* fpsoverlay h_FireEvent on-kill block, 1:1 */
void custom_names_on_kill( )
{
	if ( GET_VARIABLE( g_variables.m_name_trigger, int ) != 1 )
		return;

	const auto count = []( std::uint32_t var ) { return std::clamp( GET_VARIABLE( var, int ), 0, n_names::kMaxNames ); };
	const int mode   = GET_VARIABLE( g_variables.m_streamer_mode, int );

	if ( GET_VARIABLE( g_variables.m_use_custom_username, bool ) && GET_VARIABLE( g_variables.m_enable_name_cycle, bool ) )
		n_names::Step( s_name_cycle, count( g_variables.m_name_cycle_count ) );
	if ( mode == 1 ) {
		if ( GET_VARIABLE( g_variables.m_enable_team_label_cycle, bool ) )
			n_names::Step( s_team_cycle, count( g_variables.m_team_label_cycle_count ) );
		if ( GET_VARIABLE( g_variables.m_enable_enemy_label_cycle, bool ) )
			n_names::Step( s_enemy_cycle, count( g_variables.m_enemy_label_cycle_count ) );
	}
	if ( mode == 2 && GET_VARIABLE( g_variables.m_enable_everyone_label_cycle, bool ) )
		n_names::Step( s_everyone_cycle, count( g_variables.m_everyone_label_cycle_count ) );
}
