#pragma once
#include "../../../game/sdk/classes/c_angle.h"
#include "../../../game/sdk/classes/c_vector.h"

#include <array>

class c_base_entity;

namespace n_grenade_path
{
	struct point_t {
		c_vector m_position = { };
		bool m_bounce       = false;
	};

	struct impl_t {
		void on_paint_traverse( );

	private:
		/* engine-accurate toss sim (CBaseCSGrenade::ThrowGrenade + CBaseEntity::PhysicsToss +
		   CBaseCSGrenadeProjectile::ResolveFlyCollisionCustom). false = no grenade in hand. */
		bool simulate( c_base_entity* weapon, short item_index );
		void draw( );

		bool needs_rebuild( const c_angle& angles, const c_vector& origin, float strength, short item_index );

		std::array< point_t, 640 > m_points = { };
		int m_count                         = 0;

		bool m_detonated             = false;
		c_vector m_detonate_position = { };
		float m_detonate_radius      = 0.f;

		int m_last_tick             = -1;
		c_angle m_last_angles       = { };
		c_vector m_last_origin      = { };
		float m_last_throw_strength = -1.f;
		short m_last_item_index     = 0;
	};
}

inline n_grenade_path::impl_t g_grenade_path{ };
