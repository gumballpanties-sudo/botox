#pragma once
#include "../../../game/sdk/classes/c_vector.h"
#include <vector>

class c_base_entity;
class c_weapon_data;

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
	struct impl_t {
		void on_paint_traverse( );

		void on_player_hurt( const c_vector& origin, int victim, int damage );

		/* plugin god ( sm_god ) never reaches the client. seen as an enemy's bullet_impact landing in their hull
		   with no player_hurt after. fed from fire_event_intern, any shooter */
		void god_on_impact( int shooter, const c_vector& position );
		void god_on_hurt( int victim );
		bool is_god( int index );

		void hitboxes_on_hurt( int victim );

		struct hitbox_capsule_t {
			c_vector m_a   = { };
			c_vector m_b   = { };
			float m_radius = 0.f;
		};

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
		} m_backup_player_data[ 64 ];

		float m_fading_alpha[ 64 ]    = { };
		float m_stored_cur_time[ 64 ] = { };

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
