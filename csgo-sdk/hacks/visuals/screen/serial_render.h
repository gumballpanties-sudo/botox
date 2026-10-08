#pragma once

namespace n_serial_render
{
	struct impl_t {
		bool request( );

		void idle( );

		void on_release( );

	private:
		static constexpr int k_untouched = -99;

		int m_backup = k_untouched;

		int m_serial_frames = 0;

		int m_wanted_frame = -1;
	};
}

inline n_serial_render::impl_t g_serial_render{ };
