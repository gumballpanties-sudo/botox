#include "chud_hud.h"
#include "chud_hud_js.h"
#include "../../globals/config/config.h"
#include "../../globals/logger/logger.h"
#include "../entity_cache/entity_cache.h"
#include "../menu/menu.h"
#include "../misc/scaleform/scaleform.h"
#include "../misc/misc.h"

extern bool point_menu_is_opened( );

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <functional>
#include <utility>
#include <vector>

#define CHUD_SCHEMA "panorama/layout/hud/hud.xml"

namespace
{
	void replace_all( std::string& s, const std::string& find, const std::string& sub )
	{
		for ( size_t pos = 0;; pos += sub.length( ) ) {
			pos = s.find( find, pos );
			if ( pos == std::string::npos )
				break;
			s.erase( pos, find.length( ) );
			s.insert( pos, sub );
		}
	}

	std::string hex( const c_color& color )
	{
		char buf[ 16 ]{ };
		sprintf_s( buf, "#%02x%02x%02x%02x", static_cast< unsigned int >( color[ 0 ] ), static_cast< unsigned int >( color[ 1 ] ),
		           static_cast< unsigned int >( color[ 2 ] ), static_cast< unsigned int >( color[ 3 ] ) );
		return std::string( buf );
	}

	// panorama string literals must not be broken by quotes coming out of the game
	std::string escape_js( const std::string& in )
	{
		std::string out;
		out.reserve( in.length( ) + 8 );

		for ( char c : in ) {
			if ( c == '\'' || c == '\\' )
				out.push_back( '\\' );
			if ( c == '\n' || c == '\r' )
				continue;
			out.push_back( c );
		}

		return out;
	}

	const char* font_name( int index )
	{
		static const char* fonts[] = { "Stratum2",
		                               "Stratum2 Bold",
		                               "Stratum2 Black",
		                               "Stratum2 Light",
		                               "Stratum2 Medium",
		                               "Stratum2 Condensed",
		                               "Stratum2 Mono",
		                               "Stratum2 Bold Monodigit",
		                               "Arial",
		                               "Arial Black",
		                               "Verdana",
		                               "Tahoma",
		                               "Consolas",
		                               "Courier New",
		                               "Segoe UI",
		                               "Trebuchet MS",
		                               "Georgia",
		                               "Times New Roman",
		                               "Impact",
		                               "Comic Sans MS" };

		return fonts[ std::clamp( index, 0, static_cast< int >( ( sizeof( fonts ) / sizeof( fonts[ 0 ] ) ) - 1 ) ) ];
	}

	std::string active_font_raw( )
	{
		const auto& custom = GET_VARIABLE( g_variables.m_chud_hud_font_custom, std::string );
		if ( !custom.empty( ) )
			return custom;

		return font_name( GET_VARIABLE( g_variables.m_chud_hud_font, int ) );
	}

	std::string active_font( )
	{
		return "\"" + escape_js( active_font_raw( ) ) + "\"";
	}

	std::string font_nudge( )
	{
		const auto x = GET_VARIABLE( g_variables.m_chud_hud_font_nudge_x, float );
		const auto y = GET_VARIABLE( g_variables.m_chud_hud_font_nudge_y, float );

		if ( x == 0.f && y == 0.f )
			return "";

		char buf[ 96 ]{ };
		sprintf_s( buf, "transform: translate3d(%.0fpx, %.0fpx, 0px);", x, y );
		return buf;
	}

	std::string shadow_color( )
	{
		return hex( GET_VARIABLE( g_variables.m_chud_hud_shadow_color, c_color ) );
	}

	std::string text_shadow( )
	{
		if ( !GET_VARIABLE( g_variables.m_chud_hud_shadow, bool ) )
			return "";

		char buf[ 128 ]{ };
		sprintf_s( buf, "text-shadow: %.0fpx %.0fpx %.0fpx 1.0 %s;", GET_VARIABLE( g_variables.m_chud_hud_shadow_x, float ),
		           GET_VARIABLE( g_variables.m_chud_hud_shadow_y, float ), GET_VARIABLE( g_variables.m_chud_hud_shadow_blur, float ),
		           shadow_color( ).c_str( ) );
		return buf;
	}

	std::string img_shadow( )
	{
		if ( !GET_VARIABLE( g_variables.m_chud_hud_shadow, bool ) )
			return "";

		char buf[ 128 ]{ };
		sprintf_s( buf, "img-shadow: %.0fpx %.0fpx %.0fpx 1.0 %s;", GET_VARIABLE( g_variables.m_chud_hud_shadow_x, float ),
		           GET_VARIABLE( g_variables.m_chud_hud_shadow_y, float ), GET_VARIABLE( g_variables.m_chud_hud_shadow_blur, float ),
		           shadow_color( ).c_str( ) );
		return buf;
	}

	std::string box_shadow( )
	{
		if ( !GET_VARIABLE( g_variables.m_chud_hud_shadow, bool ) )
			return "";

		char buf[ 128 ]{ };
		sprintf_s( buf, "box-shadow: %s %.0fpx %.0fpx %.0fpx %.0fpx;", shadow_color( ).c_str( ),
		           GET_VARIABLE( g_variables.m_chud_hud_shadow_x, float ), GET_VARIABLE( g_variables.m_chud_hud_shadow_y, float ),
		           GET_VARIABLE( g_variables.m_chud_hud_shadow_blur, float ), GET_VARIABLE( g_variables.m_chud_hud_shadow_spread, float ) );
		return buf;
	}

	std::string font_px( int base )
	{
		const auto scale = GET_VARIABLE( g_variables.m_chud_hud_font_scale, float );

		int px = static_cast< int >( base * scale / 100.f + 0.5f );
		if ( px < 1 )
			px = 1;

		return std::to_string( px );
	}

	bool is_shipped_family( const std::string& family )
	{
		std::string lower = family;
		std::transform( lower.begin( ), lower.end( ), lower.begin( ),
		                []( unsigned char c ) { return static_cast< char >( std::tolower( c ) ); } );

		return lower.rfind( "stratum2", 0 ) == 0 || lower.rfind( "noto", 0 ) == 0 || lower == "symbols";
	}

	std::vector< std::string > font_files_for_family( const std::string& family )
	{
		if ( family.empty( ) || is_shipped_family( family ) )
			return { };

		std::string lower_family = family;
		std::transform( lower_family.begin( ), lower_family.end( ), lower_family.begin( ),
		                []( unsigned char c ) { return static_cast< char >( std::tolower( c ) ); } );

		std::vector< std::string > result;

		for ( const auto root : { HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER } ) {
		HKEY key = nullptr;
		if ( RegOpenKeyExA( root, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Fonts", 0, KEY_READ, &key ) != ERROR_SUCCESS )
			continue;

		for ( DWORD index = 0;; ++index ) {
			char name[ 512 ]{ };
			BYTE data[ 512 ]{ };
			DWORD name_size = sizeof( name ), data_size = sizeof( data ), type = 0;

			if ( RegEnumValueA( key, index, name, &name_size, nullptr, &type, data, &data_size ) != ERROR_SUCCESS )
				break;

			if ( type != REG_SZ )
				continue;

			std::string entry = name;

			const auto paren = entry.find( " (" );
			if ( paren != std::string::npos )
				entry.erase( paren );

			std::transform( entry.begin( ), entry.end( ), entry.begin( ),
			                []( unsigned char c ) { return static_cast< char >( std::tolower( c ) ); } );

			bool match = false;
			for ( size_t start = 0; start <= entry.length( ) && !match; ) {
				const auto amp   = entry.find( " & ", start );
				const std::string part = entry.substr( start, amp == std::string::npos ? std::string::npos : amp - start );

				match = ( part == lower_family || part.rfind( lower_family + " ", 0 ) == 0 );

				if ( amp == std::string::npos )
					break;

				start = amp + 3;
			}

			if ( !match )
				continue;

			const std::string value = reinterpret_cast< const char* >( data );

			for ( size_t start = 0; start <= value.length( ); ) {
				const auto comma = value.find( ',', start );
				std::string file = value.substr( start, comma == std::string::npos ? std::string::npos : comma - start );

				const auto slash = file.find_last_of( "\\/" );
				if ( slash != std::string::npos )
					file.erase( 0, slash + 1 );

				while ( !file.empty( ) && file.front( ) == ' ' )
					file.erase( 0, 1 );

				if ( !file.empty( ) && std::find( result.begin( ), result.end( ), file ) == result.end( ) )
					result.push_back( file );

				if ( comma == std::string::npos )
					break;

				start = comma + 1;
			}
		}

		RegCloseKey( key );
		}

		return result;
	}

	constexpr int weapon_slot_grenade = 3;

	const std::pair< const char*, const char* >* knife_look( short def )
	{
		static const std::vector< std::pair< short, std::pair< const char*, const char* > > > table = {
			{ weapon_knife,                 { "knife",                "knife" } },
			{ weapon_knife_t,               { "knife_t",              "knife" } },
			{ weapon_knife_gg,              { "knife",                "golden knife" } },
			{ weapon_knife_bayonet,         { "bayonet",              "bayonet" } },
			{ weapon_knife_css,             { "knife_css",            "classic knife" } },
			{ weapon_knife_flip,            { "knife_flip",           "flip knife" } },
			{ weapon_knife_gut,             { "knife_gut",            "gut knife" } },
			{ weapon_knife_karambit,        { "knife_karambit",       "karambit" } },
			{ weapon_knife_m9_bayonet,      { "knife_m9_bayonet",     "m9 bayonet" } },
			{ weapon_knife_tactical,        { "knife_tactical",       "huntsman knife" } },
			{ weapon_knife_falchion,        { "knife_falchion",       "falchion knife" } },
			{ weapon_knife_survival_bowie,  { "knife_survival_bowie", "bowie knife" } },
			{ weapon_knife_butterfly,       { "knife_butterfly",      "butterfly knife" } },
			{ weapon_knife_push,            { "knife_push",           "shadow daggers" } },
			{ weapon_knife_cord,            { "knife_cord",           "paracord knife" } },
			{ weapon_knife_canis,           { "knife_canis",          "survival knife" } },
			{ weapon_knife_ursus,           { "knife_ursus",          "ursus knife" } },
			{ weapon_knife_gypsy_jackknife, { "knife_gypsy_jackknife","navaja knife" } },
			{ weapon_knife_outdoor,         { "knife_outdoor",        "nomad knife" } },
			{ weapon_knife_stiletto,        { "knife_stiletto",       "stiletto knife" } },
			{ weapon_knife_widowmaker,      { "knife_widowmaker",     "talon knife" } },
			{ weapon_knife_skeleton,        { "knife_skeleton",       "skeleton knife" } },
		};

		for ( const auto& [ index, look ] : table ) {
			if ( index == def )
				return &look;
		}

		return nullptr;
	}

	int killfeed_icon_width( const std::string& weapon )
	{
		static const std::vector< std::pair< std::vector< std::string >, int > > table = {
			{ { "hegrenade", "flashbang", "smokegrenade", "molotov", "incgrenade", "decoy", "firebomb", "diversion" }, 34 },
			{ { "knife", "bayonet", "knife_t", "knife_karambit", "knife_m9_bayonet", "knife_butterfly", "knife_flip", "knife_gut",
			    "knife_tactical", "knife_falchion", "knife_survival_bowie", "knife_push", "knife_ursus", "knife_gypsy_jackknife",
			    "knife_stiletto", "knife_widowmaker", "knife_css", "knife_cord", "knife_canis", "knife_outdoor", "knife_skeleton", "taser" },
			  58 },
			{ { "glock", "usp_silencer", "hkp2000", "p250", "fiveseven", "tec9", "cz75a", "elite", "deagle", "revolver" }, 52 },
			{ { "mac10", "mp9", "mp7", "mp5sd", "ump45", "p90", "bizon" }, 66 },
			{ { "nova", "xm1014", "mag7", "sawedoff" }, 72 },
			{ { "ak47", "m4a1", "m4a1_silencer", "famas", "galilar", "aug", "sg556" }, 84 },
			{ { "awp", "ssg08", "scar20", "g3sg1", "m249", "negev" }, 96 },
		};

		for ( const auto& [ names, width ] : table ) {
			for ( const auto& name : names ) {
				if ( weapon == name )
					return width;
			}
		}

		return 70;
	}
}

void n_chud::impl_t::run( const std::string& js )
{
	if ( !m_uiengine || !m_hud_panel )
		return;

	m_uiengine->run_script( m_hud_panel, js.c_str( ), CHUD_SCHEMA, 8, 10, false, false );
}

namespace
{
	std::string fonts_conf_path( )
	{
		char exe_path[ MAX_PATH ]{ };
		if ( !GetModuleFileNameA( nullptr, exe_path, MAX_PATH ) )
			return { };

		std::string root = exe_path;
		const auto slash = root.find_last_of( "\\/" );
		if ( slash == std::string::npos )
			return { };

		root.erase( slash );
		return root + "\\csgo\\panorama\\fonts\\fonts.conf";
	}

	void clear_font_cache( )
	{
		char temp_path[ MAX_PATH ]{ };
		if ( !GetTempPathA( MAX_PATH, temp_path ) )
			return;

		const std::string cache_dir = std::string( temp_path ) + "fontconfig\\cache\\";

		WIN32_FIND_DATAA find{ };
		HANDLE handle = FindFirstFileA( ( cache_dir + "*.cache-*" ).c_str( ), &find );
		if ( handle == INVALID_HANDLE_VALUE )
			return;

		int removed = 0;
		do {
			if ( find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY )
				continue;

			if ( DeleteFileA( ( cache_dir + find.cFileName ).c_str( ) ) )
				++removed;
		} while ( FindNextFileA( handle, &find ) );

		FindClose( handle );

		g_logger.print( "cleared " + std::to_string( removed ) + " font cache files\n", "[chud]" );
	}

	bool read_fonts_conf( std::string& conf, std::string& path )
	{
		path = fonts_conf_path( );
		if ( path.empty( ) ) {
			g_logger.print( "could not resolve game path\n", "[chud]" );
			return false;
		}

		std::ifstream in( path );
		if ( !in.is_open( ) ) {
			g_logger.print( "fonts.conf not found at " + path + "\n", "[chud]" );
			return false;
		}

		conf.assign( ( std::istreambuf_iterator< char >( in ) ), std::istreambuf_iterator< char >( ) );
		return true;
	}

	bool write_fonts_conf( const std::string& path, const std::string& conf, const std::string& original )
	{
		{
			std::ifstream existing( path + ".chud_bak" );
			if ( !existing.is_open( ) ) {
				std::ofstream backup( path + ".chud_bak", std::ios::trunc );
				if ( backup.is_open( ) )
					backup << original;
			}
		}

		std::ofstream out( path, std::ios::trunc );
		if ( !out.is_open( ) ) {
			g_logger.print( "fonts.conf not writable\n", "[chud]" );
			return false;
		}

		out << conf;
		out.close( );

		clear_font_cache( );
		return true;
	}

	bool whitelist_family( std::string& conf, const std::string& family, bool& files_found )
	{
		// exact file names only: a blanket `.ttf` pattern crashes csgo on startup
		const auto files = font_files_for_family( family );
		files_found      = !files.empty( );

		std::string additions;
		for ( const auto& file : files ) {
			const std::string entry = "<fontpattern>" + file + "</fontpattern>";
			if ( conf.find( entry ) == std::string::npos )
				additions += "\t" + entry + "\n";
		}

		bool changed = false;

		char user_fonts[ MAX_PATH ]{ };
		if ( ExpandEnvironmentStringsA( "%LOCALAPPDATA%\\Microsoft\\Windows\\Fonts", user_fonts, MAX_PATH ) ) {
			const std::string dir_entry = std::string( "<dir>" ) + user_fonts + "</dir>";

			if ( conf.find( dir_entry ) == std::string::npos ) {
				const auto anchor = conf.find( "<fontpattern>" );
				if ( anchor != std::string::npos ) {
					conf.insert( anchor, dir_entry + "\n\n\t" );
					changed = true;
				}
			}
		}

		if ( additions.empty( ) )
			return changed;

		auto insert_at = conf.rfind( "</fontpattern>" );
		if ( insert_at == std::string::npos ) {
			insert_at = conf.rfind( "</fontconfig>" );
			if ( insert_at == std::string::npos ) {
				g_logger.print( "fonts.conf format not understood\n", "[chud]" );
				return changed;
			}
		}
		else {
			insert_at += strlen( "</fontpattern>" );
			if ( insert_at < conf.length( ) && conf[ insert_at ] == '\n' )
				++insert_at;
		}

		conf.insert( insert_at, additions );
		return true;
	}

	const char* DEF_HUD_MARK_OPEN  = "<!-- botox def hud font -->";
	const char* DEF_HUD_MARK_CLOSE = "<!-- /botox def hud font -->";

	std::string xml_escape( const std::string& text )
	{
		std::string out;
		for ( const char c : text ) {
			if ( c == '&' )
				out += "&amp;";
			else if ( c == '<' )
				out += "&lt;";
			else if ( c == '>' )
				out += "&gt;";
			else
				out += c;
		}

		return out;
	}

	bool strip_def_hud_alias( std::string& conf )
	{
		auto open = conf.find( DEF_HUD_MARK_OPEN );
		if ( open == std::string::npos )
			return false;

		while ( open > 0 && ( conf[ open - 1 ] == '\t' || conf[ open - 1 ] == ' ' ) )
			--open;

		auto close = conf.find( DEF_HUD_MARK_CLOSE, open );
		if ( close == std::string::npos )
			return false;

		close += strlen( DEF_HUD_MARK_CLOSE );
		while ( close < conf.length( ) && ( conf[ close ] == '\n' || conf[ close ] == '\r' ) )
			++close;

		conf.erase( open, close - open );
		return true;
	}

	std::string def_hud_alias_block( const std::string& family )
	{
		return "\t" + std::string( DEF_HUD_MARK_OPEN ) +
		       "\n"
		       "\t<match target=\"pattern\">\n"
		       "\t\t<test name=\"family\" compare=\"contains\">\n"
		       "\t\t\t<string>Stratum2</string>\n"
		       "\t\t</test>\n"
		       "\t\t<edit name=\"family\" mode=\"prepend\" binding=\"strong\">\n"
		       "\t\t\t<string>" +
		       xml_escape( family ) +
		       "</string>\n"
		       "\t\t</edit>\n"
		       "\t</match>\n\t" +
		       DEF_HUD_MARK_CLOSE + "\n\n";
	}
}

const std::vector< std::string >& n_chud::impl_t::system_fonts( )
{
	if ( !m_system_fonts.empty( ) )
		return m_system_fonts;

	m_system_fonts = m_panorama_fonts;

	std::vector< std::string > families;

	for ( const auto root : { HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER } ) {
	HKEY key = nullptr;
	if ( RegOpenKeyExA( root, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Fonts", 0, KEY_READ, &key ) != ERROR_SUCCESS )
		continue;

	for ( DWORD index = 0;; ++index ) {
		char name[ 512 ]{ };
		DWORD name_size = sizeof( name ), type = 0;

		if ( RegEnumValueA( key, index, name, &name_size, nullptr, &type, nullptr, nullptr ) != ERROR_SUCCESS )
			break;

		std::string entry = name;

		const auto paren = entry.find( " (" );
		if ( paren != std::string::npos )
			entry.erase( paren );

		static const char* styles[] = { " Bold Italic", " Bold Oblique", " Italic", " Oblique", " Bold", " Regular" };

		bool styled = false;
		for ( const auto* style : styles ) {
			const auto length = strlen( style );
			if ( entry.length( ) > length && entry.compare( entry.length( ) - length, length, style ) == 0 ) {
				styled = true;
				break;
			}
		}

		if ( styled || entry.empty( ) )
			continue;

		for ( size_t start = 0; start <= entry.length( ); ) {
			const auto amp  = entry.find( " & ", start );
			std::string part = entry.substr( start, amp == std::string::npos ? std::string::npos : amp - start );

			if ( !part.empty( ) && std::find( families.begin( ), families.end( ), part ) == families.end( ) )
				families.push_back( part );

			if ( amp == std::string::npos )
				break;

			start = amp + 3;
		}
	}

	RegCloseKey( key );
	}

	std::sort( families.begin( ), families.end( ) );
	m_system_fonts.insert( m_system_fonts.end( ), families.begin( ), families.end( ) );

	return m_system_fonts;
}

// undo button: a bad fonts.conf crashes the whole game
void n_chud::impl_t::restore_fonts_conf( )
{
	const std::string conf_path = fonts_conf_path( );
	if ( conf_path.empty( ) ) {
		g_logger.print( "could not resolve game path\n", "[chud]" );
		return;
	}

	std::ifstream backup( conf_path + ".chud_bak", std::ios::binary );
	if ( !backup.is_open( ) ) {
		g_logger.print( "no fonts.conf backup to restore\n", "[chud]" );
		return;
	}

	const std::string original( ( std::istreambuf_iterator< char >( backup ) ), std::istreambuf_iterator< char >( ) );

	std::ofstream out( conf_path, std::ios::trunc | std::ios::binary );
	if ( !out.is_open( ) ) {
		g_logger.print( "fonts.conf not writable\n", "[chud]" );
		return;
	}

	out << original;
	out.close( );

	clear_font_cache( );
	g_logger.print( "stock fonts.conf restored\n", "[chud]" );
}

void n_chud::impl_t::register_system_fonts( )
{
	std::string conf_path, original;
	if ( !read_fonts_conf( original, conf_path ) )
		return;

	const std::string family = active_font_raw( );

	std::string conf = original;
	bool files_found = false;

	const bool changed = whitelist_family( conf, family, files_found );

	if ( !files_found ) {
		g_logger.print( "no font file found for \"" + family + "\" - check the spelling\n", "[chud]" );
		return;
	}

	if ( !changed ) {
		g_logger.print( "\"" + family + "\" already registered, clearing cache instead\n", "[chud]" );
		clear_font_cache( );
		return;
	}

	if ( write_fonts_conf( conf_path, conf, original ) )
		g_logger.print( "registered \"" + family + "\" - restart csgo\n", "[chud]" );
}

void n_chud::impl_t::register_def_hud_font( )
{
	const std::string family = GET_VARIABLE( g_variables.m_def_hud_font, std::string );
	if ( family.empty( ) ) {
		g_logger.print( "pick a font family first\n", "[chud]" );
		return;
	}

	std::string conf_path, original;
	if ( !read_fonts_conf( original, conf_path ) )
		return;

	std::string conf = original;
	bool files_found = false;

	whitelist_family( conf, family, files_found );

	if ( !files_found && !is_shipped_family( family ) ) {
		g_logger.print( "no font file found for \"" + family + "\" - check the spelling\n", "[chud]" );
		return;
	}

	// replace our block, never stack another
	strip_def_hud_alias( conf );

	const auto insert_at = conf.rfind( "</fontconfig>" );
	if ( insert_at == std::string::npos ) {
		g_logger.print( "fonts.conf format not understood\n", "[chud]" );
		return;
	}

	conf.insert( insert_at, def_hud_alias_block( family ) );

	if ( write_fonts_conf( conf_path, conf, original ) )
		g_logger.print( "default hud font -> \"" + family + "\" - restart csgo\n", "[chud]" );
}

void n_chud::impl_t::clear_def_hud_font( )
{
	std::string conf_path, original;
	if ( !read_fonts_conf( original, conf_path ) )
		return;

	std::string conf = original;
	if ( !strip_def_hud_alias( conf ) ) {
		g_logger.print( "default hud font was never changed\n", "[chud]" );
		return;
	}

	if ( write_fonts_conf( conf_path, conf, original ) )
		g_logger.print( "default hud font back to stratum2 - restart csgo\n", "[chud]" );
}

static unsigned int color_key( )
{
	unsigned int hash = 2166136261u;

	const auto mix = [ & ]( const c_color& color ) {
		for ( int i = 0; i < 4; ++i ) {
			hash ^= static_cast< unsigned int >( color[ i ] );
			hash *= 16777619u;
		}
	};

	mix( GET_VARIABLE( g_variables.m_chud_hud_color, c_color ) );
	mix( GET_VARIABLE( g_variables.m_chud_hud_bg, c_color ) );
	mix( GET_VARIABLE( g_variables.m_chud_hud_secondary, c_color ) );

	return hash;
}

static std::string panel_bg_style( )
{
	if ( GET_VARIABLE( g_variables.m_chud_hud_watermark_bg, bool ) )
		return "background-color: #191919ff; border: 1px solid #323232ff; border-radius: 4px;";

	return "background-color: " + hex( GET_VARIABLE( g_variables.m_chud_hud_bg, c_color ) ) + "; border-radius: 8px;";
}

void n_chud::impl_t::hide_stock( )
{
	std::string js = k_hide_run;
	replace_all( js, "${hideFn}", k_hide_fn );
	run( js );
}

void n_chud::impl_t::expire_rows( )
{
	const float now = g_interfaces.m_global_vars_base->m_current_time;

	const bool keep_on = GET_VARIABLE( g_variables.m_keep_killfeed, bool );

	const auto drain = [ & ]( std::deque< row_t >& rows, const char* prefix, float lifetime, size_t max_rows ) {
		const auto expire = [ & ]( std::deque< row_t >::iterator it ) {
			std::string js = k_expire_row;
			replace_all( js, "${rowName}", std::string( prefix ) + std::to_string( it->m_id ) );
			run( js );

			return rows.erase( it );
		};

		for ( auto it = rows.begin( ); it != rows.end( ); ) {
			if ( !( keep_on && it->m_keep ) && now - it->m_death_time >= lifetime )
				it = expire( it );
			else
				++it;
		}

		while ( rows.size( ) > max_rows ) {
			auto victim = std::find_if( rows.begin( ), rows.end( ), [ & ]( const row_t& row ) { return !( keep_on && row.m_keep ); } );
			expire( victim != rows.end( ) ? victim : rows.begin( ) );
		}
	};

	drain( m_kill_rows, "ChudKill", 10.f, 6 );
	drain( m_chat_rows, "ChudChatLine", 14.f, 8 );
}

void n_chud::impl_t::install( )
{
	chat_reset( );

	const auto fg    = GET_VARIABLE( g_variables.m_chud_hud_color, c_color );
	const auto bg    = GET_VARIABLE( g_variables.m_chud_hud_bg, c_color );
	const auto scale = GET_VARIABLE( g_variables.m_chud_hud_scale, float );

	char scale_buf[ 16 ]{ };
	sprintf_s( scale_buf, "%.0f", scale );

	std::string js = std::string( k_install ) + k_install_2 + k_install_3 + k_install_4;
	replace_all( js, "${hideFn}", k_hide_fn );
	replace_all( js, "${fg}", hex( fg ) );
	replace_all( js, "${bg}", hex( bg ) );
	replace_all( js, "${sec}", hex( GET_VARIABLE( g_variables.m_chud_hud_secondary, c_color ) ) );
	replace_all( js, "${watermark}", GET_VARIABLE( g_variables.m_chud_hud_watermark_bg, bool ) ? "true" : "false" );
	replace_all( js, "${scale}", scale_buf );

	char pad_buf[ 16 ]{ };
	sprintf_s( pad_buf, "%.0f", GET_VARIABLE( g_variables.m_chud_hud_padding, float ) );
	replace_all( js, "${pad}", pad_buf );
	replace_all( js, "${blur}", GET_VARIABLE( g_variables.m_chud_hud_blur, bool ) ? "true" : "false" );
	replace_all( js, "${specAvatars}", GET_VARIABLE( g_variables.m_chud_hud_spectator_avatars, bool ) ? "true" : "false" );
	replace_all( js, "${specMaxW}", std::to_string( GET_VARIABLE( g_variables.m_chud_hud_spectator_max_width, int ) ) );
	replace_all( js, "${font}", active_font( ) );
	replace_all( js, "${nudge}", font_nudge( ) );
	replace_all( js, "${textShadow}", text_shadow( ) );
	replace_all( js, "${imgShadow}", img_shadow( ) );
	replace_all( js, "${boxShadow}", box_shadow( ) );

	char font_scale_buf[ 16 ]{ };
	sprintf_s( font_scale_buf, "%.0f", GET_VARIABLE( g_variables.m_chud_hud_font_scale, float ) );
	replace_all( js, "${fontScale}", font_scale_buf );
	run( js );

	m_old_health   = -1;
	m_old_armor    = -1;
	m_old_clip     = -1;
	m_old_reserve  = -1;
	m_old_max_clip = -1;
	m_old_weapons.clear( );
	m_old_timer.clear( );
	m_old_active_row = -1;
	m_old_spectators.clear( );
	m_old_killfeed_top = -1;

	m_inited = true;
	m_built  = true;
}

void n_chud::impl_t::uninstall( )
{
	std::string js = k_uninstall;
	replace_all( js, "${hideFn}", k_hide_fn );
	run( js );

	m_built  = false;
	m_inited = false;

	g_scaleform.m_should_force_update = true;
}

void n_chud::impl_t::push_colors( )
{
	std::string js = k_set_colors;
	replace_all( js, "${fg}", hex( GET_VARIABLE( g_variables.m_chud_hud_color, c_color ) ) );
	replace_all( js, "${bg}", hex( GET_VARIABLE( g_variables.m_chud_hud_bg, c_color ) ) );
	replace_all( js, "${sec}", hex( GET_VARIABLE( g_variables.m_chud_hud_secondary, c_color ) ) );
	replace_all( js, "${watermark}", GET_VARIABLE( g_variables.m_chud_hud_watermark_bg, bool ) ? "true" : "false" );
	run( js );
}

void n_chud::impl_t::tick( )
{
	const auto fg   = hex( GET_VARIABLE( g_variables.m_chud_hud_color, c_color ) );
	const auto bg   = hex( GET_VARIABLE( g_variables.m_chud_hud_bg, c_color ) );
	const auto font = active_font( );
	const auto sec  = hex( GET_VARIABLE( g_variables.m_chud_hud_secondary, c_color ) );
	const auto panel_bg = panel_bg_style( );
	const auto nudge    = font_nudge( );
	const auto fs17     = font_px( 17 );
	const auto fs18     = font_px( 18 );
	const auto tshadow  = text_shadow( );
	const auto ishadow  = img_shadow( );

	char nudge_x[ 16 ]{ }, nudge_y[ 16 ]{ };
	sprintf_s( nudge_x, "%.0f", GET_VARIABLE( g_variables.m_chud_hud_font_nudge_x, float ) );
	sprintf_s( nudge_y, "%.0f", GET_VARIABLE( g_variables.m_chud_hud_font_nudge_y, float ) );

	const float now            = g_interfaces.m_global_vars_base->m_current_time;

	const bool alive = g_ctx.m_local && g_ctx.m_local->is_alive( );

	if ( alive ) {
		const int health = std::clamp( g_ctx.m_local->get_health( ), 0, 100 );
		if ( health != m_old_health ) {
			char hp[ 16 ]{ }, frac[ 16 ]{ };
			sprintf_s( hp, "%d", health );
			sprintf_s( frac, "%d", health );

			std::string js = k_set_health;
			replace_all( js, "${hp}", hp );
			replace_all( js, "${hpFrac}", frac );
			run( js );

			m_old_health = health;
		}

		const auto weapon_handle = g_ctx.m_local->get_active_weapon_handle( );
		if ( weapon_handle ) {
			const auto weapon =
				reinterpret_cast< c_base_entity* >( g_interfaces.m_client_entity_list->get_client_entity_from_handle( weapon_handle ) );
			if ( weapon ) {
				const auto data = g_interfaces.m_weapon_system->get_weapon_data( weapon->get_item_definition_index( ) );

				const int clip     = weapon->get_ammo( );
				const int reserve  = weapon->get_ammo_reserve( );
				const int max_clip = ( data && data->m_max_clip1 > 0 ) ? data->m_max_clip1 : 0;

				if ( clip != m_old_clip || reserve != m_old_reserve || max_clip != m_old_max_clip ) {
					const bool has_ammo = ( max_clip > 0 && clip >= 0 );

					char ammo[ 32 ]{ }, frac[ 16 ]{ };
					if ( has_ammo ) {
						sprintf_s( ammo, "%d/%d", clip, reserve );
						sprintf_s( frac, "%d", std::clamp( clip * 100 / max_clip, 0, 100 ) );
					}
					else {
						sprintf_s( ammo, " " );
						sprintf_s( frac, "0" );
					}

					std::string js = k_set_ammo;
					replace_all( js, "${ammo}", ammo );
					replace_all( js, "${ammoFrac}", frac );
					replace_all( js, "${hasAmmo}", has_ammo ? "true" : "false" );
					run( js );

					m_old_clip     = clip;
					m_old_reserve  = reserve;
					m_old_max_clip = max_clip;
				}
			}
		}

		struct entry_t {
			int m_slot{ };
			std::string m_icon{ }, m_name{ };
			bool m_active{ };
		};

		std::vector< entry_t > entries;

		for ( int i = 0; i < 64; ++i ) {
			const auto handle = g_ctx.m_local->get_weapons_handle( )[ i ];
			if ( !handle || handle == 0xFFFFFFFF )
				continue;

			const auto entity = reinterpret_cast< c_base_entity* >( g_interfaces.m_client_entity_list->get_client_entity_from_handle( handle ) );
			if ( !entity )
				continue;

			const auto definition_index = entity->get_item_definition_index( );

			const auto data = g_interfaces.m_weapon_system->get_weapon_data( definition_index );
			if ( !data || !data->m_weapon_name )
				continue;

			std::string icon = data->m_weapon_name;
			const auto under = icon.find( "weapon_" );
			if ( under != std::string::npos )
				icon = icon.substr( under + 7 );

			std::string name = ( data->m_hud_name && *data->m_hud_name ) ? data->m_hud_name : icon;
			if ( !name.empty( ) && name[ 0 ] == '#' )
				name = icon;

			if ( const auto look = knife_look( definition_index ) ) {
				icon = look->first;
				name = look->second;
			}

			entry_t row;
			row.m_slot   = data->m_slot;
			row.m_icon   = icon;
			row.m_name   = name;
			row.m_active = ( handle == weapon_handle );

			entries.push_back( row );
		}

		std::stable_sort( entries.begin( ), entries.end( ), []( const entry_t& lhs, const entry_t& rhs ) { return lhs.m_slot < rhs.m_slot; } );

		std::string rows     = "[";
		std::string rows_key = "[";

		int active_row     = -1;
		int built_row_index = 0;

		for ( size_t i = 0; i < entries.size( ); ) {
			const auto& entry     = entries[ i ];
			const bool is_grenade = ( entry.m_slot == weapon_slot_grenade );

			std::string icons  = "['" + escape_js( entry.m_icon ) + "'";
			std::string name   = entry.m_name;
			bool active        = entry.m_active;
			size_t consumed    = 1;

			if ( is_grenade ) {
				for ( size_t j = i + 1; j < entries.size( ) && entries[ j ].m_slot == weapon_slot_grenade; ++j ) {
					icons += ",'" + escape_js( entries[ j ].m_icon ) + "'";
					active |= entries[ j ].m_active;
					++consumed;
				}

				if ( consumed > 1 )
					name = "grenades";
			}

			icons += "]";

			if ( rows.length( ) > 1 )
				rows += ",";

			if ( active )
				active_row = entry.m_slot + 1;

			const std::string body = "icons:" + icons + ",name:'" + escape_js( name ) + "',slot:'" + std::to_string( entry.m_slot + 1 ) + "'";

			rows += "{" + body + ",active:" + ( active ? "true" : "false" ) + "}";

			if ( rows_key.length( ) > 1 )
				rows_key += ",";
			rows_key += "{" + body + "}";

			++built_row_index;
			i += consumed;
		}

		rows += "]";
		rows_key += "]";

		if ( rows_key != m_old_weapons ) {
			std::string js = k_set_weapons;
			replace_all( js, "${rows}", rows );
			replace_all( js, "${fg}", fg );
			replace_all( js, "${bg}", bg );
			replace_all( js, "${font}", font );
			replace_all( js, "${sec}", sec );
			replace_all( js, "${panelBg}", panel_bg );
			replace_all( js, "${nudge}", nudge );
			replace_all( js, "${fs17}", fs17 );
			replace_all( js, "${textShadow}", tshadow );
			replace_all( js, "${imgShadow}", ishadow );
			run( js );

			m_old_weapons    = rows_key;
			m_old_active_row = active_row;
		}
		else if ( active_row != m_old_active_row ) {
			std::string js = k_set_active_weapon;
			replace_all( js, "${activeRow}", std::to_string( active_row ) );
			run( js );

			m_old_active_row = active_row;
		}
	}

	{
		std::string list = "[";

		if ( GET_VARIABLE( g_variables.m_chud_hud_spectators, bool ) && g_ctx.m_local &&
		     g_interfaces.m_engine_client->is_in_game( ) ) {
			g_entity_cache.enumerate( e_enumeration_type::type_players, [ & ]( c_base_entity* entity ) {
				if ( !entity || entity->is_alive( ) || entity->is_dormant( ) )
					return;

				const auto target = reinterpret_cast< c_base_entity* >(
					g_interfaces.m_client_entity_list->get_client_entity_from_handle( entity->get_observer_target_handle( ) ) );

				if ( !target || !target->is_alive( ) )
					return;

				player_info_t watcher{ }, watched{ };
				g_interfaces.m_engine_client->get_player_info( entity->get_index( ), &watcher );
				g_interfaces.m_engine_client->get_player_info( target->get_index( ), &watched );

				if ( watcher.m_is_hltv )
					return;

				const std::string text = std::string( watcher.m_name ) + " -> " + std::string( watched.m_name );

				if ( list.length( ) > 1 )
					list += ",";

				const std::string xuid = watcher.m_fake_player ? "0" : std::to_string( watcher.m_ull_xuid );

				const char* team =
					entity->get_team( ) == static_cast< int >( e_team_id::team_ct ) ? "CT" : "TERRORIST";

				list += "{i:" + std::to_string( entity->get_index( ) ) + ",t:'" + escape_js( text ) +
				        "',l:" + ( target == g_ctx.m_local ? "true" : "false" ) + ",s:'" + xuid + "',d:'" + team + "'}";
			} );
		}

		list += "]";

		if ( list != m_old_spectators ) {
			std::string js = k_set_spectators;
			replace_all( js, "${list}", list );
			run( js );

			m_old_spectators = list;
		}
	}

	while ( !m_pending_kills.empty( ) ) {
		const auto kill = m_pending_kills.front( );
		m_pending_kills.pop_front( );

		const int row_id = m_next_row_id++;

		const std::string outline = kill.m_local_involved ? " border: 2px solid " + sec + ";" : "";

		std::string js = k_add_kill;
		replace_all( js, "${rowId}", std::to_string( row_id ) );
		replace_all( js, "${attacker}", escape_js( kill.m_attacker ) );
		replace_all( js, "${victim}", escape_js( kill.m_victim ) );
		replace_all( js, "${weapon}", escape_js( kill.m_weapon ) );
		replace_all( js, "${headshot}", kill.m_headshot ? "true" : "false" );
		replace_all( js, "${hasAttacker}", kill.m_attacker.empty( ) ? "false" : "true" );
		replace_all( js, "${outline}", outline );
		replace_all( js, "${fg}", fg );
		replace_all( js, "${bg}", bg );
		replace_all( js, "${font}", font );
		replace_all( js, "${sec}", sec );
		replace_all( js, "${panelBg}", panel_bg );
		replace_all( js, "${iconWidth}", std::to_string( killfeed_icon_width( kill.m_weapon ) ) );
		replace_all( js, "${nudge}", nudge );
		replace_all( js, "${fs18}", fs18 );
		replace_all( js, "${textShadow}", tshadow );
		replace_all( js, "${imgShadow}", ishadow );
		run( js );

		m_kill_rows.push_back( { row_id, now, kill.m_local_killer && GET_VARIABLE( g_variables.m_keep_killfeed, bool ) } );
	}

	while ( !m_pending_chat.empty( ) ) {
		const auto line = m_pending_chat.front( );
		m_pending_chat.pop_front( );

		if ( line.m_replace ) {
			const auto old = std::find_if( m_chat_rows.begin( ), m_chat_rows.end( ), [ & ]( const row_t& row ) { return row.m_id == m_stack_row; } );
			if ( old != m_chat_rows.end( ) ) {
				std::string js = k_expire_row;
				replace_all( js, "${rowName}", "ChudChatLine" + std::to_string( old->m_id ) );
				run( js );
				m_chat_rows.erase( old );
			}
		}

		const int row_id = m_next_row_id++;
		if ( line.m_stack )
			m_stack_row = row_id;

		std::string js = k_add_chat;
		replace_all( js, "${rowId}", std::to_string( row_id ) );
		replace_all( js, "${text}", escape_js( line.m_text ) );
		replace_all( js, "${fg}", fg );
		replace_all( js, "${sec}", sec );
		replace_all( js, "${font}", font );
		replace_all( js, "${nudgeX}", nudge_x );
		replace_all( js, "${nudgeY}", nudge_y );
		replace_all( js, "${fs18}", fs18 );
		replace_all( js, "${textShadow}", tshadow );
		run( js );

		m_chat_rows.push_back( { row_id, now } );
	}

	expire_rows( );

	if ( now >= m_next_hide_pass ) {
		float bomb_remaining = -1.f;
		bool bomb_defused    = false;

		for ( int i = 1; i <= g_interfaces.m_client_entity_list->get_highest_entity_index( ); ++i ) {
			const auto entity = g_interfaces.m_client_entity_list->get< c_base_entity >( i );
			if ( !entity )
				continue;

			const auto networkable = static_cast< c_client_networkable* >( entity );
			if ( !networkable )
				continue;

			const auto client_class = networkable->get_client_class( );
			if ( !client_class || client_class->m_class_id != e_class_ids::c_planted_c4 )
				continue;

			if ( entity->is_bomb_defused( ) ) {
				bomb_defused = true;
				break;
			}

			if ( !entity->is_bomb_ticking( ) )
				continue;

			bomb_remaining = entity->get_c4_blow_time( ) - now;
			break;
		}

		const bool bomb_override = bomb_defused || bomb_remaining > 0.f;

		char bomb_text[ 32 ]{ };
		if ( bomb_defused )
			sprintf_s( bomb_text, "defused" );
		else if ( bomb_remaining > 0.f )
			sprintf_s( bomb_text, "%.1f", bomb_remaining );

		std::string js = k_set_timer;
		replace_all( js, "${bombOverride}", bomb_override ? "true" : "false" );
		replace_all( js, "${bombText}", bomb_text );
		replace_all( js, "${bombColor}", bomb_defused ? fg : "#ff4040ff" );
		replace_all( js, "${fg}", fg );
		run( js );

		if ( now >= m_next_nuke_pass ) {
			hide_stock( );
			m_next_nuke_pass = now + 0.5f;
		}

		m_next_hide_pass = now + ( bomb_override ? 0.1f : 0.25f );
	}

	{
		const ImVec4 wm   = n_misc::g_watermark_box;
		const bool right  = wm.z > wm.x && wm.z > static_cast< float >( g_ctx.m_width ) * 0.5f;
		const ImVec2 wm_size( wm.z - wm.x, wm.w - wm.y );
		const float wm_bottom = g_render.dpi_panel_pos( ImVec2( wm.x, wm.y ), wm_size ).y + wm_size.y * g_render.m_dpi_panel_scale;
		const int bottom      = right ? static_cast< int >( std::ceil( wm_bottom ) ) : 0;
		const auto now_ms = GetTickCount64( );

		if ( bottom != m_old_killfeed_top || now_ms >= m_next_killfeed_push ) {
			std::string js = k_set_killfeed_top;
			replace_all( js, "${wmBottom}", std::to_string( bottom ) );
			replace_all( js, "${screenH}", std::to_string( std::max( static_cast< int >( g_ctx.m_height ), 1 ) ) );
			run( js );

			m_old_killfeed_top   = bottom;
			m_next_killfeed_push = now_ms + 1000ull;
		}
	}

	{
		const int team = g_ctx.m_local ? static_cast< int >( g_ctx.m_local->get_team( ) ) : -1;

		const bool on_a_team = team == static_cast< int >( e_team_id::team_tt ) || team == static_cast< int >( e_team_id::team_ct );

		if ( team != m_old_team ) {
			if ( on_a_team )
				run( k_release_teamselect_focus );

			m_old_team = team;
		}
	}

	/* chat input line: keys handled in chat_on_key (window proc). here, on the game
	   thread: mirror the buffer into the label and run the say. */

	const bool ui_input = g_menu.m_opened || point_menu_is_opened( );

	bool push_text = false, closing = false, send = false, team = false;
	std::string text{ };

	{
		std::scoped_lock lock( m_chat_mutex );

		m_chat_drain_ms = GetTickCount64( );

		if ( m_chat_stale_close ) {
			m_chat_stale_close = false;
			closing            = !m_chat_open;
		}

		if ( m_chat_open && !m_chat_result && ( ui_input || !g_ctx.m_is_window_focused ) )
			m_chat_result = 2;

		if ( m_chat_open && m_chat_result ) {
			send    = m_chat_result == 1;
			team    = m_chat_team;
			text    = g_utilities.to_utf8( m_chat_input );
			closing = true;

			m_chat_result      = 0;
			m_chat_open        = false;
			m_chat_input_dirty = false;
			m_chat_input.clear( );
		}
		else if ( m_chat_open && m_chat_input_dirty ) {
			push_text          = true;
			team               = m_chat_team;
			text               = g_utilities.to_utf8( m_chat_input );
			m_chat_input_dirty = false;
		}
	}

	if ( push_text ) {
		std::string js = k_chat_set;
		replace_all( js, "${text}", escape_js( ( team ? "team: " : "say: " ) + text + "|" ) );
		run( js );
	}

	if ( closing ) {
		run( k_close_chat );

		if ( send && !text.empty( ) ) {
			std::string message = text;
			std::erase( message, '"' );
			std::erase( message, ';' );

			if ( !message.empty( ) ) {
				const std::string command = std::string( team ? "say_team \"" : "say \"" ) + message + "\"";
				g_interfaces.m_engine_client->client_cmd_unrestricted( command.c_str( ) );
			}
		}
	}
}

// event feeds, called from hooks, may be off the game thread

void n_chud::impl_t::on_player_death( const std::string& attacker, const std::string& victim, const std::string& weapon, bool headshot,
                                      bool local_involved, bool local_killer )
{
	if ( !GET_VARIABLE( g_variables.m_chud_hud, bool ) )
		return;

	kill_t kill;
	kill.m_attacker       = attacker;
	kill.m_victim         = victim;
	kill.m_weapon         = weapon;
	kill.m_headshot       = headshot;
	kill.m_local_involved = local_involved;
	kill.m_local_killer   = local_killer;

	m_pending_kills.push_back( kill );

	while ( m_pending_kills.size( ) > 16 )
		m_pending_kills.pop_front( );
}

void n_chud::impl_t::on_chat( const std::string& text, bool stack, bool replace )
{
	if ( !GET_VARIABLE( g_variables.m_chud_hud, bool ) )
		return;

	m_pending_chat.push_back( { text, stack, replace } );

	while ( m_pending_chat.size( ) > 16 )
		m_pending_chat.pop_front( );
}

bool n_chud::impl_t::chat_reset( )
{
	std::scoped_lock lock( m_chat_mutex );

	const bool was_open = m_chat_open;

	m_chat_open        = false;
	m_chat_result      = 0;
	m_chat_input_dirty = false;
	m_chat_open_char   = 0;
	m_chat_stale_close = false;
	m_chat_input.clear( );

	return was_open;
}

/* chat keyboard, window thread. true = ours, caller drops the msg so the game never
   sees it; panorama never holds the keyboard. mouse left alone on purpose (like csgo). */
bool n_chud::impl_t::chat_on_key( unsigned int msg, unsigned int wide_param, long long_param, bool ui_input )
{
	// no WM_SYSKEY*: alt+f4 / alt+tab must keep working
	if ( msg != WM_KEYDOWN && msg != WM_KEYUP && msg != WM_CHAR )
		return false;

	if ( ui_input || wide_param == VK_INSERT )
		return false;

	std::scoped_lock lock( m_chat_mutex );

	const bool stale = GetTickCount64( ) - m_chat_drain_ms > 1000ull;

	// line up but nothing drains it: drop it, never trap the keyboard
	if ( m_chat_open && stale ) {
		m_chat_open        = false;
		m_chat_result      = 0;
		m_chat_input_dirty = false;
		m_chat_open_char   = 0;
		m_chat_input.clear( );
		m_chat_stale_close = true;
	}

	if ( msg == WM_KEYUP )
		return false;

	if ( !m_chat_open ) {
		if ( msg != WM_KEYDOWN )
			return false;

		if ( stale )
			return false;

		if ( !m_inited || !GET_VARIABLE( g_variables.m_chud_hud, bool ) )
			return false;

		if ( !g_interfaces.m_engine_client || !g_interfaces.m_input_system || !g_interfaces.m_engine_client->is_in_game( ) ||
		     g_interfaces.m_engine_client->is_console_visible( ) )
			return false;

		const char* bind = g_interfaces.m_engine_client->key_binding_for_key( g_interfaces.m_input_system->scan_code_to_button_code( long_param ) );
		if ( !bind || !std::strstr( bind, "messagemode" ) )
			return false;

		m_chat_open        = true;
		m_chat_team        = std::strstr( bind, "messagemode2" ) != nullptr;
		m_chat_result      = 0;
		m_chat_input.clear( );
		m_chat_input_dirty = true;

		m_chat_open_char = static_cast< wchar_t >( std::towlower( MapVirtualKeyW( wide_param, MAPVK_VK_TO_CHAR ) & 0x7FFF ) );
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

		if ( character >= L' ' && character != 0x7F && m_chat_input.size( ) < 127U ) {
			m_chat_input.push_back( character );
			m_chat_input_dirty = true;
		}

		return true;
	}

	switch ( wide_param ) {
	case VK_RETURN:
		m_chat_result = 1;
		break;
	case VK_ESCAPE:
		m_chat_result = 2;
		break;
	case VK_BACK:
		if ( !m_chat_input.empty( ) ) {
			m_chat_input.pop_back( );
			m_chat_input_dirty = true;
		}
		break;
	default:
		break;
	}

	return true;
}

void n_chud::impl_t::on_level_init( )
{
	m_inited    = false;
	m_hud_panel = nullptr;

	m_old_health   = -1;
	m_old_armor    = -1;
	m_old_clip     = -1;
	m_old_reserve  = -1;
	m_old_max_clip = -1;
	m_old_layout     = -1;
	m_pending_layout = -1;
	m_old_colors     = 0u;
	m_old_weapons.clear( );
	m_old_timer.clear( );
	m_old_active_row = -1;
	m_old_spectators.clear( );
	m_old_killfeed_top = -1;

	m_pending_kills.clear( );
	m_pending_chat.clear( );

	m_kill_rows.clear( );
	m_chat_rows.clear( );
	m_next_hide_pass = 0.f;

	chat_reset( );

	m_old_team = -1;
}

bool n_chud::impl_t::resolve_hud( )
{
	if ( !m_uiengine ) {
		m_uiengine = g_interfaces.m_panorama->access_ui_engine( );
		if ( !m_uiengine )
			return false;
	}

	if ( m_hud_panel && m_uiengine->is_valid_panel_ptr( m_hud_panel ) )
		return true;

	m_hud_panel = nullptr;
	m_inited    = false;

	auto itr = m_uiengine->get_last_dispatched_event_target_panel( );
	for ( int guard = 0; itr && guard < 64 && m_uiengine->is_valid_panel_ptr( itr ); ++guard ) {
		if ( HASH_RT( itr->get_id( ) ) == HASH_BT( "CSGOHud" ) ) {
			m_hud_panel = itr;
			break;
		}

		auto parent = itr->get_parent( );
		if ( !parent || parent == itr || !m_uiengine->is_valid_panel_ptr( parent ) )
			break;

		itr = parent;
	}

	return m_hud_panel != nullptr;
}

void n_chud::impl_t::on_createmove( )
{
	const bool enabled = GET_VARIABLE( g_variables.m_chud_hud, bool );

	if ( m_should_force_update ) {
		m_should_force_update = false;
		m_inited              = false;
		m_built               = true;
	}

	if ( !enabled ) {
		if ( chat_reset( ) ) {
			run( k_close_chat );
			run( k_force_drop_focus );
		}

		if ( m_built && resolve_hud( ) )
			uninstall( );

		return;
	}

	if ( !resolve_hud( ) ) {
		chat_reset( ); // no panel, nothing drains the buffer: never hold keys
		return;
	}

	const int layout_key = static_cast< int >( GET_VARIABLE( g_variables.m_chud_hud_scale, float ) ) ^
	                       ( GET_VARIABLE( g_variables.m_chud_hud_watermark_bg, bool ) ? 0x40000000 : 0 ) ^
	                       ( GET_VARIABLE( g_variables.m_chud_hud_blur, bool ) ? 0x20000000 : 0 ) ^
	                       ( GET_VARIABLE( g_variables.m_chud_hud_spectator_avatars, bool ) ? 0x08000000 : 0 ) ^
	                       ( GET_VARIABLE( g_variables.m_chud_hud_spectator_max_width, int ) << 12 ) ^
	                       ( GET_VARIABLE( g_variables.m_chud_hud_font, int ) << 24 ) ^
	                       ( static_cast< int >( GET_VARIABLE( g_variables.m_chud_hud_padding, float ) ) << 4 ) ^
	                       ( static_cast< int >( GET_VARIABLE( g_variables.m_chud_hud_font_nudge_x, float ) ) << 10 ) ^
	                       ( static_cast< int >( GET_VARIABLE( g_variables.m_chud_hud_font_nudge_y, float ) ) << 16 ) ^
	                       ( static_cast< int >( GET_VARIABLE( g_variables.m_chud_hud_font_scale, float ) ) << 6 ) ^
	                       ( GET_VARIABLE( g_variables.m_chud_hud_shadow, bool ) ? 0x10000000 : 0 ) ^
	                       ( static_cast< int >( GET_VARIABLE( g_variables.m_chud_hud_shadow_blur, float ) ) << 2 ) ^
	                       ( static_cast< int >( GET_VARIABLE( g_variables.m_chud_hud_shadow_x, float ) ) << 12 ) ^
	                       ( static_cast< int >( GET_VARIABLE( g_variables.m_chud_hud_shadow_y, float ) ) << 18 ) ^
	                       ( static_cast< int >( GET_VARIABLE( g_variables.m_chud_hud_shadow_spread, float ) ) << 22 ) ^
	                       static_cast< int >( std::hash< std::string >{ }( shadow_color( ) ) ) ^
	                       static_cast< int >( std::hash< std::string >{ }( GET_VARIABLE( g_variables.m_chud_hud_font_custom, std::string ) ) );

	/* debounce: per-frame rebuilds during a slider drag thrash panorama and race its
	   panel creation. commit once the value sits still. */
	if ( layout_key != m_pending_layout ) {
		m_pending_layout    = layout_key;
		m_style_settle_time = g_interfaces.m_global_vars_base->m_current_time + 0.20f;
	}
	else if ( layout_key != m_old_layout && g_interfaces.m_global_vars_base->m_current_time >= m_style_settle_time ) {
		m_inited     = false;
		m_old_layout = layout_key;
	}

	if ( !m_inited ) {
		install( );
		m_old_colors = color_key( );
		push_colors( );
		return;
	}

	const unsigned int colors = color_key( );
	if ( colors != m_old_colors ) {
		m_old_colors = colors;
		push_colors( );
	}

	tick( );
}
