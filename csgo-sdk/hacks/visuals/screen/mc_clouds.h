#pragma once
#include <d3d9.h>

class c_view_setup;

namespace n_mc_clouds
{
	struct impl_t {
		// main thread, after ClientModeShared::OverrideView
		void on_override_view( const c_view_setup* setup );

		// main thread, after IVRenderView::DrawWorldLists queued this view's 2d sky + opaque world
		void on_draw_world_lists( unsigned long flags );

		// d3d thread, before Reset
		void on_device_lost( );

	private:
		struct frame_t {
			float m_offset[ 2 ]{ };
		};

		// render thread when queued, calling thread when not
		void execute( const frame_t& frame );

		bool ensure( IDirect3DDevice9* device );

		IDirect3DPixelShader9* m_shader = nullptr;
		IDirect3DTexture9* m_cells      = nullptr;

		// card refused ps_3_0 / texture: never retried
		bool m_failed = false;

		float m_origin[ 2 ]{ };
	};
}

inline n_mc_clouds::impl_t g_mc_clouds{ };
