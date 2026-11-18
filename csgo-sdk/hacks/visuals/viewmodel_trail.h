#pragma once

#include <algorithm>
#include <vector>

#include "../../game/sdk/includes/includes.h"
#include "../../globals/config/variables.h"
#include "../../globals/interfaces/interfaces.h"
#include "screen/sim_clock.h"
#include "viewmodel_fps.h"

namespace n_viewmodel_trail
{
	static constexpr int max_parts   = 3;
	static constexpr int max_samples = 128;
	static constexpr int max_bones   = 256;
	static constexpr int max_ghosts  = 64;
	static constexpr float min_time  = 0.05f;
	static constexpr float max_time  = 1.5f;

	// longer without a draw ( scoped, dead, hitch ) = history dropped, ghosts never bridge it
	static constexpr double max_gap = 0.25;

	/* live viewmodel sits in DepthRange( 0, 0.1 ) ( viewrender.cpp DrawViewModels ): ghosts just past it = under every
	   live part in any draw order, still over the world */
	static constexpr float ghost_depth_near = 0.1f;
	static constexpr float ghost_depth_far  = 0.11f;

	/* poses in camera space: walking / turning never moves a ghost, only anim + sway + bob. sim clock times, spaced
	   >= length / ( max_samples - 2 ) so the ring always spans the length; newest slot = this frame's pose */
	struct part_t {
		const model_t* m_model = nullptr;
		int m_bones            = 0;
		int m_head             = 0;
		int m_count            = 0;
		double m_time[ max_samples ]{ };
		std::vector< matrix3x4_t > m_pose{ };

		int index( const int from_oldest ) const { return ( this->m_head - this->m_count + 1 + from_oldest + max_samples ) % max_samples; }
		matrix3x4_t* pose( const int slot ) { return this->m_pose.data( ) + slot * this->m_bones; }
	};

	struct impl_t {
		// inside DrawViewModels only: the camera there is the one the viewmodel was placed on
		void record( const int part, const model_t* model, const int bone_count, const matrix3x4_t* bones )
		{
			const auto globals = g_interfaces.m_global_vars_base;
			if ( !globals || !g_viewmodel_fps.in_view_models( ) || part < 0 || part >= max_parts || !model || !bones || bone_count <= 0 ||
			     bone_count > max_bones )
				return;

			const double now = n_sim_clock::now_precise( globals );
			part_t& entry    = this->m_parts[ part ];

			if ( entry.m_model != model || entry.m_bones != bone_count ||
			     ( entry.m_count && ( now < entry.m_time[ entry.m_head ] || now - entry.m_time[ entry.m_head ] > max_gap ) ) ) {
				entry.m_model = model;
				entry.m_bones = bone_count;
				entry.m_count = 0;

				if ( entry.m_pose.size( ) < static_cast< std::size_t >( max_samples * bone_count ) )
					entry.m_pose.resize( static_cast< std::size_t >( max_samples * bone_count ) );
			}

			if ( entry.m_count && now == entry.m_time[ entry.m_head ] )
				return;

			// head keeps taking the live pose until it is a full step past the sample before it, then freezes
			const double step = length( ) / static_cast< double >( max_samples - 2 );
			const int prev    = ( entry.m_head + max_samples - 1 ) % max_samples;

			if ( entry.m_count < 2 || entry.m_time[ entry.m_head ] - entry.m_time[ prev ] >= step ) {
				entry.m_head  = entry.m_count ? ( entry.m_head + 1 ) % max_samples : 0;
				entry.m_count = std::min( entry.m_count + 1, max_samples );
			}

			entry.m_time[ entry.m_head ] = now;

			matrix3x4_t* const out = entry.pose( entry.m_head );
			for ( int bone = 0; bone < bone_count; ++bone )
				g_viewmodel_fps.to_camera( bones[ bone ], out[ bone ] );
		}

		/* fn( bones, alpha ) oldest first. ghosts evenly spread over the length ( count independent of it ), each lerped
		   between the two samples around its age = slides every frame. bones re-placed on this frame's camera into one
		   buffer: queued studiorender copies them at the call ( studiorendercontext.cpp DrawModel ) */
		template< typename t_fn >
		void for_each_ghost( void* context, const int part, const model_t* model, const int bone_count, t_fn fn )
		{
			if ( !context || !g_viewmodel_fps.in_view_models( ) || part < 0 || part >= max_parts )
				return;

			part_t& entry = this->m_parts[ part ];
			if ( entry.m_count < 2 || entry.m_model != model || entry.m_bones != bone_count )
				return;

			auto* const render_context = static_cast< c_material_render_context* >( context );
			const double now           = entry.m_time[ entry.m_head ];
			const double span          = length( );
			const int ghosts           = std::clamp( GET_VARIABLE( g_variables.m_chams_trail_ghosts, int ), 1, max_ghosts );
			const bool gradient        = GET_VARIABLE( g_variables.m_chams_trail_gradient, bool );
			const double oldest        = entry.m_time[ entry.index( 0 ) ];

			int older   = 0;
			bool pushed = false;

			for ( int ghost = ghosts; ghost >= 1; ghost-- ) {
				const double target = now - span * ghost / ghosts;
				if ( target < oldest )
					continue;

				// newest sample is at now > target: older + 1 always exists
				while ( entry.m_time[ entry.index( older + 1 ) ] < target )
					older++;

				const int a      = entry.index( older );
				const int b      = entry.index( older + 1 );
				const float mix  = static_cast< float >( ( target - entry.m_time[ a ] ) / ( entry.m_time[ b ] - entry.m_time[ a ] ) );
				const float keep = 1.f - mix;

				const matrix3x4_t* const from = entry.pose( a );
				const matrix3x4_t* const to   = entry.pose( b );

				for ( int bone = 0; bone < bone_count; ++bone ) {
					matrix3x4_t local{ };
					for ( int row = 0; row < 3; ++row )
						for ( int column = 0; column < 4; ++column )
							local[ row ][ column ] = from[ bone ][ row ][ column ] * keep + to[ bone ][ row ][ column ] * mix;

					g_viewmodel_fps.from_camera( local, this->m_world[ bone ] );
				}

				if ( !pushed ) {
					render_context->depth_range( ghost_depth_near, ghost_depth_far );
					pushed = true;
				}

				fn( this->m_world, gradient ? 1.f - static_cast< float >( ghost ) / static_cast< float >( ghosts + 1 ) : 1.f );
			}

			// back to the engine's viewmodel range ( bUseDepthHack is a constant true in csgo )
			if ( pushed )
				render_context->depth_range( 0.f, 0.1f );
		}

	private:
		static double length( ) { return std::clamp( GET_VARIABLE( g_variables.m_chams_trail_time, float ), min_time, max_time ); }

		part_t m_parts[ max_parts ]{ };
		matrix3x4_t m_world[ max_bones ]{ };
	};
}

inline n_viewmodel_trail::impl_t g_viewmodel_trail{ };
