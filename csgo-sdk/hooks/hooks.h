#pragma once
#include "detour_hook/c_detour_hook.h"

#include <cstdint>
#include <d3d9.h>
#include <stdint.h>

class c_vector;
class c_angle;
class c_view_setup;
class c_material;
class c_animation_state;
class c_base_entity;
class c_move_data;
class c_net_channel;
class c_net_message;
class bf_write;

struct game_event_t;
struct model_render_info_t;
struct matrix3x4_t;
struct view_matrix_t;

enum e_glow_style;

namespace n_hooks
{
	struct impl_t {
		bool on_attach( );
		void on_release( );

		/* d3d objects die on the end_scene thread that made them. called from end_scene's unload bail,
		   then again from eject in case no frame came. idempotent */
		void release_render_resources( );

		c_detour_hook m_alloc_key_values_memory{ };
		c_detour_hook m_create_move_proxy{ };
		c_detour_hook m_run_command{ };
		c_detour_hook m_emit_sound{ };
		c_detour_hook m_frame_stage_notify{ };
		c_detour_hook m_dispatch_user_message{ };
		c_detour_hook m_paint_traverse{ };
		c_detour_hook m_on_add_entity{ };
		c_detour_hook m_on_remove_entity{ };
		c_detour_hook m_level_init_pre_entity{ };
		c_detour_hook m_level_shutdown{ };
		c_detour_hook m_get_vcollide{ };
		c_detour_hook m_find_mdl{ };
		c_detour_hook m_particle_collection_simulate{ };
		c_detour_hook m_modify_eye_position{ };
		c_detour_hook m_override_mouse_input{ };
		c_detour_hook m_override_view{ };
		c_detour_hook m_do_post_screen_space_effects{ };
		c_detour_hook m_glow_effect_spectator{ };
		c_detour_hook m_process_movement{ };
		c_detour_hook m_process_movement_sv{ };
		c_detour_hook m_fire_event_intern{ };
		c_detour_hook m_net_earliertempents{ };
		c_detour_hook m_draw_set_color{ };
		c_detour_hook m_draw_filled_rect{ };
		c_detour_hook m_draw_textured_rect{ };
		c_detour_hook m_draw_line{ };
		c_detour_hook m_draw_textured_polygon{ };
		c_detour_hook m_level_init_post_entity{ };
		c_detour_hook m_set_image_data_r8g8b8a8{ };
		c_detour_hook m_load_file_into_buffer{ };
		c_detour_hook m_draw_model_execute{ };
		c_detour_hook m_list_leaves_in_box{ };
		c_detour_hook m_send_datagram{ };
		c_detour_hook m_draw_view_models{ };
		c_detour_hook m_is_hltv{ };
		c_detour_hook m_is_paused{ };
		c_detour_hook m_is_playing_demo{ };
		c_detour_hook m_send_net_msg{ };
		c_detour_hook m_draw_static_prop_array_fast{ };
		c_detour_hook m_push_notice{ };
		c_detour_hook m_set_visuals_data{ };
		c_detour_hook m_view_draw_fade{ };
		c_detour_hook m_push_2d_view{ };
		c_detour_hook m_draw_world_lists{ };
		c_detour_hook m_world_to_screen_matrix{ };
		c_detour_hook m_get_local_view_angles{ };

		c_detour_hook m_tier0_warning{ };

		c_detour_hook m_loose_files_allowed{ };
		c_detour_hook m_check_for_pure_server_whitelist{ };

		c_detour_hook m_lock_cursor{ };
		c_detour_hook m_reset{ };
		c_detour_hook m_create_texture{ };
		c_detour_hook m_set_depth_stencil_surface{ };
		c_detour_hook m_end_scene{ };
		c_detour_hook m_present{ };
		c_detour_hook m_draw_indexed_primitive{ };
	};
}

inline n_hooks::impl_t g_hooks{ };

namespace n_detoured_functions
{
	void* __fastcall alloc_key_values_memory( void* ecx, void* edx, int size );
	void __fastcall create_move_proxy( void* ecx, void* edx, int sequence_number, float input_sample_frametime, bool is_active );
	void __fastcall run_command( void* ecx, void* edx, void* entity, void* cmd, void* move_helper );
	void __fastcall get_local_view_angles( void* ecx, void* edx, c_angle& angles );
	void __stdcall emit_sound( void* filter, int idx, int channel, const char* sound_entry, unsigned int sound_entry_hash, const char* sample,
	                           float volume, int seed, float attenuation, int flags, int pitch, const c_vector* origin, const c_vector* direction,
	                           void* vec_origins, bool update_pos, float soundtime, int speakerentity, int unk );
	void __fastcall frame_stage_notify( void* ecx, void* edx, int stage );
	bool __fastcall dispatch_user_message( void* ecx, void* edx, int msg_type, int flags, int size, const void* msg );
	void __fastcall paint_traverse( void* ecx, void* edx, unsigned int panel, bool force_repaint, bool force );
	void __fastcall on_add_entity( void* ecx, void* edx, void* handle_entity, unsigned int entity_handle );
	void __fastcall on_remove_entity( void* ecx, void* edx, void* handle_entity, unsigned int entity_handle );
	void __stdcall level_init_pre_entity( const char* map_name );
	void __fastcall level_init_post_entity( void* ecx, void* edx );
	void __fastcall level_shutdown( void* thisptr );
	void* __fastcall get_vcollide( void* ecx, void* edx, int model_index );
	unsigned short __fastcall find_mdl( void* ecx, void* edx, const char* path );
	void __fastcall particle_collection_simulate( void* ecx, void* edx );
	void __fastcall modify_eye_position( c_animation_state* anim_state, void* edx, c_vector& input_eye_pos );
	void __fastcall override_mouse_input( void* thisptr, int edx, float* x, float* y );
	void __fastcall override_view( void* ecx, void* edx, c_view_setup* setup );
	void __fastcall do_post_screen_space_effects( void* ecx, void* edx, c_view_setup* setup );
	void __fastcall lock_cursor( void* ecx, void* edx );
	long __stdcall reset( IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* presentation_parameters );
	long __stdcall create_texture( IDirect3DDevice9* device, unsigned int width, unsigned int height, unsigned int levels, unsigned long usage,
	                               D3DFORMAT format, D3DPOOL pool, IDirect3DTexture9** texture, void** shared_handle );
	long __stdcall set_depth_stencil_surface( IDirect3DDevice9* device, IDirect3DSurface9* surface );
	long __stdcall end_scene( IDirect3DDevice9* device );
	long __stdcall present( IDirect3DDevice9* device, const RECT* source, const RECT* dest, HWND window_override, const RGNDATA* dirty_region );
	long __stdcall draw_indexed_primitive( IDirect3DDevice9* device, D3DPRIMITIVETYPE type, int base_vertex, unsigned int min_vertex, unsigned int vertices,
	                                       unsigned int start_index, unsigned int primitives );

	void draw_overlay_frame( IDirect3DDevice9* device );

	/* our per-frame end_scene work wrapped round end_scene_fn (closes the open scene). run by the steam
	   overlay's EndScene, or by present when that never comes (HLAE wraps the device, overlay off) */
	long overlay_end_scene( IDirect3DDevice9* device, long( __stdcall* end_scene_fn )( IDirect3DDevice9* ) );

	inline bool s_overlay_frame_seen = false;
	long __stdcall wndproc( HWND window, unsigned int message, unsigned int wide_parameter, long long_parameter );
	bool __cdecl glow_effect_spectator( c_base_entity* player, c_base_entity* local, e_glow_style& style, c_vector& glow_color, float& alpha_start,
	                                    float& alpha, float& time_start, float& time_target, bool& animate );
	void __fastcall process_movement( void* thisptr, void* edx, c_base_entity* player, c_move_data* move_data );
	void __fastcall process_movement_sv( void* thisptr, void* edx, void* player, c_move_data* move_data );
	bool __fastcall fire_event_intern( void* ecx, void* edx, game_event_t* game_event );
	void __stdcall draw_set_color( int r, int g, int b, int a );
	void __stdcall draw_filled_rect( int x0, int y0, int x1, int y1 );
	void __stdcall draw_textured_rect( int x0, int y0, int x1, int y1 );
	void __stdcall draw_line( int x0, int y0, int x1, int y1 );
	void __fastcall draw_textured_polygon( void* ecx, void* edx, int count, float* vertices, bool clip_vertices );
	bool __fastcall net_earliertempents( void* ecx, void* edx );
	bool __fastcall set_image_data_r8g8b8a8( void* ecx, void* edx, const uint8_t* data, uint32_t len, const char* filename, int w, int h, void* arg1,
	                                         int arg2 );
	bool __fastcall load_file_into_buffer( void* ecx, void* edx, const char* file, void* buffer, bool text, void* change_callback,
	                                       unsigned int padding );
	void __fastcall draw_model_execute( void* ecx, void* edx, void* context, void* state, model_render_info_t& info,
	                                    matrix3x4_t* custom_bone_to_world );
	int __fastcall list_leaves_in_box( void* ecx, void* edx, const c_vector& mins, const c_vector& maxs, unsigned short* list, int list_max );
	int __fastcall send_datagram( c_net_channel* net_channel, int edx, bf_write* datagram );
	void __fastcall draw_view_models( void* ecx, void* edx, c_view_setup& setup, bool draw_view_model, bool draw_scope_lens_mask );
	bool __fastcall is_hltv( void* ecx, void* edx );
	bool __fastcall is_paused( void* ecx, void* edx );
	bool __fastcall is_playing_demo( void* ecx, void* edx );
	bool __fastcall send_net_msg( void* ecx, void* edx, c_net_message* message, bool force_reliable, bool voice );
	void __fastcall push_notice( void* ecx, void* edx, const char* text, int str_len, const char* null );
	void __fastcall set_visuals_data( void* ecx, void* edx, const char* shader_name );

	/* 128 = 0-arg thiscall bool getter; whitelist check = free function, no args */
	bool __fastcall loose_files_allowed( void* ecx, void* edx );
	void __cdecl check_for_pure_server_whitelist( );

	void __fastcall view_draw_fade( void* ecx, void* edx, unsigned char* color, c_material* material, bool map_full_texture_to_screen );

	void __fastcall push_2d_view( void* ecx, void* edx, void* render_context, const c_view_setup& view, int flags, void* render_target,
	                              void* frustum );
	void __fastcall draw_world_lists( void* ecx, void* edx, void* render_context, void* list, unsigned long flags, float water_z_adjust );
	const view_matrix_t& __fastcall world_to_screen_matrix( void* ecx, void* edx );

	void __cdecl tier0_warning( const char* format, ... );

}
