#pragma once

class c_view_setup;

namespace n_overlays
{
	struct impl_t {
		void on_paint_traverse( );
		void on_frame_stage_notify( int stage );
		bool hides_scope( );

	private:
		void line_of_sight( );
		void instant_scope( );
		void spread_circle( );
		void penetration_reticle( );
		void scope_lines( );

		int m_sight_tick      = -1;
		int m_reticle_tick    = -1;
		unsigned int m_reticle_color = 0;
	};
}

inline n_overlays::impl_t g_overlays{ };
