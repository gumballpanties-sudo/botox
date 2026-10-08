#include "console.h"

#include "../../globals/includes/includes.h"

#include <cstring>

void botox_dbg_log( const char* fmt, ... );

/* no spdlog: it reads a static thread_local (cached thread id) and a manual mapped image has no tls index,
   so the first log call crashes. raw file api: no tls, no crt state, no allocation. */

static HANDLE g_log_file            = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION g_log_lock  = { };
static bool g_log_lock_ready        = false;

#ifdef _DEBUG
static HANDLE g_console_out = INVALID_HANDLE_VALUE;
#endif

static void write_all( HANDLE file, const char* text, unsigned long length )
{
	unsigned long written = 0;

	while ( written < length ) {
		unsigned long chunk = 0;

		if ( !WriteFile( file, text + written, length - written, &chunk, nullptr ) || !chunk )
			return;

		written += chunk;
	}
}

void n_console::impl_t::on_attach( const char* window_title )
{
	InitializeCriticalSection( &g_log_lock );
	g_log_lock_ready = true;

#ifdef _DEBUG
	AllocConsole( );

	if ( window_title )
		SetConsoleTitleA( window_title );

	g_console_out = GetStdHandle( STD_OUTPUT_HANDLE );
#endif

	char path[ MAX_PATH ] = { };
	HMODULE self          = nullptr;
	static int marker     = 0;

	if ( GetModuleHandleExA( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                         reinterpret_cast< const char* >( &marker ), &self ) &&
	     GetModuleFileNameA( self, path, sizeof( path ) ) ) {
		char* dot = strrchr( path, '.' );

		if ( dot )
			*dot = '\0';

		strcat_s( path, "_init.log" );
	} else if ( !GetTempPathA( sizeof( path ), path ) )
		return;
	else
		strcat_s( path, "fart_init.log" );

	g_log_file = CreateFileA( path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS, 0, nullptr );
}

void n_console::impl_t::emit( n_console::log_level level, const char* text )
{
	if ( !text )
		return;

	const char* tag = "info";

	switch ( level ) {
	case n_console::log_level::FATAL:
		tag = "fatal";
		break;
	case n_console::log_level::WARNING:
		tag = "warn";
		break;
	case n_console::log_level::DEBUG:
		tag = "debug";
		break;
	default:
		break;
	}

	SYSTEMTIME time = { };
	GetLocalTime( &time );

	char line[ 1024 ] = { };

	/* wsprintfA (user32, no crt state) caps at 1024, hence the buffer: long lines get cut, never overflow */
	const int length = wsprintfA( line, "[%02d:%02d:%02d.%03d] [%s] %s\r\n", time.wHour, time.wMinute, time.wSecond, time.wMilliseconds, tag,
	                              text );

	if ( length <= 0 )
		return;

	if ( g_log_lock_ready )
		EnterCriticalSection( &g_log_lock );

	if ( g_log_file != INVALID_HANDLE_VALUE ) {
		write_all( g_log_file, line, static_cast< unsigned long >( length ) );

	}

#ifdef _DEBUG
	if ( g_console_out != INVALID_HANDLE_VALUE )
		write_all( g_console_out, line, static_cast< unsigned long >( length ) );
#endif

	if ( g_log_lock_ready )
		LeaveCriticalSection( &g_log_lock );

	/* mirror into botox_debug.log when ticked. outside g_log_lock so the two logger locks never nest. "%s" so
	   a % isn't a format; trailing newline trimmed, the file logger adds its own. */
	int text_length = static_cast< int >( strlen( text ) );
	while ( text_length && ( text[ text_length - 1 ] == '\n' || text[ text_length - 1 ] == '\r' ) )
		--text_length;
	botox_dbg_log( "CON %s: %.*s", tag, text_length, text );
}

void n_console::impl_t::on_release( )
{
	if ( g_log_lock_ready )
		EnterCriticalSection( &g_log_lock );

	if ( g_log_file != INVALID_HANDLE_VALUE ) {
		FlushFileBuffers( g_log_file );
		CloseHandle( g_log_file );

		g_log_file = INVALID_HANDLE_VALUE;
	}

#ifdef _DEBUG
	FreeConsole( );

	g_console_out = INVALID_HANDLE_VALUE;
#endif

	if ( g_log_lock_ready ) {
		LeaveCriticalSection( &g_log_lock );

		g_log_lock_ready = false;

		DeleteCriticalSection( &g_log_lock );
	}
}
