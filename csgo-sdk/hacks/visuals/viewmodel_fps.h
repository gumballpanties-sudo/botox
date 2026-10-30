#pragma once

#include <algorithm>
#include <cmath>

#include "../../game/sdk/includes/includes.h"
#include "../../globals/config/variables.h"
#include "../../globals/interfaces/interfaces.h"
#include "../../globals/math/math.h"
#include "screen/sim_clock.h"

namespace n_viewmodel_fps
{
	/* bones held in camera space, refreshed m_viewmodel_low_fps_rate times a second, put back on the live
	   camera every frame: anim + sway + bob step, gun stays glued to the screen. main thread only */
	struct impl_t {
		// DrawViewModels, before the original
		void begin( const c_view_setup& setup )
		{
			this->m_inside = false;

			const auto globals = g_interfaces.m_global_vars_base;

			if ( !GET_VARIABLE( g_variables.m_viewmodel_low_fps, bool ) || !globals )
				return;

			g_math.angle_matrix( setup.m_angles, this->m_camera );

			this->m_camera[ 0 ][ 3 ] = setup.m_origin.m_x;
			this->m_camera[ 1 ][ 3 ] = setup.m_origin.m_y;
			this->m_camera[ 2 ][ 3 ] = setup.m_origin.m_z;

			if ( globals->m_frame_count != this->m_frame ) {
				this->m_frame = globals->m_frame_count;

				const float now  = n_sim_clock::now( globals );
				const float step = 1.f / std::clamp( GET_VARIABLE( g_variables.m_viewmodel_low_fps_rate, float ), 15.f, 60.f );
				const float gap  = now - this->m_last;

				this->m_step = gap >= step || gap < 0.f;

				// whole steps only: cadence stays even when the game fps isn't a multiple of the rate
				if ( this->m_step )
					this->m_last = gap < 0.f || gap > step * 4.f ? now : this->m_last + step * std::floor( gap / step );
			}

			this->m_inside = true;
		}

		void end( ) { this->m_inside = false; }

		// DrawModelExecute: the bones to draw with. anything drawn inside DrawViewModels = arms, gun, stattrak, sleeves
		matrix3x4_t* apply( void* state, matrix3x4_t* bones, const model_t* model )
		{
			if ( !this->m_inside || !bones || !state || !model )
				return bones;

			const auto header = *reinterpret_cast< studiohdr_t** >( state );

			if ( !header || header->n_bones <= 0 || header->n_bones > max_bones )
				return bones;

			const int count = header->n_bones;

			slot_t* slot = nullptr;

			for ( auto& entry : this->m_slot ) {
				if ( entry.m_model == model ) {
					slot = &entry;
					break;
				}
			}

			if ( !slot ) {
				slot = &this->m_slot[ 0 ];

				for ( auto& entry : this->m_slot ) {
					if ( entry.m_seen < slot->m_seen )
						slot = &entry;
				}

				slot->m_model = model;
				slot->m_seen  = -2;
			}

			matrix3x4_t* const out = slot->m_out[ this->m_frame & 1 ];

			if ( slot->m_seen == this->m_frame )
				return out;

			// new part / swapped skeleton / not drawn last frame ( scope, swap ): never show an old pose
			if ( this->m_step || slot->m_count != count || slot->m_seen != this->m_frame - 1 ) {
				for ( int bone = 0; bone < count; ++bone )
					to_camera( bones[ bone ], slot->m_held[ bone ] );
			}

			slot->m_count = count;
			slot->m_seen  = this->m_frame;

			for ( int bone = 0; bone < count; ++bone )
				from_camera( slot->m_held[ bone ], out[ bone ] );

			return out;
		}

	private:
		static constexpr int max_bones = 256;
		static constexpr int max_slots = 8;

		struct slot_t {
			const model_t* m_model = nullptr;

			int m_seen  = -2;
			int m_count = 0;

			matrix3x4_t m_held[ max_bones ]{ };

			// by frame parity: last frame's array may still be in flight on the queued material thread
			matrix3x4_t m_out[ 2 ][ max_bones ]{ };
		};

		// camera is rigid: inverse = transpose rotation, minus origin
		void to_camera( const matrix3x4_t& world, matrix3x4_t& local ) const
		{
			const auto& c = this->m_camera;

			const float away[ 3 ] = { world[ 0 ][ 3 ] - c[ 0 ][ 3 ], world[ 1 ][ 3 ] - c[ 1 ][ 3 ], world[ 2 ][ 3 ] - c[ 2 ][ 3 ] };

			for ( int row = 0; row < 3; ++row ) {
				for ( int column = 0; column < 3; ++column )
					local[ row ][ column ] = c[ 0 ][ row ] * world[ 0 ][ column ] + c[ 1 ][ row ] * world[ 1 ][ column ] + c[ 2 ][ row ] * world[ 2 ][ column ];

				local[ row ][ 3 ] = c[ 0 ][ row ] * away[ 0 ] + c[ 1 ][ row ] * away[ 1 ] + c[ 2 ][ row ] * away[ 2 ];
			}
		}

		void from_camera( const matrix3x4_t& local, matrix3x4_t& world ) const
		{
			const auto& c = this->m_camera;

			for ( int row = 0; row < 3; ++row ) {
				for ( int column = 0; column < 4; ++column )
					world[ row ][ column ] = c[ row ][ 0 ] * local[ 0 ][ column ] + c[ row ][ 1 ] * local[ 1 ][ column ] + c[ row ][ 2 ] * local[ 2 ][ column ];

				world[ row ][ 3 ] += c[ row ][ 3 ];
			}
		}

		slot_t m_slot[ max_slots ]{ };

		matrix3x4_t m_camera{ };

		int m_frame  = -1;
		float m_last = 0.f;
		bool m_step  = false;
		bool m_inside = false;
	};
}

inline n_viewmodel_fps::impl_t g_viewmodel_fps{ };
