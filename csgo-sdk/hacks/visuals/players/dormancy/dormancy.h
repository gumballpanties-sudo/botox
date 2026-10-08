#pragma once
#include "../../../../game/sdk/classes/c_engine_sound.h"
#include "../../../../game/sdk/includes/includes.h"
#include "../../../../globals/includes/includes.h"
#include "../../../entity_cache/entity_cache.h"
#include <array>

namespace n_dormancy
{
	struct impl_t {
	public:
		struct entity_sound_t {
			void override_info( sound_info_t& sound )
			{
				m_abs_origin   = *sound.m_origin;
				m_receive_time = g_interfaces.m_global_vars_base->m_real_time;
			}

			void reset( bool store_data = false, const c_vector& origin = c_vector( 0, 0, 0 ), int flags = 0 )
			{
				if ( store_data ) {
					m_receive_time = g_interfaces.m_global_vars_base->m_real_time;
					m_abs_origin   = origin;
					m_flags        = flags;
				} else {
					m_receive_time = 0.0f;
					m_abs_origin   = c_vector( 0, 0, 0 );
					m_flags        = 0;
				}
			}

			int m_receive_time    = 0;
			int m_flags           = 0;
			c_vector m_abs_origin = c_vector( 0, 0, 0 );
		} m_sound_players[ 65 ];

		void think( );
		bool adjust_player_sound( c_base_entity* player );
		void setup_adjust_player( c_base_entity* player, sound_info_t& sound );
		bool is_valid_sound( sound_info_t& sound );

		c_utl_vector< sound_info_t > m_vec_sound_buffer{ };
		c_utl_vector< sound_info_t > m_cur_sound_list{ };
	};
}

inline n_dormancy::impl_t g_dormancy{ };
