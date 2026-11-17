#pragma once
/* botox: main font atlas pixels live in ONE address range reserved for the whole session. a reload frees the old
   ~31 mb and asks for the same block again ~4 s later; in a 3 gb 32 bit csgo the heap / game took that hole by then
   ( "pixel alloc failed 4096x7796 | largest free block 28 mb" ) = font change never applied. reserved VA can't be
   taken: give_back decommits ( ram back ), take recommits at the same address. tools/px_slot_check.py */
#ifndef WIN32_LEAN_AND_MEAN
#	define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#	define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <cstddef>

namespace n_px_slot
{
	inline std::atomic< void* > g_base{ nullptr };
	inline std::atomic< bool > g_busy{ false };
	inline std::atomic< size_t > g_reserved{ 0 }; // written by the g_busy holder only

	inline size_t round_up( const size_t bytes ) { return ( bytes + 0xFFFF ) & ~static_cast< size_t >( 0xFFFF ); }

	inline void* reserve( void* const at, const size_t bytes ) { return VirtualAlloc( at, bytes, MEM_RESERVE, PAGE_NOACCESS ); }

	/* zeroed pages ( fresh commit ), or nullptr = busy / no room: caller uses the heap */
	inline void* take( const size_t size )
	{
		bool expect = false;
		if ( !size || !g_busy.compare_exchange_strong( expect, true, std::memory_order_acquire ) )
			return nullptr;

		void* base = g_base.load( std::memory_order_relaxed );

		if ( !base || g_reserved.load( std::memory_order_relaxed ) < size ) {
			/* 1/4 headroom so a bigger font size still fits next time; exact size if that much is not free */
			const size_t roomy = round_up( size + size / 4 ), exact = round_up( size );
			size_t got_size    = roomy;
			void* got          = reserve( nullptr, roomy );
			if ( !got )
				got = reserve( nullptr, got_size = exact );

			/* nothing new while the old range is held: grow in place, only when the space right after it is free. else keep
			   the old range ( this build -> heap ): dropping it in a fragmented process = no slot for the rest of the session */
			if ( !got && base ) {
				const size_t held             = g_reserved.load( std::memory_order_relaxed );
				MEMORY_BASIC_INFORMATION next = { };
				const bool room = VirtualQuery( static_cast< char* >( base ) + held, &next, sizeof( next ) ) && next.State == MEM_FREE &&
				                  held + next.RegionSize >= exact;
				if ( !room ) {
					g_busy.store( false, std::memory_order_release );
					return nullptr;
				}
				VirtualFree( base, 0, MEM_RELEASE );
				if ( !( got = reserve( base, got_size = held + next.RegionSize >= roomy ? roomy : exact ) ) ) {
					got = reserve( base, got_size = held ); // lost the race for the tail: old range back
					g_base.store( got, std::memory_order_relaxed );
					g_reserved.store( got ? got_size : 0, std::memory_order_relaxed );
					g_busy.store( false, std::memory_order_release );
					return nullptr;
				}
			}
			else if ( got && base )
				VirtualFree( base, 0, MEM_RELEASE );

			if ( !got ) {
				g_busy.store( false, std::memory_order_release );
				return nullptr;
			}

			base = got;
			g_reserved.store( got_size, std::memory_order_relaxed );
			g_base.store( base, std::memory_order_release );
		}

		if ( !VirtualAlloc( base, size, MEM_COMMIT, PAGE_READWRITE ) ) {
			g_busy.store( false, std::memory_order_release );
			return nullptr;
		}
		return base;
	}

	/* true = p was the slot: pages decommitted, range kept */
	inline bool give_back( void* const p )
	{
		if ( !p || p != g_base.load( std::memory_order_acquire ) )
			return false;
		VirtualFree( p, 0, MEM_DECOMMIT );
		g_busy.store( false, std::memory_order_release );
		return true;
	}

	/* eject: VirtualAlloc ranges outlive the module, so free it here or every re-inject leaks one */
	inline void release( )
	{
		bool expect = false;
		if ( !g_busy.compare_exchange_strong( expect, true, std::memory_order_acquire ) )
			return; // still handed out = an atlas outlived DestroyContext ( build wait timed out ), leave it mapped
		if ( void* const base = g_base.exchange( nullptr ) )
			VirtualFree( base, 0, MEM_RELEASE );
		g_reserved.store( 0, std::memory_order_relaxed );
		g_busy.store( false, std::memory_order_release );
	}

	inline size_t reserved( ) { return g_reserved.load( std::memory_order_relaxed ); }
} // namespace n_px_slot
