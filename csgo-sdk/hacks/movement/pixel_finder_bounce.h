#pragma once

#include "../../game/sdk/classes/c_vector.h"
#include <cmath>

// Backup 2026-06-01 finder trials, one native prediction per step. The
// backend keeps carried jump/duck state while the coordinator yields.
// Production bounce walks both signs down the selected span. The backup's
// positive-height formula grows upward; seed_head_backup reproduces that
// defect for comparison only.
namespace n_pf_bounce
{
	struct sample_t {
		c_vector origin{ }, velocity{ };
		float hull = 0.f;
		bool walking = false, grounded = false;
	};
	inline bool finite( const sample_t& s ) {
		return std::isfinite( s.origin.m_x ) && std::isfinite( s.origin.m_y ) && std::isfinite( s.origin.m_z ) &&
		       std::isfinite( s.velocity.m_x ) && std::isfinite( s.velocity.m_y ) && std::isfinite( s.velocity.m_z );
	}
	inline float marker_head( float raw_head ) {
		// The first working finder's placement was round_pos2(raw_head + 1).
		// Apply that conversion once, before publishing preview/Assistant dots.
		// trunc preserves its negative-coordinate int cast without overflow.
		const float shifted = raw_head + 1.f;
		return std::trunc( shifted ) + ( shifted < 0.f ? -0.969644f : 0.04f );
	}
	inline float seed_head( float top, int row ) {
		const float seam = static_cast< float >( static_cast< int >( top ) - row );
		return seam + ( seam < 0.f || top < 0.f ? -0.02f : 0.02f );
	}
	inline float seed_head_backup( float top, int row ) {
		if ( top < 0.f )
			return static_cast< float >( static_cast< int >( top ) ) - 0.02f - static_cast< float >( row );
		return static_cast< float >( static_cast< int >( top ) ) + 0.02f + static_cast< float >( row );
	}
	inline float seed_feet( float top, int row ) {
		if ( top < 0.f )
			return static_cast< float >( static_cast< int >( top ) ) - 0.972092f - static_cast< float >( row );
		return static_cast< float >( static_cast< int >( top ) ) + 0.0287018f - static_cast< float >( row );
	}
	enum class event_t { none, candidate, done };
	struct cursor_t {
		float lo = 0.f, hi = 0.f, pin = 0.f;
		int row = 0, row_cap = 0, pass = 0, pass_cap = 4, tick = 0, sims = 0, tick_cap = 0;
		bool restart = true, ascent = true;
		float head = 0.f, planted_z = 0.f;
		sample_t before{ }, after{ };
		void start( float bottom, float top, float target, float dt, bool bounce = true ) {
			*this = { };
			lo = bottom; hi = top; pin = target; ascent = bounce;
			pass_cap = bounce ? 4 : 1;
			row_cap = static_cast< int >( top - bottom );
			if ( row_cap < 0 ) row_cap = 0;
			tick_cap = static_cast< int >( 4.f / dt );
			if ( tick_cap > 512 ) tick_cap = 512;
			if ( tick_cap < 1 ) tick_cap = 1;
		}
		void next_pass( ) { ++pass; tick = 0; restart = true; }
		template< typename Backend > event_t step( Backend& b ) {
			if ( sims >= 2048 ) return event_t::done;
			if ( pass >= pass_cap || row > row_cap ) return event_t::done;
			if ( restart ) { b.reset( ); restart = false; }
			else b.resume( );
			before = b.sample( );
			bool placed = false;
			const bool plant = ascent ? before.velocity.m_z > 0.f : before.velocity.m_z < 0.f;
			if ( plant ) {
				planted_z = ascent ? seed_head( hi, row ) - 54.f : seed_feet( hi, row );
				head = ascent ? planted_z + 54.f : planted_z;
				++row;
				if ( !b.place( planted_z ) ) return event_t::none;
				before = b.sample( );
				placed = true;
			}
			b.save_before( );
			++sims; ++tick;
			after = b.predict( );
			b.save_after( );
			if ( !finite( after ) || tick >= tick_cap || ( !ascent && row > row_cap ) ) {
				if ( ascent )
					next_pass( );
				else
					pass = pass_cap;
			} else if ( ascent && before.velocity.m_z < 0.f && after.velocity.m_z < 0.f ) {
				next_pass( );
			}
			if ( placed && finite( after ) && after.velocity.m_z == pin )
				return event_t::candidate;
			return event_t::none;
		}
	};
}
