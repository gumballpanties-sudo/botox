#pragma once

#include <algorithm>
#include <cstring>

#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"
#include "chams.h"

namespace n_shot_chams
{
	struct snapshot_t {
		matrix3x4_t m_bones[ 256 ]{ };
		c_base_entity* m_entity = nullptr;
		float m_time            = 0.f;
		bool m_valid            = false;
	};

	struct impl_t {
		void on_player_hurt( const int victim )
		{
			if ( victim < 1 || victim > 64 || !GET_VARIABLE( g_variables.m_chams_enable, bool ) ||
			     !n_chams::impl_t::has_layers( GET_VARIABLE( g_variables.m_chams_shot_layers, std::vector< int > ) ) )
				return;

			c_base_entity* const entity = g_interfaces.m_client_entity_list->get< c_base_entity >( victim );
			if ( !entity || entity->is_dormant( ) )
				return;

			auto& bones = entity->get_cached_bone_data( );
			if ( bones.count( ) <= 0 || !bones.base( ) )
				return;

			snapshot_t& shot = this->m_snapshots[ victim ];
			std::memcpy( shot.m_bones, bones.base( ), sizeof( matrix3x4_t ) * std::min( bones.count( ), 256 ) );
			shot.m_entity = entity;
			shot.m_time   = g_interfaces.m_global_vars_base->m_real_time;
			shot.m_valid  = true;
		}

		void reset( )
		{
			for ( auto& shot : this->m_snapshots )
				shot.m_valid = false;
		}

		/* 0 = none. last 0.25 s fades linearly */
		float alpha( const int index )
		{
			snapshot_t& shot = this->m_snapshots[ index ];
			if ( !shot.m_valid )
				return 0.f;

			if ( g_interfaces.m_client_entity_list->get< c_base_entity >( index ) != shot.m_entity ) {
				shot.m_valid = false;
				return 0.f;
			}

			const float duration  = std::max( GET_VARIABLE( g_variables.m_chams_shot_duration, float ), 0.1f );
			const float remaining = duration - ( g_interfaces.m_global_vars_base->m_real_time - shot.m_time );
			if ( remaining <= 0.f || remaining > duration ) {
				shot.m_valid = false;
				return 0.f;
			}

			return std::min( remaining * 4.f, 1.f );
		}

		template< typename t_draw >
		void draw( const int index, t_draw draw_with_bones )
		{
			const float scale = this->alpha( index );
			if ( scale <= 0.f )
				return;

			matrix3x4_t* const bones = this->m_snapshots[ index ].m_bones;
			g_chams.draw_layers( GET_VARIABLE( g_variables.m_chams_shot_layers, std::vector< int > ),
			                     GET_VARIABLE( g_variables.m_chams_shot_colors, std::vector< c_color > ),
			                     GET_VARIABLE( g_variables.m_chams_shot_dark_colors, std::vector< c_color > ),
			                     GET_VARIABLE( g_variables.m_chams_shot_xqz, bool ), scale, [ & ]( ) { draw_with_bones( bones ); } );
			n_chams::impl_t::reset_original_color( );
		}

		snapshot_t m_snapshots[ 65 ]{ };
	};
}

inline n_shot_chams::impl_t g_shot_chams{ };
