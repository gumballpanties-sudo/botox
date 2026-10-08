#pragma once
#include <d3d9.h>

class c_view_setup;

namespace n_depth_of_field
{
	struct impl_t {
		void on_post_screen_space_effects( c_view_setup* setup );

		// d3d thread, before IDirect3DDevice9::Reset, everything below is D3DPOOL_DEFAULT
		void on_device_lost( );

	private:
		struct frame_t {
			float m_znear = 0.f;
			float m_zfar  = 0.f;

			float m_focus = 0.f;

			int m_frame = 0;

			float m_time = 0.f;
		};

		// the drawing half. render thread when queued, the calling thread when not.
		void execute( frame_t frame );

		bool build( IDirect3DDevice9* device, const D3DSURFACE_DESC& description );
		void release( );

		void selftest_intz( IDirect3DDevice9* device, IDirect3DSurface9* target );

		float focus_distance( c_view_setup* setup );

		IDirect3DPixelShader9* ensure_shader( IDirect3DDevice9* device, int style, int quality, bool debug );

		IDirect3DPixelShader9* m_shaders[ 13 ]{ };

		bool m_shader_failed[ 13 ]{ };
		// frame into stage texture ( back buffer format, so StretchRect accepts it ), then one pass packs colour +
		// signed coc into alpha, so the gather does one fetch per tap and never touches depth again.
		IDirect3DTexture9* m_stage_texture  = nullptr;
		IDirect3DSurface9* m_stage_surface  = nullptr;
		IDirect3DTexture9* m_colour_texture = nullptr;
		IDirect3DSurface9* m_colour_surface = nullptr;

		IDirect3DPixelShader9* ensure_prepare_shader( IDirect3DDevice9* device );

		IDirect3DPixelShader9* m_prepare_shader = nullptr;
		bool m_prepare_failed                   = false;

		IDirect3DPixelShader9* ensure_smooth_shader( IDirect3DDevice9* device );

		IDirect3DPixelShader9* m_smooth_shader = nullptr;
		bool m_smooth_failed                   = false;

		IDirect3DTexture9* m_blur_texture = nullptr;
		IDirect3DSurface9* m_blur_surface = nullptr;

		IDirect3DPixelShader9* ensure_coc_shader( IDirect3DDevice9* device );
		IDirect3DPixelShader9* ensure_near_shader( IDirect3DDevice9* device, int style, int quality );

		IDirect3DPixelShader9* m_coc_shader = nullptr;
		bool m_coc_failed                   = false;

		IDirect3DPixelShader9* m_near_shaders[ 12 ]{ };
		bool m_near_failed[ 12 ]{ };

		IDirect3DTexture9* m_coc_texture     = nullptr;
		IDirect3DSurface9* m_coc_surface     = nullptr;
		IDirect3DTexture9* m_coc_tmp_texture = nullptr;
		IDirect3DSurface9* m_coc_tmp_surface = nullptr;

		int m_coc_width  = 0;
		int m_coc_height = 0;

		IDirect3DSurface9* m_resolve_surface = nullptr;

		int m_selftest_state = 0;

		IDirect3DTexture9* m_probe_texture = nullptr;
		IDirect3DSurface9* m_probe_surface = nullptr;
		IDirect3DSurface9* m_probe_sysmem  = nullptr;

		float m_probe_time = 0.f;

		int m_width                        = 0;
		int m_height                       = 0;
		D3DFORMAT m_format                 = D3DFMT_UNKNOWN;
		D3DMULTISAMPLE_TYPE m_multi_sample = D3DMULTISAMPLE_NONE;

		float m_focus = 0.f;

		// a compile that failed once is never retried, it fails the same way every frame
		bool m_compile_failed = false;

		bool m_active = false;
	};
}

inline n_depth_of_field::impl_t g_depth_of_field{ };
