#include "logger.h"
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <share.h>

static SRWLOCK s_dbg_lock = SRWLOCK_INIT;
static FILE* s_dbg_file   = nullptr;
static bool s_dbg_closed  = false; /* eject closed it; a late line must not reopen "w" and wipe it */
static std::atomic< bool > s_dbg_dirty{ false };

void botox_dbg_log( const char* fmt, ... )
{
	if ( !GET_VARIABLE( g_variables.m_debug_log, bool ) ) {
		if ( s_dbg_dirty.load( std::memory_order_relaxed ) ) {
			AcquireSRWLockExclusive( &s_dbg_lock );
			if ( s_dbg_file )
				fflush( s_dbg_file );
			s_dbg_dirty = false;
			ReleaseSRWLockExclusive( &s_dbg_lock );
		}
		return;
	}
	if ( !fmt )
		return;

	AcquireSRWLockExclusive( &s_dbg_lock );

	if ( !s_dbg_file && !s_dbg_closed ) {
		s_dbg_file = _fsopen( "C:\\botox\\botox_debug.log", "w", _SH_DENYNO );
		static char log_buffer[ 256 * 1024 ];
		if ( s_dbg_file )
			setvbuf( s_dbg_file, log_buffer, _IOFBF, sizeof( log_buffer ) );
	}

	if ( s_dbg_file ) {
		va_list args;
		va_start( args, fmt );
		vfprintf( s_dbg_file, fmt, args );
		va_end( args );

		fputc( '\n', s_dbg_file );
		s_dbg_dirty = true;

		/* flush every 100 ms, not per line. a hard crash can eat the last few lines */
		static unsigned long long last_flush = 0ull;
		if ( const unsigned long long now = GetTickCount64( ); now - last_flush >= 100ull ) {
			last_flush = now;
			fflush( s_dbg_file );
			s_dbg_dirty = false;
		}
	}

	ReleaseSRWLockExclusive( &s_dbg_lock );
}

void botox_dbg_log_close( )
{
	AcquireSRWLockExclusive( &s_dbg_lock );
	if ( s_dbg_file ) {
		fclose( s_dbg_file );
		s_dbg_file = nullptr;
	}
	s_dbg_closed = true;
	s_dbg_dirty  = false;
	ReleaseSRWLockExclusive( &s_dbg_lock );
}
