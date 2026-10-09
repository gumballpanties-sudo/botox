#pragma once

#include "texturebug_wallstrafe.h"

class c_user_cmd;

namespace n_texturebug
{
	struct impl_t {
	public:
		void on_create_move( c_user_cmd* cmd );
		// grounded half: aims run at face, holds jump until arc meets target height
		// texture_bug never sees a grounded tick
		void tb_auto_align( c_user_cmd* cmd );
		void texture_bug( c_user_cmd* cmd );
		void head_bounce( c_user_cmd* cmd );
		void preserve_move( c_user_cmd* cmd );
		void reset_command( ) { m_owned_move.clear( ); m_wallstrafed = false; }

		bool m_hit      = false;
		bool m_hs_hit   = false;
		bool m_hb_hit   = false; // head bounce landed this tick, one tick only, never a ride
		bool m_acted    = false;
		bool m_owns_cmd = false;
		bool m_assist   = false;
		bool m_wallstrafed = false;
		bool m_resimmed      = false;
		float m_sent_face_dz = -1e9f;
		n_tb::wallstrafe::owned_move_t m_owned_move{ };
		float m_input_forward = 0.f, m_input_side = 0.f, m_input_yaw = 0.f;
	};
}

inline n_texturebug::impl_t g_texturebug{ };
