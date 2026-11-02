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

		/* visualize, paint traverse: wall face, sent path + catch, ground spot scan */
		void render( );

		bool m_hit     = false;
		int m_act_tick = -1000;

	private:
		void show_wall_slope( );

		bool m_slope_down  = false;
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
		int m_hold_buttons = 0; /* stance the hold plan was proven in */
		int m_free_cmd   = -1; /* cmd wc sent off a sim that ended unpinned */
		bool m_park_off  = false;

		/* ground: one wc hop replayed from each spot along the wall within reach, sliced over ticks, best one walked to */
		struct spot_job_t {
			bool valid = false, rolling = false, done = false;
			c_vector base{ }, n{ };
			float reach = 0.f;
			int count = 0, idx = 0, ticks = 0, sims = 0, slices = 0;
			int score[ 33 ]{ };
		} m_spot{ };

		/* air ( "steer to seam" ): along wall velocity targets replayed from the first air tick, sliced over k_steer_delay ticks in the
		   spot slots ( spot is ground only ), the first that lands where your path does not gets steered to */
		struct steer_job_t {
			bool valid = false, rolling = false, done = false, decided = false, steer = false;
			int start_cmd = 0, cand = -1, count = 0, tick = 0, left = 0, steer_left = 0, sims = 0, base = -1, best = -1;
			float order[ 5 ]{ }, target = 0.f, cur = 0.f;
			c_vector t{ };
			float fwd = 0.f, side = 0.f, yaw = 0.f;
			int buttons = 0;
		} m_steer{ };
		int m_ground_cmd = -1000;

		/* last wall tick, written in create move, read in paint ( same thread ) */
		struct viz_t {
			bool valid = false, ground = false, lands = false;
			const char* branch = "-";
			c_vector wall{ }, n{ }, land{ };
			c_vector path[ 17 ]{ };
			int path_n = 0, sims = 0;
			float half = 16.f, height = 72.f;
		} m_viz{ };
	};
}

inline n_wall_climb::impl_t g_wall_climb{ };
