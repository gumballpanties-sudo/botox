#pragma once
#include "player_stencil.h"

/* viewmodel glow / stencil ( visuals > world > viewmodel ) + dropped weapon glow ( visuals > edicts ): the models stamp their own
   stencil refs, player_stencil's passes build the glow + outline from them, then zero them again before anything else reads
   ( engine halo tests == 0, player mask reads 2..127 ). refs live in the low 7 bits only: bit 0x80 belongs to the web surface
   ( websurface.cpp k_wsStencilBit, marked at post, read after colour correction ) and tonemap occlusion ( viewpostprocess.cpp:622 ).
   0x50..0x57: above the engine's health indices ( 4 + players ), below vgui popups' low bits ( 254 & 0x7f down ) */
namespace n_model_glow
{
	inline constexpr int k_ref_weapon         = 0x51;
	inline constexpr int k_ref_gloves         = 0x52;
	inline constexpr int k_ref_sleeves        = 0x53;
	inline constexpr int k_ref_dropped_seen   = 0x54;
	inline constexpr int k_ref_dropped_hidden = 0x55;
	inline constexpr int k_ref_bits           = 0x7f; /* stamp write mask + fill test mask */
	inline constexpr n_player_stencil::clear_t k_clear{ n_player_stencil::k_model_band_test, n_player_stencil::k_model_band_ref, k_ref_bits };

	struct impl_t {
		// draw_view_models hook: the live gun draws between these. the scope lens mask call never stamps ( its stencil is the game's )
		void begin_view_models( bool scope_lens_mask );
		void end_view_models( );
		bool in_view_models( ) const { return this->m_in_view_models; }

		/* draw_model_execute, main thread: ref this live viewmodel part stamps, 0 = none. part = draw_model_execute's
		   e_viewmodel_part ( 0 arms, 1 sleeve, 2 weapon ) */
		int viewmodel_ref( int part );

		// context = the draw's IMatRenderContext. ref 0 = stencil back off
		static void stamp( void* context, int ref );

		// main thread, DoPostScreenSpaceEffects before the original: the engine glow writes its refs over ours after
		void on_post_screen_space_effects( );

	private:
		static bool viewmodel_wanted( );
		static bool dropped_wanted( );

		// main thread: draws every dropped gun into the stencil ( seen / hidden refs ), returns how many
		static int stamp_dropped( );

		bool m_in_view_models = false;
		int m_stamped_frame   = -1;
		int m_captured_frame  = -1;
	};
}

inline n_model_glow::impl_t g_model_glow{ };
