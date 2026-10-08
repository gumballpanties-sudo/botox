#pragma once

namespace n_indicators
{
	struct impl_t {
		void on_paint_traverse( );

		/* render thread, before Reset and on unload: frees the per-frame texture pool (DEFAULT
		   pool on d3d9ex). remade on next use */
		void release_textures( );

		struct indicator_data_t {
			void reset( )
			{
				this->m_last_velocity           = 0;
				this->m_tick_prev_velocity      = 0;
				this->m_take_off_velocity       = 0;
				this->m_take_off_time_velocity  = 0.f;
				this->m_last_on_ground_velocity = false;

				this->m_last_stamina           = 0.f;
				this->m_tick_prev_stamina      = 0.f;
				this->m_take_off_stamina       = 0.f;
				this->m_take_off_time_stamina  = 0.f;
				this->m_last_on_ground_stamina = false;
			}

			int m_last_velocity            = 0;
			int m_tick_prev_velocity       = 0;
			int m_take_off_velocity        = 0;
			float m_take_off_time_velocity = 0.f;
			bool m_last_on_ground_velocity = false;

			float m_last_stamina          = 0.f;
			float m_tick_prev_stamina     = 0.f;
			float m_take_off_stamina      = 0.f;
			float m_take_off_time_stamina = 0.f;
			bool m_last_on_ground_stamina = false;
		} m_indicator_data;

	private:
		void sniper_crosshair( );
		void sniper_crosshair_clarity( const float cx, const float cy, const bool outline );
		void sniper_crosshair_interwebz( const float cx, const float cy );
		void sniper_crosshair_custom( );
		void aimbot_fov_circle( );
		void keybind_indicators( );
		void key_press( );
		void velocity( const bool on_ground );
		void stamina( const bool on_ground );
		void velocity_graph( const bool on_ground );
		void fps_warning( );
	};
}

inline n_indicators::impl_t g_indicators{ };
