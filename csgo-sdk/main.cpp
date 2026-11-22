
#include "game/sdk/includes/includes.h"
#include "globals/fonts/fonts.h"
#include "globals/includes/includes.h"
#include "hacks/misc/scaleform/image_cache.h"
#include "hacks/misc/scaleform/moi_hud.h"
#include "hacks/misc/misc.h"                  // bot names' steam fetch thread, same
#include "hacks/misc/chat_extras.h"           // translator http thread
#include "hacks/visuals/screen/render_queue.h"
#include "hacks/web/webview2host.h"
#include "utilities/perf/perf_watch.h"
#include "utilities/memory/va_ballast.h"

#include <atomic>
#include <cstdarg> // crash witness line builder
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <intrin.h>
#include <dbghelp.h>

void botox_dbg_log( const char* fmt, ... );
void botox_dbg_log_close( );

/* first witness: spdlog builds sinks + logger + thread before a byte hits disk (crash there = 0 byte file).
   raw api, no lazy_importer, no format, no allocation. */
static void boot_crumb( const char* text )
{
	char path[ MAX_PATH ] = { };

	if ( !GetTempPathA( sizeof( path ), path ) )
		return;

	strcat_s( path, "fart_boot.log" );

	const HANDLE file = CreateFileA( path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, 0, nullptr );
	if ( file == INVALID_HANDLE_VALUE )
		return;

	unsigned long written = 0;

	WriteFile( file, text, static_cast< unsigned long >( strlen( text ) ), &written, nullptr );
	WriteFile( file, "\r\n", 2, &written, nullptr );
	FlushFileBuffers( file ); // a crash one line later must not eat this line
	CloseHandle( file );

	botox_dbg_log( "BOOT %s", text );
}

static std::uintptr_t g_self_base = 0, g_self_end = 0;
static unsigned long g_self_stamp = 0;
static void* g_crash_witness      = nullptr;
static volatile long g_crash_hits = 0, g_crash_busy = 0;
static char g_crash_text[ 4096 ], g_crash_path[ MAX_PATH ];
static int g_crash_len = 0;
static unsigned char g_self_header[ 0x1000 ];
static unsigned long g_self_header_size = 0;

using minidump_write_t = BOOL( WINAPI* )( HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION, PMINIDUMP_USER_STREAM_INFORMATION,
                                          PMINIDUMP_CALLBACK_INFORMATION );
static minidump_write_t g_minidump_write = nullptr;
static volatile long g_crash_dumps       = 0;

static void crash_append( const char* fmt, ... )
{
	va_list args;
	va_start( args, fmt );
	const int written = _vsnprintf_s( g_crash_text + g_crash_len, sizeof( g_crash_text ) - g_crash_len, _TRUNCATE, fmt, args );
	va_end( args );

	g_crash_len = written < 0 ? static_cast< int >( sizeof( g_crash_text ) ) - 1 : g_crash_len + written;
}

static void crash_append_address( const std::uintptr_t address )
{
	if ( address >= g_self_base && address < g_self_end )
		return crash_append( " botox.dll+0x%x", static_cast< unsigned >( address - g_self_base ) );

	HMODULE module = nullptr;
	strcpy_s( g_crash_path, "?" );

	if ( GetModuleHandleExA( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                         reinterpret_cast< const char* >( address ), &module ) )
		GetModuleFileNameA( module, g_crash_path, sizeof( g_crash_path ) );

	const char* name = strrchr( g_crash_path, '\\' );
	crash_append( " %s+0x%x", name ? name + 1 : g_crash_path, static_cast< unsigned >( address - reinterpret_cast< std::uintptr_t >( module ) ) );
}

static bool crash_is_return( const std::uintptr_t address )
{
	MEMORY_BASIC_INFORMATION region{ };
	if ( address < 0x10000 || !VirtualQuery( reinterpret_cast< void* >( address ), &region, sizeof( region ) ) || region.State != MEM_COMMIT ||
	     region.Type != MEM_IMAGE || !( region.Protect & ( PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY ) ) ||
	     address - reinterpret_cast< std::uintptr_t >( region.BaseAddress ) < 6 )
		return false;

	const auto code = reinterpret_cast< const unsigned char* >( address );
	return code[ -5 ] == 0xE8 || code[ -6 ] == 0xFF || code[ -3 ] == 0xFF || code[ -2 ] == 0xFF;
}

static bool crash_readable( const std::uintptr_t address, const std::size_t bytes )
{
	MEMORY_BASIC_INFORMATION region{ };
	return address >= 0x10000 && VirtualQuery( reinterpret_cast< void* >( address ), &region, sizeof( region ) ) && region.State == MEM_COMMIT &&
	       !( region.Protect & ( PAGE_NOACCESS | PAGE_GUARD ) ) &&
	       address + bytes <= reinterpret_cast< std::uintptr_t >( region.BaseAddress ) + region.RegionSize;
}

/* 6 dwords at a register: a freed object shows a heap "vtable", a live one an image address */
static void crash_append_mem( const char* name, const std::uintptr_t address )
{
	if ( !crash_readable( address, 24 ) )
		return;

	const auto words = reinterpret_cast< const unsigned* >( address );
	crash_append( " %s=[%08x %08x %08x %08x %08x %08x]", name, words[ 0 ], words[ 1 ], words[ 2 ], words[ 3 ], words[ 4 ], words[ 5 ] );
}

static bool crash_in_image( const std::uintptr_t address )
{
	MEMORY_BASIC_INFORMATION region{ };
	return VirtualQuery( reinterpret_cast< void* >( address ), &region, sizeof( region ) ) && region.State == MEM_COMMIT && region.Type == MEM_IMAGE;
}

static bool header_heal( )
{
	if ( !g_self_header_size )
		return false;

	MEMORY_BASIC_INFORMATION region{ };
	if ( !VirtualQuery( reinterpret_cast< void* >( g_self_base ), &region, sizeof( region ) ) || region.State != MEM_COMMIT ||
	     ( region.Protect & ( PAGE_NOACCESS | PAGE_GUARD ) ) )
		return false;

	if ( reinterpret_cast< const IMAGE_DOS_HEADER* >( g_self_base )->e_magic == IMAGE_DOS_SIGNATURE )
		return false;

	unsigned long old = 0;
	if ( !VirtualProtect( reinterpret_cast< void* >( g_self_base ), g_self_header_size, PAGE_READWRITE, &old ) )
		return false;

	/* magic last: another thread's check above sees MZ only once the rest is in */
	memcpy( reinterpret_cast< unsigned char* >( g_self_base ) + 2, g_self_header + 2, g_self_header_size - 2 );
	_InterlockedExchange16( reinterpret_cast< volatile short* >( g_self_base ), *reinterpret_cast< const short* >( g_self_header ) );
	VirtualProtect( reinterpret_cast< void* >( g_self_base ), g_self_header_size, old, &old );
	return true;
}

/* tier0 out of memory ( tier0 +0xabb0 ): OutputDebugStringA( "***** OUT OF MEMORY! attempted allocation size: %u ****" ) then its
   crt abort( ) = int 0x29 fastfail at tier0 +0x29314, which no handler sees. that print ( 0x40010006 ) is the only witness */
static bool crash_is_oom( const EXCEPTION_RECORD* record )
{
	static constexpr char k_oom[ ] = "***** OUT OF MEMORY!";
	return record->ExceptionCode == 0x40010006 && record->NumberParameters >= 2 && record->ExceptionInformation[ 0 ] >= sizeof( k_oom ) &&
	       record->ExceptionInformation[ 1 ] && !strncmp( reinterpret_cast< const char* >( record->ExceptionInformation[ 1 ] ), k_oom, sizeof( k_oom ) - 1 );
}

/* 32 bit address space at the oom: a big ask fails on the largest free block, not the free total */
static void crash_append_address_space( )
{
	unsigned long long free_total = 0ull, largest = 0ull, priv = 0ull, reserved = 0ull, mapped = 0ull, image = 0ull;
	MEMORY_BASIC_INFORMATION region{ };
	for ( std::uintptr_t at = 0x10000; VirtualQuery( reinterpret_cast< void* >( at ), &region, sizeof( region ) ); ) {
		const unsigned long long size = region.RegionSize;
		if ( region.State == MEM_FREE ) {
			free_total += size;
			largest = size > largest ? size : largest;
		} else if ( region.State == MEM_RESERVE )
			reserved += size;
		else if ( region.Type == MEM_PRIVATE )
			priv += size;
		else if ( region.Type == MEM_MAPPED )
			mapped += size;
		else
			image += size;

		const std::uintptr_t next = reinterpret_cast< std::uintptr_t >( region.BaseAddress ) + region.RegionSize;
		if ( next <= at )
			break;
		at = next;
	}
	crash_append( " va: free=%llu mb largest_free=%llu mb private=%llu mb reserved=%llu mb mapped=%llu mb image=%llu mb", free_total >> 20,
	              largest >> 20, priv >> 20, reserved >> 20, mapped >> 20, image >> 20 );
	/* ballast 0/3 = rescues ran out, 3/3 = the ask never reached the NtAllocateVirtualMemory detour ( or no hook ) */
	crash_append( " ballast=%d/%d rescues=%ld", n_va_ballast::left( ), n_va_ballast::k_chunks, n_va_ballast::g_rescues.load( ) );
	/* game capture frame buffers live in csgo's address space ( mapped ~+100 mb ) */
	if ( GetModuleHandleA( "DiscordHook.dll" ) )
		crash_append( " capture=discord" );
	if ( GetModuleHandleA( "graphics-hook32.dll" ) )
		crash_append( " capture=obs" );
	if ( GetModuleHandleA( "nvspcap.dll" ) )
		crash_append( " capture=nvidia" );
}

static long __stdcall crash_witness( EXCEPTION_POINTERS* info )
{
	const auto record         = info->ExceptionRecord;
	const unsigned long code  = record->ExceptionCode;

	/* a heal logs whatever raised it (e06d7363 = c++ throw): that's who would have crashed us */
	const bool healed = header_heal( );
	const bool oom    = crash_is_oom( record );
	/* c0000409 here = crash_on_abort's own record: a real fastfail never reaches a vectored handler */
	const bool abort_hit = code == 0xC0000409;
	const bool fatal     = oom || abort_hit;

	if ( !healed && !fatal && code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_STACK_OVERFLOW && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
	     code != EXCEPTION_PRIV_INSTRUCTION && code != EXCEPTION_INT_DIVIDE_BY_ZERO && code != 0xC0000374 && code != 0xC000000D )
		return EXCEPTION_CONTINUE_SEARCH;

	/* some first chance faults get handled (engine probes): 8 per session, one thread at a time. the oom gets its own one: the
	   game dies right after it, whatever the 8 already spent */
	static volatile long s_oom_logged = 0;
	if ( ( fatal ? InterlockedExchange( &s_oom_logged, 1 ) != 0 : InterlockedIncrement( &g_crash_hits ) > 8 ) || InterlockedExchange( &g_crash_busy, 1 ) )
		return EXCEPTION_CONTINUE_SEARCH;

	const auto tib = reinterpret_cast< const NT_TIB* >( NtCurrentTeb( ) );
#ifdef _WIN64
	auto slot = reinterpret_cast< const std::uintptr_t* >( info->ContextRecord->Rsp );
#else
	auto slot = reinterpret_cast< const std::uintptr_t* >( info->ContextRecord->Esp );
#endif

	SYSTEMTIME now{ };
	GetLocalTime( &now );

	g_crash_len = 0;
	crash_append( "%02d:%02d:%02d.%03d %scode=%08lx thread=%lu stamp=%08lx stack_left=%u at", now.wHour, now.wMinute, now.wSecond,
	              now.wMilliseconds, healed ? "HEADER_HEALED " : "", code, GetCurrentThreadId( ), g_self_stamp,
	              static_cast< unsigned >( reinterpret_cast< std::uintptr_t >( slot ) - reinterpret_cast< std::uintptr_t >( tib->StackLimit ) ) );
	crash_append_address( reinterpret_cast< std::uintptr_t >( record->ExceptionAddress ) );

	if ( code == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2 )
		crash_append( " (%s 0x%08x)", record->ExceptionInformation[ 0 ] == 8 ? "exec" : record->ExceptionInformation[ 0 ] ? "write" : "read",
		              static_cast< unsigned >( record->ExceptionInformation[ 1 ] ) );

	if ( abort_hit )
		crash_append( " ABORT ( std::terminate = c++ throw out of a thread / noexcept, or purecall )" );

	if ( oom ) {
		char said[ 96 ] = { };
		strncpy_s( said, reinterpret_cast< const char* >( record->ExceptionInformation[ 1 ] ), _TRUNCATE );
		for ( char& c : said )
			if ( c == '\r' || c == '\n' )
				c = ' ';
		crash_append( " OOM \"%s\"", said );
		crash_append_address_space( );
	}

	const auto stack_start = slot;

	/* our addresses on the stack, innermost first: the hook chain that got here (a repeat = recursion). a scan, so a
	   stale value can show up too */
	crash_append( "\r\n  stack:" );
	for ( int i = 0, found = 0; i < 8192 && found < 24 && reinterpret_cast< std::uintptr_t >( slot + 1 ) <= reinterpret_cast< std::uintptr_t >( tib->StackBase );
	      ++i, ++slot ) {
		if ( *slot >= g_self_base && *slot < g_self_end ) {
			crash_append_address( *slot );
			++found;
		}
	}

	/* fault outside botox: name the foreign caller chain (plugin/extension), else the line says nothing */
	const auto fault = reinterpret_cast< std::uintptr_t >( record->ExceptionAddress );
	if ( fault < g_self_base || fault >= g_self_end ) {
		crash_append( "\r\n  mods:" );
		slot = stack_start;
		for ( int i = 0, found = 0; i < 2048 && found < 20 && reinterpret_cast< std::uintptr_t >( slot + 1 ) <= reinterpret_cast< std::uintptr_t >( tib->StackBase );
		      ++i, ++slot ) {
			if ( ( *slot < g_self_base || *slot >= g_self_end ) && crash_is_return( *slot ) ) {
				crash_append_address( *slot );
				++found;
			}
		}
	}
#ifndef _WIN64
	const auto ctx = info->ContextRecord;
	crash_append( "\r\n  regs: eax=%08lx ebx=%08lx ecx=%08lx edx=%08lx esi=%08lx edi=%08lx ebp=%08lx esp=%08lx", ctx->Eax, ctx->Ebx, ctx->Ecx,
	              ctx->Edx, ctx->Esi, ctx->Edi, ctx->Ebp, ctx->Esp );

	if ( fault < g_self_base || fault >= g_self_end ) {
		crash_append( "\r\n  mem:" );
		crash_append_mem( "eax", ctx->Eax );
		crash_append_mem( "ebx", ctx->Ebx );
		crash_append_mem( "ecx", ctx->Ecx );
		crash_append_mem( "esi", ctx->Esi );
		crash_append_mem( "edi", ctx->Edi );
		crash_append_mem( "esp", ctx->Esp );
	}
#endif

	/* jumped into unmapped code (null vfunc, freed object): nothing handles that, so it's the real crash. 1 per session */
	char dump_path[ MAX_PATH ] = { };
	/* oom: the memory map only ( who holds the address space ), small, the game is dead either way */
	const bool dump = g_minidump_write &&
	                  ( fatal || ( code == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2 && record->ExceptionInformation[ 1 ] == fault &&
	                             !crash_in_image( fault ) ) ) &&
	                  InterlockedIncrement( &g_crash_dumps ) == 1;
	if ( dump ) {
		sprintf_s( dump_path, "C:\\botox\\crash_%02d%02d%02d.dmp", now.wHour, now.wMinute, now.wSecond );
		crash_append( "\r\n  dump: %s", dump_path );
	}
	crash_append( "\r\n" );

	CreateDirectoryA( "C:\\botox", nullptr );
	const HANDLE file = CreateFileA( "C:\\botox\\botox_crash.log", FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, 0, nullptr );
	if ( file != INVALID_HANDLE_VALUE ) {
		unsigned long written = 0;
		WriteFile( file, g_crash_text, static_cast< unsigned long >( g_crash_len ), &written, nullptr );
		FlushFileBuffers( file );
		CloseHandle( file );
	}

	/* after the log line: a dump that hangs or dies still leaves the line on disk */
	if ( dump ) {
		if ( oom )
			n_va_ballast::give_dump_room( ); // dbghelp's heap grows into it, else a 0 byte dump at largest free 0 mb
		const HANDLE dump_file = CreateFileA( dump_path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr );
		if ( dump_file != INVALID_HANDLE_VALUE ) {
			MINIDUMP_EXCEPTION_INFORMATION exception{ GetCurrentThreadId( ), info, FALSE };
			const int type = oom ? MiniDumpWithFullMemoryInfo | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules
			                     : MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules;
			g_minidump_write( GetCurrentProcess( ), GetCurrentProcessId( ), dump_file, static_cast< MINIDUMP_TYPE >( type ), &exception, nullptr, nullptr );
			CloseHandle( dump_file );
		}
	}

	InterlockedExchange( &g_crash_busy, 0 );
	return EXCEPTION_CONTINUE_SEARCH;
}

/* abort( ) ( std::terminate, purecall ) = SIGABRT then an int 0x29 fastfail no vectored handler sees: 11-18 config load died with no
   line, no dump, last log lines unflushed. runs on the dying thread, so the stack scan finds the throw site under the dispatch frames */
static void __cdecl crash_on_abort( int )
{
	CONTEXT context{ };
	RtlCaptureContext( &context );

	EXCEPTION_RECORD record{ };
	record.ExceptionCode             = 0xC0000409;
	record.ExceptionAddress          = _ReturnAddress( );
	record.NumberParameters          = 1;
	record.ExceptionInformation[ 0 ] = 7; // FAST_FAIL_FATAL_APP_EXIT

	EXCEPTION_POINTERS info{ &record, &context };
	crash_witness( &info );
	botox_dbg_log_close( );
}

using abort_handler_t = void( __cdecl* )( int );
static abort_handler_t g_prev_abort = SIG_DFL;
static bool g_abort_hooked          = false;

static void crash_witness_install( void* instance )
{
	MEMORY_BASIC_INFORMATION region{ };
	g_self_base = g_self_end = reinterpret_cast< std::uintptr_t >( instance );

	const bool readable_header = VirtualQuery( instance, &region, sizeof( region ) ) && region.State == MEM_COMMIT &&
	                             !( region.Protect & ( PAGE_NOACCESS | PAGE_GUARD ) );

	while ( VirtualQuery( reinterpret_cast< void* >( g_self_end ), &region, sizeof( region ) ) && region.AllocationBase == instance )
		g_self_end += region.RegionSize;

	// PE stamp: crash_sym refuses a log line from another build
	const auto dos = static_cast< const IMAGE_DOS_HEADER* >( instance );
	if ( readable_header && dos->e_magic == IMAGE_DOS_SIGNATURE ) {
		const auto nt = reinterpret_cast< const IMAGE_NT_HEADERS* >( g_self_base + dos->e_lfanew );
		if ( nt->Signature == IMAGE_NT_SIGNATURE ) {
			g_self_stamp = nt->FileHeader.TimeDateStamp;

			const unsigned long size = nt->OptionalHeader.SizeOfHeaders;
			if ( size && size <= sizeof( g_self_header ) ) {
				memcpy( g_self_header, instance, size );
				g_self_header_size = size;
			}
		}
	}

	// here, not in the handler: LoadLibrary under a fault can deadlock on the loader lock
	if ( const HMODULE dbghelp = LoadLibraryA( "dbghelp.dll" ) )
		g_minidump_write = reinterpret_cast< minidump_write_t >( GetProcAddress( dbghelp, "MiniDumpWriteDump" ) );

	g_crash_witness = AddVectoredExceptionHandler( 1, crash_witness );

	g_prev_abort   = signal( SIGABRT, crash_on_abort );
	g_abort_hooked = g_prev_abort != SIG_ERR;
}

static void crash_witness_remove( )
{
	if ( g_crash_witness )
		RemoveVectoredExceptionHandler( g_crash_witness );

	g_crash_witness = nullptr;

	/* an unmapped module must not stay the abort handler. put back only if still ours ( someone may have set theirs since ) */
	if ( g_abort_hooked ) {
		g_abort_hooked = false;
		if ( const abort_handler_t current = signal( SIGABRT, g_prev_abort ); current != crash_on_abort )
			signal( SIGABRT, current );
	}
}

static std::atomic< bool > g_media_thread_alive{ false };

/* winrt session calls block for ms, so not on the render thread. metadata only, the thumbnail is built in end_scene */
static unsigned long __stdcall media_player_thread( void* )
{
	g_media_thread_alive = true;

	LI_FN( SetThreadPriority )( LI_FN( GetCurrentThread )( ), THREAD_PRIORITY_BELOW_NORMAL );

	/* fresh thread, so it has no apartment yet - winrt throws CO_E_NOTINITIALIZED without this */
	try {
		winrt::init_apartment( winrt::apartment_type::multi_threaded );
	} catch ( ... ) {
	}

	if ( !g_media_player.init( ) ) {
		g_console.print< n_console::log_level::WARNING >( "failed to initialise media player" );
		g_media_thread_alive = false;
		return 0;
	}

	while ( !g_ctx.m_unloading ) {
		if ( GET_VARIABLE( g_variables.m_media_player, bool ) ||
		     ( GET_VARIABLE( g_variables.m_clantag, bool ) && GET_VARIABLE( g_variables.m_clantag_animation, int ) == 4 ) )
			g_media_player.on_update( );

		// a control click refreshes 250 ms later, the app needs that long to switch track
		for ( int i = 0; i < 20 && !g_ctx.m_unloading; i++ ) {
			if ( g_media_player.run_command( ) ) {
				std::this_thread::sleep_for( std::chrono::milliseconds( 250 ) );
				break;
			}
			std::this_thread::sleep_for( std::chrono::milliseconds( 50 ) );
		}
	}

	g_media_player.shutdown( );

	g_media_thread_alive = false;

	return 0;
}

static std::atomic< bool > g_lyrics_thread_alive{ false };

/* lrclib fetch blocks up to its http timeouts on every track change: on the media thread that froze title, art and every control */
static unsigned long __stdcall lyrics_thread( void* )
{
	g_lyrics_thread_alive = true;

	LI_FN( SetThreadPriority )( LI_FN( GetCurrentThread )( ), THREAD_PRIORITY_BELOW_NORMAL );

	while ( !g_ctx.m_unloading ) {
		g_media_player.update_lyrics( GET_VARIABLE( g_variables.m_media_player, bool ) && GET_VARIABLE( g_variables.m_media_player_lyrics, bool ) );
		std::this_thread::sleep_for( std::chrono::milliseconds( 250 ) );
	}

	g_lyrics_thread_alive = false;

	return 0;
}

static std::atomic< bool > g_visualizer_thread_alive{ false };

static unsigned long __stdcall visualizer_thread( void* )
{
	g_visualizer_thread_alive = true;

	LI_FN( SetThreadPriority )( LI_FN( GetCurrentThread )( ), THREAD_PRIORITY_BELOW_NORMAL );

	g_media_player.visualizer_thread( g_ctx.m_unloading, [ ]( ) {
		return GET_VARIABLE( g_variables.m_media_player, bool ) && GET_VARIABLE( g_variables.m_media_player_visualizer, bool ) &&
		       !GET_VARIABLE( g_variables.m_media_player_simple, bool );
	} );

	g_visualizer_thread_alive = false;

	return 0;
}

static unsigned long __stdcall on_attach( void* instance )
{
	boot_crumb( "on_attach entered" );

	boot_screen_begin( );

	g_console.on_attach( "fart" );

	boot_crumb( "console ready" );

	/* first file log line (missing = init thread never ran). kept split from the vformat so the crumbs tell them apart */
	const std::string attached_line = std::vformat( "attached, module @ {:p}", std::make_format_args( instance ) );

	boot_crumb( "vformat ok" );

	g_console.print( attached_line.c_str( ) );

	boot_crumb( "first log line written" );

	const auto stage = []( const char* name, bool ( *fn )( ), const bool fatal ) -> bool {
		g_console.print( std::vformat( "initialising {:s}", std::make_format_args( name ) ).c_str( ) );

		const bool ok = fn( );
		boot_screen_step( );

		if ( ok ) {
			g_console.print( std::vformat( "initialised {:s}", std::make_format_args( name ) ).c_str( ) );
			return true;
		}

		g_console.print< n_console::log_level::WARNING >( std::vformat( "failed to initialise {:s}", std::make_format_args( name ) ).c_str( ) );

		if ( fatal )
			boot_crumb( "FATAL stage failed, module parked without hooks" );

		return !fatal;
	};

	const bool ready = stage( "module handles", [ ] { return g_modules.on_attach( ); }, true ) &&
	                   ( boot_crumb( "modules done, interfaces next" ), stage( "interfaces", [ ] { return g_interfaces.on_attach( ); }, true ) ) &&
	                   ( boot_crumb( "interfaces done, netvars next" ), stage( "netvar manager", [ ] { return g_netvars.on_attach( ); }, true ) ) &&
	                   stage( "convars", [ ] { return g_convars.on_attach( ); }, false ) &&
	                   stage( "input system", [ ] { return g_input.on_attach( ); }, false ) &&
	                   stage( "config system", [ ] { return g_config.on_attach( ); }, false );

	if ( !ready ) {
		while ( !g_ctx.m_eject_requested )
			std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );

		g_console.on_release( );
		botox_dbg_log_close( );
		crash_witness_remove( );
		n_va_ballast::release( ); // no hooks were made

		// same rule as the normal path below: never come back into a module we just unmapped
		LI_FN( FreeLibraryAndExitThread )( static_cast< HMODULE >( instance ), 0 );

		return 0;
	}

	boot_crumb( "config done, hooks next" );

	LI_FN( GetWindowsDirectoryA )( g_ctx.m_windows_directory, 64 );
	g_console.print( std::vformat( "windows directory - {:s}", std::make_format_args( g_ctx.m_windows_directory ) ).c_str( ) );

	g_fonts.on_attach( );
	g_console.print( "initialised font file vector" );

	g_render.m_reload_fonts = true;

	g_console.print( "initialising hooks" );
	if ( !g_hooks.on_attach( ) )
		g_console.print< n_console::log_level::WARNING >( "failed to initialise hooks" );
	else
		g_console.print( "initialised hooks" );

	boot_screen_step( );

	boot_crumb( "hooks done, init complete" );

	g_console.print( "initialising media player" );
	g_utilities.create_thread( media_player_thread, nullptr );
	g_utilities.create_thread( visualizer_thread, nullptr );
	g_utilities.create_thread( lyrics_thread, nullptr );

	discord_rpc_start( );

	while ( !g_ctx.m_eject_requested )
		std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );

	boot_crumb( "eject requested, teardown start" );

	boot_screen_eject_begin( );

	g_ctx.m_world_restore_requested = true;

	for ( int i = 0; i < 20 && !g_ctx.m_world_restore_done.load( std::memory_order_acquire ); i++ )
		std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );

	boot_screen_eject_end( );

	/* flag first, then a breath: every detour now passes straight to the original, nothing draws into freed buffers */
	g_ctx.m_unloading         = true;
	g_image_cache.m_shutdown  = true;
	g_moi_hud.m_shutdown      = true;

	std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );

	for ( int i = 0; i < 20 && !g_ctx.m_render_release_done.load( std::memory_order_acquire ); i++ )
		std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );

	/* media thread (winrt call), image cache worker (download), font worker (atlas build) must drop out
	   before the module goes, or they return into freed code */
	for ( int i = 0; i < 100 && ( g_media_thread_alive || g_visualizer_thread_alive || g_lyrics_thread_alive || g_image_cache.m_worker_alive || g_moi_hud.m_workers > 0 ||
	                              g_render.m_font_build_running || g_bot_names_fetching || g_discord_rpc_alive || g_image_pick_alive || g_url_image_downloads > 0 ||
	                              n_chat_extras::g_worker_alive );
	      i++ ) {
		image_pick_cancel( ); // every pass: the dialog may not have been up yet on the first one
		std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
	}

	/* the flags drop INSIDE the thread body; the std::thread wrapper after it is ours too, let it walk out */
	std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );

	boot_crumb( "workers down, releasing" );

	g_media_player.on_release( );

	// convars back, hooks off, in-flight detours drained, buffers + materials freed
	g_hooks.on_release( );

	/* NtAllocateVirtualMemory detour is off: give the address space back, a re-inject reserves its own */
	n_va_ballast::release( );

	g_input.on_release( );

	boot_crumb( "released, freeing module" );

	g_console.on_release( );

	const auto net_channel_info = g_interfaces.m_engine_client->get_net_channel_info( );

	if ( g_interfaces.m_client_state && g_interfaces.m_engine_client->is_connected_safe( ) && net_channel_info && !net_channel_info->is_loopback( ) )
		g_interfaces.m_client_state->m_delta_tick = -1;

	/* a detour can sit in a long original (map load): freeing under it = return into unmapped code.
	   queued render calls are our vtables in the game's queue, same rule */
	const auto busy = [ ] { return g_ctx.m_hooks_in_flight.load( std::memory_order_acquire ) > 0 || n_render_queue::pending( ) > 0; };

	for ( int i = 0; i < 3000 && busy( ); i++ )
		std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );

	std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );

	const bool drained = !busy( );
	if ( !drained )
		boot_crumb( n_render_queue::pending( ) > 0 ? "render queue not drained after 30s, module left mapped"
		                                          : "hooks still in flight after 30s, module left mapped" );

	botox_dbg_log_close( );
	n_perf::close_file( );
	crash_witness_remove( );

	if ( !drained )
		ExitThread( 0 );

	LI_FN( FreeLibraryAndExitThread )( static_cast< HMODULE >( instance ), 0 );

	return 0;
}

int __stdcall DllMain( void* instance, unsigned long reason_for_call, void* reserved )
{
	switch ( reason_for_call ) {
	case DLL_PROCESS_ATTACH: {
		boot_crumb( "---- DLL_PROCESS_ATTACH ----" );

#ifdef FART_STUB_TEST
		return TRUE;
#else
		LI_FN( DisableThreadLibraryCalls )( reinterpret_cast< HMODULE >( instance ) );

		crash_witness_install( instance );

		/* first, while the big holes are still there */
		n_va_ballast::reserve( );

		boot_crumb( "spawning init thread" );

		/* RAW CreateThread: a std::thread wrapper runs OUR code after the body returns, and this thread ends in
		   FreeLibraryAndExitThread */
		const HANDLE init_thread = LI_FN( CreateThread )( nullptr, 0, on_attach, instance, 0, nullptr );

		if ( !init_thread ) {
			crash_witness_remove( );
			n_va_ballast::release( );
			return FALSE;
		}

		LI_FN( CloseHandle )( init_thread );

		return TRUE;
#endif
	}
	case DLL_PROCESS_DETACH:
		/* reserved != null = game quit, not eject: every other thread is already dead and the CRT runs our static
		   dtors right after this. skip COM releases there (RPC with no thread pool = ntdll c000000d). no locks here */
		if ( reserved )
			wv2::AbandonForExit( );
		crash_witness_remove( );
		break;
	}

	return TRUE;
}
