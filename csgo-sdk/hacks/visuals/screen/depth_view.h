#pragma once
#include <d3d9.h>

class c_view_setup;

namespace n_depth_view
{
	struct impl_t {
		// emphasize + show depth, first screen pass
		void on_post_screen_space_effects( c_view_setup* setup );

		// adaptive fog ( OtisFX AdaptiveFog.fx ), after ao, before depth warp + dof. shares the frame copy
		void on_post_screen_space_effects_fog( c_view_setup* setup );

		// d3d thread, before Reset: every target is D3DPOOL_DEFAULT
		void on_device_lost( );

	private:
		struct frame_t {
			int m_frame    = 0;
			float m_znear  = 0.f;
			float m_zfar   = 0.f;
			bool m_fog     = false;
		};

		void submit( c_view_setup* setup, bool fog );

		// render thread when queued, calling thread when not
		void execute( frame_t frame );

		void release( );

		IDirect3DPixelShader9* ensure_shader( IDirect3DDevice9* device, bool fog );

		IDirect3DPixelShader9* m_shader     = nullptr;
		IDirect3DPixelShader9* m_fog_shader = nullptr;

		int m_width  = 0;
		int m_height = 0;

		// card refused ps_3_0: never retried
		bool m_compile_failed     = false;
		bool m_fog_compile_failed = false;

		bool m_active = false;
	};
}

inline n_depth_view::impl_t g_depth_view{ };
