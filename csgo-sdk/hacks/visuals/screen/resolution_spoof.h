#pragma once
#include "stream_guard.h"

#include <d3d9.h>
#include <functional>
#include <vector>

namespace n_resolution_spoof
{
	struct impl_t {
		void on_present( IDirect3DDevice9* device, const std::function< void( ) >& draw = { } );

		bool wants_overlay_pass( );

		/* ...only while that pass really runs. if Present never reaches us the draw goes nowhere,
		   so end_scene keeps it once the pass missed a couple of frames. */
		bool pass_is_running( );

		void on_end_scene( );

		bool enabled( );

		/* aspect changer, every stretch site reads these. res spoof "stretch" = a 4:3 res fills a 16:9 screen like a
		   real stretched res: world + hud + crosshair take the res shape, cheat overlays get it from the upscale */
		bool stretch_res( );
		bool upscale_stretches( );
		bool aspect_on( );
		float aspect_wanted( );
		bool aspect_hud( );
		bool aspect_crosshair( );
		bool aspect_cheat_overlay( );

		/* screen space half: how much wider a screen space draw must get to match the stretched world.
		   1 = nothing. crosshair paths only; world draws get it via the engine matrix. */
		float overlay_aspect_scale( );

		float aspect_scale( );

		bool low_res_grid( float& pixels_x, float& pixels_y );

		/* classic crosshair ( HudWeapon paint, game thread ). while the overlay pass runs its rects leave the full res
		   frame and get rebuilt with the game's own math at the spoofed height in the low res target: whole low res
		   pixels, stretched once by the upscale. crunched instead they bled into the next cells = soft + twice as wide */
		void crosshair_color( int r, int g, int b, int a, bool outline );
		bool crosshair_capture( int x0, int y0, int x1, int y1 );
		void crosshair_flush( );

		void on_imgui_new_frame( );

		// game thread: net_graphheight is a convar write, not on the d3d thread
		void on_frame_stage_notify( );

		// d3d thread, before Reset: a default pool surface must not survive it
		void on_device_lost( );

		void on_release( );

		int m_screen_width  = 0;
		int m_screen_height = 0;

		void target_size( int screen_width, int screen_height, int& width, int& height );

	private:
		void release( );

		/* manual get / set of exactly what the quad touches. never a D3DSBT_ALL block: its Apply under the engine's
		   state cache is what moved world textures with the view ( ref-csgo-pure-device-get-state ) */
		void begin_quad_state( IDirect3DDevice9* device );

		// shader = a ps_3_0 is bound, so the draw goes through n_fx_compat::draw_up ( vs_3_0 pair, amd )
		bool draw_quad( IDirect3DDevice9* device, IDirect3DTexture9* texture, int quad_width, int quad_height, bool shader = false );

		void end_quad_state( IDirect3DDevice9* device );

		/* downscale = exact area average: every low res pixel is the mean of the screen px it covers, split px weighted
		   by overlap ( gamma space, like an msaa resolve ). trilinear mips picked the lod off the wider axis
		   ( 1920x1080 -> 1280x960: 1.5 ) and blurred the 1.125 axis to ~540 lines. box is separable: x pass into
		   m_columns ( target width x screen height ), y pass into the target. one loop per pass: nested loops take
		   d3dx 10 - 40 s to compile */
		bool area_shader( IDirect3DDevice9* device );

		IDirect3DPixelShader9* m_area_shader = nullptr;
		bool m_area_failed                   = false;

		struct quad_state_t {
			IDirect3DVertexShader9* m_vertex_shader    = nullptr;
			IDirect3DPixelShader9* m_pixel_shader      = nullptr;
			IDirect3DVertexDeclaration9* m_declaration = nullptr;
			IDirect3DBaseTexture9* m_texture           = nullptr;
			DWORD m_fvf                                = 0;
			D3DVIEWPORT9 m_viewport{ };
			DWORD m_render[ 14 ]{ };
			DWORD m_stage[ 8 ]{ };
			DWORD m_sampler[ 8 ]{ };
			float m_constants[ 2 ][ 4 ]{ };
			int m_int_constant[ 4 ]{ };
			n_stream_guard::state_t m_streams{ };
			bool m_captured = false;
		} m_quad{ };

		void net_graph_scale( bool enabled );

		// screen size the targets were built for
		int m_built_screen_width  = 0;
		int m_built_screen_height = 0;

		IDirect3DTexture9* m_capture         = nullptr;
		IDirect3DSurface9* m_capture_surface = nullptr;

		// area x pass result, fp16 when the card has it ( 8 bit = +-0.5 lsb before the y pass )
		IDirect3DTexture9* m_columns         = nullptr;
		IDirect3DSurface9* m_columns_surface = nullptr;

		IDirect3DTexture9* m_texture = nullptr;
		IDirect3DSurface9* m_surface = nullptr;
		int m_width                  = 0;
		int m_height                 = 0;
		D3DFORMAT m_format           = D3DFMT_UNKNOWN;

		int m_frames_without_pass = 0;

		struct crosshair_rect_t {
			int m_x0, m_y0, m_x1, m_y1;
			unsigned int m_color;
			bool m_outline;
		};

		// game thread only: filled under HudWeapon, drained by MatSystemTopPanel the same frame
		crosshair_rect_t m_crosshair[ 48 ]{ };
		int m_crosshair_count        = 0;
		unsigned int m_crosshair_rgba = 0;
		bool m_crosshair_is_outline  = false;
	};
}

inline n_resolution_spoof::impl_t g_resolution_spoof{ };
