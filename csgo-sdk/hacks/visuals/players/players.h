#pragma once
#include "../../../game/sdk/classes/c_vector.h"
#include "../../../game/sdk/structs/bounding_box_t.h"
#include "../../../game/sdk/structs/matrix_t.h"
#include <string>
#include <vector>

class c_base_entity;
class c_weapon_data;
struct draw_object_t;
struct studiohdr_t;
struct IDirect3DTexture9;

enum e_padding_direction {
	padding_direction_left = 0,
	padding_direction_top,
	padding_direction_right,
	padding_direction_bottom,
	padding_direction_max
};

struct damage_number_t {
	c_vector m_origin  = { };
	int m_damage       = 0;
	int m_victim       = 0;
	float m_spawn_time = 0.f;
};

namespace n_players
{
	struct hitbox_capsule_t {
		c_vector m_a   = { };
		c_vector m_b   = { };
		float m_radius = 0.f;
	};

	/* null matrix = game camera ( g_render.world_to_screen ), else world_to_projection mapped onto a width x height box */
	struct projector_t {
		const view_matrix_t* m_matrix = nullptr;
		float m_width = 0.f, m_height = 0.f;

		bool operator( )( const c_vector& world, c_vector_2d& screen ) const;
	};

	/* everything draw_player reads, gathered from the entity ( players( ) ) or made up ( esp preview ) */
	struct esp_player_t {
		float m_alpha = 1.f;
		bounding_box_t m_box{ };

		std::string m_name = { };
		bool m_bot         = false;
		int m_health       = 0;
		float m_health_factor = 0.f; /* animated health / max */
		bool m_visible     = false;
		bool m_god         = false;
		float m_distance   = 0.f;

		c_weapon_data* m_weapon_data = nullptr;
		short m_item_definition_index = 0;
		float m_ammo_factor = 0.f;

		bool m_flags[ 16 ] = { }; /* e_player_flags row shown */
		int m_money        = 0;

		bool m_has_obb      = false;
		c_vector m_obb_mins = { }, m_obb_maxs = { };
		matrix3x4_t m_frame = { };

		/* bone parts: m_studio null ( dormant / shared ) skips skeleton, hitboxes, trail, head avatar */
		studiohdr_t* m_studio                  = nullptr;
		const matrix3x4_t* m_bones             = nullptr;
		const matrix3x4_t* m_backtrack_bones   = nullptr; /* oldest record, skeleton type 1 */
		std::vector< hitbox_capsule_t > m_capsules = { };
		std::vector< c_vector > m_trail        = { };     /* head per valid record */

		IDirect3DTexture9* m_avatar = nullptr;
		bool m_has_head             = false;
		c_vector m_head = { }, m_view_right = { };
		float m_head_radius = 0.f;
	};

	void draw_player( const esp_player_t& player, const projector_t& w2s, std::vector< draw_object_t >& out );

	std::vector< hitbox_capsule_t > hitbox_capsules( studiohdr_t* studio, int hitbox_set, const matrix3x4_t* matrices, int bone_count );

	struct impl_t {
		void on_paint_traverse( );

		void on_player_hurt( const c_vector& origin, int victim, int damage );

		/* plugin god ( sm_god ) never reaches the client. seen as an enemy's bullet_impact landing in their hull
		   with no player_hurt after. fed from fire_event_intern, any shooter */
		void god_on_impact( int shooter, const c_vector& position );
		void god_on_hurt( int victim );
		bool is_god( int index );

		void hitboxes_on_hurt( int victim );

		using hitbox_capsule_t = n_players::hitbox_capsule_t;

		struct hit_hitboxes_t {
			std::vector< hitbox_capsule_t > m_capsules = { };
			float m_time                               = 0.f;
		};

		std::vector< damage_number_t > m_damage_numbers = { };
		std::vector< hit_hitboxes_t > m_hit_hitboxes    = { };

		struct {
			c_base_entity* m_active_weapon = nullptr;
			c_weapon_data* m_weapon_data   = nullptr;
			float m_animated_health        = 1.f;
			float m_ammo                   = 1.f;
		} m_backup_player_data[ 65 ];

		float m_fading_alpha[ 65 ]    = { };
		float m_stored_cur_time[ 65 ] = { };

	private:
		struct god_t {
			float m_pending = 0.f;
			int m_hits      = 0;
			int m_strikes   = 0;
		} m_god[ 65 ];

		void god_resolve( int index );

		struct wallbang_t {
			int m_tick    = -1;
			bool m_result = false;
		} m_wallbang[ 65 ];

		bool wallbangable( c_base_entity* entity, int index );

		void players( );
		void sound_arrows( );
		void damage_numbers( );
		void hit_hitboxes( );
	};
}

inline n_players::impl_t g_players{ };
