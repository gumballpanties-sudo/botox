#pragma once
#define WINDOWS_IGNORE_PACKING_MISMATCH
#include <algorithm>
#include <atomic>
#include <vector>
#include <Windows.h>
#include <string>
#include <unordered_set>
#include "../../game/sdk/classes/c_vector.h"
#include "../../game/sdk/classes/c_angle.h"
class c_user_cmd;
class c_base_entity;
enum class route_marker_type_t {
	floor,
	ceiling,
	bounce,
	pixelsurf,
	headbang
};
struct jump_marker_t {
	c_vector position;
	std::string map;
	route_marker_type_t type = route_marker_type_t::floor;
	float scale = 0.f;
	bool active = true;
	bool needs_duck = false;
	bool needs_jump = true;
	float required_speed = 0.f;
	jump_marker_t() = default;
	jump_marker_t( const c_vector& pos, const std::string& map_name, route_marker_type_t marker_type )
		: position( pos ), map( map_name ), type( marker_type ), active( true ) {}
};
struct route_studio_t {
	std::vector< jump_marker_t > markers;
	std::string current_route_name = "default";
	bool scanning = false;
	bool dirty = false;
};
struct points_check_t {
	c_vector pos;
	std::string map;
	float currentScale     = 0.f;
	bool open_settings     = false;
	bool jump              = true;
	bool minijump          = true;
	bool longjump          = true;
	bool jumpbug           = true;
	bool crouch_hop        = true;
	bool mini_crouch_hop   = true;
	bool c_jump            = true;
	bool c_minijump        = true;
	bool c_longjump        = true;
	bool c_jumpbug         = true;
	bool c_crouch_hop      = true;
	bool c_mini_crouch_hop = true;
	bool high_crouch_jump  = true;
	bool c_high_crouch_jump = true;
	bool active            = true;
	float radius           = 300.f;
	float delta_strafe     = 0.0f;
	int selected_preset    = 0;
	bool advanced_enabled  = false;
	int selected_advanced  = 0;
	float first_jump_radius = 250.f;
	bool use_custom_first_jump_radius = false;
	bool enable_restricted_binds = false;
	int selected_restricted_bind = 0;
	float restricted_binds_radius = 200.f;
	bool is_free_point = false;
	int point_type = 0;
	bool show_3d_radius = false;
	c_vector surf_dir{ };
	int surf_dir_state = 0;
	points_check_t( const c_vector& Pos = c_vector(), const std::string& Map = "" ) : pos( Pos ), map( Map ), is_free_point( false ), point_type( 0 ) { }
};

struct AnimatedPoint {
	c_vector position;
	float animation_progress = 0.f;
	bool is_appearing        = true;
	bool is_removing         = false;
	float current_size       = 0.f;
	c_vector_2d pick_offset{ };
};

bool check( float a, float b );

/* route calculator: ordered world markers. leave point i with one jump, arrive at i + 1.
   z only: the ground between markers is the run-up, horizontal distance is never checked. */
enum route_point_type_t : int {
	route_pt_ground = 0,
	route_pt_pixelsurf,
	route_pt_edgebug,
	route_pt_headbang,
	route_pt_texturebug,
	route_pt_pixeljump,
	route_pt_headbounce,
	route_pt_count,
};
const char* route_point_type_name( int type );

namespace n_route
{
	enum style_t : int {
		style_jump = 0,
		style_minijump,
		style_longjump,
		style_jumpbug,
		style_walk_off,
		style_count,
	};
	inline constexpr unsigned int k_all_styles = ( 1u << style_count ) - 1u;
	extern const char* const k_style_names[ style_count ];

	enum timing_t : int {
		timing_rest = 0,
		timing_bhop,
		timing_delay,
		timing_count,
	};

	enum move_t : int {
		move_jump = 0,
		move_minijump,
		move_longjump,
		move_hop,
		move_mj_hop,
		move_lj_hop,
		move_crouch_hop,
		move_delay_hop,
		move_jumpbug,
		move_walk_off,
		move_count,
	};
	inline constexpr unsigned int k_all_moves = ( 1u << move_count ) - 1u;
	extern const char* const k_move_names[ move_count ];
	/* the one place a press is turned into a name, so the bar and the global list can never drift */
	int move_of( int style, int timing, bool ducked_in );
}

struct route_point_t {
	c_vector pos{ };
	/* origin z the feet must reach, re-derived each solve: pixel = pixel_calc quantise
	   (trunc + 0.03125), floor = marker z. */
	float quantized_z = 0.f;
	c_vector wall_normal{ };
	bool snapped = false;
	int snap_type = -1;
	bool enabled = true;
	int type     = route_pt_ground;
	unsigned int styles = n_route::k_all_styles;
	float measured_z   = 0.f;
	bool has_measured  = false;
	bool allow_stand = true;
	bool allow_duck  = true;
	int ent = 0;
	c_vector ent_offset{ };
	bool pj_skin = false;
	bool hb_ceiling = false;
	float roof = 3.4e38f;
	/* head seam tb: solved as ducked pixelsurf at pos, shown as texturebug at the finder dot */
	bool head_tb = false;
	float mark_z = 0.f;
};

inline c_vector route_point_mark( const route_point_t& p ) { return p.head_tb ? c_vector( p.pos.m_x, p.pos.m_y, p.mark_z ) : p.pos; }
inline int route_point_shown_type( const route_point_t& p )
{
	return p.head_tb && p.type == route_pt_pixelsurf ? route_pt_texturebug : p.type;
}

namespace n_route
{
	constexpr float k_creep_gap = 0.0005f;
	constexpr float k_route_travel = 250.f;
	constexpr float k_flat_nz = 0.999f;
	constexpr float k_skin_lo   = 2.f - 0.03125f + 0.98f * k_creep_gap;
	constexpr float k_skin_hi   = 2.f - 0.03125f + 0.0312460f;

	struct sim_t {
		float z           = 0.f;
		float vz          = 0.f;
		float stamina     = 0.f;
		float fall        = 0.f;
		float land_cost   = 0.f;
		float duck_amount = 0.f;
		bool ducked       = false;
		bool ducking      = false;
		bool on_ground    = true;
		float duck_speed     = 8.f;
		float last_duck_time = -1000.f;
		float full_duck_time = 0.f;
		bool fl_ducking      = false;
		bool raw_duck        = false;
		float time           = 0.f;
		bool on_player       = false;
		float ceiling        = 3.4e38f;
		float roof           = 3.4e38f;
		bool roof_hit        = false;
	};

	struct solution_t {
		std::vector< std::string > elements{ };
		int cost = 0;
		float gap = 0.f;
	};

	bool measure_launch( c_user_cmd* cmd );
	int ground_player( );

	struct solve_input_t {
		std::vector< route_point_t > points{ };
		unsigned int global_moves = k_all_moves;
		unsigned int start_styles = k_all_styles;
		float start_z             = 0.f;
		bool start_on_ground      = true;
		bool start_on_player      = false;
		float start_vz            = 0.f;
		float start_stamina       = 0.f;
		bool start_ducked         = false;
		float start_duck_amount   = 0.f;
		float start_duck_speed    = 8.f;
		float travel              = k_route_travel;
		int delay_ticks           = 2;
		int max_results           = 16;
		/* route calc worker: progress 0..999 out, stop flag in ( null = sync caller ) */
		std::atomic< int >* progress      = nullptr;
		const std::atomic< bool >* cancel = nullptr;
	};
	struct solve_output_t {
		std::vector< solution_t > routes{ };
		long long total = 0;
		/* jumps = route length: one move per point, never more */
		int jumps        = 0;
		/* per route length (console): best gap at the destination + hit count. neither set = that
		   length was never tried, a different fault. */
		float reach_gap[ 16 ]{ };
		int reach_hits[ 16 ]{ };
		long double space    = 0.L;
		long double searched = 0.L;
		/* the search outgrew the address space at failed_at and stopped ( no beam, every arc is kept ) */
		bool out_of_memory   = false;
		int failed_at   = -1;
		float want_z    = 0.f;
		float closest_z = 0.f;
		float best_gap  = 3.4e38f;
		unsigned int blocked_moves = 0u;
		unsigned int refused_moves = 0u;
		std::vector< std::string > miss_lines{ };
		std::vector< int > arcs_after{ };
	};
	void solve( const solve_input_t& in, solve_output_t& out );
	/* route calc solve running on its worker: anything else touching the engine numbers waits */
	bool solve_busy( );
	void route_calc_shutdown( );
	/* budget_ms < 0 = no limit ( route calc worker ); pixel calc runs on the game thread and passes its own */
	std::string solve_hint( const solve_input_t& in, unsigned int refused, std::string& example, int budget_ms = -1 );
	std::string move_list( unsigned int mask, const char* join );
	/* distance calculator: longest block (kz distance, edge to edge) a longjump / jumpbug clears onto a floor dz over
	   the takeoff, perfect strafes. max_speed = m_flMaxspeed (takeoff = 1.1x). -1 = never gets that high */
	float max_block( bool jumpbug, float dz, float max_speed );

	void dump_jump_matrix( );

	float target_z( const route_point_t& point );
	int lip_kind( const c_vector& at, float pz, float& gap_out );
	float floor_nz( );
	std::string route_line( const solution_t& s );
	std::string route_text( const std::vector< std::string >& elements );

	enum popup_row_kind_t : int { popup_row_combo = 0, popup_row_head, popup_row_note, popup_row_error };
	struct popup_row_t {
		std::string text;
		int kind = popup_row_combo;
	};
	void draw_popup( const char* title, const std::vector< popup_row_t >& rows, float started );
}

enum edgebug_type_t : int {
	eb_standing = 0,
	eb_ducking,
};
namespace n_movement
{

	struct impl_t {
		struct edgebug_data_t {
			edgebug_type_t m_edgebug_method{ };
			bool m_will_edgebug{ };
			bool m_will_fail{ };
			bool m_strafing{ };
			float m_yaw_delta{ };
			float m_starting_yaw{ };
			float m_side_move{ };
			float m_forward_move{ };
			float m_saved_mousedx{ };
			int m_ticks_to_stop{ };
			int m_last_tick{ };
			void reset( );
		} m_edgebug_data;
		struct jumpbug_data_t {
			int m_height_diff                    = 0.f;
			float m_vertical_velocity_at_landing = 0.f;
			float m_abs_height_diff              = 0.f;
			int m_ticks_till_land                = 0;
			bool m_can_jb                        = false;
		} m_jumpbug_data;
		struct kangaroo_data_t {
			bool m_successful = false;
		} m_kangaroo_data;
		struct trajectory_segment_t {
			std::vector<c_vector> points;
			std::string map_name;
		};

		struct pixelsurf_data_t {
			bool m_predicted_succesful = false, m_in_pixel_surf = false, m_should_duck = false;
			bool m_pin_detected = false;
			int m_pin_kind = 0;
			bool m_start_pinned   = false;
			int m_start_pin_kind  = 0;
			float m_sent_face_dz  = -1e9f;
			int m_prediction_ticks      = 0;
			c_user_cmd* m_simulated_cmd = { };
			bool should_pixel_surf      = false;
			bool should_unduck          = false;
			bool m_ps_hold              = false;

			std::vector<trajectory_segment_t> m_trajectory_segments;
			bool m_was_in_surf = false;
			c_vector m_last_position;
			bool m_delete_mode = false;

			void reset( );
		} m_pixelsurf_data;
		struct fireman_data_t {
			bool is_ladder = false;
			bool awall     = false;
			bool fr_hit    = false;
			/* authored fwd/side and the cmd view this tick, movement_fix must not rotate it again */
			bool owns_cmd = false;
			bool was_ladder = false;
			bool engaged    = false;
			bool fell_ready = false;
			bool yaw_valid   = false;
			float ladder_yaw = 0.f;
			int launch_ticks = 0;
			bool launch_flung = false;
			bool launch_jumped = false;
			bool launch_pressed = false;
			int launch_refused = 0;
			int ride_ticks = 0;
			float ride_last_z = 0.f;
			int ride_stall    = 0;
			bool ride_released = false;
			bool ride_descend = false;
			int ride_desc_ticks  = 0;
			int ride_wait_ticks  = 0;
			int ride_hover_ticks = 0;
			/* late catch push-off: 0 none, 1 pushed off the floor (+200 * normal), 2 jump pressed (never push again) */
			int ride_push = 0;
			bool ride_floor_press = false;
			c_vector ride_point{ };
			int brake_ticks = 0;
			bool drop_in_lock = false;
			int lock_lost_ticks = 0;
			int lock_ticks      = 0;
			bool drop_in_fall = false;
			bool ladder_lock      = false;
			int ladder_lost_ticks = 0;
			/* a lock must earn its tick: idle = ticks keys were eaten with no approach written; past the
			   limit keys go back. cooldown stops re-arming the same refusal next tick. */
			int lock_idle_ticks = 0;
			int lock_cooldown   = 0;
			float last_ground_z = 0.f;
			bool ground_z_valid = false;
			int grab_tick = -1000;
			/* log only: last latch press, checked next tick ( [fr] nolatch ) */
			int press_tick       = -1000;
			const char* press_by = "";
			float press_yaw      = 0.f;
			c_vector press_origin{ };
			bool key_held      = false;
			bool key_on_ladder = false;
			bool backing_out = false;
			int steer_ticks = 0;
			bool recall_valid  = false;
			bool recall_flat   = false;
			int recall_tick    = -1000;
			c_vector recall_point{ };
			c_vector recall_normal{ };
			float recall_nz    = 0.f;
			bool left_valid       = false;
			c_vector left_point{ };
			c_vector left_normal{ };
			int left_ground_ticks = 0;
			bool player_trick = false;
			bool side_valid  = false;
			int side_ground_ticks = 0;
			float side_half  = 0.f;
			float side_back  = 0.f;
			bool side_press  = false;
			float side_ov    = 0.f;
			c_vector side_point{ };
			c_vector side_normal{ };
			c_vector side_u{ };
			bool fall_seen       = false;
			bool fall_side       = false;
			int fall_win_ticks   = 0;
			float fall_seen_drop = 0.f;
			float fall_min_gap   = 0.f;
			float fall_last_gap  = 0.f;
			float fall_last_dist = 0.f;
			float fall_last_h    = 0.f;
			float fall_last_ov   = 0.f;
			bool drop_col_valid  = false;
			c_vector drop_col{ };
			struct fall_hist_t {
				float drop = 0.f, gap = 0.f, dist = 0.f, h = 0.f, ov = 0.f, err = -1.f, need = -1.f, along = 0.f, in = 0.f, spin = 0.f;
				int wrote = 0;
			} fall_hist[ 24 ]{ };
			int fall_hist_n = 0;
			/* log only: when the guide started vs takeoff / key press, how far the view spun (360s) */
			int air_ticks    = 0;
			int key_ticks    = 0;
			float air_turned = 0.f;
		} m_fireman_data;
		struct pixelsurf_assist_t {
			bool in_crosshair = false;
			bool set_point    = false;
			bool pending_announce = false;
			std::string pending_info;
			std::string pending_stance;
		} m_pixelsurf_assist_t;
		bool m_lirili_larila_pending_render = false;
		c_vector m_lirili_larila_pending_origin{ };
		struct headbang_data_t {
			bool m_is_active = false;
			bool m_scanning = false;
			c_vector m_wall_normal{ };
			c_vector m_edge_position{ };
			c_vector m_bounce_position{ };
			float m_ceiling_z = 0.f;
			float m_ground_z = 0.f;
		} m_headbang_data;
		route_studio_t m_route_studio;
		struct ladder_bug_t {
			bool founded = false;
		} m_ladder_bug_data;
		struct texture_debug_ray_t {
			c_vector start{ };
			c_vector end{ };
			bool hit = false;
			bool is_primary = false;
			float z_frac = 0.f;
		};
		struct air_stuck_t {
			bool m_air_stuck          = false;
			/* written by the texture bug copy exactly. no
			   consumers; don't delete, the copy would need editing and break the exact-copy rule. */
			float m_antoha_debug_fwd  = 0.f;
			float m_antoha_debug_side = 0.f;
			float m_antoha_debug_dist = 0.f;
			bool m_texture_bug_detect = false;
			bool m_texture_bug_owns_cmd = false;
			bool m_texture_tb_holding = false;
			c_vector m_texture_tb_locked_vel{ };
			c_vector m_texture_tb_wall_normal{ };
			std::vector< texture_debug_ray_t > m_texture_debug_rays{ };
			float m_texture_debug_fwd         = 0.f;
			float m_texture_debug_side        = 0.f;
			float m_texture_debug_dist        = 0.f;
			bool m_wall_found                 = false;
			bool m_aligned                    = false;
			bool m_predicted                  = false;
			bool m_used_fallback              = false;
			bool m_duck                       = false;
			bool m_has_pixel_jump_target      = false;
			bool m_point_descend_active       = false;
			float m_align                     = 0.f;
			float m_adjust                    = 0.f;
			float m_best_yaw                  = 0.f;
			float m_best_forward              = 0.f;
			float m_pixel_jump_gap            = 0.f;
			int m_auto_align_block_until_tick = -1;
			c_vector m_wall_normal{ };
			c_vector m_predicted_velocity{ };
			c_vector m_pixel_jump_target{ };

			void reset( )
			{
				m_air_stuck          = false;
				m_antoha_debug_fwd   = 0.f;
				m_antoha_debug_side  = 0.f;
				m_antoha_debug_dist  = 0.f;
				m_texture_bug_detect   = false;
				m_texture_bug_owns_cmd = false;
				m_texture_tb_holding          = false;
				m_texture_tb_locked_vel       = c_vector{ };
				m_texture_tb_wall_normal      = c_vector{ };
				m_texture_debug_fwd           = 0.f;
				m_texture_debug_side          = 0.f;
				m_texture_debug_dist          = 0.f;
				m_wall_found                  = false;
				m_aligned                     = false;
				m_predicted                   = false;
				m_used_fallback               = false;
				m_duck                        = false;
				m_has_pixel_jump_target       = false;
				m_point_descend_active        = false;
				m_align                       = 0.f;
				m_adjust                      = 0.f;
				m_best_yaw                    = 0.f;
				m_best_forward                = 0.f;
				m_pixel_jump_gap              = 0.f;
				m_auto_align_block_until_tick = -1;
				m_wall_normal                 = c_vector( );
				m_predicted_velocity          = c_vector( );
				m_pixel_jump_target           = c_vector( );
			}
		} m_air_stuck_data;

		struct edge_jump_data_t {
			bool m_detected = false;
			bool m_ladder_detected = false;
		} m_edge_jump_data;
		struct long_jump_data_t {
			bool m_detected = false;
			bool m_ducking = false;
			int m_adaptive_key   = 0;
			bool m_adaptive_armed = false;
			int m_adaptive_tick  = 0;
		} m_long_jump_data;
		struct mini_jump_data_t {
			bool m_detected = false;
		} m_mini_jump_data;
		struct auto_one_hop_data_t {
			static constexpr int phase_idle = 0;
			static constexpr int phase_climbing = 1;
			static constexpr int phase_jumped = 2;
			static constexpr int phase_strafing = 3;
			int m_phase = phase_idle;
			c_vector m_ladder_normal{};
			c_angle m_saved_view{};
			int m_air_ticks = 0;
			void reset( ) {
				m_phase = phase_idle;
				m_ladder_normal = c_vector( 0, 0, 0 );
				m_saved_view = c_angle( 0, 0, 0 );
				m_air_ticks = 0;
			}
		} m_auto_one_hop_data;
		struct ladder_freelook_climb_data_t {
			static constexpr int phase_idle = 0;
			static constexpr int phase_climbing = 1;
			static constexpr int phase_jumping = 2;
			static constexpr int phase_strafing = 3;
			int m_phase = phase_idle;
			c_vector m_ladder_normal{ };
			float m_server_yaw = 0.0f;
			float m_exit_yaw = 0.0f;
			int m_air_ticks = 0;
			bool m_keep_strafing = false;
			void reset() {
				m_phase = phase_idle;
				m_ladder_normal = c_vector( 0, 0, 0 );
				m_server_yaw = 0.0f;
				m_exit_yaw = 0.0f;
				m_air_ticks = 0;
				m_keep_strafing = false;
			}
		} m_ladder_freelook_climb_data;

		struct auto_strafe_onladder_data_t {
			bool m_was_on_ladder = false;
			bool m_active = false;
			float m_start_time = 0.0f;
			const float m_duration = 5.0f;
			void reset() {
				m_was_on_ladder = false;
				m_active = false;
				m_start_time = 0.0f;
			}
		} m_autostrafe_onladder_data;

		struct auto_one_hop_onladder_data_t {
			bool m_was_on_ladder = false;
			bool m_active = false;
			float m_start_time = 0.0f;
			const float m_duration = 5.0f;
			float m_current_rotation = 0.0f;
			float m_initial_yaw = 0.0f;
			bool m_is_rotating = false;
			bool m_has_jumped_off = false;
			bool m_strafing = false;
			void reset() {
				m_was_on_ladder = false;
				m_active = false;
				m_start_time = 0.0f;
				m_current_rotation = 0.0f;
				m_initial_yaw = 0.0f;
				m_is_rotating = false;
				m_has_jumped_off = false;
				m_strafing = false;
			}
		} m_auto_one_hop_onladder_data;

		struct auto_crouch_data_t {
			float m_ducking_velo = 0.f;
			float m_standing_velo = 0.f;
			float m_ducking_origin = 0.f;
			float m_standing_origin = 0.f;
			bool m_founded = false;
		} m_auto_crouch_data;

		struct auto_bounce_data_t {
			bool m_owns_cmd = false;
			bool m_found    = false;
			bool m_keep     = false;
			int m_armed_tick = 0;
			float m_entry_speed = 0.f;
			float m_gain   = 0.f;
			float m_angle  = 0.f;
			float m_result = 0.f;
			c_vector m_target{ };
			c_vector m_normal{ };
			int m_last_cmd     = 0;
			int m_clip_cmd     = 0;
			float m_last_vz    = 0.f;
			float m_last_speed = 0.f;
			bool m_jump_owed = false;
			int m_refused    = 0;
			static constexpr int k_viz_max = 300;
			c_vector m_viz[ k_viz_max ]{ };
			int m_viz_n = 0;
			c_vector m_viz_land{ };
			c_vector m_viz_normal{ };

			void disarm( )
			{
				m_viz_n       = 0;
				m_owns_cmd    = false;
				m_found       = false;
				m_keep        = false;
				m_armed_tick  = 0;
				m_entry_speed = 0.f;
				m_gain        = 0.f;
				m_angle       = 0.f;
				m_result      = 0.f;
				m_target      = c_vector( );
				m_normal      = c_vector( );
				m_refused     = 0;
			}

			void reset( )
			{
				disarm( );
				m_last_cmd   = 0;
				m_clip_cmd   = 0;
				m_last_vz    = 0.f;
				m_last_speed = 0.f;
				m_jump_owed  = false;
			}
		} m_auto_bounce_data;

		struct texture_bug_data_t {
			bool m_hit = false;
			bool m_setup = false;
			bool m_debug_box_valid = false;
			float m_founded_forward = 0.f;
			float m_founded_side = 0.f;
			c_angle m_founded_view{ 0, 0, 0 };
			c_vector m_wall_normal{ };
			c_vector m_debug_box_origin{ };
			c_vector m_debug_box_mins{ };
			c_vector m_debug_box_maxs{ };
			c_angle m_debug_box_angles{ 0.f, 0.f, 0.f };
			float m_brush_height = 0.f;
			float m_v_perp = 0.f;
			float m_ratio = 0.f;
			float m_required_ratio = 0.f;
			void reset() {
				m_hit = false; m_setup = false; m_debug_box_valid = false;
				m_founded_forward = 0.f; m_founded_side = 0.f;
				m_founded_view = c_angle( 0, 0, 0 ); m_wall_normal = c_vector( 0.f, 0.f, 0.f );
				m_debug_box_origin = c_vector( 0.f, 0.f, 0.f ); m_debug_box_mins = c_vector( 0.f, 0.f, 0.f );
				m_debug_box_maxs = c_vector( 0.f, 0.f, 0.f ); m_debug_box_angles = c_angle( 0.f, 0.f, 0.f );
				m_brush_height = 0.f; m_v_perp = 0.f;
				m_ratio = 0.f; m_required_ratio = 0.f;
			}
		} m_texture_bug_data;

		struct pixel_calc_t {
			c_vector point_vec{};
			c_vector normal_pos_vec{};
			std::vector< n_route::popup_row_t > rows{};
			float popup_started = 0.f;
			/* snapped onto a finder dot: don't re-quantise ( same rule as route_point_t::snapped ) */
			bool snapped       = false;
			std::string map{};
			void clear( )
			{
				point_vec      = c_vector( 0.f, 0.f, 0.f );
				normal_pos_vec = c_vector( 0.f, 0.f, 0.f );
				rows.clear( );
				popup_started = 0.f;
				snapped       = false;
				map.clear( );
			}
		} m_pixel_calc_data;

		struct route_calc_t {
			std::vector< route_point_t > points{ };
			std::vector< n_route::solution_t > solutions{ };
			long long total_solutions = 0;
			/* GetTickCount64 seconds of the calculate press, 0 = no popup. written on create_move, read on
			   the d3d thread; float write is atomic enough ( worst case one frame of stale alpha ). */
			float popup_started       = 0.f;
			bool solved               = false;
			std::string message{ };
			std::string note{ };
			int selected_row = 0;
			std::string map{ };
			/* bumped on every edit: a worker result for an older gen is dropped */
			int gen = 0;
			void clear( )
			{
				points.clear( );
				drop_solutions( );
				selected_row = 0;
				map.clear( );
			}
			void drop_solutions( )
			{
				++gen;
				solutions.clear( );
				total_solutions = 0;
				popup_started   = 0.f;
				solved          = false;
				message.clear( );
				note.clear( );
			}
		} m_route_calc_data;

		struct dist_calc_t {
			c_vector a{ }, b{ }, a_aim{ };
			int count      = 0;
			float block    = 0.f, dz = 0.f;
			float lj_max   = -1.f, jb_max = -1.f;
			std::string map{ };
		} m_dist_calc_data;
		void dist_calc( c_user_cmd* cmd );
		void dist_calc_render( );

		void on_create_move_pre( );
		void on_create_move_post( );
		void auto_align_sent( const c_user_cmd* cmd );
		void on_paint_traverse( );
		void on_frame_stage_notify( int stage );
		void on_end_scene( );
		void rotate_movement( c_user_cmd* cmd, c_angle& angle );
		void pixel_finder( c_user_cmd* cmd );
		void on_level_init( );
		void pixelsurf_assist_ground_help( c_user_cmd* cmd );

		void auto_crouch( c_user_cmd* cmd );
		void pixelsurf_assist_render( );
		float point_to_line_distance( const c_vector_2d& point, const c_vector_2d& line_start, const c_vector_2d& line_end );
		void pixel_calc( c_user_cmd* cmd );
		void pixel_calc_render( );
		void pixel_calc_ui( );
		void route_calc( c_user_cmd* cmd );
		void route_pj_creep( c_user_cmd* cmd );
		void route_calc_render( );
		/* key legend + result bar, on_end_scene only. ImGui::Begin from paint_traverse crashes. */
		void route_calc_ui( );
		bool get_closest_wall_normal( c_vector& out_normal );

		void load_pixelsurf_points( );
		void save_pixelsurf_points( );

		float m_user_forward_move = 0.f;
		float m_user_side_move    = 0.f;
		int m_user_buttons        = 0;

		float m_user_forward_move_raw = 0.f;
		float m_user_side_move_raw    = 0.f;
		int m_user_buttons_raw        = 0;

		struct trick_states_t {
			bool m_edge_bug_queued  = false;
			int m_edge_bug_tick     = 0;
			bool m_texture_bug_now  = false;
			bool m_air_stuck_now    = false;
			bool m_wall_climb_now   = false;
			bool m_pixel_surf_now   = false;
			bool m_texture_bug_surf = false;
		};

		[[nodiscard]] trick_states_t get_trick_states( );

		void strafe_optimizer_mouse( float* x );
		void auto_strafe_mouse( float* x, float* y );

		void apply_nulls( );
		void half_sideways( );
		void tung_surf( c_user_cmd* cmd );

	private:
		void run_detections( );
		void jump_stats( );

		void bunny_hop( );
		void delay_hop( c_user_cmd* cmd );
		void edge_jump( );
		void air_stuck( c_user_cmd* cmd );
		void air_stuck_serverside( c_user_cmd* cmd );
		void long_jump( );
		void adaptive_key_cancel( );
		void mini_jump( );
		void jump_bug( );
		void kangaroo( );
		void jump_bug_crouch( c_user_cmd* cmd );
		void pixel_surf_fix( );
		void pixel_surf( float target_ps_velocity );
		void detect_edgebug( c_user_cmd* cmd );
		void auto_align( c_user_cmd* cmd );
		void AutoStrafe( c_user_cmd* cmd );
		void strafe_optimizer( c_user_cmd* cmd );
		void strafe_to_yaw( c_user_cmd* cmd, c_angle& angle, const float yaw );
		void movement_fix( const c_angle& old_view_point );
		void autobounce_assist( c_user_cmd* cmd );
		void auto_bounce( c_user_cmd* cmd, bool yield = false );
		void auto_bounce_render( );
		void pixelsurf_assist( c_user_cmd* cmd );
		void fire_man( c_user_cmd* cmd );
		void ladder_bug( c_user_cmd* cmd );
		void fast_ladder( c_user_cmd* cmd );
		void ladder_glide( c_user_cmd* cmd );
		void anti_ladder( c_user_cmd* cmd );
		void auto_one_hop( c_user_cmd* cmd );
		void blockbot( c_user_cmd* cmd );
		void fast_stop( c_user_cmd* cmd );
		void ladder_freelook_climb( c_user_cmd* cmd );
		bool is_bind_restricted_in_radius( const points_check_t& point, const c_vector& player_pos, const std::string& bind_type );
	};
}
inline n_movement::impl_t g_movement{ };
extern std::vector< points_check_t > m_bounce_points_check;
extern std::vector< points_check_t > m_points_check;
extern std::vector< AnimatedPoint > animated_points;
extern std::vector< AnimatedPoint > animated_points2;
extern std::vector< AnimatedPoint > animated_points3;
extern std::vector< AnimatedPoint > animated_points4;
bool pf_dot_screen( const AnimatedPoint& ap, c_vector_2d& out );
bool pf_tb_is_head( const c_vector& dot );
void pf_tb_rise_seam( c_vector& dot );
bool pf_hb_is_ceiling( const c_vector& dot );
bool pf_hb_window( const c_vector& dot, float& lower, float& upper );
float pf_head_over( );
void pf_check_catch( const c_vector& pos, const c_vector& wall_n, float slid, bool ducked );
void pf_check_trick( const char* kind, const c_vector& pos, bool ducked );
void movement_add_window( int type, const std::string& info );
c_vector movement_nearest_edge( const c_vector& p, float reach, const c_vector& facing = { }, c_vector* normal_out = nullptr );
