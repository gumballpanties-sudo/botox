#pragma once
#include <d3d9.h>

class c_view_setup;
class c_base_entity;
class c_vector;

namespace n_ambient_occlusion
{
	struct impl_t {
		void on_post_screen_space_effects( c_view_setup* setup );

		// d3d thread, before Reset: both targets are D3DPOOL_DEFAULT
		void on_device_lost( );

	private:
		// main-thread snapshot for the draw. view setup and entity list are gone by then: no game pointers here.
		struct frame_t {
			float m_znear   = 0.f;
			float m_zfar    = 0.f;
			float m_fov     = 90.f;

			int m_frame = 0;

			float m_fog_start   = 0.f;
			float m_fog_end     = 0.f;
			float m_fog_density = 0.f;

			float m_smoke[ 6 ][ 4 ]{ };
			int m_smoke_count = 0;

			bool m_smoke_inside = false;
		};

		// render thread when queued, calling thread when not
		void execute( frame_t frame );

		bool build( IDirect3DDevice9* device, const D3DSURFACE_DESC& description );
		void release( );

		void read_fog( );

		bool fog_from_entity( c_base_entity* entity );

		void read_smokes( const c_view_setup* setup, frame_t& frame );

		bool smoke_from_entity( c_base_entity* entity, c_vector& centre, float& radius );

		enum e_shader { shader_prepare, shader_occlusion, shader_composite, shader_max };

		IDirect3DPixelShader9* ensure_shader( IDirect3DDevice9* device, e_shader which );

		IDirect3DPixelShader9* m_shaders[ shader_max ]{ };

		IDirect3DTexture9* m_normal_texture = nullptr;
		IDirect3DSurface9* m_normal_surface = nullptr;

		IDirect3DTexture9* m_occlusion_texture = nullptr;
		IDirect3DSurface9* m_occlusion_surface = nullptr;

		int m_width  = 0;
		int m_height = 0;

		int m_occlusion_width  = 0;
		int m_occlusion_height = 0;

		float m_scale = 0.f;

		float m_fog_start   = 0.f;
		float m_fog_end     = 0.f;
		float m_fog_density = 0.f;
		int m_fog_frame     = -1;

		int m_fog_index      = 0;
		int m_fog_scan_frame = -64;

		int m_smoke_indices[ 6 ]{ };
		int m_smoke_index_count = 0;
		int m_smoke_scan_frame  = -32;

		bool m_compile_failed = false;

		// targets exist to tear down; set by the draw, read by the main thread so an off pass stops queueing
		bool m_active = false;
	};
}

inline n_ambient_occlusion::impl_t g_ambient_occlusion{ };
