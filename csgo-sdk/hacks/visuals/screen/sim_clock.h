#pragma once
#include "../../../game/sdk/classes/c_global_vars_base.h"

#include <algorithm>

namespace n_sim_clock
{
	// realtime = wall clock ( host.cpp Host_AccumulateTime ), blind to host_framerate / host_timescale / demo speed.
	// missed frames add 1 s so every "too long since last frame" gate resets. main thread only
	inline float now( const c_global_vars_base* globals )
	{
		static int frame    = -1;
		static double clock = 0.0;

		if ( !globals )
			return 0.f;

		if ( globals->m_frame_count != frame ) {
			clock += globals->m_frame_count - frame == 1 ? std::clamp( globals->m_abs_frame_time, 0.f, 0.25f ) : 1.0;
			frame = globals->m_frame_count;
		}

		return static_cast< float >( clock );
	}
}
