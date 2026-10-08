#include "interfaces.h"

#include "../../globals/includes/includes.h"
#include "../../hacks/misc/scaleform/scaleform.h"
#include "../../utilities/memory/virtual.h"

bool n_interfaces::impl_t::on_attach( )
{
	if ( !( m_base_client = static_cast< c_base_client_dll* >( g_modules[ CLIENT_DLL ].find_interface( "VClient018" ) ) ) )
		return false;

	if ( !( m_engine_client = static_cast< c_engine_client* >( g_modules[ ENGINE_DLL ].find_interface( "VEngineClient014" ) ) ) )
		return false;

	if ( !( m_client_entity_list = static_cast< c_client_entity_list* >( g_modules[ CLIENT_DLL ].find_interface( "VClientEntityList003" ) ) ) )
		return false;

	if ( !( m_input_system = static_cast< c_input_system* >( g_modules[ INPUTSYSTEM_DLL ].find_interface( "InputSystemVersion001" ) ) ) )
		return false;

	if ( !( m_material_system = static_cast< c_material_system* >( g_modules[ MATERIALSYSTEM_DLL ].find_interface( "VMaterialSystem080" ) ) ) )
		return false;

	if ( !( m_engine_trace = static_cast< c_engine_trace* >( g_modules[ ENGINE_DLL ].find_interface( "EngineTraceClient004" ) ) ) )
		return false;

	if ( !( m_debug_overlay = static_cast< c_debug_overlay* >( g_modules[ ENGINE_DLL ].find_interface( "VDebugOverlay004" ) ) ) )
		return false;

	if ( !( m_convar = static_cast< c_convar* >( g_modules[ VSTDLIB_DLL ].find_interface( "VEngineCvar007" ) ) ) )
		return false;

	if ( !( m_model_info = static_cast< c_model_info* >( g_modules[ ENGINE_DLL ].find_interface( "VModelInfoClient004" ) ) ) )
		return false;

	if ( !( m_game_movement = static_cast< c_game_movement* >( g_modules[ CLIENT_DLL ].find_interface( "GameMovement001" ) ) ) )
		return false;

	if ( !( m_prediction = static_cast< c_prediction* >( g_modules[ CLIENT_DLL ].find_interface( "VClientPrediction001" ) ) ) )
		return false;

	if ( !( m_model_render = static_cast< c_model_render* >( g_modules[ ENGINE_DLL ].find_interface( "VEngineModel016" ) ) ) )
		return false;

	if ( !( m_model_cache = static_cast< c_model_cache* >( g_modules[ DATACACHE_DLL ].find_interface( "MDLCache004" ) ) ) )
		return false;

	if ( !( m_string_tables = static_cast< c_network_string_table_container* >(
	            g_modules[ ENGINE_DLL ].find_interface( "VEngineClientStringTable001" ) ) ) )
		return false;

	m_server_string_tables = static_cast< c_network_string_table_container* >(
		g_modules[ ENGINE_DLL ].find_interface( "VEngineServerStringTable001" ) );

	if ( !( m_engine_sound = static_cast< c_engine_sound* >( g_modules[ ENGINE_DLL ].find_interface( "IEngineSoundClient003" ) ) ) )
		return false;

	if ( !( m_physics_surface_props = static_cast< c_physics_surface_props* >( g_modules[ PHYSICS_DLL ].find_interface( "VPhysicsSurfaceProps001" ) ) ) )
		return false;

	if ( !( m_surface = static_cast< c_surface* >( g_modules[ VGUI_DLL ].find_interface( "VGUI_Surface031" ) ) ) )
		return false;

	if ( !( m_game_types = static_cast< c_game_types* >( g_modules[ MATCHMAKING_DLL ].find_interface( "VENGINE_GAMETYPES_VERSION002" ) ) ) )
		return false;

	if ( !( m_localize = static_cast< c_localize* >( g_modules[ LOCALIZE_DLL ].find_interface( "Localize_001" ) ) ) )
		return false;

	if ( !( m_panel = static_cast< c_panel* >( g_modules[ VGUI2_DLL ].find_interface( "VGUI_Panel009" ) ) ) )
		return false;

	if ( !( m_physics_collison = static_cast< c_physics_collison* >( g_modules[ PHYSICS_DLL ].find_interface( "VPhysicsCollision007" ) ) ) )
		return false;

	if ( !( m_studio_render = static_cast< c_studio_render* >( g_modules[ STUDIORENDER_DLL ].find_interface( "VStudioRender026" ) ) ) )
		return false;

	if ( !( m_render_view = static_cast< c_render_view* >( g_modules[ ENGINE_DLL ].find_interface( "VEngineRenderView014" ) ) ) )
		return false;

	if ( !( m_game_event_manager = static_cast< game_event_manager_t* >( g_modules[ ENGINE_DLL ].find_interface( "GAMEEVENTSMANAGER002" ) ) ) )
		return false;

	if ( !( m_panorama = static_cast< c_panorama_ui_engine* >( g_modules[ PANORAMA_DLL ].find_interface( "PanoramaUIEngine001" ) ) ) )
		return false;

	if ( !( m_file_system = g_modules[ FILESYSTEM_DLL ].find_interface( "VFileSystem017" ) ) )
		g_console.print< n_console::log_level::WARNING >( "failed to find IFileSystem (sv_pure bypass off)" );

	if ( !( m_key_values_system =
	            reinterpret_cast< c_key_values_system*( __cdecl* )( ) >( g_modules[ VSTDLIB_DLL ].find_export( HASH_BT( "KeyValuesSystem" ) ) )( ) ) )
		return false;
	else
		g_console.print(
			std::format( "found IKeyValuesSystem @ {:p}", reinterpret_cast< void* >( m_key_values_system )  ).c_str( ) );

	/* operand of an instruction inside HudUpdate / HudProcessInput. a build with other codegen shifts it, and the 4 bytes read
	   there are then no client.dll address: fail the stage instead of dereferencing them */
	const auto client_global = [ this ]( const unsigned int index, const unsigned int operand ) -> void** {
		void** const address = *reinterpret_cast< void*** >( g_virtual.get< unsigned int >( m_base_client, index ) + operand );
		return g_modules[ CLIENT_DLL ].contains( address ) ? address : nullptr;
	};

	const auto global_vars_slot = client_global( 11, 0xa );
	if ( !global_vars_slot || !( m_global_vars_base = *reinterpret_cast< c_global_vars_base** >( global_vars_slot ) ) )
		return false;
	else
		g_console.print(
			std::format( "found IGlobalVarsBase @ {:p}", reinterpret_cast< void* >( m_global_vars_base ) ).c_str( ) );

	const auto client_mode_slot = client_global( 10, 0x5 );
	if ( !client_mode_slot || !( m_client_mode = *client_mode_slot ) )
		return false;
	else
		g_console.print(
			std::format( "found IClientModeShared @ {:p}", reinterpret_cast< void* >( m_client_mode ) ).c_str( ) );

	/* a pattern miss returns null: bail and log the sig, never add the operand offset to null */
	const auto scan = []( unsigned int module_hash, const char* signature ) -> unsigned char* {
		return g_modules[ module_hash ].find_pattern( signature );
	};

	const auto weapon_system_match = scan( CLIENT_DLL, "8B 35 ? ? ? ? FF 10 0F B7 C0" );
	if ( !weapon_system_match )
		return false;

	if ( !( m_weapon_system = *reinterpret_cast< c_weapon_system** >( weapon_system_match + 0x2 ) ) )
		return false;
	else
		g_console.print(
			std::format( "found IWeaponSystem @ {:p}", reinterpret_cast< void* >( m_weapon_system ) ).c_str( ) );

	const auto client_state_match = scan( ENGINE_DLL, "A1 ? ? ? ? 8B 88 ? ? ? ? 85 C9 75 07" );
	if ( !client_state_match )
		return false;

	if ( !( m_client_state = **reinterpret_cast< c_client_state*** >( client_state_match + 0x1 ) ) )
		return false;
	else
		g_console.print( std::format( "found IClientState @ {:p}",  reinterpret_cast< void* >( m_client_state ) ).c_str( ) );

	const auto input_match = scan( CLIENT_DLL, "B9 ? ? ? ? F3 0F 11 04 24 FF 50 10" );
	if ( !input_match )
		return false;

	if ( !( m_input = *reinterpret_cast< c_input** >( input_match + 0x1 ) ) )
		return false;
	else
		g_console.print( std::format( "found IInput @ {:p}", reinterpret_cast< void* >( m_input ) ).c_str( ) );

	const auto direct_device_match = scan( SHADERAPIDX9_DLL, "A1 ? ? ? ? 50 8B 08 FF 51 0C" );
	if ( !direct_device_match )
		return false;

	if ( !( m_direct_device = **reinterpret_cast< IDirect3DDevice9*** >( direct_device_match + 0x1 ) ) )
		return false;
	else
		g_console.print(
			std::format( "found IDirect3DDevice9 @ {:p}",  reinterpret_cast< void* >( m_direct_device ) ).c_str( ) );

	if ( GetModuleHandleA( "AfxHookSource.dll" ) )
		g_console.print( "HLAE loaded (AfxHookSource.dll): device above is its wrapper" );

	if ( !( g_scaleform.compare_extension = reinterpret_cast< decltype( g_scaleform.compare_extension ) >(
				g_modules[ PANORAMA_DLL ].find_pattern( "55 8B EC 53 57 8B 7D 08 85" ) ) ) )
		g_console.print( "FAILED TO FIND COMPARE_EXTENSION" );

	SteamClient = this->m_engine_client->steam_api_context( )->m_steam_client;
	if ( !SteamClient )
		return false;

	SteamUser = this->m_engine_client->steam_api_context( )->m_steam_user;
	if ( !SteamUser )
		return false;

	SteamFriends = this->m_engine_client->steam_api_context( )->m_steam_friends;
	if ( !SteamFriends )
		return false;

	SteamUtils = this->m_engine_client->steam_api_context( )->m_steam_utils;
	if ( !SteamUtils )
		return false;

	const int steam_user =
		reinterpret_cast< std::add_pointer_t< int( ) > >( g_modules[ STEAM_API_DLL ].find_export( HASH_BT( "SteamAPI_GetHSteamUser" ) ) )( );
	const int steam_pipe =
		reinterpret_cast< std::add_pointer_t< int( ) > >( g_modules[ STEAM_API_DLL ].find_export( HASH_BT( "SteamAPI_GetHSteamPipe" ) ) )( );

	SteamGameCoordinator =
		static_cast< ISteamGameCoordinator* >( SteamClient->GetISteamGenericInterface( steam_user, steam_pipe, "SteamGameCoordinator001" ) );
	if ( !SteamGameCoordinator )
		return false;

	if ( const auto beams_match = scan( CLIENT_DLL, "B9 ? ? ? ? A1 ? ? ? ? FF 10 A1 ? ? ? ? B9" ) ) {
		m_view_render_beams = *reinterpret_cast< c_view_render_beams** >( beams_match + 0x1 );
		g_console.print( std::format( "found IViewRenderBeams @ {:p}", reinterpret_cast< void* >( m_view_render_beams ) ).c_str( ) );
	} else
		g_console.print( "FAILED TO FIND IVIEWRENDERBEAMS" );

	if ( m_client_mode ) {
		if ( const auto offset = g_modules[ CLIENT_DLL ].find_pattern( "E8 ? ? ? ? 8B 4F ? 85 C9 74 06 51" ) )
			chat_element = *reinterpret_cast< c_hudchat** >( reinterpret_cast< uintptr_t >( m_client_mode ) +
			                                                 *reinterpret_cast< uint8_t* >( offset + 7 ) );
	}

	return true;
}
