#pragma once
#include "../../game/sdk/structs/matrix_t.h"
#include <array>
#include <deque>
#include <optional>

class c_base_entity;
class c_net_channel;
class c_user_cmd;

namespace n_lagcomp
{
	enum class e_shot : int { none, record, live };

	struct impl_t {
		// fixed buffer size; g_ctx.m_max_allocations (runtime) must never exceed it.
		// = 128 tick at sv_maxunlag 1.0 + 200ms tolerance.
		static constexpr int max_records = 160;

		struct sequence_object_t {
			sequence_object_t( int in_reliable_state, int out_reliable_state, int sequence_nr, float current_time )
				: m_in_reliable_state( in_reliable_state ), m_out_reliable_state( out_reliable_state ), m_sequence_nr( sequence_nr ),
				  m_current_time( current_time ){ };

			int m_in_reliable_state{ };
			int m_out_reliable_state{ };
			int m_sequence_nr{ };
			float m_current_time{ };
		};

		struct record_t {
			int m_player = -1;

			float m_sim_time{ };

			bool m_valid{ };

			c_vector m_vec_origin{ };

			matrix3x4_t m_matrix[ 128 ]{ };
		};

		std::array< int, 65 > m_record_location{ };
		std::array< record_t*, 65 > m_records{ };

		float lerp_time( );

		float max_unlag( );

		float real_latency( );

		bool is_hosting( );

		float extend( );

		float correct_time( );

		float window_center( );

		bool live_accepted( );

		float server_time( );

		void begin_command( c_user_cmd* cmd );

		static constexpr float k_edge_margin = 0.03f;

		static constexpr float k_max_backtrack = 0.2f;

		float time_limit( );

		bool is_valid( float sim_time, float margin = k_edge_margin );

		/* is_valid + no further behind the live sim than time_limit. every shot / draw path uses this */
		bool usable( c_base_entity* entity, float sim_time );

		void commit_shot( record_t* record );

		int push_ticks( );

		e_shot m_shot = e_shot::none;

		void on_frame_stage_notify( );

		record_t* oldest_record( const int ent_index );

		void update_incoming_sequences( c_net_channel* net_channel );
		void clear_incoming_sequences( );
		void add_latency_to_net_channel( c_net_channel* net_channel, float latency );
		void on_create_move_update( c_net_channel* net_channel );

		void on_release( );

	private:
		int m_server_tick         = 0;
		c_user_cmd* m_last_cmd    = nullptr;
		int m_record_push         = 0;

		void refresh_window( );
		int m_window_frame   = -1;
		float m_window_center = 0.f, m_window_unlag = 0.2f, m_window_limit = 0.2f;

		int m_rec_built = 0, m_rec_off = 0;
		float m_rec_report = 0.f;
		c_vector m_rec_last_off{ };

		std::deque< sequence_object_t > m_sequences = { };
		int m_real_incoming_sequence                = 0;
		int m_last_incoming_sequence                = 0;

		/* sequence our reliable state last changed on. diag only; never cap the rewind with it */
		int m_last_reliable_change = 0;

		/* snapshot reported last packet. may stall, must never step back (remote reliable-state check).
		   seq 0 = none yet. */
		sequence_object_t m_last_spoofed{ 0, 0, 0, 0.f };
	};
}

inline n_lagcomp::impl_t g_lagcomp{ };
