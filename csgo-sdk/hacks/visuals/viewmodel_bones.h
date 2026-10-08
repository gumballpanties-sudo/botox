#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

#include "../../game/sdk/includes/includes.h"
#include "../../globals/config/variables.h"
#include "../../globals/globals.h"
#include "../../globals/interfaces/interfaces.h"
#include "../../globals/math/math.h"

namespace n_viewmodel_bones
{
	struct proxy_t {
		float m_center[ 3 ]{ };

		float m_center_then[ 3 ]{ };

		float m_rotation[ 3 ][ 3 ]{ };
	};

	static constexpr int max_proxies = 16;

	static constexpr int weapon_proxies = 10;

	static constexpr int other_proxies = max_proxies - weapon_proxies;

	struct set_t {
		int m_count = 0;

		int m_frame = -1;

		int m_weapon = 0;

		float m_span = 0.f;

		proxy_t m_proxy[ max_proxies ]{ };
	};

	struct impl_t {
		void collect( void* state, model_render_info_t& info, matrix3x4_t* bones, const bool allowed )
		{
			if ( !allowed || !bones || !state || !info.model || !g_interfaces.m_global_vars_base )
				return;

			if ( !GET_VARIABLE( g_variables.m_true_motion_blur, bool ) || !GET_VARIABLE( g_variables.m_true_motion_blur_viewmodel, bool ) )
				return;

			const char* const model_name = info.model->m_name;

			if ( !model_name || !strstr( model_name, "weapons/v_" ) )
				return;

			const auto header = *reinterpret_cast< studiohdr_t** >( state );

			if ( !header )
				return;

			const int bone_count = header->n_bones;

			if ( bone_count <= 0 || bone_count > max_bones )
				return;

			const int frame = g_interfaces.m_global_vars_base->m_frame_count;

			if ( frame != this->m_frame ) {
				this->m_frame        = frame;
				this->m_weapon_count = 0;
				this->m_other_count  = 0;
				this->m_span         = 0.f;
				this->m_clock += std::clamp( g_interfaces.m_global_vars_base->m_abs_frame_time, 0.f, 0.25f );
			}

			slot_t* slot = nullptr;

			for ( auto& entry : this->m_slot ) {
				if ( entry.m_model == info.model ) {
					slot = &entry;
					break;
				}
			}

			if ( !slot ) {
				slot = &this->m_slot[ 0 ];

				for ( auto& entry : this->m_slot ) {
					if ( entry.m_frame < slot->m_frame )
						slot = &entry;
				}

				slot->m_model  = info.model;
				slot->m_frame  = -1;
				slot->m_bones  = 0;
				slot->m_picks  = 0;
				slot->m_filled = 0;

				slot->m_weapon = strstr( model_name, "v_models" ) == nullptr;
			}

			if ( slot->m_frame == frame )
				return;

			if ( slot->m_picks <= 0 || slot->m_bones != bone_count ) {
				this->choose( *slot, bones, bone_count );
				slot->m_filled = 0;
			}

			// frame gap = missing clock time, history can't be timed
			if ( slot->m_frame != frame - 1 )
				slot->m_filled = 0;

			matrix3x4_t ride{ };

			if ( info.model_to_world ) {
				ride = *info.model_to_world;
			}
			else {
				g_math.angle_matrix( c_angle( info.angles.m_x, info.angles.m_y, info.angles.m_z ), ride );

				ride[ 0 ][ 3 ] = info.origin.m_x;
				ride[ 1 ][ 3 ] = info.origin.m_y;
				ride[ 2 ][ 3 ] = info.origin.m_z;
			}

			const float ride_origin[ 3 ] = { ride[ 0 ][ 3 ], ride[ 1 ][ 3 ], ride[ 2 ][ 3 ] };

			pose_t local[ max_proxies ]{ };

			for ( int index = 0; index < slot->m_picks; ++index ) {
				const matrix3x4_t& now = bones[ slot->m_pick[ index ] ];

				const float away[ 3 ] = { now[ 0 ][ 3 ] - ride_origin[ 0 ], now[ 1 ][ 3 ] - ride_origin[ 1 ], now[ 2 ][ 3 ] - ride_origin[ 2 ] };

				for ( int row = 0; row < 3; ++row ) {
					for ( int column = 0; column < 3; ++column )
						local[ index ][ row ][ column ] = ride[ 0 ][ row ] * now[ 0 ][ column ] + ride[ 1 ][ row ] * now[ 1 ][ column ] +
						                                  ride[ 2 ][ row ] * now[ 2 ][ column ];

					local[ index ][ row ][ 3 ] = ride[ 0 ][ row ] * away[ 0 ] + ride[ 1 ][ row ] * away[ 1 ] + ride[ 2 ][ row ] * away[ 2 ];
				}
			}

			// anim reset / swap / teleport: history before the pop must never be blended across
			if ( slot->m_filled > 0 && popped( *slot, local ) )
				slot->m_filled = 0;

			slot->m_head = ( slot->m_head + 1 ) % history_size;

			std::memcpy( slot->m_pose[ slot->m_head ], local, sizeof( local ) );

			slot->m_time[ slot->m_head ] = this->m_clock;

			if ( slot->m_filled < history_size )
				++slot->m_filled;

			this->emit( *slot, bones, ride );

			slot->m_bones = bone_count;
			slot->m_frame = frame;

			if ( this->m_weapon_count > 0 || this->m_other_count > 0 )
				this->publish( frame );
		}

		// screen pass side, main thread once a frame. a copy: the draws run on the material thread when queued.
		set_t read( ) const
		{
			const int ready = this->m_ready.load( std::memory_order_acquire );

			if ( ready < 0 )
				return set_t{ };

			return this->m_published[ ready ];
		}

	private:
		static constexpr int max_bones    = 192;
		static constexpr int max_slots    = 6;
		static constexpr int history_size = 64;

		using pose_t = float[ 3 ][ 4 ];

		struct slot_t {
			const model_t* m_model = nullptr;

			int m_frame = -1;
			int m_bones = 0;

			bool m_weapon = false;

			// WHICH bones this part publishes, chosen once and never touched again
			int m_picks = 0;
			int m_pick[ max_proxies ]{ };

			// ride-local poses of the picks, newest at m_head
			int m_head   = 0;
			int m_filled = 0;

			double m_time[ history_size ]{ };
			pose_t m_pose[ history_size ][ max_proxies ]{ };
		};

		bool popped( const slot_t& slot, const pose_t ( &local )[ max_proxies ] ) const
		{
			constexpr float max_move = 24.f;
			constexpr float min_cos  = 0.7071f;

			for ( int index = 0; index < slot.m_picks; ++index ) {
				const pose_t& old = slot.m_pose[ slot.m_head ][ index ];
				const pose_t& now = local[ index ];

				const float d[ 3 ] = { now[ 0 ][ 3 ] - old[ 0 ][ 3 ], now[ 1 ][ 3 ] - old[ 1 ][ 3 ], now[ 2 ][ 3 ] - old[ 2 ][ 3 ] };

				if ( d[ 0 ] * d[ 0 ] + d[ 1 ] * d[ 1 ] + d[ 2 ] * d[ 2 ] > max_move * max_move )
					return true;

				float trace = 0.f;

				for ( int row = 0; row < 3; ++row ) {
					for ( int column = 0; column < 3; ++column )
						trace += now[ row ][ column ] * old[ row ][ column ];
				}

				if ( ( trace - 1.f ) * 0.5f < min_cos )
					return true;
			}

			return false;
		}

		// lerp + gram-schmidt on the axes ( columns ); never a cross product, a mirrored skeleton would flip
		static void blend_pose( const pose_t& a, const pose_t& b, const float t, pose_t& out )
		{
			for ( int row = 0; row < 3; ++row ) {
				for ( int column = 0; column < 4; ++column )
					out[ row ][ column ] = a[ row ][ column ] + ( b[ row ][ column ] - a[ row ][ column ] ) * t;
			}

			for ( int column = 0; column < 3; ++column ) {
				for ( int prior = 0; prior < column; ++prior ) {
					const float along = out[ 0 ][ column ] * out[ 0 ][ prior ] + out[ 1 ][ column ] * out[ 1 ][ prior ] +
					                    out[ 2 ][ column ] * out[ 2 ][ prior ];

					for ( int row = 0; row < 3; ++row )
						out[ row ][ column ] -= along * out[ row ][ prior ];
				}

				const float length =
					std::sqrt( out[ 0 ][ column ] * out[ 0 ][ column ] + out[ 1 ][ column ] * out[ 1 ][ column ] + out[ 2 ][ column ] * out[ 2 ][ column ] );

				if ( length < 1e-6f ) {
					for ( int row = 0; row < 3; ++row )
						out[ row ][ column ] = a[ row ][ column ];

					continue;
				}

				for ( int row = 0; row < 3; ++row )
					out[ row ][ column ] /= length;
			}
		}

		/* farthest point sampling on positions, seeded by the root: proxies spread over the whole part.
		   runs once per skeleton, the set must be stable. */
		void choose( slot_t& slot, matrix3x4_t* bones, const int bone_count )
		{
			int quota = slot.m_weapon ? weapon_proxies : other_proxies;

			if ( quota > bone_count )
				quota = bone_count;

			slot.m_pick[ 0 ] = 0;
			slot.m_picks     = 1;

			for ( int bone = 0; bone < bone_count; ++bone )
				this->m_distance[ bone ] = squared_bones( bones[ bone ], bones[ 0 ] );

			while ( slot.m_picks < quota ) {
				int best = 0;

				for ( int bone = 1; bone < bone_count; ++bone ) {
					if ( this->m_distance[ bone ] > this->m_distance[ best ] )
						best = bone;
				}

				if ( this->m_distance[ best ] < 0.25f )
					break;

				slot.m_pick[ slot.m_picks++ ] = best;

				for ( int bone = 0; bone < bone_count; ++bone ) {
					const float distance = squared_bones( bones[ bone ], bones[ best ] );

					if ( distance < this->m_distance[ bone ] )
						this->m_distance[ bone ] = distance;
				}
			}
		}

		// proxy = the pick's pose one shutter ago vs now, both on THIS frame's ride: what a real shutter swept
		void emit( const slot_t& slot, matrix3x4_t* bones, const matrix3x4_t& ride )
		{
			const double shutter = std::clamp( GET_VARIABLE( g_variables.m_true_motion_blur_shutter, float ), 0.f, 60.f ) * 0.001;
			const double target  = slot.m_time[ slot.m_head ] - shutter;

			int newer   = slot.m_head;
			int older   = slot.m_head;
			float blend = 0.f;

			for ( int back = 1; back < slot.m_filled; ++back ) {
				const int index = ( slot.m_head - back + history_size ) % history_size;

				older = index;

				if ( slot.m_time[ index ] > target ) {
					newer = index;
					continue;
				}

				const double gap = slot.m_time[ newer ] - slot.m_time[ index ];

				blend = gap > 1e-9 ? static_cast< float >( std::clamp( ( slot.m_time[ newer ] - target ) / gap, 0.0, 1.0 ) ) : 1.f;
				break;
			}

			if ( slot.m_weapon ) {
				const double then = slot.m_time[ newer ] + ( slot.m_time[ older ] - slot.m_time[ newer ] ) * blend;

				this->m_span = static_cast< float >( slot.m_time[ slot.m_head ] - then );
			}

			for ( int index = 0; index < slot.m_picks; ++index ) {
				proxy_t* target_proxy = nullptr;

				if ( slot.m_weapon ) {
					if ( this->m_weapon_count < weapon_proxies )
						target_proxy = &this->m_weapon_proxy[ this->m_weapon_count++ ];
				}
				else if ( this->m_other_count < other_proxies ) {
					target_proxy = &this->m_other_proxy[ this->m_other_count++ ];
				}

				if ( !target_proxy )
					return;

				proxy_t& proxy = *target_proxy;

				const matrix3x4_t& world = bones[ slot.m_pick[ index ] ];
				const pose_t& now        = slot.m_pose[ slot.m_head ][ index ];

				pose_t then{ };

				if ( blend <= 0.f || older == newer )
					std::memcpy( then, slot.m_pose[ newer ][ index ], sizeof( then ) );
				else
					blend_pose( slot.m_pose[ newer ][ index ], slot.m_pose[ older ][ index ], blend, then );

				float local[ 3 ][ 3 ]{ };

				for ( int row = 0; row < 3; ++row ) {
					for ( int column = 0; column < 3; ++column )
						local[ row ][ column ] = then[ row ][ 0 ] * now[ column ][ 0 ] + then[ row ][ 1 ] * now[ column ][ 1 ] +
						                         then[ row ][ 2 ] * now[ column ][ 2 ];
				}

				float turned[ 3 ][ 3 ]{ };

				for ( int row = 0; row < 3; ++row ) {
					for ( int column = 0; column < 3; ++column )
						turned[ row ][ column ] = ride[ row ][ 0 ] * local[ 0 ][ column ] + ride[ row ][ 1 ] * local[ 1 ][ column ] +
						                          ride[ row ][ 2 ] * local[ 2 ][ column ];
				}

				for ( int row = 0; row < 3; ++row ) {
					for ( int column = 0; column < 3; ++column )
						proxy.m_rotation[ row ][ column ] = turned[ row ][ 0 ] * ride[ column ][ 0 ] + turned[ row ][ 1 ] * ride[ column ][ 1 ] +
						                                    turned[ row ][ 2 ] * ride[ column ][ 2 ];
				}

				// step taken in ride-local units: world coords never enter the difference
				const float step[ 3 ] = { then[ 0 ][ 3 ] - now[ 0 ][ 3 ], then[ 1 ][ 3 ] - now[ 1 ][ 3 ], then[ 2 ][ 3 ] - now[ 2 ][ 3 ] };

				for ( int axis = 0; axis < 3; ++axis ) {
					proxy.m_center[ axis ] = world[ axis ][ 3 ];

					proxy.m_center_then[ axis ] =
						world[ axis ][ 3 ] + ride[ axis ][ 0 ] * step[ 0 ] + ride[ axis ][ 1 ] * step[ 1 ] + ride[ axis ][ 2 ] * step[ 2 ];
				}
			}
		}

		// the weapon's slots first, so a third part that overflows the sixteen never costs the gun one
		void publish( const int frame )
		{
			const int ready = this->m_ready.load( std::memory_order_relaxed );
			const int next  = ready == 0 ? 1 : 0;

			set_t& target = this->m_published[ next ];

			int count = 0;

			for ( int index = 0; index < this->m_weapon_count && count < max_proxies; ++index )
				target.m_proxy[ count++ ] = this->m_weapon_proxy[ index ];

			for ( int index = 0; index < this->m_other_count && count < max_proxies; ++index )
				target.m_proxy[ count++ ] = this->m_other_proxy[ index ];

			target.m_count  = count;
			target.m_frame  = frame;
			target.m_weapon = this->m_weapon_count < count ? this->m_weapon_count : count;
			target.m_span   = this->m_span;

			this->m_ready.store( next, std::memory_order_release );
		}

		static float squared_bones( const matrix3x4_t& a, const matrix3x4_t& b )
		{
			const float d[ 3 ] = { a[ 0 ][ 3 ] - b[ 0 ][ 3 ], a[ 1 ][ 3 ] - b[ 1 ][ 3 ], a[ 2 ][ 3 ] - b[ 2 ][ 3 ] };

			return d[ 0 ] * d[ 0 ] + d[ 1 ] * d[ 1 ] + d[ 2 ] * d[ 2 ];
		}

		slot_t m_slot[ max_slots ]{ };

		float m_distance[ max_bones ]{ };

		// this frame's proxies, kept apart so the gun's share can never be eaten by an arms draw
		proxy_t m_weapon_proxy[ weapon_proxies ]{ };
		proxy_t m_other_proxy[ other_proxies ]{ };

		int m_weapon_count = 0;
		int m_other_count  = 0;

		int m_frame = -1;

		// sum of absoluteframetime: gpGlobals->realtime is a float and rounds to 0.24 ms after an hour
		double m_clock = 0.0;

		float m_span = 0.f;

		set_t m_published[ 2 ]{ };
		std::atomic< int > m_ready{ -1 };
	};
}

inline n_viewmodel_bones::impl_t g_viewmodel_bones{ };
