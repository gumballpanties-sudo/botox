#include "misc.h"
#include "scaleform/image_cache.h"
#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"
#include "../../utilities/perf/perf_watch.h"
#include "../../hooks/hooks.h"
#include <algorithm>
#include <cstring>
#include <format>
#include <map>
#include <mutex>
#include <random>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
	constexpr int k_info_size   = 0x158;
	constexpr int k_xuid        = 0x08;
	constexpr int k_name        = 0x10;
	constexpr int k_name_size   = 128;
	constexpr int k_user_id     = 0x90;
	constexpr int k_friends_id  = 0xB8;
	constexpr int k_fake_player = 0x13C;
	constexpr int k_hltv        = 0x13D;

	constexpr uint64_t k_steam_id_base = 76561197960265728ull;
	constexpr size_t k_pool_want       = 32;

	struct profile_t {
		std::string m_name  = { };
		uint64_t m_xuid     = 0;
		uint64_t m_asked_at = 0;
		bool m_ready        = false;
		int m_uses          = 0;
	};

	enum e_field { field_ping, field_rank, field_wins, field_level, field_medal, field_count };

	constexpr const char* k_fields[ field_count ] = { "m_iPing", "m_iCompetitiveRanking", "m_iCompetitiveWins", "m_nPersonaDataPublicLevel",
	                                                  "m_nActiveCoinRank" };

	/* coin/pin item def ids, checked against items_game.txt */
	constexpr int k_medals[] = { 874,  969,  904,  941,  1331, 1332, 1339, 1340, 1341, 1357, 1358, 1359, 1367, 1368, 1369, 1376, 1377,
	                             1378, 4674, 4675, 4676, 4737, 4738, 4739, 4819, 4820, 4821, 4873, 4874, 4875, 6001, 6002, 6003, 6004,
	                             6006, 6008, 6009, 6010, 6011, 6014, 6015, 6017, 6018, 6019, 6024, 6031, 6032 };

	struct bot_t {
		std::vector< uint8_t > m_original = { };
		std::vector< uint8_t > m_written  = { };
		bool m_assigned                   = false;
		profile_t m_profile               = { };
		int m_stats[ field_count ]        = { };
	};

	/* key = raw userid bytes, unique per connection: a slot reused by a human is never ours */
	std::unordered_map< int, bot_t > s_bots;
	uint32_t s_signature = 0;
	int s_generation     = 0;
	std::mt19937 s_rng{ std::random_device{ }( ) };

	void roll_stats( bot_t& bot )
	{
		const auto pick = []( int low, int high ) { return std::uniform_int_distribution< int >( low, high )( s_rng ); };

		bot.m_stats[ field_ping ]  = pick( 17, 155 );
		bot.m_stats[ field_rank ]  = pick( 1, 18 );
		bot.m_stats[ field_wins ]  = pick( 10, 600 );
		bot.m_stats[ field_level ] = pick( 1, 40 );
		bot.m_stats[ field_medal ] = pick( 0, 2 ) ? k_medals[ pick( 0, static_cast< int >( std::size( k_medals ) ) - 1 ) ] : 0;
	}

	/* writes go into the LISTEN SERVER's CCSPlayerResource so pov demos and gotv record them. its think (0.1s) puts the real
	   values back with Set( ), which flags the slot changed; PreClientUpdate runs after that and right before the snapshot,
	   so ours is what gets packed. stop writing = the next think restores. live server.dll disasm 10-09: IServerGameDLL
	   GetAllServerClasses 10 / PreClientUpdate 5, IServerTools GetIServerEntity 1 / FirstEntity 7 / NextEntity 8,
	   SendProp 0x54 bytes: name +0x30, table +0x48, offset +0x4C (low 20 bits) */
	constexpr int k_prop_size    = 0x54;
	constexpr int k_prop_name    = 0x30;
	constexpr int k_prop_table   = 0x48;
	constexpr int k_prop_offset  = 0x4C;
	constexpr int k_offset_mask  = ( 1 << 20 ) - 1;
	constexpr int k_max_slots    = 65;

	struct server_t {
		void* m_game_dll                         = nullptr;
		void* m_tools                            = nullptr;
		void* m_resource_vtable                  = nullptr;
		std::uintptr_t m_offsets[ field_count ]  = { };
		std::mutex m_lock;
		int m_values[ k_max_slots ][ field_count ] = { };
		uint8_t m_masks[ k_max_slots ]           = { };
	} s_server;

	void find_offsets( const uint8_t* table, std::uintptr_t base )
	{
		const auto* props = *reinterpret_cast< const uint8_t* const* >( table );
		const int count   = *reinterpret_cast< const int* >( table + 4 );

		for ( int i = 0; props && i < count; ++i ) {
			const uint8_t* prop = props + i * k_prop_size;
			const char* name    = *reinterpret_cast< const char* const* >( prop + k_prop_name );
			const auto offset   = base + ( *reinterpret_cast< const int* >( prop + k_prop_offset ) & k_offset_mask );
			if ( !name )
				continue;

			if ( !std::strcmp( name, "baseclass" ) ) {
				if ( const auto* child = *reinterpret_cast< const uint8_t* const* >( prop + k_prop_table ) )
					find_offsets( child, offset );
				continue;
			}

			for ( int field = 0; field < field_count; ++field )
				if ( !std::strcmp( name, k_fields[ field ] ) )
					s_server.m_offsets[ field ] = offset;
		}
	}

	/* main thread, retried until hooked: interfaces, offsets, the resource class vtable, then the hook */
	void server_setup( )
	{
		static uint64_t next = 0;
		const uint64_t now   = GetTickCount64( );
		if ( g_hooks.m_pre_client_update.is_hooked( ) || now < next )
			return;
		next = now + 2000;

		if ( !s_server.m_game_dll ) {
			void* sv = GetModuleHandleA( "server.dll" );
			if ( !sv )
				return;

			module_t module( sv, "server.dll" );
			s_server.m_game_dll = module.find_interface( "ServerGameDLL005" );
			s_server.m_tools    = module.find_interface( "VSERVERTOOLS001" );
			if ( !s_server.m_game_dll || !s_server.m_tools ) {
				s_server.m_game_dll = nullptr;
				return;
			}

			for ( auto* cls = g_virtual.call< const uint8_t* >( s_server.m_game_dll, 10 ); cls;
			      cls       = *reinterpret_cast< const uint8_t* const* >( cls + 8 ) ) {
				const char* name = *reinterpret_cast< const char* const* >( cls );
				if ( name && !std::strcmp( name, "CCSPlayerResource" ) ) {
					find_offsets( *reinterpret_cast< const uint8_t* const* >( cls + 4 ), 0 );
					break;
				}
			}

			g_console.print( std::format( "[bot names] server offsets ping {:x} rank {:x} wins {:x} level {:x} medal {:x}", s_server.m_offsets[ 0 ],
			                              s_server.m_offsets[ 1 ], s_server.m_offsets[ 2 ], s_server.m_offsets[ 3 ], s_server.m_offsets[ 4 ] )
			                     .c_str( ) );
		}

		if ( !s_server.m_resource_vtable ) {
			auto* client_resource = find_player_resource( );
			void* resource        = client_resource ? g_virtual.call< void*, void* >( s_server.m_tools, 1, client_resource ) : nullptr;
			if ( !resource )
				return;
			s_server.m_resource_vtable = *reinterpret_cast< void** >( resource );
		}

		const bool ok = g_hooks.m_pre_client_update.create( g_virtual.get( s_server.m_game_dll, 5 ), &n_detoured_functions::pre_client_update );
		g_console.print( std::format( "[bot names] server PreClientUpdate hook {}", ok ? "ok" : "FAILED" ).c_str( ) );
	}

	void set_server_values( const std::map< std::pair< int, int >, int >& wanted )
	{
		std::lock_guard< std::mutex > lock( s_server.m_lock );
		std::memset( s_server.m_masks, 0, sizeof( s_server.m_masks ) );
		for ( const auto& [ key, value ] : wanted ) {
			s_server.m_values[ key.first ][ key.second ] = value;
			s_server.m_masks[ key.first ] |= 1 << key.second;
		}
	}

	/* steam pool: worker appends, main thread reads */
	std::mutex s_pool_lock;
	std::vector< profile_t > s_pool;
	bool s_fetch_done = false;

	/* steam hands strangers a shared grey "?" handle until the face downloads, and the hud avatar keeps whatever it got
	   first. a handle on 2+ xuids = that placeholder. profile handed out only once its own face is in */
	std::unordered_set< int > s_placeholders;
	constexpr uint64_t k_warm_give_up_ms = 15'000;

	void warm_pool( )
	{
		static uint64_t next = 0;
		const uint64_t now   = GetTickCount64( );
		if ( !SteamFriends || now < next )
			return;
		next = now + 500;

		struct seen_t {
			profile_t* m_profile;
			int m_small, m_medium, m_large;
		};
		std::vector< seen_t > seen;
		std::unordered_map< int, int > counts;

		std::lock_guard< std::mutex > lock( s_pool_lock );
		for ( auto& entry : s_pool ) {
			if ( entry.m_ready )
				continue;

			if ( !entry.m_asked_at )
				entry.m_asked_at = now;

			const CSteamID id( static_cast< uint64 >( entry.m_xuid ) );
			if ( SteamFriends->RequestUserInformation( id, false ) && now - entry.m_asked_at < k_warm_give_up_ms )
				continue;

			seen.push_back( { &entry, SteamFriends->GetSmallFriendAvatar( id ), SteamFriends->GetMediumFriendAvatar( id ),
			                  SteamFriends->GetLargeFriendAvatar( id ) } );
			++counts[ seen.back( ).m_small ];
			++counts[ seen.back( ).m_medium ];
		}

		for ( const auto& [ handle, count ] : counts )
			if ( handle > 0 && count > 1 )
				s_placeholders.insert( handle );

		for ( const auto& s : seen ) {
			const bool face = !s_placeholders.empty( ) && s.m_small > 0 && s.m_medium > 0 && s.m_large > 0 &&
			                  !s_placeholders.contains( s.m_small ) && !s_placeholders.contains( s.m_medium );
			if ( !face && now - s.m_profile->m_asked_at < k_warm_give_up_ms )
				continue;

			s.m_profile->m_ready = true;
			g_console.print( std::format( "[bot names] warm '{}' xuid {} handles {}/{}/{} {} ms{}", s.m_profile->m_name, s.m_profile->m_xuid, s.m_small,
			                              s.m_medium, s.m_large, now - s.m_profile->m_asked_at, face ? "" : " GAVE UP" )
			                     .c_str( ) );
		}
	}

	void put_be( uint8_t* out, uint64_t value, int bytes )
	{
		for ( int i = 0; i < bytes; ++i )
			out[ i ] = static_cast< uint8_t >( value >> ( 8 * ( bytes - 1 - i ) ) );
	}

	void fetch_worker( )
	{
		n_perf::background_thread( );

		std::mt19937 rng{ std::random_device{ }( ) };
		std::uniform_int_distribution< uint32_t > account( 20'000'000u, 420'000'000u );

		int failures = 0;
		for ( size_t attempt = 0; attempt < k_pool_want * 10 && failures < 6 && !g_ctx.m_unloading; ++attempt ) {
			{
				std::lock_guard< std::mutex > lock( s_pool_lock );
				if ( s_pool.size( ) >= k_pool_want )
					break;
			}

			const uint64_t xuid = k_steam_id_base + account( rng );
			std::string body;
			if ( !n_image_cache::http_get_text( std::format( "https://steamcommunity.com/profiles/{}/?xml=1", xuid ), body ) ) {
				++failures;
				continue;
			}
			failures = 0;

			constexpr std::string_view tag = "<steamID><![CDATA[";
			const size_t begin             = body.find( tag );
			if ( begin == std::string::npos )
				continue;

			const size_t name_at = begin + tag.size( );
			const size_t end     = body.find( "]]>", name_at );
			if ( end == std::string::npos || end == name_at )
				continue;

			constexpr std::string_view default_avatar = "fef49e7fa7e1997310d705b2a6158ff8dc1cdfeb";
			const size_t icon_at                      = body.find( "<avatarIcon>" );
			if ( icon_at == std::string::npos || body.find( default_avatar, icon_at ) < body.find( "</avatarIcon>", icon_at ) )
				continue;

			std::lock_guard< std::mutex > lock( s_pool_lock );
			s_pool.push_back( { body.substr( name_at, std::min< size_t >( end - name_at, k_name_size - 1 ) ), xuid } );
		}

		{
			std::lock_guard< std::mutex > lock( s_pool_lock );
			s_fetch_done = true;
		}
		g_bot_names_fetching = false;
	}

	bool make_profile( profile_t& out )
	{
		switch ( GET_VARIABLE( g_variables.m_bot_names_mode, int ) ) {
		case 1:
			out.m_name = GET_VARIABLE( g_variables.m_bot_names_same, std::string ).substr( 0, k_name_size - 1 );
			return true;
		case 2: {
			static constexpr char k_chars[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
			std::uniform_int_distribution< int > pick( 0, static_cast< int >( sizeof( k_chars ) ) - 2 );

			const int length = std::clamp( GET_VARIABLE( g_variables.m_bot_names_length, int ), 1, 32 );
			out.m_name.clear( );
			for ( int i = 0; i < length; ++i )
				out.m_name += k_chars[ pick( s_rng ) ];
			return true;
		}
		case 3: {
			std::lock_guard< std::mutex > lock( s_pool_lock );
			profile_t* best = nullptr;
			bool warming    = !s_fetch_done;
			for ( auto& entry : s_pool ) {
				if ( !entry.m_ready )
					warming |= !entry.m_uses;
				else if ( !best || entry.m_uses < best->m_uses )
					best = &entry;
			}

			/* reuse a name only once nothing fresh is still coming */
			if ( !best || ( best->m_uses && warming ) )
				return false;

			++best->m_uses;
			out = *best;
			return true;
		}
		default:
			out = { };
			return true;
		}
	}

	void start_fetch( )
	{
		{
			std::lock_guard< std::mutex > lock( s_pool_lock );
			if ( s_fetch_done || s_pool.size( ) >= k_pool_want )
				return;
		}

		if ( g_bot_names_fetching.exchange( true ) )
			return;

		std::thread( fetch_worker ).detach( );
	}

	uint32_t settings_signature( )
	{
		const std::string key = std::format( "{}|{}|{}|{}", GET_VARIABLE( g_variables.m_bot_names_mode, int ),
		                                     GET_VARIABLE( g_variables.m_bot_names_same, std::string ),
		                                     GET_VARIABLE( g_variables.m_bot_names_length, int ), s_generation );
		return HASH_RT( key.c_str( ) );
	}

	std::vector< uint8_t > patched( const bot_t& bot )
	{
		std::vector< uint8_t > data = bot.m_original;

		if ( !bot.m_profile.m_name.empty( ) ) {
			std::memset( data.data( ) + k_name, 0, k_name_size );
			std::memcpy( data.data( ) + k_name, bot.m_profile.m_name.data( ), bot.m_profile.m_name.size( ) );
		}

		if ( bot.m_profile.m_xuid ) {
			put_be( data.data( ) + k_xuid, bot.m_profile.m_xuid, 8 );
			put_be( data.data( ) + k_friends_id, static_cast< uint32_t >( bot.m_profile.m_xuid - k_steam_id_base ), 4 );
		}

		if ( GET_VARIABLE( g_variables.m_bot_names_no_prefix, bool ) )
			data[ k_fake_player ] = 0;

		return data;
	}

	void restore_all( c_network_string_table* table )
	{
		set_server_values( { } );

		for ( int i = 0; table && !s_bots.empty( ) && i < table->get_num_strings( ); ++i ) {
			int length       = 0;
			const auto* data = static_cast< const uint8_t* >( table->get_string_user_data( i, &length ) );
			if ( !data || length < k_info_size )
				continue;

			int user_id = 0;
			std::memcpy( &user_id, data + k_user_id, sizeof( user_id ) );

			if ( const auto it = s_bots.find( user_id ); it != s_bots.end( ) )
				table->set_string_user_data( i, static_cast< int >( it->second.m_original.size( ) ), it->second.m_original.data( ) );
		}

		s_bots.clear( );
	}
}

void bot_names_frame( )
{
	auto* table = g_interfaces.m_server_string_tables ? g_interfaces.m_server_string_tables->find_table( "userinfo" ) : nullptr;

	if ( !table || !GET_VARIABLE( g_variables.m_bot_names, bool ) || g_ctx.m_world_restore_requested.load( std::memory_order_acquire ) ) {
		restore_all( table );
		return;
	}

	if ( GET_VARIABLE( g_variables.m_bot_names_mode, int ) == 3 ) {
		start_fetch( );
		warm_pool( );
	}

	if ( const uint32_t signature = settings_signature( ); signature != s_signature ) {
		s_signature = signature;
		for ( auto& [ id, bot ] : s_bots ) {
			bot.m_assigned = false;
			roll_stats( bot );
		}
	}

	const bool fake_ping    = GET_VARIABLE( g_variables.m_bot_names_ping, bool );
	const bool fake_profile = GET_VARIABLE( g_variables.m_bot_names_profile, bool );
	if ( fake_ping || fake_profile )
		server_setup( );

	std::map< std::pair< int, int >, int > wanted;
	std::vector< int > seen;
	for ( int i = 0; i < table->get_num_strings( ); ++i ) {
		int length       = 0;
		const auto* data = static_cast< const uint8_t* >( table->get_string_user_data( i, &length ) );
		if ( !data || length < k_info_size )
			continue;

		int user_id = 0;
		std::memcpy( &user_id, data + k_user_id, sizeof( user_id ) );

		const bool engine_bot = data[ k_fake_player ] && !data[ k_hltv ];
		auto it               = s_bots.find( user_id );
		if ( it == s_bots.end( ) ) {
			if ( !engine_bot )
				continue;

			it = s_bots.emplace( user_id, bot_t{ } ).first;
			roll_stats( it->second );
		}

		auto& bot = it->second;
		seen.push_back( user_id );

		for ( int field = 0; field < field_count && i < 64; ++field )
			if ( field == field_ping ? fake_ping : fake_profile )
				wanted[ { i + 1, field } ] = bot.m_stats[ field ];

		if ( bot.m_written.empty( ) || std::memcmp( data, bot.m_written.data( ), k_info_size ) != 0 )
			bot.m_original.assign( data, data + length );

		if ( !bot.m_assigned && make_profile( bot.m_profile ) )
			bot.m_assigned = true;

		bot.m_written = patched( bot );
		if ( std::memcmp( data, bot.m_written.data( ), k_info_size ) != 0 )
			table->set_string_user_data( i, static_cast< int >( bot.m_written.size( ) ), bot.m_written.data( ) );
	}

	std::erase_if( s_bots, [ & ]( const auto& entry ) { return std::find( seen.begin( ), seen.end( ), entry.first ) == seen.end( ); } );

	set_server_values( wanted );
}

void bot_names_server_write( )
{
	std::lock_guard< std::mutex > lock( s_server.m_lock );
	if ( !s_server.m_resource_vtable || std::ranges::none_of( s_server.m_masks, []( uint8_t mask ) { return mask != 0; } ) )
		return;

	/* walk the live list every send: never holds a pointer across a map change */
	uint8_t* resource = nullptr;
	for ( void* entity = g_virtual.call< void* >( s_server.m_tools, 7 ); entity; entity = g_virtual.call< void*, void* >( s_server.m_tools, 8, entity ) )
		if ( *reinterpret_cast< void** >( entity ) == s_server.m_resource_vtable ) {
			resource = static_cast< uint8_t* >( entity );
			break;
		}

	for ( int index = 1; resource && index < k_max_slots; ++index )
		for ( int field = 0; field < field_count; ++field )
			if ( ( s_server.m_masks[ index ] & ( 1 << field ) ) && s_server.m_offsets[ field ] )
				*reinterpret_cast< int* >( resource + s_server.m_offsets[ field ] + index * sizeof( int ) ) = s_server.m_values[ index ][ field ];
}

void bot_names_randomize( )
{
	++s_generation;

	if ( GET_VARIABLE( g_variables.m_bot_names_mode, int ) == 3 && !g_bot_names_fetching ) {
		std::lock_guard< std::mutex > lock( s_pool_lock );
		s_pool.clear( );
		s_fetch_done = false;
	}
}

std::string bot_names_status( )
{
	std::lock_guard< std::mutex > lock( s_pool_lock );
	if ( g_bot_names_fetching )
		return std::format( "fetching steam names... {}/{}", s_pool.size( ), k_pool_want );

	if ( s_fetch_done && s_pool.empty( ) )
		return "steam unreachable, randomize to retry";

	const auto ready = std::ranges::count_if( s_pool, []( const profile_t& entry ) { return entry.m_ready; } );
	return ready < static_cast< std::ptrdiff_t >( s_pool.size( ) ) ? std::format( "{} steam names, {} avatars loaded", s_pool.size( ), ready )
	                                                               : std::format( "{} steam names", s_pool.size( ) );
}
