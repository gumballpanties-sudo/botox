#pragma once
#include "../../game/sdk/classes/c_angle.h"
#include "../../game/sdk/classes/c_vector.h"

#include <array>

class c_base_entity;
class c_user_cmd;

namespace n_grenade_aim
{
	struct impl_t {
		/* inside prediction begin/end: origin/velocity must be post-move ( ThrowGrenade reads them after ProcessMovement ) */
		void on_create_move( );

		/* after every other view writer, or the throw tick angle gets overwritten */
		void apply( c_user_cmd* cmd );

		void on_paint_traverse( );

	private:
		void aim( c_base_entity* weapon, short item_index, float max_time, bool pin_pulled, bool throw_tick );

		bool m_throw         = false;
		bool m_auto_released = false;
		float m_release_time = 0.f;
		int m_lock           = 0;
		c_angle m_view       = { };

		/* 3 s max flight at 128 tick + launch + end */
		std::array< c_vector, 400 > m_path = { };
		int m_path_count                   = 0;
	};
}

inline n_grenade_aim::impl_t g_grenade_aim{ };
