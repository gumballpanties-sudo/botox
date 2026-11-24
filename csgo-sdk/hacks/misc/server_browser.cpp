#include "server_browser.h"
#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"

#pragma warning( push )
#pragma warning( disable : 4996 ) /* matchmakingtypes.h inline strncpy / _snprintf */
#include "../../dependencies/steam/isteammatchmaking.h"
#pragma warning( pop )

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>

using n_server_browser::server_t;

namespace
{
	/* same csgo build under two ids: csgo_legacy branch of cs2's app, and the 2026 standalone. live servers ( cybershoke
	   included, 11-24 probe ) all register under the standalone, so a csgo_legacy client asking for its own id gets 0 */
	constexpr std::uint32_t k_csgo_apps[ ] = { 730, 4465480 };

	/* own handle from the game's user + pipe: the engine steam_api_context layout is not verified past m_steam_utils */
	ISteamMatchmakingServers* matchmaking_servers( )
	{
		static ISteamMatchmakingServers* servers = nullptr;
		if ( servers || !SteamClient )
			return servers;

		using handle_fn = int( __cdecl* )( );
		const auto user = reinterpret_cast< handle_fn >( g_modules[ STEAM_API_DLL ].find_export( HASH_BT( "SteamAPI_GetHSteamUser" ) ) );
		const auto pipe = reinterpret_cast< handle_fn >( g_modules[ STEAM_API_DLL ].find_export( HASH_BT( "SteamAPI_GetHSteamPipe" ) ) );
		if ( user && pipe )
			servers = SteamClient->GetISteamMatchmakingServers( user( ), pipe( ), STEAMMATCHMAKINGSERVERS_INTERFACE_VERSION );
		return servers;
	}

	/* same string servers register with ( their steam.inf ) */
	std::string patch_version( )
	{
		const std::string dir = g_utilities.csgo_dir( );
		if ( dir.empty( ) )
			return { };

		std::ifstream file( dir + "\\steam.inf" );
		for ( std::string line; std::getline( file, line ); ) {
			if ( line.rfind( "PatchVersion=", 0 ) != 0 )
				continue;
			line.erase( 0, 13 );
			line.erase( line.find_last_not_of( " \t\r" ) + 1 );
			return line;
		}
		return { };
	}

	/* = what connect asks: Host_IsSecureServerAllowed ( aaa_bypass hook ), bypass on = yes */
	bool secure_allowed( )
	{
		const auto original = g_hooks.m_aaa_bypass.get_original< decltype( &n_detoured_functions::aaa_bypass ) >( );
		if ( !original )
			return !std::strstr( GetCommandLineA( ), "-insecure" );

		return ( GET_VARIABLE( g_variables.m_aaa_bypass, bool ) && !GET_VARIABLE( g_variables.m_safe_mode, bool ) ) || original( );
	}

	bool is_cybershoke( std::string name )
	{
		std::transform( name.begin( ), name.end( ), name.begin( ), []( unsigned char c ) { return static_cast< char >( std::tolower( c ) ); } );
		return name.find( "cybershoke" ) != std::string::npos;
	}

	struct response_t final : ISteamMatchmakingServerListResponse {
		void ServerResponded( HServerListRequest request, int index ) override
		{
			ISteamMatchmakingServers* const servers = matchmaking_servers( );
			const gameserveritem_t* const item      = servers ? servers->GetServerDetails( request, index ) : nullptr;
			if ( !item )
				return;

			server_t server;
			server.m_address = item->m_NetAdr.GetConnectionAddressString( );

			/* cybershoke pads hostnames with tabs ( 11-24 probe ) */
			const std::string name = item->GetName( );
			const std::size_t first = name.find_first_not_of( " \t\r\n" );
			server.m_name = first == std::string::npos ? server.m_address : name.substr( first, name.find_last_not_of( " \t\r\n" ) - first + 1 );

			server.m_map         = item->m_szMap;
			server.m_app_id      = item->m_nAppID;
			server.m_players     = item->m_nPlayers;
			server.m_max_players = item->m_nMaxPlayers;
			server.m_bots        = item->m_nBotPlayers;
			server.m_ping        = item->m_nPing;
			server.m_password    = item->m_bPassword;
			server.m_secure      = item->m_bSecure;
			server.m_cybershoke  = is_cybershoke( server.m_name );
			g_server_browser.add( server );
		}

		void ServerFailedToRespond( HServerListRequest, int ) override { }

		void RefreshComplete( HServerListRequest, EMatchMakingServerResponse ) override { g_server_browser.finished( ); }
	};

	response_t s_response;
} // namespace

void n_server_browser::impl_t::on_frame( )
{
	/* paint_traverse's eject block only runs if the top panel paints before main.cpp gives up waiting: release here too */
	if ( g_ctx.m_world_restore_requested.load( std::memory_order_acquire ) ) {
		this->on_eject( );
		return;
	}

	if ( m_want_refresh.exchange( false ) )
		this->start( );
	else if ( !m_requests.empty( ) && m_finished.exchange( false ) )
		this->release( ); /* list is copied out, free steam's copy ( 32 bit VA ) */

	const bool allowed = secure_allowed( );
	std::string join;
	{
		std::scoped_lock lock( m_mutex );
		join.swap( m_join );
		if ( allowed != m_status.m_secure_allowed ) {
			m_status.m_secure_allowed = allowed;
			m_generation++;
		}
	}
	if ( !join.empty( ) )
		g_interfaces.m_engine_client->client_cmd_unrestricted( ( "connect " + join ).c_str( ) );
}

void n_server_browser::impl_t::on_eject( )
{
	m_want_refresh = false;
	this->release( );
}

void n_server_browser::impl_t::join( const std::string& address )
{
	std::scoped_lock lock( m_mutex );
	m_join = address;
}

bool n_server_browser::impl_t::snapshot( std::vector< server_t >& out, status_t& status, unsigned& generation )
{
	std::scoped_lock lock( m_mutex );
	if ( generation == m_generation )
		return false;

	out        = m_servers;
	status     = m_status;
	generation = m_generation;
	return true;
}

void n_server_browser::impl_t::add( const server_t& server )
{
	std::scoped_lock lock( m_mutex );
	m_servers.push_back( server );
	m_generation++;
}

void n_server_browser::impl_t::finished( )
{
	std::scoped_lock lock( m_mutex );
	if ( m_pending > 0 && --m_pending == 0 ) {
		m_status.m_refreshing = false;
		m_finished            = true;
	}
	m_generation++;
}

void n_server_browser::impl_t::start( )
{
	this->release( );
	m_finished = false;

	ISteamMatchmakingServers* const servers = matchmaking_servers( );

	status_t status;
	status.m_version        = patch_version( );
	status.m_app_id         = SteamUtils ? SteamUtils->GetAppID( ) : 0;
	status.m_secure_allowed = secure_allowed( );
	status.m_no_steam       = !servers || !status.m_app_id;
	status.m_refreshing     = !status.m_no_steam;

	std::vector< std::uint32_t > apps{ status.m_app_id };
	for ( const std::uint32_t app : k_csgo_apps )
		if ( app != status.m_app_id )
			apps.push_back( app );

	{
		std::scoped_lock lock( m_mutex );
		m_servers.clear( );
		m_status  = status;
		m_pending = status.m_no_steam ? 0 : static_cast< int >( apps.size( ) );
		m_generation++;
	}
	if ( status.m_no_steam )
		return;

	/* ponytail: one internet list per app, no paging; a steam cap on huge lists drops servers past it */
	std::vector< MatchMakingKeyValuePair_t > filters;
	if ( !status.m_version.empty( ) )
		filters.emplace_back( "version_match", status.m_version.c_str( ) );

	for ( const std::uint32_t app : apps ) {
		/* steam wants &pointer to one contiguous array ( tf_quickplay_ui.cpp:875 ) */
		MatchMakingKeyValuePair_t* list = filters.data( );
		if ( void* const request = servers->RequestInternetServerList( app, &list, static_cast< uint32 >( filters.size( ) ), &s_response ) )
			m_requests.push_back( request );
		else
			this->finished( ); /* never coming: count it done */
	}

	if ( m_requests.empty( ) ) {
		std::scoped_lock lock( m_mutex );
		m_status.m_no_steam = true;
		m_generation++;
	}
}

void n_server_browser::impl_t::release( )
{
	if ( m_requests.empty( ) )
		return;

	if ( ISteamMatchmakingServers* const servers = matchmaking_servers( ) )
		for ( void* const request : m_requests )
			servers->ReleaseRequest( request ); /* also cancels a running refresh, no more callbacks after */
	m_requests.clear( );

	std::scoped_lock lock( m_mutex );
	m_pending = 0;
	if ( m_status.m_refreshing ) {
		m_status.m_refreshing = false;
		m_generation++;
	}
}
