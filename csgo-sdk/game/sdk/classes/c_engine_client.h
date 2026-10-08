#pragma once
#include "../../../dependencies/steam/isteamclient.h"
#include "../../../game/sdk/classes/c_angle.h"
#include "../../../globals/macros/macros.h"
#include "../../../utilities/memory/virtual.h"
#include "../../../utilities/modules/modules.h"

#include <vadefs.h>

struct player_info_t;
struct view_matrix_t;

class c_net_channel_info;

struct steam_api_context_t {
	ISteamClient* m_steam_client;
	ISteamUser* m_steam_user;
	ISteamFriends* m_steam_friends;
	ISteamUtils* m_steam_utils;
	ISteamMatchmaking* m_steam_matchmaking;
	void* m_steam_game_search;
	ISteamMatchmakingServers* m_steam_matchmaking_servers;
	ISteamUserStats* m_steam_user_stats;
	ISteamApps* m_steam_apps;
	ISteamNetworking* m_steam_networking;
	ISteamRemoteStorage* m_steam_remote_storage;
	ISteamScreenshots* m_steam_screenshots;
	ISteamHTTP* m_steam_http;
	ISteamController* m_controller;
	ISteamUGC* m_steam_ugc;
	ISteamAppList* m_steam_app_list;
	ISteamMusic* m_steam_music;
	ISteamMusicRemote* m_steam_music_remote;
	ISteamHTMLSurface* m_steam_html_surface;
	ISteamInventory* m_steam_inventory;
	ISteamVideo* m_steam_video;
	ISteamParentalSettings* m_steam_parental_settings;
	void* m_steam_input;
};

class c_engine_client
{
public:
	bool get_player_info( int index, player_info_t* info )
	{
		return g_virtual.call< bool >( this, 8, index, info );
	}

	int get_player_for_user_id( int uid )
	{
		return g_virtual.call< int >( this, 9, uid );
	}

	int get_local_player( )
	{
		return g_virtual.call< int >( this, 12 );
	}

	bool cull_box( const c_vector& mins, const c_vector& max )
	{
		return g_virtual.call< bool >( this, 34, std::cref( mins ), std::cref( max ) );
	}

	bool is_console_visible( )
	{
		return g_virtual.call< bool >( this, 11 );
	}

	int get_max_clients( )
	{
		return g_virtual.call< int >( this, 20 );
	}

	const char* key_binding_for_key( int button_code )
	{
		return g_virtual.call< const char* >( this, 22, button_code );
	}

	bool is_in_game( )
	{
		return g_virtual.call< bool >( this, 26 );
	}

	bool is_connected( )
	{
		return g_virtual.call< bool >( this, 27 );
	}

	bool is_connected_safe( )
	{
		return is_connected( ) && is_in_game( ) && !is_playing_demo( ) && !is_hltv( );
	}

	void* get_bsp_tree_query( )
	{
		return g_virtual.call< void* >( this, 43 );
	}

	const char* get_level_name( )
	{
		return g_virtual.call< const char* >( this, 52 );
	}

	const char* get_level_name_short( )
	{
		return g_virtual.call< const char* >( this, 53 );
	}

	c_net_channel_info* get_net_channel_info( )
	{
		return g_virtual.call< c_net_channel_info* >( this, 78 );
	}

	bool is_playing_demo( )
	{
		return g_virtual.call< bool >( this, 82 );
	}

	bool is_recording_demo( )
	{
		return g_virtual.call< bool >( this, 83 );
	}

	bool is_playing_time_demo( )
	{
		return g_virtual.call< bool >( this, 84 );
	}

	bool is_taking_screenshot( )
	{
		return g_virtual.call< bool >( this, 92 );
	}

	bool is_hltv( )
	{
		return g_virtual.call< bool >( this, 93 );
	}

	void set_view_angles( c_angle& view_angle )
	{
		g_virtual.call< void >( this, 19, std::ref( view_angle ) );
	}

	void get_view_angles( c_angle& view_angle )
	{
		g_virtual.call< void >( this, 18, std::ref( view_angle ) );
	}

	unsigned int get_engine_build_number( )
	{
		return g_virtual.call< unsigned int >( this, 104 );
	}

	const char* get_product_version_string( )
	{
		return g_virtual.call< const char* >( this, 105 );
	}

	const view_matrix_t& get_world_to_screen_matrix( )
	{
		return g_virtual.call< const view_matrix_t& >( this, 37 );
	}

	void execute_client_cmd( const char* cmd_string )
	{
		g_virtual.call< void >( this, 108, cmd_string );
	}

	void client_cmd_unrestricted( const char* cmd_string, bool from_console_or_keybind = false )
	{
		g_virtual.call< void >( this, 114, cmd_string, from_console_or_keybind );
	}

	steam_api_context_t* steam_api_context( )
	{
		return g_virtual.call< steam_api_context_t* >( this, 185 );
	}

	void fire_events( )
	{
		static auto fire_events_address = reinterpret_cast< std::uintptr_t >(
			g_modules[ ENGINE_DLL ].find_pattern( "e9 ? ? ? ? cc cc cc cc cc cc cc cc cc cc cc 8b 0d ? ? ? ? 8d 89" ) );

		using fire_events_type = void( __cdecl* )( );

		if ( fire_events_address )
			reinterpret_cast< fire_events_type >( fire_events_address )( );
	}
};
