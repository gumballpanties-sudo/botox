#pragma once
#include "../../globals/includes/includes.h"
#include <atomic>
#include <iterator>
#include <string>
#include <vector>

struct IDirect3DTexture9;
struct IDirect3DDevice9;

namespace n_misc
{
	struct spectator_data_t {
		std::string m_text          = { };
		IDirect3DTexture9* m_avatar = { };
		c_color m_color             = { };
		int m_index                 = { };
	};

	/* one row, shared by every style's collect pass. avatar = INDEX, never a texture:
	   on_remove_entity frees it the frame a player leaves. */
	struct spectator_entry_t {
		std::string m_name        = { };
		std::string m_target_name = { };
		std::string m_mode    = { };
		int m_obs_mode        = { };
		int m_index           = { };
		int m_team            = { };
		bool m_fake_player    = { };
		bool m_watching_local = { };
		int m_target_index        = { };
		int m_target_team         = { };
		bool m_target_fake_player = { };
	};

	std::string watermark_label( );
	std::string watermark_brand( );

	struct clantag_frame_t {
		std::string m_text = { };
		float m_hold       = 1.f;
	};
	std::vector< clantag_frame_t > parse_clantag_frames( const std::string& value, float fallback_hold );
	std::string join_clantag_frames( const std::vector< clantag_frame_t >& frames );

	inline ImVec4 g_watermark_box{ };
	inline ImVec4 g_watermark_gif_box{ };

	struct media_look_t {
		bool m_box     = true;
		ImU32 m_title  = IM_COL32( 255, 255, 255, 255 );
		ImU32 m_artist = IM_COL32( 146, 144, 145, 255 );
		int m_fx       = 0;
	};

	struct world_texture_preset_t {
		const char* m_label;
		const char* m_texture;
	};

	inline constexpr world_texture_preset_t world_texture_presets[] = {
		{ "off", nullptr },
		{ "dev", "dev/dev_measuregeneric01b" },
		{ "dev orange", "dev/dev_measuregeneric01" },
		{ "dev wall", "dev/dev_measurewall01a" },
		{ "grid", "dev/graygrid" },
		{ "snow", "ground/snow01" },
		{ "snow field", "dev/snowfield" },
		{ "ice", "ground/ice01" },
		{ "cobblestone", "cobblestone/hr_c/inferno/cobblestone_a" },
		{ "sandstone", "anubis/sandstone01" },
		{ "grass", "grass/hr_grass/grass_a" },
		{ "grass old", "nature/ground_grass01" },
		{ "dirt", "nature/dirtfloor006a" },
		{ "mud", "hr_massive/mud_ground_1" },
		{ "sand", "nature/sandfloor010a" },
		{ "gravel", "ground/hr_g/hr_gravel_001_color" },
		{ "rock", "nature/rockwall007" },
		{ "stone", "stone/hr_stone/hr_stone_a" },
		{ "concrete floor", "concrete/concretefloor008a" },
		{ "concrete wall", "concrete/concretewall004a" },
		{ "brick", "brick/brickwall008a" },
		{ "plaster", "plaster/plasterwall003a" },
		{ "mudbrick", "de_dust/hr_dust/hr_dust_mudbrick_01_color" },
		{ "tile", "tile/hr_t/inferno/tile_a" },
		{ "herringbone", "tile/hr_t/inferno/herringbone_a" },
		{ "roof tile", "tile/hr_t/inferno/roofing_tile_a" },
		{ "marble", "de_mirage/marble/de_mirage_marble_01" },
		{ "asphalt", "asphalt/hr_c/hr_asphalt_001" },
		{ "wood floor", "wood/woodfloor005a" },
		{ "wood wall", "wood/woodwall009a" },
		{ "metal floor", "metal/metalfloor001a" },
		{ "metal wall", "metal/metalwall001a" },
		{ "metal roof", "metal/metalroof005a" },
		{ "carpet", "carpet/carpet03" },
		{ "white", "vgui/white" },
		{ "custom", nullptr },
	};
	inline constexpr int world_texture_preset_count  = static_cast< int >( std::size( world_texture_presets ) );
	inline constexpr int world_texture_preset_custom = world_texture_preset_count - 1;

	// index = m_world_texture_preset / m_world_texture_custom slot (7)
	inline constexpr const char* world_texture_categories[] = { "ground", "floors", "walls", "wood", "metal", "roofs / ceilings", "other" };
	inline constexpr int world_texture_category_count = static_cast< int >( std::size( world_texture_categories ) );

	bool media_player_donor_look( int style, media_look_t& look );
	void media_player_donor_frame( ImDrawList* list, ImVec2 min, ImVec2 max, int style );

	struct impl_t {
		struct practice_t {
			c_angle saved_angles    = { };
			c_vector saved_position = { };		} practice;

		void on_create_move_pre( );

		void on_end_scene( );

		void on_paint_traverse( );

		void on_frame_stage_notify( int stage );

		void on_level_shutdown( );

		void performance( bool restore = false );

		void force_lagpush( );

	private:
		void draw_watermark( );
		void draw_watermark_kamibebra( );
		void draw_watermark_clarity( );
		void draw_watermark_clarity_v2( );
		void draw_watermark_delusional( );
		void draw_watermark_interwebz( );
		void draw_watermark_havoc( );
		void draw_watermark_onetap( );
		void draw_watermark_airflow( );
		void draw_watermark_evolve( );
		void draw_watermark_legendware( );
		void draw_watermark_interium( );
		void draw_watermark_skebob( );
		void draw_watermark_cumidere( );
		void draw_watermark_illusory( );
		void draw_watermark_lumi( );
		void draw_watermark_billware( );
		void draw_watermark_dna( );
		void draw_watermark_dna_clarity( );
		void draw_watermark_cucumber( );
		void draw_watermark_howeweware( );
		void draw_watermark_gif( );
		void draw_media_player( );
		void update_clantag( );
		void practice_window_think( );
		void disable_post_processing( );
		void remove_panorama_blur( );
		void remove_smoke( );
		void remove_flash( );
		void flash_debug( );
		void old_shaders( );
		void world_modulation( );
		void world_texture( );
		void force_maxunlag( );
		void force_host_lagcomp( );
		void fix_offline_ping( );
		void force_crosshair( );

		void draw_spectator_list( );
		void draw_spectating_local( );

		void collect_spectators( std::vector< spectator_entry_t >& out, bool all_players = false, bool keep_hltv = false );

		void draw_spectator_list_delusional( );
		void draw_spectator_list_interwebz( );
		void draw_spectator_list_winxp( );
		void draw_spectator_list_clarity( );

		void draw_spectator_list_chillware( );

		void draw_spectator_list_lobotomy( );
		void draw_spectator_list_kamibebra( );

		void draw_spectator_list_clarity_v2( );

		void draw_spectator_list_eyes( );

		void draw_spectator_list_cumhacck( );

		void draw_spectator_list_airplane( );
		void draw_spectator_list_bhopcheat( );
		void draw_spectator_list_millionware( );
		void draw_spectator_list_aimware( );
		void draw_spectator_list_havoc( );
		void draw_spectator_list_onetap( );
		void draw_spectator_list_airflow( );
		void draw_spectator_list_evolve( );
		void draw_spectator_list_legendware( );
		void draw_spectator_list_interium( );

		void draw_spectator_list_chillware_v2( );

		void draw_spectator_list_skebob( );
		void draw_spectator_list_cumidere( );
		void draw_spectator_list_illusory( );
		void draw_spectator_list_lumi( );
		void draw_spectator_list_lumi_new( );
		void draw_spectator_list_lumi_interwebz( );
		void draw_spectator_list_billware( );
		void draw_spectator_list_inkabanium( );
		void draw_spectator_list_dna( );
		void draw_spectator_list_cucumber( );
		void draw_spectator_list_howeweware( );

	public:
		/* gifs + gdi+ + eyes art, render thread only. textures_only = Reset hook (decoded gifs stay), full = unload */
		void release_spectator_textures( bool textures_only = false );
	};
}

inline n_misc::impl_t g_misc{ };

void on_hit_marker( );
void on_hit_sound( );
void on_healthshot( int trigger );
void on_death_particles( int victim_index, int attacker_index );
void kill_effects_paint( );
void kill_effects_world( const c_vector& origin, const c_angle& angles );
void kill_effects_capture( IDirect3DDevice9* device );
void kill_effects_release_textures( );
void kill_effects_shutdown( );
bool kill_effects_hides_ragdoll( int entity_index );
bool kill_effects_wants_ragdoll_dme( );
void ragdoll_force_hook( );
void ragdoll_force_unhook( );
void RenderHitmarker( );

enum class e_points_trick {
	edge_bug, fireman, jump_bug, head_ceiling,
	pixel_surf, texture_bug, air_stuck, wall_climb,
	pixel_surf_ride, texture_bug_ride, air_stuck_ride, wall_climb_ride
};
void points_on_trick( e_points_trick trick );
inline float g_trick_time = -1.f;
inline int g_trick_fired[ static_cast< int >( e_points_trick::pixel_surf_ride ) ]{ };
void points_draw( );

/* frame = smoothing (main thread), create_move = snap on disable, draw = arrow (end_scene).
   demo_angles rewrites what CPrediction::GetLocalViewAngles hands the demo recorder */
void fake_pov_frame( );
void fake_pov_create_move( );
void fake_pov_draw( );
void fake_pov_demo_angles( c_angle& angles );

/* rewrites the listen server's userinfo table (clients + demos + gotv). frame = paint_traverse
   (main thread = server thread, host_thread_mode 0). randomize = menu button. status = menu line */
void bot_names_frame( );
void bot_names_randomize( );
std::string bot_names_status( );
/* steam name fetch thread alive; waited on before FreeLibrary */
inline std::atomic< bool > g_bot_names_fetching{ false };

/* rich presence thread ("botox" + map), started once at load; alive flag is waited on before unload */
void discord_rpc_start( );
/* main thread, frame_stage_notify start: hands the map to the rpc thread, which never calls the engine itself */
void discord_rpc_frame( );
inline std::atomic< bool > g_discord_rpc_alive{ false };

/* menu image box "browse": file dialog (+ freeimage.host upload) thread. cancel = eject closes dialog + aborts upload */
void image_pick_cancel( );
inline std::atomic< bool > g_image_pick_alive{ false };
/* gif/mascot box url downloads (spectator_list.cpp file_image_frame) in flight */
inline std::atomic< int > g_url_image_downloads{ 0 };

class c_base_entity;
c_base_entity* find_player_resource( );

struct player_list_entry_t {
	int m_user_id = 0;
	bool m_revive = false;
	bool m_ignore = false;
	bool m_only   = false;
};
inline player_list_entry_t g_player_list[ 65 ]{ };
struct player_info_t;
void player_list_stamp( int index, const player_info_t& info );
void player_list_save( int index, const player_info_t& info );
const player_list_entry_t* player_list_get( int index );
bool player_list_aim_allowed( int index );
