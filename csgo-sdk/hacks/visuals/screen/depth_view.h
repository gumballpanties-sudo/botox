#pragma once
#include <d3d9.h>

class c_view_setup;

namespace n_depth_view
{
	struct impl_t {
		void on_post_screen_space_effects( c_view_setup* setup );

		// d3d thread, before Reset: every target is D3DPOOL_DEFAULT
		void on_device_lost( );

	private:
		struct frame_t {
			int m_frame = 0;
		};

		// render thread when queued, calling thread when not
		void execute( frame_t frame );

		bool build( IDirect3DDevice9* device, const D3DSURFACE_DESC& description );
		void release( );

		IDirect3DPixelShader9* ensure_shader( IDirect3DDevice9* device );

		IDirect3DPixelShader9* m_shader = nullptr;

		IDirect3DTexture9* m_frame_texture = nullptr;
		IDirect3DSurface9* m_frame_surface = nullptr;

		IDirect3DSurface9* m_resolve_surface = nullptr;

		int m_width                        = 0;
		int m_height                       = 0;
		D3DFORMAT m_format                 = D3DFMT_UNKNOWN;
		D3DMULTISAMPLE_TYPE m_multi_sample = D3DMULTISAMPLE_NONE;

		// card refused ps_3_0: never retried
		bool m_compile_failed = false;

		bool m_active = false;
	};
}

inline n_depth_view::impl_t g_depth_view{ };
