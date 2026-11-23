#pragma once
#include <atomic>
#include <cstdint>
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
		bool has_duck     = false;
		float duck_amount = 0.f;
		short weapon_def  = 0;
		int weapon_slot   = -1;
		c_vector position{ };
		c_vector velocity{ };
		/* live replay: raw = cmd before any movement feature, keys = feature binds down ( vk bitmap ) */
		bool live         = false;
		float raw_pitch   = 0.f;
		float raw_yaw     = 0.f;
		float raw_forward = 0.f;
		float raw_side    = 0.f;
		float raw_up      = 0.f;
		int raw_buttons   = 0;
		std::uint64_t keys[ 4 ]{ };
		/* mouse auto strafe type 2 ate this cmd: never reaches the cmd */
		float strafe_mx = 0.f;
		float strafe_my = 0.f;

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

	/* one finished clipper session: first move ( from standing ) until standing for the stop delay */
	struct buffered_clip_t {
		std::vector< frame_t > frames;
		bool rolled = false; /* hit max length, front cut: frame 0 is moving */
		std::string saved_as;
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

	/* movement feature section of create_move: g_input.check_input inside = a feature bind ( noted / answered by the route ) */
	struct feat_scope_t {
		feat_scope_t( );
		~feat_scope_t( );
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

		std::deque< frame_t > m_clip_session;
		bool m_clip_active      = false;
		bool m_clip_rolled      = false;
		int m_clip_still_ticks  = 0;

		/* ticks actually sent this playback: take over source, desync check */
		std::vector< frame_t > m_play_capture;
		float m_play_max_drift = 0.f;
		int m_play_drift_tick  = -1;

		frame_t m_user_input{ };
		int m_user_move_mask      = 0;
		int m_user_move_mask_prev = 0;

		bool m_playing        = false;
		bool m_wish_to_start  = false;
		size_t m_play_idx     = 0;
		int m_approach_ticks  = 0;
		float m_step_realtime = 0.f;
		float m_smooth_t0     = 0.f;
		c_angle m_smooth_from{ };
		float m_route_render_start_time = 0.f;
		std::vector< frame_t > m_play_frames;
		std::string m_active_route_name;

		/* m_clips is touched by game thread (cmd, paint) AND render thread (menu, editor): hold this */
		std::recursive_mutex m_clips_mutex;
		std::vector< clip_t > m_clips;
		int m_selected_clip = -1;
		std::deque< buffered_clip_t > m_clip_history; /* newest first */
		int m_clip_selected = -1;

		float m_indicator_alpha   = 0.f;
		float m_last_auto_refresh = 0.f;
		std::size_t m_clips_stamp = 0;

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

		/* live replay. atomics: check_input asks from any thread */
		std::atomic< unsigned long > m_feat_thread{ 0ul };
		std::uint64_t m_feat_keys[ 4 ]{ };
		bool m_feat_armed = false;
		std::atomic< int > m_bind_state{ -1 };
		std::atomic< bool > m_bind_force{ false };
		std::atomic< std::uint64_t > m_bind_keys[ 4 ]{ };
		bool m_cmd_live      = false;
		bool m_play_has_live = false;
		int m_play_live      = 0;
		float m_cmd_mouse_x  = 0.f;
		float m_cmd_mouse_y  = 0.f;

		bool owns_cmd( ) const
		{
			return m_playing || m_wish_to_start;
		}

		/* this cmd: -1 no playback, 0 approach / raw frame ( replay owns the cmd ), 1 live frame ( features own the move ) */
		int play_state( ) const
		{
			return m_bind_state.load( );
		}

		bool in_feat_scope( ) const;
		int bind_play( int key, int style ) const;
		void bind_note( int key, int style );
		/* auto strafe type 2's eaten mouse: live frame = the route's, else noted for the frame */
		void strafe_mouse( float& x, float& y );

		/* before anything snapshots the cmd ( old_view_point, input buttons ): a live frame's raw input goes on here */
		void begin_cmd( c_user_cmd* cmd );
		void capture_user_input( c_user_cmd* cmd );
		void apply_playback( c_user_cmd* cmd, bool relay = false ) const;
		void on_create_move( c_user_cmd* cmd );
		void on_frame_stage( int stage );
		void on_paint_traverse( );
		void on_end_scene( );
		void camera_lock( float* x, float* y );
		void force_stop( bool keep_played = false );
		void reset_all( );
		void set_round_frozen( bool frozen );

		std::filesystem::path get_root_path( ) const;
		void ensure_root( );
		void refresh_clips( bool only_if_changed = false );
		void delete_clip( size_t index );
		bool play_clip( size_t index );
		void open_clip_editor( size_t index );
		void save_buffered( size_t index );
		void clear_clip_history( );

		static int server_tickrate( );
		bool playable( const clip_t& clip ) const;

	private:
		static bool select_weapon( c_user_cmd* cmd, const frame_t& frame );
		float yaw_offset( size_t idx ) const;
		c_angle camera_at( size_t idx, float now ) const;
		void playback_view( c_user_cmd* cmd, const c_angle& own ) const;
		void sync_binds( );
		void check_map_change( );
		void apply_replay_jetpack( bool active );
		bool save_frames( const std::vector< frame_t >& frames, const char* kind, std::string* saved_as = nullptr );
		void save_clip( );
		void clip_tick( const frame_t& f, bool can_start, bool still );
		bool finish_clip( );
		void drop_clip_session( );
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
