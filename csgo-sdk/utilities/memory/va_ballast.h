#pragma once
/* botox: 32 bit csgo dies on tier0's OUT OF MEMORY abort when a 16 mb ask ( queued render memstack, dlmalloc mmap, panorama / v8 )
   finds no hole that big while 100-300 mb sits free in small pieces ( botox_crash.log 11-19..11-21, largest free 5-15 mb ).
   ballast = address space reserved at inject ( no ram, no commit ), handed back one chunk per reserve that failed for room;
   the NtAllocateVirtualMemory detour then retries into the hole, end_scene re-takes chunks when room comes back.
   no net address space: a game that truly needs > 4 gb still dies, this only stops fragmentation deaths. tools/va_ballast_check.py */
#ifndef WIN32_LEAN_AND_MEAN
#	define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#	define NOMINMAX
#endif
#include <windows.h>

#include <atomic>

namespace n_va_ballast
{
	constexpr int k_chunks         = 3;
	constexpr SIZE_T k_chunk_bytes = 24u << 20;                        // biggest ask seen: 16 mb + dlmalloc header ( 16 mb + 64 kb )
	constexpr long k_no_memory     = static_cast< long >( 0xC0000017 ); // STATUS_NO_MEMORY: no free range that big

	constexpr SIZE_T k_dump_bytes  = 8u << 20; // MiniDumpWriteDump's own heap growth at the oom ( 11-22 dumps were 0 bytes, largest free 0 mb )

	inline std::atomic< void* > g_chunks[ k_chunks ]{ };
	inline std::atomic< void* > g_dump_room{ nullptr };
	inline std::atomic< long > g_rescues{ 0 };
	inline std::atomic< SIZE_T > g_last_ask{ 0 };
	inline std::atomic< bool > g_last_ok{ false };
	inline std::atomic< unsigned long > g_rearm_thread{ 0 }; // its own reserve must not be "rescued" with a chunk

	inline void reserve( )
	{
		for ( auto& chunk : g_chunks )
			if ( !chunk.load( ) )
				chunk.store( VirtualAlloc( nullptr, k_chunk_bytes, MEM_RESERVE, PAGE_NOACCESS ) );
		if ( !g_dump_room.load( ) )
			g_dump_room.store( VirtualAlloc( nullptr, k_dump_bytes, MEM_RESERVE, PAGE_NOACCESS ) );
	}

	/* crash witness, right before the oom dump */
	inline void give_dump_room( )
	{
		if ( void* const base = g_dump_room.exchange( nullptr ) )
			VirtualFree( base, 0, MEM_RELEASE );
	}

	/* rescues spent chunks: take them back once the game freed room again ( load buffers gone, gc ). only where a hole fits two
	   chunks, so the game keeps one chunk of room and the re-take never starts the next oom. only writer of non-null slots */
	inline int rearm( )
	{
		int took = 0;
		g_rearm_thread.store( GetCurrentThreadId( ) );
		for ( auto& chunk : g_chunks ) {
			if ( chunk.load( ) )
				continue;
			void* const probe = VirtualAlloc( nullptr, 2 * k_chunk_bytes, MEM_RESERVE, PAGE_NOACCESS );
			if ( !probe )
				break;
			VirtualFree( probe, 0, MEM_RELEASE );
			if ( void* const base = VirtualAlloc( nullptr, k_chunk_bytes, MEM_RESERVE, PAGE_NOACCESS ) ) {
				chunk.store( base );
				++took;
			}
		}
		g_rearm_thread.store( 0 );
		return took;
	}

	/* the exchange picks who frees: safe on any thread under any lock ( NtFreeVirtualMemory, no heap ) */
	inline bool give_one( )
	{
		for ( auto& chunk : g_chunks )
			if ( void* const base = chunk.exchange( nullptr ) ) {
				VirtualFree( base, 0, MEM_RELEASE );
				return true;
			}
		return false;
	}

	inline void release( )
	{
		while ( give_one( ) ) {
		}
		give_dump_room( );
	}

	inline int left( )
	{
		int n = 0;
		for ( const auto& chunk : g_chunks )
			n += chunk.load( std::memory_order_relaxed ) != nullptr;
		return n;
	}

	/* our process, any address, new range. a fixed address ( conflict ) or a commit in place ( pagefile ) is no room problem.
	   rearm's own probe failing is no oom either */
	inline bool rescuable( const HANDLE process, void* const* base, const ULONG_PTR zero_bits, const SIZE_T* size, const ULONG type )
	{
		return process == GetCurrentProcess( ) && base && !*base && !zero_bits && size && *size && ( type & MEM_RESERVE ) &&
		       g_rearm_thread.load( std::memory_order_relaxed ) != GetCurrentThreadId( );
	}

	/* one chunk back per retry until it fits or none left. again( ) resets the in/out args and calls the original */
	template < class F >
	long retry( long status, const SIZE_T ask, F&& again )
	{
		while ( status == k_no_memory && give_one( ) ) {
			status = again( );
			g_last_ask.store( ask, std::memory_order_relaxed );
			g_last_ok.store( status >= 0, std::memory_order_relaxed );
			g_rescues.fetch_add( 1, std::memory_order_release );
		}
		return status;
	}
} // namespace n_va_ballast
