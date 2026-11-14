#pragma once
#include <atomic>
#include <cstdint>

class c_view_setup;
class c_texture;
class c_material_render_context;

/* visuals > world "rear view mirror": CViewRender::RenderView renders the view turned 180 into an own target before the main view,
   a draggable rounded imgui box shows it */
namespace n_rear_mirror
{
	/* csgo CViewSetup: operator= copies 0x14C, bitfields +0x148 ( bit 2 = m_bDoBloomAndToneMapping ), m_pCSMVolumeCuller +0xAC */
	constexpr int k_view_size   = 0x14C;
	constexpr int k_clear_flags = 0x1 | 0x2 | 0x20; /* VIEW_CLEAR_COLOR | DEPTH | STENCIL */
	constexpr int k_what_to_draw = 0x4;             /* RENDERVIEW_SUPPRESSMONITORRENDERING: no hud, no viewmodel, monitors drawn once */
	/* CViewRender m_rbTakeFreezeFrame +0x4CC, m_flFreezeFrameUntil +0x4D0 ( RenderView freeze frame test ) */
	constexpr int k_take_freeze_frame = 0x4CC, k_freeze_frame_until = 0x4D0;
	/* CViewRender m_flSmokeOverlayAmount +0x588, m_flWaterOverlayHeightTarget +0x58C, m_flWaterOverlayHeight +0x590: frametime fades */
	constexpr int k_overlay_fades = 0x588, k_overlay_fades_size = 0xC;
	/* C_CSPlayer m_bFlashScreenshotHasBeenGrabbed ( CCSViewRender::PerformFlashbangEffect ) */
	constexpr int k_flash_grabbed = 0x1046A;

	struct impl_t {
		/* game thread, top of CViewRender::RenderView. true = target pushed and `out` holds the back view: render it, then end( ) */
		bool begin( void* view_render, const c_view_setup& setup, std::uint8_t* out );
		void end( );

		/* any hook under RenderView: true inside the mirror pass, botox passes stay out of it */
		bool drawing( ) const { return this->m_drawing.load( std::memory_order_relaxed ); }

		/* render thread, imgui frame */
		void draw( );

	private:
		std::atomic< bool > m_drawing{ false };
		bool m_failed                        = false;
		int m_frame                          = -1;
		c_texture* m_texture                 = nullptr;
		c_material_render_context* m_context = nullptr;

		/* held for the pass, put back in end( ) */
		std::uint8_t* m_view_render = nullptr;
		std::uint8_t m_fades[ k_overlay_fades_size ]{ };
		std::uint8_t* m_flash[ 2 ]{ };
		std::uint8_t m_flash_was[ 2 ]{ };
		int m_pixel_visibility = -1;
		float m_next_u = 1.f, m_next_v = 1.f;

		std::atomic< c_texture* > m_shown{ nullptr };
		std::atomic< float > m_u{ 1.f }, m_v{ 1.f };
		std::atomic< int > m_width{ 0 }, m_height{ 0 };
		std::atomic< unsigned long > m_drawn_tick{ 0 };
	};
}

inline n_rear_mirror::impl_t g_rear_mirror{ };
