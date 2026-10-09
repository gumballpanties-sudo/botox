
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

#include <atomic>
#include <cstdarg> // crash witness line builder
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <intrin.h>

void botox_dbg_log( const char* fmt, ... );
void botox_dbg_log_close( );
//1123
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

static long __stdcall crash_witness( EXCEPTION_POINTERS* info )
{
	const auto record         = info->ExceptionRecord;
	const unsigned long code  = record->ExceptionCode;

	/* a heal logs whatever raised it (e06d7363 = c++ throw): that's who would have crashed us */
	const bool healed = header_heal( );

	if ( !healed && code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_STACK_OVERFLOW && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
	     code != EXCEPTION_PRIV_INSTRUCTION && code != EXCEPTION_INT_DIVIDE_BY_ZERO && code != 0xC0000374 && code != 0xC000000D )
		return EXCEPTION_CONTINUE_SEARCH;

	/* some first chance faults get handled (engine probes): 8 per session, one thread at a time */
	if ( InterlockedIncrement( &g_crash_hits ) > 8 || InterlockedExchange( &g_crash_busy, 1 ) )
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
#endif
	crash_append( "\r\n" );

	CreateDirectoryA( "C:\\botox", nullptr );
	const HANDLE file = CreateFileA( "C:\\botox\\botox_crash.log", FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, 0, nullptr );
	if ( file != INVALID_HANDLE_VALUE ) {
		unsigned long written = 0;
		WriteFile( file, g_crash_text, static_cast< unsigned long >( g_crash_len ), &written, nullptr );
		FlushFileBuffers( file );
		CloseHandle( file );
	}

	InterlockedExchange( &g_crash_busy, 0 );
	return EXCEPTION_CONTINUE_SEARCH;
}

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

	g_crash_witness = AddVectoredExceptionHandler( 1, crash_witness );
}

static void crash_witness_remove( )
{
	if ( g_crash_witness )
		RemoveVectoredExceptionHandler( g_crash_witness );

	g_crash_witness = nullptr;
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
		if ( GET_VARIABLE( g_variables.m_media_player, bool ) )
			g_media_player.on_update( );

		g_media_player.update_lyrics( GET_VARIABLE( g_variables.m_media_player, bool ) && GET_VARIABLE( g_variables.m_media_player_lyrics, bool ) );

		for ( int i = 0; i < 4 && !g_ctx.m_unloading; i++ )
			std::this_thread::sleep_for( std::chrono::milliseconds( 250 ) );
	}

	g_media_player.shutdown( );

	g_media_thread_alive = false;

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

	g_console.on_attach( "fart" );

	boot_crumb( "console ready" );

	/* first file log line (missing = init thread never ran). kept split from the vformat so the crumbs tell them apart */
	const std::string attached_line = std::vformat( "attached, module @ {:p}", std::make_format_args( instance ) );

	boot_crumb( "vformat ok" );

	g_console.print( attached_line.c_str( ) );

	boot_crumb( "first log line written" );

	const auto stage = []( const char* name, bool ( *fn )( ), const bool fatal ) -> bool {
		g_console.print( std::vformat( "initialising {:s}", std::make_format_args( name ) ).c_str( ) );

		if ( fn( ) ) {
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

	boot_crumb( "hooks done, init complete" );

	g_console.print( "initialising media player" );
	g_utilities.create_thread( media_player_thread, nullptr );
	g_utilities.create_thread( visualizer_thread, nullptr );

	discord_rpc_start( );

	while ( !g_ctx.m_eject_requested )
		std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );

	boot_crumb( "eject requested, teardown start" );

	g_ctx.m_world_restore_requested = true;

	for ( int i = 0; i < 20 && !g_ctx.m_world_restore_done.load( std::memory_order_acquire ); i++ )
		std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );

	/* flag first, then a breath: every detour now passes straight to the original, nothing draws into freed buffers */
	g_ctx.m_unloading         = true;
	g_image_cache.m_shutdown  = true;
	g_moi_hud.m_shutdown      = true;

	std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );

	for ( int i = 0; i < 20 && !g_ctx.m_render_release_done.load( std::memory_order_acquire ); i++ )
		std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );

	/* media thread (winrt call), image cache worker (download), font worker (atlas build) must drop out
	   before the module goes, or they return into freed code */
	for ( int i = 0; i < 100 && ( g_media_thread_alive || g_visualizer_thread_alive || g_image_cache.m_worker_alive || g_moi_hud.m_workers > 0 ||
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

		boot_crumb( "spawning init thread" );

		/* RAW CreateThread: a std::thread wrapper runs OUR code after the body returns, and this thread ends in
		   FreeLibraryAndExitThread */
		const HANDLE init_thread = LI_FN( CreateThread )( nullptr, 0, on_attach, instance, 0, nullptr );

		if ( !init_thread ) {
			crash_witness_remove( );
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
