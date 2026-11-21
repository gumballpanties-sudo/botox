#pragma once
/* botox: 32 bit csgo dies on tier0's OUT OF MEMORY abort when a 16 mb ask ( queued render memstack, dlmalloc mmap, panorama / v8 )
   finds no hole that big while 100-300 mb sits free in small pieces ( botox_crash.log 11-19..11-21, largest free 5-15 mb ).
   ballast = address space reserved at inject ( no ram, no commit ), handed back one chunk per reserve that failed for room;
   the NtAllocateVirtualMemory detour then retries into the hole. tools/va_ballast_check.py */
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

	inline std::atomic< void* > g_chunks[ k_chunks ]{ };
	inline std::atomic< long > g_rescues{ 0 };
	inline std::atomic< SIZE_T > g_last_ask{ 0 };
	inline std::atomic< bool > g_last_ok{ false };

	/* ponytail: 3 rescues per inject, never re-armed. re-take chunks after a level load if long sessions still die */
	inline void reserve( )
	{
		for ( auto& chunk : g_chunks )
			if ( !chunk.load( ) )
				chunk.store( VirtualAlloc( nullptr, k_chunk_bytes, MEM_RESERVE, PAGE_NOACCESS ) );
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
	}

	inline int left( )
	{
		int n = 0;
		for ( const auto& chunk : g_chunks )
			n += chunk.load( std::memory_order_relaxed ) != nullptr;
		return n;
	}

	/* our process, any address, new range. a fixed address ( conflict ) or a commit in place ( pagefile ) is no room problem */
	inline bool rescuable( const HANDLE process, void* const* base, const ULONG_PTR zero_bits, const SIZE_T* size, const ULONG type )
	{
		return process == GetCurrentProcess( ) && base && !*base && !zero_bits && size && *size && ( type & MEM_RESERVE );
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
