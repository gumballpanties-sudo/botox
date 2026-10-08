#include "../../globals/includes/includes.h"
#include "../hooks.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

static const char* const WARNING_SPAM[] = {
	"too many stacking levels",
};

void __cdecl n_detoured_functions::tier0_warning( const char* format, ... )
{
	static auto original = g_hooks.m_tier0_warning.get_original< void( __cdecl* )( const char*, ... ) >( );
	const n_ctx::hook_scope_t hook_scope_guard;

	if ( !format )
		return;

	char buffer[ 1024 ]{ };

	va_list args;
	va_start( args, format );
	std::vsnprintf( buffer, sizeof( buffer ), format, args );
	va_end( args );

	for ( const char* spam : WARNING_SPAM ) {
		if ( std::strstr( buffer, spam ) )
			return;
	}

	original( "%s", buffer );
}
