#include "gta_hud.h"
#include "gta_data.h"
#include "gta_font.h"
#include "gta_logic.h"
#include "gta_radar.h"

#include "../../utilities/console/console.h"
#include "../menu/menu.h"
#include "../misc/misc.h"
#include "../misc/scaleform/image_cache.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace n_gta_logic;
namespace font = n_gta_font;
namespace data = n_gta_data;

extern void botox_dbg_log( const char* fmt, ... );
extern bool point_menu_is_opened( );

bool n_gta_hud::enabled( ) { return GET_VARIABLE( g_variables.m_gta_hud, bool ); }

namespace
{
	using n_gta_hud::blip_t;
	using n_gta_hud::snapshot_t;

	// module init, not a function static: game + render thread both call now_ms (no thread safe statics here)
	const auto k_start = std::chrono::steady_clock::now( );
	long long now_ms( ) { return std::chrono::duration_cast< std::chrono::milliseconds >( std::chrono::steady_clock::now( ) - k_start ).count( ); }

	/* ---- assets: botox-assets gta/, cached under C:\botox\gta\ ---- */
	constexpr const char* k_cache_dir = "C:\\botox\\";
	constexpr int k_fetch_threads     = 8;
	constexpr int k_font_base         = data::k_hud_count;
	constexpr int k_icon_base         = k_font_base + 2;
	constexpr int k_cross_base        = k_icon_base + data::k_icons_count;
	constexpr int k_asset_count       = k_cross_base + data::k_crosshairs_count;

	const data::file_t& asset( int i )
	{
		if ( i < k_font_base )
			return data::k_hud[ i ];
		if ( i < k_icon_base )
			return data::k_fonts[ i - k_font_base ];
		if ( i < k_cross_base )
			return data::k_icons[ i - k_icon_base ];
		return data::k_crosshairs[ i - k_cross_base ];
	}

	std::atomic< int > s_load_state{ 0 }; // 0 idle, 1 loading, 2 ready, 3 failed
	int s_missing = 0;
	std::vector< std::vector< std::uint8_t > > s_files{ }; // per asset, immutable once state 2

	struct texture_t {
		IDirect3DTexture9* m_texture = nullptr;
		bool m_tried = false;
	};
	std::vector< texture_t > s_textures{ };

	std::string cache_path( const char* rel )
	{
		std::string p = std::string( k_cache_dir ) + rel;
		std::replace( p.begin( ), p.end( ), '/', '\\' );
		return p;
	}

	bool read_exact( const std::string& path, unsigned int size, std::vector< std::uint8_t >& out )
	{
		std::ifstream file( path, std::ios::binary );
		out.assign( std::istreambuf_iterator< char >( file ), std::istreambuf_iterator< char >( ) );
		if ( out.size( ) == size )
			return true;
		out.clear( );
		return false;
	}

	void load_worker( )
	{
		std::atomic< int > next{ 0 };
		std::vector< std::thread > pool{ };
		for ( int t = 0; t < k_fetch_threads; ++t )
			pool.emplace_back( [ & ] {
				std::error_code ec{ };
				for ( int i; ( i = next++ ) < k_asset_count; ) {
					const data::file_t& f  = asset( i );
					const std::string path = cache_path( f.m_path );
					std::filesystem::create_directories( std::filesystem::path( path ).parent_path( ), ec );
					// a cached file of the wrong size (older art, half write) is fetched again
					if ( std::filesystem::exists( path, ec ) && std::filesystem::file_size( path, ec ) == f.m_size )
						continue;
					if ( !n_image_cache::download_file( std::string( data::k_raw_base ) + f.m_path, path ) )
						botox_dbg_log( "GTA: download failed %s%s", data::k_raw_base, f.m_path );
				}
			} );
		for ( std::thread& t : pool )
			t.join( );

		std::vector< std::vector< std::uint8_t > > files( k_asset_count );
		int missing = 0;
		for ( int i = 0; i < k_asset_count; ++i ) {
			const std::string path = cache_path( asset( i ).m_path );
			if ( !read_exact( path, asset( i ).m_size, files[ i ] ) ) {
				DeleteFileA( path.c_str( ) );
				++missing;
			}
		}
		char line[ 160 ]{ };
		sprintf_s( line, "GTA: %d files, %d missing (%sgta)", k_asset_count, missing, k_cache_dir );
		g_console.print( line );
		s_missing = missing;
		if ( missing == k_asset_count )
			s_load_state = 3;
		else {
			s_files      = std::move( files );
			s_load_state = 2;
		}
		--g_url_image_downloads;
	}

	int find( const data::file_t* list, int count, const char* name )
	{
		for ( int i = 0; i < count; ++i )
			if ( std::strcmp( list[ i ].m_name, name ) == 0 )
				return i;
		return -1;
	}
	int hud_tex( const char* name ) { return find( data::k_hud, data::k_hud_count, name ); }
	int icon_tex( const char* model )
	{
		const int i = find( data::k_icons, data::k_icons_count, model );
		return i < 0 ? -1 : k_icon_base + i;
	}
	int cross_tex( const char* model )
	{
		const int i = find( data::k_crosshairs, data::k_crosshairs_count, model );
		return i < 0 ? -1 : k_cross_base + i;
	}

	// render thread, lazy. csgo is d3d9ex: MANAGED can be refused, DEFAULT + DYNAMIC then (released before Reset, reset.cpp)
	IDirect3DTexture9* texture( int i )
	{
		if ( i < 0 || i >= k_asset_count || s_load_state != 2 )
			return nullptr;
		if ( s_textures.size( ) != static_cast< size_t >( k_asset_count ) )
			s_textures.resize( k_asset_count );
		texture_t& t = s_textures[ i ];
		if ( !t.m_tried ) {
			t.m_tried                      = true;
			const auto& f                  = s_files[ i ];
			IDirect3DDevice9* const device = g_interfaces.m_direct_device;
			const auto load = [ & ]( DWORD usage, D3DPOOL pool ) {
				return D3DXCreateTextureFromFileInMemoryEx( device, f.data( ), static_cast< UINT >( f.size( ) ), D3DX_DEFAULT_NONPOW2, D3DX_DEFAULT_NONPOW2, 1,
				                                            usage, D3DFMT_A8R8G8B8, pool, D3DX_FILTER_NONE, D3DX_FILTER_NONE, 0, nullptr, nullptr, &t.m_texture );
			};
			if ( !device || f.empty( ) || ( FAILED( load( 0, D3DPOOL_MANAGED ) ) && FAILED( load( D3DUSAGE_DYNAMIC, D3DPOOL_DEFAULT ) ) ) )
				t.m_texture = nullptr;
		}
		return t.m_texture;
	}

	/* ---- events: game thread writes, render thread drains (both under g_gta_hud.m_lock) ---- */
	struct events_t {
		std::string m_success{ };             // mission passed / failed text
		std::vector< std::pair< std::string, long long > > m_chat{ }; // ready-made CFont lines + arrival ms
	} s_ev{ };

	unsigned int net_offset( unsigned int hash ) { return g_netvars[ hash ].m_offset; }

	template < class T >
	T field( const void* base, unsigned int ofs )
	{
		return *reinterpret_cast< const T* >( reinterpret_cast< std::uintptr_t >( base ) + ofs );
	}

	// CCSGameRules* (cod4 / urt sig)
	std::uintptr_t game_rules( )
	{
		static const auto site = g_modules[ CLIENT_DLL ].find_pattern( "A1 ? ? ? ? 85 C0 0F 84 ? ? ? ? 80 B8 ? ? ? ? ? 74 7A" );
		return site ? reinterpret_cast< std::uintptr_t >( **reinterpret_cast< void*** >( site + 1 ) ) : 0;
	}

	const char* gxt( const char* key )
	{
		for ( int i = 0; i < data::k_gxt_count; ++i )
			if ( std::strcmp( data::k_gxt[ i ].m_key, key ) == 0 )
				return data::k_gxt[ i ].m_text;
		return "";
	}

	// sa's font only maps printable ascii, csgo place names are utf8
	std::string ascii( const std::string& s )
	{
		std::string out{ };
		for ( const char c : s )
			if ( c >= 32 && c < 127 )
				out += c;
		return out;
	}

	/* ---- render state ---- */
	snapshot_t s_s{ };
	struct draw_t {
		ImDrawList* m_list = nullptr;
		float m_w = 640.f, m_h = 448.f; // layout px (pre-stretch width)
		float m_sx = 1.f, m_sy = 1.f;   // SCREEN_STRETCH_X / _Y units
		unsigned int m_frame = 0;       // 30 hz frame counter
		float m_dt = 0.f;               // ms since last frame
	} g{ };

	float sx( float v ) { return v * g.m_sx; }
	float sy( float v ) { return v * g.m_sy; }

	ImU32 col( const font::rgba_t& c ) { return IM_COL32( c.r, c.g, c.b, c.a ); }

	// CSprite2d::Draw: uv (0,0) at (x1, y1), (1,1) at (x2, y2), mirrored when x1 > x2 / y1 > y2
	void sprite( int tex, float x1, float y1, float x2, float y2, font::rgba_t c = { } )
	{
		IDirect3DTexture9* const t = texture( tex );
		if ( !t || !c.a )
			return;
		g.m_list->AddImageQuad( reinterpret_cast< ImTextureID >( t ), ImVec2( x1, y1 ), ImVec2( x2, y1 ), ImVec2( x2, y2 ), ImVec2( x1, y2 ), ImVec2( 0.f, 0.f ),
		                        ImVec2( 1.f, 0.f ), ImVec2( 1.f, 1.f ), ImVec2( 0.f, 1.f ), col( c ) );
	}

	// CSprite::RenderOneXLUSprite: centre + half size, udir / vdir mirror
	void xlu_sprite( int tex, float x, float y, float hw, float hh, bool udir, bool vdir )
	{
		sprite( tex, udir ? x + hw : x - hw, vdir ? y + hh : y - hh, udir ? x - hw : x + hw, vdir ? y - hh : y + hh );
	}

	// CSprite2d::DrawRect
	void rect( float x1, float y1, float x2, float y2, font::rgba_t c )
	{
		if ( c.a )
			g.m_list->AddRectFilled( ImVec2( std::min( x1, x2 ), std::min( y1, y2 ) ), ImVec2( std::max( x1, x2 ), std::max( y1, y2 ) ), col( c ) );
	}

	// CSprite2d::DrawBarChart 0x728640 (no progressAdd, no percentage, black border)
	void bar_chart( float x, float y, int width, int height, float progress, font::rgba_t c )
	{
		progress           = std::clamp( progress, 0.f, 100.f );
		const float end_x  = x + static_cast< float >( width );
		const float curr_x = std::min( x + static_cast< float >( width ) * progress / 100.f, end_x );
		const float h      = static_cast< float >( height );
		rect( x, y, curr_x, y + h, c );
		rect( curr_x, y, end_x, y + h,
		      { static_cast< std::uint8_t >( c.r / 2.f ), static_cast< std::uint8_t >( c.g / 2.f ), static_cast< std::uint8_t >( c.b / 2.f ), c.a } );
		const float bw = sx( 2.f ), bh = sy( 2.f );
		const font::rgba_t black{ 0, 0, 0, c.a };
		rect( x, y, end_x, y + bh, black );
		rect( x, y + h - bh, end_x, y + h, black );
		rect( x, y, x + bw, y + h, black );
		rect( end_x - bw, y, end_x, y + h, black );
	}

	// CFont, one call = one SA PrintString / PrintStringFromBottom
	void text( font::details_t& d, float x, float y, const char* s, bool from_bottom = false )
	{
		std::vector< font::quad_t > quads{ };
		font::context_t ctx{ };
		ctx.m_w   = g.m_w;
		ctx.m_h   = g.m_h;
		ctx.m_out = &quads;
		if ( from_bottom )
			font::print_from_bottom( ctx, d, x, y, s );
		else
			font::print( ctx, d, x, y, s );
		for ( const font::quad_t& q : quads ) {
			IDirect3DTexture9* const t = texture( k_font_base + q.m_texture );
			if ( !t || !q.m_color.a )
				continue;
			g.m_list->AddImageQuad( reinterpret_cast< ImTextureID >( t ), ImVec2( q.m_x[ 0 ], q.m_y[ 0 ] ), ImVec2( q.m_x[ 1 ], q.m_y[ 1 ] ),
			                        ImVec2( q.m_x[ 2 ], q.m_y[ 2 ] ), ImVec2( q.m_x[ 3 ], q.m_y[ 3 ] ), ImVec2( q.m_u[ 0 ], q.m_v[ 0 ] ),
			                        ImVec2( q.m_u[ 1 ], q.m_v[ 1 ] ), ImVec2( q.m_u[ 2 ], q.m_v[ 2 ] ), ImVec2( q.m_u[ 3 ], q.m_v[ 3 ] ), col( q.m_color ) );
		}
	}

	font::details_t fresh( )
	{
		font::details_t d{ };
		d.m_wrap_end = d.m_centre_x = g.m_w;
		return d;
	}

	/* ---- render-side state that lives across frames ---- */
	int s_display_money       = 0;    // CPlayerInfo::m_nDisplayMoney
	bool s_money_init         = false;
	unsigned int s_money_frame = 0;
	int s_last_wanted         = 0;    // CHud::m_LastWanted
	long long s_wanted_change = -100000;
	zone_t s_zone{ };
	wasted_t s_wasted{ };
	success_t s_success{ };

	struct chat_entry_t {
		std::string m_text;
		long long m_ms;
	};
	std::vector< chat_entry_t > s_chat{ }; // newest last
	std::string s_chat_prefix{ };          // "Say: " while typing, empty = closed
	std::string s_chat_typed{ };
	constexpr int k_chat_lines        = 8;
	constexpr long long k_chat_life   = 12000;

	/* ---- chat (no sa twin): DrawSubtitles 0x58C250 look = subtitles font, 225 grey, black drop shadow, smaller. stacked upward
	   from above the voice rows over the radar disc, left edge on the disc, newest lowest, 12 s then a 1 s fade ---- */
	void draw_chat( )
	{
		const long long now = now_ms( );
		const bool open     = !s_chat_prefix.empty( );
		const float x       = sx( 36.f );
		float bottom        = g.m_h - sy( 108.f + 40.f ); // over the lifted voice rows (scaleform k_rm_voice_lift)
		const auto style    = [ & ]( std::uint8_t a, bool one_line ) {
			font::details_t d = fresh( );
			d.set_style( font::font_subtitles );
			d.m_prop = true;
			d.set_align( font::align_left );
			d.m_wrap_end = one_line ? g.m_w * 4.f : x + sx( 260.f );
			// subtitles run 0.58 x 1.22 + shadow 2; chat at about csgo chat size
			d.m_scale_x = sx( 0.2f ), d.m_scale_y = sy( 0.45f );
			d.set_drop_shadow( 1 );
			d.m_drop_color = { 0, 0, 0, a };
			d.m_color      = font::hud_rgba( font::hud_light_gray, a );
			return d;
		};
		const auto stack = [ & ]( const char* s, std::uint8_t a, bool one_line ) {
			font::details_t d = style( a, one_line );
			font::context_t ctx{ };
			ctx.m_w = g.m_w, ctx.m_h = g.m_h;
			bottom -= font::height( d ) * static_cast< float >( font::process_string( ctx, d, false, x, bottom, s ) );
			text( d, x, bottom, s );
		};
		if ( open ) {
			// one line, the tail that fits the chat width (csgo's input scrolls the same way)
			const font::details_t d = style( 255, true );
			std::string typed       = s_chat_typed;
			std::string line        = s_chat_prefix + typed;
			while ( !typed.empty( ) && font::string_width( d, line.c_str( ), true ) > sx( 260.f ) )
				typed.erase( 0, 1 ), line = s_chat_prefix + typed;
			stack( line.c_str( ), 255, true );
			// caret: PrintChar never draws '_', so a bar, blinking like the csgo one
			if ( ( now >> 9 ) & 1 ) {
				const float cx = x + font::string_width( d, line.c_str( ), true );
				rect( cx, bottom + sy( 1.f ), cx + std::max( sx( 1.f ), 1.f ), bottom + 20.f * d.m_scale_y, { 225, 225, 225, 255 } );
			}
		}
		int shown = 0;
		for ( auto it = s_chat.rbegin( ); it != s_chat.rend( ) && shown < k_chat_lines; ++it, ++shown ) {
			const long long age = now - it->m_ms;
			if ( !open && age > k_chat_life )
				break; // the rest is older still
			const float fade = open ? 1.f : std::clamp( static_cast< float >( k_chat_life - age ) / 1000.f, 0.f, 1.f );
			stack( it->m_text.c_str( ), static_cast< std::uint8_t >( ftol( 255.f * fade ) ), false );
		}
	}

	/* ---- DrawCrossHairs 0x58E020 ---- */
	void draw_crosshair( )
	{
		if ( !s_s.m_alive )
			return;
		const sa_weapon_t& w = sa_weapon( s_s.m_def, s_s.m_knife );
		// any csgo scope = MODE_SNIPER with the sniper's CROSSHAIR texture: 4 mirrored 210 x 210 quarters round the centre
		if ( s_s.m_scoped ) {
			const int t    = cross_tex( "sniper" );
			const float cw = sx( 210.f ), ch = sy( 210.f );
			const float cx = g.m_w / 2.f, cy = g.m_h / 2.f;
			xlu_sprite( t, cx - cw / 2.f, cy - ch / 2.f, cw / 2.f, ch / 2.f, false, false );
			xlu_sprite( t, cx + cw / 2.f, cy - ch / 2.f, cw / 2.f, ch / 2.f, true, false );
			xlu_sprite( t, cx - cw / 2.f, cy + ch / 2.f, cw / 2.f, ch / 2.f, false, true );
			xlu_sprite( t, cx + cw / 2.f, cy + ch / 2.f, cw / 2.f, ch / 2.f, true, true );
			// not in the exe (user ask): the 420 x 420 scope leaves the sides (+ a strip top / bottom) open, black like its corners
			const font::rgba_t black{ 0, 0, 0, 255 };
			rect( 0.f, 0.f, cx - cw, g.m_h, black );
			rect( cx + cw, 0.f, g.m_w, g.m_h, black );
			rect( cx - cw, 0.f, cx + cw, cy - ch, black );
			rect( cx - cw, cy + ch, cx + cw, g.m_h, black );
			return;
		}
		if ( !circle_crosshair( w ) )
			return;
		// aim weapon circle: sa centres it at m_f3rdPersonCHairMult (0.53, 0.4), csgo aims down the middle
		const float r  = weapon_radius( w, s_s.m_counter, s_s.m_ducking );
		const float cx = g.m_w / 2.f, cy = g.m_h / 2.f;
		if ( r == 0.2f )
			rect( cx - 1.f, cy - 1.f, cx + 1.f, cy + 1.f, { 255, 255, 255, 255 } );
		const int t    = hud_tex( "sitem16" );
		const float ha = sx( 64.f ) * r * 0.5f, hb = sy( 64.f ) * r * 0.5f;
		sprite( t, cx - ha, cy - hb, cx, cy );
		sprite( t, cx + ha, cy - hb, cx, cy );
		sprite( t, cx - ha, cy + hb, cx, cy );
		sprite( t, cx + ha, cy + hb, cx, cy );
	}

	/* ---- DrawPlayerInfo 0x58EAF0 (PC: every element state 5 = always on, alpha 255) ---- */
	constexpr int k_max_health = 100; // CPlayerInfo::m_nMaxHealth, csgo 100

	void draw_player_info( )
	{
		const float w = g.m_w;
		char buf[ 32 ]{ };

		// clock
		{
			if ( s_s.m_clock_ms >= 0 )
				clock_text( s_s.m_clock_ms, buf, sizeof( buf ) );
			else {
				const std::time_t now = std::time( nullptr );
				std::tm tm{ };
				localtime_s( &tm, &now );
				sprintf_s( buf, "%02d:%02d", tm.tm_hour, tm.tm_min );
			}
			font::details_t d = fresh( );
			d.m_scale_x = sx( 0.55f ), d.m_scale_y = sy( 1.1f );
			d.m_prop    = false;
			d.set_style( font::font_pricedown );
			d.set_align( font::align_right );
			d.m_right_wrap = 0.f;
			d.set_edge( 2 );
			d.m_drop_color = { 0, 0, 0, 255 };
			d.m_color      = font::hud_rgba( font::hud_light_gray, 255 );
			text( d, w - sx( 32.f ), sy( 22.f ), buf );
		}

		// health (flashes under 10), armor (hidden at <= 1)
		{
			const int x = ftol( w - sx( 141.f ) ), y = ftol( y_for_health( sy( 77.f ), sy( 10.f ), k_max_health ) );
			if ( !( s_s.m_health < 10 && each_frames( g.m_frame, 8 ) ) ) {
				const float x109 = sx( 109.f );
				const int width  = health_bar_width( x109, k_max_health );
				bar_chart( x109 - static_cast< float >( width ) + static_cast< float >( x ), static_cast< float >( y ), width,
				           static_cast< std::uint8_t >( sy( 9.f ) ), static_cast< float >( s_s.m_health ) * 100.f / static_cast< float >( k_max_health ),
				           font::hud_rgba( font::hud_red, 255 ) );
			}
		}
		if ( s_s.m_armor > 1 ) {
			const int x = ftol( w - sx( 94.f ) ), y = ftol( y_for_health( sy( 48.f ), sy( 3.f ), k_max_health ) );
			bar_chart( static_cast< float >( x ), static_cast< float >( y ), static_cast< unsigned short >( sx( 62.f ) ), static_cast< std::uint8_t >( sy( 9.f ) ),
			           static_cast< float >( s_s.m_armor ), font::hud_rgba( font::hud_light_gray, 255 ) );
		}

		// money, rolled once per 30 hz frame
		{
			if ( !s_money_init )
				s_display_money = s_s.m_money, s_money_init = true, s_money_frame = g.m_frame;
			for ( int n = 0; s_money_frame != g.m_frame && n < 300; ++n, ++s_money_frame )
				s_display_money = money_step( s_display_money, s_s.m_money );
			s_money_frame     = g.m_frame;
			const bool red    = money_text( s_display_money, buf, sizeof( buf ) );
			font::details_t d = fresh( );
			d.m_color         = font::hud_rgba( red ? font::hud_red : font::hud_green, 255 );
			d.m_prop          = false;
			d.m_scale_x = sx( 0.55f ), d.m_scale_y = sy( 1.1f );
			d.set_align( font::align_right );
			d.m_right_wrap = 0.f;
			d.set_style( font::font_pricedown );
			d.set_drop_shadow( 0 );
			d.set_edge( 2 );
			d.m_drop_color = { 0, 0, 0, 255 };
			text( d, w - sx( 32.f ), y_for_health( sy( 89.f ), sy( 12.f ), k_max_health ), buf );
		}

		// weapon icon (DrawWeaponIcon 0x58D7D0) + ammo (DrawAmmo 0x5893B0)
		{
			const sa_weapon_t& wp = sa_weapon( s_s.m_def, s_s.m_knife );
			const float x0 = static_cast< float >( ftol( w - ( sx( 32.f ) + w * 0.17343046f ) ) ), y0 = static_cast< float >( ftol( sy( 20.f ) ) );
			const float iw = sx( 47.f ), ih = sy( 58.f );
			if ( !wp.m_model[ 0 ] )
				sprite( hud_tex( "fist" ), x0, y0, x0 + iw, y0 + ih );
			else
				xlu_sprite( icon_tex( wp.m_model ), x0 + iw / 2.f, y0 + ih / 2.f, iw / 2.f, ih / 2.f, false, false );

			if ( ammo_text( wp, s_s.m_clip, s_s.m_reserve, s_s.m_clip_max, buf, sizeof( buf ) ) ) {
				const int ax = ftol( w - ( w * 0.17343046f + sx( 32.f ) ) + sx( 47.f ) * 0.5f ), ay = ftol( sy( 20.f ) + sy( 43.f ) );
				font::details_t d = fresh( );
				d.m_scale_x = sx( 0.3f ), d.m_scale_y = sy( 0.7f );
				d.set_align( font::align_center );
				d.m_centre_x = sy( 640.f );
				d.m_prop     = true;
				d.set_edge( 1 );
				d.m_drop_color = { 0, 0, 0, 255 };
				d.set_style( font::font_subtitles );
				d.m_color = font::hud_rgba( font::hud_light_blue, 255 );
				text( d, static_cast< float >( ax ), static_cast< float >( ay ), buf );
			}
		}
	}

	/* ---- DrawWanted 0x58D9A0: six "]" gothic stars right to left, empty slots = bigger black ghosts ---- */
	void draw_wanted( )
	{
		const int level      = std::clamp( s_s.m_kills, 0, 6 );
		const bool changed   = level != s_last_wanted;
		const long long now  = now_ms( );
		if ( changed )
			s_last_wanted = level, s_wanted_change = now;
		if ( level <= 0 || changed ) // bab228: the frame of a change draws nothing
			return;
		const bool recent = now <= s_wanted_change + 2000;
		float x           = g.m_w - sx( 29.f );
		const float y     = y_for_health( sy( 114.f ), sy( 12.f ), k_max_health );
		for ( int i = 0; i < 6; ++i, x -= sx( 18.f ) ) {
			const int kind = wanted_star( i, level, 0, recent, g.m_frame );
			if ( !kind )
				continue;
			font::details_t d = fresh( );
			d.set_align( font::align_right );
			d.m_prop = true;
			d.set_style( font::font_gothic );
			d.set_edge( 1 );
			d.m_drop_color = { 0, 0, 0, 255 };
			d.m_scale_x = sx( 0.605f ), d.m_scale_y = sy( 1.21f );
			if ( kind == 3 ) {
				d.set_edge( 0 );
				d.m_color   = { 0, 0, 0, static_cast< std::uint8_t >( ftol( 255.f * 0.7f ) ) };
				d.m_scale_x = sx( 0.605f ) * 1.2f, d.m_scale_y = sy( 1.21f ) * 1.2f;
				text( d, x, y - sy( 1.f ) * 2.f, "]" );
				continue;
			}
			font::rgba_t gold = font::hud_rgba( font::hud_gold, 255 );
			if ( kind == 2 )
				gold = { static_cast< std::uint8_t >( ftol( gold.r * 0.8f ) ), static_cast< std::uint8_t >( ftol( gold.g * 0.8f ) ),
				         static_cast< std::uint8_t >( ftol( gold.b * 0.8f ) ), 255 };
			d.m_color = gold;
			text( d, x, y, "]" );
		}
	}

	/* ---- DrawSuccessFailedMessage 0x58C6A0 + DrawBustedWastedMessage 0x58CA50 ---- */
	void draw_big_messages( )
	{
		{
			int lines = 1;
			for ( const char* p = s_success.m_text; ( p = std::strstr( p, "~n~" ) ) != nullptr; p += 3 )
				++lines;
			float alpha = 0.f;
			if ( s_success.step( g.m_w, g.m_h, g.m_sy, g.m_dt, lines, alpha ) && s_success.m_text[ 0 ] ) {
				const auto a      = static_cast< std::uint8_t >( ftol( alpha ) );
				font::details_t d = fresh( );
				d.m_scale_x = sx( 1.3f ), d.m_scale_y = sy( 1.8f );
				d.m_prop    = true;
				d.m_justify = false;
				d.set_align( font::align_center );
				d.m_centre_x = sx( 590.f );
				d.set_style( font::font_pricedown );
				d.set_edge( 2 );
				d.m_drop_color = { 0, 0, 0, a };
				d.m_color      = font::hud_rgba( font::hud_gold, a );
				text( d, static_cast< float >( static_cast< int >( g.m_w ) / 2 ), s_success.m_y, s_success.m_text );
			}
		}
		{
			const bool arming = s_wasted.m_on && s_wasted.m_x == 0.f;
			const float alpha = s_wasted.step( g.m_dt );
			if ( arming )
				s_zone.m_state = name_dont_show;
			if ( alpha >= 0.f ) {
				const auto a      = static_cast< std::uint8_t >( ftol( alpha ) );
				font::details_t d = fresh( );
				d.m_scale_x = sx( 2.1f ), d.m_scale_y = sy( 2.1f );
				d.m_prop    = true;
				d.m_justify = false;
				d.set_align( font::align_center );
				d.set_style( font::font_gothic );
				d.set_edge( 3 );
				d.m_drop_color = { 0, 0, 0, a };
				d.m_color      = font::hud_rgba( font::hud_light_gray, a );
				text( d, g.m_w / 2.f, static_cast< float >( static_cast< int >( g.m_h ) / 2 ) - sy( 30.f ), gxt( "DEAD" ), true );
			}
		}
	}

	/* ---- csgo overview = sa radar tiles: resource/overviews/<map>.txt + <map>_radar.dds (cod4 loader) ----
	   parsed + read once per map; a device Reset only drops the texture, rebuilt from the kept bytes */
	struct overview_t {
		std::string m_map{ };
		bool m_tried = false, m_have = false, m_tex_tried = false;
		float m_pos_x = 0.f, m_pos_y = 0.f, m_scale = 0.f;
		std::vector< std::uint8_t > m_dds{ };
		IDirect3DTexture9* m_texture = nullptr;
	} s_overview{ };

	void release_overview( )
	{
		if ( s_overview.m_texture )
			s_overview.m_texture->Release( );
		s_overview.m_texture   = nullptr;
		s_overview.m_tex_tried = false;
	}

	void load_overview( const std::string& map )
	{
		if ( s_overview.m_map != map || !s_overview.m_tried ) {
			release_overview( );
			s_overview       = { };
			s_overview.m_map   = map;
			s_overview.m_tried = true;
			if ( map.empty( ) )
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
			std::ifstream dds( base / ( map + "_radar.dds" ), std::ios::binary );
			s_overview.m_dds.assign( std::istreambuf_iterator< char >( dds ), std::istreambuf_iterator< char >( ) );
			s_overview.m_have = !s_overview.m_dds.empty( );
		}
		if ( !s_overview.m_have || s_overview.m_texture || s_overview.m_tex_tried || !g_interfaces.m_direct_device )
			return;
		s_overview.m_tex_tried = true;
		const auto& f          = s_overview.m_dds;
		const auto load        = [ & ]( DWORD usage, D3DPOOL pool ) {
			return D3DXCreateTextureFromFileInMemoryEx( g_interfaces.m_direct_device, f.data( ), static_cast< UINT >( f.size( ) ), D3DX_DEFAULT_NONPOW2,
			                                            D3DX_DEFAULT_NONPOW2, 1, usage, D3DFMT_A8R8G8B8, pool, D3DX_FILTER_NONE, D3DX_FILTER_NONE, 0,
			                                            nullptr, nullptr, &s_overview.m_texture );
		};
		if ( FAILED( load( 0, D3DPOOL_MANAGED ) ) && FAILED( load( D3DUSAGE_DYNAMIC, D3DPOOL_DEFAULT ) ) )
			s_overview.m_texture = nullptr;
	}

	/* ---- CHud::DrawRadar 0x58A330: map (24-gon mask), disc ring, north, icon blips, trace blips, arrow ---- */
	void draw_radar( )
	{
		namespace rd = n_gta_radar;
		load_overview( s_s.m_map );
		rd::radar_t r{ };
		r.m_cx = sx( 87.f ), r.m_cy = g.m_h - sy( 66.f ), r.m_rx = sx( 47.f ), r.m_ry = sy( 38.f );
		r.m_ox = s_s.m_origin[ 0 ], r.m_oy = s_s.m_origin[ 1 ];
		r.m_range = 180.f * std::clamp( GET_VARIABLE( g_variables.m_gta_hud_radar_units, float ), 1.f, 100.f );
		r.set_yaw( s_s.m_yaw );

		ImDrawList* const l          = g.m_list;
		const ImDrawListFlags flags  = l->Flags;
		l->Flags                    &= ~ImDrawListFlags_AntiAliasedFill; // sa fills have no fringe
		const auto poly = [ & ]( const rd::vec_t* p, int n, font::rgba_t c ) {
			ImVec2 v[ 32 ];
			for ( int i = 0; i < n && i < 32; ++i )
				v[ i ] = ImVec2( p[ i ].x, p[ i ].y );
			l->AddConvexPolyFilled( v, std::min( n, 32 ), col( c ) );
		};

		// map: out-of-world sea first (DrawRadarSection oob colour), the overview clipped to the mask on top
		static const std::vector< rd::vec_t > mask = rd::mask_polygon( );
		{
			std::vector< rd::vec_t > screen{ };
			for ( const rd::vec_t& m : mask )
				screen.push_back( r.to_screen( m ) );
			poly( screen.data( ), static_cast< int >( screen.size( ) ), { 111, 137, 170, 255 } );
		}
		if ( s_overview.m_texture ) {
			const float span = 1024.f * s_overview.m_scale, px = s_overview.m_pos_x, py = s_overview.m_pos_y;
			const std::vector< rd::vec_t > quad = { r.to_radar( px, py ), r.to_radar( px + span, py ), r.to_radar( px + span, py - span ), r.to_radar( px, py - span ) };
			const std::vector< rd::vec_t > cut  = rd::clip( quad, mask );
			if ( cut.size( ) >= 3 ) {
				const int n = static_cast< int >( cut.size( ) );
				l->PushTextureID( reinterpret_cast< ImTextureID >( s_overview.m_texture ) );
				l->PrimReserve( ( n - 2 ) * 3, n );
				const ImDrawIdx base = static_cast< ImDrawIdx >( l->_VtxCurrentIdx );
				for ( const rd::vec_t& c : cut ) {
					const rd::vec_t w = r.to_world( c ), s = r.to_screen( c );
					l->PrimWriteVtx( ImVec2( s.x, s.y ), ImVec2( ( w.x - px ) / span, ( py - w.y ) / span ), IM_COL32_WHITE );
				}
				for ( int i = 1; i + 1 < n; ++i ) {
					l->PrimWriteIdx( base );
					l->PrimWriteIdx( static_cast< ImDrawIdx >( base + i ) );
					l->PrimWriteIdx( static_cast< ImDrawIdx >( base + i + 1 ) );
				}
				l->PopTextureID( );
			}
		}

		// disc ring: radardisc tinted black, uv (1,1) on the centre, mirrored per quadrant
		{
			const int disc = hud_tex( "radardisc" );
			const font::rgba_t black{ 0, 0, 0, 255 };
			const float x36 = sx( 36.f ), x87 = sx( 87.f ), x138 = sx( 138.f );
			const float y108 = g.m_h - sy( 108.f ), y66 = g.m_h - sy( 66.f ), y24 = g.m_h - sy( 24.f );
			sprite( disc, x36, y108, x87, y66, black );
			sprite( disc, x138, y108, x87, y66, black );
			sprite( disc, x36, y24, x87, y66, black );
			sprite( disc, x138, y24, x87, y66, black );
		}

		// DrawRadarSprite: 2 * (int)SX(8) x 2 * (int)SY(8), white, upright
		const float iw = static_cast< float >( ftol( sx( 8.f ) ) ), ih = static_cast< float >( ftol( sy( 8.f ) ) );
		const auto icon = [ & ]( const char* name, float wx, float wy ) {
			rd::vec_t p = r.to_radar( wx, wy );
			rd::limit( p );
			const rd::vec_t s = r.to_screen( p );
			sprite( hud_tex( name ), s.x - iw, s.y - ih, s.x + iw, s.y + ih );
		};
		icon( "radar_north", r.m_ox, r.m_oy + r.m_range * 1.41421f );
		// csgo bomb as an icon blip: planted = fire, carried / dropped = flag
		if ( s_s.m_bomb )
			icon( s_s.m_bomb_planted ? "radar_fire" : "radar_flag", s_s.m_bomb_pos[ 0 ], s_s.m_bomb_pos[ 1 ] );

		// trace blips: teammates = recruited homies (grove 70,200,0, size 2), enemies = script threat (180,25,31, size 3)
		for ( int pass = 0; pass < 2; ++pass )
			for ( const blip_t& b : s_s.m_blips ) {
				if ( b.m_enemy != ( pass == 1 ) )
					continue;
				rd::vec_t p = r.to_radar( b.m_pos[ 0 ], b.m_pos[ 1 ] );
				rd::limit( p );
				const rd::vec_t s  = r.to_screen( p );
				const int height   = rd::height_of( b.m_pos[ 2 ] - s_s.m_origin[ 2 ] );
				const int size     = b.m_enemy ? 3 : 2;
				const font::rgba_t fill = b.m_enemy ? font::rgba_t{ 180, 25, 31, 255 } : font::rgba_t{ 70, 200, 0, 255 };
				const rd::shape_t outer = rd::trace_shape( s.x, s.y, size, height, true, g.m_sx, g.m_sy );
				const rd::shape_t inner = rd::trace_shape( s.x, s.y, size, height, false, g.m_sx, g.m_sy );
				poly( outer.m_p, outer.m_n, { 0, 0, 0, 255 } );
				poly( inner.m_p, inner.m_n, fill );
			}

		// player arrow last: radar_centre, half-diagonal (int)SX(8) on both axes (pixel square)
		{
			rd::vec_t c = r.to_radar( r.m_ox, r.m_oy );
			rd::limit( c );
			const rd::vec_t p = r.to_screen( c );
			const float wh    = static_cast< float >( static_cast< unsigned int >( sx( 8.f ) ) );
			rd::vec_t v[ 4 ]{ };
			rd::rotating_quad( p.x, p.y, rd::arrow_angle( s_s.m_body_yaw, r ), wh, wh, v );
			if ( IDirect3DTexture9* const t = texture( hud_tex( "radar_centre" ) ) )
				l->AddImageQuad( reinterpret_cast< ImTextureID >( t ), ImVec2( v[ 0 ].x, v[ 0 ].y ), ImVec2( v[ 1 ].x, v[ 1 ].y ), ImVec2( v[ 2 ].x, v[ 2 ].y ),
				                 ImVec2( v[ 3 ].x, v[ 3 ].y ), ImVec2( 0.f, 0.f ), ImVec2( 1.f, 0.f ), ImVec2( 1.f, 1.f ), ImVec2( 0.f, 1.f ), IM_COL32_WHITE );
		}
		l->Flags = flags;
	}

	/* ---- DrawAreaName 0x58AA50 (DrawAfterFade) ---- */
	void draw_area_name( )
	{
		const float alpha = s_zone.step( s_s.m_zone.c_str( ), static_cast< int >( g.m_dt ), s_wasted.showing( ) );
		if ( alpha < 0.f || !s_zone.m_print[ 0 ] )
			return;
		const auto a      = static_cast< std::uint8_t >( ftol( alpha ) );
		font::details_t d = fresh( );
		d.m_prop    = true;
		d.m_scale_x = sx( 1.2f ), d.m_scale_y = sy( 1.9f );
		d.set_edge( 2 );
		d.set_align( font::align_right );
		d.m_right_wrap = sx( 180.f );
		d.m_drop_color = { 0, 0, 0, a };
		d.set_style( font::font_gothic );
		d.m_color = font::hud_rgba( font::hud_light_blue, a );
		text( d, g.m_w - sx( 32.f ), ( g.m_h - sy( 104.f ) ) + sy( 76.f ), s_zone.m_print, true );
	}
}

/* ---- game thread ---- */

void n_gta_hud::impl_t::on_paint_traverse( )
{
	const bool on      = n_gta_hud::enabled( );
	auto* const engine = g_interfaces.m_engine_client;
	const bool in_game = engine && engine->is_in_game( ) && g_ctx.m_local;

	if ( on && s_load_state == 0 ) {
		s_load_state = 1;
		++g_url_image_downloads; // before the thread runs: an eject right now must still wait for it
		std::thread( load_worker ).detach( );
	}

	drain_chat( on && in_game );

	snapshot_t s{ };
	s.m_in_game = on && in_game && g_convars.int_or( HASH_BT( "cl_drawhud" ), 1 ) != 0;
	if ( !s.m_in_game ) {
		std::lock_guard< std::mutex > lock( m_lock );
		m_snapshot = std::move( s );
		return;
	}

	c_base_entity* const local = g_ctx.m_local;
	s.m_team                   = local->get_team( );
	s.m_map                    = engine->get_level_name_short( ) ? engine->get_level_name_short( ) : "";

	// subject: local, or whoever is followed in eye / chase
	c_base_entity* subject = local;
	bool chase             = false;
	if ( !local->is_alive( ) ) {
		const int mode = local->get_observer_mode( );
		auto* target   = g_interfaces.m_client_entity_list->get< c_base_entity >( local->get_observer_target_handle( ) );
		if ( ( mode == 4 || mode == 5 ) && target && target->is_player( ) && target->is_alive( ) && target != local )
			subject = target, chase = mode == 5;
		else if ( ( s.m_team == 2 || s.m_team == 3 ) && mode <= 2 )
			s.m_wasted = true; // none / death / freeze cam right after dying: sa's wasted screen
		else
			s.m_free = true; // spectator team, free roam, fixed cam: nobody's stats to show
	}
	s.m_alive  = subject->is_alive( );
	s.m_health = std::clamp( subject->get_health( ), 0, 100 );
	s.m_armor  = std::clamp( subject->get_armor( ), 0, 100 );
	s.m_money  = subject->get_money( );
	static const unsigned int round_kills = net_offset( HASH_BT( "CCSPlayer->m_iNumRoundKills" ) );
	s.m_kills  = round_kills ? field< int >( subject, round_kills ) : 0;
	s.m_scoped = subject->is_scoped( );
	s.m_ducking = ( subject->get_flags( ) & fl_ducking ) != 0;
	if ( const char* place = subject->get_last_place( ); place && *place ) {
		const wchar_t* w = g_interfaces.m_localize ? g_interfaces.m_localize->find_safe( place ) : nullptr;
		s.m_zone         = ascii( w && *w ? g_utilities.to_utf8( w ) : std::string( place ) );
		if ( s.m_zone.empty( ) ) // non-latin client language: sa's font has no glyphs, the raw token ("BombsiteA") still reads
			s.m_zone = ascii( place );
	}

	// active weapon
	if ( auto* const weapon = g_interfaces.m_client_entity_list->get< c_base_entity >( subject->get_active_weapon_handle( ) ) ) {
		const short def                 = weapon->get_item_definition_index( );
		const c_weapon_data* const data = g_interfaces.m_weapon_system->get_weapon_data( def );
		s.m_def                         = def;
		s.m_knife                       = data && data->m_weapon_type == weapontype_knife;
		s.m_clip                        = weapon->get_ammo( );
		s.m_reserve                     = weapon->get_ammo_reserve( );
		s.m_clip_max                    = data ? data->m_max_clip1 : 0;
		if ( data && data->m_weapon_type == weapontype_grenade ) {
			// grenade count = CBasePlayer::m_iAmmo[ m_iPrimaryAmmoType ]
			static const unsigned int ammo_offset = net_offset( HASH_BT( "CBasePlayer->m_iAmmo" ) );
			static const unsigned int type_offset = net_offset( HASH_BT( "CBaseCombatWeapon->m_iPrimaryAmmoType" ) );
			const int type = type_offset ? field< int >( weapon, type_offset ) : -1;
			s.m_reserve    = ammo_offset && type >= 0 && type < 32 ? field< int >( subject, ammo_offset + type * 4 ) : 1;
		}
		else if ( data && data->m_weapon_type == weapontype_c4 )
			s.m_reserve = 1;
		// m_fAttackButtonCounter has no csgo twin: inaccuracy over the standing value, 0 standing still
		// ponytail: 0.25 per standing-inaccuracy step, capped 4 (radius x3), tune in game
		if ( data && subject == local ) {
			const float stand = std::max( data->m_inaccuracy_stand[ 0 ], 0.0001f );
			s.m_counter       = std::clamp( ( weapon->get_inaccuracy( ) - stand ) / stand * 0.25f, 0.f, 4.f );
		}
	}

	// radar
	const c_vector& origin = subject->get_abs_origin( );
	s.m_origin[ 0 ] = origin.m_x, s.m_origin[ 1 ] = origin.m_y, s.m_origin[ 2 ] = origin.m_z;
	c_angle view{ };
	engine->get_view_angles( view );
	static const unsigned int eye_yaw = net_offset( HASH_BT( "CCSPlayer->m_angEyeAngles[1]" ) );
	// radar turns with the rendered camera (CalculateCachedSinCos), arrow = body: in-eye = target eyes, chase = our view angles
	s.m_body_yaw = subject == local ? view.m_y : eye_yaw ? field< float >( subject, eye_yaw ) : subject->get_abs_angles( ).m_y;
	s.m_yaw      = subject == local || chase ? view.m_y : s.m_body_yaw;
	const int subject_team = subject->get_team( );
	for ( int i = 1; i <= 64; ++i ) {
		auto* const p = g_interfaces.m_client_entity_list->get< c_base_entity >( i );
		if ( !p || p == subject || p->is_dormant( ) || !p->is_player( ) || !p->is_alive( ) )
			continue;
		const int team = p->get_team( );
		if ( team != 2 && team != 3 )
			continue;
		blip_t b{ };
		const c_vector& o = p->get_abs_origin( );
		b.m_pos[ 0 ] = o.m_x, b.m_pos[ 1 ] = o.m_y, b.m_pos[ 2 ] = o.m_z;
		b.m_team  = team;
		b.m_enemy = team != subject_team;
		s.m_blips.push_back( b );
	}

	// clock: bomb timer while planted, else round time left, warmup countdown
	const float now_game = g_interfaces.m_global_vars_base->m_current_time;
	const int highest    = std::min( g_interfaces.m_client_entity_list->get_highest_entity_index( ), 2048 );
	for ( int i = 1; i <= highest; ++i ) {
		auto* const e = g_interfaces.m_client_entity_list->get< c_base_entity >( i );
		if ( !e || e->is_dormant( ) )
			continue;
		const auto client_class = static_cast< c_client_networkable* >( e )->get_client_class( );
		if ( !client_class )
			continue;
		if ( client_class->m_class_id == e_class_ids::c_planted_c4 && e->is_bomb_ticking( ) ) {
			const c_vector& o = e->get_abs_origin( );
			s.m_bomb = s.m_bomb_planted = true;
			s.m_bomb_pos[ 0 ] = o.m_x, s.m_bomb_pos[ 1 ] = o.m_y, s.m_bomb_pos[ 2 ] = o.m_z;
			s.m_clock_ms = static_cast< int >( std::max( e->get_c4_blow_time( ) - now_game, 0.f ) * 1000.f );
		}
		else if ( client_class->m_class_id == e_class_ids::cc4 && !s.m_bomb ) {
			auto* owner = g_interfaces.m_client_entity_list->get< c_base_entity >( e->get_owner_entity_handle( ) );
			if ( owner && owner->is_player( ) && owner->get_team( ) != subject_team )
				continue; // carried by an enemy: sa only shows what the player knows
			const c_vector& o = owner ? owner->get_abs_origin( ) : e->get_abs_origin( );
			s.m_bomb = true;
			s.m_bomb_pos[ 0 ] = o.m_x, s.m_bomb_pos[ 1 ] = o.m_y, s.m_bomb_pos[ 2 ] = o.m_z;
		}
	}
	if ( s.m_clock_ms < 0 ) {
		static const unsigned int warmup_ofs = net_offset( HASH_BT( "CCSGameRulesProxy->m_bWarmupPeriod" ) );
		static const unsigned int warmup_end = net_offset( HASH_BT( "CCSGameRulesProxy->m_fWarmupPeriodEnd" ) );
		static const unsigned int time_ofs   = net_offset( HASH_BT( "CCSGameRulesProxy->m_iRoundTime" ) );
		static const unsigned int start_ofs  = net_offset( HASH_BT( "CCSGameRulesProxy->m_fRoundStartTime" ) );
		const std::uintptr_t rules = game_rules( );
		bool warmup = false;
		float wend = 0.f, start = 0.f;
		int round = 0;
		if ( rules && time_ofs && start_ofs ) {
			const void* r = reinterpret_cast< void* >( rules );
			warmup        = warmup_ofs && field< bool >( r, warmup_ofs );
			wend          = warmup_end ? field< float >( r, warmup_end ) : 0.f;
			round         = field< int >( r, time_ofs );
			start         = field< float >( r, start_ofs );
			s.m_clock_ms  = round_clock_ms( warmup, wend, round, start, now_game );
		}
		// one line per clock state change: says which input pins the clock (see memory proj-gta-hud-1116)
		static int s_logged = -2;
		const int state = !rules ? -1 : ( warmup ? 1 : 0 ) | ( round > 0 ? 2 : 0 ) | ( s.m_clock_ms > 0 ? 4 : 0 ) | ( s.m_clock_ms < 0 ? 8 : 0 );
		if ( state != s_logged ) {
			s_logged = state;
			botox_dbg_log( "GTAT: rules %p ofs %u/%u/%u warmup %d wend %.1f round %d start %.1f now %.1f -> %d ms", reinterpret_cast< void* >( rules ),
			               warmup_ofs, time_ofs, start_ofs, warmup ? 1 : 0, wend, round, start, now_game, s.m_clock_ms );
		}
	}

	std::lock_guard< std::mutex > lock( m_lock );
	m_snapshot = std::move( s );
}

void n_gta_hud::impl_t::on_event( game_event_t* event )
{
	if ( !n_gta_hud::enabled( ) || !event || !g_interfaces.m_engine_client || !g_ctx.m_local )
		return;
	const std::string_view name = event->get_name( );
	if ( name != "round_end" )
		return;
	// reason 16 = game commencing, 10 = draw: no mission result
	const int reason = event->get_int( "reason" ), winner = event->get_int( "winner" ), team = g_ctx.m_local->get_team( );
	if ( reason == 16 || reason == 10 || ( team != 2 && team != 3 ) || ( winner != 2 && winner != 3 ) )
		return;
	std::lock_guard< std::mutex > lock( m_lock );
	s_ev.m_success = gxt( winner == team ? "M_PASSR" : "M_FAIL" );
}

// game thread (push_notice): csgo + botox chat lines -> CFont lines
void n_gta_hud::impl_t::on_chat( const char* text, int client, bool html )
{
	if ( !n_gta_hud::enabled( ) || !text || !*text )
		return;
	const int sender = !html && client > 0 && client <= 64 ? client : 0;
	int team         = 0;
	if ( sender )
		if ( auto* e = g_interfaces.m_client_entity_list->get< c_base_entity >( sender ) )
			team = e->get_team( );
	std::string line = chat_line( text, html, team );
	std::lock_guard< std::mutex > lock( m_lock );
	// stamped on arrival (render may not drain while out of game / cl_drawhud 0), newest kept
	if ( s_ev.m_chat.size( ) >= static_cast< size_t >( k_chat_lines ) )
		s_ev.m_chat.erase( s_ev.m_chat.begin( ) );
	s_ev.m_chat.emplace_back( std::move( line ), now_ms( ) );
}

// game thread (paint_traverse): send / drop the typed line, mc_hud's drain
void n_gta_hud::impl_t::drain_chat( bool active )
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
			send             = m_chat_result == 1;
			team             = m_chat_team;
			message          = g_utilities.to_utf8( m_chat_input );
			m_chat_open      = false;
			m_chat_result    = 0;
			m_chat_open_char = 0;
			m_chat_input.clear( );
		}
	}
	std::erase( message, '"' );
	std::erase( message, ';' );
	if ( send && !message.empty( ) )
		g_interfaces.m_engine_client->client_cmd_unrestricted( ( std::string( team ? "say_team \"" : "say \"" ) + message + "\"" ).c_str( ) );
}

// window thread: the messagemode bind opens our line instead of the csgo chat panel (urt / cod4 / mc copy)
bool n_gta_hud::impl_t::chat_on_key( unsigned int msg, unsigned int wide_param, long long_param, bool ui_input )
{
	if ( msg != WM_KEYDOWN && msg != WM_KEYUP && msg != WM_CHAR )
		return false;
	if ( ui_input || wide_param == g_menu.menu_key( ) )
		return false;
	std::scoped_lock lock( m_chat_mutex );
	const bool stale = GetTickCount64( ) - m_chat_drain_ms > 1000ull;
	if ( m_chat_open && stale ) {
		m_chat_open      = false;
		m_chat_result    = 0;
		m_chat_open_char = 0;
		m_chat_input.clear( );
	}
	if ( msg == WM_KEYUP )
		return false;
	if ( !m_chat_open ) {
		if ( msg != WM_KEYDOWN || stale || !n_gta_hud::ready( ) )
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
		if ( m_chat_open_char ) {
			const wchar_t opener = m_chat_open_char;
			m_chat_open_char     = 0;
			if ( character == opener || character == static_cast< wchar_t >( opener - 32 ) )
				return true;
		}
		// MAX_SAY_TEXT 150
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

bool n_gta_hud::hides_scope( )
{
	const snapshot_t& s = g_gta_hud.m_snapshot;
	return ready( ) && s.m_in_game && s.m_alive && s.m_scoped;
}

// fonts are what every element needs; without them the stock hud stays (offline, 404, half a cache)
bool n_gta_hud::ready( )
{
	return enabled( ) && s_load_state == 2 && !s_files[ k_font_base ].empty( ) && !s_files[ k_font_base + 1 ].empty( );
}

void n_gta_hud::impl_t::on_release( ) { }

/* ---- render thread ---- */

void n_gta_hud::impl_t::on_end_scene( )
{
	if ( !n_gta_hud::enabled( ) )
		return;
	static long long s_last = 0;
	const long long now     = now_ms( );
	g.m_dt                  = s_last ? static_cast< float >( std::clamp< long long >( now - s_last, 0, 100 ) ) : 0.f;
	s_last                  = now;
	g.m_frame               = frame_of( now );
	{
		std::lock_guard< std::mutex > lock( m_lock );
		// chat drains in or out of game: lines carry their arrival time
		for ( auto& [ line, ms ] : s_ev.m_chat )
			s_chat.push_back( { std::move( line ), ms } );
		s_ev.m_chat.clear( );
		if ( s_chat.size( ) > static_cast< size_t >( k_chat_lines ) )
			s_chat.erase( s_chat.begin( ), s_chat.end( ) - k_chat_lines );
		if ( !m_snapshot.m_in_game )
			return;
		s_s = m_snapshot;
		if ( !s_ev.m_success.empty( ) )
			s_success.set( s_ev.m_success.c_str( ) ), s_ev.m_success.clear( );
	}
	{
		std::scoped_lock lock( m_chat_mutex );
		s_chat_prefix = m_chat_open ? std::string( m_chat_team ? "Say (team): " : "Say: " ) : std::string( );
		s_chat_typed  = m_chat_open ? ascii( g_utilities.to_utf8( m_chat_input ) ) : std::string( );
		std::erase( s_chat_typed, '~' );
	}
	if ( !n_gta_hud::ready( ) )
		return;

	const int w = static_cast< int >( g_ctx.m_width ), h = static_cast< int >( g_ctx.m_height );
	if ( w <= 0 || h <= 0 )
		return;
	// sa stretches 640 x 448 over the whole screen; laid out in the pre-stretch width, shifted after (cod4 note)
	const float left = g_render.to_overlay_x( 0.f ), right = g_render.to_overlay_x( static_cast< float >( w ) );
	g.m_list               = ImGui::GetBackgroundDrawList( );
	g.m_w                  = right - left;
	g.m_h                  = static_cast< float >( h );
	g.m_sx                 = g.m_w / 640.f;
	g.m_sy                 = g.m_h / 448.f;
	const int first_vertex = g.m_list->VtxBuffer.Size;

	s_wasted.set( s_s.m_wasted );

	// CHud::Draw order, then DrawAfterFade. spectating nobody: no player stats, the radar stays
	if ( !s_s.m_free ) {
		draw_crosshair( );
		draw_player_info( );
		draw_wanted( );
	}
	draw_radar( );
	draw_big_messages( );
	draw_area_name( );
	draw_chat( );

	for ( int v = first_vertex; left != 0.f && v < g.m_list->VtxBuffer.Size; ++v )
		g.m_list->VtxBuffer[ v ].pos.x += left;
	g.m_list->AddCallback( ImDrawCallback_ResetRenderState, nullptr );
}

void n_gta_hud::impl_t::release_textures( )
{
	for ( texture_t& t : s_textures ) {
		if ( t.m_texture )
			t.m_texture->Release( );
		t = { };
	}
	release_overview( );
}

/* ---- options (visuals tab popup) ---- */

void n_gta_hud::draw_options( )
{
	const int state = s_load_state;
	if ( state == 2 )
		ImGui::TextDisabled( s_missing ? "gta: %d files missing" : "gta: ready", s_missing );
	else if ( state == 3 ) {
		ImGui::TextDisabled( "gta: download failed" );
		if ( ImGui::Button( "retry##gta", ImVec2( -1.f, 15.f ) ) )
			s_load_state = 0;
	}
	else
		ImGui::TextDisabled( "gta: loading" );
}
