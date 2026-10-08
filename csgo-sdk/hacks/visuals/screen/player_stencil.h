#pragma once
#include <atomic>
#include <d3d9.h>
#include <vector>

namespace n_player_stencil
{
	struct impl_t {
		bool enabled( ) const;

		bool glow_enabled( ) const;

		// main thread, right after the original DoPostScreenSpaceEffects ( = RenderGlowEffects )
		void after_glow( );

		// d3d thread, before Reset / on unload: everything here is D3DPOOL_DEFAULT
		void on_device_lost( );

	private:
		struct settings_t {
			bool m_outline = false;
			bool m_glow    = false;

			float m_visible[ 4 ]   = { };
			float m_invisible[ 4 ] = { };
			float m_thickness      = 1.f;

			float m_glow_visible[ 4 ]         = { };
			float m_glow_invisible[ 4 ]       = { };
			float m_glow_visible_outer[ 4 ]   = { };
			float m_glow_invisible_outer[ 4 ] = { };
			float m_gradient[ 4 ] = { };
			float m_wave[ 4 ]       = { };
			float m_wave_color[ 4 ] = { };

			bool m_inner                 = false;
			float m_inner_reach[ 4 ]     = { };
			float m_inner_visible[ 4 ]   = { };
			float m_inner_invisible[ 4 ] = { };

			// bones mode: hitbox capsules projected on the main thread. ends in ndc, sigma in ndc y units per end
			bool m_inner_bones = false;
			struct capsule_t {
				float m_a[ 2 ], m_b[ 2 ], m_sigma[ 2 ];
			};
			std::vector< capsule_t > m_capsules;
		};

		bool glow_wanted( ) const;
		bool inner_enabled( ) const;

		// render thread when queued, else calling thread. draw returns whether the column passes ran
		void execute( const settings_t& settings );
		bool draw( IDirect3DDevice9* device, IDirect3DSurface9* target, IDirect3DSurface9* depth_stencil, const settings_t& settings );

		bool build( IDirect3DDevice9* device, const D3DSURFACE_DESC& description );
		bool ensure_shaders( IDirect3DDevice9* device );
		void release_targets( );

		IDirect3DPixelShader9* m_fill_shader    = nullptr;
		IDirect3DPixelShader9* m_rows_shader    = nullptr;
		IDirect3DPixelShader9* m_blur_shader    = nullptr;
		IDirect3DPixelShader9* m_glow_shader    = nullptr;
		IDirect3DPixelShader9* m_columns_shader = nullptr;
		IDirect3DPixelShader9* m_inner_shader   = nullptr;
		IDirect3DPixelShader9* m_capsule_shader = nullptr;
		IDirect3DPixelShader9* m_bone_shader    = nullptr;

		IDirect3DTexture9* m_mask_texture     = nullptr;
		IDirect3DSurface9* m_mask_surface     = nullptr;
		IDirect3DTexture9* m_distance_texture = nullptr;
		IDirect3DSurface9* m_distance_surface = nullptr;
		IDirect3DTexture9* m_blur_texture     = nullptr;
		IDirect3DSurface9* m_blur_surface     = nullptr;

		IDirect3DSurface9* m_msaa_surface = nullptr;

		unsigned int m_width               = 0;
		unsigned int m_height              = 0;
		D3DFORMAT m_format                 = D3DFMT_UNKNOWN;
		D3DMULTISAMPLE_TYPE m_multi_sample = D3DMULTISAMPLE_NONE;
		DWORD m_multi_sample_quality       = 0;

		// failed compile is final. render thread writes, main reads
		std::atomic< bool > m_failed{ false };

		// glow ran on the last frame it was wanted. render thread writes, main reads
		std::atomic< bool > m_glow_live{ true };
	};
}

inline n_player_stencil::impl_t g_player_stencil{ };
