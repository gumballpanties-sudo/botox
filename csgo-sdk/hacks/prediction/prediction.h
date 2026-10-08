#pragma once

class c_user_cmd;
class c_base_entity;
class c_angle;
#include "../../game/sdk/classes/c_vector.h"
#include "../../globals/macros/macros.h"
#include "../../utilities/modules/modules.h"

namespace n_prediction
{
	struct impl_t {
		struct {
			c_vector m_velocity{ };
			c_vector m_origin{ };
			c_angle m_view_angles{ };
			float* m_fall_velocity{ };
			float m_stamina{ };
			float m_duck_amount{ };
			int m_flags{ };
			int m_move_type{ };
		} backup_data;

		/* real_command = the tick's real command; probes get attack bits stripped + view angles restored. lean = probe skips
		   pre_think / think / post_think ( air movement reads none of it ), collision bounds go stale until the next full sim */
		void begin( c_base_entity* local, c_user_cmd* cmd, bool real_command = false, bool lean = false );
		void end( c_base_entity* local ) const;

		unsigned int m_sim_count{ };
		unsigned int m_restore_count{ };
		float m_bounds_max_speed{ };
		float m_last_max_speed{ };
		float m_real_max_speed{ };
		static constexpr float k_no_face = -1e9f;
		float m_pin_face_dz = k_no_face;
		float m_last_face_dz = k_no_face;
		bool m_in_begin{ };
		void update( );
		void restore_entity_to_predicted_frame( int frame );
		void stamp_sent_stamina( c_user_cmd* cmd );
		int snapshot_slots( ) const;
		bool snapshot_save( int index );
		void snapshot_load( int index );
		bool should_force_128_tick( ) const;
		float get_interval_per_tick( ) const;
		/* pin vz ( -g*dt/2 ) off the ENGINE interval, never the 128 checkbox */
		float get_engine_target_predict_z_velocity( ) const;
		bool is_target_predict_z_velocity( float velocity, float epsilon = 0.001f ) const;

	private:
		void update_button_state( c_base_entity* local, c_user_cmd* cmd );

		unsigned int* m_prediction_random_seed = nullptr;
		c_base_entity** m_prediction_player    = nullptr;

		float m_old_current_time{ };
		float m_old_frame_time{ };
		float m_old_interval_per_tick{ };
		int m_old_tick_count{ };
	};
}

inline n_prediction::impl_t g_prediction{ };