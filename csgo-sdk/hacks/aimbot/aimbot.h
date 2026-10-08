#pragma once
#include "../lagcomp/lagcomp.h"

class c_base_entity;
class c_cconvar;
class c_angle;
class c_vector;
struct matrix3x4_t;

namespace n_aimbot
{
	struct impl_t {
		void on_create_move_post( );

		void run_backtrack( );

		void run_zeusbug( );

		bool zeusbug_busy( ) const { return m_zb_select != 0; }

		void rcs_track( );

		/* outside the alive gate ("off" edge must go out while dead), called from create_move_proxy */
		void run_nospread( );

		void nospread_pre_move( );

		void nospread_mark_shot( );

		void nospread_frame_stage( int stage );

		void on_level_init( );

		float active_fov( );

		/* g_ctx.m_cmd lets a bullet out ( semi-auto held = no ); non-guns = attack held */
		bool shot_leaves( );

		float m_shot_pitch = 0.f, m_shot_yaw = 0.f;
		bool m_shot_view_valid = false;
		bool m_shot_is_aim     = false;

	private:
		bool m_ns_sent_on    = false;
		float m_ns_last_send = 0.f;

		c_cconvar* m_ns_convar = nullptr;
		bool m_ns_looked_up    = false;
		bool m_ns_held         = false;
		float m_ns_window_end  = 0.f;

		void nospread_apply( bool on );

		int m_zb_select = 0;
		int m_zb_cmd    = 0;
		bool m_zb_back  = false;

		bool can_aimbot( );

		float rcs_strength( bool gated );

		int m_target_hitbox  = 0;
		int m_lock_index     = 0;
		bool m_visible_first = false;

		/* rcs state. m_rcs_shots = our burst counter ( m_iShotsFired resets between taps ). applied_x/y = comp baked into the camera
		   ( visible only ), unwound as punch decays. the aim's own turn is a target move, never comp */
		int m_rcs_shots           = 0;
		int m_rcs_last_game_shots = 0;
		short m_rcs_last_weapon   = 0;
		float m_rcs_applied_x = 0.f, m_rcs_applied_y = 0.f;
		bool m_rcs_comping = false;

		/* punch left over from the previous weapon; don't comp it. cleared once decayed (rcs_track) */
		bool m_rcs_stale_punch = false;

		bool m_rcs_aim_burst = false;
	};
}

inline n_aimbot::impl_t g_aimbot{ };
