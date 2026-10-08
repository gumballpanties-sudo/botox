#pragma once
#include "../../game/sdk/classes/c_vector.h"

class c_user_cmd;

namespace n_wall_climb
{
	struct impl_t {
	public:
		void on_create_move( c_user_cmd* cmd );

		[[nodiscard]] bool caught( const c_user_cmd* cmd ) const;

		[[nodiscard]] bool active( const c_user_cmd* cmd ) const;

		void check_stomp( const c_user_cmd* cmd );

		/* debug log: first stage after wc that rewrote what wc sent on a wall tick */
		void trace_stomp( const c_user_cmd* cmd, const char* stage );

		bool m_hit     = false;
		int m_act_tick = -1000;

	private:
		int m_catch_cmd    = -1;
		int m_active_cmd   = -1;
		float m_sent_fwd   = 0.f;
		float m_sent_side  = 0.f;
		int m_sent_buttons = 0;
		float m_sent_yaw   = 0.f;
		int m_stomp_cmd    = -1;
		int m_pred_cmd     = -1; /* debug log: END of what wc sent, checked against the next START ( `div` ) */
		bool m_pred_stomped = false;
		c_vector m_pred_org{ }, m_pred_vel{ };
		float m_ground_z = -1e9f;
		bool m_holding   = false;
		int m_free_cmd   = -1; /* cmd wc sent off a sim that ended unpinned */
		bool m_park_off  = false;
	};
}

inline n_wall_climb::impl_t g_wall_climb{ };
