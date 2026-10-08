#pragma once
#include "../../../utilities/perf/perf_watch.h"

#include <d3d9.h>
#include <initializer_list>

/* per effect gpu ms, logged once a second as "FXGPU: dof=1.23 mblur=0.80 ...". timestamp queries read back a few frames
   later and never with D3DGETDATA_FLUSH: a busy slot drops that frame's sample instead of stalling. d3d thread only. */
namespace n_gpu_timer
{
	enum e_pass : int {
		pass_depth_view = 0,
		pass_reflections,
		pass_ambient_occlusion,
		pass_depth_of_field,
		pass_motion_blur,
		pass_bloom,
		pass_color_correction,
		pass_max
	};

	inline constexpr const char* k_names[ pass_max ] = { "depth_view", "ssr", "ao", "dof", "mblur", "bloom", "cc" };

	inline constexpr int k_ring = 4;

	struct slot_t {
		IDirect3DQuery9* m_disjoint = nullptr;
		IDirect3DQuery9* m_start    = nullptr;
		IDirect3DQuery9* m_stop     = nullptr;
		IDirect3DQuery9* m_freq     = nullptr;
		bool m_pending              = false;
	};

	inline slot_t s_slots[ pass_max ][ k_ring ]{ };
	inline int s_next[ pass_max ]{ };
	inline double s_sum[ pass_max ]{ };
	inline int s_count[ pass_max ]{ };
	inline int s_open_pass             = -1;
	inline bool s_unsupported          = false;
	inline unsigned long long s_logged = 0ull;

	inline void release_slot( slot_t& slot )
	{
		for ( IDirect3DQuery9** query : { &slot.m_disjoint, &slot.m_start, &slot.m_stop, &slot.m_freq } ) {
			if ( *query )
				( *query )->Release( );
			*query = nullptr;
		}

		slot.m_pending = false;
	}

	inline void on_device_lost( )
	{
		for ( auto& ring : s_slots )
			for ( slot_t& slot : ring )
				release_slot( slot );

		s_open_pass = -1;
	}

	// true = slot free again
	inline bool poll( slot_t& slot, const int pass )
	{
		if ( !slot.m_pending )
			return true;

		BOOL disjoint = TRUE;
		UINT64 start = 0, stop = 0, freq = 0;

		if ( slot.m_disjoint->GetData( &disjoint, sizeof( disjoint ), 0 ) != S_OK || slot.m_start->GetData( &start, sizeof( start ), 0 ) != S_OK ||
		     slot.m_stop->GetData( &stop, sizeof( stop ), 0 ) != S_OK || slot.m_freq->GetData( &freq, sizeof( freq ), 0 ) != S_OK )
			return false;

		slot.m_pending = false;

		if ( !disjoint && freq && stop >= start ) {
			s_sum[ pass ] += static_cast< double >( stop - start ) * 1000.0 / static_cast< double >( freq );
			++s_count[ pass ];
		}

		return true;
	}

	inline void report( )
	{
		const unsigned long long tick = GetTickCount64( );

		if ( tick - s_logged < 1000ull )
			return;

		s_logged = tick;

		char line[ 256 ];
		int length = snprintf( line, sizeof( line ), "FXGPU:" );
		bool any   = false;

		for ( int pass = 0; pass < pass_max && length > 0 && length < static_cast< int >( sizeof( line ) ); ++pass ) {
			if ( !s_count[ pass ] )
				continue;

			length += snprintf( line + length, sizeof( line ) - length, " %s=%.2f", k_names[ pass ], s_sum[ pass ] / s_count[ pass ] );
			any = true;

			s_sum[ pass ]   = 0.0;
			s_count[ pass ] = 0;
		}

		if ( !any )
			return;

		botox_dbg_log( "%s", line );

#if BOTOX_PERF_WATCH
		// main thread opens the perf file; never race it into a second fopen
		if ( n_perf::s_file )
			fprintf( n_perf::s_file, "%s\n", line );
#endif
	}

	inline void begin( IDirect3DDevice9* device, const int pass )
	{
		s_open_pass = -1;

		if ( s_unsupported || !device )
			return;

		slot_t& slot = s_slots[ pass ][ s_next[ pass ] ];

		if ( !slot.m_start ) {
			if ( FAILED( device->CreateQuery( D3DQUERYTYPE_TIMESTAMPDISJOINT, &slot.m_disjoint ) ) ||
			     FAILED( device->CreateQuery( D3DQUERYTYPE_TIMESTAMP, &slot.m_start ) ) ||
			     FAILED( device->CreateQuery( D3DQUERYTYPE_TIMESTAMP, &slot.m_stop ) ) ||
			     FAILED( device->CreateQuery( D3DQUERYTYPE_TIMESTAMPFREQ, &slot.m_freq ) ) ) {
				release_slot( slot );
				s_unsupported = true;
				return;
			}
		}

		if ( !poll( slot, pass ) )
			return;

		slot.m_disjoint->Issue( D3DISSUE_BEGIN );
		slot.m_start->Issue( D3DISSUE_END );

		s_open_pass = pass;
	}

	inline void end( )
	{
		const int pass = s_open_pass;

		if ( pass < 0 )
			return;

		s_open_pass = -1;

		slot_t& slot = s_slots[ pass ][ s_next[ pass ] ];

		slot.m_stop->Issue( D3DISSUE_END );
		slot.m_disjoint->Issue( D3DISSUE_END );
		slot.m_freq->Issue( D3DISSUE_END );
		slot.m_pending = true;

		s_next[ pass ] = ( s_next[ pass ] + 1 ) % k_ring;

		for ( slot_t& other : s_slots[ pass ] )
			poll( other, pass );

		report( );
	}

	struct scope_t {
		scope_t( IDirect3DDevice9* device, const int pass ) { begin( device, pass ); }
		~scope_t( ) { end( ); }
		scope_t( const scope_t& )            = delete;
		scope_t& operator=( const scope_t& ) = delete;
	};
}
