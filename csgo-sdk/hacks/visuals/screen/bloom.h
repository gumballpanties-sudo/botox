#pragma once
#include <d3d9.h>

class c_view_setup;

namespace n_bloom
{
	constexpr int k_levels = 8;

	struct impl_t {
		// runs in DoPostScreenSpaceEffects so the glow hits the world, never the hud or our overlay
		void on_post_screen_space_effects( c_view_setup* setup );

		// d3d thread, before Reset: every target below is D3DPOOL_DEFAULT
		void on_device_lost( );

	private:
		struct frame_t {
			float m_frame_time = 0.f;
		};

		// render thread when queued, calling thread when not
		void execute( frame_t frame );

		bool build( IDirect3DDevice9* device, const D3DSURFACE_DESC& description );
		void release_targets( );
		void release( );

		bool check_format( IDirect3DDevice9* device );

		enum e_shader : int {
			shader_prepass = 0,
			shader_downsample,
			shader_adapt,
			shader_upsample,
			shader_combine,
			shader_max
		};

		IDirect3DPixelShader9* ensure_shader( IDirect3DDevice9* device, int shader );

		IDirect3DPixelShader9* m_shaders[ shader_max ]{ };

		bool m_shader_failed[ shader_max ]{ };

		IDirect3DTexture9* m_frame_texture = nullptr;
		IDirect3DSurface9* m_frame_surface = nullptr;

		IDirect3DSurface9* m_resolve_surface = nullptr;

		IDirect3DTexture9* m_textures[ k_levels ]{ };
		IDirect3DSurface9* m_surfaces[ k_levels ]{ };

		int m_sizes[ k_levels ][ 2 ]{ };

		IDirect3DTexture9* m_adapt_texture = nullptr;
		IDirect3DSurface9* m_adapt_surface = nullptr;

		int m_width                        = 0;
		int m_height                       = 0;
		D3DFORMAT m_format                 = D3DFMT_UNKNOWN;
		D3DMULTISAMPLE_TYPE m_multi_sample = D3DMULTISAMPLE_NONE;

		// never retried
		bool m_compile_failed = false;

		bool m_format_failed = false;

		bool m_active = false;
	};
}

inline n_bloom::impl_t g_bloom{ };
