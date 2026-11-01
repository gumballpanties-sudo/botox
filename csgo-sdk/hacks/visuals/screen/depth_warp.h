#pragma once
#include <d3d9.h>

class c_view_setup;

namespace n_depth_warp
{
	struct impl_t {
		void on_post_screen_space_effects( c_view_setup* setup );

		// d3d thread, before Reset: every target is D3DPOOL_DEFAULT
		void on_device_lost( );

	private:
		struct frame_t {
			float m_znear  = 0.f;
			float m_zfar   = 0.f;
			double m_clock = 0.0;
			int m_frame    = 0;
		};

		// render thread when queued, calling thread when not
		void execute( frame_t frame );

		bool build( IDirect3DDevice9* device, const D3DSURFACE_DESC& description, bool history );
		void release( );

		IDirect3DPixelShader9* ensure_shader( IDirect3DDevice9* device, int effect, int write, int blend );

		IDirect3DPixelShader9* m_shaders[ 11 ][ 4 ]{ };
		IDirect3DPixelShader9* m_write[ 2 ]{ };
		bool m_failed[ 11 ][ 4 ]{ };
		bool m_write_failed[ 2 ]{ };

		IDirect3DTexture9* m_frame_texture = nullptr;
		IDirect3DSurface9* m_frame_surface = nullptr;
		IDirect3DSurface9* m_resolve_surface = nullptr;

		// slit scan history, persists across frames
		IDirect3DTexture9* m_history_texture = nullptr;
		IDirect3DSurface9* m_history_surface = nullptr;
		int m_history_effect = -1;

		int m_width                        = 0;
		int m_height                       = 0;
		D3DFORMAT m_format                 = D3DFMT_UNKNOWN;
		D3DMULTISAMPLE_TYPE m_multi_sample = D3DMULTISAMPLE_NONE;

		unsigned m_scan_frame = 0;
		int m_logged_effect   = -2;
		bool m_active         = false;
	};
}

inline n_depth_warp::impl_t g_depth_warp{ };
