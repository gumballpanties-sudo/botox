#include "urt_hud.h"
#include "urt_core.h"
#include "urt_logic.h"

#include "../../utilities/console/console.h"
#include "../menu/menu.h"
#include "../misc/misc.h"
#include "../misc/scaleform/image_cache.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <thread>

#include <mmsystem.h>
#pragma comment( lib, "winmm" )

using namespace n_urt;
using namespace n_urt_logic;

extern bool point_menu_is_opened( );
extern void botox_dbg_log( const char* fmt, ... );

bool n_urt_hud::enabled( ) { return GET_VARIABLE( g_variables.m_urt_hud, bool ); }

namespace
{
	using n_urt_hud::player_t;
	using n_urt_hud::snapshot_t;

	int now_ms( )
	{
		static const auto start = std::chrono::steady_clock::now( );
		return static_cast< int >( std::chrono::duration_cast< std::chrono::milliseconds >( std::chrono::steady_clock::now( ) - start ).count( ) );
	}

	/* ---- assets: botox-assets urt/<pk3 path>, cached once ---- */
	constexpr const char* k_base_url  = "https://raw.githubusercontent.com/gumballpanties-sudo/botox-assets/main/urt/";
	constexpr const char* k_cache_dir = "C:\\botox\\urt\\";
	constexpr int k_fetch_threads     = 8;

	std::atomic< int > s_load_state{ 0 }; // 0 idle, 1 loading, 2 ready, 3 failed
	int s_missing = 0;
	std::vector< std::vector< std::uint8_t > > s_files{ }; // per k_images, immutable once state 2
	std::vector< texture_t > s_textures{ };

	std::string cache_path( const char* rel )
	{
		std::string p = std::string( k_cache_dir ) + rel;
		std::replace( p.begin( ), p.end( ), '/', '\\' );
		return p;
	}

	bool read_bytes( const std::string& path, std::vector< std::uint8_t >& out )
	{
		std::ifstream file( path, std::ios::binary );
		out.assign( std::istreambuf_iterator< char >( file ), std::istreambuf_iterator< char >( ) );
		return !out.empty( );
	}

	void load_worker( )
	{
		std::atomic< size_t > next{ 0 };
		std::vector< std::thread > pool{ };
		for ( int t = 0; t < k_fetch_threads; ++t )
			pool.emplace_back( [ & ] {
				std::error_code ec{ };
				for ( size_t i; ( i = next++ ) < std::size( k_images ); ) {
					const std::string path = cache_path( k_images[ i ].m_path );
					std::filesystem::create_directories( std::filesystem::path( path ).parent_path( ), ec );
					if ( !std::filesystem::exists( path, ec ) && !n_image_cache::download_file( std::string( k_base_url ) + k_images[ i ].m_path, path ) )
						botox_dbg_log( "URT: download failed %s%s", k_base_url, k_images[ i ].m_path );
				}
			} );
		for ( std::thread& t : pool )
			t.join( );

		std::vector< std::vector< std::uint8_t > > files( std::size( k_images ) );
		int missing = 0;
		for ( size_t i = 0; i < std::size( k_images ); ++i ) {
			const std::string path = cache_path( k_images[ i ].m_path );
			if ( !read_bytes( path, files[ i ] ) ) {
				DeleteFileA( path.c_str( ) );
				++missing;
			}
		}
		char line[ 160 ]{ };
		sprintf_s( line, "URT: %zu files, %d missing (%s)", std::size( k_images ), missing, k_cache_dir );
		g_console.print( line );
		s_missing = missing;
		if ( missing == static_cast< int >( std::size( k_images ) ) )
			s_load_state = 3;
		else {
			s_files      = std::move( files );
			s_load_state = 2;
		}
		--g_url_image_downloads;
	}

	int img( std::string_view name )
	{
		static std::unordered_map< std::string, int > cache{ };
		const auto it = cache.find( std::string( name ) );
		if ( it != cache.end( ) )
			return it->second;
		return cache[ std::string( name ) ] = image_index( name );
	}

	// trap_S_StartLocalSound: the wav straight out of memory
	void play( std::string_view wav )
	{
		const int i = image_index( wav );
		if ( s_load_state == 2 && i >= 0 && !s_files[ i ].empty( ) )
			PlaySoundA( reinterpret_cast< LPCSTR >( s_files[ i ].data( ) ), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT );
	}

	/* ---- cvars: render thread owns them, the game thread reads the few it needs through atomics ---- */
	cvars_t s_cv{ };
	std::string s_cv_text{ "\x01" };
	struct game_cvars_t {
		std::atomic< int > m_hitsound{ 0 }, m_killsound{ 0 }, m_chat_sound{ 2 }, m_team_chats_only{ 0 }, m_ally{ 0 }, m_enemy{ 0 }, m_fixed{ 0 };
	} s_gcv{ };

	void sync_cvars( )
	{
		const std::string& saved = GET_VARIABLE( g_variables.m_urt_hud_cvars, std::string );
		if ( saved == s_cv_text )
			return;
		s_cv.load( saved );
		s_cv_text = saved;
		s_gcv.m_hitsound        = s_cv.i( "cg_hitsound" );
		s_gcv.m_killsound       = s_cv.i( "cg_killsound" );
		s_gcv.m_chat_sound      = s_cv.i( "cg_chatSound" );
		s_gcv.m_team_chats_only = s_cv.i( "cg_teamChatsOnly" );
		s_gcv.m_ally            = s_cv.i( "cg_skinAlly" );
		s_gcv.m_enemy           = s_cv.i( "cg_skinEnemy" );
		s_gcv.m_fixed           = s_cv.i( "cg_skinFixed" );
	}

	void save_cvar( const std::string& name, const std::string& value )
	{
		s_cv.set( name, value );
		GET_VARIABLE( g_variables.m_urt_hud_cvars, std::string ) = s_cv.save( );
	}

	/* ---- teams ---- */
	// csgo 2 T / 3 CT / 1 spectator -> urt 1 red / 2 blue / 3 spectator
	int urt_team( int cs ) { return cs == 2 ? 1 : cs == 3 ? 2 : cs == 1 ? 3 : 0; }

	int skin( int team, int my_team ) { return skin_for( team, my_team, s_gcv.m_ally, s_gcv.m_enemy, s_gcv.m_fixed != 0 ); }

	// sub_101234: "<skin color code>name^7"
	std::string colored( const std::string& name, int team, int my_team )
	{
		static constexpr const char* k_codes[ 14 ] = { "^2", "^4", "^1", "^6", "^8", "^9", "^7", "^0", "^3", "^7", "^4", "^7", "^6", "^5" };
		return std::string( k_codes[ skin( team, my_team ) ] ) + name + "^7";
	}

	std::string player_name( int index )
	{
		player_info_t info{ };
		if ( index > 0 && g_interfaces.m_engine_client->get_player_info( index, &info ) )
			return to_q3( strip_colors( std::string( info.m_name, strnlen( info.m_name, sizeof( info.m_name ) ) ) ) );
		return { };
	}

	unsigned int net_offset( unsigned int hash ) { return g_netvars[ hash ].m_offset; }

	template < class T >
	T field( const void* base, unsigned int ofs )
	{
		return *reinterpret_cast< const T* >( reinterpret_cast< std::uintptr_t >( base ) + ofs );
	}

	std::uintptr_t game_rules( )
	{
		static const auto site =
			g_modules[ CLIENT_DLL ].find_pattern( "8B 4D 04 E8 ? ? ? ? 8B 0D ? ? ? ? 8B 01 FF 90 98 04 00 00 C6 45 FC 00" );
		return site ? reinterpret_cast< std::uintptr_t >( **reinterpret_cast< void*** >( site + 10 ) ) : 0;
	}

	/* ---- events: game thread writes, render thread drains (both under g_urt_hud.m_lock) ---- */
	struct lag_sample_t {
		int m_kind; // 0 frame, 1 snapshot
		int m_value;
		int m_flags; // snapshot: 1 rate delayed (choke), -1 value = dropped
	};

	struct events_t {
		std::vector< std::pair< int, std::string > > m_lines{ }; // 0 chat box, 1 message box
		std::vector< obituary_t > m_obits{ };
		std::vector< std::string > m_centers{ };
		std::vector< lag_sample_t > m_lag{ };
		bool m_hit[ 15 ]{ };  // CG hit locations (gfx/hud/damage), cleared on spawn
		bool m_limp = false;
		int m_weapon_switch = 0;
		unsigned int m_last_weapon = 0;
		float m_view[ 2 ]{ };
	} s_ev{ };

	/* ---- minimap texture (sub_159275): game thread traces, render thread uploads ---- */
	struct mapgen_t {
		std::string m_map{ };
		int m_state = 0; // 0 idle, 1 tracing, 2 pixels ready, 3 uploaded, 4 failed
		int m_row   = 0;
		float m_min[ 3 ]{ }, m_max[ 3 ]{ }, m_step = 0.f;
		std::vector< std::uint32_t > m_px{ }; // 512 x 512, file row order (row 0 = world max y), bgra
	} s_gen{ };
	IDirect3DTexture9* s_map_texture = nullptr;
	std::string s_map_texture_name{ };

	std::string map_cache( const std::string& map ) { return std::string( k_cache_dir ) + "maps\\" + map + ".bspTGA"; }

	// 18 byte header as the qvm writes it: type 2, 512 x 512, 32 bpp, descriptor 3 (bottom-left origin)
	void write_tga( const std::string& path, const std::vector< std::uint32_t >& px )
	{
		std::error_code ec{ };
		std::filesystem::create_directories( std::filesystem::path( path ).parent_path( ), ec );
		std::ofstream f( path, std::ios::binary );
		std::uint8_t h[ 18 ]{ };
		h[ 2 ] = 2, h[ 13 ] = 2, h[ 15 ] = 2, h[ 16 ] = 32, h[ 17 ] = 3;
		f.write( reinterpret_cast< const char* >( h ), 18 );
		f.write( reinterpret_cast< const char* >( px.data( ) ), static_cast< std::streamsize >( px.size( ) * 4 ) );
		static constexpr char k_footer[ 26 ] = { 0, 0, 0, 0, 0, 0, 0, 0, 'T', 'R', 'U', 'E', 'V', 'I', 'S', 'I', 'O', 'N', '-', 'X', 'F', 'I', 'L', 'E', '.', 0 };
		f.write( k_footer, 26 );
	}

	bool read_tga( const std::string& path, std::vector< std::uint32_t >& px )
	{
		std::vector< std::uint8_t > b{ };
		if ( !read_bytes( path, b ) || b.size( ) < 18 + 512 * 512 * 4 || b[ 2 ] != 2 || b[ 16 ] != 32 )
			return false;
		px.resize( 512 * 512 );
		std::memcpy( px.data( ), b.data( ) + 18, px.size( ) * 4 );
		return true;
	}

	// CM_BoxTrace with zero extents, CONTENTS_SOLID
	bool trace_down( const c_vector& from, const c_vector& to, c_game_trace& tr )
	{
		c_trace_filter_world filter{ };
		g_interfaces.m_engine_trace->trace_ray( ray_t( from, to ), contents_solid, &filter, &tr );
		return true;
	}

	// a few rows per frame (4 ms budget), cached on disk like urt's maps/<map>.bspTGA
	void mapgen_step( const snapshot_t& s, std::mutex& lock )
	{
		if ( !s.m_have_world || s.m_map.empty( ) )
			return;
		if ( s_gen.m_map != s.m_map ) {
			std::lock_guard< std::mutex > guard( lock );
			s_gen       = { };
			s_gen.m_map = s.m_map;
			std::memcpy( s_gen.m_min, s.m_world_min, sizeof( s_gen.m_min ) );
			std::memcpy( s_gen.m_max, s.m_world_max, sizeof( s_gen.m_max ) );
			s_gen.m_step = std::max( s_gen.m_max[ 0 ] - s_gen.m_min[ 0 ], s_gen.m_max[ 1 ] - s_gen.m_min[ 1 ] ) * 0.001953f;
			if ( read_tga( map_cache( s.m_map ), s_gen.m_px ) ) {
				s_gen.m_state = 2;
				return;
			}
			s_gen.m_px.assign( 512 * 512, 0u );
			s_gen.m_state = s_gen.m_step > 0.f ? 1 : 4;
		}
		if ( s_gen.m_state != 1 )
			return;

		const auto begin  = std::chrono::steady_clock::now( );
		const float span  = s_gen.m_max[ 2 ] - s_gen.m_min[ 2 ];
		while ( s_gen.m_row < 512 ) {
			const int r   = s_gen.m_row;
			const float y = s_gen.m_max[ 1 ] - static_cast< float >( r ) * s_gen.m_step;
			if ( y > s_gen.m_min[ 1 ] ) {
				for ( int c = 0; c < 512; ++c ) {
					const float x = s_gen.m_max[ 0 ] - static_cast< float >( c ) * s_gen.m_step;
					if ( x <= s_gen.m_min[ 0 ] )
						break;
					c_game_trace tr{ };
					trace_down( c_vector( x, y, s_gen.m_max[ 2 ] ), c_vector( x, y, s_gen.m_min[ 2 ] ), tr );
					c_vector p = tr.m_end;
					p.m_z -= 20.f;
					while ( p.m_z > s_gen.m_min[ 2 ] && ( g_interfaces.m_engine_trace->get_point_contents_world_only( p, contents_solid ) & contents_solid ) )
						p.m_z -= 20.f;
					c_game_trace floor{ };
					trace_down( p, c_vector( x, y, s_gen.m_min[ 2 ] ), floor );
					if ( floor.m_start_solid || floor.m_fraction == 1.f || span <= 0.f )
						continue;
					s_gen.m_px[ static_cast< size_t >( r ) * 512 + c ] = map_pixel( floor.m_end.m_z, s_gen.m_min[ 2 ], span );
				}
			}
			++s_gen.m_row;
			if ( std::chrono::steady_clock::now( ) - begin > std::chrono::milliseconds( 4 ) )
				break;
		}
		if ( s_gen.m_row >= 512 ) {
			write_tga( map_cache( s_gen.m_map ), s_gen.m_px );
			g_console.print( ( "URT: Minimap Tga for maps/" + s_gen.m_map + ".bspTGA written." ).c_str( ) );
			std::lock_guard< std::mutex > guard( lock );
			s_gen.m_state = 2;
		}
	}

	/* ---- render thread state ---- */
	ring_t s_chat{ }, s_msg{ };
	struct center_t {
		std::string m_text{ };
		int m_time = 0, m_y = 120;
	} s_center{ };
	int s_lag_frame[ 128 ]{ }, s_lag_frame_n = 0;
	int s_lag_snap[ 128 ]{ }, s_lag_snap_flags[ 128 ]{ }, s_lag_snap_n = 0;
	bool s_hit[ 15 ]{ };
	bool s_limp = false;
	int s_weapon_switch = 0;
	int s_frame_times[ 4 ]{ }, s_frame_index = 0, s_last_frame = 0;
	int s_speed_max = 0, s_speed_last = 0, s_speed_time = 0, s_accel = 0; // sub_54620
	snapshot_t s_s{ };
	int s_now = 0;
	float s_layout_w = 640.f, s_layout_h = 480.f; // layout px (pre-stretch width)
	std::atomic< bool > s_map_flip{ false };
	std::string s_chat_line{ };

	float* team_color( float out[ 4 ], const float in[ 4 ] )
	{
		std::memcpy( out, in, 16 );
		return out;
	}

	const skin_t& skin_row( int team ) { return k_skins[ skin( team, s_s.m_team ) ]; }

	// the flag icon table is per skin, the team rgb (row -44 = the previous row's tail) is the plain team color
	void team_rgb( int team, float out[ 4 ] )
	{
		const int k = skin( team, s_s.m_team );
		out[ 0 ] = k == 1 ? 0.f : k == 2 ? 1.f : 0.f;
		out[ 1 ] = k == 0 ? 1.f : 0.f;
		out[ 2 ] = k == 1 ? 1.f : 0.f;
		out[ 3 ] = 1.f;
		if ( k > 2 )
			std::memcpy( out, k_skins[ k ].m_c[ 2 ], 12 );
	}

	void text( float x, float y, float scale, const float* color, const std::string& t, int style, float adjust = 0.f, int limit = 0 )
	{
		text_paint( x, y, scale, color, t.c_str( ), adjust, limit, style, s_cv );
	}
	int width( const std::string& t, float scale ) { return text_width( t.c_str( ), scale, 0, s_cv ); }
	int height( const std::string& t, float scale ) { return text_height( t.c_str( ), scale, 0, s_cv ); }

	const player_t* player( int index )
	{
		for ( const player_t& p : s_s.m_players )
			if ( p.m_index == index )
				return &p;
		return nullptr;
	}

	/* ---- CG_DrawChat (sub_26171) / CG_DrawMessage (sub_26510) ---- */
	void draw_chat( )
	{
		float bg[ 4 ] = { 0.f, 0.f, 0.f, 0.f };
		const int mode = s_cv.i( "cg_chatBG" );
		if ( mode == 1 )
			bg[ 3 ] = 0.2f;
		else if ( mode == 2 )
			bg[ 3 ] = 0.4f;
		else if ( mode == 3 ) {
			const int speed = std::min( static_cast< int >( std::sqrt( s_s.m_velocity[ 0 ] * s_s.m_velocity[ 0 ] + s_s.m_velocity[ 1 ] * s_s.m_velocity[ 1 ] ) ), 350 );
			bg[ 3 ] = static_cast< float >( 350 - speed ) * 0.002857f * 0.3f;
		}
		const int slots = std::min( s_cv.i( "cg_chatHeight" ), 8 );
		if ( slots <= 0 )
			return;
		const float px    = std::clamp( s_cv.f( "cg_chatSize" ), 8.f, 16.f );
		const int line_h  = static_cast< int >( px * 1.2f );
		const float scale = px * 0.020833f;
		if ( s_chat.m_head == s_chat.m_tail )
			return;
		if ( s_now - s_chat.m_time[ s_chat.m_head % slots ] > s_cv.i( "cg_chatTime" ) )
			++s_chat.m_head;
		const int count  = s_chat.m_tail - s_chat.m_head;
		const int bottom = 410 - ( s_cv.f( "cg_chatOrder" ) <= 1.f ? slots : count ) * line_h;
		float widest     = 0.f;
		for ( int i = s_chat.m_head; i < s_chat.m_tail; ++i )
			widest = std::max( widest, static_cast< float >( width( s_chat.m_text[ i % slots ], scale ) ) );
		fill_rect( 0.f, static_cast< float >( bottom - line_h ), widest + static_cast< float >( width( "OO", scale ) ),
		           ( static_cast< float >( count ) + 0.5f ) * static_cast< float >( line_h ), bg );
		for ( int i = s_chat.m_head; i < s_chat.m_tail; ++i )
			text( 5.f, static_cast< float >( bottom + ( i - s_chat.m_head ) * line_h ), scale, k_white, s_chat.m_text[ i % slots ], style_shadowed );

		// typed line (EXE_SAY), one row under the box
		if ( !s_chat_line.empty( ) )
			text( 5.f, static_cast< float >( bottom + count * line_h ), scale, k_white, s_chat_line, style_shadowed );
	}

	void draw_messages( )
	{
		const int slots = std::min( s_cv.i( "cg_msgHeight" ), 8 );
		if ( slots <= 0 || s_msg.m_head == s_msg.m_tail )
			return;
		const float px   = std::clamp( s_cv.f( "cg_msgSize" ), 8.f, 16.f );
		const int line_h = static_cast< int >( px * 1.2f );
		if ( s_now - s_msg.m_time[ s_msg.m_head % slots ] > s_cv.i( "cg_msgTime" ) )
			++s_msg.m_head;
		for ( int i = s_msg.m_head; i < s_msg.m_tail; ++i )
			text( 5.f, static_cast< float >( line_h + ( i - s_msg.m_head ) * line_h ), px * 0.020833f, k_white, s_msg.m_text[ i % slots ], style_shadowed );
	}

	/* ---- CG_DrawCenterString (sub_27701) ---- */
	void draw_center( )
	{
		if ( !s_center.m_time )
			return;
		const float* c = fade_color( s_center.m_time, static_cast< int >( s_cv.f( "cg_centertime" ) * 1000.f ), s_now );
		if ( !c ) {
			s_center.m_time = 0;
			return;
		}
		int lines = 1;
		for ( char ch : s_center.m_text )
			lines += ch == '\n';
		float y = static_cast< float >( s_center.m_y - ( lines << 4 ) / 2 );
		size_t at = 0;
		while ( at <= s_center.m_text.size( ) ) {
			size_t e = s_center.m_text.find( '\n', at );
			if ( e == std::string::npos )
				e = s_center.m_text.size( );
			const std::string line = s_center.m_text.substr( at, std::min< size_t >( e - at, 100 ) );
			const int w = width( line, 0.35f ), h = height( line, 0.35f );
			text( static_cast< float >( ( 640 - w ) / 2 ), y + static_cast< float >( h ), 0.35f, c, line, style_shadowed );
			y += static_cast< float >( h + 6 );
			at = e + 1;
		}
	}

	/* ---- CG_DrawCrosshair (sub_30085 + styles) ---- */
	bool friendly_target( )
	{
		return s_s.m_target > 0 && s_s.m_target_team == s_s.m_subject_team && ( s_s.m_target_team == 1 || s_s.m_target_team == 2 );
	}

	void draw_crosshair( )
	{
		const int style = s_cv.i( "cg_drawCrosshair" );
		if ( style == 0 || s_s.m_weapon == wp_bomb || ( k_weapons[ s_s.m_weapon ].m_flags & 2 ) )
			return;
		float color[ 4 ];
		s_cv.rgba( friendly_target( ) ? "cg_crosshairFriendRGB" : "cg_crosshairRGB", color );
		set_color( color );
		const float sx = g_draw.m_sx, sy = g_draw.m_sy;
		const float speed = std::sqrt( s_s.m_velocity[ 0 ] * s_s.m_velocity[ 0 ] + s_s.m_velocity[ 1 ] * s_s.m_velocity[ 1 ] + s_s.m_velocity[ 2 ] * s_s.m_velocity[ 2 ] );

		if ( style < 9 ) {
			const float w = s_cv.f( "cg_crosshairSize" ) * sx, h = s_cv.f( "cg_crosshairSize" ) * sy;
			const float x = static_cast< float >( s_cv.i( "cg_crosshairX" ) ) * sx + 1.f, y = static_cast< float >( s_cv.i( "cg_crosshairY" ) ) * sy + 1.f;
			const crosshair_t& ch = k_crosshairs[ std::clamp( style, 1, static_cast< int >( std::size( k_crosshairs ) ) ) - 1 ];
			const accuracy_t& acc = k_weapons[ s_s.m_weapon ].m_modes[ std::clamp( s_s.m_mode, 0, 2 ) ];
			const float spread    = dynamic_spread( acc.m_base, acc.m_move, acc.m_kick, acc.m_flags, s_s.m_pulse, s_s.m_ducked, s_s.m_scoped, speed );
			const float cx = x + ( s_layout_w - w ) * 0.5f, cy = y + ( s_layout_h - h ) * 0.5f;
			const auto part = [ & ]( int k, float px, float py ) {
				if ( ch.m_parts[ k ] >= 0 )
					stretch_pic( px, py, w, h, 0.f, 0.f, 1.f, 1.f, ch.m_parts[ k ] );
			};
			part( 0, cx, cy );
			part( 1, cx + w * 0.5f + spread * sx, cy );
			part( 2, cx - w * 0.5f - spread * sx, cy );
			part( 3, cx, cy - h * 0.5f - spread * sy );
			part( 4, cx, cy + h * 0.5f + spread * sy );
		}
		else if ( style == 9 ) {
			const float size = static_cast< float >( std::clamp( s_cv.i( "cg_crosshairSize" ), 1, 50 ) );
			const float gap  = ring_gap( !s_s.m_on_ground, speed, s_s.m_pulse );
			fill_rect( 320.f - size - gap, 240.f, size, 1.f, color );
			fill_rect( 321.f + gap, 240.f, size, 1.f, color );
			fill_rect( 320.f, 240.f - size - gap, 1.f, size, color );
			fill_rect( 320.f, 241.f + gap, 1.f, size, color );
		}
		else if ( style <= 12 ) {
			const gaps_t g   = cross_gaps( s_s.m_weapon, s_s.m_pulse );
			const float size = std::min( static_cast< float >( std::clamp( s_cv.i( "cg_crosshairSize" ), 1, 50 ) ) + g.m_size_add, 50.f );
			if ( style == 11 || style == 12 )
				fill_rect( 320.f, 240.f, 1.f, 1.f, color );
			fill_rect( 320.f - size - g.m_side, 240.f, size, 1.f, color );
			fill_rect( 321.f + g.m_side, 240.f, size, 1.f, color );
			if ( style != 12 )
				fill_rect( 320.f, 240.f - size - g.m_top, 1.f, size, color );
			fill_rect( 320.f, 241.f + g.m_bottom, 1.f, size, color );
		}
		else if ( style == 13 )
			fill_rect( 320.f, 240.f, 1.f, 1.f, color );
		else {
			int size = std::clamp( s_cv.i( "cg_crosshairSize" ), 5, 60 );
			if ( size % 2 )
				++size;
			stretch_pic( s_layout_w * 0.5f, s_layout_h * 0.5f - static_cast< float >( size / 2 ) + 1.f, 2.f, static_cast< float >( size ), 0, 0, 0, 0, -1 );
			stretch_pic( s_layout_w * 0.5f - static_cast< float >( size / 2 ) + 1.f, s_layout_h * 0.5f, static_cast< float >( size ), 2.f, 0, 0, 0, 0, -1 );
		}
		set_color( nullptr );
	}

	/* ---- CG_DrawCrosshairNames (sub_31935 / 30651 / 31094 / 30330) ---- */
	int s_name_client = 0, s_name_time = 0;

	void draw_crosshair_names( )
	{
		if ( s_s.m_target > 0 ) {
			s_name_client = s_s.m_target;
			s_name_time   = s_now;
		}
		if ( !s_cv.i( "cg_drawCrosshair" ) || !s_cv.i( "cg_drawCrosshairNames" ) || !s_name_client || s_name_client == s_s.m_subject || !s_name_time ||
		     s_now - s_name_time > 1000 )
			return;
		const player_t* p = player( s_name_client );
		if ( !p || p->m_team != s_s.m_subject_team || p->m_name.empty( ) )
			return;
		const float dx = p->m_pos[ 0 ] - s_s.m_eye[ 0 ], dy = p->m_pos[ 1 ] - s_s.m_eye[ 1 ], dz = p->m_pos[ 2 ] - s_s.m_eye[ 2 ];
		const float cp = std::cos( s_s.m_view[ 0 ] * 0.017453f ), yaw = s_s.m_view[ 1 ] * 0.017453f;
		if ( cp * std::cos( yaw ) * dx + cp * std::sin( yaw ) * dy - std::sin( s_s.m_view[ 0 ] * 0.017453f ) * dz <= 0.f )
			return;

		const int health = p->m_index == s_s.m_target ? s_s.m_target_health : 100;
		const int type   = s_cv.i( "cg_crosshairNamesType" );
		if ( type == 0 || type > 3 ) {
			const float* c = fade_color( s_name_time, 1000, s_now );
			if ( !c )
				return;
			float col[ 4 ];
			team_color( col, c )[ 3 ] = 1.f;
			const char* status = health < 50 ? "needs medic" : health < 25 ? "near death" : health < 50 ? "badly wounded" : health < 75 ? "wounded" : "healthy";
			const std::string name = colored( p->m_name, p->m_team, s_s.m_team );
			text( 42.f, 474.f, 0.2f, col, p->m_c4 ? va( "%s%s (BOMB): %s", name.c_str( ), "^7", status ) : va( "%s%s: %s", name.c_str( ), "^7", status ), style_shadowed );
			return;
		}

		const float fade = 1.f - static_cast< float >( s_now - s_name_time ) * 0.001f;
		const float white[ 4 ] = { 1.f, 1.f, 1.f, fade }, black[ 4 ] = { 0.f, 0.f, 0.f, fade };
		const float bar[ 4 ] = { health < 50 ? 1.f : health < 75 ? 1.f : 0.f, health < 50 ? 0.f : 1.f, 0.f, fade };
		const std::string label = p->m_c4 ? va( "%s (BOMB)", p->m_name.c_str( ) ) : p->m_name;
		const float scale       = std::clamp( s_cv.f( "cg_crosshairNamesSize" ), 0.f, 1.f );
		float ax = 320.f, ay = type == 3 ? 180.f : 300.f, bar_h = 0.f;
		if ( type == 1 ) {
			// 3d: above the head, scaled by distance bucket
			const float dist = std::sqrt( dx * dx + dy * dy + dz * dz );
			const float lift = dist > 600.f ? dist * 0.001667f : dist > 100.f ? 1.f : dist > 46.f ? dist * 0.01f : 0.46f;
			if ( lift != 0.46f ) {
				const float sp = std::sin( s_s.m_view[ 0 ] * 0.017453f ), cy = std::cos( yaw ), sy = std::sin( yaw );
				const float wz = dz + lift * 64.f;
				const float fwd = cp * cy * dx + cp * sy * dy - sp * wz;
				const float right = sy * dx - cy * dy;
				const float up = sp * cy * dx + sp * sy * dy + cp * wz;
				if ( fwd <= 0.f )
					return;
				const float tx = std::tan( s_s.m_fov * 0.5f * 0.0174533f );
				const float ty = tx * 0.75f;
				ax = 320.f / tx * right / fwd + 320.f;
				ay = -( 240.f / ty ) * up / fwd + 240.f;
			}
			else
				ay = 300.f;
			bar_h = 13.f;
		}
		const float tw = static_cast< float >( width( label, scale ) );
		const float th = std::max( static_cast< float >( height( label, scale ) ), 8.f );
		text( ax - tw * 0.5f, ay, scale, white, label, style_shadowed );
		const float bw = std::max( tw, 75.f );
		const float bh = bar_h > 0.f ? bar_h * 0.5f : th * 0.5f;
		fill_rect( ax - bw * 0.5f, ay + bh, bw, bh, black );
		if ( health > 0 )
			fill_rect( ax - bw * 0.5f + 1.f, ay + bh + 1.f, bw * static_cast< float >( health ) * 0.01f - 2.f, bh - 2.f, bar );
	}

	/* ---- CG_DrawZoomMask (sub_40667): sniper scope ---- */
	bool scoped_weapon( int id ) { return id == wp_g36 || id == wp_psg1 || id == wp_sr8 || id == wp_tod50 || id == wp_frf1; }

	void draw_scope( )
	{
		const int id = s_s.m_weapon;
		int style    = 0;
		if ( id == wp_g36 )
			style = s_cv.i( "cg_scopeG36" );
		else if ( id == wp_psg1 )
			style = s_cv.i( "cg_scopePSG" );
		else if ( id == wp_sr8 || id == wp_tod50 )
			style = s_cv.i( "cg_scopeSR8" );
		else if ( id == wp_frf1 )
			style = s_cv.i( "cg_scopeFRF1" );
		else
			return;
		const float sw = s_layout_w, sh = s_layout_h, sx = g_draw.m_sx, sy = g_draw.m_sy;
		set_color( nullptr );
		stretch_pic( 0.f, 0.f, sw, sh, 0.f, 0.f, 1.f, 1.f, img( "gfx/crosshairs/scopes/scope" ) );

		float tint[ 4 ], base[ 4 ];
		s_cv.rgba( "cg_scopeRGB", base );
		s_cv.rgba( friendly_target( ) ? "cg_scopeFriendRGB" : "cg_scopeRGB", tint );
		set_color( tint );
		const float cx = sw * 0.5f, cy = sh * 0.5f;
		const auto r = [ & ]( float x, float y, float w, float h ) { stretch_pic( x, y, w, h, 0, 0, 0, 0, -1 ); };
		switch ( std::clamp( style, 0, 4 ) ) {
		case 4:
			r( cx, cy - 250.f * sy, sx, 185.f * sy );
			r( cx, cy + 65.f * sy, sx, 185.f * sy );
			r( cx - 250.f * sx, cy, 185.f * sx, sy );
			r( cx + 65.f * sx, cy, 185.f * sx, sy );
			r( cx - 1.f, cy - 1.f, 3.f, 3.f );
			break;
		case 3: r( cx - 1.f, cy - 1.f, 3.f, 3.f ); break;
		case 2:
			r( cx, cy, sx, 235.f * sy );
			r( cx - 190.f * sx, cy - 2.f * sx, 174.f * sx, 3.f * sy );
			r( cx - 235.f * sx, cy - 2.f * sx, 45.f * sx, 6.f * sy );
			r( cx + 16.f * sx, cy - 2.f * sx, 174.f * sx, 3.f * sy );
			r( cx + 190.f * sx, cy - 2.f * sy, 46.f * sx, 6.f * sy );
			break;
		case 1:
			r( cx, cy - 130.f * sy, 1.f, 260.f * sy );
			r( cx - 130.f * sx, cy, 260.f * sx, 1.f );
			set_color( base );
			r( cx - 2.f * sx, cy - 205.f * sy, 3.f * sx, 75.f * sy );
			r( cx - 5.f * sx, cy - 236.f * sy, 6.f * sx, 31.f * sy );
			r( cx, cy + 130.f * sy, 3.f * sx, 75.f * sy );
			r( cx, cy + 205.f * sy, 6.f * sx, 30.f * sy );
			r( cx - 205.f * sx, cy, 75.f * sx, 3.f * sy );
			r( cx - 235.f * sx, cy, 30.f * sx, 6.f * sy );
			r( cx + 130.f * sx, cy - 2.f * sx, 75.f * sx, 3.f * sy );
			r( cx + 205.f * sx, cy - 5.f * sy, 31.f * sx, 6.f * sy );
			break;
		default: {
			static constexpr float k_black[ 4 ] = { 0.f, 0.f, 0.f, 1.f };
			r( cx, cy - 100.f * sy, sx, 200.f * sy );
			r( cx - 100.f * sx, cy, 200.f * sx, sy );
			set_color( k_black );
			r( cx - 4.f * sx, cy - 236.f * sy, 5.f * sx, 136.f * sy );
			r( cx, cy + 100.f * sy, 5.f * sx, 135.f * sy );
			r( cx - 235.f * sx, cy, 135.f * sx, 5.f * sy );
			r( cx + 100.f * sx, cy - 4.f * sy, 136.f * sx, 5.f * sy );
			break;
		}
		}

		// breathing scopering, sized by speed
		const int ring = img( "gfx/crosshairs/scopes/scopering" );
		float radius   = 0.f;
		if ( s_cv.i( "cg_scopering" ) && !s_cv.i( "cg_instagib" ) && ( s_cv.i( "cg_scopering" ) == 2 || id == wp_sr8 ) && id == wp_sr8 && !s_s.m_on_ground ) {
			radius = 34.f;
			stretch_pic( cx - radius, cy - radius, radius * 2.f + 1.f, radius * 2.f + 1.f, 0, 0, 1, 1, ring );
			radius = 340.f;
		}
		if ( radius == 0.f ) {
			const float* v = s_s.m_velocity;
			radius         = std::min( std::sqrt( v[ 0 ] * v[ 0 ] + v[ 1 ] * v[ 1 ] + v[ 2 ] * v[ 2 ] ), 340.f );
		}
		radius *= 0.2f;
		stretch_pic( cx - radius, cy - radius, radius * 2.f + 1.f, radius * 2.f + 1.f, 0, 0, 1, 1, ring );
		set_color( nullptr );
	}

	/* ---- minimap (sub_159893) ---- */
	void upload_map( )
	{
		if ( s_gen.m_state != 2 || s_map_texture_name == s_gen.m_map )
			return;
		if ( s_map_texture )
			s_map_texture->Release( ), s_map_texture = nullptr;
		s_map_texture_name = s_gen.m_map;
		IDirect3DDevice9* const device = g_interfaces.m_direct_device;
		if ( !device )
			return;
		if ( FAILED( device->CreateTexture( 512, 512, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &s_map_texture, nullptr ) ) &&
		     FAILED( device->CreateTexture( 512, 512, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &s_map_texture, nullptr ) ) ) {
			s_map_texture = nullptr;
			return;
		}
		D3DLOCKED_RECT rect{ };
		if ( FAILED( s_map_texture->LockRect( 0, &rect, nullptr, 0 ) ) )
			return;
		// bottom-left origin: file row f = image row 511 - f
		for ( int f = 0; f < 512; ++f )
			std::memcpy( static_cast< std::uint8_t* >( rect.pBits ) + static_cast< size_t >( 511 - f ) * rect.Pitch, s_gen.m_px.data( ) + static_cast< size_t >( f ) * 512, 2048 );
		s_map_texture->UnlockRect( 0 );
	}

	void draw_minimap( )
	{
		if ( !s_cv.i( "cg_maptoggle" ) || !s_s.m_have_world )
			return;
		const int size = std::min( s_cv.i( "cg_mapsize" ), 512 );
		if ( size <= 0 )
			return;
		const float* mn = s_s.m_world_min;
		const float* mx = s_s.m_world_max;
		const float step  = std::max( mx[ 0 ] - mn[ 0 ], mx[ 1 ] - mn[ 1 ] ) * 0.001953f;
		const float scale = step * 512.f / static_cast< float >( size );
		float l212 = 0.f, l224 = 0.f;
		const float fs = static_cast< float >( size );
		if ( mx[ 0 ] - mn[ 0 ] > mx[ 1 ] - mn[ 1 ] )
			l224 = ( mx[ 1 ] - mn[ 1 ] ) / ( mx[ 0 ] - mn[ 0 ] ) * fs - fs;
		else
			l212 = ( mx[ 0 ] - mn[ 0 ] ) / ( mx[ 1 ] - mn[ 1 ] ) * fs - fs;
		int bx = 0, by = 0;
		if ( !map_box( s_cv.i( "cg_mappos" ), size, l212, l224, bx, by ) )
			return;
		const float sx = g_draw.m_sx, sy = g_draw.m_sy;
		const float alpha = std::clamp( s_cv.f( "cg_mapalpha" ), 0.f, 1.f );
		float c[ 4 ] = { 1.f, 1.f, 1.f, alpha };
		set_color( c );
		if ( s_map_texture ) {
			set_blend( 0 );
			g_draw.m_list->AddImageQuad( reinterpret_cast< ImTextureID >( s_map_texture ), ImVec2( static_cast< float >( static_cast< int >( bx * sx ) ), static_cast< float >( static_cast< int >( by * sy ) ) ),
			                             ImVec2( static_cast< float >( static_cast< int >( bx * sx ) + static_cast< int >( size * sx ) ), static_cast< float >( static_cast< int >( by * sy ) ) ),
			                             ImVec2( static_cast< float >( static_cast< int >( bx * sx ) + static_cast< int >( size * sx ) ), static_cast< float >( static_cast< int >( by * sy ) + static_cast< int >( size * sy ) ) ),
			                             ImVec2( static_cast< float >( static_cast< int >( bx * sx ) ), static_cast< float >( static_cast< int >( by * sy ) + static_cast< int >( size * sy ) ) ),
			                             ImVec2( 1, 1 ), ImVec2( 0, 1 ), ImVec2( 0, 0 ), ImVec2( 1, 0 ), col( c ) );
		}
		const auto to_screen = [ & ]( float wx, float wy, float& px, float& py ) {
			px = ( fs + ( static_cast< float >( bx ) + ( wx - mx[ 0 ] ) / scale ) ) * sx;
			py = ( static_cast< float >( by ) - ( wy - mx[ 1 ] ) / scale ) * sy;
		};
		const int bomb = img( "minimap_bomb" ), arrow = img( "MiniMapArrow" );

		// bomb spots, pulsing (red = not planted there)
		if ( s_s.m_have_sites ) {
			const int pulse = static_cast< int >( std::cos( static_cast< float >( s_now ) * 0.003333f ) * 32.f );
			for ( int i = 0; i < 2; ++i ) {
				float px, py;
				to_screen( s_s.m_sites[ i ][ 0 ], s_s.m_sites[ i ][ 1 ], px, py );
				static constexpr float k_red[ 4 ] = { 1.f, 0.f, 0.f, 1.f };
				set_color( k_red );
				stretch_pic( static_cast< float >( static_cast< int >( px - static_cast< float >( pulse >> 1 ) ) ), static_cast< float >( static_cast< int >( py - static_cast< float >( pulse >> 1 ) ) ),
				             static_cast< float >( pulse ), static_cast< float >( pulse ), 0, 0, 1, 1, bomb );
			}
		}

		const float zspan = mx[ 2 ] - mn[ 2 ];
		const auto draw_arrow = [ & ]( const float pos[ 3 ], float yaw, float floor_size, bool local, int team, float a ) {
			const int n   = arrow_cell( yaw );
			const float s = static_cast< float >( n % 8 ), t = static_cast< float >( n / 8 );
			float px, py;
			to_screen( pos[ 0 ], pos[ 1 ], px, py );
			const float hz = zspan > 0.f ? ( pos[ 2 ] - mn[ 2 ] ) / zspan : 0.f;
			const float r  = std::max( fs * ( hz + 1.f ) * 0.025f * s_cv.f( "cg_maparrowscale" ), floor_size );
			if ( local ) {
				const float p = ( std::cos( static_cast< float >( s_now ) * 0.006667f ) + 1.f ) * 0.5f;
				const float ring[ 4 ] = { p, p, p, 1.f };
				set_color( ring );
				stretch_pic( px - r * 1.3f, py - r * 1.3f, r * 2.6f, r * 2.6f, s * 0.125f, t * 0.125f + 0.125f, s * 0.125f + 0.125f, t * 0.125f, arrow );
			}
			float col[ 4 ];
			team_rgb( team, col );
			col[ 3 ] = a;
			set_color( col );
			stretch_pic( px - r, py - r, r * 2.f, r * 2.f, s * 0.125f, t * 0.125f + 0.125f, s * 0.125f + 0.125f, t * 0.125f, arrow );
		};

		// teammates (everyone while free spectating), local drawn last with the live view
		for ( const player_t& p : s_s.m_players ) {
			if ( !p.m_alive || p.m_dormant || p.m_team == 3 || p.m_index == s_s.m_subject )
				continue;
			if ( !s_s.m_free_spec && p.m_team != s_s.m_subject_team )
				continue;
			draw_arrow( p.m_pos, p.m_yaw, 10.f, false, p.m_team, alpha );
		}
		if ( s_s.m_alive && !s_s.m_free_spec )
			draw_arrow( s_s.m_origin, s_s.m_yaw, 9.f, true, s_s.m_subject_team, 1.f );

		// bomb (dropped / planted): attackers always, defenders once planted
		if ( s_s.m_bomb_seen && ( s_s.m_team != 2 || s_s.m_bomb_planted ) ) {
			float px, py;
			to_screen( s_s.m_bomb_pos[ 0 ], s_s.m_bomb_pos[ 1 ], px, py );
			set_color( k_color_table[ 0 ] );
			stretch_pic( px - 11.f, py - 11.f, 22.f, 22.f, 0, 0, 1, 1, bomb );
			const float y[ 4 ] = { 1.f, 1.f, 0.f, ( std::cos( static_cast< float >( s_now ) * 0.02f ) + 1.f ) * 0.5f };
			set_color( y );
			stretch_pic( px - 10.f, py - 10.f, 20.f, 20.f, 0, 0, 1, 1, bomb );
		}
		set_color( nullptr );
	}

	/* ---- status bar: health figure (sub_55591), weapon (sub_53264), speedo + fire mode (sub_55089 / 54620) ---- */
	void draw_health( )
	{
		if ( s_s.m_health <= 0 || !s_s.m_alive )
			return;
		const float rx = 5.f, ry = 385.f, rh = 90.f;
		const skin_t& row = skin_row( s_s.m_subject_team );
		float body[ 4 ], dark[ 4 ], grey[ 4 ];
		team_color( body, row.m_c[ 5 ] );
		team_color( grey, row.m_c[ 6 ] );
		team_color( dark, row.m_c[ 7 ] );
		float stam = s_s.m_stamina * static_cast< float >( s_s.m_health ) * 0.09f;
		float hp   = static_cast< float >( s_s.m_health ) * 0.09f;

		const bool numbers = s_cv.i( "cg_drawhealth" ) != 0;
		const int outline  = img( numbers ? "gfx/hud/outline.tga" : "gfx/hud/outline_o.tga" );
		const float w      = numbers ? 90.f : static_cast< float >( static_cast< int >( 90.f * 0.5f ) );
		if ( numbers ) {
			const std::string n = va( "%i", s_s.m_health & 255 );
			text( rx + 35.f - static_cast< float >( width( n, 0.25f ) / 2 ), ry + 50.f, 0.25f, body, n, style_shadowed_light );
		}
		set_color( k_color_table[ 0 ] );
		draw_pic( rx + 1.f, ry + 1.f, w, rh, outline );
		set_color( nullptr );
		draw_pic( rx, ry, w, rh, outline );
		for ( int i = 0; i < 9; ++i ) {
			const float s = std::max( stam - static_cast< float >( 8 - i ), 0.f );
			const float h = std::max( hp - static_cast< float >( 8 - i ), 0.f );
			const int seg = img( va( numbers ? "gfx/hud/stamina%i.tga" : "gfx/hud/stamina%i_o.tga", i + 1 ) );
			set_color( k_color_table[ 0 ] );
			draw_pic( rx + 1.f, ry + 1.f, w, rh, seg );
			float mix[ 4 ];
			for ( int k = 0; k < 4; ++k )
				mix[ k ] = dark[ k ] + ( grey[ k ] - dark[ k ] ) * h;
			if ( s >= 1.f ) {
				set_color( body );
				draw_pic( rx, ry, w, rh, seg );
			}
			else if ( s <= 0.f ) {
				set_color( mix );
				draw_pic( rx, ry, w, rh, seg );
			}
			else {
				set_color( mix );
				draw_pic( rx, ry, w, rh, seg );
				body[ 3 ] = s;
				set_color( body );
				draw_pic( rx, ry, w, rh, seg );
				body[ 3 ] = 1.f;
			}
			stam -= s;
			hp -= h;
		}
		// sub_55384: hit locations in the dark color, legs = limping
		set_color( dark );
		static constexpr const char* k_parts[ 15 ] = { nullptr,
			                                           "gfx/hud/damage/head.tga",       "gfx/hud/damage/head.tga",      "gfx/hud/damage/all_chest.tga",
			                                           "gfx/hud/damage/all_chest.tga",  "gfx/hud/damage/right_arm.tga", "gfx/hud/damage/left_arm.tga",
			                                           "gfx/hud/damage/waist.tga",      "gfx/hud/damage/waist.tga",     "gfx/hud/damage/right_thigh.tga",
			                                           "gfx/hud/damage/left_thigh.tga", "gfx/hud/damage/right_calf.tga", "gfx/hud/damage/left_calf.tga",
			                                           "gfx/hud/damage/right_foot.tga", "gfx/hud/damage/left_foot.tga" };
		for ( int i = 1; i < 15; ++i )
			if ( s_hit[ i ] )
				draw_pic( rx, ry, w, rh, img( k_parts[ i ] ) );
		if ( s_limp )
			for ( const char* p : { "gfx/hud/damage/right_calf.tga", "gfx/hud/damage/left_calf.tga", "gfx/hud/damage/right_foot.tga", "gfx/hud/damage/left_foot.tga" } )
				draw_pic( rx, ry, w, rh, img( p ) );
		set_color( nullptr );
	}

	void draw_weapon( )
	{
		if ( !s_s.m_alive || s_s.m_weapon <= 0 || s_s.m_weapon == 16 )
			return;
		const float rx = 565.f, ry = 448.f, rh = 32.f, half = 0.5f;
		static constexpr float k_red[ 4 ] = { 1.f, 0.17f, 0.17f, 1.f }, k_orange[ 4 ] = { 0.88f, 0.31f, 0.13f, 1.f };
		const int clip = s_s.m_clip, max = std::max( s_s.m_clip_max, 1 );
		const bool insta = s_cv.i( "cg_instagib" ) != 0;
		const float* cc  = ( clip <= max / 4 && !insta ) || ( clip == 0 && insta ) ? k_red : clip <= max / 2 && !insta ? k_orange : k_white;
		const int ammo   = img( va( "icons/ammo/%s.tga", weapon_short( s_s.m_weapon ) ) );
		const std::string n = va( "%i", clip ), clips = va( "x%i", s_s.m_clips );
		const char* name    = k_weapons[ s_s.m_weapon ].m_name;
		const int mode      = s_cv.i( "cg_hudWeaponInfo" );
		const auto clip_y   = [ & ]( ) { return ry + rh - 14.f + static_cast< float >( height( n, half ) / 2 ); };
		const auto clips_at = [ & ]( ) { text( rx + 52.f, ry + rh - 24.f + static_cast< float >( height( clips, 0.2f ) ), 0.25f, k_white, clips, style_shadowed ); };
		if ( mode == 0 ) {
			text( rx + 20.f - static_cast< float >( width( n, half ) ), clip_y( ), half, cc, n, style_shadowed );
			draw_pic( rx + 20.f, ry, 32.f, 32.f, ammo );
			clips_at( );
		}
		else if ( mode == 1 ) {
			clips_at( );
			const int x = 50 - width( n, half );
			text( rx + static_cast< float >( x ), clip_y( ), half, cc, n, style_shadowed );
			text( rx + static_cast< float >( x ) - static_cast< float >( width( name, 0.25f ) ) - 4.f, ry + rh - 14.f + static_cast< float >( height( name, 0.25f ) ), 0.25f, k_white, name,
			      style_shadowed );
		}
		else {
			const int x = 20 - width( n, half );
			text( rx + static_cast< float >( x ), clip_y( ), half, cc, n, style_shadowed );
			text( rx + static_cast< float >( x ) - static_cast< float >( width( name, 0.25f ) ) - 6.f, ry + rh - 14.f + static_cast< float >( height( name, 0.25f ) ), 0.25f, k_white, name,
			      style_shadowed );
			draw_pic( rx + 20.f, ry, 32.f, 32.f, ammo );
			clips_at( );
		}
	}

	void draw_speedo_mode( )
	{
		if ( !s_s.m_alive )
			return;
		const float rx = 560.f, ry = 428.f, rw = 70.f, rh = 20.f, scale = 0.2f;
		if ( s_cv.i( "cg_speedo" ) > 0 ) {
			const int speed = static_cast< int >( std::sqrt( s_s.m_velocity[ 0 ] * s_s.m_velocity[ 0 ] + s_s.m_velocity[ 1 ] * s_s.m_velocity[ 1 ] ) );
			s_speed_max     = std::max( s_speed_max, speed );
			if ( s_now - s_speed_time > 40 ) {
				s_accel      = speed - s_speed_last;
				s_speed_last = speed;
				s_speed_time = s_now;
			}
			const char* prefix = "^7";
			float y_ofs        = 0.f;
			const int acc_mode = s_cv.i( "cg_accelero" );
			if ( acc_mode <= 1 ) {
				y_ofs = 11.f;
				if ( acc_mode == 1 )
					prefix = s_accel > 0 ? "^2" : s_accel < 0 ? "^1" : "^7";
			}
			if ( s_cv.i( "cg_hudWeaponInfo" ) == 0 )
				y_ofs += 14.f;
			std::string t{ };
			const int mode = s_cv.i( "cg_speedo" );
			if ( mode == 1 )
				t = va( "speed: %s%i%s (%i) %sups", prefix, speed, "^7", s_speed_max, "^3" );
			else if ( mode == 2 )
				t = va( "speed: %s%.1f%s (%.1f) %skph", prefix, speed * 0.091408, "^7", s_speed_max * 0.091408, "^3" );
			else
				t = va( "speed: %s%.1f%s (%.1f) %smph", prefix, speed * 0.056818, "^7", s_speed_max * 0.056818, "^3" );
			text( rx + rw - static_cast< float >( width( t, 0.2f ) ) - 79.f, ry + rh + y_ofs, scale, k_white, t, style_shadowed );
			if ( acc_mode > 1 ) {
				static constexpr float k_red[ 4 ] = { 1.f, 0.f, 0.f, 1.f }, k_green[ 4 ] = { 0.f, 1.f, 0.f, 1.f }, k_black[ 4 ] = { 0.f, 0.f, 0.f, 1.f };
				const int bar = static_cast< int >( std::clamp( s_cv.f( "cg_accelSize" ), 8.f, 502.f ) );
				fill_rect( rx + rw - static_cast< float >( bar ) - 78.f, ry + rh + 5.f + y_ofs, static_cast< float >( bar ), 7.f, k_black );
				int fill = static_cast< int >( static_cast< float >( bar ) * static_cast< float >( std::min( std::abs( s_accel ), 30 ) ) * 0.033333f );
				if ( fill >= bar )
					fill = bar - 2;
				fill_rect( rx + rw - static_cast< float >( fill ) - 79.f, ry + rh + 6.f + y_ofs, static_cast< float >( fill ), 5.f, s_accel > 0 ? k_green : k_red );
			}
		}
		else
			s_accel = s_speed_max = s_speed_last = 0;

		if ( !s_s.m_mode_name.empty( ) )
			text( rx + rw - static_cast< float >( width( s_s.m_mode_name, 0.2f ) ), ry + rh, scale, k_white, s_s.m_mode_name, style_shadowed );
	}

	/* ---- top right (sub_44369): ping | fps, team scores, timer, team overlay ---- */
	void draw_team_scores( int mode, int y )
	{
		if ( mode )
			y = 28;
		const auto box = [ & ]( int team, float x, float tx, bool mirror, int score ) {
			float c[ 4 ];
			team_color( c, skin_row( team ).m_c[ 3 ] );
			c[ 3 ] *= 0.6f;
			set_color( c );
			const int stripes = img( "gfx/2d/stripes.tga" );
			if ( mirror )
				stretch_pic( x * g_draw.m_sx, static_cast< float >( y ) * g_draw.m_sy, 70.f * g_draw.m_sx, 20.f * g_draw.m_sy, 1, 1, 0, 0, stripes );
			else
				draw_pic( x, static_cast< float >( y ), 70.f, 20.f, stripes );
			const std::string t = va( "%i", score );
			text( tx < 0.f ? 635.f - static_cast< float >( width( t, 0.3f ) ) : tx - static_cast< float >( width( t, 0.3f ) ), static_cast< float >( y + 15 ), 0.3f, k_white, t,
			      style_shadowed_light );
		};
		if ( mode ) {
			box( 1, 251.f, 310.f, false, s_s.m_red_score );
			float c[ 4 ];
			team_color( c, skin_row( 2 ).m_c[ 3 ] );
			c[ 3 ] *= 0.6f;
			set_color( c );
			stretch_pic( 319.f * g_draw.m_sx, static_cast< float >( y ) * g_draw.m_sy, 70.f * g_draw.m_sx, 20.f * g_draw.m_sy, 1, 1, 0, 0, img( "gfx/2d/stripes.tga" ) );
			const std::string t = va( "%i", s_s.m_blue_score );
			text( 330.f, static_cast< float >( y + 15 ), 0.3f, k_white, t, style_shadowed_light );
		}
		else {
			box( 1, 575.f, -1.f, false, s_s.m_red_score );
			y += 24;
			box( 2, 575.f, -1.f, false, s_s.m_blue_score );
		}
		set_color( nullptr );
	}

	// sub_43282
	void overlay_row( const player_t& p, int x, int y, int w )
	{
		const skin_t& row = skin_row( p.m_team );
		float bg[ 4 ];
		team_color( bg, row.m_c[ 4 ] );
		grad_rect( static_cast< float >( x ), static_cast< float >( y ), static_cast< float >( w ), 11.f, bg );
		const std::string t = s_cv.i( "cg_drawTeamOverlayScores" ) ? va( "%s %i:%i", p.m_name.c_str( ), p.m_kills, p.m_deaths ) : p.m_name;
		const float tx      = static_cast< float >( 632 - width( t, 0.18f ) );
		if ( p.m_alive && p.m_c4 && p.m_team == s_s.m_team ) {
			const float hl[ 4 ] = { 1.f, 1.f, 0.f, 0.5f + 0.5f * std::cos( static_cast< float >( s_now ) * 0.01f ) };
			text( tx, static_cast< float >( y + 8 ), 0.18f, hl, t, style_shadowed_light );
		}
		else
			text( tx, static_cast< float >( y + 8 ), 0.18f, p.m_alive ? row.m_c[ 0 ] : row.m_c[ 1 ], t, style_shadowed_light );
		float marker[ 4 ];
		team_rgb( p.m_team, marker );
		if ( p.m_alive )
			fill_rect( 636.f, static_cast< float >( y ), 4.f, 11.f, marker );
		else
			draw_rect( 636.f, static_cast< float >( y ), 4.f, 11.f, 1.f, marker );
	}

	void draw_top_right( )
	{
		int left = s_cv.i( "cg_drawTeamOverlayScores" ) ? 540 : 560;
		int y = 9;
		const int right = 636;
		if ( s_cv.i( "cg_drawFPS" ) ) {
			int total = 0;
			for ( int t : s_frame_times )
				total += t;
			const int fps = total > 0 ? 4000 / total : 0;
			const std::string ping = va( "%i ping", s_s.m_ping ), f = va( "%i fps", fps );
			text( static_cast< float >( right - width( ping, 0.18f ) ), static_cast< float >( y ), 0.18f, k_white, ping, style_shadowed );
			text( static_cast< float >( right - 38 ), static_cast< float >( y ), 0.18f, k_white, "|", style_shadowed );
			text( static_cast< float >( right - 41 - width( f, 0.18f ) ), static_cast< float >( y ), 0.18f, k_white, f, style_shadowed );
			y += 8;
		}
		const int scores = s_cv.i( "cg_drawTeamScores" );
		if ( scores == 2 )
			draw_team_scores( 1, 0 );
		else if ( scores ) {
			draw_team_scores( 0, y );
			y += 48;
		}
		if ( s_cv.i( "cg_drawTimer" ) ) {
			if ( s_s.m_round_ms >= 0 ) {
				const std::string t = round_timer( s_s.m_round, s_s.m_max_rounds, s_s.m_round_ms );
				static constexpr float k_shadow[ 4 ] = { 0.f, 0.f, 0.f, 0.8f };
				const int timer = img( "icons/timer.tga" );
				set_color( k_shadow );
				draw_pic( static_cast< float >( right - 11 ), static_cast< float >( y ) + 4.5f, 11.f, 11.f, timer );
				set_color( nullptr );
				draw_pic( static_cast< float >( right ) - 11.5f, static_cast< float >( y + 4 ), 11.f, 11.f, timer );
				text( static_cast< float >( right - 15 - width( t, 0.22f ) ), static_cast< float >( y + 14 ), 0.22f, k_white, t, style_shadowed );
			}
		}

		// team overlay, bottom right upwards
		const int mode = s_cv.i( "cg_drawTeamOverlay" );
		if ( !mode || s_s.m_team == 3 )
			return;
		int red = 0, blue = 0;
		for ( const player_t& p : s_s.m_players )
			red += p.m_team == 1, blue += p.m_team == 2;
		y              = 420;
		const int w    = 630 - left;
		const player_t* me = player( s_s.m_me );
		if ( mode != 4 && me )
			overlay_row( *me, left + 10, y, w );
		y -= 13;
		if ( s_s.m_team != 0 ) {
			if ( mode == 3 || mode == 4 ) {
				const skin_t& row = skin_row( s_s.m_team );
				grad_rect( static_cast< float >( left + 10 ), static_cast< float >( y ), static_cast< float >( w ), 11.f, row.m_c[ 4 ] );
				const std::string t = va( "Friendly: %d", s_s.m_team == 1 ? red : blue );
				text( static_cast< float >( 635 - width( t, 0.18f ) ), static_cast< float >( y + 8 ), 0.18f, row.m_c[ 0 ], t, style_shadowed_light );
				y -= 11;
			}
			else
				for ( const player_t& p : s_s.m_players )
					if ( p.m_index != s_s.m_me && p.m_team == s_s.m_team )
						overlay_row( p, left + 10, y, w ), y -= 11;
		}
		y -= 5;
		if ( mode == 2 || mode == 3 ) {
			const int enemy = s_s.m_team == 1 ? 2 : s_s.m_team == 2 ? 1 : 0;
			const skin_t& row = skin_row( enemy );
			grad_rect( static_cast< float >( left + 10 ), static_cast< float >( y ), static_cast< float >( w ), 11.f, row.m_c[ 4 ] );
			const std::string t = va( "Enemies: %d", enemy == 1 ? red : enemy == 2 ? blue : 0 );
			text( static_cast< float >( 635 - width( t, 0.18f ) ), static_cast< float >( y + 8 ), 0.18f, row.m_c[ 0 ], t, style_shadowed_light );
		}
		else
			for ( const player_t& p : s_s.m_players ) {
				if ( p.m_index == s_s.m_me || p.m_name.empty( ) || p.m_team == 3 )
					continue;
				const bool show = p.m_team == s_s.m_team ? s_s.m_team == 0 : p.m_team != 0;
				if ( show )
					overlay_row( p, left + 10, y, w ), y -= 11;
			}
	}

	/* ---- small elements ---- */
	void draw_location( )
	{
		if ( !s_cv.i( "cg_drawMapLocation" ) || s_s.m_location.empty( ) || s_s.m_team == 3 )
			return;
		const int type = s_cv.i( "cg_crosshairNamesType" );
		const bool low = type >= 1 && type <= 3;
		text( low ? 50.f : 60.f, low ? 476.f : 465.f, 0.2f, k_white, s_s.m_location, style_shadowed_light );
	}

	void draw_clock( )
	{
		const int mode = s_cv.i( "cg_drawclock" );
		if ( !mode )
			return;
		SYSTEMTIME t{ };
		GetLocalTime( &t );
		std::string s{ };
		if ( mode == 2 ) {
			const int h = t.wHour % 12;
			s = va( "%02i:%02i:%02i %s", h == 0 ? 12 : h, t.wMinute, t.wSecond, t.wHour > 11 ? "PM" : "AM" );
		}
		else
			s = va( "%02i:%02i:%02i", t.wHour, t.wMinute, t.wSecond );
		text( 320.f - static_cast< float >( width( s, 0.24f ) / 2 ), 24.f, 0.24f, k_white, s, style_shadowed );
	}

	void draw_keys( )
	{
		const int mode = s_cv.i( "cg_drawKeys" );
		if ( mode != 2 && !( mode == 1 && s_s.m_following ) )
			return;
		static constexpr float k_idle[ 4 ] = { 1.f, 1.f, 1.f, 0.12f };
		static constexpr std::pair< int, const char* > k_keys[] = { { 2, "gfx/2d/key_forward.tga" }, { 4, "gfx/2d/key_backward.tga" }, { 8, "gfx/2d/key_left.tga" },
			                                                        { 16, "gfx/2d/key_right.tga" }, { 32, "gfx/2d/key_sprint.tga" }, { 64, "gfx/2d/key_walk.tga" } };
		for ( const auto& [ bit, path ] : k_keys ) {
			set_color( s_s.m_keys & bit ? k_white : k_idle );
			draw_pic( 288.f, 336.f, 64.f, 64.f, img( path ) );
		}
		set_color( k_white );
		if ( s_s.m_keys & 1 )
			draw_pic( 288.f, 336.f, 64.f, 64.f, img( "gfx/2d/key_jump.tga" ) );
		if ( s_s.m_keys & 128 )
			draw_pic( 288.f, 336.f, 64.f, 64.f, img( "gfx/2d/key_crouch.tga" ) );
		set_color( nullptr );
	}

	// sub_26925 + sub_26806
	void draw_lagometer( )
	{
		if ( s_cv.i( "cg_lagometer" ) ) {
			const float x = 586.f;
			float y       = 9.f;
			if ( s_cv.i( "cg_drawFPS" ) )
				y += 8.f;
			if ( s_cv.i( "cg_drawTeamScores" ) && s_cv.i( "cg_drawTeamScores" ) != 2 )
				y += 48.f;
			if ( s_cv.i( "cg_drawTimer" ) )
				y += 24.f;
			set_color( nullptr );
			draw_pic( x, y, 48.f, 48.f, img( "lagometer" ) );
			const float ax = x * g_draw.m_sx, ay = y * g_draw.m_sy, aw = 48.f * g_draw.m_sx, ah = 48.f * g_draw.m_sy;
			float range = ah / 3.f, mid = ay + range, v = range * 0.003333f;
			for ( int a = 0; a < static_cast< int >( aw ); ++a ) {
				const int i = ( s_lag_frame_n - 1 - a ) & 127;
				float h     = static_cast< float >( s_lag_frame[ i ] ) * v;
				if ( h > 0.f ) {
					set_color( k_color_table[ 3 ] );
					h = std::min( h, range );
					stretch_pic( ax + aw - static_cast< float >( a ), mid - h, 1.f, h, 0, 0, 0, 0, -1 );
				}
				else if ( h < 0.f ) {
					set_color( k_color_table[ 4 ] );
					h = std::min( -h, range );
					stretch_pic( ax + aw - static_cast< float >( a ), mid, 1.f, h, 0, 0, 0, 0, -1 );
				}
			}
			range = ah / 2.f, v = range * 0.001111f;
			for ( int a = 0; a < static_cast< int >( aw ); ++a ) {
				const int i = ( s_lag_snap_n - 1 - a ) & 127;
				float h     = static_cast< float >( s_lag_snap[ i ] );
				if ( h > 0.f ) {
					set_color( s_lag_snap_flags[ i ] & 1 ? k_color_table[ 3 ] : k_color_table[ 2 ] );
					h = std::min( h * v, range );
					stretch_pic( ax + aw - static_cast< float >( a ), ay + ah - h, 1.f, h, 0, 0, 0, 0, -1 );
				}
				else if ( h < 0.f ) {
					set_color( k_color_table[ 1 ] );
					stretch_pic( ax + aw - static_cast< float >( a ), ay + ah - range, 1.f, range, 0, 0, 0, 0, -1 );
				}
			}
			set_color( nullptr );
		}
		// connection interrupted
		if ( s_cv.i( "cg_noci" ) || s_s.m_since_received < 1.f )
			return;
		const std::string t = "Connection Interrupted";
		text( 320.f - static_cast< float >( width( t, 0.4f ) / 2 ), 100.f, 0.4f, k_white, t, style_shadowed );
		if ( !( ( s_now >> 9 ) & 1 ) )
			draw_pic( 616.f, 456.f, 24.f, 24.f, img( "gfx/2d/net.tga" ) );
	}

	// sub_32285 panel + sub_32917 / sub_32388
	void follow_panel( )
	{
		if ( s_cv.i( "cg_cleanFollow" ) )
			return;
		static constexpr float k_edge[ 4 ] = { 0.8f, 0.8f, 0.8f, 0.25f }, k_red[ 4 ] = { 0.5f, 0.f, 0.f, 0.25f }, k_blue[ 4 ] = { 0.f, 0.f, 0.5f, 0.25f },
		                       k_none[ 4 ] = { 0.f, 0.f, 0.f, 0.25f };
		draw_rect( 169.f, 404.f, 302.f, 72.f, 1.f, k_edge );
		fill_rect( 170.f, 405.f, 300.f, 70.f, s_s.m_team == 1 ? k_red : s_s.m_team == 2 ? k_blue : k_none );
	}

	void draw_follow( )
	{
		if ( s_s.m_following ) {
			follow_panel( );
			const float y = s_cv.i( "cg_cleanFollow" ) ? 475.f : 430.f;
			const std::string verb = "following ", name = s_s.m_follow_name;
			const float vw = static_cast< float >( width( verb, 0.4f ) ), nw = static_cast< float >( width( name, 0.4f ) );
			const float x  = 320.f - ( vw + nw ) * 0.5f;
			text( x, y, 0.4f, k_white, verb, style_shadowed );
			float c[ 4 ];
			team_color( c, skin_row( s_s.m_subject_team ).m_c[ 2 ] );
			text( x + vw, y, 0.4f, c, name, style_shadowed );
			if ( s_cv.i( "cg_cleanFollow" ) )
				return;
			if ( s_s.m_team == 3 )
				text( 180.f, 440.f, 0.25f, k_white, "Press 'ESC' to open menu and join game", style_normal );
			text( 180.f, 455.f, 0.25f, k_white, "Press 'USE' to switch to chase cam", style_normal );
			text( 180.f, 470.f, 0.25f, k_white, "Press 'FIRE' to follow the next player", style_normal );
		}
		else if ( s_s.m_free_spec && !s_cv.i( "cg_cleanFollow" ) ) {
			follow_panel( );
			const std::string t = "SPECTATOR";
			text( 320.f - static_cast< float >( width( t, 0.425f ) / 2 ), 430.f, 0.4f, k_white, t, style_shadowed );
			text( 180.f, 455.f, 0.25f, k_white, "Press 'ESC' to open menu and join game", style_normal );
			text( 180.f, 470.f, 0.25f, k_white, "Press 'FIRE' or 'USE' to switch to follow mode", style_normal );
		}
	}

	void centered( const std::string& t, float y, float scale, const float* c = k_white )
	{
		text( 320.f - static_cast< float >( width( t, scale ) / 2 ), y, scale, c, t, style_shadowed );
	}

	// sub_47206 strat time (sub_34299 warmup title removed on request)
	void draw_warmup( )
	{
		if ( s_s.m_warmup_ms < 0 && s_s.m_strat_ms >= 1000 )
			centered( va( "Strat Time: %i", s_s.m_strat_ms / 1000 ), 94.f, 0.425f );
	}

	// sub_35467 bomb carrier icon + defuse bar
	void draw_bomb( )
	{
		if ( !s_s.m_alive )
			return;
		if ( s_s.m_defuse >= 0.f ) {
			static constexpr float k_edge[ 4 ] = { 0.8f, 0.8f, 0.8f, 0.5f }, k_red[ 4 ] = { 0.5f, 0.f, 0.f, 0.5f }, k_blue[ 4 ] = { 0.f, 0.f, 0.5f, 0.5f };
			draw_rect( -1.f, 226.f, 643.f, 28.f, 1.f, k_edge );
			fill_rect( 0.f, 227.f, 641.f * s_s.m_defuse, 26.f, s_s.m_team == 1 ? k_red : k_blue );
			static constexpr const char* k_fmt[ 4 ] = { "DEFUSING        %02d%%", "DEFUSING...     %02d%%", "DEFUSING..      %02d%%", "DEFUSING.       %02d%%" };
			text( 64.f, 247.f, 0.315f, k_white, va( k_fmt[ ( s_now >> 9 ) & 3 ], static_cast< int >( s_s.m_defuse * 100.f ) ), style_shadowed );
		}
		// carrier bomb icon (296,434 pulse) removed on request
	}

	// sub_176297 weapon select row, 1.4 s after a switch
	void draw_weapon_select( )
	{
		const float* fade = fade_color( s_weapon_switch, 1400, s_now );
		if ( !fade || s_s.m_inventory.empty( ) || !s_s.m_alive )
			return;
		static constexpr float k_dim[ 4 ] = { 0.8f, 0.8f, 0.8f, 0.3f }, k_bg[ 4 ] = { 0.f, 0.f, 0.f, 0.2f };
		float f[ 4 ];
		std::memcpy( f, fade, sizeof( f ) );
		int x = 320 - static_cast< int >( s_s.m_inventory.size( ) ) * 40;
		for ( const int id : s_s.m_inventory ) {
			fill_rect( static_cast< float >( x + 4 ), 400.f, 72.f, 30.f, k_bg );
			const int icon = img( va( "icons/weapons/%s.tga", weapon_short( id ) ) );
			if ( id == s_s.m_weapon ) {
				const std::string n = k_weapons[ id ].m_name;
				text( static_cast< float >( x + 76 - width( n, 0.18f ) ), 398.f, 0.18f, f, n, style_shadowed_light );
				set_color( nullptr );
				draw_pic( static_cast< float >( x + 8 ), 399.f, 64.f, 32.f, icon );
			}
			else {
				set_color( k_dim );
				draw_pic( static_cast< float >( x + 18 ), 403.f, 48.f, 24.f, icon );
			}
			x += 80;
		}
		set_color( nullptr );
	}

	void drain_events( )
	{
		for ( auto& [ box, line ] : s_ev.m_lines )
			ring_push( box == 0 ? s_chat : s_msg, std::move( line ), s_cv.i( box == 0 ? "cg_chatHeight" : "cg_msgHeight" ), s_cv.i( box == 0 ? "cg_chatTime" : "cg_msgTime" ), s_now,
			           box == 0 );
		s_ev.m_lines.clear( );
		const auto center = [ ]( const std::string& t ) { s_center = { t, s_now, 120 }; };
		for ( const obituary_t& o : s_ev.m_obits ) {
			const obituary_out_t out = obituary( o, s_cv.i( "cg_drawKillLog" ) );
			for ( const std::string& m : out.m_msg )
				if ( !m.empty( ) )
					ring_push( s_msg, m, s_cv.i( "cg_msgHeight" ), s_cv.i( "cg_msgTime" ), s_now, false );
			if ( !out.m_center.empty( ) )
				center( out.m_center );
		}
		s_ev.m_obits.clear( );
		for ( const std::string& c : s_ev.m_centers )
			center( c );
		s_ev.m_centers.clear( );
		for ( const lag_sample_t& l : s_ev.m_lag ) {
			if ( l.m_kind == 0 )
				s_lag_frame[ s_lag_frame_n++ & 127 ] = l.m_value;
			else {
				s_lag_snap_flags[ s_lag_snap_n & 127 ] = l.m_flags;
				s_lag_snap[ s_lag_snap_n++ & 127 ]     = l.m_value;
			}
		}
		s_ev.m_lag.clear( );
		std::memcpy( s_hit, s_ev.m_hit, sizeof( s_hit ) );
		s_limp          = s_ev.m_limp;
		s_weapon_switch = s_ev.m_weapon_switch;
	}
}

/* ---- game thread ---- */

void n_urt_hud::impl_t::on_paint_traverse( )
{
	const bool on      = n_urt_hud::enabled( );
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
	const int me               = engine->get_local_player( );
	s.m_me                     = me;
	s.m_team                   = urt_team( local->get_team( ) );
	s.m_map                    = engine->get_level_name_short( ) ? engine->get_level_name_short( ) : "";

	// subject: local, or whoever is followed in eye / chase (urt's ps = the followed player's)
	c_base_entity* subject = local;
	if ( !local->is_alive( ) ) {
		const int mode = local->get_observer_mode( );
		auto* target   = g_interfaces.m_client_entity_list->get< c_base_entity >( local->get_observer_target_handle( ) );
		if ( ( mode == 4 || mode == 5 ) && target && target->is_player( ) && target->is_alive( ) && target != local ) {
			subject       = target;
			s.m_following = true;
		}
		else
			s.m_free_spec = true;
	}
	s.m_subject      = subject->get_index( );
	s.m_subject_team = urt_team( subject->get_team( ) );
	s.m_alive        = subject->is_alive( );
	if ( s.m_following )
		s.m_follow_name = player_name( s.m_subject );
	s.m_health = std::clamp( subject->get_health( ), 0, 100 );
	if ( subject == local ) {
		// csgo m_flStamina 0 = fresh, sv_staminamax = spent
		const float max = std::max( g_convars.float_or( HASH_BT( "sv_staminamax" ), 80.f ), 1.f );
		s.m_stamina     = 1.f - std::clamp( local->get_stamina( ) / max, 0.f, 1.f );
	}
	const c_vector& origin = subject->get_abs_origin( );
	const c_vector& vel    = subject->get_velocity( );
	s.m_origin[ 0 ] = origin.m_x, s.m_origin[ 1 ] = origin.m_y, s.m_origin[ 2 ] = origin.m_z;
	s.m_velocity[ 0 ] = vel.m_x, s.m_velocity[ 1 ] = vel.m_y, s.m_velocity[ 2 ] = vel.m_z;
	s.m_ducked    = subject->get_duck_amount( ) > 0.5f;
	s.m_on_ground = ( subject->get_flags( ) & fl_onground ) != 0;
	c_angle view{ };
	engine->get_view_angles( view );
	static const unsigned int eye_yaw = net_offset( HASH_BT( "CCSPlayer->m_angEyeAngles[1]" ) );
	s.m_yaw     = subject == local ? view.m_y : eye_yaw ? field< float >( subject, eye_yaw ) : subject->get_abs_angles( ).m_y;
	s.m_view[ 0 ] = view.m_x, s.m_view[ 1 ] = view.m_y;
	const c_vector eye = local->get_eye_position( );
	s.m_eye[ 0 ] = eye.m_x, s.m_eye[ 1 ] = eye.m_y, s.m_eye[ 2 ] = eye.m_z;
	if ( subject == local ) {
		const int b = g_ctx.m_input_buttons;
		s.m_keys    = ( b & in_jump ? 1 : 0 ) | ( b & in_forward ? 2 : 0 ) | ( b & in_back ? 4 : 0 ) | ( b & in_moveleft ? 8 : 0 ) | ( b & in_moveright ? 16 : 0 ) |
		           ( b & in_speed ? 64 : 0 ) | ( b & in_duck ? 128 : 0 );
	}
	else {
		const float yaw = s.m_yaw * 0.0174533f;
		const float f = vel.m_x * std::cos( yaw ) + vel.m_y * std::sin( yaw ), r = vel.m_x * std::sin( yaw ) - vel.m_y * std::cos( yaw );
		s.m_keys = ( !s.m_on_ground && vel.m_z > 0.f ? 1 : 0 ) | ( f > 50.f ? 2 : 0 ) | ( f < -50.f ? 4 : 0 ) | ( r < -50.f ? 8 : 0 ) | ( r > 50.f ? 16 : 0 ) |
		           ( s.m_ducked ? 128 : 0 );
	}
	if ( const char* place = subject->get_last_place( ); place && *place ) {
		const wchar_t* w = g_interfaces.m_localize ? g_interfaces.m_localize->find_safe( place ) : nullptr;
		s.m_location     = to_q3( w && *w ? g_utilities.to_utf8( w ) : std::string( place ) );
	}
	if ( auto* const nci = engine->get_net_channel_info( ) ) {
		s.m_ping           = static_cast< int >( ( nci->get_latency( FLOW_OUTGOING ) + nci->get_latency( FLOW_INCOMING ) ) * 1000.f );
		s.m_since_received = nci->get_time_since_last_received( );
	}

	// weapons
	const unsigned int active = subject->get_active_weapon_handle( );
	s.m_weapon_handle         = active;
	for ( int i = 0; i < 64; ++i ) {
		const unsigned int handle = subject->get_weapons_handle( )[ i ];
		if ( !handle || handle == 0xFFFFFFFF )
			continue;
		auto* const weapon = g_interfaces.m_client_entity_list->get< c_base_entity >( handle );
		if ( !weapon )
			continue;
		const c_weapon_data* const data = g_interfaces.m_weapon_system->get_weapon_data( weapon->get_item_definition_index( ) );
		if ( !data || !data->m_weapon_name )
			continue;
		const int id = weapon_for( data->m_weapon_name );
		if ( id == wp_bomb )
			s.m_carry_c4 = true;
		if ( id != wp_none )
			s.m_inventory.push_back( id );
		if ( handle != active )
			continue;
		s.m_weapon   = id;
		s.m_clip     = std::max( weapon->get_ammo( ), 0 );
		s.m_clip_max = std::max( data->m_max_clip1, 0 );
		s.m_clips    = s.m_clip_max > 0 ? ( std::max( weapon->get_ammo_reserve( ), 0 ) + s.m_clip_max - 1 ) / s.m_clip_max : 0;
		s.m_scoped   = subject->is_scoped( );
		// ponytail: ps.stats[1] (server side recoil) has no csgo twin, m_fAccuracyPenalty * 25 lands in the same 0..1 band
		s.m_pulse    = std::clamp( weapon->get_accuracy_penalty( ) * 25.f, 0.f, 1.5f );
		// fire mode text (sub_55089) only for urt guns with modes: csgo's own burst / auto state picks the name
		const weapon_t& uw = k_weapons[ id ];
		if ( uw.m_modes[ 1 ].m_name[ 0 ] ) {
			const char* want = weapon->is_burst_mode( ) ? "Burst" : data->m_full_auto ? "Automatic" : "Semi-Automatic";
			for ( int m = 0; m < 3; ++m )
				if ( std::strcmp( uw.m_modes[ m ].m_name, want ) == 0 )
					s.m_mode = m, s.m_mode_name = want;
			if ( s.m_mode_name.empty( ) )
				s.m_mode_name = uw.m_modes[ 0 ].m_name;
		}
	}
	std::sort( s.m_inventory.begin( ), s.m_inventory.end( ), [ ]( int a, int b ) {
		const auto slot = [ ]( int id ) {
			return id == wp_knife ? 3 : id == wp_he || id == wp_flash || id == wp_smoke ? 4 : id == wp_bomb ? 5 : id == wp_beretta || id == wp_deagle || id == wp_glock || id == wp_colt || id == wp_magnum ? 2 : 1;
		};
		return slot( a ) < slot( b );
	} );

	// crosshair entity (sub_30250)
	{
		c_vector fwd{ };
		const float cp = std::cos( view.m_x * 0.0174533f ), sp = std::sin( view.m_x * 0.0174533f ), cy = std::cos( view.m_y * 0.0174533f ), sy = std::sin( view.m_y * 0.0174533f );
		fwd = c_vector( cp * cy, cp * sy, -sp );
		c_game_trace tr{ };
		c_trace_filter filter( local );
		g_interfaces.m_engine_trace->trace_ray( ray_t( eye, eye + fwd * 8192.f ), mask_shot | contents_grate, &filter, &tr );
		if ( tr.m_hit_entity && tr.m_hit_entity->is_player( ) && tr.m_hit_entity->get_index( ) <= 64 ) {
			s.m_target        = tr.m_hit_entity->get_index( );
			s.m_target_team   = urt_team( tr.m_hit_entity->get_team( ) );
			s.m_target_health = std::clamp( tr.m_hit_entity->get_health( ), 0, 100 );
		}
	}

	// players + resource
	static const unsigned int res_kills  = net_offset( HASH_BT( "CCSPlayerResource->m_iKills" ) );
	static const unsigned int res_deaths = net_offset( HASH_BT( "CCSPlayerResource->m_iDeaths" ) );
	static const unsigned int res_score  = net_offset( HASH_BT( "CCSPlayerResource->m_iScore" ) );
	static const unsigned int res_site_a = net_offset( HASH_BT( "CCSPlayerResource->m_bombsiteCenterA" ) );
	static const unsigned int res_site_b = net_offset( HASH_BT( "CCSPlayerResource->m_bombsiteCenterB" ) );
	static const unsigned int team_num   = net_offset( HASH_BT( "CCSTeam->m_iTeamNum" ) );
	static const unsigned int team_score = net_offset( HASH_BT( "CCSTeam->m_scoreTotal" ) );
	static const unsigned int bomb_zone  = net_offset( HASH_BT( "CCSPlayer->m_bInBombZone" ) );
	const void* resource = nullptr;
	const int highest    = std::min( g_interfaces.m_client_entity_list->get_highest_entity_index( ), 2048 );
	for ( int i = 65; i <= highest; ++i ) {
		auto* const e = g_interfaces.m_client_entity_list->get< c_base_entity >( i );
		if ( !e )
			continue;
		const auto client_class = static_cast< c_client_networkable* >( e )->get_client_class( );
		if ( !client_class )
			continue;
		const e_class_ids id = client_class->m_class_id;
		if ( id == e_class_ids::ccs_team && team_num && team_score ) {
			const int num = field< int >( e, team_num ), score = field< int >( e, team_score );
			if ( num == 2 )
				s.m_red_score = score;
			else if ( num == 3 )
				s.m_blue_score = score;
		}
		else if ( id == e_class_ids::ccs_player_resource )
			resource = e;
		else if ( id == e_class_ids::c_planted_c4 && e->is_bomb_ticking( ) && !e->is_dormant( ) ) {
			const c_vector& o = e->get_abs_origin( );
			s.m_bomb_planted = s.m_bomb_seen = true;
			s.m_bomb_pos[ 0 ] = o.m_x, s.m_bomb_pos[ 1 ] = o.m_y;
			if ( local->is_defusing( ) && local->is_alive( ) ) {
				static const unsigned int length = net_offset( HASH_BT( "CPlantedC4->m_flDefuseLength" ) );
				const float len = length ? field< float >( e, length ) : 10.f;
				s.m_defuse      = std::clamp( 1.f - ( e->get_defuse_end_time( ) - g_interfaces.m_global_vars_base->m_current_time ) / std::max( len, 0.1f ), 0.f, 1.f );
			}
		}
		else if ( id == e_class_ids::cc4 && !s.m_bomb_planted && !e->is_dormant( ) ) {
			auto* owner = g_interfaces.m_client_entity_list->get< c_base_entity >( e->get_owner_entity_handle( ) );
			const c_vector& o = owner ? owner->get_abs_origin( ) : e->get_abs_origin( );
			s.m_bomb_seen     = true;
			s.m_bomb_pos[ 0 ] = o.m_x, s.m_bomb_pos[ 1 ] = o.m_y;
		}
	}
	if ( resource && res_site_a && res_site_b ) {
		const c_vector a = field< c_vector >( resource, res_site_a ), b = field< c_vector >( resource, res_site_b );
		if ( a.m_x != 0.f || a.m_y != 0.f ) {
			s.m_have_sites = true;
			s.m_sites[ 0 ][ 0 ] = a.m_x, s.m_sites[ 0 ][ 1 ] = a.m_y, s.m_sites[ 1 ][ 0 ] = b.m_x, s.m_sites[ 1 ][ 1 ] = b.m_y;
		}
	}
	if ( bomb_zone && subject == local )
		s.m_in_bomb_zone = field< bool >( local, bomb_zone );

	for ( int i = 1; i <= 64; ++i ) {
		auto* const p = g_interfaces.m_client_entity_list->get< c_base_entity >( i );
		player_info_t info{ };
		if ( !engine->get_player_info( i, &info ) )
			continue;
		player_t pl{ };
		pl.m_index = i;
		pl.m_name  = player_name( i );
		if ( p ) {
			pl.m_team    = urt_team( p->get_team( ) );
			pl.m_alive   = p->is_alive( );
			pl.m_dormant = p->is_dormant( );
			const c_vector& o = p->get_abs_origin( );
			pl.m_pos[ 0 ] = o.m_x, pl.m_pos[ 1 ] = o.m_y, pl.m_pos[ 2 ] = o.m_z;
			pl.m_yaw = eye_yaw ? field< float >( p, eye_yaw ) : p->get_abs_angles( ).m_y;
		}
		if ( resource ) {
			if ( res_kills )
				pl.m_kills = field< int >( resource, res_kills + i * 4 );
			if ( res_deaths )
				pl.m_deaths = field< int >( resource, res_deaths + i * 4 );
			if ( res_score )
				pl.m_score = field< int >( resource, res_score + i * 4 );
		}
		if ( s.m_bomb_seen && !s.m_bomb_planted && p && pl.m_alive ) {
			const float dx = pl.m_pos[ 0 ] - s.m_bomb_pos[ 0 ], dy = pl.m_pos[ 1 ] - s.m_bomb_pos[ 1 ];
			pl.m_c4 = dx * dx + dy * dy < 1.f;
		}
		if ( i == s.m_target )
			s.m_target_c4 = pl.m_c4;
		s.m_players.push_back( std::move( pl ) );
	}

	// world bounds (CWorld), the minimap's CM bounds
	if ( auto* const world = g_interfaces.m_client_entity_list->get< c_base_entity >( 0 ) ) {
		static const unsigned int mins = net_offset( HASH_BT( "CWorld->m_WorldMins" ) ), maxs = net_offset( HASH_BT( "CWorld->m_WorldMaxs" ) );
		if ( mins && maxs ) {
			const c_vector a = field< c_vector >( world, mins ), b = field< c_vector >( world, maxs );
			s.m_world_min[ 0 ] = a.m_x, s.m_world_min[ 1 ] = a.m_y, s.m_world_min[ 2 ] = a.m_z;
			s.m_world_max[ 0 ] = b.m_x, s.m_world_max[ 1 ] = b.m_y, s.m_world_max[ 2 ] = b.m_z;
			s.m_have_world     = b.m_x > a.m_x && b.m_y > a.m_y;
		}
	}

	// round clock (CCSGameRules): round x of mp_maxrounds | time left, warmup countdown, freeze = strat time
	static const unsigned int warmup_ofs = net_offset( HASH_BT( "CCSGameRulesProxy->m_bWarmupPeriod" ) );
	static const unsigned int warmup_end = net_offset( HASH_BT( "CCSGameRulesProxy->m_fWarmupPeriodEnd" ) );
	static const unsigned int freeze_ofs = net_offset( HASH_BT( "CCSGameRulesProxy->m_bFreezePeriod" ) );
	static const unsigned int time_ofs   = net_offset( HASH_BT( "CCSGameRulesProxy->m_iRoundTime" ) );
	static const unsigned int start_ofs  = net_offset( HASH_BT( "CCSGameRulesProxy->m_fRoundStartTime" ) );
	static const unsigned int rounds_ofs = net_offset( HASH_BT( "CCSGameRulesProxy->m_totalRoundsPlayed" ) );
	const float now_game                 = g_interfaces.m_global_vars_base->m_current_time;
	if ( const std::uintptr_t rules = game_rules( ); rules && time_ofs && start_ofs ) {
		const float start = field< float >( reinterpret_cast< void* >( rules ), start_ofs );
		const int round   = field< int >( reinterpret_cast< void* >( rules ), time_ofs );
		const bool warmup = warmup_ofs && field< bool >( reinterpret_cast< void* >( rules ), warmup_ofs );
		if ( warmup && warmup_end )
			s.m_warmup_ms = static_cast< int >( std::max( field< float >( reinterpret_cast< void* >( rules ), warmup_end ) - now_game, 0.f ) * 1000.f );
		if ( !warmup && freeze_ofs && field< bool >( reinterpret_cast< void* >( rules ), freeze_ofs ) )
			s.m_strat_ms = static_cast< int >( std::max( start - now_game, 0.f ) * 1000.f );
		if ( round > 0 )
			s.m_round_ms = static_cast< int >( std::clamp( static_cast< float >( round ) - std::max( now_game - start, 0.f ), 0.f, static_cast< float >( round ) ) * 1000.f );
		s.m_round      = ( rounds_ofs ? field< int >( reinterpret_cast< void* >( rules ), rounds_ofs ) : 0 ) + 1;
		s.m_max_rounds = warmup ? 0 : g_convars.int_or( HASH_BT( "mp_maxrounds" ), 0 );
	}

	mapgen_step( s, m_lock );

	std::lock_guard< std::mutex > lock( m_lock );
	// lagometer: frame = time since the last packet minus the interp window (>0 extrapolating), snapshot = ping per tick
	{
		static int s_tick = 0;
		const float interp = std::max( g_convars.float_or( HASH_BT( "cl_interp" ), 0.f ),
		                               g_convars.float_or( HASH_BT( "cl_interp_ratio" ), 2.f ) / std::max( g_convars.float_or( HASH_BT( "cl_updaterate" ), 64.f ), 1.f ) );
		s_ev.m_lag.push_back( { 0, static_cast< int >( ( s.m_since_received - interp ) * 1000.f ), 0 } );
		if ( g_interfaces.m_global_vars_base->m_tick_count != s_tick ) {
			s_tick = g_interfaces.m_global_vars_base->m_tick_count;
			auto* const nci = engine->get_net_channel_info( );
			const bool lost = nci && nci->get_avg_loss( FLOW_INCOMING ) > 0.01f;
			const bool choke = nci && nci->get_avg_choke( FLOW_INCOMING ) > 0.01f;
			s_ev.m_lag.push_back( { 1, lost ? -1 : std::max( s.m_ping, 1 ), choke ? 1 : 0 } );
		}
		if ( s_ev.m_lag.size( ) > 512 )
			s_ev.m_lag.erase( s_ev.m_lag.begin( ), s_ev.m_lag.end( ) - 256 );
	}
	if ( s.m_weapon_handle != s_ev.m_last_weapon ) {
		if ( s_ev.m_last_weapon )
			s_ev.m_weapon_switch = now_ms( );
		s_ev.m_last_weapon = s.m_weapon_handle;
	}
	m_snapshot = std::move( s );
}

void n_urt_hud::impl_t::on_event( game_event_t* event )
{
	if ( !n_urt_hud::enabled( ) || !event || !g_interfaces.m_engine_client )
		return;
	const std::string_view name = event->get_name( );
	auto* const engine          = g_interfaces.m_engine_client;
	const int me                = engine->get_local_player( );
	const auto index            = [ & ]( const char* key ) { return engine->get_player_for_user_id( event->get_int( key ) ); };
	const auto team             = [ ]( int i ) {
		auto* e = g_interfaces.m_client_entity_list->get< c_base_entity >( i );
		return e ? urt_team( e->get_team( ) ) : 0;
	};

	if ( name == "player_death" ) {
		const int victim = index( "userid" ), attacker = index( "attacker" );
		std::string weapon = event->get_string( "weapon" );
		obituary_t o{ };
		const int my_team = team( me );
		o.m_victim        = colored( player_name( victim ), team( victim ), my_team );
		o.m_attacker      = colored( player_name( attacker ), team( attacker ), my_team );
		o.m_me_victim     = victim == me;
		o.m_me_attacker   = attacker == me && victim != me;
		if ( weapon == "planted_c4" )
			o.m_kind = death_bomb;
		else if ( attacker <= 0 || weapon == "world" || weapon == "worldspawn" || weapon == "trigger_hurt" )
			o.m_kind = weapon == "worldspawn" || weapon == "world" ? death_fall : death_world;
		else if ( attacker == victim )
			o.m_kind = death_suicide, o.m_weapon = weapon_for( "weapon_" + weapon );
		else {
			o.m_weapon = weapon == "taser" ? -1 : weapon == "inferno" ? wp_he : weapon_for( "weapon_" + weapon );
			o.m_flavor = weapon == "inferno" || weapon == "incgrenade" || weapon == "molotov" || event->get_bool( "noscope" ) ? 1 : 0;
			if ( o.m_weapon == wp_none && weapon.find( "knife" ) != std::string::npos )
				o.m_weapon = wp_knife;
			if ( auto* a = g_interfaces.m_client_entity_list->get< c_base_entity >( attacker ) )
				o.m_attacker_health = std::clamp( a->get_health( ), 0, 100 );
		}
		// cg_drawKillLog >= 5 place: teammates of the reference player with a higher score
		const int ref = o.m_me_attacker ? attacker : victim;
		{
			std::lock_guard< std::mutex > lock( m_lock );
			int ref_score = 0;
			for ( const player_t& p : m_snapshot.m_players )
				if ( p.m_index == ref )
					ref_score = p.m_score, o.m_ref_team = p.m_team;
			for ( const player_t& p : m_snapshot.m_players )
				if ( p.m_index != ref && p.m_team == o.m_ref_team && p.m_score > ref_score )
					++o.m_place;
			o.m_ref_score = ref_score;
			s_ev.m_obits.push_back( std::move( o ) );
		}
		if ( attacker == me && victim != me && s_gcv.m_killsound )
			play( "sound/feedback/kill.wav" );
		return;
	}

	if ( name == "player_hurt" ) {
		const int victim = index( "userid" ), attacker = index( "attacker" );
		if ( attacker == me && victim != me && s_gcv.m_hitsound && ( event->get_int( "health" ) > 0 || !s_gcv.m_killsound ) )
			play( "sound/feedback/hit.wav" );
		if ( victim == me ) {
			std::lock_guard< std::mutex > lock( m_lock );
			// csgo hitgroup -> urt hit location slots
			switch ( event->get_int( "hitgroup" ) ) {
			case 1: s_ev.m_hit[ 1 ] = true; break;
			case 2: s_ev.m_hit[ 3 ] = true; break;
			case 3: s_ev.m_hit[ 7 ] = true; break;
			case 4: s_ev.m_hit[ 6 ] = true; break;
			case 5: s_ev.m_hit[ 5 ] = true; break;
			case 6: s_ev.m_hit[ 10 ] = true, s_ev.m_limp = true; break;
			case 7: s_ev.m_hit[ 9 ] = true, s_ev.m_limp = true; break;
			default: break;
			}
		}
		return;
	}

	if ( name == "player_spawn" && index( "userid" ) == me ) {
		std::lock_guard< std::mutex > lock( m_lock );
		std::memset( s_ev.m_hit, 0, sizeof( s_ev.m_hit ) );
		s_ev.m_limp = false;
	}
}

// game thread (push_notice): csgo chat -> the chat box, "name^3: text" like urt's server lines
void n_urt_hud::impl_t::on_chat( const char* text, int client, bool html )
{
	if ( !n_urt_hud::enabled( ) || !text || !*text )
		return;
	std::string out{ };
	const int sender = !html && client > 0 && client <= 64 ? client : 0;
	int sender_team  = 0;
	if ( sender )
		if ( auto* e = g_interfaces.m_client_entity_list->get< c_base_entity >( sender ) )
			sender_team = urt_team( e->get_team( ) );
	const std::string_view t( text );
	for ( size_t i = 0; i < t.size( ); ) {
		const auto b = static_cast< unsigned char >( t[ i ] );
		if ( html && b == '<' ) {
			const size_t close = t.find( '>', i );
			if ( close == std::string_view::npos )
				break;
			i = close + 1;
			continue;
		}
		if ( b > 0 && b < 17 ) {
			out += b == 3 ? ( sender_team == 1 ? "^1" : sender_team == 2 ? "^4" : "^3" ) : "^3";
			++i;
			continue;
		}
		const size_t start = i++;
		while ( i < t.size( ) && ( static_cast< unsigned char >( t[ i ] ) & 0xC0 ) == 0x80 )
			++i;
		out += to_q3( t.substr( start, i - start ) );
	}
	const bool team_chat = out.find( "(Counter-Terrorist)" ) != std::string::npos || out.find( "(Terrorist)" ) != std::string::npos;
	if ( s_gcv.m_team_chats_only && !team_chat && sender )
		return;
	if ( s_gcv.m_chat_sound > 1 && sender )
		play( "sound/player/talk.wav" );
	std::lock_guard< std::mutex > lock( m_lock );
	s_ev.m_lines.emplace_back( 0, std::move( out ) );
}

// game thread (paint_traverse): send / drop the typed line, mc_hud's drain
void n_urt_hud::impl_t::drain_chat( bool active )
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
	if ( send && !message.empty( ) )
		g_interfaces.m_engine_client->client_cmd_unrestricted( ( std::string( team ? "say_team \"" : "say \"" ) + message + "\"" ).c_str( ) );
}

// window thread: the messagemode bind opens our line instead of the csgo chat panel (cod4 / mc copy)
bool n_urt_hud::impl_t::chat_on_key( unsigned int msg, unsigned int wide_param, long long_param, bool ui_input )
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
		if ( msg != WM_KEYDOWN || stale || !n_urt_hud::enabled( ) )
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

bool n_urt_hud::hides_scope( )
{
	const snapshot_t& s = g_urt_hud.m_snapshot;
	return enabled( ) && s.m_in_game && s.m_scoped && scoped_weapon( s.m_weapon );
}

void n_urt_hud::impl_t::on_release( ) { }

/* ---- render thread ---- */

void n_urt_hud::impl_t::on_end_scene( )
{
	if ( !n_urt_hud::enabled( ) )
		return;
	sync_cvars( );
	s_now = now_ms( );
	if ( s_last_frame )
		s_frame_times[ s_frame_index++ & 3 ] = std::max( s_now - s_last_frame, 1 );
	s_last_frame = s_now;
	{
		std::lock_guard< std::mutex > lock( m_lock );
		if ( !m_snapshot.m_in_game )
			return;
		s_s = m_snapshot;
		drain_events( );
		if ( s_load_state == 2 )
			upload_map( );
	}
	{
		std::scoped_lock lock( m_chat_mutex );
		s_chat_line = m_chat_open ? std::string( m_chat_team ? "say_team: " : "say: " ) + to_q3( g_utilities.to_utf8( m_chat_input ) ) + ( ( s_now >> 8 ) & 1 ? "_" : "" )
		                          : std::string( );
	}
	if ( s_load_state != 2 )
		return;

	const int w = static_cast< int >( g_ctx.m_width ), h = static_cast< int >( g_ctx.m_height );
	if ( w <= 0 || h <= 0 )
		return;
	// stretched like urt (cgs.screenXScale / YScale), laid out in the pre-stretch width, shifted after (cod4 note)
	const float left = g_render.to_overlay_x( 0.f ), right = g_render.to_overlay_x( static_cast< float >( w ) );
	g_draw.m_list     = ImGui::GetBackgroundDrawList( );
	g_draw.m_sx       = ( right - left ) / 640.f;
	g_draw.m_sy       = static_cast< float >( h ) / 480.f;
	g_draw.m_files    = &s_files;
	g_draw.m_textures = &s_textures;
	g_draw.m_blend    = 0;
	set_color( nullptr );
	const int first_vertex = g_draw.m_list->VtxBuffer.Size;
	s_layout_w = right - left, s_layout_h = static_cast< float >( h );
	if ( s_map_flip.exchange( false ) )
		save_cvar( "cg_maptoggle", s_cv.i( "cg_maptoggle" ) ? "0" : "1" );

	// CG_Draw2D order
	const bool draw2d = s_cv.i( "cg_draw2D" ) != 0;
	if ( s_s.m_alive && s_s.m_scoped && scoped_weapon( s_s.m_weapon ) )
		draw_scope( );
	if ( draw2d ) {
		if ( s_s.m_free_spec ) {
			draw_follow( );
			draw_crosshair_names( );
			draw_minimap( );
		}
		else {
			if ( !( s_s.m_scoped && scoped_weapon( s_s.m_weapon ) ) && s_s.m_alive )
				draw_crosshair( );
			draw_minimap( );
			if ( s_s.m_alive ) {
				draw_crosshair_names( );
				draw_weapon_select( );
			}
		}
		draw_bomb( );
	}
	draw_chat( );
	draw_messages( );
	if ( draw2d ) {
		draw_clock( );
		draw_keys( );
		draw_location( );
		draw_top_right( );
		if ( s_s.m_subject_team != 3 && !s_s.m_free_spec ) {
			draw_health( );
			draw_weapon( );
			draw_speedo_mode( );
		}
		if ( !s_s.m_free_spec )
			draw_follow( );
		draw_warmup( );
		draw_lagometer( );
		draw_center( );
	}

	set_blend( 0 );
	for ( int v = first_vertex; left != 0.f && v < g_draw.m_list->VtxBuffer.Size; ++v )
		g_draw.m_list->VtxBuffer[ v ].pos.x += left;
	g_draw.m_list->AddCallback( ImDrawCallback_ResetRenderState, nullptr );
}

void n_urt_hud::impl_t::release_textures( )
{
	for ( texture_t& t : s_textures ) {
		if ( t.m_texture )
			t.m_texture->Release( );
		t = { };
	}
	if ( s_map_texture )
		s_map_texture->Release( );
	s_map_texture = nullptr;
	s_map_texture_name.clear( );
}

/* ---- options (visuals tab popup): every cg_ hud cvar ---- */

void n_urt_hud::draw_options( )
{
	sync_cvars( );
	const int state = s_load_state;
	if ( state == 2 )
		ImGui::TextDisabled( s_missing ? "urt: %d files missing" : "urt: ready", s_missing );
	else if ( state == 3 ) {
		ImGui::TextDisabled( "urt: download failed" );
		if ( ImGui::Button( "retry##urt", ImVec2( -1.f, 15.f ) ) )
			s_load_state = 0;
	}
	else
		ImGui::TextDisabled( "urt: loading" );
	if ( s_gen.m_state == 1 )
		ImGui::TextDisabled( "minimap: tracing %d%%", s_gen.m_row * 100 / 512 );

	enum e_kind { k_bool, k_int, k_float, k_color, k_combo };
	struct option_t {
		const char* m_cvar;
		const char* m_label;
		e_kind m_kind;
		float m_min, m_max;
		const char* m_items = nullptr; // k_combo: \0 separated, item 0 = value m_min
	};
	static constexpr const char* k_scope_styles = "cross + black bars\0cross + ticks\0mil-dot rail\0dot\0cross + dot\0";
	static constexpr const char* k_skins = "team\0green\0red\0blue\0purple\0orange\0olive\0white\0black\0desert\0cowboy\0cavalry\0droogs\0dr pink\0dr blue\0";
	static constexpr option_t k_options[] = {
		{ "cg_draw2D", "draw 2d", k_bool, 0, 1 },
		{ "cg_drawCrosshair", "crosshair", k_combo, 0, 14, "off\0circle\0cross\0cross 2\0expanding cross\0expanding triangle\0expanding cross 2\0expanding cross 3\0dot\0ring\0cross gaps\0cross gaps + dot\0cross gaps no top\0pixel\0plus\0" },
		{ "cg_crosshairSize", "crosshair size", k_float, 1, 60 },
		{ "cg_crosshairX", "crosshair x", k_int, -320, 320 },
		{ "cg_crosshairY", "crosshair y", k_int, -240, 240 },
		{ "cg_crosshairRGB", "crosshair color", k_color, 0, 1 },
		{ "cg_crosshairFriendRGB", "crosshair friend color", k_color, 0, 1 },
		{ "cg_drawCrosshairNames", "crosshair names", k_bool, 0, 1 },
		{ "cg_crosshairNamesType", "names type", k_combo, 0, 3, "bottom text\0above head\0health bar low\0health bar high\0" },
		{ "cg_crosshairNamesSize", "names size", k_float, 0, 1 },
		{ "cg_scopeRGB", "scope color", k_color, 0, 1 },
		{ "cg_scopeFriendRGB", "scope friend color", k_color, 0, 1 },
		{ "cg_scopeG36", "scope g36 (aug / sg553)", k_combo, 0, 4, k_scope_styles },
		{ "cg_scopePSG", "scope psg1 (autos)", k_combo, 0, 4, k_scope_styles },
		{ "cg_scopeSR8", "scope sr8 (awp)", k_combo, 0, 4, k_scope_styles },
		{ "cg_scopeFRF1", "scope frf1 (scout)", k_combo, 0, 4, k_scope_styles },
		{ "cg_scopering", "scope ring", k_combo, 0, 2, "off\0sr8 (awp)\0all scopes\0" },
		{ "cg_drawhealth", "health numbers", k_bool, 0, 1 },
		{ "cg_hudWeaponInfo", "weapon info", k_combo, 0, 2, "icon\0name\0icon + name\0" },
		{ "cg_speedo", "speedometer", k_combo, 0, 3, "off\0ups\0kph\0mph\0" },
		{ "cg_accelero", "accelerometer", k_combo, 0, 2, "off\0colored speed\0bar\0" },
		{ "cg_accelSize", "accelerometer size", k_float, 8, 502 },
		{ "cg_drawFPS", "fps / ping", k_bool, 0, 1 },
		{ "cg_drawTimer", "timer", k_bool, 0, 1 },
		{ "cg_drawTeamScores", "team scores", k_combo, 0, 2, "off\0top right\0top center\0" },
		{ "cg_drawTeamOverlay", "team overlay", k_combo, 0, 4, "off\0everyone\0team + enemy count\0counts\0friendly count + enemies\0" },
		{ "cg_drawTeamOverlayScores", "team overlay scores", k_bool, 0, 1 },
		{ "cg_lagometer", "lagometer", k_bool, 0, 1 },
		{ "cg_noci", "hide connection interrupted", k_bool, 0, 1 },
		{ "cg_drawclock", "clock", k_combo, 0, 2, "off\0" "24 hour\0" "12 hour\0" },
		{ "cg_drawKeys", "keys", k_combo, 0, 2, "off\0spectating\0always\0" },
		{ "cg_drawMapLocation", "map location", k_bool, 0, 1 },
		{ "cg_maptoggle", "minimap", k_bool, 0, 1 },
		{ "cg_mapsize", "minimap size", k_int, 0, 512 },
		{ "cg_mappos", "minimap position", k_combo, 1, 11, "top left\0left\0bottom left\0bottom left inset\0bottom center\0bottom right\0right\0top right\0top right inset\0top center\0center\0" },
		{ "cg_mapalpha", "minimap alpha", k_float, 0, 1 },
		{ "cg_maparrowscale", "minimap arrow scale", k_float, 0, 10 },
		{ "cg_drawKillLog", "kill log", k_combo, 0, 5, "off\0flavor text\0weapon tag\0plain\0center print own\0center print + place\0" },
		{ "cg_msgHeight", "message lines", k_int, 0, 8 },
		{ "cg_msgTime", "message time ms", k_int, 0, 20000 },
		{ "cg_msgSize", "message size", k_int, 8, 16 },
		{ "cg_chatHeight", "chat lines", k_int, 0, 8 },
		{ "cg_chatTime", "chat time ms", k_int, 0, 20000 },
		{ "cg_chatSize", "chat size", k_int, 8, 16 },
		{ "cg_chatOrder", "chat box", k_combo, 1, 2, "fixed\0fit lines\0" },
		{ "cg_chatBG", "chat background", k_combo, 0, 3, "off\0light\0dark\0by speed\0" },
		{ "cg_chatSound", "chat sound", k_combo, 0, 2, "off\0votes\0chat + votes\0" },
		{ "cg_teamChatsOnly", "team chats only", k_bool, 0, 1 },
		{ "cg_centertime", "center print seconds", k_float, 0, 10 },
		{ "cg_cleanFollow", "clean follow", k_bool, 0, 1 },
		{ "cg_hitsound", "hit sound", k_bool, 0, 1 },
		{ "cg_killsound", "kill sound", k_bool, 0, 1 },
		{ "cg_skinAlly", "ally skin", k_combo, 0, 14, k_skins },
		{ "cg_skinEnemy", "enemy skin", k_combo, 0, 14, k_skins },
		{ "cg_skinFixed", "fixed skins (ally = blue)", k_bool, 0, 1 },
		{ "cg_instagib", "instagib", k_bool, 0, 1 },
	};
	for ( const option_t& o : k_options ) {
		const std::string id = std::string( o.m_label ) + "##" + o.m_cvar;
		switch ( o.m_kind ) {
		case k_bool: {
			bool b = s_cv.i( o.m_cvar ) != 0;
			if ( ImGui::Checkbox( id.c_str( ), &b ) )
				save_cvar( o.m_cvar, b ? "1" : "0" );
			break;
		}
		case k_int: {
			int v = s_cv.i( o.m_cvar );
			if ( ImGui::SliderInt( id.c_str( ), &v, static_cast< int >( o.m_min ), static_cast< int >( o.m_max ) ) )
				save_cvar( o.m_cvar, std::to_string( v ) );
			break;
		}
		case k_combo: {
			int v = std::clamp( s_cv.i( o.m_cvar ), static_cast< int >( o.m_min ), static_cast< int >( o.m_max ) ) - static_cast< int >( o.m_min );
			if ( ImGui::Combo( id.c_str( ), &v, o.m_items ) )
				save_cvar( o.m_cvar, std::to_string( v + static_cast< int >( o.m_min ) ) );
			break;
		}
		case k_float: {
			float v = s_cv.f( o.m_cvar );
			if ( ImGui::SliderFloat( id.c_str( ), &v, o.m_min, o.m_max, "%.2f" ) )
				save_cvar( o.m_cvar, va( "%.2f", v ) );
			break;
		}
		case k_color: {
			float c[ 4 ];
			s_cv.rgba( o.m_cvar, c );
			if ( ImGui::ColorEdit4( id.c_str( ), c, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar ) )
				save_cvar( o.m_cvar, va( "%g %g %g %g", c[ 0 ], c[ 1 ], c[ 2 ], c[ 3 ] ) );
			break;
		}
		}
	}
	if ( ImGui::Button( "defaults##urt", ImVec2( -1.f, 15.f ) ) )
		GET_VARIABLE( g_variables.m_urt_hud_cvars, std::string ).clear( );
}

// +maptoggle (sub_19309): flips cg_maptoggle. render-thread cvars, so the flip goes through the saved string next frame
bool n_urt_hud::on_key( unsigned int msg, unsigned int vk )
{
	if ( msg != WM_KEYDOWN || !n_urt_hud::enabled( ) )
		return false;
	const int key = GET_VARIABLE( g_variables.m_urt_hud_map_key, key_bind_t ).m_key;
	if ( key <= 0 || static_cast< int >( vk ) != key || g_menu.m_opened )
		return false;
	s_map_flip = true;
	return false;
}
