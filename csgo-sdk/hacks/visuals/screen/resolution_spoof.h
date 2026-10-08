#pragma once
#include "stream_guard.h"

#include <d3d9.h>
#include <functional>
#include <vector>

class c_view_setup;

namespace n_resolution_spoof
{
	struct impl_t {
		void on_present( IDirect3DDevice9* device, const std::function< void( ) >& draw = { } );

		void on_post_screen_space_effects( c_view_setup* setup );

		bool render_scale_active( );

		bool wants_overlay_pass( );

		/* ...only while that pass really runs. if Present never reaches us the draw goes nowhere,
		   so end_scene keeps it once the pass missed a couple of frames. */
		bool pass_is_running( );

		void on_end_scene( );

		bool enabled( );

		/* aspect ratio changer, screen space half: how much wider a screen space draw must get to
		   match the stretched world. 1 = nothing. crosshair paths only; world draws get it via the engine matrix. */
		float overlay_aspect_scale( );

		float aspect_scale( );

		bool low_res_grid( float& pixels_x, float& pixels_y );

		void on_imgui_new_frame( );

		// game thread: net_graphheight is a convar write, not on the d3d thread
		void on_frame_stage_notify( );

		// d3d thread, before Reset: a default pool surface must not survive it
		void on_device_lost( );

		void on_release( );

		int m_screen_width  = 0;
		int m_screen_height = 0;

		int m_view_width  = 0;
		int m_view_height = 0;

		void target_size( int screen_width, int screen_height, int& width, int& height );

	private:
		void release( );

		void release_scale( );
		bool build_scale_target( IDirect3DDevice9* device, int width, int height, D3DFORMAT format );

		void viewport_scale( bool enabled );

		bool begin_quad_state( IDirect3DDevice9* device, IDirect3DStateBlock9** state_block );
		bool draw_quad( IDirect3DDevice9* device, IDirect3DTexture9* texture, int quad_width, int quad_height );

		void end_quad_state( IDirect3DDevice9* device, IDirect3DStateBlock9* state_block );

		n_stream_guard::state_t m_quad_streams{ };

		void net_graph_scale( bool enabled );

		struct chain_level_t {
			IDirect3DTexture9* m_texture = nullptr;
			IDirect3DSurface9* m_surface = nullptr;
			int m_width                  = 0;
			int m_height                 = 0;
		};

		bool build_chain( IDirect3DDevice9* device, int screen_width, int screen_height, int width, int height, D3DFORMAT format );

		std::vector< chain_level_t > m_chain{ };

		int m_chain_screen_width  = 0;
		int m_chain_screen_height = 0;

		IDirect3DTexture9* m_capture         = nullptr;
		IDirect3DSurface9* m_capture_surface = nullptr;

		bool m_capture_has_mips = false;

		IDirect3DTexture9* m_texture = nullptr;
		IDirect3DSurface9* m_surface = nullptr;
		int m_width                  = 0;
		int m_height                 = 0;
		D3DFORMAT m_format           = D3DFMT_UNKNOWN;

		IDirect3DTexture9* m_scale_texture = nullptr;
		IDirect3DSurface9* m_scale_surface = nullptr;
		int m_scale_width                  = 0;
		int m_scale_height                 = 0;
		D3DFORMAT m_scale_format           = D3DFMT_UNKNOWN;

		int m_frames_without_pass = 0;
	};
}

inline n_resolution_spoof::impl_t g_resolution_spoof{ };
