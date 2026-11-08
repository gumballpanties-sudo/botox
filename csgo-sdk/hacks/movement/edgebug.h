#pragma once
#define WINDOWS_IGNORE_PACKING_MISMATCH
#include <Windows.h>
#include <string>
#include <vector>
#include <deque>
#include <algorithm>
#include "../../game/sdk/classes/c_vector.h"
#include "../../game/sdk/classes/c_angle.h"
class c_user_cmd;
namespace n_edgebug
{
	constexpr int k_predicted_cmd_max = 256;
	struct predicted_cmd_t {
		c_angle viewangle;
		float forwardmove;
		float sidemove;
		int buttons;
		c_vector origin;
	};
	struct edge_context_t {
		float backup_vel_z = 0.f;
		float current_vel_z = 0.f;
		float gravity_tick = 0.f;
		bool was_falling = false;
		bool on_ground = false;
		int mode = 0;
		/* false = no IsNearEdge trace ( route calc worker thread, sim ticks off the live player anyway ) */
		bool trace_edge = true;
	};
	void draw_plan( const c_vector* path, int count, int from, const c_vector& pad_pos, float alpha = 1.f );
	struct impl_t {
	public:
		void PrePredictionEdgeBug( c_user_cmd* cmd );
		void EdgeBugPostPredict( c_user_cmd* cmd );
		// called from on_paint_traverse: only queues render data, ImGui::Begin from paint would crash
		void render( );
		// public verdict for the route calculator, must use these bands + mode or it promises
		// edgebugs the assist won't recognise. pass vel before and after the tick
		bool IsEdgeBugTick( float backup_vel_z, float current_vel_z, bool on_ground, bool trace_edge = true );
		float lock_progress( ) const
		{
			return m_prediction_ticks > 0 ? std::clamp( static_cast< float >( m_replay_hits + 1 ) / static_cast< float >( m_prediction_ticks + 1 ), 0.f, 1.f ) : 1.f;
		}
		void log_lock( bool on );
		void add_mouse_x( float x ) { m_mouse_x += x; }
		// dead / spectating: EdgeBugPostPredict never drains it, or respawn's first cmd steers by the whole wait
		void drop_mouse( )
		{
			m_mouse_x = 0.f;
			m_donor   = { };
			m_hw      = { };
			m_lob     = { };
		}
		/* style 4 lobotomy v2: lobotomyv2-main/hacks/movement/edgebug.cpp 1:1 */
		void lob_pre( c_user_cmd* cmd );
		void lob_post( c_user_cmd* cmd );
		void lob_frame_start( );
		void lob_mouse( float* x );
		/* style 3 hotwheels: hacks/movement/movement.cpp edge_bug / detect_edgebug / fsn / override_mouse_input 1:1 */
		void hw_post( c_user_cmd* cmd );
		void hw_frame_start( );
		void hw_mouse( float* x, float* y );
		void donor_pre( c_user_cmd* cmd );
		void donor_post( c_user_cmd* cmd, int style );
		void donor_frame_start( );
		void donor_mouse_fix( c_user_cmd* cmd );
		bool donor_live( ) const { return m_donor.ticks_left != 0; }
		bool m_found = false;
		int m_found_tick = 0;
		int m_prediction_ticks = 0;
		bool m_ducked = false;
		c_vector m_viz_path[k_predicted_cmd_max + 1];
		int m_viz_count = 0;
		c_vector m_viz_pos{ };
	private:
		struct donor_t {
			int ticks_left = 0, eblength = 0, detecttick = 0, edgebugtick = 0;
			bool crouched = false, strafing = false;
			float forwardmove = 0.f, sidemove = 0.f, yawdelta = 0.f, startingyaw = 0.f;
			short mousedx = 0;
		} m_donor;
		struct hw_t {
			bool will_edgebug = false, will_fail = false, strafing = false, ducking = false;
			float yaw_delta = 0.f, starting_yaw = 0.f, side_move = 0.f, forward_move = 0.f, saved_mousedx = 0.f;
			int ticks_to_stop = 0, last_tick = 0;
		} m_hw;
		/* donor globals + function statics, one strafe state per donor strafe block */
		struct lob_strafe_t {
			float side = 1.f, old_yaw = 0.f;
		};
		struct lob_t {
			c_vector vel_backup{ };
			c_angle view_backup{ }, new_angle{ }, aim{ }, aim_to{ };
			bool found = false, ducked = false, brk = false;
			int ticks = 0, last_type = 0, sec_last_type = 0, hits = 0, eb_tick = 0, pred_ticks = 0, pred_stamp = 0, mouse_offset = 0;
			lob_strafe_t strafe[ 4 ];
			predicted_cmd_t cmds[ 128 ];
		} m_lob;
		bool lob_check( c_user_cmd* cmd );
		void hw_reset( );
		void hw_detect( c_user_cmd* cmd );
		void donor_reset( );
		bool donor_check( c_user_cmd* cmd, bool& brk );
		bool EdgeBugCheck( const edge_context_t& ctx );
		bool IsNearEdge( );
		bool FindEdgeTarget( int reach_tick, int window_ticks );
		void ApplyAutoStrafe( c_user_cmd* cmd, bool duck );
		void ApplyAutoStrafeToEdge( c_user_cmd* cmd, bool duck );
		void CommitStrafe( c_user_cmd* cmd, c_angle wish );
		void ReStorePrediction( );
		c_vector m_backup_velocity{ };
		int m_backup_move_type = 0;
		int m_backup_flags = 0;
		float m_user_forward_move = 0.f;
		float m_user_side_move = 0.f;
		int m_user_buttons = 0;
		predicted_cmd_t m_predicted_cmds[k_predicted_cmd_max];
		predicted_cmd_t m_run_cmds[k_predicted_cmd_max];
		int m_replay_hits = 0;
		static constexpr int k_plan_max = 8;
		struct plan_t {
			predicted_cmd_t cmds[k_predicted_cmd_max];
			int ticks = 0, variant = 0, fam = 0;
			bool duck = false; /* FL_DUCKING on the sim's bug tick */
			float bend = 0.f;
			c_vector end_pos{ };
		};
		plan_t m_plans[k_plan_max];
		int m_plan_count = 0;
		c_vector m_follow_end{ };
		c_vector m_viz_alt[k_plan_max][k_predicted_cmd_max + 1];
		int m_viz_alt_count[k_plan_max]{ };
		c_vector m_viz_alt_pos[k_plan_max]{ };
		int m_viz_alts = 0, m_viz_sel = -1;
		int m_viz_base = 0, m_viz_alt_base = 0;
		// paint thread: what is on screen, eased toward the plan each frame. [k_plan_max] = the followed path.
		// pts[0] sits on absolute tick `base`, so a tick step or a plan swap never slides a path along itself
		struct viz_disp_t {
			c_vector pts[k_predicted_cmd_max + 1];
			int n = 0, base = 0;
			c_vector pad{ };
			float alpha = 0.f;
		};
		viz_disp_t m_disp[k_plan_max + 1];
		float m_mouse_x = 0.f;
		float m_steer_since_plan = 0.f;
		int m_last_search_tick = -1;
		int m_last_contact_tick = -1;
		// strafe state, reset per run_variant: never function statics, or a variant's move depends on
		// which ran before it (unreproducible search, duck sweep can't dedupe)
		float m_strafe_side = 1.f;
		float m_strafe_last_yaw = 0.f;
		// lip the to-edge strafe aims at, world xy, found once per real tick.
		// geometry is static during the sim, so never re-trace inside the sim loop
		c_vector m_edge_target{ };
		bool m_edge_target_valid = false;
		// live velocity yaw at search start, reference for the max strafe angle, never the view:
		// in air only a wish across the path accelerates, so a view-relative cap doesn't cap
		float m_course_yaw = 0.f;
		bool m_course_valid = false;
	};
}
inline n_edgebug::impl_t g_edgebug{ };