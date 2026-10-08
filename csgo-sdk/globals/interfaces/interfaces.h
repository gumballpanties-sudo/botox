#pragma once
#include <cstdint>

#include "../../dependencies/steam/isteamclient.h"
#include "../../dependencies/steam/isteamfriends.h"
#include "../../dependencies/steam/isteamgamecoordinator.h"
#include "../../dependencies/steam/isteamuser.h"
#include "../../dependencies/steam/isteamutils.h"

class c_base_client_dll;
class c_engine_client;
class c_client_entity_list;
class c_input_system;
class c_material_system;
class c_engine_trace;
class c_debug_overlay;
class c_convar;
class c_input;
class c_model_info;
class c_game_movement;
class c_move_helper;
class c_prediction;
class c_model_render;
class c_model_cache;
class c_network_string_table_container;
class c_surface;
class c_game_types;
class c_localize;
class c_panel;
class c_physics_collison;
class c_studio_render;
class c_render_view;
class c_panorama_ui_engine;
class c_key_values_system;
class c_global_vars_base;
class c_weapon_system;
class c_client_state;
class c_engine_sound;
class c_physics_surface_props;
class c_hudchat;
class c_view_render_beams;

struct IDirect3DDevice9;
struct game_event_manager_t;

namespace n_interfaces
{
	inline c_hudchat* chat_element = nullptr;

	struct impl_t {
		bool on_attach( );

		c_base_client_dll* m_base_client                 = nullptr;
		c_engine_client* m_engine_client                 = nullptr;
		c_client_entity_list* m_client_entity_list       = nullptr;
		c_input_system* m_input_system                   = nullptr;
		c_material_system* m_material_system             = nullptr;
		c_engine_trace* m_engine_trace                   = nullptr;
		c_debug_overlay* m_debug_overlay                 = nullptr;
		c_convar* m_convar                               = nullptr;
		c_model_info* m_model_info                       = nullptr;
		c_game_movement* m_game_movement                 = nullptr;
		c_prediction* m_prediction                       = nullptr;
		c_model_render* m_model_render                   = nullptr;
		c_network_string_table_container* m_string_tables = nullptr;
		c_network_string_table_container* m_server_string_tables = nullptr;
		c_model_cache* m_model_cache                     = nullptr;
		c_engine_sound* m_engine_sound                   = nullptr;
		c_physics_surface_props* m_physics_surface_props = nullptr;
		c_surface* m_surface                             = nullptr;
		c_game_types* m_game_types                       = nullptr;
		c_localize* m_localize                           = nullptr;
		c_panel* m_panel                                 = nullptr;
		c_physics_collison* m_physics_collison           = nullptr;
		c_studio_render* m_studio_render                 = nullptr;
		c_render_view* m_render_view                     = nullptr;
		game_event_manager_t* m_game_event_manager       = nullptr;
		c_key_values_system* m_key_values_system         = nullptr;
		c_global_vars_base* m_global_vars_base           = nullptr;
		void* m_client_mode                              = nullptr;
		c_weapon_system* m_weapon_system                 = nullptr;
		c_client_state* m_client_state                   = nullptr;
		c_input* m_input                                 = nullptr;
		c_move_helper* m_move_helper                     = nullptr;
		IDirect3DDevice9* m_direct_device                = nullptr;
		c_panorama_ui_engine* m_panorama                 = nullptr;

		c_view_render_beams* m_view_render_beams         = nullptr;

		/* IFileSystem, only a vtable for the sv_pure bypass hook. null = no filesystem_stdio, hook skipped */
		void* m_file_system                              = nullptr;
	};
}

inline n_interfaces::impl_t g_interfaces{ };
