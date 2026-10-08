#include "input.h"
#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"
#include "../../hooks/hooks.h"
#include "../../hacks/chud_hud/chud_hud.h"

#include <d3d9.h>
#include <thread>
#include <unordered_map>

void botox_dbg_log( const char* fmt, ... );
extern bool point_popup_visible( );

bool n_input::impl_t::on_attach( )
{
	D3DDEVICE_CREATION_PARAMETERS creation_parameters{ };
	while ( FAILED( g_interfaces.m_direct_device->GetCreationParameters( &creation_parameters ) ) )
		std::this_thread::sleep_for( std::chrono::milliseconds( 200 ) );

	this->m_window = creation_parameters.hFocusWindow;

	if ( !this->m_window )
		return false;

	this->m_old_wnd_proc = reinterpret_cast< WNDPROC >(
		SetWindowLongPtrW( this->m_window, GWLP_WNDPROC, reinterpret_cast< LONG_PTR >( n_detoured_functions::wndproc ) ) );

	if ( !this->m_old_wnd_proc )
		return false;

	return true;
}

void n_input::impl_t::on_release( )
{
	if ( this->m_old_wnd_proc )
		SetWindowLongPtrW( this->m_window, GWLP_WNDPROC, reinterpret_cast< LONG_PTR >( this->m_old_wnd_proc ) );

	g_interfaces.m_input_system->enable_input( true );
}

bool n_input::impl_t::keys_blocked( )
{
	static c_cconvar* mouse_enable = nullptr;
	if ( !mouse_enable )
		mouse_enable = g_convars[ HASH_BT( "cl_mouseenable" ) ];

	const char* why = g_menu.m_opened                ? "menu"
	                  : point_popup_visible( )       ? "point popup"
	                  : g_chud.m_chat_open           ? "chud chat"
	                  : !g_ctx.m_is_window_focused   ? "unfocused"
	                  : g_interfaces.m_engine_client && g_interfaces.m_engine_client->is_console_visible( ) ? "console"
	                  : mouse_enable && !mouse_enable->get_int( ) ? "panorama"
	                                                              : nullptr;

	static const char* last_why = nullptr;
	if ( why != last_why ) {
		last_why = why;
		botox_dbg_log( "KEYS: %s", why ? why : "free" );
	}

	return why != nullptr;
}

bool n_input::impl_t::on_wndproc( unsigned int msg, unsigned int wide_param, long long_param )
{
	int current_key          = 0;
	e_key_state current_state = e_key_state::none;

	switch ( msg ) {
	/* alt-tab: key ups go to the other window, held keys would stick down. no seq bump = no toggle flip */
	case WM_KILLFOCUS:
	case WM_ACTIVATEAPP:
		if ( msg == WM_KILLFOCUS || !wide_param )
			for ( auto& state : this->m_key_state )
				if ( state == e_key_state::down )
					state = e_key_state::up;
		return false;
	case WM_KEYDOWN:
	case WM_SYSKEYDOWN:
		if ( wide_param < 256U ) {
			current_key   = wide_param;
			current_state = e_key_state::down;
		}
		break;
	case WM_KEYUP:
	case WM_SYSKEYUP:
		if ( wide_param < 256U ) {
			current_key   = wide_param;
			current_state = e_key_state::up;
		}
		break;
	case WM_LBUTTONDOWN:
	case WM_LBUTTONUP:
	case WM_LBUTTONDBLCLK:
		current_key   = VK_LBUTTON;
		current_state = msg == WM_LBUTTONUP ? e_key_state::up : e_key_state::down;
		break;
	case WM_RBUTTONDOWN:
	case WM_RBUTTONUP:
	case WM_RBUTTONDBLCLK:
		current_key   = VK_RBUTTON;
		current_state = msg == WM_RBUTTONUP ? e_key_state::up : e_key_state::down;
		break;
	case WM_MBUTTONDOWN:
	case WM_MBUTTONUP:
	case WM_MBUTTONDBLCLK:
		current_key   = VK_MBUTTON;
		current_state = msg == WM_MBUTTONUP ? e_key_state::up : e_key_state::down;
		break;
	case WM_XBUTTONDOWN:
	case WM_XBUTTONUP:
	case WM_XBUTTONDBLCLK:
		current_key   = ( GET_XBUTTON_WPARAM( wide_param ) == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2 );
		current_state = msg == WM_XBUTTONUP ? e_key_state::up : e_key_state::down;
		break;
	default:
		return false;
	}

	/* a press typed into a menu / chat / console never goes down: nothing fires on it or on its late
	   release after the ui closes. releases always land, so a key held into the ui can't stick down */
	if ( current_state == e_key_state::down && current_key != VK_INSERT && current_key != VK_END && this->keys_blocked( ) )
		return false;

	if ( current_state == e_key_state::up && this->m_key_state[ current_key ] == e_key_state::down ) {
		this->m_key_state[ current_key ] = e_key_state::released;
		++this->m_release_seq[ current_key ];
	} else
		this->m_key_state[ current_key ] = current_state;

	return true;
}

bool n_input::impl_t::check_input( key_bind_t* key_data )
{
	const bool blocked = this->keys_blocked( );

	switch ( key_data->m_key_style ) {
	case 0: {
		return g_menu.m_opened ? false : true;
	}
	case 1: {
		return this->is_key_down( key_data->m_key ) && !blocked;
	}
	case 2: {
		/* toggle off the per-KEY release COUNTER (never consumed), so binds sharing a key each see every
		   release once. is_key_released( ) CONSUMES the edge: the first poller would win. */
		static std::unordered_map< key_bind_t*, bool > toggled;
		static std::unordered_map< key_bind_t*, std::uint32_t > last_seen_seq;

		const std::uint32_t seq = this->m_release_seq[ key_data->m_key ];
		const auto [ it, inserted ] = last_seen_seq.try_emplace( key_data, seq );

		if ( !inserted && it->second != seq && !blocked )
			toggled[ key_data ] = !toggled[ key_data ];
		it->second = seq;

		return toggled[ key_data ];
	}
	}

	return false;
}