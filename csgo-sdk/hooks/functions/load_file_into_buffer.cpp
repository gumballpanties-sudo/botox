#include "../../globals/includes/includes.h"
#include "../../hacks/misc/scaleform/moi_hud.h"
#include "../hooks.h"

/* CSource2UIFileSystem::LoadFileIntoBuffer (IUIFileSystem slot 0): every panorama layout / style / image read.
   moi hud swaps a stock hud path for its served copy; a failed read of that copy falls back to stock */
bool __fastcall n_detoured_functions::load_file_into_buffer( void* ecx, void* edx, const char* file, void* buffer, bool text,
                                                             void* change_callback, unsigned int padding )
{
	static auto original = g_hooks.m_load_file_into_buffer.get_original< decltype( &n_detoured_functions::load_file_into_buffer ) >( );
	HOOK_SCOPE_OR_BAIL( original( ecx, edx, file, buffer, text, change_callback, padding ) );

	if ( const char* served = g_moi_hud.redirect( file ) ) {
		if ( original( ecx, edx, served, buffer, text, change_callback, padding ) )
			return true;

		g_moi_hud.redirect_failed( file, served );
	}

	return original( ecx, edx, file, buffer, text, change_callback, padding );
}
