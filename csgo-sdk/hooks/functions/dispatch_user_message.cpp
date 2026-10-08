#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"
#include "../hooks.h"
#include "../../hacks/misc/chat_extras.h"

#include <cctype>
#include <cstring>

static bool raw_contains( const unsigned char* data, int size, const char* needle )
{
	const int needle_len = static_cast< int >( std::strlen( needle ) );
	const int scan_len   = size < 0x200 ? size : 0x200;
	for ( int i = 0; i + needle_len <= scan_len; ++i ) {
		int j = 0;
		while ( j < needle_len && std::tolower( data[ i + j ] ) == std::tolower( static_cast< unsigned char >( needle[ j ] ) ) )
			++j;
		if ( j == needle_len )
			return true;
	}
	return false;
}

bool __fastcall n_detoured_functions::dispatch_user_message( void* ecx, void* edx, int msg_type, int flags, int size, const void* msg )
{
	static auto original = g_hooks.m_dispatch_user_message.get_original< decltype( &n_detoured_functions::dispatch_user_message ) >( );
	HOOK_SCOPE_OR_BAIL( original( ecx, edx, msg_type, flags, size, msg ) );

	if ( GET_VARIABLE( g_variables.m_hide_y6o_ps, bool ) && msg && size > 0 && msg_type >= 5 && msg_type <= 8 ) {
		const auto data = static_cast< const unsigned char* >( msg );
		if ( raw_contains( data, size, "y6" ) && raw_contains( data, size, "ps assisted" ) )
			return true;
	}

	/* CS_UM_SayText / CS_UM_SayText2 */
	if ( msg_type == 5 || msg_type == 6 )
		n_chat_extras::on_chat_message( msg_type, msg, size );

	return original( ecx, edx, msg_type, flags, size, msg );
}
