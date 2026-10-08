#pragma once
#include "../../../game/sdk/classes/c_vector.h"
#include "../../../game/sdk/structs/game_event_t.h"

#include <array>
#include <deque>

/* bullet tracers + movement trail (engine beams), impact boxes (debug overlay), world hit marker.
   all fed on the main thread: events from fire_event_intern, polling in frame_stage render_start */
namespace n_bullets
{
	/* server only networks bullet_impact to clients that listen for it; valve's client never does.
	   body empty: fire_event_intern already sees every event */
	struct impact_listener_t : c_game_event_listener {
		impact_listener_t( ) { debug_id = 42; }
		void fire_game_event( game_event_t* ) override { }
	};

	struct impl_t {
		void on_bullet_impact( game_event_t* event );
		void on_player_hurt( int victim_index );
		void on_frame_stage_notify( int stage );
		void on_paint_traverse( );
		void on_level_init( );
		void on_release( );

	private:
		void register_listener( );
		void flush_tracers( );
		void client_impacts( );
		void movement_trail( );

		struct impact_t {
			c_vector m_position = { };
			float m_time        = 0.f;
		};

		struct marker_t {
			c_vector m_position = { };
			float m_time        = 0.f;
		};

		struct pending_tracer_t {
			bool m_valid     = false;
			c_vector m_start = { };
			c_vector m_end   = { };
		};

		impact_listener_t m_listener{ };
		bool m_listening = false;

		std::array< pending_tracer_t, 65 > m_tracers{ };
		std::deque< impact_t > m_local_impacts{ };
		std::deque< marker_t > m_markers{ };

		float m_last_client_impact = 0.f;
		bool m_client_list_bad     = false;

		c_vector m_trail_last = { };
		bool m_trail_valid    = false;
	};
}

inline n_bullets::impl_t g_bullets{ };
