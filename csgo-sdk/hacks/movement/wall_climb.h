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
		void reset_latch( );

		void show_wall_slope( );

		bool m_slope_down  = false;
		int m_catch_cmd    = -1;
		int m_active_cmd   = -1;
		float m_sent_fwd   = 0.f;
		float m_sent_side  = 0.f;
		int m_sent_buttons = 0;
		float m_sent_yaw   = 0.f;
		int m_stomp_cmd    = -1;

		int m_div_cmd             = -1;
		c_vector m_div_org        = { };
		c_vector m_div_vel        = { };
		const char* m_div_branch  = "";

		float m_scan_angle    = 0.f;
		int m_last_tick       = -1000;
		float m_last_off      = 0.f;
		float m_last_fwd      = 0.f;
		float m_last_wall_yaw = 0.f;
		float m_last_side     = 0.f;

		bool m_latch_arm    = false;
		bool m_latch_hold   = false;
		int m_latch_wait    = 0;
		float m_latch_yaw   = 0.f;
		float m_latch_fwd   = 0.f;
		int m_latch_stand   = 0;
		int m_latch_duck    = 0;
	};
}

inline n_wall_climb::impl_t g_wall_climb{ };
