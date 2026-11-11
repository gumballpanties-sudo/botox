#pragma once
#include "../../game/sdk/classes/c_vector.h"
#include "../../game/sdk/classes/c_angle.h"
#include "tick_scale.h"
class c_user_cmd;

namespace n_edge_catch
{
	constexpr int k_max_ticks = 64;

	struct impl_t {
		void pre( c_user_cmd* cmd );
		void post( c_user_cmd* cmd );
		void reset( bool forget = false );
		bool active( ) const { return m_active; }
		void render( ) const;

	private:
		struct tick_t {
			bool strafe = false;
			bool duck   = false;
			float yaw   = 0.f;
			float side  = 0.f;
			c_vector origin{ };
		};
		void search( c_user_cmd* cmd );
		bool baseline( c_user_cmd* cmd );
		bool family( c_user_cmd* cmd, bool first_neg );
		bool chain( c_user_cmd* cmd, int prefix, bool neg, bool save, int base );
		bool jb_works( c_user_cmd* cmd, int k, int from, int base );
		void apply( c_user_cmd* cmd, const tick_t& t, const c_angle& view ) const;
		void sim( c_user_cmd* cmd, int index, const tick_t& t, int buttons );
		bool out( ) const;
		bool landed( ) const;
		int strafed( ) const;

		int m_start_flags     = 0;
		int m_start_move_type = 0;
		c_vector m_start_vel{ };
		bool m_eb_latched = false;
		float m_takeoff_z = 0.f;
		bool m_had_ground = false;
		float m_prev_vz   = 0.f;
		float m_prev_yaw  = 0.f;
		bool m_yaw_valid  = false;
		bool m_turn_left  = false;

		tick_t m_plan[ k_max_ticks ];
		tick_t m_seed[ 2 ][ 30 ];
		bool m_jb         = false;
		bool m_jb_plan    = false;
		bool m_duck       = false;
		bool m_active     = false;
		int m_hit         = 0;
		int m_last        = 0;
		int m_miss        = 0;
		int m_kind        = 0;
		float m_ground_z  = 0.f;
		float m_miss_vz   = 0.f;
		c_vector m_land_pos{ };
		c_vector m_viz[ k_max_ticks + 1 ];

		n_tick::c_sim_budget m_budget;
		c_angle m_view{ };
		float m_fwd0       = 0.f;
		float m_side0      = 0.f;
		int m_buttons0     = 0;
		int m_tick_base0   = 0;
		int m_max_ticks    = 0;
		float m_reach      = 8.f;
		int m_sims         = 0;
		int m_chains       = 0;
		int m_saved[ 2 ]{ };
	};
}

inline n_edge_catch::impl_t g_edge_catch{ };
inline int g_ec_fired = 0;
