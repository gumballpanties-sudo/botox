#pragma once

#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"
#include <algorithm>
#include <bit>
#include <cmath>

namespace n_tick
{
	inline int s_cached_tick   = -1;
	inline float s_cached_ipt  = 0.015625f;

	inline bool forced_128( )
	{
		return GET_VARIABLE( g_variables.m_tick_fix_128, bool );
	}

	inline float interval( )
	{
		const int tick = g_interfaces.m_global_vars_base ? g_interfaces.m_global_vars_base->m_tick_count : 0;
		if ( tick == s_cached_tick )
			return s_cached_ipt;

		float ipt = g_interfaces.m_global_vars_base ? g_interfaces.m_global_vars_base->m_interval_per_tick : 0.015625f;
		if ( ipt <= 0.f )
			ipt = 0.015625f;
		if ( forced_128( ) )
			ipt = 1.f / 128.f;

		s_cached_ipt  = ipt;
		s_cached_tick = tick;
		return ipt;
	}

	inline float rate( ) { return 1.f / interval( ); }

	// real server tick, ignores the 128 checkbox. wall-clock caps must use this, or forced 128 on 64 halves them
	inline float engine_interval( )
	{
		const float ipt = g_interfaces.m_global_vars_base ? g_interfaces.m_global_vars_base->m_interval_per_tick : 0.015625f;
		return ipt > 0.f ? ipt : 0.015625f;
	}

	inline float scale( ) { return std::clamp( 64.f * interval( ), 0.25f, 4.f ); }

	inline long long qpc_freq( )
	{
		static const long long freq = [ ] {
			LARGE_INTEGER f{ };
			QueryPerformanceFrequency( &f );
			return f.QuadPart > 0ll ? f.QuadPart : 1ll;
		}( );
		return freq;
	}

	inline long long qpc_now( )
	{
		LARGE_INTEGER c{ };
		QueryPerformanceCounter( &c );
		return c.QuadPart;
	}

	constexpr double k_cmd_total_s = 0.6 / 128.0;

	enum e_search : int { search_none = -1, search_eb, search_tb, search_as, search_aa, search_wc, search_pf, search_ab, search_ba, search_psa, search_es, search_tung };

	inline unsigned s_hungry_prev = 0u, s_hungry_now = 0u, s_started_now = 0u;
	inline unsigned s_fair_cuts = 0u;

	inline long long s_cmd_start = 0ll, s_cmd_span = 0ll;
	inline bool s_cmd_clock_live = false;

	inline void begin_cmd_clock( )
	{
		s_cmd_start = qpc_now( );
		s_cmd_span  = static_cast< long long >( static_cast< double >( qpc_freq( ) ) * k_cmd_total_s );
		s_cmd_clock_live = true;
		s_hungry_prev    = s_hungry_now;
		s_hungry_now = s_started_now = 0u;
	}

	inline long long cmd_clock_left( )
	{
		if ( !s_cmd_clock_live )
			return ( 1ll << 62 );

		const long long left = s_cmd_span - ( qpc_now( ) - s_cmd_start );
		return left > 0ll ? left : 0ll;
	}

	inline int ticks( int ticks_at_64 )
	{
		const float s = scale( );
		if ( s <= 0.f )
			return ticks_at_64;
		const int out = static_cast< int >( std::round( static_cast< float >( ticks_at_64 ) / s ) );
		return out < 1 ? 1 : out;
	}

	class c_sim_budget
	{
		long long m_freq = 1ll, m_start = 0ll, m_span = 0ll;
		e_search m_id = search_none;

		static long long now( ) { return qpc_now( ); }

		bool hungry( ) const
		{
			if ( m_id != search_none && s_cmd_clock_live )
				s_hungry_now |= 1u << m_id;
			return true;
		}

	public:
		void start( const float fraction, const float interval_override = 0.f, const e_search id = search_none )
		{
			m_freq  = qpc_freq( );
			m_start = now( );
			m_id    = id;
			const float ipt = interval_override > 0.f ? interval_override : interval( );
			m_span  = static_cast< long long >( static_cast< double >( m_freq ) * static_cast< double >( ipt ) *
			                                    static_cast< double >( std::clamp( fraction, 0.02f, 0.9f ) ) );

			long long left = cmd_clock_left( );
			if ( s_cmd_clock_live ) {
				if ( id != search_none )
					s_started_now |= 1u << id;
				if ( const int owed = std::popcount( s_hungry_prev & ~s_started_now ); owed > 0 && m_span > left / ( 1 + owed ) ) {
					left /= 1 + owed;
					s_fair_cuts++;
				}
			}
			if ( m_span > left )
				m_span = left;
		}

		long long used_us( ) const { return ( now( ) - m_start ) * 1000000ll / m_freq; }
		long long left_us( ) const { return std::max( 0ll, m_span - ( now( ) - m_start ) ) * 1000000ll / m_freq; }

		bool expired( ) const { return now( ) - m_start >= m_span && hungry( ); }

		bool cannot_fit( const long long want_us ) const { return ( now( ) - m_start ) + want_us * m_freq / 1000000ll >= m_span && hungry( ); }
	};
}
