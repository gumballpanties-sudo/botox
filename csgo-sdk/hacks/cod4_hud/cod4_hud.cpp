#include "cod4_hud.h"
#include "cod4_expr.h"
#include "cod4_iwd.h"
#include "cod4_logic.h"

#include "../../dependencies/imgui/imgui.h"
#include "../../utilities/console/console.h"
#include "../menu/menu.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cwctype>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string_view>
#include <thread>
#include <unordered_map>

using namespace n_cod4;
using n_cod4_expr::value_t;

extern bool point_menu_is_opened( );

bool n_cod4_hud::enabled( ) { return GET_VARIABLE( g_variables.m_cod4_hud, bool ); }

namespace
{
	int now_ms( )
	{
		static const auto start = std::chrono::steady_clock::now( );
		return static_cast< int >( std::chrono::duration_cast< std::chrono::milliseconds >( std::chrono::steady_clock::now( ) - start ).count( ) );
	}

	// csgo radar overviews have north = +y, cod4's compass rotates against that
	constexpr float k_north_yaw = 90.f;

	// _hardpoints.gsc: radar_mp 3, airstrike_mp 5, helicopter_mp 7 kills without dying
	constexpr int k_streaks[] = { 3, 5, 7 };
	constexpr const char* k_hardpoints[] = { "radar_mp", "airstrike_mp", "helicopter_mp" };
	constexpr const char* k_hardpoint_hints[] = { "MP_EARNED_RADAR", "MP_EARNED_AIRSTRIKE", "MP_EARNED_HELICOPTER" };

	/* ---- events: game thread writes, render thread drains (both under g_cod4_hud.m_lock) ---- */
	struct queued_line_t {
		int m_window;
		std::string m_text;
		int m_time;
	};

	struct notify_t {
		std::string m_title{ }, m_text{ };
		int m_start = 0, m_duration = 0;
	};

	struct events_t {
		std::vector< queued_line_t > m_lines{ };
		std::string m_center{ };
		int m_center_time   = 0;
		int m_hit_time      = -100000;
		int m_xp_total      = 0, m_xp_shown = 0, m_xp_time = -100000; // _rank.gsc updateRankScoreHUD
		std::vector< float > m_damage_yaws{ };                         // pending CG_DamageFeedback
		std::array< int, 65 > m_fire_time{ };                          // weapon_fire per player index
		int m_streak        = 0;
		int m_hardpoint     = -1; // held, k_hardpoints index
		std::deque< notify_t > m_notifies{ };
		int m_radar_until   = 0;
		int m_weapon_switch = -100000;
		unsigned int m_last_weapon = 0;
	} s_ev{ };
	events_t s_rv{ };                 // render thread's copy, taken under the lock each frame
	std::deque< notify_t > s_notifies{ }; // render thread

	// weapon_fire only reaches clients that listen for it; fire_event_intern then sees it (body empty)
	struct fire_listener_t : c_game_event_listener {
		fire_listener_t( ) { debug_id = 42; }
		void fire_game_event( game_event_t* ) override { }
	} s_listener{ };
	bool s_listening = false;

	// CCSGameRules*, null while loading (same site as skins_internal.h game_rules_up)
	std::uintptr_t game_rules( )
	{
		static const auto site =
			g_modules[ CLIENT_DLL ].find_pattern( "8B 4D 04 E8 ? ? ? ? 8B 0D ? ? ? ? 8B 01 FF 90 98 04 00 00 C6 45 FC 00" );
		return site ? reinterpret_cast< std::uintptr_t >( **reinterpret_cast< void*** >( site + 10 ) ) : 0;
	}

	/* ---- render thread state ---- */
	dvars_t s_dv{ };
	std::string s_dv_text{ "\x01" };
	msg_window_t s_windows[ 2 ]{ };
	std::string s_chat_line{ }; // typed line + prompt, empty = closed (copied from the chat mutex each frame)
	health_overlay_t s_health{ };

	struct damage_slot_t {
		int m_time = -100000, m_duration = 0;
		float m_yaw = 0.f;
	};
	damage_slot_t s_damage[ 8 ]{ };

	struct actor_state_t {
		float m_ping[ 2 ]{ };
		int m_ping_time = 0; // radar sweep ping (0x4278d0)
		int m_fire_time = 0;
	};
	actor_state_t s_actor_state[ 65 ]{ };
	float s_sweep = 0.f; // 0x427a20 radar line position 0..1
	int s_sweep_last = 0;

	/* ---- assets: iwd index + decoded images on a worker, textures on the render thread ---- */
	struct texture_t {
		IDirect3DTexture9* m_texture = nullptr;
		int m_w = 0, m_h = 0;
		bool m_tried = false;
	};

	std::atomic< int > s_load_state{ 0 }; // 0 idle, 1 loading, 2 ready, 3 failed
	std::string s_cod4_dir{ };
	std::vector< n_cod4_iwd::image_t > s_images{ }; // per k_materials, written by the worker before state 2
	std::vector< texture_t > s_textures{ };

	struct overview_t {
		std::string m_map{ };
		IDirect3DTexture9* m_texture = nullptr;
		float m_pos_x = 0.f, m_pos_y = 0.f, m_scale = 1.f;
		bool m_tried = false;
	} s_overview{ };

	std::string steam_path( )
	{
		char buf[ MAX_PATH ]{ };
		DWORD size = sizeof( buf );
		if ( RegGetValueA( HKEY_CURRENT_USER, "Software\\Valve\\Steam", "SteamPath", RRF_RT_REG_SZ, nullptr, buf, &size ) == ERROR_SUCCESS )
			return buf;
		return "C:/Program Files (x86)/Steam";
	}

	std::string find_cod4( )
	{
		const std::string custom = GET_VARIABLE( g_variables.m_cod4_hud_path, std::string );
		std::vector< std::string > roots{ };
		if ( !custom.empty( ) )
			roots.push_back( custom );

		const std::string steam = steam_path( );
		roots.push_back( steam + "/steamapps/common/Call of Duty 4" );

		// other steam libraries: "path"  "D:\\SteamLibrary"
		std::ifstream vdf( steam + "/steamapps/libraryfolders.vdf" );
		for ( std::string line; std::getline( vdf, line ); ) {
			const size_t key = line.find( "\"path\"" );
			if ( key == std::string::npos )
				continue;
			const size_t a = line.find( '"', key + 6 ), b = a == std::string::npos ? a : line.find( '"', a + 1 );
			if ( b == std::string::npos )
				continue;
			std::string p = line.substr( a + 1, b - a - 1 );
			for ( size_t i = 0; ( i = p.find( "\\\\", i ) ) != std::string::npos; )
				p.erase( i, 1 );
			roots.push_back( p + "/steamapps/common/Call of Duty 4" );
		}

		std::error_code ec{ };
		for ( const std::string& r : roots )
			if ( std::filesystem::exists( std::filesystem::path( r ) / "main" / "iw_00.iwd", ec ) )
				return r;
		return { };
	}

	void load_worker( )
	{
		const std::string dir = find_cod4( );
		if ( dir.empty( ) ) {
			g_console.print( "COD4: no call of duty 4 install found (set the path under visuals > cod4 hud)" );
			s_load_state = 3;
			return;
		}

		std::unordered_map< std::string, n_cod4_iwd::entry_t > index{ };
		std::vector< std::string > files{ };
		std::error_code ec{ };
		for ( const auto& e : std::filesystem::directory_iterator( std::filesystem::path( dir ) / "main", ec ) )
			if ( e.path( ).extension( ) == ".iwd" )
				files.push_back( e.path( ).string( ) );
		std::sort( files.begin( ), files.end( ) );
		for ( const std::string& f : files )
			n_cod4_iwd::index_iwd( f, index );

		std::vector< n_cod4_iwd::image_t > images( std::size( k_materials ) );
		int missing = 0;
		for ( size_t i = 0; i < std::size( k_materials ); ++i ) {
			const material_t& m = k_materials[ i ];
			if ( m.m_image[ 0 ] == '$' ) {
				images[ i ].m_w = images[ i ].m_h = 1;
				images[ i ].m_bgra = { 255, 255, 255, 255 };
				continue;
			}
			const auto it = index.find( "images/" + n_cod4_iwd::lower( m.m_image ) + ".iwi" );
			std::vector< std::uint8_t > file{ };
			if ( it == index.end( ) || !n_cod4_iwd::read_entry( it->second, file ) || !n_cod4_iwd::decode_iwi( file, images[ i ] ) )
				++missing;
		}

		s_cod4_dir = dir;
		s_images   = std::move( images );
		char line[ 160 ]{ };
		sprintf_s( line, "COD4: %zu iwds, %zu materials, %d missing (%s)", files.size( ), std::size( k_materials ), missing, dir.c_str( ) );
		g_console.print( line );
		s_load_state = 2;
	}

	IDirect3DTexture9* make_texture( const n_cod4_iwd::image_t& img )
	{
		IDirect3DDevice9* const device = g_interfaces.m_direct_device;
		if ( !device || img.m_bgra.empty( ) )
			return nullptr;

		// csgo is d3d9ex: MANAGED is refused, DEFAULT + DYNAMIC instead (released before Reset, reset.cpp)
		IDirect3DTexture9* tex = nullptr;
		if ( FAILED( device->CreateTexture( img.m_w, img.m_h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, nullptr ) ) || !tex ) {
			tex = nullptr;
			if ( FAILED( device->CreateTexture( img.m_w, img.m_h, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &tex, nullptr ) ) || !tex )
				return nullptr;
		}
		D3DLOCKED_RECT rect{ };
		if ( FAILED( tex->LockRect( 0, &rect, nullptr, 0 ) ) ) {
			tex->Release( );
			return nullptr;
		}
		for ( int y = 0; y < img.m_h; ++y )
			std::memcpy( static_cast< std::uint8_t* >( rect.pBits ) + static_cast< size_t >( y ) * rect.Pitch, img.m_bgra.data( ) + static_cast< size_t >( y ) * img.m_w * 4,
			             static_cast< size_t >( img.m_w ) * 4 );
		tex->UnlockRect( 0 );
		return tex;
	}

	const texture_t* texture( int material )
	{
		if ( material < 0 || material >= static_cast< int >( std::size( k_materials ) ) || s_load_state != 2 )
			return nullptr;
		if ( s_textures.size( ) != std::size( k_materials ) )
			s_textures.resize( std::size( k_materials ) );
		texture_t& t = s_textures[ material ];
		if ( !t.m_tried ) {
			t.m_tried   = true;
			t.m_texture = make_texture( s_images[ material ] );
			t.m_w = s_images[ material ].m_w, t.m_h = s_images[ material ].m_h;
		}
		return t.m_texture ? &t : nullptr;
	}

	int mat( std::string_view name )
	{
		static std::unordered_map< std::string, int > cache{ };
		const auto it = cache.find( std::string( name ) );
		if ( it != cache.end( ) )
			return it->second;
		const int i = material_index( name );
		cache[ std::string( name ) ] = i;
		return i;
	}

	// resource/overviews/<map>.txt + <map>_radar.dds, loose files in csgo/
	void load_overview( const std::string& map )
	{
		if ( s_overview.m_map == map && s_overview.m_tried )
			return;
		if ( s_overview.m_texture )
			s_overview.m_texture->Release( );
		s_overview = { };
		s_overview.m_map   = map;
		s_overview.m_tried = true;
		if ( map.empty( ) || !g_interfaces.m_direct_device )
			return;

		char exe[ MAX_PATH ]{ };
		GetModuleFileNameA( nullptr, exe, MAX_PATH );
		const std::filesystem::path base = std::filesystem::path( exe ).parent_path( ) / "csgo" / "resource" / "overviews";

		std::ifstream txt( base / ( map + ".txt" ) );
		bool have = false;
		for ( std::string line; std::getline( txt, line ); ) {
			const auto value = [ & ]( const char* key, float& out ) {
				const size_t k = line.find( std::string( "\"" ) + key + "\"" );
				if ( k == std::string::npos )
					return;
				const size_t a = line.find( '"', k + std::strlen( key ) + 2 );
				if ( a != std::string::npos )
					out = static_cast< float >( std::atof( line.c_str( ) + a + 1 ) ), have = true;
			};
			value( "pos_x", s_overview.m_pos_x );
			value( "pos_y", s_overview.m_pos_y );
			value( "scale", s_overview.m_scale );
		}
		if ( !have || s_overview.m_scale <= 0.f )
			return;

		const std::string dds = ( base / ( map + "_radar.dds" ) ).string( );
		const auto load       = [ & ]( DWORD usage, D3DPOOL pool ) {
            return D3DXCreateTextureFromFileExA( g_interfaces.m_direct_device, dds.c_str( ), D3DX_DEFAULT_NONPOW2, D3DX_DEFAULT_NONPOW2, 1, usage,
			                                           D3DFMT_A8R8G8B8, pool, D3DX_FILTER_NONE, D3DX_FILTER_NONE, 0, nullptr, nullptr, &s_overview.m_texture );
		};
		if ( FAILED( load( 0, D3DPOOL_MANAGED ) ) && FAILED( load( D3DUSAGE_DYNAMIC, D3DPOOL_DEFAULT ) ) )
			s_overview.m_texture = nullptr;
	}

	/* ---- csgo weapon icons (the icon font botox already ships, 48 px instance) ---- */
	std::uint32_t utf8_codepoint( const char8_t* s )
	{
		const auto* b = reinterpret_cast< const unsigned char* >( s );
		if ( !b || !b[ 0 ] )
			return 0;
		if ( b[ 0 ] < 0x80 )
			return b[ 0 ];
		if ( ( b[ 0 ] & 0xE0 ) == 0xC0 && b[ 1 ] )
			return ( b[ 0 ] & 0x1F ) << 6 | ( b[ 1 ] & 0x3F );
		if ( ( b[ 0 ] & 0xF0 ) == 0xE0 && b[ 1 ] && b[ 2 ] )
			return ( b[ 0 ] & 0x0F ) << 12 | ( b[ 1 ] & 0x3F ) << 6 | ( b[ 2 ] & 0x3F );
		return 0;
	}

	std::uint32_t icon_for_definition( short definition ) { return utf8_codepoint( g_utilities.get_weapon_icon( definition ) ); }

	// game thread: player_death "weapon" (classname minus weapon_) -> icon codepoint
	std::uint32_t icon_for_event_weapon( const std::string& name )
	{
		static std::unordered_map< std::string, std::uint32_t > s_map{ };
		if ( s_map.empty( ) && g_interfaces.m_weapon_system ) {
			static constexpr short k_defs[] = { 1,  2,  3,  4,  7,  8,  9,  10, 11, 13, 14, 16, 17, 19, 23, 24, 25, 26, 27, 28, 29, 30,
				                                31, 32, 33, 34, 35, 36, 38, 39, 40, 42, 43, 44, 45, 46, 47, 48, 49, 59, 60, 61, 63, 64 };
			for ( const short d : k_defs )
				if ( const c_weapon_data* data = g_interfaces.m_weapon_system->get_weapon_data( d ); data && data->m_weapon_name ) {
					std::string n = data->m_weapon_name;
					if ( n.rfind( "weapon_", 0 ) == 0 )
						n.erase( 0, 7 );
					s_map[ n ] = icon_for_definition( d );
				}
		}
		if ( const auto it = s_map.find( name ); it != s_map.end( ) )
			return it->second;
		if ( name.rfind( "knife", 0 ) == 0 || name == "bayonet" )
			return icon_for_definition( e_item_definition_index::weapon_knife );
		return 0;
	}

	/* ---- draw primitives (real pixels) ---- */
	ImDrawList* s_list = nullptr;
	scr_t s_scr{ };
	DWORD s_saved_address[ 2 ]{ D3DTADDRESS_WRAP, D3DTADDRESS_WRAP };

	ImU32 col( const float c[ 4 ], float alpha_mul = 1.f )
	{
		const auto b = [ ]( float v ) { return static_cast< int >( std::clamp( v, 0.f, 1.f ) * 255.f + 0.5f ); };
		return IM_COL32( b( c[ 0 ] ), b( c[ 1 ] ), b( c[ 2 ] ), b( c[ 3 ] * alpha_mul ) );
	}

	void cb_additive( const ImDrawList*, const ImDrawCmd* )
	{
		g_interfaces.m_direct_device->SetRenderState( D3DRS_SRCBLEND, D3DBLEND_SRCALPHA );
		g_interfaces.m_direct_device->SetRenderState( D3DRS_DESTBLEND, D3DBLEND_ONE );
	}
	void cb_address( const ImDrawList*, const ImDrawCmd* cmd )
	{
		IDirect3DDevice9* const d = g_interfaces.m_direct_device;
		const auto mode           = static_cast< DWORD >( reinterpret_cast< std::uintptr_t >( cmd->UserCallbackData ) );
		d->GetSamplerState( 0, D3DSAMP_ADDRESSU, &s_saved_address[ 0 ] );
		d->GetSamplerState( 0, D3DSAMP_ADDRESSV, &s_saved_address[ 1 ] );
		d->SetSamplerState( 0, D3DSAMP_ADDRESSU, mode );
		d->SetSamplerState( 0, D3DSAMP_ADDRESSV, mode );
		d->SetSamplerState( 0, D3DSAMP_BORDERCOLOR, 0 );
	}
	void cb_address_restore( const ImDrawList*, const ImDrawCmd* )
	{
		g_interfaces.m_direct_device->SetSamplerState( 0, D3DSAMP_ADDRESSU, s_saved_address[ 0 ] );
		g_interfaces.m_direct_device->SetSamplerState( 0, D3DSAMP_ADDRESSV, s_saved_address[ 1 ] );
	}

	struct blend_scope_t {
		bool m_on;
		explicit blend_scope_t( int material ) : m_on( material >= 0 && k_materials[ material ].m_additive )
		{
			if ( m_on )
				s_list->AddCallback( cb_additive, nullptr );
		}
		~blend_scope_t( )
		{
			if ( m_on )
				s_list->AddCallback( ImDrawCallback_ResetRenderState, nullptr );
		}
	};

	// R_AddCmdDrawStretchPic
	void pic( int material, float x, float y, float w, float h, const float color[ 4 ], float s0 = 0.f, float t0 = 0.f, float s1 = 1.f, float t1 = 1.f )
	{
		const texture_t* t = texture( material );
		if ( !t || color[ 3 ] <= 0.f )
			return;
		blend_scope_t blend( material );
		s_list->AddImageQuad( reinterpret_cast< ImTextureID >( t->m_texture ), ImVec2( x, y ), ImVec2( x + w, y ), ImVec2( x + w, y + h ), ImVec2( x, y + h ),
		                      ImVec2( s0, t0 ), ImVec2( s1, t0 ), ImVec2( s1, t1 ), ImVec2( s0, t1 ), col( color ) );
	}

	// CG_DrawRotatedPic (0x42f9d0): rotated about the rect center, degrees, screen y down
	void rotated_pic( int material, float x, float y, float w, float h, float angle, const float color[ 4 ] )
	{
		const texture_t* t = texture( material );
		if ( !t || color[ 3 ] <= 0.f )
			return;
		const float c = std::cos( angle * 0.0174532924f ), s = std::sin( angle * 0.0174532924f );
		const float cx = x + w * 0.5f, cy = y + h * 0.5f;
		const auto at  = [ & ]( float dx, float dy ) { return ImVec2( cx + dx * c - dy * s, cy + dx * s + dy * c ); };
		blend_scope_t blend( material );
		s_list->AddImageQuad( reinterpret_cast< ImTextureID >( t->m_texture ), at( -w * 0.5f, -h * 0.5f ), at( w * 0.5f, -h * 0.5f ), at( w * 0.5f, h * 0.5f ),
		                      at( -w * 0.5f, h * 0.5f ), ImVec2( 0, 0 ), ImVec2( 1, 0 ), ImVec2( 1, 1 ), ImVec2( 0, 1 ), col( color ) );
	}

	// csgo icon glyph in an icon box (real px): em size 0.9 x box height so glyph heights stay relative like csgo's
	// killfeed, vertically centered, black outline all round. returns the advance (glyph + both outline edges)
	float weapon_glyph( std::uint32_t cp, float x, float box_top, float box_h, float alpha, bool draw )
	{
		ImFont* const font = g_render.live_font( g_render.m_fonts[ e_font_names::font_name_cod4_weapon_icon_48 ] );
		if ( !font || !cp || font->FontSize <= 0.f )
			return 0.f;
		const ImFontGlyph* g = font->FindGlyph( static_cast< ImWchar >( cp ) );
		if ( !g || g->Codepoint != cp )
			return 0.f;
		const float k = box_h * 0.9f / font->FontSize;
		const float t = std::max( 1.f, std::round( box_h / 22.f ) );
		const float w = ( g->X1 - g->X0 ) * k, h = ( g->Y1 - g->Y0 ) * k;
		const float gx = x + t, gy = box_top + ( box_h - h ) * 0.5f;
		if ( draw && alpha > 0.f ) {
			const ImTextureID tex = font->ContainerAtlas->TexID;
			const int a           = static_cast< int >( std::clamp( alpha, 0.f, 1.f ) * 255.f );
			for ( int dy = -1; dy <= 1; ++dy )
				for ( int dx = -1; dx <= 1; ++dx )
					if ( dx || dy )
						s_list->AddImage( tex, ImVec2( gx + dx * t, gy + dy * t ), ImVec2( gx + dx * t + w, gy + dy * t + h ), ImVec2( g->U0, g->V0 ),
						                  ImVec2( g->U1, g->V1 ), IM_COL32( 0, 0, 0, a ) );
			s_list->AddImage( tex, ImVec2( gx, gy ), ImVec2( gx + w, gy + h ), ImVec2( g->U0, g->V0 ), ImVec2( g->U1, g->V1 ), IM_COL32( 255, 255, 255, a ) );
		}
		return w + 2.f * t;
	}

	/* ---- text: R_AddCmdDrawText -> RB text (0x5f3730): glyph quads, ^0-^9, ^\x01|\x02 icons, shadow ---- */
	void color_code( char c, const float base[ 4 ], float out[ 4 ] )
	{
		static constexpr std::uint32_t k_table[ 8 ] = { 0x000000, 0xFF5C5C, 0x00FF00, 0xFFFF00, 0x0000FF, 0x00FFFF, 0xFF5CFF, 0xFFFFFF };
		int i = c - '0';
		if ( i < 0 || i > 9 )
			i = 7;
		if ( i < 8 ) {
			out[ 0 ] = ( k_table[ i ] >> 16 & 255 ) / 255.f, out[ 1 ] = ( k_table[ i ] >> 8 & 255 ) / 255.f, out[ 2 ] = ( k_table[ i ] & 255 ) / 255.f;
		}
		else {
			// ^8 = the local player's faction color, ^9 the other one (0x5f3020, g_TeamColor_Allies / Axis)
			const bool axis    = g_cod4_hud.m_snapshot.m_team == 2;
			const float* c8    = s_dv.color( ( i == 8 ) == axis ? "g_TeamColor_Axis" : "g_TeamColor_Allies" );
			out[ 0 ] = c8[ 0 ], out[ 1 ] = c8[ 1 ], out[ 2 ] = c8[ 2 ];
		}
		out[ 3 ] = base[ 3 ];
	}

	// x, y = baseline origin in real px; k = real px per font pixel
	float text( int font, std::string_view s, float x, float y, float kx, float ky, const float color[ 4 ], int style, const float* glow = nullptr )
	{
		const font_t& f = k_fonts[ font ];
		const texture_t* atlas = texture( mat( f.m_material ) );
		if ( !atlas || color[ 3 ] <= 0.f )
			return 0.f;
		const ImTextureID id = reinterpret_cast< ImTextureID >( atlas->m_texture );
		const float shadow   = style == 3 || style == 6 ? ( style == 6 ? 2.f : 1.f ) : 0.f;

		for ( int pass = shadow > 0.f ? 0 : 1; pass < 2; ++pass ) {
			float pen = x;
			float c[ 4 ]{ color[ 0 ], color[ 1 ], color[ 2 ], color[ 3 ] };
			const float off = pass == 0 ? shadow : 0.f;
			for ( size_t i = 0; i < s.size( ); ) {
				const unsigned char ch = static_cast< unsigned char >( s[ i ] );
				if ( ch == '^' && i + 1 < s.size( ) ) {
					const char n = s[ i + 1 ];
					if ( n != '^' && n >= '0' && n <= '9' ) {
						color_code( n, color, c );
						i += 2;
						continue;
					}
					if ( ( n == 1 || n == 2 ) && i + 8 <= s.size( ) ) {
						// 0x5f3090: icon w/h from the bytes, vertically centered on the font height
						const int wpx = icon_pixels( static_cast< unsigned char >( s[ i + 2 ] ), f.m_pixel_height );
						const int hpx = icon_pixels( static_cast< unsigned char >( s[ i + 3 ] ), f.m_pixel_height );
						int material  = -1;
						std::memcpy( &material, s.data( ) + i + 4, 4 );
						const float w = wpx * kx, h = hpx * ky;
						const float top = y - ( f.m_pixel_height * ky + h ) * 0.5f;
						if ( material < 0 ) {
							pen += weapon_glyph( static_cast< std::uint32_t >( -material ), pen, top, h, color[ 3 ], pass == 1 );
							i += 8;
							continue;
						}
						if ( pass == 1 ) {
							const float white[ 4 ]{ 1.f, 1.f, 1.f, color[ 3 ] };
							if ( n == 1 )
								pic( material, pen, top, w, h, white );
							else
								pic( material, pen, top, w, h, white, 1.f, 0.f, 0.f, 1.f );
						}
						pen += w;
						i += 8;
						continue;
					}
				}
				const glyph_t* g = glyph( font, ch );
				if ( g->m_w && g->m_h ) {
					const float gx = pen + g->m_x0 * kx + off, gy = y + g->m_y0 * ky + off;
					const float sh[ 4 ]{ 0.f, 0.f, 0.f, color[ 3 ] };
					s_list->AddImage( id, ImVec2( gx, gy ), ImVec2( gx + g->m_w * kx, gy + g->m_h * ky ), ImVec2( g->m_s0, g->m_t0 ), ImVec2( g->m_s1, g->m_t1 ),
					                  col( pass == 0 ? sh : c ) );
				}
				pen += g->m_dx * kx;
				++i;
			}
			if ( pass == 1 && !glow )
				return pen - x;
			if ( pass == 1 ) {
				// glow pass (0x5f3730, second loop): glow material additive, glyph 1.75 x 1.125 from (-0.375 w, -0.0625 h),
				// four copies at +-2 font px
				const texture_t* g_atlas = texture( mat( f.m_glow_material ) );
				if ( g_atlas ) {
					s_list->AddCallback( cb_additive, nullptr );
					float gpen = x;
					const float gc[ 4 ]{ glow[ 0 ], glow[ 1 ], glow[ 2 ], glow[ 3 ] * color[ 3 ] };
					static constexpr float k_offsets[ 4 ][ 2 ] = { { -1.f, -1.f }, { -1.f, 1.f }, { 1.f, -1.f }, { 1.f, 1.f } };
					for ( size_t i = 0; i < s.size( ); ) {
						if ( s[ i ] == '^' && i + 1 < s.size( ) && s[ i + 1 ] >= '0' && s[ i + 1 ] <= '9' ) {
							i += 2;
							continue;
						}
						const glyph_t* g = glyph( font, static_cast< unsigned char >( s[ i ] ) );
						if ( g->m_w && g->m_h ) {
							const float bx = gpen + g->m_x0 * kx - g->m_w * kx * 0.375f, by = y + g->m_y0 * ky - g->m_h * ky * 0.0625f;
							for ( const auto& o : k_offsets ) {
								const float gx = bx + 2.f * o[ 0 ] * kx, gy = by + 2.f * o[ 1 ] * ky;
								s_list->AddImage( reinterpret_cast< ImTextureID >( g_atlas->m_texture ), ImVec2( gx, gy ),
								                  ImVec2( gx + g->m_w * kx * 1.75f, gy + g->m_h * ky * 1.125f ), ImVec2( g->m_s0, g->m_t0 ), ImVec2( g->m_s1, g->m_t1 ),
								                  col( gc ) );
							}
						}
						gpen += g->m_dx * kx;
						++i;
					}
					s_list->AddCallback( ImDrawCallback_ResetRenderState, nullptr );
				}
				return pen - x;
			}
		}
		return 0.f;
	}

	// UI_DrawText (0x53d650): virtual x/y + aligns, text scale -> font pixels, positions snapped to whole pixels
	float ui_text( std::string_view s, float vx, float vy, int ha, int va, int font_enum, float scale, const float color[ 4 ], int style,
	               const float* glow = nullptr )
	{
		const int font = pick_font( font_enum, scale, s_scr.m_sy, s_dv );
		const float k  = scale * 48.f / static_cast< float >( k_fonts[ font ].m_pixel_height );
		const float x = std::floor( s_scr.x( vx, ha ) + 0.5f ), y = std::floor( s_scr.y( vy, va ) + 0.5f );
		return text( font, s, x, y, s_scr.w( k, ha ), s_scr.h( k, va ), color, style, glow );
	}

	// UI_TextWidth in virtual units
	float ui_text_width( std::string_view s, int font_enum, float scale )
	{
		const int font = pick_font( font_enum, scale, s_scr.m_sy, s_dv );
		return static_cast< float >( text_width( s, font ) ) * scale * 48.f / static_cast< float >( k_fonts[ font ].m_pixel_height );
	}

	/* ---- menu expression context ---- */
	struct ctx_t : n_cod4_expr::context_t {
		const n_cod4_hud::snapshot_t* m_s = nullptr;
		int m_now     = 0;
		bool m_vitals = false; // scorebars menu: team score = health, other team = armor

		std::string team_name( ) const
		{
			if ( m_s->m_team == 3 )
				return "TEAM_ALLIES";
			if ( m_s->m_team == 2 )
				return "TEAM_AXIS";
			return "TEAM_SPECTATOR";
		}

		value_t call( std::string_view n, const std::vector< value_t >& a ) override
		{
			const auto arg = [ & ]( size_t i ) { return i < a.size( ) ? a[ i ] : value_t::num( 0 ); };
			const auto num = [ ]( double v ) { return value_t::num( v ); };
			const std::string s0 = arg( 0 ).text( );

			if ( n == "dvarint" || n == "dvarbool" || n == "dvarfloat" ) {
				if ( s0 == "wideScreen" )
					return num( s_scr.m_w / s_scr.m_h > 4.f / 3.f + 0.01f ? 1 : 0 );
				if ( s0 == "ui_scorelimit" )
					return num( m_vitals ? std::max( { 100, m_s->m_health, m_s->m_armor } ) : m_s->m_score_limit );
				if ( s0 == "ui_timelimit" )
					return num( m_s->m_timeleft >= 0.f ? 1 : 0 );
				// csgo's c4 is not sab / sd's: no suitcase, no tenths clock
				if ( s0 == "ui_bomb_timer" )
					return num( 0 );
				if ( s0 == "ui_uav_allies" || s0 == "ui_uav_axis" || s0 == "ui_uav_client" )
					return num( m_now < s_rv.m_radar_until ? 1 : 0 );
				return num( n == "dvarint" ? s_dv.i( s0 ) : s_dv.f( s0 ) );
			}
			if ( n == "dvarstring" ) {
				if ( s0 == "g_TeamIcon_Allies" || s0 == "g_TeamIcon_Axis" ) {
					const std::string faction = s_dv.s( s0 == "g_TeamIcon_Allies" ? "scr_allies" : "scr_axis" );
					for ( const faction_t& f : k_factions )
						if ( faction == f.m_name )
							return value_t::str( f.m_icon );
				}
				return value_t::str( s_dv.s( s0 ) );
			}
			if ( n == "team" ) {
				if ( s0 == "name" )
					return value_t::str( team_name( ) );
				return num( m_vitals ? m_s->m_health : m_s->m_team_score );
			}
			if ( n == "otherteam" )
				return num( m_vitals ? m_s->m_armor : m_s->m_enemy_score );
			if ( n == "marinesfield" )
				return num( m_s->m_ct_score );
			if ( n == "opforfield" )
				return num( m_s->m_t_score );
			if ( n == "player" )
				return num( m_s->m_player_score );
			if ( n == "scoreatrank" )
				return num( arg( 0 ).number( ) <= 1 ? m_s->m_score_rank1 : m_s->m_score_rank2 );
			if ( n == "localvarstring" )
				return value_t::str( m_s->m_team == 2 ? "opfor" : "marines" );
			if ( n == "timeleft" )
				// run out = round over in cod4, the clock goes; community servers sit at 0 forever
				return num( m_s->m_timeleft <= 0.f ? -1.0 : std::ceil( m_s->m_timeleft ) );
			if ( n == "secondsascountdown" ) {
				const int t = std::max( 0, static_cast< int >( arg( 0 ).number( ) ) );
				char b[ 16 ];
				sprintf_s( b, "%d:%02d", t / 60, t % 60 );
				return value_t::str( b );
			}
			if ( n == "stat" ) {
				const int xp = GET_VARIABLE( g_variables.m_cod4_hud_xp, int );
				int lo = 0, hi = 0;
				rank_bounds( xp, lo, hi );
				const int id = static_cast< int >( arg( 0 ).number( ) );
				return num( id == 2301 ? xp : id == 2351 ? lo : id == 2352 ? hi : 0 );
			}
			if ( n == "locstring" ) {
				std::initializer_list< std::string > none{ };
				if ( a.size( ) >= 3 )
					return value_t::str( localize( s0, { a[ 1 ].text( ), a[ 2 ].text( ) } ) );
				if ( a.size( ) == 2 )
					return value_t::str( localize( s0, { a[ 1 ].text( ) } ) );
				return value_t::str( localize( s0, none ) );
			}
			if ( n == "max" )
				return num( std::max( arg( 0 ).number( ), arg( 1 ).number( ) ) );
			if ( n == "min" )
				return num( std::min( arg( 0 ).number( ), arg( 1 ).number( ) ) );
			if ( n == "int" )
				return num( std::trunc( arg( 0 ).number( ) ) );
			if ( n == "milliseconds" )
				return num( m_now );
			if ( n == "flashbanged" )
				return num( m_s->m_flashed ? 1 : 0 );
			if ( n == "ui_active" )
				return num( m_s->m_console || g_menu.m_opened ? 1 : 0 );
			if ( n == "spectatingclient" )
				return num( m_s->m_alive ? 0 : 1 );
			if ( n == "scoped" )
				return num( m_s->m_scoped && m_s->m_sniper ? 1 : 0 );
			if ( n == "actionslotusable" ) {
				const int slot = static_cast< int >( arg( 0 ).number( ) );
				return num( ( slot == 3 && m_s->m_has_c4 ) || ( slot == 4 && s_rv.m_hardpoint >= 0 ) ? 1 : 0 );
			}
			if ( n == "keybinding" ) {
				if ( s0.find( "actionslot 4" ) != std::string::npos ) {
					const int vk = GET_VARIABLE( g_variables.m_cod4_hud_hardpoint_key, key_bind_t ).m_key;
					char name[ 32 ]{ };
					if ( vk > 0 && GetKeyNameTextA( static_cast< LONG >( MapVirtualKeyA( static_cast< UINT >( vk ), MAPVK_VK_TO_VSC ) << 16 ), name, sizeof( name ) ) )
						return value_t::str( name );
				}
				if ( s0.find( "actionslot 3" ) != std::string::npos )
					return value_t::str( "5" );
				return value_t::str( "" );
			}
			if ( n == "hudfade" )
				return num( 1 );
			// inkillcam selecting_location adsjavelin menuisopen: no csgo counterpart
			return num( 0 );
		}
	};

	struct compiled_t {
		std::unordered_map< const char*, std::unique_ptr< n_cod4_expr::node_t > > m_nodes{ };
		const n_cod4_expr::node_t* get( const char* src )
		{
			if ( !src )
				return nullptr;
			auto it = m_nodes.find( src );
			if ( it == m_nodes.end( ) )
				it = m_nodes.emplace( src, n_cod4_expr::parser_t::parse( src ) ).first;
			return it->second.get( );
		}
	} s_compiled{ };

	value_t evaluate( const char* src, ctx_t& ctx, value_t fallback )
	{
		const n_cod4_expr::node_t* n = s_compiled.get( src );
		return n ? n_cod4_expr::eval( *n, ctx ) : fallback;
	}

	bool visible( const char* src, ctx_t& ctx ) { return !src || std::strcmp( src, "1" ) == 0 || evaluate( src, ctx, value_t::num( 1 ) ).truth( ); }

	// vitals scorebars: health stays the top bar, the losing-score swap is cut out of the rect expressions
	const char* vitals_expr( const char* src )
	{
		constexpr std::string_view k_swap = "(team(\"score\") < otherteam(\"score\"))";
		static std::unordered_map< const char*, std::string > s_fixed{ }; // node map: c_str stays put for s_compiled's pointer key
		if ( !src || !std::strstr( src, "< otherteam(" ) )
			return src;
		auto it = s_fixed.find( src );
		if ( it == s_fixed.end( ) ) {
			std::string e = src;
			for ( size_t p; ( p = e.find( k_swap ) ) != std::string::npos; )
				e.replace( p, k_swap.size( ), "0" );
			it = s_fixed.emplace( src, std::move( e ) ).first;
		}
		return it->second.c_str( );
	}

	/* ---- ownerdraws ---- */
	struct od_t {
		float m_x, m_y, m_w, m_h; // virtual rect
		int m_ha, m_va;
		float m_color[ 4 ];
		int m_material;
		float m_scale;
		int m_font, m_style;
	};

	// real rect of an ownerdraw rect
	void real_rect( const od_t& o, float r[ 4 ] )
	{
		r[ 0 ] = s_scr.x( o.m_x, o.m_ha ), r[ 1 ] = s_scr.y( o.m_y, o.m_va ), r[ 2 ] = s_scr.w( o.m_w, o.m_ha ), r[ 3 ] = s_scr.h( o.m_h, o.m_va );
	}

	// 0x424c80: compass rect = item rect with w/h * compassSize
	void compass_rect( const od_t& o, float r[ 4 ] )
	{
		od_t c = o;
		c.m_w *= s_dv.f( "compassSize" ), c.m_h *= s_dv.f( "compassSize" );
		real_rect( c, r );
	}

	// compass north vector: rotating compass = view forward, else the map's north
	void compass_basis( const n_cod4_hud::snapshot_t& s, float north[ 2 ] )
	{
		const float yaw = ( s_dv.b( "compassRotation" ) ? s.m_yaw : k_north_yaw ) * 0.0174532924f;
		north[ 0 ] = std::cos( yaw ), north[ 1 ] = std::sin( yaw );
	}

	void od_compass_map( const od_t& o, const n_cod4_hud::snapshot_t& s )
	{
		float r[ 4 ];
		compass_rect( o, r );
		const float white[ 4 ]{ 1.f, 1.f, 1.f, o.m_color[ 3 ] };

		if ( !s_overview.m_texture ) {
			pic( o.m_material, r[ 0 ], r[ 1 ], r[ 2 ], r[ 3 ], white ); // compass_map_default
			return;
		}

		// 0x425a80 + R_AddCmdDrawStretchPicRotateST: centre = player in map st, radius = maxRange / 2, turned by the view
		const float span = 1024.f * s_overview.m_scale;
		const float cu = ( s.m_origin[ 0 ] - s_overview.m_pos_x ) / span, cv = ( s_overview.m_pos_y - s.m_origin[ 1 ] ) / span;
		const float radius = s_dv.f( "compassMaxRange" ) * 0.5f / span;
		const float rot    = s_dv.b( "compassRotation" ) ? -( s.m_yaw - k_north_yaw ) : 0.f;
		const float c = std::cos( rot * 0.0174532924f ), sn = std::sin( rot * 0.0174532924f );
		const auto st = [ & ]( float dx, float dy ) { return ImVec2( cu + ( dx * c - dy * sn ) * radius, cv + ( dx * sn + dy * c ) * radius ); };

		s_list->AddCallback( cb_address, reinterpret_cast< void* >( static_cast< std::uintptr_t >( D3DTADDRESS_BORDER ) ) );
		s_list->AddImageQuad( reinterpret_cast< ImTextureID >( s_overview.m_texture ), ImVec2( r[ 0 ], r[ 1 ] ), ImVec2( r[ 0 ] + r[ 2 ], r[ 1 ] ),
		                      ImVec2( r[ 0 ] + r[ 2 ], r[ 1 ] + r[ 3 ] ), ImVec2( r[ 0 ], r[ 1 ] + r[ 3 ] ), st( -1, -1 ), st( 1, -1 ), st( 1, 1 ), st( -1, 1 ),
		                      col( white ) );
		s_list->AddCallback( cb_address_restore, nullptr );
		s_list->AddCallback( ImDrawCallback_ResetRenderState, nullptr );
	}

	// 0x426890: tickertape u = yaw / 360 +- stretch / 2, texture wraps
	void od_ticker( const od_t& o, const n_cod4_hud::snapshot_t& s )
	{
		float r[ 4 ];
		compass_rect( o, r );
		const texture_t* t = texture( o.m_material );
		if ( !t )
			return;
		const float rot = -( s.m_yaw - k_north_yaw ), half = s_dv.f( "compassTickertapeStretch" ) * 0.5f;
		const float u0 = rot / 360.f - half, u1 = rot / 360.f + half;
		s_list->AddCallback( cb_address, reinterpret_cast< void* >( static_cast< std::uintptr_t >( D3DTADDRESS_WRAP ) ) );
		s_list->AddImage( reinterpret_cast< ImTextureID >( t->m_texture ), ImVec2( r[ 0 ], r[ 1 ] ), ImVec2( r[ 0 ] + r[ 2 ], r[ 1 ] + r[ 3 ] ), ImVec2( u0, 0.f ),
		                  ImVec2( u1, 1.f ), col( o.m_color ) );
		s_list->AddCallback( cb_address_restore, nullptr );
		s_list->AddCallback( ImDrawCallback_ResetRenderState, nullptr );
	}

	// 0x426160 compass player arrow: rotation only when compassRotation off
	void od_compass_player( const od_t& o, const n_cod4_hud::snapshot_t& s )
	{
		float r[ 4 ];
		compass_rect( o, r );
		const float w = s_scr.w( s_dv.f( "compassPlayerWidth" ) * s_dv.f( "compassSize" ), o.m_ha );
		const float h = s_scr.h( s_dv.f( "compassPlayerHeight" ) * s_dv.f( "compassSize" ), o.m_va );
		const float angle = s_dv.b( "compassRotation" ) ? 0.f : angle_delta( k_north_yaw, s.m_yaw );
		rotated_pic( o.m_material, r[ 0 ] + r[ 2 ] * 0.5f - w * 0.5f, r[ 1 ] + r[ 3 ] * 0.5f - h * 0.5f, w, h, angle, o.m_color );
	}

	// 0x427e00 friendlies, 0x4284d0 enemies (radar sweep + sound pings)
	void od_compass_actors( const od_t& o, const n_cod4_hud::snapshot_t& s, bool enemies, int now )
	{
		float r[ 4 ];
		compass_rect( o, r );
		float north[ 2 ];
		compass_basis( s, north );
		const float me[ 2 ]{ s.m_origin[ 0 ], s.m_origin[ 1 ] };
		const float cx = r[ 0 ] + r[ 2 ] * 0.5f, cy = r[ 1 ] + r[ 3 ] * 0.5f;
		const float w = s_scr.w( s_dv.f( "compassFriendlyWidth" ) * s_dv.f( "compassSize" ), o.m_ha );
		const float h = s_scr.h( s_dv.f( "compassFriendlyHeight" ) * s_dv.f( "compassSize" ), o.m_va );
		const float sound_fade = s_dv.f( "compassSoundPingFadeTime" ) * 1000.f, radar_fade = s_dv.f( "compassRadarPingFadeTime" ) * 1000.f;
		const bool show_all    = s_dv.b( "g_compassShowEnemies" );

		for ( const n_cod4_hud::actor_t& a : s.m_actors ) {
			if ( a.m_enemy != enemies )
				continue;
			const actor_state_t& st = s_actor_state[ a.m_index ];
			float pos[ 2 ]{ a.m_pos[ 0 ], a.m_pos[ 1 ] };
			float sound_a = 0.f, radar_a = 0.f;

			if ( enemies ) {
				if ( st.m_fire_time && static_cast< float >( now ) < static_cast< float >( st.m_fire_time ) + sound_fade )
					sound_a = st.m_fire_time < now ? 1.f - static_cast< float >( now - st.m_fire_time ) / sound_fade : 1.f;
				if ( show_all )
					radar_a = 1.f;
				else if ( st.m_ping_time && now < st.m_ping_time + radar_fade ) {
					radar_a = st.m_ping_time < now ? 1.f - static_cast< float >( now - st.m_ping_time ) / radar_fade : 1.f;
					pos[ 0 ] = st.m_ping[ 0 ], pos[ 1 ] = st.m_ping[ 1 ];
				}
				if ( sound_a <= 0.f && radar_a <= 0.f )
					continue;
			}

			float p[ 2 ];
			const float local_rect[ 4 ]{ 0.f, 0.f, r[ 2 ], r[ 3 ] };
			if ( world_to_compass( local_rect, s_dv.f( "compassMaxRange" ), north, me, pos, p ) && !s_dv.b( "compassClampIcons" ) )
				continue;
			const float x = cx + p[ 0 ] - w * 0.5f, y = cy + p[ 1 ] - h * 0.5f;

			if ( enemies ) {
				float c[ 4 ]{ 1.f, 1.f, 1.f, 1.f };
				if ( radar_a > 0.f ) {
					c[ 3 ] = radar_a * o.m_color[ 3 ];
					pic( mat( "compassping_enemy" ), x, y, w, h, c );
				}
				if ( sound_a > 0.f ) {
					c[ 3 ] = sound_a * o.m_color[ 3 ];
					pic( mat( "compassping_enemyfiring" ), x, y, w, h, c );
				}
				continue;
			}

			const float angle = angle_normalize_360( ( s_dv.b( "compassRotation" ) ? s.m_yaw : k_north_yaw ) - a.m_yaw );
			// firing (0x73ab3c): compassping_friendlyfiring_mp over the base icon, alpha 1 -> 0 over compassSoundPingFadeTime,
			// the base icon skipped while that overlay is fully up
			float firing_a = 0.f;
			if ( st.m_fire_time && static_cast< float >( now ) < static_cast< float >( st.m_fire_time ) + sound_fade )
				firing_a = st.m_fire_time < now ? 1.f - static_cast< float >( now - st.m_fire_time ) / sound_fade : 1.f;
			firing_a = std::min( firing_a, o.m_color[ 3 ] );
			if ( firing_a != 1.f )
				rotated_pic( mat( "compassping_friendly_mp" ), x, y, w, h, angle, o.m_color );
			if ( firing_a > 0.f ) {
				const float c[ 4 ]{ o.m_color[ 0 ], o.m_color[ 1 ], o.m_color[ 2 ], firing_a };
				rotated_pic( mat( "compassping_friendlyfiring_mp" ), x, y, w, h, angle, c );
			}
		}
	}

	// 0x428930 radar line while a UAV is up, 0x427a20 sweep timing
	void od_radar_line( const od_t& o, const n_cod4_hud::snapshot_t& s, int now )
	{
		if ( now >= s_rv.m_radar_until )
			return;
		float r[ 4 ];
		compass_rect( o, r );
		const float thick = s_dv.f( "compassRadarLineThickness" );
		if ( thick <= 0.f )
			return;
		// the line crosses the compass square left -> right in screen space, one pass per compassRadarUpdateTime
		const float u = ( 0.5f - s_sweep ) / thick + 0.5f;
		const texture_t* t = texture( mat( "compass_radarline" ) );
		if ( !t )
			return;
		const float white[ 4 ]{ 1.f, 1.f, 1.f, o.m_color[ 3 ] };
		blend_scope_t blend( mat( "compass_radarline" ) );
		s_list->AddCallback( cb_address, reinterpret_cast< void* >( static_cast< std::uintptr_t >( D3DTADDRESS_BORDER ) ) );
		s_list->AddImage( reinterpret_cast< ImTextureID >( t->m_texture ), ImVec2( r[ 0 ], r[ 1 ] ), ImVec2( r[ 0 ] + r[ 2 ], r[ 1 ] + r[ 3 ] ), ImVec2( u, 0.f ),
		                  ImVec2( u + 1.f / thick, 1.f ), col( white ) );
		s_list->AddCallback( cb_address_restore, nullptr );
		( void )s;
	}

	// 0x426d60 objectives: bomb sites from the overview txt, edge-clamped like cod4's waypoints
	void od_compass_objectives( const od_t& o, const n_cod4_hud::snapshot_t& s )
	{
		if ( !s_dv.b( "ui_hud_showobjicons" ) || !s_overview.m_texture )
			return;
		static std::string s_sites_map{ };
		static float s_sites[ 2 ][ 2 ]{ };
		static bool s_have_sites = false;
		if ( s_sites_map != s.m_map ) {
			s_sites_map  = s.m_map;
			s_have_sites = false;
			char exe[ MAX_PATH ]{ };
			GetModuleFileNameA( nullptr, exe, MAX_PATH );
			std::ifstream txt( std::filesystem::path( exe ).parent_path( ) / "csgo" / "resource" / "overviews" / ( s.m_map + ".txt" ) );
			float v[ 4 ]{ -1, -1, -1, -1 };
			for ( std::string line; std::getline( txt, line ); ) {
				const char* keys[ 4 ] = { "\"bombA_x\"", "\"bombA_y\"", "\"bombB_x\"", "\"bombB_y\"" };
				for ( int k = 0; k < 4; ++k ) {
					const size_t at = line.find( keys[ k ] );
					if ( at == std::string::npos )
						continue;
					const size_t q = line.find( '"', at + std::strlen( keys[ k ] ) );
					if ( q != std::string::npos )
						v[ k ] = static_cast< float >( std::atof( line.c_str( ) + q + 1 ) );
				}
			}
			if ( v[ 0 ] >= 0.f && v[ 2 ] >= 0.f ) {
				const float span = 1024.f * s_overview.m_scale;
				for ( int i = 0; i < 2; ++i ) {
					s_sites[ i ][ 0 ] = s_overview.m_pos_x + v[ i * 2 ] * span;
					s_sites[ i ][ 1 ] = s_overview.m_pos_y - v[ i * 2 + 1 ] * span;
				}
				s_have_sites = true;
			}
		}
		if ( !s_have_sites )
			return;

		float r[ 4 ];
		compass_rect( o, r );
		float north[ 2 ];
		compass_basis( s, north );
		const float me[ 2 ]{ s.m_origin[ 0 ], s.m_origin[ 1 ] };
		const float w = s_scr.w( s_dv.f( "compassObjectiveWidth" ) * s_dv.f( "compassSize" ), o.m_ha );
		const float h = s_scr.h( s_dv.f( "compassObjectiveHeight" ) * s_dv.f( "compassSize" ), o.m_va );
		const bool attack = s.m_team == 2; // T plants: red target, CT defends: green
		for ( int i = 0; i < 2; ++i ) {
			float p[ 2 ];
			const float local_rect[ 4 ]{ 0.f, 0.f, r[ 2 ], r[ 3 ] };
			world_to_compass( local_rect, s_dv.f( "compassMaxRange" ), north, me, s_sites[ i ], p );
			const char* name = attack ? ( i ? "compass_waypoint_target_b" : "compass_waypoint_target_a" ) : ( i ? "compass_waypoint_defend_b" : "compass_waypoint_defend_a" );
			// 0x426d60 alpha: full inside compassMaxRange, down to compassObjectiveMinAlpha at compassObjectiveMaxRange
			const float dx = s_sites[ i ][ 0 ] - me[ 0 ], dy = s_sites[ i ][ 1 ] - me[ 1 ];
			const float dist = std::sqrt( dx * dx + dy * dy ), maxr = s_dv.f( "compassMaxRange" ), objr = s_dv.f( "compassObjectiveMaxRange" );
			float a = 1.f;
			if ( dist > maxr )
				a = dist >= objr ? s_dv.f( "compassObjectiveMinAlpha" ) : 1.f - ( dist - maxr ) / std::max( 1.f, objr - maxr ) * ( 1.f - s_dv.f( "compassObjectiveMinAlpha" ) );
			const float c[ 4 ]{ 1.f, 1.f, 1.f, std::min( a, o.m_color[ 3 ] ) };
			pic( mat( name ), r[ 0 ] + r[ 2 ] * 0.5f + p[ 0 ] - w * 0.5f, r[ 1 ] + r[ 3 ] * 0.5f + p[ 1 ] - h * 0.5f, w, h, c );
		}
	}

	// low ammo pulse (0x4210a0): lerp toward (1, .3, .3) by 0.5 + 0.5 sin(time)
	void low_ammo_color( float c[ 4 ], int now )
	{
		const float f = 0.5f + 0.5f * std::sin( static_cast< float >( now ) * 0.00628318544f * s_dv.f( "lowAmmoWarningPulseFreq" ) );
		c[ 0 ] += f * ( 1.f - c[ 0 ] );
		c[ 1 ] += f * ( 0.3f - c[ 1 ] );
		c[ 2 ] += f * ( 0.3f - c[ 2 ] );
	}

	// 0x4221c0 -> 0x421680: bullets right to left from the rect origin, spent rounds 0.3 grey
	void od_clip_graphic( const od_t& o, const n_cod4_hud::snapshot_t& s, int now )
	{
		if ( s.m_weapon < 0 || s.m_clip_max <= 0 )
			return;
		const weapon_t& w = k_weapons[ s.m_weapon ];
		struct clip_t {
			const char* m_material;
			float m_dims[ 4 ]; // w h step start
		};
		// magazine 0x4211b0, short magazine 0x421280, shotgun 0x421350, rocket 0x421430
		static constexpr clip_t k_clips[] = { { nullptr, { } },
			                                  { "ammo_counter_bullet", { 4.f, 8.f, 4.f, 4.f } },
			                                  { "ammo_counter_riflebullet", { 32.f, 8.f, 40.f, 32.f } },
			                                  { "ammo_counter_shotgunshell", { 16.f, 8.f, 20.f, 16.f } },
			                                  { "ammo_counter_rocket", { 64.f, 16.f, 72.f, 64.f } } };
		int type = w.m_clip;
		if ( type == 6 || type == 0 )
			type = 1;

		const bool low = static_cast< float >( s.m_clip ) <= w.m_low_ammo * static_cast< float >( s.m_clip_max );
		float c[ 4 ]{ o.m_color[ 0 ], o.m_color[ 1 ], o.m_color[ 2 ], o.m_color[ 3 ] };
		if ( low )
			low_ammo_color( c, now );

		const int count = std::min( s.m_clip_max, 200 );
		if ( type == 5 ) {
			// beltfed 0x421510: 8x4 rounds in rows of 20, each row 2 up and running the other way
			const int m = mat( "ammo_counter_beltbullet" );
			float step = 8.f, x = o.m_x, y = 4.f * 0.25f * static_cast< float >( count / 20 ) + o.m_y;
			for ( int i = 0; i < count; ++i ) {
				if ( i == s.m_clip )
					c[ 0 ] = 0.3f, c[ 1 ] = 0.3f, c[ 2 ] = 0.3f;
				if ( i % 20 == 0 ) {
					step = -step;
					y += -2.f;
					x += step;
				}
				pic( m, s_scr.x( x, o.m_ha ), s_scr.y( y, o.m_va ), s_scr.w( 8.f, o.m_ha ), s_scr.h( 4.f, o.m_va ), c );
				x += step;
			}
			return;
		}

		const clip_t& k = k_clips[ type ];
		const int m     = mat( k.m_material );
		float x         = o.m_x - k.m_dims[ 3 ];
		const float y   = type == 4 ? o.m_y - k.m_dims[ 1 ] * 0.5f : type == 3 ? o.m_y - k.m_dims[ 1 ] * 0.5f : o.m_y - 4.f;
		for ( int i = 0; i < count; ++i ) {
			if ( i == s.m_clip )
				c[ 0 ] = 0.3f, c[ 1 ] = 0.3f, c[ 2 ] = 0.3f;
			pic( m, s_scr.x( x, o.m_ha ), s_scr.y( y, o.m_va ), s_scr.w( k.m_dims[ 0 ], o.m_ha ), s_scr.h( k.m_dims[ 1 ], o.m_va ), c );
			x -= k.m_dims[ 2 ];
		}
	}

	// 0x421760 ammo stock: reserve count, red when low (0x43e060: reserve <= 20% of max)
	void od_ammo_stock( const od_t& o, const n_cod4_hud::snapshot_t& s )
	{
		if ( s.m_weapon < 0 || s.m_clip_max <= 0 )
			return;
		float c[ 4 ]{ o.m_color[ 0 ], o.m_color[ 1 ], o.m_color[ 2 ], o.m_color[ 3 ] };
		if ( static_cast< double >( std::min( s.m_reserve_max, 999 ) ) * 0.2 >= static_cast< double >( std::min( s.m_reserve, 999 ) ) )
			c[ 0 ] = 1.f, c[ 1 ] = 0.3f, c[ 2 ] = 0.3f;
		ui_text( std::to_string( std::min( s.m_reserve, 999 ) ), o.m_x, o.m_y, o.m_ha, o.m_va, o.m_font, o.m_scale, c, o.m_style );
	}

	// 0x43e7f0 weapon name: 1800 ms after a switch, last 700 fading, right aligned 28 left of the rect
	void od_weapon_name( const od_t& o, const n_cod4_hud::snapshot_t& s, int now )
	{
		const int since = now - s_rv.m_weapon_switch;
		if ( since >= 1800 || s.m_weapon_name.empty( ) )
			return;
		const int left = 1800 - since;
		float c[ 4 ]{ o.m_color[ 0 ], o.m_color[ 1 ], o.m_color[ 2 ], left < 700 ? static_cast< float >( left ) / 700.f : 1.f };
		const float w = ui_text_width( s.m_weapon_name, o.m_font, o.m_scale );
		ui_text( s.m_weapon_name, o.m_x + o.m_w - w - 28.f, o.m_y, o.m_ha, o.m_va, o.m_font, o.m_scale, c, o.m_style );
	}

	// 0x456c10 offhand icon (the slot's grenade hudIcon), 0x456fd0 count, red (0.89, 0.18, 0.01) at 0
	void od_offhand( const od_t& o, const n_cod4_hud::snapshot_t& s, bool frag, bool count )
	{
		const int weapon = frag ? s.m_frag_weapon : s.m_smoke_weapon;
		const int n      = frag ? s.m_frag_count : s.m_smoke_count;
		if ( weapon < 0 )
			return;
		if ( !count ) {
			float r[ 4 ];
			real_rect( o, r );
			const std::uint32_t cp = frag ? s.m_frag_glyph : s.m_smoke_glyph;
			if ( const float w = weapon_glyph( cp, 0.f, 0.f, r[ 3 ], 0.f, false ); w > 0.f ) {
				weapon_glyph( cp, r[ 0 ] + ( r[ 2 ] - w ) * 0.5f, r[ 1 ], r[ 3 ], o.m_color[ 3 ], true );
				return;
			}
			pic( mat( k_weapons[ weapon ].m_hud_icon ? k_weapons[ weapon ].m_hud_icon : "" ), r[ 0 ], r[ 1 ], r[ 2 ], r[ 3 ], o.m_color );
			return;
		}
		float c[ 4 ]{ o.m_color[ 0 ], o.m_color[ 1 ], o.m_color[ 2 ], o.m_color[ 3 ] };
		if ( !n )
			c[ 0 ] = 0.89f, c[ 1 ] = 0.18f, c[ 2 ] = 0.01f;
		ui_text( std::to_string( n ), o.m_x, o.m_y, o.m_ha, o.m_va, o.m_font, o.m_scale, c, o.m_style );
	}

	// 0x421d90 dpad: slot 3 = c4, slot 4 = held hardpoint (dpadIcon); 1.2x when active
	void od_dpad( const od_t& o, const n_cod4_hud::snapshot_t& s, int slot )
	{
		const char* icon = nullptr;
		if ( slot == 3 && s.m_has_c4 )
			icon = "hud_icon_c4";
		if ( slot == 4 && s_rv.m_hardpoint >= 0 )
			if ( const weapon_t* w = weapon_for( k_hardpoints[ s_rv.m_hardpoint ] ) )
				icon = w->m_dpad_icon;
		if ( !icon )
			return;
		float r[ 4 ];
		real_rect( o, r );
		if ( slot == 3 )
			if ( const float w = weapon_glyph( s.m_c4_glyph, 0.f, 0.f, r[ 3 ], 0.f, false ); w > 0.f ) {
				weapon_glyph( s.m_c4_glyph, r[ 0 ] + ( r[ 2 ] - w ) * 0.5f, r[ 1 ], r[ 3 ], o.m_color[ 3 ], true );
				return;
			}
		pic( mat( icon ), r[ 0 ], r[ 1 ], r[ 2 ], r[ 3 ], o.m_color );
	}

	// 0x43fef0 low health overlay
	void od_low_health( const od_t& o, const n_cod4_hud::snapshot_t& s, int now )
	{
		if ( !s.m_alive || s.m_health <= 0 || s.m_max_health <= 0 )
			return;
		s_health.update( std::clamp( static_cast< float >( s.m_health ) / static_cast< float >( s.m_max_health ), 0.f, 1.f ), now, s_dv );
		const float a = s_health.alpha( now );
		if ( a <= 0.f )
			return;
		float r[ 4 ];
		real_rect( o, r );
		const float c[ 4 ]{ o.m_color[ 0 ], o.m_color[ 1 ], o.m_color[ 2 ], a };
		pic( o.m_material, r[ 0 ], r[ 1 ], r[ 2 ], r[ 3 ], c );
	}

	// 0x42ba70 center print: cg_centertime, last 100 ms fading, centered
	void od_center_print( const od_t& o, int now )
	{
		std::string msg;
		int start;
		{
			msg   = s_rv.m_center;
			start = s_rv.m_center_time;
		}
		if ( msg.empty( ) || now < start )
			return;
		const int dur = static_cast< int >( s_dv.f( "cg_centertime" ) * 1000.f );
		if ( now - start >= dur )
			return;
		const int left = dur - ( now - start );
		const float c[ 4 ]{ o.m_color[ 0 ], o.m_color[ 1 ], o.m_color[ 2 ], o.m_color[ 3 ] * ( left < 100 ? static_cast< float >( left ) / 100.f : 1.f ) };
		const float w = ui_text_width( msg, 0, o.m_scale );
		ui_text( msg, o.m_x - std::floor( w * 0.5f + 0.5f ), o.m_y, o.m_ha, o.m_va, 0, o.m_scale, c, o.m_style );
	}

	// holdbreathhint ownerdraw 71: scoped sniper, PLATFORM_HOLD_BREATH with the walk key
	void od_hold_breath( const od_t& o, const n_cod4_hud::snapshot_t& s )
	{
		if ( !s.m_scoped || !s.m_sniper || !s_dv.b( "cg_drawBreathHint" ) )
			return;
		const std::string hint = localize( "PLATFORM_HOLD_BREATH", { "SHIFT" } );
		const float w          = ui_text_width( hint, 0, o.m_scale );
		const float c[ 4 ]{ 1.f, 1.f, 1.f, 1.f };
		ui_text( hint, o.m_x - w * 0.5f, o.m_y, o.m_ha, o.m_va, 0, o.m_scale, c, o.m_style );
	}

	void ownerdraw( int id, const od_t& o, const n_cod4_hud::snapshot_t& s, int now )
	{
		switch ( id ) {
		case 71: return od_hold_breath( o, s );
		case 81: return od_weapon_name( o, s, now );
		case 90: return od_center_print( o, now );
		case 103: return od_offhand( o, s, true, false );
		case 104: return od_offhand( o, s, false, false );
		case 105: return od_offhand( o, s, true, true );
		case 106: return od_offhand( o, s, false, true );
		case 112: return od_low_health( o, s, now );
		case 117: return od_clip_graphic( o, s, now );
		case 119: return od_ammo_stock( o, s );
		case 146: return od_ticker( o, s );
		case 150: return od_compass_player( o, s );
		case 151: {
			float r[ 4 ];
			compass_rect( o, r );
			const float white[ 4 ]{ 1.f, 1.f, 1.f, o.m_color[ 3 ] };
			return pic( o.m_material, r[ 0 ], r[ 1 ], r[ 2 ], r[ 3 ], white );
		}
		case 152: return od_compass_objectives( o, s );
		case 158: return od_compass_actors( o, s, false, now );
		case 159:
			od_compass_map( o, s );
			return od_radar_line( o, s, now );
		case 166: return; // night vision: no csgo counterpart
		case 168: return od_dpad( o, s, 3 );
		case 169: return od_dpad( o, s, 4 );
		case 170: return od_compass_actors( o, s, true, now );
		default: return; // 20 stance (parked offscreen in mp), 155/156 helicopters / planes, 193-196 / 266 voice
		}
	}

	/* ---- menus (Menu_Paint for the hud list) ---- */
	void paint_menus( ctx_t& ctx, const n_cod4_hud::snapshot_t& s, int now, int from, int to )
	{
		for ( int mi = from; mi < to; ++mi ) {
			const menu_t& m = k_menus[ mi ];
			// warmup = cod4's pre-match: no "Tied" score bar
			if ( ( s.m_warmup && !std::strcmp( m.m_name, "scorebar" ) ) || !visible( m.m_visible, ctx ) )
				continue;
			ctx.m_vitals   = !std::strcmp( m.m_name, "scorebars" );
			const float my = m.m_exp_y ? static_cast< float >( evaluate( m.m_exp_y, ctx, value_t::num( m.m_rect[ 1 ] ) ).number( ) ) : m.m_rect[ 1 ];

			for ( int ii = m.m_first; ii < m.m_first + m.m_count; ++ii ) {
				const menu_item_t& it = k_items[ ii ];
				if ( !visible( it.m_visible, ctx ) )
					continue;

				float rect[ 4 ]{ it.m_rect[ 0 ], it.m_rect[ 1 ], it.m_rect[ 2 ], it.m_rect[ 3 ] };
				const char* exps[ 4 ] = { it.m_exp_x, it.m_exp_y, it.m_exp_w, it.m_exp_h };
				for ( int k = 0; k < 4; ++k )
					if ( exps[ k ] )
						rect[ k ] = static_cast< float >( evaluate( ctx.m_vitals ? vitals_expr( exps[ k ] ) : exps[ k ], ctx, value_t::num( rect[ k ] ) ).number( ) );

				od_t o{ };
				o.m_x = m.m_rect[ 0 ] + rect[ 0 ], o.m_y = my + rect[ 1 ], o.m_w = rect[ 2 ], o.m_h = rect[ 3 ];
				o.m_ha = it.m_horz_align ? it.m_horz_align : m.m_horz_align;
				o.m_va = it.m_vert_align ? it.m_vert_align : m.m_vert_align;
				std::memcpy( o.m_color, it.m_forecolor, sizeof( o.m_color ) );
				if ( it.m_exp_alpha )
					o.m_color[ 3 ] = static_cast< float >( evaluate( it.m_exp_alpha, ctx, value_t::num( o.m_color[ 3 ] ) ).number( ) );
				o.m_material = it.m_background ? mat( it.m_background ) : -1;
				if ( it.m_exp_material )
					o.m_material = mat( evaluate( it.m_exp_material, ctx, value_t::str( "" ) ).text( ) );
				o.m_scale = it.m_text_scale, o.m_font = it.m_text_font, o.m_style = it.m_text_style;

				if ( it.m_type == 8 ) {
					ownerdraw( it.m_ownerdraw, o, s, now );
					continue;
				}
				if ( it.m_type == 19 ) {
					// game message window (0x45d740), mode 1 = newest at the bottom stacking up
					msg_window_t& w   = s_windows[ std::clamp( it.m_window_index, 0, 1 ) ];
					const std::string p = "con_gameMsgWindow" + std::to_string( std::clamp( it.m_window_index, 0, 1 ) );
					const int lh       = msg_line_height( it.m_text_scale );
					w.expire( now );
					const int font = pick_font( it.m_text_font, it.m_text_scale, s_scr.m_sy, s_dv );
					const float k  = static_cast< float >( lh ) / static_cast< float >( k_fonts[ font ].m_pixel_height );
					// typed line one row under the chat window's newest message
					if ( it.m_window_index == 0 && !s_chat_line.empty( ) ) {
						const float white[ 4 ]{ 1.f, 1.f, 1.f, 1.f };
						text( font, s_chat_line, std::floor( s_scr.x( o.m_x, o.m_ha ) + 0.5f ), std::floor( s_scr.y( o.m_y + static_cast< float >( lh ), o.m_va ) + 0.5f ),
						      s_scr.w( k, o.m_ha ), s_scr.h( k, o.m_va ), white, it.m_text_style );
					}
					for ( const auto& row : w.rows_up( now, static_cast< int >( o.m_y ), lh,static_cast< int >( s_dv.f( ( p + "ScrollTime" ).c_str( ) ) * 1000.f ),
					                                   static_cast< int >( s_dv.f( ( p + "FadeInTime" ).c_str( ) ) * 1000.f ),
					                                   static_cast< int >( s_dv.f( ( p + "FadeOutTime" ).c_str( ) ) * 1000.f ) ) ) {
						const float white[ 4 ]{ 1.f, 1.f, 1.f, row.m_alpha };
						float x = o.m_x;
						if ( it.m_text_align == 1 )
							x -= static_cast< float >( text_width( row.m_line->m_text, font ) ) * k * 0.5f;
						else if ( it.m_text_align == 2 )
							x -= static_cast< float >( text_width( row.m_line->m_text, font ) ) * k;
						text( font, row.m_line->m_text, std::floor( s_scr.x( x, o.m_ha ) + 0.5f ), std::floor( s_scr.y( static_cast< float >( row.m_y ), o.m_va ) + 0.5f ),
						      s_scr.w( k, o.m_ha ), s_scr.h( k, o.m_va ), white, it.m_text_style );
					}
					continue;
				}

				// WINDOW_STYLE_SHADER: background * forecolor over the rect
				if ( it.m_style == 3 && o.m_material >= 0 && o.m_w > 0.f && o.m_h != 0.f ) {
					// a negative exp rect H (scorebars backdrop when losing) draws flipped upward, as in the game
					float r[ 4 ];
					real_rect( o, r );
					pic( o.m_material, r[ 0 ], r[ 1 ], r[ 2 ], r[ 3 ], o.m_color );
				}

				const char* src = it.m_exp_text;
				std::string label = src ? evaluate( src, ctx, value_t::str( "" ) ).text( ) : it.m_text ? localize( it.m_text ) : std::string( );
				if ( !label.empty( ) ) {
					float x = o.m_x;
					const float w = ui_text_width( label, it.m_text_font, it.m_text_scale );
					if ( it.m_text_align == 1 )
						x += ( o.m_w - w ) * 0.5f;
					else if ( it.m_text_align == 2 )
						x += o.m_w - w;
					ui_text( label, x, o.m_y, o.m_ha, o.m_va, it.m_text_font, it.m_text_scale, o.m_color, it.m_text_style );
				}
			}
		}
	}

	/* ---- code drawn parts ---- */
	// CG_DrawCrosshair (0x42f6e0): reticleCenter + 4 reticleSide pieces pushed out by the spread
	void draw_crosshair( const n_cod4_hud::snapshot_t& s )
	{
		if ( !s_dv.b( "cg_drawCrosshair" ) || !s.m_alive || s.m_scoped || s.m_weapon < 0 )
			return;
		const weapon_t& w      = k_weapons[ s.m_weapon ];
		const float spread_deg = s.m_spread_min + ( s.m_spread_max - s.m_spread_min ) * s.m_spread_scale / 255.f;

		float c[ 4 ]{ 1.f, 1.f, 1.f, 1.f };
		c[ 3 ] = reticle_alpha( s_dv.f( "cg_crosshairAlpha" ), 1.f, s.m_spread_scale, s_dv.f( "cg_crosshairAlphaMin" ) );
		if ( c[ 3 ] < 0.01f )
			return;

		const float cx = s_scr.m_w * 0.5f, cy = s_scr.m_h * 0.5f;
		if ( w.m_reticle_center ) {
			const float size = s_scr.w( static_cast< float >( w.m_reticle_center_size ), 2 );
			pic( mat( w.m_reticle_center ), cx - size * 0.5f, cy - size * 0.5f, size, size, c );
		}
		if ( !w.m_reticle_side )
			return;
		// 0x42f4a0: size = reticleSideSize, offset from 0x42f370, top / right / bottom / left at 0 90 180 270 with an s/8 nudge
		const float ofs = reticle_offset( spread_deg, s.m_tan_half_fov_y, w.m_reticle_min_ofs );
		const float sz = static_cast< float >( w.m_reticle_side_size ), n = 0.125f * sz;
		const int m    = mat( w.m_reticle_side );
		const auto at  = [ & ]( float vx, float vy, float angle ) { rotated_pic( m, s_scr.x( vx, 2 ), s_scr.y( vy, 2 ), s_scr.w( sz, 2 ), s_scr.h( sz, 2 ), angle, c ); };
		at( -0.5f * sz, -sz - ofs - n, 0.f );
		at( ofs, -0.5f * sz, 90.f );
		at( -0.5f * sz - n, ofs, 180.f );
		at( -sz - ofs - n, -0.5f * sz - n, 270.f );
	}

	// ads overlay: adsOverlayShader at adsOverlayWidth x adsOverlayHeight centred, black past its edges
	void draw_scope( const n_cod4_hud::snapshot_t& s )
	{
		if ( !s.m_scoped || s.m_weapon < 0 || !k_weapons[ s.m_weapon ].m_ads_overlay )
			return;
		const weapon_t& w = k_weapons[ s.m_weapon ];
		const float x0 = std::floor( s_scr.x( -0.5f * static_cast< float >( w.m_ads_overlay_w ), 2 ) );
		const float y0 = std::floor( s_scr.y( -0.5f * static_cast< float >( w.m_ads_overlay_h ), 2 ) );
		const float x1 = std::ceil( s_scr.x( 0.5f * static_cast< float >( w.m_ads_overlay_w ), 2 ) );
		const float y1 = std::ceil( s_scr.y( 0.5f * static_cast< float >( w.m_ads_overlay_h ), 2 ) );
		const ImU32 black = IM_COL32( 0, 0, 0, 255 );
		s_list->AddRectFilled( ImVec2( 0.f, 0.f ), ImVec2( x0, s_scr.m_h ), black );
		s_list->AddRectFilled( ImVec2( x1, 0.f ), ImVec2( s_scr.m_w, s_scr.m_h ), black );
		s_list->AddRectFilled( ImVec2( x0, 0.f ), ImVec2( x1, y0 ), black );
		s_list->AddRectFilled( ImVec2( x0, y1 ), ImVec2( x1, s_scr.m_h ), black );
		const float white[ 4 ]{ 1.f, 1.f, 1.f, 1.f };
		pic( mat( w.m_ads_overlay ), x0, y0, x1 - x0, y1 - y0, white );
	}

	// CG_DrawDamageDirectionIndicators (0x42ac90): hit_direction below center turned toward the hit, 2 - 2 t / time alpha
	void draw_damage( const n_cod4_hud::snapshot_t& s, int now )
	{
		if ( s.m_scoped && !s_dv.b( "cg_hudDamageIconInScope" ) )
			return;
		const int m = mat( "hit_direction" );
		const float w = s_scr.w( s_dv.f( "cg_hudDamageIconWidth" ), 2 ), h = s_scr.h( s_dv.f( "cg_hudDamageIconHeight" ), 2 );
		const float ofs = s_scr.h( s_dv.f( "cg_hudDamageIconOffset" ), 2 );
		const texture_t* t = texture( m );
		if ( !t )
			return;
		const float cx = s_scr.m_w * 0.5f, cy = s_scr.m_h * 0.5f;
		for ( const damage_slot_t& d : s_damage ) {
			const int t0 = now - d.m_time;
			if ( t0 <= 0 || t0 >= d.m_duration )
				continue;
			const float a = std::min( 1.f, 2.f - 2.f * static_cast< float >( t0 ) / static_cast< float >( d.m_duration ) );
			const float ang = ( s.m_yaw - d.m_yaw ) * 0.0174532924f, c = std::cos( ang ), sn = std::sin( ang );
			const auto p    = [ & ]( float x, float y ) { return ImVec2( cx + x * c - y * sn, cy + x * sn + y * c ); };
			const float col4[ 4 ]{ 1.f, 1.f, 1.f, a };
			s_list->AddImageQuad( reinterpret_cast< ImTextureID >( t->m_texture ), p( -w * 0.5f, ofs ), p( w * 0.5f, ofs ), p( w * 0.5f, ofs + h ), p( -w * 0.5f, ofs + h ),
			                      ImVec2( 0, 0 ), ImVec2( 1, 0 ), ImVec2( 1, 1 ), ImVec2( 0, 1 ), col( col4 ) );
		}
	}

	// _damagefeedback.gsc: damage_feedback 24x48 at center -12 -12, alpha 1 fading over 1 s
	void draw_hit_marker( int now )
	{
		const int since = now - s_rv.m_hit_time;
		if ( since < 0 || since >= 1000 )
			return;
		const float c[ 4 ]{ 1.f, 1.f, 1.f, 1.f - static_cast< float >( since ) / 1000.f };
		pic( mat( "damage_feedback" ), s_scr.x( -12.f, 2 ), s_scr.y( -12.f, 2 ), s_scr.w( 24.f, 2 ), s_scr.h( 48.f, 2 ), c );
	}

	// _rank.gsc updateRankScoreHUD: "+N" center / middle at y -60, default font, fontscale 2.0 (12 virtual px per fontscale),
	// color (1, 1, .5) or red when negative, alpha .85, held 1 s then faded over .75 s; _hud.gsc fontPulse: x2 over 3 frames
	// of 50 ms, back over 5
	void draw_xp( int now )
	{
		const int since = now - s_rv.m_xp_time;
		if ( since < 0 || since >= 1750 || !s_rv.m_xp_shown || !s_dv.b( "onlinegame" ) )
			return;
		float alpha = 0.85f;
		if ( since > 1000 )
			alpha *= 1.f - static_cast< float >( since - 1000 ) / 750.f;
		const int frame = since / 50;
		float pulse     = 1.f;
		if ( frame < 3 )
			pulse = 1.f + static_cast< float >( frame + 1 ) / 3.f;
		else if ( frame < 8 )
			pulse = 2.f - static_cast< float >( frame - 2 ) / 5.f;
		const float scale = 2.f * pulse * 12.f / 48.f;
		const bool neg    = s_rv.m_xp_shown < 0;
		const std::string label = ( neg ? std::string( ) : localize( "MP_PLUS" ) ) + std::to_string( s_rv.m_xp_shown );
		const float c[ 4 ]{ 1.f, neg ? 0.f : 1.f, neg ? 0.f : 0.5f, alpha };
		const float w = ui_text_width( label, 0, scale );
		ui_text( label, -w * 0.5f, -60.f + 12.f * 2.f * pulse * 0.5f, 2, 2, 0, scale, c, 3 );
	}

	// _hud_message.gsc notifyMessage (not splitscreen): objective font, title fontscale 2.5 / text 1.75 (12 virtual px per
	// fontscale, _hud.gsc fontHeight), title TOP at screen top + 30, text TOP under its BOTTOM, glow (0.3, 0.6, 0.3) alpha 1,
	// held `duration` then setPulseFX's 1 s decay
	void draw_notify( int now )
	{
		if ( s_notifies.empty( ) )
			return;
		notify_t& n = s_notifies.front( );
		if ( !n.m_start )
			n.m_start = now;
		const int since = now - n.m_start;
		if ( since >= n.m_duration + 1000 ) {
			s_notifies.pop_front( );
			return;
		}
		float a = 1.f;
		if ( since > n.m_duration )
			a = 1.f - static_cast< float >( since - n.m_duration ) / 1000.f;
		// ponytail: setPulseFX(100, ...) types one letter per 100 ms; the random decode glyphs while typing are not drawn
		const auto typed = [ & ]( const std::string& t ) { return t.substr( 0, std::min( t.size( ), static_cast< size_t >( since / 100 + 1 ) ) ); };
		const float glow[ 4 ]{ 0.3f, 0.6f, 0.3f, 1.f }, white[ 4 ]{ 1.f, 1.f, 1.f, a };
		const float title_h = 2.5f * 12.f, text_h = 1.75f * 12.f;
		ui_text( typed( n.m_title ), -ui_text_width( n.m_title, 6, title_h / 48.f ) * 0.5f, 30.f + title_h, 2, 1, 6, title_h / 48.f, white, 0, glow );
		ui_text( typed( n.m_text ), -ui_text_width( n.m_text, 6, text_h / 48.f ) * 0.5f, 30.f + title_h + text_h, 2, 1, 6, text_h / 48.f, white, 0, glow );
	}

	/* ---- options ---- */
	// options_game.menu Draw Crosshair + the hardcore game mode; older builds' other dvar overrides drop back to stock
	constexpr const char* k_user_dvars[] = { "cg_drawCrosshair", "ui_hud_hardcore" };

	void sync_dvars( )
	{
		const std::string& text = GET_VARIABLE( g_variables.m_cod4_hud_dvars, std::string );
		if ( text != s_dv_text ) {
			s_dv_text = text;
			std::string kept{ };
			for ( size_t p = 0; p < text.size( ); ) {
				const size_t e         = std::min( text.find( '\n', p ), text.size( ) );
				const std::string line = text.substr( p, e - p );
				p                      = e + 1;
				for ( const char* name : k_user_dvars )
					if ( line.rfind( std::string( name ) + ' ', 0 ) == 0 )
						kept += line + '\n';
			}
			s_dv.load( kept );
			// faction colors follow scr_allies / scr_axis (_scoreboard.gsc)
			for ( const char* side : { "allies", "axis" } ) {
				const std::string faction = s_dv.s( std::string( "scr_" ) + side );
				const std::string key     = std::string( "g_TeamColor_" ) + ( side[ 1 ] == 'l' ? "Allies" : "Axis" );
				for ( const faction_t& f : k_factions )
					if ( faction == f.m_name ) {
						auto it = s_dv.m_map.find( key );
						if ( it != s_dv.m_map.end( ) )
							it->second.m_v[ 0 ] = f.m_color[ 0 ], it->second.m_v[ 1 ] = f.m_color[ 1 ], it->second.m_v[ 2 ] = f.m_color[ 2 ];
					}
			}
		}
	}

	std::string player_name( int index )
	{
		player_info_t info{ };
		if ( index > 0 && g_interfaces.m_engine_client->get_player_info( index, &info ) )
			return std::string( info.m_name, strnlen( info.m_name, sizeof( info.m_name ) ) );
		return { };
	}

	// ^ in a name would start a color code
	std::string clean_name( std::string n )
	{
		std::erase( n, '^' );
		std::erase_if( n, [ ]( char c ) { return static_cast< unsigned char >( c ) < 32; } );
		return n;
	}

	int team_of( int index )
	{
		auto* e = g_interfaces.m_client_entity_list->get< c_base_entity >( index );
		return e ? e->get_team( ) : 0;
	}
}

/* ---- game thread ---- */

void n_cod4_hud::impl_t::on_paint_traverse( )
{
	const bool on      = n_cod4_hud::enabled( );
	auto* const engine = g_interfaces.m_engine_client;
	const bool in_game = engine && engine->is_in_game( ) && g_ctx.m_local;

	// stock crosshair: hud edit, not a convar. panorama = scaleform hud_removal, classic = paint_traverse HudWeapon skip

	if ( on && s_load_state == 0 ) {
		s_load_state = 1;
		std::thread( load_worker ).detach( );
	}

	if ( on && !s_listening && g_interfaces.m_game_event_manager )
		s_listening = g_interfaces.m_game_event_manager->add_listener( &s_listener, "weapon_fire", false );

	drain_chat( on && in_game );

	snapshot_t s{ };
	s.m_in_game = on && in_game && g_convars.int_or( HASH_BT( "cl_drawhud" ), 1 ) != 0;

	if ( s.m_in_game ) {
		c_base_entity* const local = g_ctx.m_local;
		s.m_alive      = local->is_alive( );
		s.m_team       = local->get_team( );
		s.m_health     = std::max( local->get_health( ), 0 );
		s.m_max_health = 100;
		s.m_armor      = std::clamp( local->get_armor( ), 0, 999 );
		s.m_flashed    = local->get_flash_duration( ) > 0.f;
		s.m_console    = engine->is_console_visible( );
		s.m_ducked     = local->get_duck_amount( ) > 0.5f;
		const c_vector& origin = local->get_abs_origin( );
		s.m_origin[ 0 ] = origin.m_x, s.m_origin[ 1 ] = origin.m_y, s.m_origin[ 2 ] = origin.m_z;
		c_angle view{ };
		engine->get_view_angles( view );
		s.m_yaw = view.m_y;
		// cg_fov 65 (horizontal at 4:3) -> tan of the vertical half angle the reticle math divides by
		s.m_tan_half_fov_y = std::tan( 32.5f * 0.0174532924f ) * 0.75f;
		s.m_map            = engine->get_level_name_short( ) ? engine->get_level_name_short( ) : "";

		static const unsigned int ammo_offset = g_netvars[ HASH_BT( "CBasePlayer->m_iAmmo" ) ].m_offset;
		static const unsigned int type_offset = g_netvars[ HASH_BT( "CBaseCombatWeapon->m_iPrimaryAmmoType" ) ].m_offset;
		const unsigned int active             = local->get_active_weapon_handle( );
		s.m_weapon_handle                     = active;
		int frag_rank = 99, smoke_rank = 99;
		float active_max_speed = 250.f;

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
			const weapon_t* const cod   = weapon_for( name );
			const int cod_index         = cod ? static_cast< int >( cod - k_weapons ) : -1;

			if ( name == "weapon_c4" ) {
				s.m_has_c4   = true;
				s.m_c4_glyph = icon_for_definition( weapon->get_item_definition_index( ) );
			}

			if ( data->m_slot == 3 && cod_index >= 0 ) {
				int count = 1;
				if ( ammo_offset && type_offset ) {
					const int type = *reinterpret_cast< int* >( reinterpret_cast< std::uintptr_t >( weapon ) + type_offset );
					if ( type >= 0 && type < 32 )
						count = std::max( 1, *reinterpret_cast< int* >( reinterpret_cast< std::uintptr_t >( local ) + ammo_offset + type * 4 ) );
				}
				const bool lethal = name == "weapon_hegrenade" || name == "weapon_molotov" || name == "weapon_incgrenade";
				// the held one shows, else csgo's own cycle order
				static constexpr std::string_view k_order[] = { "weapon_hegrenade", "weapon_molotov", "weapon_incgrenade", "weapon_flashbang", "weapon_smokegrenade", "weapon_decoy" };
				int rank = handle == active ? -1 : 50;
				for ( int k = 0; k < 6 && rank == 50; ++k )
					if ( k_order[ k ] == name )
						rank = k;
				if ( lethal ) {
					s.m_frag_count += count;
					if ( rank < frag_rank )
						frag_rank = rank, s.m_frag_weapon = cod_index, s.m_frag_glyph = icon_for_definition( weapon->get_item_definition_index( ) );
				}
				else {
					s.m_smoke_count += count;
					if ( rank < smoke_rank )
						smoke_rank = rank, s.m_smoke_weapon = cod_index, s.m_smoke_glyph = icon_for_definition( weapon->get_item_definition_index( ) );
				}
			}

			if ( handle != active )
				continue;

			s.m_weapon      = cod_index;
			s.m_knife       = data->m_is_melee_weapon || data->m_slot == 2;
			s.m_scoped      = local->is_scoped( );
			s.m_sniper      = data->m_weapon_type == 5;
			s.m_clip_max    = data->m_max_clip1 > 0 ? data->m_max_clip1 : 0;
			s.m_clip        = std::max( weapon->get_ammo( ), 0 );
			s.m_reserve     = std::max( weapon->get_ammo_reserve( ), 0 );
			s.m_reserve_max = std::max( data->m_primary_max_reserve_ammo, 1 );

			// the gun in hand is csgo's: its own name (cod4 role-match names were wrong for most guns)
			const char* hud  = data->m_hud_name;
			const wchar_t* w = hud && g_interfaces.m_localize ? g_interfaces.m_localize->find_safe( hud ) : nullptr;
			s.m_weapon_name  = w && *w && *w != L'#' ? g_utilities.to_utf8( w ) : std::string( name.substr( name.find( '_' ) + 1 ) );

			// BG_GetSpreadForWeapon: min + (max - min) * aimSpreadScale / 255
			if ( cod && !s.m_knife ) {
				s.m_spread_min = s.m_ducked ? cod->m_hip_ducked_min : cod->m_hip_stand_min;
				s.m_spread_max = s.m_ducked && cod->m_hip_ducked_max > 0.f ? cod->m_hip_ducked_max : cod->m_hip_max;
			}
			active_max_speed = data->m_max_speed[ 0 ];
		}

		// aimSpreadScale run like cod4's pmove: every frame, shots = predicted clip drop on the same gun
		{
			static float s_scale = 0.f, s_real = 0.f;
			static c_angle s_view{ };
			static unsigned int s_handle = 0;
			static int s_clip            = 0;
			const float real             = g_interfaces.m_global_vars_base->m_real_time;
			const float dt               = std::clamp( real - s_real, 0.f, 0.25f );
			if ( s.m_alive && s.m_weapon >= 0 && dt > 0.f ) {
				const weapon_t& w   = k_weapons[ s.m_weapon ];
				const bool hip      = !s.m_scoped;
				const c_vector& vel = local->get_velocity( );
				aim_spread_in_t in{ };
				in.m_dt         = dt;
				in.m_turn[ 0 ]  = hip ? angle_delta( view.m_x, s_view.m_x ) : 0.f;
				in.m_turn[ 1 ]  = hip ? angle_delta( view.m_y, s_view.m_y ) : 0.f;
				in.m_speed      = std::sqrt( vel.m_x * vel.m_x + vel.m_y * vel.m_y );
				in.m_speed_full = active_max_speed;
				in.m_move_keys  = hip && ( g_ctx.m_input_buttons & ( in_forward | in_back | in_moveleft | in_moveright ) ) != 0;
				in.m_in_air     = hip && !( local->get_flags( ) & fl_onground );
				in.m_ducked     = s.m_ducked;
				s_scale         = aim_spread_update( s_scale, w, in );
				if ( hip && active == s_handle && s.m_clip < s_clip )
					s_scale = aim_spread_fire( s_scale, w, std::min( s_clip - s.m_clip, 5 ) );
			}
			if ( dt > 0.f || real < s_real ) {
				s_real = real, s_view = view;
				s_handle = active, s_clip = s.m_clip;
			}
			s.m_spread_scale = s_scale;
		}

		// team scores (CCSTeam), player scores (CCSPlayerResource)
		static const unsigned int team_num   = g_netvars[ HASH_BT( "CCSTeam->m_iTeamNum" ) ].m_offset;
		static const unsigned int team_score = g_netvars[ HASH_BT( "CCSTeam->m_scoreTotal" ) ].m_offset;
		static const unsigned int res_score  = g_netvars[ HASH_BT( "CCSPlayerResource->m_iScore" ) ].m_offset;
		const int highest = std::min( g_interfaces.m_client_entity_list->get_highest_entity_index( ), 2048 );
		std::vector< int > scores{ };
		for ( int i = 65; i <= highest && team_num && team_score; ++i ) {
			auto* const e = g_interfaces.m_client_entity_list->get< c_base_entity >( i );
			if ( !e )
				continue;
			const auto networkable = static_cast< c_client_networkable* >( e );
			const auto client_class = networkable->get_client_class( );
			if ( !client_class )
				continue;
			if ( client_class->m_class_id == e_class_ids::ccs_team ) {
				const int num   = *reinterpret_cast< int* >( reinterpret_cast< std::uintptr_t >( e ) + team_num );
				const int score = *reinterpret_cast< int* >( reinterpret_cast< std::uintptr_t >( e ) + team_score );
				if ( num == 2 )
					s.m_t_score = score;
				else if ( num == 3 )
					s.m_ct_score = score;
			}
			else if ( client_class->m_class_id == e_class_ids::ccs_player_resource && res_score ) {
				const int* arr = reinterpret_cast< int* >( reinterpret_cast< std::uintptr_t >( e ) + res_score );
				const int me   = engine->get_local_player( );
				for ( int p = 1; p <= 64; ++p ) {
					if ( !g_interfaces.m_client_entity_list->get< c_base_entity >( p ) && p != me )
						continue;
					if ( p == me )
						s.m_player_score = arr[ p ];
					scores.push_back( arr[ p ] );
				}
			}
		}
		std::sort( scores.rbegin( ), scores.rend( ) );
		s.m_score_rank1 = scores.size( ) > 0 ? scores[ 0 ] : 0;
		s.m_score_rank2 = scores.size( ) > 1 ? scores[ 1 ] : 0;
		s.m_team_score  = s.m_team == 2 ? s.m_t_score : s.m_ct_score;
		s.m_enemy_score = s.m_team == 2 ? s.m_ct_score : s.m_t_score;
		s.m_score_limit = g_convars.int_or( HASH_BT( "mp_maxrounds" ), 30 ) / 2 + 1;

		// CCSGameRules: m_fRoundStartTime = freeze end, so freeze time holds the full round time
		static const unsigned int warmup_ofs = g_netvars[ HASH_BT( "CCSGameRulesProxy->m_bWarmupPeriod" ) ].m_offset;
		static const unsigned int warmup_end = g_netvars[ HASH_BT( "CCSGameRulesProxy->m_fWarmupPeriodEnd" ) ].m_offset;
		static const unsigned int time_ofs   = g_netvars[ HASH_BT( "CCSGameRulesProxy->m_iRoundTime" ) ].m_offset;
		static const unsigned int start_ofs  = g_netvars[ HASH_BT( "CCSGameRulesProxy->m_fRoundStartTime" ) ].m_offset;
		if ( const std::uintptr_t rules = game_rules( ); rules && warmup_ofs && time_ofs && start_ofs ) {
			s.m_warmup        = *reinterpret_cast< bool* >( rules + warmup_ofs );
			const float round = static_cast< float >( *reinterpret_cast< int* >( rules + time_ofs ) );
			const float start = *reinterpret_cast< float* >( rules + start_ofs );
			// warmup: csgo's round clock stands still, count the warmup down instead
			if ( s.m_warmup && warmup_end )
				s.m_timeleft = std::max( *reinterpret_cast< float* >( rules + warmup_end ) - g_interfaces.m_global_vars_base->m_current_time, 0.f );
			else if ( round > 0.f )
				s.m_timeleft = std::clamp( round - ( g_interfaces.m_global_vars_base->m_current_time - start ), 0.f, round );
		}

		// compass actors: everyone alive and networked
		const int me = engine->get_local_player( );
		for ( int i = 1; i <= 64; ++i ) {
			if ( i == me )
				continue;
			auto* const p = g_interfaces.m_client_entity_list->get< c_base_entity >( i );
			if ( !p || p->is_dormant( ) || !p->is_alive( ) )
				continue;
			const int team = p->get_team( );
			if ( team != 2 && team != 3 )
				continue;
			actor_t a{ };
			a.m_index = i;
			a.m_enemy = team != s.m_team;
			const c_vector& o = p->get_abs_origin( );
			a.m_pos[ 0 ] = o.m_x, a.m_pos[ 1 ] = o.m_y;
			static const unsigned int eye_yaw = g_netvars[ HASH_BT( "CCSPlayer->m_angEyeAngles[1]" ) ].m_offset;
			a.m_yaw = eye_yaw ? *reinterpret_cast< float* >( reinterpret_cast< std::uintptr_t >( p ) + eye_yaw ) : p->get_abs_angles( ).m_y;
			s.m_actors.push_back( a );
		}
	}

	std::lock_guard< std::mutex > lock( m_lock );
	if ( s.m_weapon_handle != s_ev.m_last_weapon ) {
		s_ev.m_last_weapon   = s.m_weapon_handle;
		s_ev.m_weapon_switch = now_ms( );
	}
	if ( !s.m_alive )
		s_ev.m_streak = 0;
	m_snapshot = std::move( s );
}

void n_cod4_hud::impl_t::on_event( game_event_t* event )
{
	if ( !n_cod4_hud::enabled( ) || !event || !g_interfaces.m_engine_client )
		return;

	const std::string_view name = event->get_name( );
	auto* const engine          = g_interfaces.m_engine_client;
	const int me                = engine->get_local_player( );
	const int now               = now_ms( );
	const auto index            = [ & ]( const char* key ) { return engine->get_player_for_user_id( event->get_int( key ) ); };

	std::lock_guard< std::mutex > lock( m_lock );

	if ( name == "player_death" ) {
		const int victim = index( "userid" ), attacker = index( "attacker" );
		std::string weapon_raw = event->get_string( "weapon" );
		// event names that are not weapon_ classnames
		if ( weapon_raw == "inferno" )
			weapon_raw = "molotov";
		else if ( weapon_raw == "planted_c4" )
			weapon_raw = "c4";
		else if ( weapon_raw.rfind( "knifegg", 0 ) == 0 )
			weapon_raw = "knife";
		const bool headshot          = event->get_bool( "headshot" );
		const std::string victim_name = clean_name( player_name( victim ) );
		if ( victim_name.empty( ) )
			return;
		std::string attacker_name = attacker > 0 && attacker != victim ? clean_name( player_name( attacker ) ) : std::string( );

		// 0x433b60 / 0x4451f0: ^8 = the local player's team, ^9 the other, ^7 no team
		const int my_team = m_snapshot.m_team;
		const auto color  = [ & ]( int who ) {
			const int t = team_of( who );
			if ( ( t != 2 && t != 3 ) || ( my_team != 2 && my_team != 3 ) )
				return '7';
			return t == my_team ? '8' : '9';
		};

		// MOD icons: melee / headshot / falling / suicide replace the weapon icon (1.4 x 1.4)
		int material = -1;
		float w = 1.4f, h = 1.4f;
		bool flip = false;
		const bool melee = weapon_raw.find( "knife" ) != std::string::npos || weapon_raw == "bayonet";
		if ( attacker == victim || attacker <= 0 ) {
			material = material_index( weapon_raw == "world" || weapon_raw == "worldspawn" || weapon_raw.empty( ) ? "killiconfalling" : "killiconsuicide" );
			attacker_name.clear( );
		}
		int extra = -1;
		if ( material == -1 && attacker > 0 ) {
			// csgo icon (already faces right, toward the victim), headshot icon after it like csgo's own feed
			if ( const std::uint32_t cp = icon_for_event_weapon( weapon_raw ) ) {
				material = -static_cast< int >( cp );
				if ( headshot )
					extra = material_index( "killiconheadshot" );
			}
		}
		if ( material != -1 ) {
		}
		else if ( melee )
			material = material_index( "killiconmelee" );
		else if ( headshot )
			material = material_index( "killiconheadshot" );
		else if ( const weapon_t* wd = weapon_for( "weapon_" + weapon_raw ) ) {
			material = material_index( wd->m_kill_icon ? wd->m_kill_icon : "" );
			kill_icon_ratio( wd->m_kill_icon_ratio, w, h );
			flip = wd->m_flip_kill_icon != 0;
		}
		if ( material == -1 )
			material = material_index( "killicondied" ), w = h = 1.4f, flip = false;

		// ui_hud_obituaries gates the window's menu item (gamemessages.menu), not the line
		s_ev.m_lines.push_back( { 0, obituary_line( attacker_name, color( attacker ), victim_name, color( victim ), material, w, h, flip, extra ), now } );

		// CG_Obituary center print: "You killed X" / "Killed by X"
		if ( attacker == me && victim != me ) {
			s_ev.m_center      = localize( "CGAME_YOUKILLED", { victim_name } );
			s_ev.m_center_time = now;
			// _globallogic.gsc + _rank.gsc: kill / headshot 10 xp
			s_ev.m_xp_total += 10;
			s_ev.m_xp_shown = s_ev.m_xp_total;
			s_ev.m_xp_time  = now;
			int& xp         = GET_VARIABLE( g_variables.m_cod4_hud_xp, int );
			xp += 10;

			// _hardpoints.gsc giveHardpoint at 3 / 5 / 7
			++s_ev.m_streak;
			for ( int k = 0; k < 3; ++k ) {
				if ( s_ev.m_streak != k_streaks[ k ] )
					continue;
				if ( s_ev.m_hardpoint < k )
					s_ev.m_hardpoint = k;
				const int key = GET_VARIABLE( g_variables.m_cod4_hud_hardpoint_key, key_bind_t ).m_key;
				char kname[ 32 ]{ };
				if ( key > 0 )
					GetKeyNameTextA( static_cast< LONG >( MapVirtualKeyA( static_cast< UINT >( key ), MAPVK_VK_TO_VSC ) << 16 ), kname, sizeof( kname ) );
				s_ev.m_notifies.push_back( { localize( "MP_KILLSTREAK_N", { std::to_string( s_ev.m_streak ) } ), localize( k_hardpoint_hints[ k ], { kname } ), 0, 4000 } );
			}
		}
		else if ( victim == me && attacker > 0 && attacker != me ) {
			s_ev.m_center      = localize( "CGAME_YOUWEREKILLED", { attacker_name } );
			s_ev.m_center_time = now;
		}
		if ( victim == me )
			s_ev.m_streak = 0;
		return;
	}

	if ( name == "player_hurt" ) {
		const int victim = index( "userid" ), attacker = index( "attacker" );
		if ( attacker == me && victim != me )
			s_ev.m_hit_time = now;
		if ( victim == me && attacker > 0 && attacker != me ) {
			auto* const a = g_interfaces.m_client_entity_list->get< c_base_entity >( attacker );
			if ( a && g_ctx.m_local ) {
				// CG_DamageFeedback: yaw the damage travelled (attacker -> me)
				const c_vector& from = a->get_abs_origin( );
				const c_vector& to   = g_ctx.m_local->get_abs_origin( );
				s_ev.m_damage_yaws.push_back( std::atan2( to.m_y - from.m_y, to.m_x - from.m_x ) * 57.2957795f );
			}
		}
		return;
	}

	if ( name == "weapon_fire" ) {
		const int who = index( "userid" );
		if ( who > 0 && who <= 64 )
			s_ev.m_fire_time[ who ] = now;
		return;
	}

	if ( name == "bomb_planted" ) {
		s_ev.m_lines.push_back( { 1, localize( "MP_EXPLOSIVES_PLANTED" ), now } );
		return;
	}
	if ( name == "bomb_defused" ) {
		s_ev.m_lines.push_back( { 1, localize( "MP_EXPLOSIVES_DEFUSED" ), now } );
		return;
	}
	// cs_shareddefs.h RoundEndReason: 10 Round_Draw, 16 Game_Commencing (warmup end, winner 1 = no team)
	if ( name == "round_end" ) {
		const int winner = event->get_int( "winner" ), reason = event->get_int( "reason" );
		if ( m_snapshot.m_warmup || reason == 16 )
			return;
		if ( winner == 2 || winner == 3 )
			s_ev.m_lines.push_back( { 1, localize( winner == m_snapshot.m_team ? "MP_ROUND_WIN" : "MP_ROUND_LOSS" ), now } );
		else if ( reason == 10 )
			s_ev.m_lines.push_back( { 1, localize( "MP_DRAW" ), now } );
		return;
	}
	if ( name == "player_spawn" && index( "userid" ) == me ) {
		s_ev.m_streak   = 0;
		s_ev.m_xp_total = 0;
	}
}

// game thread (push_notice): csgo chat goes to game message window 0 with the obituaries, like cod4's
void n_cod4_hud::impl_t::on_chat( const char* text, int client, bool html )
{
	if ( !n_cod4_hud::enabled( ) || !text || !*text )
		return;
	const int sender = !html && client > 0 && client <= 64 ? team_of( client ) : 0;
	std::lock_guard< std::mutex > lock( m_lock );
	std::string line = chat_line( text, html, sender, m_snapshot.m_team );
	if ( line.size( ) > 2 )
		s_ev.m_lines.push_back( { 0, std::move( line ), now_ms( ) } );
}

// game thread (paint_traverse): send / drop the typed line, mc_hud's drain
void n_cod4_hud::impl_t::drain_chat( bool active )
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

// window thread: the messagemode bind opens our line instead of the csgo chat panel, copy of mc_hud chat_on_key
bool n_cod4_hud::impl_t::chat_on_key( unsigned int msg, unsigned int wide_param, long long_param, bool ui_input )
{
	// no WM_SYSKEY*: alt+f4 / alt+tab keep working
	if ( msg != WM_KEYDOWN && msg != WM_KEYUP && msg != WM_CHAR )
		return false;
	if ( ui_input || wide_param == g_menu.menu_key( ) )
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
		if ( msg != WM_KEYDOWN || stale || !n_cod4_hud::enabled( ) )
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

		// ponytail: quake 3's MAX_SAY_TEXT 150, cod4's own field length unchecked
		if ( character >= L' ' && character != 0x7F && m_chat_input.size( ) < 150U )
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

bool n_cod4_hud::hides_scope( )
{
	const snapshot_t& s = g_cod4_hud.m_snapshot;
	return enabled( ) && s.m_in_game && s.m_scoped && s.m_weapon >= 0 && k_weapons[ s.m_weapon ].m_ads_overlay;
}

void n_cod4_hud::impl_t::on_release( )
{
	if ( s_listening && g_interfaces.m_game_event_manager ) {
		g_interfaces.m_game_event_manager->remove_listener( &s_listener );
		s_listening = false;
	}
}

/* ---- render thread ---- */

void n_cod4_hud::impl_t::on_end_scene( )
{
	if ( !n_cod4_hud::enabled( ) )
		return;

	sync_dvars( );

	const int now = now_ms( );
	snapshot_t s{ };
	{
		std::lock_guard< std::mutex > lock( m_lock );
		if ( !m_snapshot.m_in_game )
			return;
		s = m_snapshot;

		for ( queued_line_t& l : s_ev.m_lines ) {
			const std::string p = "con_gameMsgWindow" + std::to_string( l.m_window );
			s_windows[ l.m_window ].add( std::move( l.m_text ), l.m_time, static_cast< int >( s_dv.f( p + "MsgTime" ) * 1000.f ), s_dv.i( p + "LineCount" ),
			                             static_cast< int >( s_dv.f( p + "FadeOutTime" ) * 1000.f ) );
		}
		s_ev.m_lines.clear( );

		// 0x442d80: oldest slot, duration cg_hudDamageIconTime, yaw jittered +- 10
		for ( float yaw : s_ev.m_damage_yaws ) {
			damage_slot_t* slot = &s_damage[ 0 ];
			for ( damage_slot_t& d : s_damage )
				if ( d.m_time < slot->m_time )
					slot = &d;
			slot->m_time     = now;
			slot->m_duration = s_dv.i( "cg_hudDamageIconTime" );
			slot->m_yaw      = angle_normalize_360( yaw + ( static_cast< float >( rand( ) ) / static_cast< float >( RAND_MAX ) - 0.5f ) * 20.f );
		}
		s_ev.m_damage_yaws.clear( );

		// xp popup resets once faded
		if ( now - s_ev.m_xp_time >= 1750 )
			s_ev.m_xp_total = 0;

		for ( int i = 1; i <= 64; ++i )
			s_actor_state[ i ].m_fire_time = s_ev.m_fire_time[ i ];

		for ( notify_t& n : s_ev.m_notifies )
			s_notifies.push_back( std::move( n ) );
		s_ev.m_notifies.clear( );
		s_rv = s_ev;
	}

	{
		// EXE_SAY / EXE_SAYTEAM ("say" / "say_team"), Field_Draw's cursor blinks on realtime >> 8
		std::scoped_lock lock( m_chat_mutex );
		s_chat_line = m_chat_open ? std::string( m_chat_team ? "say_team: " : "say: " ) + g_utilities.to_utf8( m_chat_input ) + ( ( now >> 8 ) & 1 ? "_" : "" )
		                          : std::string( );
	}

	if ( s_load_state != 2 )
		return;

	// 0x427a20 / 0x4278d0: sweep 0..1 every compassRadarUpdateTime, enemies between the old and new line ping
	if ( now < s_rv.m_radar_until ) {
		const float period = std::max( 0.05f, s_dv.f( "compassRadarUpdateTime" ) ) * 1000.f;
		const float before = s_sweep;
		s_sweep += static_cast< float >( now - s_sweep_last ) / period;
		s_sweep -= std::floor( s_sweep );
		const float range = s_dv.f( "compassMaxRange" );
		for ( const actor_t& a : s.m_actors ) {
			if ( !a.m_enemy )
				continue;
			// screen-space x of the enemy across the compass square, 0..1
			float north[ 2 ];
			compass_basis( s, north );
			const float dx = a.m_pos[ 0 ] - s.m_origin[ 0 ], dy = a.m_pos[ 1 ] - s.m_origin[ 1 ];
			const float x = ( dx * north[ 1 ] - dy * north[ 0 ] ) / range + 0.5f;
			const bool crossed = before <= s_sweep ? ( x > before && x <= s_sweep ) : ( x > before || x <= s_sweep );
			if ( crossed ) {
				s_actor_state[ a.m_index ].m_ping[ 0 ] = a.m_pos[ 0 ], s_actor_state[ a.m_index ].m_ping[ 1 ] = a.m_pos[ 1 ];
				s_actor_state[ a.m_index ].m_ping_time = now;
			}
		}
	}
	s_sweep_last = now;

	load_overview( s.m_map );

	const int w = static_cast< int >( g_ctx.m_width ), h = static_cast< int >( g_ctx.m_height );
	if ( w <= 0 || h <= 0 || !s_dv.b( "cg_draw2D" ) || !s_dv.b( "hud_enable" ) )
		return;
	// stretched aspect: lay out in the screen's pre-stretch width, the background pass then scales x about the center
	// to exactly 0..w (a stretch block scaled the whole-screen hud past the edges)
	const float left = g_render.to_overlay_x( 0.f ), right = g_render.to_overlay_x( static_cast< float >( w ) );
	s_scr  = scr_t::make( right - left, static_cast< float >( h ) );
	s_list = ImGui::GetBackgroundDrawList( );
	const int first_vertex = s_list->VtxBuffer.Size;

	ctx_t ctx{ };
	ctx.m_s   = &s;
	ctx.m_now = now;

	// ads overlay under the hud, low health menu first (hud.txt order), then the code drawn center pieces, then the rest of the
	// menus, hud elems last
	if ( s.m_alive )
		draw_scope( s );
	paint_menus( ctx, s, now, 0, 1 );
	if ( s.m_alive ) {
		draw_damage( s, now );
		draw_crosshair( s );
	}
	paint_menus( ctx, s, now, 1, static_cast< int >( std::size( k_menus ) ) );
	draw_hit_marker( now );
	draw_xp( now );
	draw_notify( now );

	for ( int v = first_vertex; left != 0.f && v < s_list->VtxBuffer.Size; ++v )
		s_list->VtxBuffer[ v ].pos.x += left;
	s_list->AddCallback( ImDrawCallback_ResetRenderState, nullptr );
}

void n_cod4_hud::impl_t::release_textures( )
{
	for ( texture_t& t : s_textures ) {
		if ( t.m_texture )
			t.m_texture->Release( );
		t = { };
	}
	if ( s_overview.m_texture )
		s_overview.m_texture->Release( );
	s_overview = { };
}

/* ---- options (visuals tab popup) ---- */

void n_cod4_hud::draw_options( )
{
	sync_dvars( );

	const int state = s_load_state;
	if ( state == 2 )
		ImGui::TextDisabled( "cod4: %s", s_cod4_dir.c_str( ) );
	else if ( state == 3 ) {
		ImGui::TextDisabled( "cod4: install not found" );
		if ( ImGui::Button( "retry##cod4" ) )
			s_load_state = 0;
	}
	else
		ImGui::TextDisabled( "cod4: loading" );

	static constexpr const char* k_labels[] = { "draw crosshair", "hardcore" };
	for ( size_t i = 0; i < std::size( k_user_dvars ); ++i ) {
		auto it = s_dv.m_map.find( k_user_dvars[ i ] );
		if ( it == s_dv.m_map.end( ) )
			continue;
		bool b = it->second.m_v[ 0 ] != 0.f;
		if ( ImGui::Checkbox( k_labels[ i ], &b ) ) {
			it->second.m_v[ 0 ] = b ? 1.f : 0.f;
			GET_VARIABLE( g_variables.m_cod4_hud_dvars, std::string ) = s_dv.save( );
		}
	}
}

// hardpoint key (window thread): uses the held hardpoint, radar_mp = 30 s uav sweep (_hardpoints.gsc radarViewTime)
bool n_cod4_hud::on_key( unsigned int msg, unsigned int vk )
{
	if ( msg != WM_KEYDOWN || !n_cod4_hud::enabled( ) || static_cast< int >( vk ) != GET_VARIABLE( g_variables.m_cod4_hud_hardpoint_key, key_bind_t ).m_key )
		return false;
	std::lock_guard< std::mutex > lock( g_cod4_hud.m_lock );
	if ( !g_cod4_hud.m_snapshot.m_in_game || g_cod4_hud.m_snapshot.m_console || s_ev.m_hardpoint < 0 )
		return false;
	if ( s_ev.m_hardpoint == 0 )
		s_ev.m_radar_until = now_ms( ) + 30000;
	s_ev.m_hardpoint = -1;
	return false;
}
