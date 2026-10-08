#include "mc_hud.h"
#include "mc_assets.h"
#include "mc_logic.h"

#include "../../dependencies/imgui/imgui.h"
#include "../menu/menu.h"
#include "../../utilities/console/console.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <cwctype>
#include <deque>
#include <string_view>

extern void botox_dbg_log( const char* fmt, ... );
extern bool point_menu_is_opened( );

using namespace n_mc_assets;
using n_mc_logic::text_t;

bool n_mc_hud::enabled( ) { return GET_VARIABLE( g_variables.m_mc_hud, bool ); }

namespace
{
	double now_seconds( )
	{
		static const auto start = std::chrono::steady_clock::now( );
		return std::chrono::duration< double >( std::chrono::steady_clock::now( ) - start ).count( );
	}

	// MC runs its gui at 20 ticks a second
	long long mc_tick( ) { return static_cast< long long >( now_seconds( ) * 20.0 ); }
	long long now_ms( ) { return static_cast< long long >( now_seconds( ) * 1000.0 ); }

	struct line_t {
		text_t m_text{ };
		long long m_tick = 0;
		int m_message    = 0;
	};

	// guarded by g_mc_hud.m_lock
	std::deque< line_t > s_lines{ };
	int s_next_message = 0;

	void push_line( const text_t& text, bool replace_last )
	{
		const long long tick = mc_tick( );

		std::lock_guard< std::mutex > lock( g_mc_hud.m_lock );

		int id = s_next_message;
		if ( replace_last && !s_lines.empty( ) ) {
			id = s_lines.back( ).m_message;
			while ( !s_lines.empty( ) && s_lines.back( ).m_message == id )
				s_lines.pop_back( );
		}
		else
			++s_next_message;

		// ChatComponent.addMessage: wrapped at chat width 320, newest last, 100 messages kept
		for ( auto& line : n_mc_logic::wrap( text, 320 ) )
			s_lines.push_back( { std::move( line ), tick, id } );

		while ( !s_lines.empty( ) && s_lines.front( ).m_message < s_next_message - 100 )
			s_lines.pop_front( );
	}

	/* ---- render ---- */

	struct texture_t {
		IDirect3DTexture9* m_texture = nullptr;
		float m_u = 1.f, m_v = 1.f; // sprite size / texture size, d3dx may pad
		bool m_tried = false;
	};

	std::array< texture_t, sprite_max > s_textures{ };

	const texture_t* texture( int sprite )
	{
		if ( sprite < 0 || sprite >= sprite_max || !g_interfaces.m_direct_device )
			return nullptr;

		texture_t& t = s_textures[ sprite ];
		if ( !t.m_tried ) {
			t.m_tried = true;

			const sprite_t& s = k_sprites[ sprite ];

			// csgo runs d3d9ex: MANAGED is refused there, DEFAULT + DYNAMIC instead (freed before Reset, reset.cpp)
			const auto load = [ & ]( DWORD usage, D3DPOOL pool ) {
				return D3DXCreateTextureFromFileInMemoryEx( g_interfaces.m_direct_device, s.m_png, s.m_size, D3DX_DEFAULT_NONPOW2, D3DX_DEFAULT_NONPOW2,
				                                            1, usage, D3DFMT_A8R8G8B8, pool, D3DX_FILTER_NONE, D3DX_FILTER_NONE, 0, nullptr, nullptr,
				                                            &t.m_texture );
			};

			const HRESULT managed = load( 0, D3DPOOL_MANAGED );
			const HRESULT result  = SUCCEEDED( managed ) ? managed : load( D3DUSAGE_DYNAMIC, D3DPOOL_DEFAULT );

			if ( SUCCEEDED( result ) && t.m_texture ) {
				if ( sprite == sprite_hotbar ) {
					const char* const line = SUCCEEDED( managed ) ? "MC: textures in managed pool" : "MC: textures in default pool (d3d9ex)";
					botox_dbg_log( "%s", line );
					g_console.print( line );
				}

				D3DSURFACE_DESC desc{ };
				if ( SUCCEEDED( t.m_texture->GetLevelDesc( 0, &desc ) ) && desc.Width && desc.Height ) {
					t.m_u = static_cast< float >( s.m_w ) / static_cast< float >( desc.Width );
					t.m_v = static_cast< float >( s.m_h ) / static_cast< float >( desc.Height );
				}
			}
			else {
				t.m_texture = nullptr;
				char line[ 96 ]{ };
				sprintf_s( line, "MC: texture %d failed managed=%08lx default=%08lx", sprite, managed, result );
				botox_dbg_log( "%s", line );
				g_console.print( line );
			}
		}

		return t.m_texture ? &t : nullptr;
	}

	// gui units -> screen px, scale = MC gui scale (integer, so every texel lands on whole pixels)
	struct canvas_t {
		ImDrawList* m_list = nullptr;
		float m_s          = 1.f;

		ImVec2 p( float x, float y ) const { return ImVec2( x * m_s, y * m_s ); }
	};

	ImU32 argb( int alpha, std::uint32_t rgb )
	{
		return IM_COL32( ( rgb >> 16 ) & 0xFF, ( rgb >> 8 ) & 0xFF, rgb & 0xFF, std::clamp( alpha, 0, 255 ) );
	}

	void blit( const canvas_t& c, int sprite, float x, float y, float w, float h, ImU32 tint = IM_COL32_WHITE, float u0 = 0.f, float v0 = 0.f,
	           float u1 = 1.f, float v1 = 1.f )
	{
		const texture_t* const t = texture( sprite );
		if ( !t )
			return;

		c.m_list->AddImage( reinterpret_cast< ImTextureID >( t->m_texture ), c.p( x, y ), c.p( x + w, y + h ), ImVec2( u0 * t->m_u, v0 * t->m_v ),
		                    ImVec2( u1 * t->m_u, v1 * t->m_v ), tint );
	}

	void fill( const canvas_t& c, float x0, float y0, float x1, float y1, ImU32 color ) { c.m_list->AddRectFilled( c.p( x0, y0 ), c.p( x1, y1 ), color ); }

	// Font.drawInBatch: shadow first at +1 with rgb * 0.25, then the glyphs
	void text( const canvas_t& c, const text_t& t, float x, float y, int alpha, bool shadow )
	{
		for ( int pass = shadow ? 0 : 1; pass < 2; ++pass ) {
			const float offset = pass == 0 ? 1.f : 0.f;
			float pen          = x + offset;

			for ( size_t i = 0; i < t.size( ); ++i ) {
				const std::uint32_t cp = t.m_cp[ i ];
				const int advance      = n_mc_logic::advance( cp );

				if ( cp != ' ' && advance > 0 ) {
					if ( const glyph_t* const g = n_mc_logic::glyph( cp ) ) {
						std::uint32_t rgb = t.m_rgb[ i ];
						if ( pass == 0 )
							rgb = ( ( rgb >> 16 & 0xFF ) / 4 ) << 16 | ( ( rgb >> 8 & 0xFF ) / 4 ) << 8 | ( rgb & 0xFF ) / 4;

						const sprite_t& sheet = k_sprites[ g->m_sprite ];
						blit( c, g->m_sprite, pen, y + offset + g->m_top, g->m_w * g->m_scale, g->m_h * g->m_scale, argb( alpha, rgb ),
						      static_cast< float >( g->m_x ) / sheet.m_w, static_cast< float >( g->m_y ) / sheet.m_h,
						      static_cast< float >( g->m_x + g->m_w ) / sheet.m_w, static_cast< float >( g->m_y + g->m_h ) / sheet.m_h );
					}
				}

				pen += static_cast< float >( advance );
			}
		}
	}

	void item( const canvas_t& c, const n_mc_hud::slot_t& slot, float x, float y )
	{
		if ( slot.m_sprite < 0 )
			return;

		if ( slot.m_sprite == sprite_tnt_top ) {
			for ( const face_t& face : k_tnt_faces ) {
				const texture_t* const t = texture( face.m_sprite );
				if ( !t )
					continue;

				const int shade = static_cast< int >( face.m_shade * 255.f + 0.5f );
				const auto at   = [ & ]( int k ) { return c.p( x + face.m_corner[ k ][ 0 ], y + face.m_corner[ k ][ 1 ] ); };
				const auto uv   = [ & ]( int k ) { return ImVec2( face.m_corner[ k ][ 2 ] * t->m_u, face.m_corner[ k ][ 3 ] * t->m_v ); };

				c.m_list->AddImageQuad( reinterpret_cast< ImTextureID >( t->m_texture ), at( 0 ), at( 1 ), at( 2 ), at( 3 ), uv( 0 ), uv( 1 ), uv( 2 ),
				                        uv( 3 ), IM_COL32( shade, shade, shade, 255 ) );
			}
		}
		else if ( slot.m_sprite == sprite_splash_potion ) {
			// splash_potion.json: layer0 potion_overlay tinted, layer1 bottle
			blit( c, sprite_potion_overlay, x, y, 16.f, 16.f, argb( 255, slot.m_tint ) );
			blit( c, sprite_splash_potion, x, y, 16.f, 16.f );
		}
		else
			blit( c, slot.m_sprite, x, y, 16.f, 16.f );

		// GuiGraphics.renderItemCount: only when count != 1
		if ( slot.m_count > 1 ) {
			const text_t count = n_mc_logic::plain( std::to_string( slot.m_count ) );
			text( c, count, x + 19 - 2 - n_mc_logic::width( count ), y + 6 + 3, 255, true );
		}
	}

	void cb_point( const ImDrawList*, const ImDrawCmd* )
	{
		IDirect3DDevice9* const device = g_interfaces.m_direct_device;
		device->SetSamplerState( 0, D3DSAMP_MINFILTER, D3DTEXF_POINT );
		device->SetSamplerState( 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT );
		device->SetSamplerState( 0, D3DSAMP_MIPFILTER, D3DTEXF_NONE );
	}

	// RenderPipelines CROSSHAIR: blend ONE_MINUS_DST_COLOR, ONE_MINUS_SRC_COLOR. alpha test drops the clear texels
	void cb_invert( const ImDrawList*, const ImDrawCmd* )
	{
		IDirect3DDevice9* const device = g_interfaces.m_direct_device;
		device->SetRenderState( D3DRS_SEPARATEALPHABLENDENABLE, FALSE );
		device->SetRenderState( D3DRS_SRCBLEND, D3DBLEND_INVDESTCOLOR );
		device->SetRenderState( D3DRS_DESTBLEND, D3DBLEND_INVSRCCOLOR );
		device->SetRenderState( D3DRS_ALPHATESTENABLE, TRUE );
		device->SetRenderState( D3DRS_ALPHAFUNC, D3DCMP_GREATER );
		device->SetRenderState( D3DRS_ALPHAREF, 0 );
	}

	/* ---- game thread helpers ---- */

	std::string_view short_name( const c_weapon_data* data )
	{
		std::string_view name = data->m_weapon_name ? data->m_weapon_name : "";
		if ( name.substr( 0, 7 ) == "weapon_" )
			name.remove_prefix( 7 );
		return name;
	}

	std::string localized( const c_weapon_data* data )
	{
		if ( data->m_hud_name && g_interfaces.m_localize ) {
			if ( const wchar_t* const name = g_interfaces.m_localize->find_safe( data->m_hud_name ); name && *name && *name != L'#' )
				return g_utilities.to_utf8( name );
		}

		return std::string( short_name( data ) );
	}

	std::string player_name( int index )
	{
		player_info_t info{ };
		if ( index > 0 && g_interfaces.m_engine_client->get_player_info( index, &info ) )
			return std::string( info.m_name, strnlen( info.m_name, sizeof( info.m_name ) ) );
		return { };
	}

	// MobEffects colours, splash_potion.json default -13083194 (water) for anything else
	std::uint32_t potion_tint( std::string_view name )
	{
		if ( name == "weapon_hegrenade" )
			return 0xA9656A; // instant_damage
		if ( name == "weapon_flashbang" )
			return 0xF6F6F6; // invisibility
		if ( name == "weapon_smokegrenade" )
			return 0x484D48; // weakness
		if ( name == "weapon_molotov" || name == "weapon_incgrenade" )
			return 0xFF9900; // fire_resistance
		return 0x385DC6;
	}

	// csgo's own grenade cycle order, the stack shows the first one when no grenade is held
	int grenade_rank( std::string_view name )
	{
		static constexpr std::string_view k_order[] = { "weapon_hegrenade", "weapon_flashbang", "weapon_smokegrenade", "weapon_decoy",
			                                            "weapon_molotov",   "weapon_incgrenade" };
		for ( int i = 0; i < static_cast< int >( std::size( k_order ) ); ++i )
			if ( k_order[ i ] == name )
				return i;
		return 50;
	}
}

/* ---- game thread ---- */

void n_mc_hud::impl_t::drain_chat( bool active )
{
	const bool ui_input = g_menu.m_opened || point_menu_is_opened( );

	bool send = false, team = false;
	std::string message{ };

	{
		std::scoped_lock lock( m_chat_mutex );

		m_chat_drain_ms = GetTickCount64( );

		if ( m_chat_open && !m_chat_result && ( !active || ui_input || !g_ctx.m_is_window_focused ) )
			m_chat_result = 2;

		if ( m_chat_open && m_chat_result ) {
			send    = m_chat_result == 1;
			team    = m_chat_team;
			message = g_utilities.to_utf8( m_chat_input );

			m_chat_open      = false;
			m_chat_result    = 0;
			m_chat_open_char = 0;
			m_chat_input.clear( );
		}
	}

	std::erase( message, '"' );
	std::erase( message, ';' );

	if ( send && !message.empty( ) ) {
		const std::string command = std::string( team ? "say_team \"" : "say \"" ) + message + "\"";
		g_interfaces.m_engine_client->client_cmd_unrestricted( command.c_str( ) );
	}
}

void n_mc_hud::impl_t::on_paint_traverse( )
{
	const bool on = n_mc_hud::enabled( );
	if ( on != m_was_enabled ) {
		botox_dbg_log( "MC: hud %s", on ? "on" : "off" );
		m_was_enabled = on;
	}

	auto* const engine = g_interfaces.m_engine_client;
	const bool in_game = engine && engine->is_in_game( ) && g_ctx.m_local;

	// hud_weapon.cpp:77 (classic) + the panorama reticle both stop on `crosshair 0`
	const bool hide_crosshair = on && in_game && GET_VARIABLE( g_variables.m_mc_hud_crosshair, bool );
	if ( hide_crosshair != m_crosshair_hidden ) {
		if ( hide_crosshair ) {
			const int current = g_convars.int_or( HASH_BT( "crosshair" ), 1 );
			// ponytail: crosshair is archived, a 0 here is our own leftover from quitting while on, put back 1
			m_saved_crosshair = current ? current : 1;
			g_convars.set_if_present( HASH_BT( "crosshair" ), 0.f );
		}
		else
			g_convars.set_if_present( HASH_BT( "crosshair" ), static_cast< float >( m_saved_crosshair ) );

		m_crosshair_hidden = hide_crosshair;
		botox_dbg_log( "MC: crosshair %s, saved %d", hide_crosshair ? "hidden" : "restored", m_saved_crosshair );
	}

	drain_chat( on && in_game );

	snapshot_t s{ };
	const int draw_hud = g_convars.int_or( HASH_BT( "cl_drawhud" ), 1 );
	s.m_in_game        = on && in_game && draw_hud != 0;

	{
		const bool engine_in_game = engine && engine->is_in_game( );
		const int state = static_cast< int >( on ) | static_cast< int >( engine_in_game ) << 1 | static_cast< int >( g_ctx.m_local != nullptr ) << 2 |
		                  static_cast< int >( draw_hud != 0 ) << 3 | static_cast< int >( GET_VARIABLE( g_variables.m_mc_hud_crosshair, bool ) ) << 4 |
		                  static_cast< int >( g_ctx.m_local && g_ctx.m_local->is_alive( ) ) << 5;
		static int s_state = -1;
		if ( state != s_state ) {
			s_state = state;
			char line[ 160 ]{ };
			sprintf_s( line, "MC: state on=%d in_game=%d local=%d drawhud=%d xhair_opt=%d alive=%d", on, engine_in_game, g_ctx.m_local != nullptr,
			           draw_hud, ( state >> 4 ) & 1, ( state >> 5 ) & 1 );
			botox_dbg_log( "%s", line );
			g_console.print( line );
		}
	}

	if ( s.m_in_game ) {
		c_base_entity* const local = g_ctx.m_local;

		s.m_alive         = local->is_alive( );
		s.m_scoped        = local->is_scoped( );
		s.m_health        = n_mc_logic::to_mc( std::clamp( local->get_health( ), 0, 100 ) );
		s.m_armor         = n_mc_logic::to_mc( std::clamp( local->get_armor( ), 0, 100 ) );
		s.m_money         = std::max( local->get_money( ), 0 );
		s.m_weapon_handle = local->get_active_weapon_handle( );

		static const unsigned int ammo_offset = g_netvars[ HASH_BT( "CBasePlayer->m_iAmmo" ) ].m_offset;
		static const unsigned int type_offset = g_netvars[ HASH_BT( "CBaseCombatWeapon->m_iPrimaryAmmoType" ) ].m_offset;

		static int last_selected = 0;
		s.m_selected             = last_selected;

		int grenades = 0, grenade_shown = 99;
		c_base_entity* active_weapon  = nullptr;
		const c_weapon_data* active_data = nullptr;

		for ( int i = 0; i < 64; ++i ) {
			const unsigned int handle = local->get_weapons_handle( )[ i ];
			if ( !handle || handle == 0xFFFFFFFF )
				continue;

			auto* const weapon = g_interfaces.m_client_entity_list->get< c_base_entity >( handle );
			if ( !weapon )
				continue;

			const c_weapon_data* const data = g_interfaces.m_weapon_system->get_weapon_data( weapon->get_item_definition_index( ) );
			if ( !data || !data->m_weapon_name )
				continue;

			const std::string_view name = data->m_weapon_name;
			const bool active           = handle == s.m_weapon_handle;

			switch ( data->m_slot ) {
			case 0: s.m_slots[ 0 ].m_sprite = weapon->get_ammo( ) > 0 ? sprite_crossbow_arrow : sprite_crossbow_standby; break;
			case 1: s.m_slots[ 1 ].m_sprite = sprite_bow; break;
			case 2: s.m_slots[ 2 ].m_sprite = sprite_diamond_sword; break;
			case 3: {
				int count = 1;
				if ( ammo_offset && type_offset ) {
					const int type = *reinterpret_cast< int* >( reinterpret_cast< std::uintptr_t >( weapon ) + type_offset );
					if ( type >= 0 && type < 32 )
						count = std::max( 1, *reinterpret_cast< int* >( reinterpret_cast< std::uintptr_t >( local ) + ammo_offset + type * 4 ) );
				}
				grenades += count;

				const int rank = active ? -1 : grenade_rank( name );
				if ( rank < grenade_shown ) {
					grenade_shown           = rank;
					s.m_slots[ 3 ].m_tint   = potion_tint( name );
					s.m_slots[ 3 ].m_sprite = sprite_splash_potion;
				}
				break;
			}
			case 4: s.m_slots[ 4 ].m_sprite = sprite_tnt_top; break;
			default: break;
			}

			if ( active ) {
				active_weapon = weapon;
				active_data   = data;
				if ( data->m_slot >= 0 && data->m_slot <= 4 )
					s.m_selected = last_selected = data->m_slot;
			}
		}

		s.m_slots[ 3 ].m_count = grenades;

		if ( active_weapon && active_data ) {
			s.m_item_name = localized( active_data );

			if ( active_data->m_max_clip1 > 0 ) {
				// slot 9 = clip, offhand = reserve, both arrows
				if ( const int clip = active_weapon->get_ammo( ); clip > 0 )
					s.m_slots[ 8 ] = { sprite_arrow, clip, 0 };
				if ( const int reserve = active_weapon->get_ammo_reserve( ); reserve > 0 )
					s.m_offhand = { sprite_arrow, reserve, 0 };
			}
		}
	}

	std::lock_guard< std::mutex > lock( m_lock );
	m_snapshot = std::move( s );
}

void n_mc_hud::impl_t::on_player_death( int attacker_index, int victim_index, const char* weapon )
{
	if ( !n_mc_hud::enabled( ) )
		return;

	const std::string victim = player_name( victim_index );
	if ( victim.empty( ) )
		return;

	const std::string_view event_weapon = weapon ? weapon : "";
	const std::string attacker          = attacker_index != victim_index ? player_name( attacker_index ) : std::string( );

	// "[AK-47]": the killer's held weapon when it is the one in the event (thrown grenades never are)
	std::string item = std::string( event_weapon );
	if ( auto* const killer = g_interfaces.m_client_entity_list->get< c_base_entity >( attacker_index ); killer && !attacker.empty( ) ) {
		if ( auto* const held = g_interfaces.m_client_entity_list->get< c_base_entity >( killer->get_active_weapon_handle( ) ) ) {
			if ( const c_weapon_data* const data = g_interfaces.m_weapon_system->get_weapon_data( held->get_item_definition_index( ) );
			     data && data->m_weapon_name ) {
				const std::string_view held_name = short_name( data );
				const bool melee = event_weapon.substr( 0, 5 ) == "knife" || event_weapon == "bayonet";
				if ( held_name == event_weapon || ( melee && data->m_slot == 2 && held_name != "taser" ) )
					item = localized( data );
			}
		}
	}

	push_line( n_mc_logic::plain( n_mc_logic::death_message( victim, attacker, event_weapon, item ) ), false );
}

void n_mc_hud::impl_t::on_chat( const std::string& html, bool replace_last )
{
	if ( !n_mc_hud::enabled( ) || html.empty( ) )
		return;

	push_line( n_mc_logic::from_html( html ), replace_last );
}

void n_mc_hud::impl_t::on_notice( const char* text, int client, bool formatted )
{
	if ( !n_mc_hud::enabled( ) || !text || !*text )
		return;

	if ( formatted )
		return push_line( n_mc_logic::from_html( text ), false );

	int team = 0;
	if ( client > 0 )
		if ( auto* const sender = g_interfaces.m_client_entity_list->get< c_base_entity >( client ) )
			team = static_cast< int >( sender->get_team( ) );

	push_line( n_mc_logic::from_notice( text, team ), false );
}

void n_mc_hud::impl_t::on_release( )
{
	if ( m_crosshair_hidden ) {
		g_convars.set_if_present( HASH_BT( "crosshair" ), static_cast< float >( m_saved_crosshair ) );
		m_crosshair_hidden = false;
	}
}

/* ---- window thread ---- */

bool n_mc_hud::impl_t::chat_on_key( unsigned int msg, unsigned int wide_param, long long_param, bool ui_input )
{
	// no WM_SYSKEY*: alt+f4 / alt+tab keep working
	if ( msg != WM_KEYDOWN && msg != WM_KEYUP && msg != WM_CHAR )
		return false;

	if ( ui_input || wide_param == VK_INSERT )
		return false;

	std::scoped_lock lock( m_chat_mutex );

	// nothing drains the line (paint_traverse stopped): drop it, never trap the keyboard
	const bool stale = GetTickCount64( ) - m_chat_drain_ms > 1000ull;
	if ( m_chat_open && stale ) {
		m_chat_open      = false;
		m_chat_result    = 0;
		m_chat_open_char = 0;
		m_chat_input.clear( );
	}

	// releases always reach the game, a key held while opening would stick otherwise
	if ( msg == WM_KEYUP )
		return false;

	if ( !m_chat_open ) {
		if ( msg != WM_KEYDOWN || stale || !n_mc_hud::enabled( ) )
			return false;

		if ( !g_interfaces.m_engine_client || !g_interfaces.m_input_system || !g_interfaces.m_engine_client->is_in_game( ) ||
		     g_interfaces.m_engine_client->is_console_visible( ) )
			return false;

		const char* bind = g_interfaces.m_engine_client->key_binding_for_key( g_interfaces.m_input_system->scan_code_to_button_code( long_param ) );
		if ( !bind || !std::strstr( bind, "messagemode" ) )
			return false;

		m_chat_open      = true;
		m_chat_team      = std::strstr( bind, "messagemode2" ) != nullptr;
		m_chat_result    = 0;
		m_chat_open_ms   = GetTickCount64( );
		m_chat_open_char = static_cast< wchar_t >( std::towlower( MapVirtualKeyW( wide_param, MAPVK_VK_TO_CHAR ) & 0x7FFF ) );
		m_chat_input.clear( );
		return true;
	}

	if ( msg == WM_CHAR ) {
		const wchar_t character = static_cast< wchar_t >( wide_param );

		// the opening key's own WM_CHAR
		if ( m_chat_open_char ) {
			const wchar_t opener = m_chat_open_char;
			m_chat_open_char     = 0;

			if ( character == opener || character == static_cast< wchar_t >( opener - 32 ) )
				return true;
		}

		// ChatScreen: EditBox max length 256
		if ( character >= L' ' && character != 0x7F && m_chat_input.size( ) < 256U )
			m_chat_input.push_back( character );

		return true;
	}

	switch ( wide_param ) {
	case VK_RETURN: m_chat_result = 1; break;
	case VK_ESCAPE: m_chat_result = 2; break;
	case VK_BACK:
		if ( !m_chat_input.empty( ) )
			m_chat_input.pop_back( );
		break;
	default: break;
	}

	return true;
}

/* ---- render thread ---- */

void n_mc_hud::impl_t::on_end_scene( )
{
	if ( !n_mc_hud::enabled( ) )
		return;

	static int s_drawn = -1;
	const auto draw_log = [ & ]( int key, const char* why, int a = 0, int b = 0, int c = 0 ) {
		if ( key != s_drawn ) {
			s_drawn = key;
			char line[ 160 ]{ };
			sprintf_s( line, "MC: draw %s %d %d %d", why, a, b, c );
			botox_dbg_log( "%s", line );
			g_console.print( line );
		}
	};

	snapshot_t s{ };
	std::vector< line_t > lines{ };
	{
		std::lock_guard< std::mutex > lock( m_lock );
		if ( !m_snapshot.m_in_game ) {
			draw_log( 0, "skip, snapshot not in game" );
			return;
		}

		s = m_snapshot;
		const size_t keep = std::min< size_t >( s_lines.size( ), 20 );
		lines.assign( s_lines.end( ) - static_cast< std::ptrdiff_t >( keep ), s_lines.end( ) );
	}

	bool chat_open = false;
	std::wstring input{ };
	unsigned long long open_ms = 0ull;
	{
		std::scoped_lock lock( m_chat_mutex );
		chat_open = m_chat_open;
		input     = m_chat_input;
		open_ms   = m_chat_open_ms;
	}

	const int w = static_cast< int >( g_ctx.m_width ), h = static_cast< int >( g_ctx.m_height );
	if ( w <= 0 || h <= 0 ) {
		draw_log( 1, "skip, screen size", w, h );
		return;
	}

	const int scale = n_mc_logic::gui_scale( w, h, std::max( GET_VARIABLE( g_variables.m_mc_hud_gui_scale, int ), 0 ) );
	draw_log( 2 + scale * 2 + s.m_alive, "frame w h scale", w, h, scale );
	if ( static bool s_logged = false; !s_logged && !texture( sprite_hotbar ) ) {
		s_logged = true;
		botox_dbg_log( "MC: draw hotbar texture missing, device=%p", g_interfaces.m_direct_device );
	}
	const int sw = n_mc_logic::scaled( w, scale ), sh = n_mc_logic::scaled( h, scale );
	const int cx = sw / 2;

	const long long tick = mc_tick( );

	ImDrawList* const list = ImGui::GetBackgroundDrawList( );
	const auto block       = g_render.begin_stretch_block( list, false );
	const canvas_t c{ list, static_cast< float >( scale ) };

	list->AddCallback( cb_point, nullptr );

	static n_mc_logic::hearts_t s_hearts{ };
	static unsigned int s_highlight_handle = 0;
	static long long s_highlight_tick      = -1000;

	if ( s.m_alive ) {
		// Gui.renderCrosshair
		if ( GET_VARIABLE( g_variables.m_mc_hud_crosshair, bool ) && !s.m_scoped ) {
			list->AddCallback( cb_invert, nullptr );
			blit( c, sprite_crosshair, static_cast< float >( ( sw - 15 ) / 2 ), static_cast< float >( ( sh - 15 ) / 2 ), 15.f, 15.f );
			list->AddCallback( ImDrawCallback_ResetRenderState, nullptr );
			list->AddCallback( cb_point, nullptr );
		}

		// Gui.renderItemHotbar
		blit( c, sprite_hotbar, cx - 91.f, sh - 22.f, 182.f, 22.f );
		blit( c, sprite_hotbar_selection, cx - 91.f - 1.f + s.m_selected * 20.f, sh - 22.f - 1.f, 24.f, 23.f );
		if ( s.m_offhand.m_sprite >= 0 )
			blit( c, sprite_hotbar_offhand_left, cx - 91.f - 29.f, sh - 23.f, 29.f, 24.f );

		for ( int i = 0; i < 9; ++i )
			item( c, s.m_slots[ i ], cx - 90.f + i * 20.f + 2.f, sh - 16.f - 3.f );

		if ( s.m_offhand.m_sprite >= 0 )
			item( c, s.m_offhand, cx - 91.f - 26.f, sh - 16.f - 3.f );

		// Gui.renderPlayerHealth, 20 max health -> one row, row height 11
		const bool blink = s_hearts.update( s.m_health, tick, now_ms( ) );
		const float left = cx - 91.f, right = cx + 91.f, top = sh - 39.f;

		if ( s.m_armor > 0 ) {
			for ( int i = 0; i < 10; ++i ) {
				const int value  = i * 2 + 1;
				const int sprite = value < s.m_armor ? sprite_armor_full : value == s.m_armor ? sprite_armor_half : sprite_armor_empty;
				blit( c, sprite, left + i * 8.f, top - 10.f, 9.f, 9.f );
			}
		}

		n_mc_logic::java_random_t random{ };
		random.set_seed( tick * 312871 );

		for ( int i = 9; i >= 0; --i ) {
			float y = top;
			if ( s.m_health <= 4 )
				y += static_cast< float >( random.next_int_pow2( 2 ) );

			const float x = left + i * 8.f;
			blit( c, blink ? sprite_heart_container_blinking : sprite_heart_container, x, y, 9.f, 9.f );

			if ( blink && i * 2 < s_hearts.m_display_health )
				blit( c, i * 2 + 1 == s_hearts.m_display_health ? sprite_heart_half_blinking : sprite_heart_full_blinking, x, y, 9.f, 9.f );

			if ( i * 2 < s.m_health )
				blit( c, i * 2 + 1 == s.m_health ? sprite_heart_half : sprite_heart_full, x, y, 9.f, 9.f );
		}

		// food: always 20 (decorative), saturation > 0 so no jitter
		for ( int i = 0; i < 10; ++i ) {
			const float x = right - i * 8.f - 9.f;
			blit( c, sprite_food_empty, x, top, 9.f, 9.f );
			blit( c, sprite_food_full, x, top, 9.f, 9.f );
		}

		// ExperienceBarRenderer: money / 16000 as progress
		const float bar_x = static_cast< float >( ( sw - 182 ) / 2 ), bar_y = sh - 24.f - 5.f;
		const int filled  = std::min( static_cast< int >( std::min( s.m_money, 16000 ) / 16000.f * 183.f ), 182 );
		blit( c, sprite_experience_bar_background, bar_x, bar_y, 182.f, 5.f );
		if ( filled > 0 )
			blit( c, sprite_experience_bar_progress, bar_x, bar_y, static_cast< float >( filled ), 5.f, IM_COL32_WHITE, 0.f, 0.f, filled / 182.f,
			      1.f );

		// ContextualBarRenderer level: money, 4 black copies then 0x80FF20
		if ( s.m_money > 0 ) {
			const text_t level = n_mc_logic::plain( std::to_string( s.m_money ) );
			const float lx     = static_cast< float >( ( sw - n_mc_logic::width( level ) ) / 2 ), ly = sh - 24.f - 9.f - 2.f;

			text_t black = level;
			std::fill( black.m_rgb.begin( ), black.m_rgb.end( ), 0u );
			text( c, black, lx + 1.f, ly, 255, false );
			text( c, black, lx - 1.f, ly, 255, false );
			text( c, black, lx, ly + 1.f, 255, false );
			text( c, black, lx, ly - 1.f, 255, false );

			text_t green = level;
			std::fill( green.m_rgb.begin( ), green.m_rgb.end( ), 0x80FF20u );
			text( c, green, lx, ly, 255, false );
		}

		// Gui.renderSelectedItemName: 40 ticks on switch
		if ( s.m_weapon_handle != s_highlight_handle ) {
			s_highlight_handle = s.m_weapon_handle;
			s_highlight_tick   = tick;
		}

		const int remaining = 40 - static_cast< int >( tick - s_highlight_tick );
		if ( remaining > 0 && !s.m_item_name.empty( ) ) {
			if ( const int alpha = n_mc_logic::highlight_alpha( remaining ); alpha > 0 ) {
				const text_t name = n_mc_logic::plain( s.m_item_name );
				text( c, name, static_cast< float >( ( sw - n_mc_logic::width( name ) ) / 2 ), sh - 59.f, alpha, true );
			}
		}
	}
	else
		s_hearts = { };

	// ChatComponent.render: line 9, width 320 (+4 left pad, +8 right), bottom (h - 40), 10 lines or 20 while typing
	{
		const int rows   = chat_open ? 20 : 10;
		const int bottom = sh - 40;
		const int shown  = std::min( rows, static_cast< int >( lines.size( ) ) );

		const auto fade = [ & ]( const line_t& line ) {
			return chat_open ? 1.f : n_mc_logic::chat_fade( static_cast< int >( tick - line.m_tick ) );
		};

		for ( int row = shown - 1; row >= 0; --row ) {
			const line_t& line = lines[ lines.size( ) - 1 - row ];
			const float alpha  = fade( line );
			if ( alpha <= 1e-5f )
				continue;

			const float y = static_cast< float >( bottom - row * 9 );
			fill( c, 0.f, y - 9.f, 4.f + 320.f + 8.f, y, IM_COL32( 0, 0, 0, static_cast< int >( alpha * 0.5f * 255.f ) ) );
		}

		for ( int row = shown - 1; row >= 0; --row ) {
			const line_t& line = lines[ lines.size( ) - 1 - row ];
			const int alpha    = static_cast< int >( fade( line ) * 255.f );
			if ( alpha <= 3 )
				continue;

			text( c, line.m_text, 4.f, static_cast< float >( bottom - row * 9 - 8 ), alpha, true );
		}
	}

	// ChatScreen.render: 0x80000000 bar, EditBox at (4, h - 12) #E0E0E0, "_" cursor blinking every 300 ms
	if ( chat_open ) {
		fill( c, 2.f, sh - 14.f, sw - 2.f, sh - 2.f, IM_COL32( 0, 0, 0, 128 ) );

		text_t typed = n_mc_logic::plain( g_utilities.to_utf8( input ), 0xE0E0E0 );

		// EditBox scrolls to keep the end visible
		const int room = sw - 4 - 4 - n_mc_logic::advance( '_' );
		size_t first   = 0;
		for ( int width = n_mc_logic::width( typed ); width > room && first < typed.size( ); ++first )
			width -= n_mc_logic::advance( typed.m_cp[ first ] );
		typed = n_mc_logic::slice( typed, first, typed.size( ) );

		text( c, typed, 4.f, sh - 12.f, 255, true );

		if ( ( GetTickCount64( ) - open_ms ) / 300ull % 2ull == 0ull )
			text( c, n_mc_logic::plain( "_", 0xE0E0E0 ), 4.f + n_mc_logic::width( typed ), sh - 12.f, 255, true );
	}

	list->AddCallback( ImDrawCallback_ResetRenderState, nullptr );
	g_render.end_stretch_block( block );
}

void n_mc_hud::impl_t::release_textures( )
{
	for ( auto& t : s_textures ) {
		if ( t.m_texture )
			t.m_texture->Release( );
		t = { };
	}
}
