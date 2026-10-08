#pragma once

class c_view_setup;
struct rect_t;

namespace n_screen
{
	struct impl_t {
		bool on_draw_view_models( c_view_setup& setup );

		// CViewRender::SetUpView calls IClientModeShared::OverrideView, slot 18
		void on_override_view( c_view_setup* setup );

		// third person + aspect ratio must land before CalcView; FRAME_RENDER_START is the last stage ahead of it
		void on_frame_stage_notify( int stage );

		void on_release( );

	private:
		void update_screen_effect_texture( int texture_index, int x, int y, int w, int h, bool dest_fullscreen = false, rect_t* actual_rect = { } );

		void viewmodel_offset( );
		void viewmodel_convars( bool restore = false );

		void third_person( bool restore = false );
		void aspect_ratio( bool restore = false );
	};
}

inline n_screen::impl_t g_screen{ };