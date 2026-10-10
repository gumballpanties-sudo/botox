#pragma once
#include <atomic>
#include <d3d9.h>

class c_view_setup;
class c_user_cmd;
struct view_matrix_t;

namespace n_flip_world
{
	struct impl_t {
		void on_render_start( );

		// end of DoPostScreenSpaceEffects: world, gun, glow, post passes all drawn
		void on_post_screen_space_effects( const c_view_setup* setup );

		// IVRenderView::Push2DView (45): HUD view copies CRender::m_matrixWorldToScreen here
		void on_push_2d_view( );

		// IVEngineClient::WorldToScreenMatrix (37)
		const view_matrix_t& on_world_to_screen_matrix( const view_matrix_t& matrix );

		void on_mouse_input( float* x );
		void on_create_move( c_user_cmd* cmd );

		bool mirroring( ) const { return this->m_mirroring.load( std::memory_order_relaxed ); }

		void on_device_lost( );
		void on_release( );

	private:
		struct slot_t {
			float* m_matrix = nullptr;
			float m_written[ 16 ]{ };
			bool m_held = false;
		};

		struct view_rect_t {
			int x = 0, y = 0, w = 0, h = 0;
		};

		bool wanted( ) const;

		void hand( bool hold );

		void apply( slot_t& slot );
		void restore( slot_t& slot );

		void execute( view_rect_t rect );
		bool build( IDirect3DDevice9* device, const D3DSURFACE_DESC& description );
		void release_targets( );

		slot_t m_engine{ };
		slot_t m_client{ };
		bool m_client_resolved = false;

		std::atomic< unsigned long > m_game_thread{ 0 };
		std::atomic< unsigned long > m_render_thread{ 0 };
		std::atomic< bool > m_mirroring{ false };
		bool m_frame_mirrored = false;

		int m_hand_own = -1;

		IDirect3DTexture9* m_texture       = nullptr;
		IDirect3DSurface9* m_surface       = nullptr;
		IDirect3DSurface9* m_resolve       = nullptr;
		IDirect3DPixelShader9* m_shader    = nullptr;
		D3DSURFACE_DESC m_description{ };
		std::atomic< bool > m_failed{ false };
	};
}

inline n_flip_world::impl_t g_flip_world{ };
