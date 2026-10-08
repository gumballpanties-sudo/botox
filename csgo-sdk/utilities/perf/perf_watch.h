#pragma once

#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <share.h>

void botox_dbg_log( const char* fmt, ... );

#ifndef BOTOX_PERF_WATCH
#define BOTOX_PERF_WATCH 1
#endif

#ifndef BOTOX_PERF_FRAME_MS
#define BOTOX_PERF_FRAME_MS 16.0
#endif

/* one command should never own more than a tick; 5 ms says the movement searches piled up */
#ifndef BOTOX_PERF_CMD_MS
#define BOTOX_PERF_CMD_MS 5.0
#endif

namespace n_perf
{
	inline std::atomic< bool > s_eco_workers{ false };

	/* first line of every background worker: below normal, and with fast cores on EcoQoS too, which parks it
	   on efficiency cores (hybrid cpus) so it never takes a performance core from the game */
	inline void background_thread( )
	{
		SetThreadPriority( GetCurrentThread( ), THREAD_PRIORITY_BELOW_NORMAL );
		if ( !s_eco_workers.load( std::memory_order_relaxed ) )
			return;
		THREAD_POWER_THROTTLING_STATE state{ };
		state.Version     = THREAD_POWER_THROTTLING_CURRENT_VERSION;
		state.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
		state.StateMask   = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
		SetThreadInformation( GetCurrentThread( ), ThreadPowerThrottling, &state, sizeof( state ) );
	}

	enum e_zone : int {
		zone_cmd = 0,
		zone_cmd_prediction,
		zone_cmd_movement_pre,
		zone_cmd_movement_post,
		zone_cmd_edgebug,
		zone_cmd_aimbot,
		zone_cmd_lagcomp,
		zone_cmd_chud,
		zone_cmd_scaleform,
		zone_cmd_misc,
		zone_cmd_pixel_finder,
		zone_cmd_air_stuck,
		zone_cmd_tung,
		zone_cmd_detections,
		zone_cmd_texturebug,
		zone_cmd_fireman,
		zone_cmd_auto_bounce,
		zone_cmd_pixel_surf,
		zone_cmd_edge_skip,

		zone_paint,
		zone_paint_players,
		zone_paint_misc,
		zone_paint_movement,
		zone_paint_melt,
		zone_end_scene,
		zone_overlay,
		zone_avatars,
		zone_screen_pass,
		zone_draw_model,
		zone_frame_stage,

		zone_max
	};

	inline const char* zone_name( const int id )
	{
		static const char* names[ zone_max ] = { "cmd",     "cm.pred",  "cm.mv_pre", "cm.mv_post", "cm.eb",     "cm.aim",  "cm.lagcomp",
			                                     "cm.chud", "cm.scale", "cm.misc",   "cm.pf",      "cm.as",      "cm.tung",    "cm.det",    "cm.tb",      "cm.fm",   "cm.ab",
			                                     "cm.ps",   "cm.es",    "paint",    "pt.players", "pt.misc", "pt.movement", "pt.melt",
			                                     "endscene", "overlay", "avatars",   "screenpass", "drawmodel", "framestage" };
		return id >= 0 && id < zone_max ? names[ id ] : "?";
	}

#if !BOTOX_PERF_WATCH
	inline void zone_add( int, long long ) { }
	inline void cmd_begin( ) { }
	inline void cmd_end( ) { }
	inline void frame_end( ) { }
	inline void close_file( ) { }

	struct c_scope {
		explicit c_scope( int ) { }
		~c_scope( ) { }
	};
#else
	inline long long s_frame[ zone_max ]{ };
	inline long long s_cmd_start   = 0ll;
	inline long long s_frame_start = 0ll;

	inline unsigned long long s_second = 0ull;
	inline int s_lines_this_second     = 0;

	inline long long freq( )
	{
		static const long long f = [ ] {
			LARGE_INTEGER v{ };
			QueryPerformanceFrequency( &v );
			return v.QuadPart > 0ll ? v.QuadPart : 1ll;
		}( );
		return f;
	}

	inline long long now( )
	{
		LARGE_INTEGER c{ };
		QueryPerformanceCounter( &c );
		return c.QuadPart;
	}

	inline double ms( const long long qpc ) { return static_cast< double >( qpc ) * 1000.0 / static_cast< double >( freq( ) ); }

	inline void zone_add( const int id, const long long qpc )
	{
		if ( id >= 0 && id < zone_max )
			s_frame[ id ] += qpc;
	}

	inline FILE* s_file        = nullptr;
	inline bool s_file_closed  = false;

	inline FILE* file( )
	{
		if ( s_file || s_file_closed )
			return s_file;

		s_file = _fsopen( "C:\\botox\\botox_perf.log", "w", _SH_DENYNO );
		if ( s_file ) {
			static char buffer[ 64 * 1024 ];
			setvbuf( s_file, buffer, _IOFBF, sizeof( buffer ) );
		}

		return s_file;
	}

	inline void close_file( )
	{
		if ( s_file )
			fclose( s_file );
		s_file        = nullptr;
		s_file_closed = true;
	}

	inline void flush_on_timer( )
	{
		static unsigned long long last = 0ull;
		if ( const unsigned long long tick = GetTickCount64( ); tick - last >= 250ull ) {
			last = tick;
			if ( FILE* f = file( ) )
				fflush( f );
		}
	}

	inline void write_line( const char* what, const double total_ms )
	{
		const unsigned long long second = GetTickCount64( ) / 1000ull;
		if ( second != s_second ) {
			s_second            = second;
			s_lines_this_second = 0;
		}

		if ( ++s_lines_this_second > 30 )
			return;

		int order[ zone_max ];
		for ( int i = 0; i < zone_max; ++i )
			order[ i ] = i;

		for ( int i = 1; i < zone_max; ++i ) {
			const int key = order[ i ];
			int j         = i - 1;
			while ( j >= 0 && s_frame[ order[ j ] ] < s_frame[ key ] ) {
				order[ j + 1 ] = order[ j ];
				--j;
			}
			order[ j + 1 ] = key;
		}

		char line[ 1024 ];
		int length = snprintf( line, sizeof( line ), "PERF: %s=%.1fms", what, total_ms );

		for ( int i = 0; i < zone_max && length >= 0 && length < static_cast< int >( sizeof( line ) ); ++i ) {
			const double zone_ms = ms( s_frame[ order[ i ] ] );
			if ( zone_ms < 0.05 )
				break;

			length += snprintf( line + length, sizeof( line ) - length, " %s=%.1f", zone_name( order[ i ] ), zone_ms );
		}

		botox_dbg_log( "%s", line );

		if ( FILE* f = file( ) ) {
			fprintf( f, "%s\n", line );
			flush_on_timer( );
		}
	}

	inline void cmd_begin( ) { s_cmd_start = now( ); }

	inline void cmd_end( )
	{
		if ( !s_cmd_start )
			return;

		const long long spent = now( ) - s_cmd_start;
		s_cmd_start           = 0ll;

		zone_add( zone_cmd, spent );

		if ( const double spent_ms = ms( spent ); spent_ms >= BOTOX_PERF_CMD_MS )
			write_line( "cmd", spent_ms );
	}

	inline void frame_end( )
	{
		const long long stamp = now( );

		if ( s_frame_start ) {
			if ( const double frame_ms = ms( stamp - s_frame_start ); frame_ms >= BOTOX_PERF_FRAME_MS )
				write_line( "frame", frame_ms );
		}

		s_frame_start = stamp;

		for ( auto& zone : s_frame )
			zone = 0ll;
	}

	struct c_scope {
		explicit c_scope( const int id ) : m_id( id ), m_start( now( ) ) { }
		~c_scope( ) { zone_add( m_id, now( ) - m_start ); }

		c_scope( const c_scope& )            = delete;
		c_scope& operator=( const c_scope& ) = delete;

		int m_id;
		long long m_start;
	};
#endif

	struct mem_counts_t {
		std::size_t m_draw_data      = 0U;
		std::size_t m_players        = 0U;
		std::size_t m_edicts         = 0U;
		std::size_t m_damage_numbers = 0U;
		std::size_t m_kill_rows      = 0U;
		std::size_t m_chat_rows      = 0U;
		std::size_t m_image_cache    = 0U;
		std::size_t m_cubes          = 0U;
	};

#if !BOTOX_PERF_WATCH
	inline bool mem_due( ) { return false; }
	inline void mem_tick( const mem_counts_t& ) { }
#else
	inline bool mem_due( )
	{
		static unsigned long long last = 0ull;

		const unsigned long long tick = GetTickCount64( );
		if ( tick - last < 1000ull )
			return false;

		last = tick;
		return true;
	}

	struct process_memory_counters_t {
		unsigned long m_cb;
		unsigned long m_page_fault_count;
		std::size_t m_peak_working_set;
		std::size_t m_working_set;
		std::size_t m_quota_peak_paged_pool;
		std::size_t m_quota_paged_pool;
		std::size_t m_quota_peak_non_paged_pool;
		std::size_t m_quota_non_paged_pool;
		std::size_t m_pagefile_usage;
		std::size_t m_peak_pagefile_usage;
	};

	using get_process_memory_info_t = int( __stdcall* )( void*, process_memory_counters_t*, unsigned long );

	inline get_process_memory_info_t process_memory_info( )
	{
		static const get_process_memory_info_t fn = [ ]( ) -> get_process_memory_info_t {
			const HMODULE kernel32 = GetModuleHandleA( "kernel32.dll" );

			return kernel32 ? reinterpret_cast< get_process_memory_info_t >( GetProcAddress( kernel32, "K32GetProcessMemoryInfo" ) ) : nullptr;
		}( );

		return fn;
	}

	inline void mem_tick( const mem_counts_t& counts )
	{
		static const unsigned long long start = GetTickCount64( );

		const get_process_memory_info_t query = process_memory_info( );
		if ( !query )
			return;

		process_memory_counters_t memory{ };
		memory.m_cb = sizeof( memory );

		if ( !query( GetCurrentProcess( ), &memory, sizeof( memory ) ) )
			return;

		constexpr std::size_t k_step = 8ull * 1024ull * 1024ull;

		static std::size_t high_water = 0U;
		if ( memory.m_pagefile_usage < high_water + k_step )
			return;

		high_water = memory.m_pagefile_usage;

		char line[ 512 ];
		snprintf( line, sizeof( line ), "MEM: t=%llus priv=%.1fmb ws=%.1fmb draw=%zu players=%zu edicts=%zu dmg=%zu kills=%zu chat=%zu imgcache=%zu cubes=%zu",
		          ( GetTickCount64( ) - start ) / 1000ull, static_cast< double >( memory.m_pagefile_usage ) / ( 1024.0 * 1024.0 ),
		          static_cast< double >( memory.m_working_set ) / ( 1024.0 * 1024.0 ), counts.m_draw_data, counts.m_players, counts.m_edicts,
		          counts.m_damage_numbers, counts.m_kill_rows, counts.m_chat_rows, counts.m_image_cache, counts.m_cubes );

		botox_dbg_log( "%s", line );

		if ( FILE* f = file( ) ) {
			fprintf( f, "%s\n", line );
			flush_on_timer( );
		}
	}
#endif
}

#define PERF_ZONE( zone ) const n_perf::c_scope perf_zone_scope( n_perf::zone )
