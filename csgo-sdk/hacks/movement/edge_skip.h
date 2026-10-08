#pragma once
#include "../../game/sdk/classes/c_vector.h"
#include "../../game/sdk/classes/c_angle.h"
#include "tick_scale.h"
class c_user_cmd;

namespace n_edge_skip
{
	constexpr int k_max_ticks = 256;

	float perfect_angle( const c_vector& vel, float friction, float max_speed );

	struct impl_t {
		void pre( c_user_cmd* cmd );
		void post( c_user_cmd* cmd );
		void reset( bool forget = false );
		bool active( ) const { return m_active; }
		float lock_amount( ) const;
		void render( ) const;

	private:
		struct tick_t {
			float yaw  = 0.f;
			float side = 0.f;
			bool duck  = false;
			c_vector origin{ };
		};
		void search( c_user_cmd* cmd );
		bool family( c_user_cmd* cmd, bool first_neg );
		bool grab( c_user_cmd* cmd, int prefix, float ground_z, bool first_neg, int window );
		void apply( c_user_cmd* cmd, const tick_t& t, const c_angle& view ) const;
		void sim( c_user_cmd* cmd, int index, const tick_t& t );
		bool replay_to( c_user_cmd* cmd, int end );
		bool out( ) const;
		bool bent( );

		int m_start_flags     = 0;
		int m_start_move_type = 0;
		c_vector m_start_vel{ };
		int m_user_buttons = 0;
		bool m_eb_latched  = false;
		float m_ground_z  = 0.f;
		bool m_had_ground = false;
		int m_air         = 0;
		float m_prev_yaw = 0.f;
		bool m_yaw_valid = false;
		bool m_turn_left = false;

		tick_t m_plan[ k_max_ticks ];
		bool m_active = false;
		int m_active_tick = 0;
		int m_hit     = 0;
		int m_last    = 0;
		float m_plan_ground_z = 0.f;
		c_vector m_land_pos{ };
		c_vector m_viz[ k_max_ticks + 1 ];

		n_tick::c_sim_budget m_budget;
		c_angle m_view{ };
		int m_base_buttons = 0;
		int m_tick_base0   = 0;
		int m_max_ticks    = 0;
		float m_course_yaw = 0.f;
		float m_max_bend   = 180.f;
		int m_bends        = 0;
		int m_sims         = 0;
		int m_bugs = 0, m_chains = 0, m_cands = 0, m_twins = 0;
		int m_snaps = 0;
		int m_found_bug = -1;
	};
}

inline n_edge_skip::impl_t g_edge_skip{ };

inline int g_eb_sound_tick = 0;
inline int g_es_sound_tick = 0;
inline int g_es_fired = 0;
