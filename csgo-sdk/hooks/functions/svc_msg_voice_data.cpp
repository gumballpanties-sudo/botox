#include "../../globals/globals.h"
#include "../hooks.h"

#include "../../hacks/network/botox_net.h"

bool __fastcall n_detoured_functions::svc_msg_voice_data( void* ecx, void* edx, const void* message )
{
	static auto original = g_hooks.m_svc_msg_voice_data.get_original< decltype( &n_detoured_functions::svc_msg_voice_data ) >( );
	HOOK_SCOPE_OR_BAIL( original( ecx, edx, message ) );

	if ( message && g_botox_net.on_voice_data( message ) )
		return true;

	return original( ecx, edx, message );
}
