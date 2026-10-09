#pragma once
#pragma once

#include <any>
#include <atomic>
#include <deque>
#include <functional>
#include <shared_mutex>
#include <string>
#include <vector>

#pragma comment( lib, "d3d9.lib" )
#pragma comment( lib, "d3dx9.lib" )
#include <d3d9.h>
#include <d3dx9.h>
#include <d3dx9tex.h>

#include "../../dependencies/steam/isteamclient.h"
#include "../../game/sdk/classes/c_vector.h"

struct ImFont;
struct ImDrawList;
struct ImVec2;
struct font_set_t;
struct indicator_font_set_t;
struct ImColor;

inline constexpr float k_indicator_scale_min = 0.1f, k_indicator_scale_max = 4.f;
class c_vector_2d;
class c_vector;

enum e_font_names {
	font_name_verdana_11 = 0,
	font_name_verdana_bd_11,
	font_name_icon_12,
	font_name_indicator_29,
	font_name_tahoma_12,
	font_name_tahoma_bd_12,
	font_name_icon_13,
	font_name_chillware_13,
	font_name_lobotomy_13,
	font_name_kamibebra_13,
	font_name_kamibebra_bold_13,
	font_name_lobotomy_bold_13,
	font_name_cumhacck_35,
	font_name_airplane_title,
	font_name_airplane_rows,
	font_name_clarity_icon_50,
	font_name_clarity_inter_semibold_14,
	font_name_clarity_inter_bold_15,
	font_name_clarity_inter_medium_15,
	font_name_sunflower_30,
	font_name_interwebz_calibri,
	font_name_havoc_tahoma_13,
	font_name_airflow_14,
	font_name_evolve_16,
	font_name_evolve_bold_16,
	font_name_legendware_verdana_12,
	font_name_legendware_lucida_10,
	font_name_interium_droid_24,
	font_name_skebob_nunito_14,
	font_name_skebob_arial_12,
	font_name_skebob_arial_13,
	font_name_cumidere_tahoma_16,
	font_name_illusory_tahoma_13,
	font_name_lumi_rubik_16,
	font_name_lumi_proggy_13,
	font_name_inba_tahoma_13,
	font_name_dna_montserrat_15,
	font_name_dna_pt_root_bold_15,
	font_name_dna_clarity_icon_50,
	font_name_cucumber_tahoma_14,
	font_name_cucumber_verdana_14,
	font_name_howeweware_minecraft_14,
	font_name_points_montserrat_italic_18,
	font_name_max
};

enum e_custom_font_names {
	custom_font_name_indicator = 0,
	custom_font_name_esp,
	custom_font_name_max
};

enum e_glyph_tier {
	glyph_tier_basic = 0,
	glyph_tier_full
};

enum e_draw_type {
	draw_type_none = 0,
	draw_type_text,
	draw_type_line,
	draw_type_rect,
	draw_type_gradient_rect,
	draw_type_triangle,
	draw_type_texture,
	draw_type_circle,
	draw_type_filled_circle,
	draw_type_poly,
	draw_type_corner_rect,
	draw_type_callback,
	draw_type_max
};

enum e_text_flags {
	text_flag_none       = 0,
	text_flag_dropshadow = 1,
	text_flag_outline    = 2
};

enum e_rect_flags {
	rect_flag_none          = 0,
	rect_flag_inner_outline = 1,
	rect_flag_outer_outline = 2
};

enum e_triangle_flags {
	triangle_flag_none    = 0,
	triangle_flag_outline = 1,
	triangle_flag_filled  = 2
};

struct draw_object_t {
	draw_object_t( const e_draw_type type, std::any&& obj ) : m_type( type ), m_obj( std::move( obj ) ) { }

	e_draw_type m_type = e_draw_type::draw_type_none;
	std::any m_obj     = { };
};

struct texture_draw_object_t {
	c_vector_2d m_position = { };
	c_vector_2d m_size     = { };
	unsigned int m_color   = { };
	void* m_texture_id     = { };
	float m_rounding       = { };
	int m_draw_flags       = { };
};

struct text_draw_object_t {
	ImFont* m_font               = { };
	c_vector_2d m_position       = { };
	std::string m_text           = { };
	unsigned int m_color         = { };
	unsigned int m_outline_color = { };
	e_text_flags m_draw_flags    = { };
	float m_font_size            = 0.f;
};

struct line_draw_object_t {
	c_vector_2d m_start  = { };
	c_vector_2d m_end    = { };
	unsigned int m_color = 0x0;
	float m_thickness    = 0.f;
};

struct rect_draw_object_t {
	c_vector_2d m_min            = { };
	c_vector_2d m_max            = { };
	unsigned int m_color         = { };
	unsigned int m_outline_color = { };
	bool m_filled                = false;
	float m_rounding             = 0.f;
	int m_corner_rounding_flags  = 0;
	float m_thickness            = 1.f;
	unsigned int m_outline_flags = e_rect_flags::rect_flag_none;
	float m_outline_thickness = 1.f;
};

struct gradient_rect_draw_object_t {
	gradient_rect_draw_object_t( ) = default;

	gradient_rect_draw_object_t( const c_vector_2d& min, const c_vector_2d& max, const unsigned int top_color, const unsigned int bottom_color,
	                             const float rounding = 0.f )
		: m_min( min ), m_max( max ), m_top_color( top_color ), m_bottom_color( bottom_color ), m_rounding( rounding )
	{
	}

	c_vector_2d m_min           = { };
	c_vector_2d m_max           = { };
	unsigned int m_top_color    = { };
	unsigned int m_bottom_color = { };
	float m_rounding            = 0.f;
};

struct circle_draw_object_t {
	circle_draw_object_t( ) = default;

	circle_draw_object_t( const c_vector_2d& center, const float radius, const unsigned int color, const int segments = 32,
	                      const float thickness = 1.f )
		: m_center( center ), m_radius( radius ), m_color( color ), m_segments( segments ), m_thickness( thickness )
	{
	}

	c_vector_2d m_center = { };
	float m_radius       = 0.f;
	unsigned int m_color = { };
	int m_segments       = 32;
	float m_thickness    = 1.f;
};

struct filled_circle_draw_object_t {
	filled_circle_draw_object_t( ) = default;

	filled_circle_draw_object_t( const c_vector_2d& center, const float radius, const unsigned int color, const int segments = 32 )
		: m_center( center ), m_radius( radius ), m_color( color ), m_segments( segments )
	{
	}

	c_vector_2d m_center = { };
	float m_radius       = 0.f;
	unsigned int m_color = { };
	int m_segments       = 32;
};

struct corner_rect_draw_object_t {
	c_vector_2d m_min    = { };
	c_vector_2d m_max    = { };
	float m_arm_x        = 0.f;
	float m_arm_y        = 0.f;
	float m_rounding     = 0.f;
	unsigned int m_color = { };
	float m_thickness    = 1.f;
};

struct poly_draw_object_t {
	c_vector_2d m_points[ 32 ]   = { };
	int m_count                  = 0;
	unsigned int m_color         = { };
	unsigned int m_outline_color = { };
	float m_thickness            = 1.f;
	bool m_outline               = false;
};

/* runs on the RENDER thread, in queue order, with the background list (vertex post-passes, own d3d
   textures). captures must own their data: the game thread has moved on. */
struct callback_draw_object_t {
	std::function< void( ImDrawList* ) > m_callback = { };
};

struct triangle_draw_object_t {
	c_vector_2d m_first          = { };
	c_vector_2d m_second         = { };
	c_vector_2d m_third          = { };
	unsigned int m_color         = { };
	unsigned int m_draw_flags    = e_triangle_flags::triangle_flag_none;
	unsigned int m_outline_color = { };
	float m_thickness            = 0.f;
};

namespace n_render
{
	struct impl_t {
		void clear_draw_data( )
		{
			if ( this->m_draw_data.empty( ) )
				return;

			this->m_draw_data.clear( );
		}

		void on_end_scene( const std::function< void( ) >& function, IDirect3DDevice9* device );

		void swap_draw_data( )
		{
			std::unique_lock< std::shared_mutex > lock( this->m_mutex );

			this->m_draw_data.swap( this->m_thread_safe_draw_data );
		}

		float to_overlay_x( float screen_x );

		struct stretch_block_t {
			ImDrawList* m_list;
			int m_vertex_begin, m_vertex_end, m_command_begin, m_command_end;
			bool m_panel;
		};

		stretch_block_t begin_stretch_block( ImDrawList* list, bool panel = true );
		void end_stretch_block( stretch_block_t block );

		void queue_stretch_block_begin( );
		void queue_stretch_block_end( );

		std::vector< stretch_block_t > m_stretch_blocks;

		float m_dpi_scale = 1.f;

		float m_dpi_panel_scale = 1.f;

		ImVec2 screen_mouse( );
		float m_screen_mouse_x = -FLT_MAX, m_screen_mouse_y = -FLT_MAX;

		ImVec2 dpi_panel_pos( ImVec2 pos, ImVec2 size );
		ImVec2 dpi_panel_layout_pos( ImVec2 shown, ImVec2 size );

		void on_release( );

		void draw_cached_data( );

		bool world_to_screen( const c_vector& origin, c_vector_2d& screen );

		void cache_view_matrix( );

		std::vector< draw_object_t > m_draw_data             = { };
		std::vector< draw_object_t > m_thread_safe_draw_data = { };
		std::shared_mutex m_mutex                            = { };

		std::shared_mutex m_font_mutex = { };

		ImFont *m_fonts[ e_font_names::font_name_max ]{ }, *m_custom_fonts[ e_custom_font_names::custom_font_name_max ]{ };

		std::atomic< font_set_t* > m_pending_fonts{ nullptr };
		std::atomic< bool > m_font_build_running{ false };
		font_set_t* m_uploading_fonts = nullptr; // render thread: built set whose texture is going up in slices

		// render thread, before NewFrame: swap a finished set in and free the old atlas
		void install_fonts( font_set_t* set );

		std::atomic< indicator_font_set_t* > m_pending_indicator_fonts{ nullptr };
		indicator_font_set_t* m_indicator_fonts = nullptr;

		// render thread, before NewFrame: install a baked set, (re)upload its texture, start a bake when settled
		void update_indicator_fonts( IDirect3DDevice9* device );

		void release_indicator_texture( );

		/* the indicator font for this scale (baked one if ready, else the base) + its draw size. game thread
		   under the font read lock, or render thread */
		ImFont* indicator_font( float scale, float& font_size );

		bool m_initialised = false, m_reload_fonts = true, m_reset_indicator_font = false;

		/* returns font if it still belongs to the live atlas, else the verdana fallback - never
		   dereference a stored ImFont* without it, a rebuild frees the whole set */
		ImFont* live_font( ImFont* font );

		void text( ImDrawList* draw_list, ImFont* font, const c_vector_2d& position, const std::string& text, const unsigned int& color,
		           const unsigned int& outline_color, e_text_flags draw_flags = e_text_flags::text_flag_dropshadow, float font_size = 0.f );

		void rect( ImDrawList* draw_list, const c_vector_2d& min, const c_vector_2d& max, const unsigned int& color,
		           const unsigned int& outline_color, bool filled, float rounding, int corner_rounding_flags, float thickness,
		           unsigned int outline_flags, float outline_thickness = 1.f );

		void corner_rect( float x1, float y1, float x2, float y2, const unsigned int& color, float thickness = 1.f, float rounding = 0.f );

		void copy_and_convert( const uint8_t* rgba_data, uint8_t* out, const size_t size );
		IDirect3DTexture9* steam_image( CSteamID steam_id );

		IDirect3DTexture9* m_counter_terrorist_avatar = nullptr;
		IDirect3DTexture9* m_terrorist_avatar         = nullptr;
	};
}

inline n_render::impl_t g_render{ };
