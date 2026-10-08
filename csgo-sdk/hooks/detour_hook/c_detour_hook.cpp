#include "c_detour_hook.h"

#include "../../globals/includes/includes.h"

bool c_detour_hook::create( void* function, void* detour, bool enable_now )
{
	this->m_base_fn = function;

	if ( !this->m_base_fn )
		return false;

	this->m_replace_fn = detour;

	if ( !this->m_replace_fn )
		return false;

	const MH_STATUS status = MH_CreateHook( this->m_base_fn, this->m_replace_fn, &this->m_original_fn );

	if ( status != MH_OK ) {
		g_console.print( std::format( "failed to create hook function, status: {:s}\nbase function -> {:p}", MH_StatusToString( status ), m_base_fn ).c_str( ) );
		return false;
	}

	if ( !enable_now )
		return true;

	if ( !this->replace( ) )
		return false;

	return true;
}

bool c_detour_hook::replace( )
{
	if ( !this->m_base_fn )
		return false;

	if ( this->m_is_hooked )
		return false;

	const MH_STATUS status = MH_EnableHook( this->m_base_fn );

	if ( status != MH_OK ) {
		g_console.print( std::format( "failed to enable hook function, status: {:s}\nbase function -> {:p}", MH_StatusToString( status ), m_base_fn ).c_str( ) );
		return false;
	}

	m_is_hooked = true;
	return true;
}
