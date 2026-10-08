#pragma once
#include <d3d9.h>

class c_view_setup;

namespace n_true_motion_blur
{
	struct impl_t {
		// runs in the DoPostScreenSpaceEffects detour ( client mode slot 44 ): after world + viewmodel, before hud.
		// main thread half, draws nothing: snapshots the view + camera motion and queues the draw.
		void on_post_screen_space_effects( c_view_setup* setup );

		// d3d thread, before IDirect3DDevice9::Reset, every target below is D3DPOOL_DEFAULT
		void on_device_lost( );

	private:
		struct frame_t {
			float m_znear = 0.f;
			float m_zfar  = 0.f;

			float m_fov      = 90.f;
			float m_prev_fov = 90.f;

			float m_reprojection[ 3 ][ 4 ]{ };

			/* the gun's own reprojection: the viewmodel is bolted to the camera so the one above says it never
			   moved. built from viewmodel world transform vs camera; covers sway, bob, lag, kick, deploy. */
			float m_viewmodel_reprojection[ 3 ][ 4 ]{ };

			float m_viewmodel_znear    = 1.f;
			float m_viewmodel_zfar     = 1000.f;
			float m_viewmodel_fov      = 68.f;
			float m_viewmodel_prev_fov = 68.f;

			bool m_viewmodel_valid = false;

			int m_bone_count = 0;

			float m_bone_center[ 16 ][ 4 ]{ };

			float m_bone[ 16 ][ 3 ][ 4 ]{ };

			int m_player_count = 0;

			float m_player_box[ 10 ][ 2 ][ 4 ]{ };

			float m_player[ 10 ][ 3 ][ 4 ]{ };

			float m_view_to_world[ 3 ][ 4 ]{ };

			float m_exposure = 0.f;

			float m_blend = 0.f;

			float m_viewmodel_blend = 0.f;

			bool m_valid = false;

			int m_frame = 0;
		};

		// main thread. player boxes, their reprojections and the view to world rows. always stores this
		// frame's poses, even on a frame it builds nothing, next frame measures against them.
		void collect_players( frame_t& frame, const float basis[ 3 ][ 3 ], const float origin[ 3 ] );

		// main thread. one view angle sample a frame, which is the history the three below read
		void sway_sample( const float angles[ 3 ], float time );

		bool sway_angles_at( float time, float out[ 3 ] ) const;

		bool sway_at( float time, float out[ 3 ] );

		bool sway_velocity( float time, float out[ 3 ] );

		// the drawing half. render thread when queued, the calling thread when not.
		void execute( frame_t frame );

		bool build( IDirect3DDevice9* device, const D3DSURFACE_DESC& description );
		void release( );

		enum e_shader { shader_info, shader_tile_h, shader_tile_v, shader_neighbour, shader_gather, shader_accumulate, shader_copy, shader_max };

		IDirect3DPixelShader9* ensure_shader( IDirect3DDevice9* device, e_shader which );

		IDirect3DPixelShader9* m_shaders[ shader_max ]{ };

		IDirect3DTexture9* m_stage_texture = nullptr;
		IDirect3DSurface9* m_stage_surface = nullptr;

		IDirect3DSurface9* m_resolve_surface = nullptr;

		IDirect3DTexture9* m_info_texture = nullptr;
		IDirect3DSurface9* m_info_surface = nullptr;

		IDirect3DTexture9* m_tile_row_texture = nullptr;
		IDirect3DSurface9* m_tile_row_surface = nullptr;

		IDirect3DTexture9* m_tile_texture = nullptr;
		IDirect3DSurface9* m_tile_surface = nullptr;

		IDirect3DTexture9* m_neighbour_texture = nullptr;
		IDirect3DSurface9* m_neighbour_surface = nullptr;

		IDirect3DTexture9* m_blur_texture = nullptr;
		IDirect3DSurface9* m_blur_surface = nullptr;

		IDirect3DTexture9* m_history_texture[ 2 ]{ };
		IDirect3DSurface9* m_history_surface[ 2 ]{ };
		int m_history_index = 0;

		bool m_history_empty = true;

		int m_width  = 0;
		int m_height = 0;

		int m_tile_size   = 0;
		int m_tile_width  = 0;
		int m_tile_height = 0;

		D3DFORMAT m_format                 = D3DFMT_UNKNOWN;
		D3DMULTISAMPLE_TYPE m_multi_sample = D3DMULTISAMPLE_NONE;

		bool m_compile_failed = false;

		bool m_active = false;

		// main thread only. the camera as it was last frame, and when that was.
		bool m_have_previous  = false;
		float m_previous_time = 0.f;
		float m_previous_fov  = 90.f;
		float m_previous_origin[ 3 ]{ };
		float m_previous_basis[ 3 ][ 3 ]{ };

		float m_previous_report = 0.f;

		/* main thread only. each player's LAST frame position by entity index, and the frame written. a slot
		   not written last frame has no history ( dormant, spawned, new owner ). position only, no rotation. */
		static constexpr int max_player_slots = 65;

		int m_player_frame[ max_player_slots ]{ };
		float m_player_pose[ max_player_slots ][ 3 ]{ };

		static constexpr int sway_samples = 128;

		int m_sway_head   = 0;
		int m_sway_filled = 0;
		int m_sway_frame  = -1;

		float m_sway_time[ sway_samples ]{ };
		float m_sway_angles[ sway_samples ][ 3 ]{ };

		// main thread only. the viewmodel's world pose last frame
		bool m_have_previous_viewmodel  = false;
		float m_previous_viewmodel_fov  = 68.f;
		float m_previous_viewmodel_origin[ 3 ]{ };
		float m_previous_viewmodel_basis[ 3 ][ 3 ]{ };
	};
}

inline n_true_motion_blur::impl_t g_true_motion_blur{ };
