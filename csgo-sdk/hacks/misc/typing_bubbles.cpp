#include "misc.h"

#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"
#include "../chud_hud/chud_hud.h"
#include "../cod4_hud/cod4_hud.h"
#include "../gta_hud/gta_hud.h"
#include "../mc_hud/mc_hud.h"
#include "../network/botox_net.h"
#include "../urt_hud/urt_hud.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

namespace
{
	std::atomic< unsigned long long > g_opener_ms{ 0ull };

	/* 3-dots-bounce.svg: r 3 dots at x 4 / 12 / 20, begin 0.1 s apart, hop 6 px over 0.6 s ( both keySplines = one parabola ), loop 1.05 s */
	constexpr float k_period = 1.05f;

	float hop( const float t )
	{
		const float v = t / 0.3f - 1.f;
		return t >= 0.f && t <= 0.6f ? 6.f * ( 1.f - v * v ) : 0.f;
	}
} // namespace

void typing_bubbles_on_key( const unsigned int msg, const long long_param )
{
	if ( msg != WM_KEYDOWN || !GET_VARIABLE( g_variables.m_typing_bubbles, bool ) || !g_interfaces.m_engine_client || !g_interfaces.m_input_system ||
	     !g_interfaces.m_engine_client->is_in_game( ) || g_input.keys_blocked( ) )
		return;

	const char* bind = g_interfaces.m_engine_client->key_binding_for_key( g_interfaces.m_input_system->scan_code_to_button_code( long_param ) );
	if ( bind && std::strstr( bind, "messagemode" ) )
		g_opener_ms.store( GetTickCount64( ), std::memory_order_relaxed );
}

bool typing_bubbles_local( )
{
	if ( g_chud.m_chat_open || g_mc_hud.m_chat_open || g_cod4_hud.m_chat_open || g_urt_hud.m_chat_open || g_gta_hud.m_chat_open )
		return true;

	unsigned long long opened = g_opener_ms.load( std::memory_order_relaxed );
	if ( !opened )
		return false;

	/* native chat zeroes cl_mouseenable while up ( csgo panoramaenginehandler ), so do buy / pause menus: only after our opener */
	static c_cconvar* mouse_enable = nullptr;
	if ( !mouse_enable )
		mouse_enable = g_convars[ HASH_BT( "cl_mouseenable" ) ];
	if ( mouse_enable && !mouse_enable->get_int( ) )
		return true;

	if ( GetTickCount64( ) - opened > 500ull )
		g_opener_ms.compare_exchange_strong( opened, 0ull, std::memory_order_relaxed );
	return false;
}

void typing_bubbles_paint( )
{
	if ( !GET_VARIABLE( g_variables.m_typing_bubbles, bool ) || !GET_VARIABLE( g_variables.m_botox_network, bool ) ||
	     GET_VARIABLE( g_variables.m_safe_mode, bool ) || !g_interfaces.m_engine_client->is_in_game( ) )
		return;

	const int local        = g_interfaces.m_engine_client->get_local_player( );
	const bool local_typing = g_interfaces.m_input && g_interfaces.m_input->m_camera_in_third_person && typing_bubbles_local( );
	const float scale       = std::clamp( GET_VARIABLE( g_variables.m_typing_bubbles_size, float ), 0.5f, 4.f ) * 0.5f;
	const unsigned int rgba = GET_VARIABLE( g_variables.m_typing_bubbles_color, c_color ).get_u32( );
	const float phase       = std::fmod( g_interfaces.m_global_vars_base->m_real_time, k_period );

	for ( int i = 1; i <= 64; i++ ) {
		if ( i == local ? !local_typing : !g_botox_net.typing( i ) )
			continue;

		const auto player = g_interfaces.m_client_entity_list->get< c_base_entity >( i );
		if ( !player || player->is_dormant( ) || !player->is_alive( ) )
			continue;

		/* hull top 72 standing, 54 ducked */
		const float top   = 72.f - 18.f * std::clamp( player->get_duck_amount( ), 0.f, 1.f );
		const c_vector at = player->get_abs_origin( ) + c_vector( 0.f, 0.f, top + 8.f + 3.f * scale );

		for ( int dot = 0; dot < 3; dot++ ) {
			const float x = static_cast< float >( dot - 1 ) * 8.f / 3.f;
			const float y = hop( phase - 0.1f * static_cast< float >( dot ) ) / 3.f;
			kill_effects_disc( at, 3.f * scale, x - 1.f, y - 1.f, x + 1.f, y + 1.f, rgba );
		}
	}
}
