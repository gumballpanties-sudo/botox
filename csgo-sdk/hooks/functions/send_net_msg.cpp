#include "../../game/sdk/classes/c_net_message.h"
#include "../../globals/globals.h"
#include "../hooks.h"

#include "../../hacks/network/botox_net.h"

bool __fastcall n_detoured_functions::send_net_msg( void* ecx, void* edx, c_net_message* message, bool force_reliable, bool voice )
{
	static auto original = g_hooks.m_send_net_msg.get_original< decltype( &n_detoured_functions::send_net_msg ) >( );
	HOOK_SCOPE_OR_BAIL( original( ecx, edx, message, force_reliable, voice ) );

	const int type = message->get_type( );

	if ( type == 14  )
		return false;

	/* net_StringCmd */
	if ( type == 5 )
		g_botox_net.on_string_cmd( message );

	/* clc_VoiceData */
	if ( type == 10 )
		g_botox_net.on_voice_send( message );

	return original( ecx, edx, message, force_reliable, voice );
}
