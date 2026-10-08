#pragma once
#include <d3d9.h>

namespace n_depth_source
{
	enum route_t : int { route_none = 0, route_resz, route_nvapi, route_live };

	enum verify_t : int { verify_none = 0, verify_clear, verify_wait, verify_ok, verify_skipped };

	struct impl_t {
		void on_created_texture( IDirect3DDevice9* device, IDirect3DTexture9* texture, unsigned int width, unsigned int height, D3DFORMAT format );

		// d3d thread, before Reset: everything here is D3DPOOL_DEFAULT
		void on_device_lost( );

		bool ensure( IDirect3DDevice9* device, const D3DSURFACE_DESC& description );

		void resolve( IDirect3DDevice9* device, IDirect3DSurface9* bound_depth, int frame );

		IDirect3DTexture9* texture( ) const;

		// true = depth never goes through the engine's material system copy, passes may queue
		bool own_route( ) const { return this->m_own_depth != nullptr; }

		IDirect3DTexture9* own_depth( ) const { return this->m_own_depth; }

		void skip_next_resolve( ) { this->m_skip_resolve = true; }

		bool live_route( ) const { return this->m_route == route_live; }

		IDirect3DSurface9* map_depth_stencil( IDirect3DSurface9* surface ) const
		{
			return this->m_live && surface && surface == this->m_auto_depth ? this->m_own_surface : surface;
		}

		// d3d thread, between frames: route 3 goes live, route checks run
		void on_present( IDirect3DDevice9* device );

		const char* route_name( ) const;
		const char* verify_name( ) const;

	private:
		bool pick_live_route( IDirect3DDevice9* device, const D3DSURFACE_DESC& description );

		void resolve_resz( IDirect3DDevice9* device, IDirect3DSurface9* depth_surface );

		bool nvapi_ready( );
		void resolve_nvapi( IDirect3DDevice9* device, IDirect3DSurface9* depth_surface );

		bool clear_sentinel( IDirect3DDevice9* device );

		// samples an 8x8 grid of our INTZ; still all sentinel = route never wrote, drop it
		void verify( IDirect3DDevice9* device );
		bool ensure_probe( IDirect3DDevice9* device );
		void release_probe( );

		void release( );

		IDirect3DTexture9* m_depth_texture = nullptr;

		IDirect3DTexture9* m_own_depth = nullptr;

		route_t m_route = route_none;

		int m_failed = 0;

		bool m_rebuild = false;

		bool m_resz_supported = false;

		bool m_nvapi_checked             = false;
		bool m_nvapi_available           = false;
		void* m_nvapi_registered_texture = nullptr;
		void* m_nvapi_registered_surface = nullptr;

		bool m_ignore_capture = false;

		bool m_skip_resolve = false;

		bool m_live                      = false;
		bool m_live_forced               = false;
		IDirect3DSurface9* m_auto_depth  = nullptr;
		IDirect3DSurface9* m_own_surface = nullptr;

		verify_t m_verify   = verify_none;
		int m_verify_frames = 0;
		int m_resolves      = 0;

		IDirect3DTexture9* m_probe_texture = nullptr;
		IDirect3DSurface9* m_probe_surface = nullptr;
		IDirect3DSurface9* m_probe_sysmem  = nullptr;
		IDirect3DPixelShader9* m_probe_ps  = nullptr;

		int m_resolved_frame = -1;

		unsigned int m_width  = 0;
		unsigned int m_height = 0;
	};
}

inline n_depth_source::impl_t g_depth_source{ };
