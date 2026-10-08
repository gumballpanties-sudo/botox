#include "misc.h"
#include "scaleform/image_cache.h"
#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"
#include "../../utilities/perf/perf_watch.h"
#include <algorithm>
#include <cstring>
#include <format>
#include <mutex>
#include <random>
#include <thread>
#include <unordered_map>
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
		std::string m_name = { };
		uint64_t m_xuid    = 0;
	};

	struct bot_t {
		std::vector< uint8_t > m_original = { };
		std::vector< uint8_t > m_written  = { };
		bool m_assigned                   = false;
		profile_t m_profile               = { };
		int m_ping                        = 0;
	};

	/* key = raw userid bytes, unique per connection: a slot reused by a human is never ours */
	std::unordered_map< int, bot_t > s_bots;
	uint32_t s_signature = 0;
	int s_generation     = 0;
	std::mt19937 s_rng{ std::random_device{ }( ) };
	std::vector< int > s_pinged;

	int roll_ping( )
	{
		return std::uniform_int_distribution< int >( 17, 155 )( s_rng );
	}

	/* bots get 0 from the server (IsBot is skipped) and a value that never changes is never re-sent, so the
	   client m_iPing copy keeps whatever we write. client only: a recorded demo still has 0. empty = put 0 back */
	void write_pings( const std::vector< std::pair< int, int > >& pings )
	{
		auto* resource = find_player_resource( );

		std::vector< int > pinged;
		for ( const auto& [ index, ping ] : pings ) {
			if ( resource )
				resource->get_resource_ping( index ) = ping;
			pinged.push_back( index );
		}

		for ( const int index : s_pinged )
			if ( resource && std::find( pinged.begin( ), pinged.end( ), index ) == pinged.end( ) )
				resource->get_resource_ping( index ) = 0;

		s_pinged = std::move( pinged );
	}

	/* steam pool: worker appends, main thread reads */
	std::mutex s_pool_lock;
	std::vector< profile_t > s_pool;
	size_t s_pool_next = 0;
	bool s_fetch_done  = false;

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
			if ( s_pool.empty( ) || ( s_pool_next >= s_pool.size( ) && !s_fetch_done ) )
				return false;

			out = s_pool[ s_pool_next++ % s_pool.size( ) ];
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
		if ( !s_pinged.empty( ) )
			write_pings( { } );

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

	if ( GET_VARIABLE( g_variables.m_bot_names_mode, int ) == 3 )
		start_fetch( );

	if ( const uint32_t signature = settings_signature( ); signature != s_signature ) {
		s_signature = signature;
		for ( auto& [ id, bot ] : s_bots ) {
			bot.m_assigned = false;
			bot.m_ping     = roll_ping( );
		}
	}

	const bool fake_ping = GET_VARIABLE( g_variables.m_bot_names_ping, bool );
	std::vector< std::pair< int, int > > pings;
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

			it = s_bots.emplace( user_id, bot_t{ .m_ping = roll_ping( ) } ).first;
		}

		auto& bot = it->second;
		seen.push_back( user_id );

		if ( fake_ping && i < 64 )
			pings.emplace_back( i + 1, bot.m_ping );

		if ( bot.m_written.empty( ) || std::memcmp( data, bot.m_written.data( ), k_info_size ) != 0 )
			bot.m_original.assign( data, data + length );

		if ( !bot.m_assigned && make_profile( bot.m_profile ) )
			bot.m_assigned = true;

		bot.m_written = patched( bot );
		if ( std::memcmp( data, bot.m_written.data( ), k_info_size ) != 0 )
			table->set_string_user_data( i, static_cast< int >( bot.m_written.size( ) ), bot.m_written.data( ) );
	}

	std::erase_if( s_bots, [ & ]( const auto& entry ) { return std::find( seen.begin( ), seen.end( ), entry.first ) == seen.end( ); } );

	if ( !pings.empty( ) || !s_pinged.empty( ) )
		write_pings( pings );
}

void bot_names_randomize( )
{
	++s_generation;

	if ( GET_VARIABLE( g_variables.m_bot_names_mode, int ) == 3 && !g_bot_names_fetching ) {
		std::lock_guard< std::mutex > lock( s_pool_lock );
		s_pool.clear( );
		s_pool_next  = 0;
		s_fetch_done = false;
	}
}

std::string bot_names_status( )
{
	std::lock_guard< std::mutex > lock( s_pool_lock );
	if ( g_bot_names_fetching )
		return std::format( "fetching steam names... {}/{}", s_pool.size( ), k_pool_want );

	return s_fetch_done && s_pool.empty( ) ? "steam unreachable, randomize to retry" : std::format( "{} steam names", s_pool.size( ) );
}
