#pragma once
#include "color_curve.h"

#include <d3d9.h>

namespace n_color_correction
{
	struct impl_t {
		bool on_end_scene_post( IDirect3DDevice9* device );

		IDirect3DTexture9* input_texture( ) const { return this->m_input_texture; }
		int width( ) const { return this->m_width; }
		int height( ) const { return this->m_height; }

		bool wants_pass( );

		// d3d thread, before Reset: every surface below is D3DPOOL_DEFAULT
		void on_device_lost( );

	private:
		bool build( IDirect3DDevice9* device, const D3DSURFACE_DESC& description );

		void release_targets( );
		void release( );

		enum e_grade_flag : unsigned int {
			grade_temperature = 1u << 0,
			grade_levels      = 1u << 1,
			grade_hue         = 1u << 2,
			grade_curves      = 1u << 3,
			grade_tone        = 1u << 4,
			grade_basic       = 1u << 5,
			grade_sharpen     = 1u << 6,
			grade_grain       = 1u << 7,
			grade_vignette    = 1u << 8,
			grade_invert      = 1u << 9,
			grade_posterize   = 1u << 10,
			grade_deband      = 1u << 11,
			grade_shift       = 1u << 12,

			grade_shader_mask = ( 1u << 13 ) - 1,
			grade_shader_max  = grade_shader_mask + 1
		};

		unsigned int grade_flags( );

		IDirect3DPixelShader9* ensure_shader( IDirect3DDevice9* device, unsigned int flags );

		IDirect3DPixelShader9* m_shaders[ grade_shader_max ]{ };

		bool m_shader_failed[ grade_shader_max ]{ };

		IDirect3DTexture9* m_input_texture = nullptr;
		IDirect3DSurface9* m_input_surface = nullptr;

		IDirect3DSurface9* m_resolve_surface = nullptr;

		int m_width                        = 0;
		int m_height                       = 0;
		D3DFORMAT m_format                 = D3DFMT_UNKNOWN;
		D3DMULTISAMPLE_TYPE m_multi_sample = D3DMULTISAMPLE_NONE;

		/* 256x1 curves table on s1 ( master then r / g / b ), rebaked only when a curve changes.
		   false = never built: a fresh default pool texture holds garbage */
		bool update_lut( IDirect3DDevice9* device, const n_color_curve::curve_t ( &curves )[ 4 ] );

		IDirect3DTexture9* m_lut_texture = nullptr;
		n_color_curve::curve_t m_lut_key[ 4 ]{ };
		bool m_lut_valid = false;

		// never retried
		bool m_compile_failed = false;

		unsigned int m_grain_frame = 0;
	};
}

inline n_color_correction::impl_t g_color_correction{ };
