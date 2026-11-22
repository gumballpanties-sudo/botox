#pragma once

#include <algorithm>
#include <cmath>
#include <cstring>

#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"
#include "../lagcomp/lagcomp.h"
#include "chams.h"

namespace n_shot_chams
{
	struct snapshot_t {
		matrix3x4_t m_bones[ 256 ]{ };
		c_base_entity* m_entity = nullptr;
		float m_time            = 0.f;
		bool m_valid            = false;
	};

	struct fired_t {
		float m_sent   = 0.f;
		float m_rewind = 0.f;
	};

	struct impl_t {
		/* create_move end, tick_count final ( backtrack, lua ). m_rewind = victim time the server lag compensates to */
		void on_shot( const c_user_cmd* cmd )
		{
			this->m_fired[ this->m_fired_next++ % k_fired ] = {
				g_interfaces.m_global_vars_base->m_real_time,
				g_math.ticks_to_time( cmd->m_tick_count - g_lagcomp.push_ticks( ) ) - g_lagcomp.lerp_time( ) };
		}

		/* player_hurt lands one rtt after the shot: cached bones are that later pose ( landed / ducked = ghost above ).
		   shot whose send + rtt is nearest now -> lagcomp record nearest its rewind time = the pose that took the hit */
		const matrix3x4_t* hit_pose( const int victim )
		{
			const auto* const records = g_lagcomp.m_records[ victim ];
			if ( !records )
				return nullptr;

			const float now = g_interfaces.m_global_vars_base->m_real_time;
			const float rtt = g_lagcomp.real_latency( );

			const fired_t* fired = nullptr;
			for ( const auto& f : this->m_fired )
				if ( f.m_sent > 0.f && now >= f.m_sent && now - f.m_sent < 1.f &&
				     ( !fired || std::fabs( now - f.m_sent - rtt ) < std::fabs( now - fired->m_sent - rtt ) ) )
					fired = &f;

			if ( !fired )
				return nullptr;

			const n_lagcomp::impl_t::record_t* best = nullptr;
			const int capacity                      = std::clamp( g_ctx.m_max_allocations, 0, n_lagcomp::impl_t::max_records );
			for ( int i = 0; i < capacity; i++ )
				if ( records[ i ].m_player == victim &&
				     ( !best || std::fabs( records[ i ].m_sim_time - fired->m_rewind ) < std::fabs( best->m_sim_time - fired->m_rewind ) ) )
					best = &records[ i ];

			return best && std::fabs( best->m_sim_time - fired->m_rewind ) <= 0.2f ? best->m_matrix : nullptr;
		}

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
			if ( const matrix3x4_t* const pose = this->hit_pose( victim ) )
				std::memcpy( shot.m_bones, pose, sizeof( matrix3x4_t ) * std::min( bones.count( ), 128 ) );
			else
				std::memcpy( shot.m_bones, bones.base( ), sizeof( matrix3x4_t ) * std::min( bones.count( ), 256 ) );
			shot.m_entity = entity;
			shot.m_time   = g_interfaces.m_global_vars_base->m_real_time;
			shot.m_valid  = true;
		}

		void reset( )
		{
			for ( auto& shot : this->m_snapshots )
				shot.m_valid = false;

			for ( auto& fired : this->m_fired )
				fired = { };
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

		static constexpr int k_fired = 16;
		fired_t m_fired[ k_fired ]{ };
		unsigned int m_fired_next = 0;
	};
}

inline n_shot_chams::impl_t g_shot_chams{ };
