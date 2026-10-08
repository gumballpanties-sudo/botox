#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"
#include "../hooks.h"

#undef max

#include <algorithm>

int __fastcall n_detoured_functions::send_datagram( c_net_channel* net_channel, int edx, bf_write* datagram )
{
	static auto original = g_hooks.m_send_datagram.get_original< decltype( &n_detoured_functions::send_datagram ) >( );
	HOOK_SCOPE_OR_BAIL( original( net_channel, edx, datagram ) );

	auto nci = g_interfaces.m_engine_client->get_net_channel_info( );

	if ( net_channel != g_interfaces.m_client_state->m_net_channel )
		return original( net_channel, edx, datagram );

	if ( g_lagcomp.is_hosting( ) ) {
		g_ctx.m_extend_applied = 0.f;
		return original( net_channel, edx, datagram );
	}

	if ( !g_interfaces.m_engine_client->is_connected_safe( ) || datagram || !nci ) {
		g_ctx.m_extend_applied = 0.f;
		return original( net_channel, edx, datagram );
	}

	const int old_in_reliable_state = net_channel->m_in_reliable_state;
	const int old_in_sequence       = net_channel->m_in_sequence_nr;

	const float extend = std::max( 0.f, g_lagcomp.extend( ) - nci->get_latency( FLOW_OUTGOING ) );
	if ( extend <= 0.f ) {
		g_ctx.m_extend_applied = 0.f;
		return original( net_channel, edx, datagram );
	}

	g_lagcomp.add_latency_to_net_channel( net_channel, extend );

	const int result = original( net_channel, edx, datagram );

	net_channel->m_in_reliable_state = old_in_reliable_state;
	net_channel->m_in_sequence_nr    = old_in_sequence;

	return result;
}
