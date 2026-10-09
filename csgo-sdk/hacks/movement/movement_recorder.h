#pragma once
#include <atomic>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>
#include "../../game/sdk/classes/c_angle.h"
#include "../../game/sdk/classes/c_vector.h"

class c_user_cmd;
struct ImDrawList;

namespace n_movement_recorder
{
	struct frame_t {
		float view_pitch = 0.f;
		float view_yaw   = 0.f;
		float forward    = 0.f;
		float side       = 0.f;
		float up         = 0.f;
		int buttons      = 0;
		short mouse_dx   = 0;
		short mouse_dy   = 0;
		bool jetpack      = false;
		bool has_velocity = false;
		short weapon_def  = 0;
		int weapon_slot   = -1;
		c_vector position{ };
		c_vector velocity{ };

		frame_t( ) = default;
		frame_t( c_user_cmd* cmd, const c_vector& pos );

		void replay( c_user_cmd* cmd ) const;
	};

	struct clip_t {
		std::string filename;
		std::string display_name;
		std::string map;
		int tickrate = 64;
		std::vector< frame_t > frames;

		std::string label( ) const
		{
			return tickrate == 64 ? display_name : display_name + " (" + std::to_string( tickrate ) + " tick)";
		}
	};

	enum e_action : unsigned {
		action_start_rec  = 1u << 0,
		action_stop_rec   = 1u << 1,
		action_save       = 1u << 2,
		action_start_play = 1u << 3,
		action_stop_play  = 1u << 4,
		action_clear      = 1u << 5,
		action_clip       = 1u << 6,
	};

	struct impl_t {
		/* menu ( render thread ) posts, on_create_move consumes */
		std::atomic< unsigned > m_requests{ 0u };

		void request( e_action action )
		{
			m_requests.fetch_or( action );
		}

		/* game thread only */
		bool m_recording = false;
		std::vector< frame_t > m_recording_frames;
		std::deque< frame_t > m_clip_buffer;

		frame_t m_user_input{ };
		int m_user_move_mask      = 0;
		int m_user_move_mask_prev = 0;

		bool m_playing        = false;
		bool m_wish_to_start  = false;
		size_t m_play_idx     = 0;
		int m_approach_ticks  = 0;
		float m_step_realtime = 0.f;
		float m_route_render_start_time = 0.f;
		std::vector< frame_t > m_play_frames;
		std::string m_active_route_name;

		/* m_clips is touched by game thread (cmd, paint) AND render thread (menu, editor): hold this */
		std::recursive_mutex m_clips_mutex;
		std::vector< clip_t > m_clips;
		int m_selected_clip = -1;

		float m_indicator_alpha   = 0.f;
		float m_last_auto_refresh = 0.f;

		std::string m_current_map;
		bool m_replay_jetpack_active       = false;
		bool m_editor_open                 = false;
		bool m_editor_ignore_enter_release = false;
		bool m_round_frozen                = false;
		int m_editor_clip_index            = -1;
		int m_editor_trim_start            = 0;
		int m_editor_trim_end              = 0;
		char m_editor_name[ 128 ]{ };

		std::vector< float > m_point_progress;

		bool owns_cmd( ) const
		{
			return m_playing || m_wish_to_start;
		}

		void capture_user_input( c_user_cmd* cmd );
		void apply_playback( c_user_cmd* cmd ) const;
		void on_create_move( c_user_cmd* cmd );
		void on_frame_stage( int stage );
		void on_paint_traverse( );
		void on_end_scene( );
		void camera_lock( float* x, float* y );
		void force_stop( bool keep_played = false );
		void set_round_frozen( bool frozen );

		std::filesystem::path get_root_path( ) const;
		void ensure_root( );
		void refresh_clips( );
		void delete_clip( size_t index );
		bool play_clip( size_t index );
		void open_clip_editor( size_t index );

		static int server_tickrate( );
		bool playable( const clip_t& clip ) const;

	private:
		static bool select_weapon( c_user_cmd* cmd, const frame_t& frame );
		void check_map_change( );
		void apply_replay_jetpack( bool active );
		void save_frames( const std::vector< frame_t >& frames, const char* kind );
		void clip_route( );
		void reset_all( );
		void render_clip_editor( );
		void apply_clip_editor( );
		void rename_clip( size_t index, const std::string& new_name );
		void render_route_preview( );
		void render_path( const std::vector< frame_t >& frames, float start_time );
		void render_indicator( );
		void render_clipper_box( );
	};
}

inline n_movement_recorder::impl_t g_movement_recorder{ };
