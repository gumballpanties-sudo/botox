#pragma once
#include "../../../../game/sdk/classes/c_engine_sound.h"
#include "../../../../game/sdk/includes/includes.h"
#include "../../../../globals/includes/includes.h"

namespace n_sound_esp
{
	struct impl_t {
		// m_real_time of each slot's last sound, 0 = never
		float m_last_sound[ 65 ] = { };
		c_vector m_origin[ 65 ] = { };

		void think( );
		void reset( );

		float alpha( int index, float duration, bool fade ) const;

		float player_gate( c_base_entity* player ) const;

	private:
		c_utl_vector< sound_info_t > m_sounds{ };
	};
}

inline n_sound_esp::impl_t g_sound_esp{ };
