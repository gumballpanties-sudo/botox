#include "../../globals/includes/includes.h"
#include "../../utilities/memory/va_ballast.h"
#include "../hooks.h"

/* every reserve in the process lands here ( VirtualAlloc, tier0 dlmalloc, memstacks, heap growth ), on any thread, often under
   the heap or loader lock: no log, no allocation, no static init. only the ballast swap ( utilities/memory/va_ballast.h ) */
long __stdcall n_detoured_functions::nt_allocate_virtual_memory( void* process, void** base, ULONG_PTR zero_bits, SIZE_T* size, unsigned long type,
                                                                 unsigned long protect )
{
	const auto original = g_hooks.m_nt_allocate_virtual_memory.get_original< decltype( &n_detoured_functions::nt_allocate_virtual_memory ) >( );
	HOOK_SCOPE_OR_BAIL( original( process, base, zero_bits, size, type, protect ) );

	/* args read only after a no-room failure: the kernel leaves them as passed then ( a bad pointer fails access violation instead ) */
	const long status = original( process, base, zero_bits, size, type, protect );
	if ( status != n_va_ballast::k_no_memory || !n_va_ballast::rescuable( process, base, zero_bits, size, type ) )
		return status;

	const SIZE_T ask = *size;
	return n_va_ballast::retry( status, ask, [ & ] {
		*base = nullptr;
		*size = ask;
		return original( process, base, zero_bits, size, type, protect );
	} );
}
