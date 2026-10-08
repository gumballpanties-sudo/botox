#pragma once
#include <d3d9.h>
#include <memory>
#include <string>
#include <vector>

class c_view_setup;

// GAME search path (loose + vpk). length 0 = to end of file
bool read_game_file( const char* path, std::vector< unsigned char >& out, unsigned int offset = 0, unsigned int length = 0 );

namespace n_reflections
{
	struct cube_data_t {
		D3DFORMAT m_format = D3DFMT_UNKNOWN;
		int m_size         = 0;
		int m_pitch = 0;
		int m_rows  = 0;
		std::vector< unsigned char > m_faces[ 6 ];
	};

	struct impl_t {
		void on_post_screen_space_effects( c_view_setup* setup );

		// d3d thread, before Reset: every target is D3DPOOL_DEFAULT
		void on_device_lost( );

		// leak watch only: unlocked read of the render side's upload cache
		std::size_t cube_texture_count( ) const { return this->m_cube_textures.size( ); }

	private:
		struct frame_t {
			float m_fov = 90.f;

			int m_frame = 0;

			float m_right[ 3 ]{ };
			float m_down[ 3 ]{ };
			float m_forward[ 3 ]{ };

			std::shared_ptr< const cube_data_t > m_cube[ 2 ];
			float m_cube_blend = 1.f;

			int m_generation = 0;
		};

		// render thread when queued, calling thread when not
		void execute( frame_t frame );

		bool build( IDirect3DDevice9* device, const D3DSURFACE_DESC& description );
		void release_targets( );
		void release( );

		// main thread: nearest map cubemap to the eye, loaded on first use
		void pick_cubemap( const c_view_setup* setup, frame_t& frame );
		void load_cube_samples( );
		std::shared_ptr< const cube_data_t > load_cube( int index );

		// render thread: upload cache, dropped on map change / device lost / switch off
		IDirect3DCubeTexture9* cube_texture( IDirect3DDevice9* device, const std::shared_ptr< const cube_data_t >& data );
		void release_cubes( );

		enum e_shader { shader_normals, shader_smooth, shader_trace, shader_filter, shader_composite, shader_max };

		IDirect3DPixelShader9* ensure_shader( IDirect3DDevice9* device, e_shader which );

		IDirect3DPixelShader9* m_shaders[ shader_max ]{ };

		IDirect3DTexture9* m_frame_texture = nullptr;
		IDirect3DSurface9* m_frame_surface = nullptr;

		IDirect3DSurface9* m_resolve_surface = nullptr;

		IDirect3DTexture9* m_normal_texture[ 2 ]{ };
		IDirect3DSurface9* m_normal_surface[ 2 ]{ };

		IDirect3DTexture9* m_reflection_texture[ 2 ]{ };
		IDirect3DSurface9* m_reflection_surface[ 2 ]{ };

		int m_width                        = 0;
		int m_height                       = 0;
		int m_trace_width                  = 0;
		int m_trace_height                 = 0;
		D3DFORMAT m_format                 = D3DFMT_UNKNOWN;
		D3DMULTISAMPLE_TYPE m_multi_sample = D3DMULTISAMPLE_NONE;

		// main thread cubemap state. origins off the bsp's LUMP_CUBEMAPS
		struct cube_sample_t {
			int m_origin[ 3 ];
		};

		std::string m_level{ };
		std::vector< cube_sample_t > m_cube_samples{ };
		std::vector< std::shared_ptr< const cube_data_t > > m_cube_data{ };
		std::vector< bool > m_cube_tried{ };
		std::vector< int > m_cube_recent{ };
		int m_cube_current      = -1;
		int m_cube_previous     = -1;
		float m_cube_switch     = 0.f;
		int m_generation        = 0;

		// render thread upload cache
		struct cube_texture_t {
			std::shared_ptr< const cube_data_t > m_data;
			IDirect3DCubeTexture9* m_texture = nullptr;
		};

		std::vector< cube_texture_t > m_cube_textures{ };
		int m_cube_generation = -1;

		bool m_compile_failed = false;

		bool m_active = false;
	};
}

inline n_reflections::impl_t g_reflections{ };
