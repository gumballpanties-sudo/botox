#pragma once
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "mc_assets.h"

/* pure minecraft 1.21.11 rules, no game deps: tools/mc_hud_check.py compiles + runs tools/mc_hud_test.cpp against this */
namespace n_mc_logic
{
	inline constexpr std::uint32_t k_white = 0xFFFFFF;

	// one colour per codepoint, the chat keeps <font color> runs through wrapping
	struct text_t {
		std::vector< std::uint32_t > m_cp{ };
		std::vector< std::uint32_t > m_rgb{ };

		void push( std::uint32_t cp, std::uint32_t rgb )
		{
			m_cp.push_back( cp );
			m_rgb.push_back( rgb );
		}

		size_t size( ) const { return m_cp.size( ); }
	};

	inline std::uint32_t decode_one( std::string_view s, size_t& i )
	{
		const auto b = static_cast< unsigned char >( s[ i++ ] );
		int extra          = 0;
		std::uint32_t cp   = b;
		if ( b >= 0xF0 && b < 0xF8 ) {
			extra = 3;
			cp    = b & 0x07;
		}
		else if ( b >= 0xE0 ) {
			extra = 2;
			cp    = b & 0x0F;
		}
		else if ( b >= 0xC0 ) {
			extra = 1;
			cp    = b & 0x1F;
		}
		else if ( b >= 0x80 )
			return 0xFFFD;

		for ( ; extra > 0; --extra ) {
			if ( i >= s.size( ) || ( static_cast< unsigned char >( s[ i ] ) & 0xC0 ) != 0x80 )
				return 0xFFFD;
			cp = ( cp << 6 ) | ( static_cast< unsigned char >( s[ i++ ] ) & 0x3F );
		}
		return cp;
	}

	// csgo chat formats wrap names in U+200E, no glyph -> '?'
	inline bool visible( std::uint32_t cp )
	{
		if ( cp == '\n' )
			return true;
		return cp >= 0x20 && cp != 0xAD && cp != 0xFEFF && !( cp >= 0x200B && cp <= 0x200F ) && !( cp >= 0x202A && cp <= 0x202E ) &&
			   !( cp >= 0x2060 && cp <= 0x2069 );
	}

	inline void append( text_t& out, std::string_view utf8, std::uint32_t rgb )
	{
		for ( size_t i = 0; i < utf8.size( ); ) {
			const std::uint32_t cp = decode_one( utf8, i );
			if ( visible( cp ) )
				out.push( cp, rgb );
		}
	}

	inline text_t plain( std::string_view utf8, std::uint32_t rgb = k_white )
	{
		text_t out;
		append( out, utf8, rgb );
		return out;
	}

	inline const n_mc_assets::glyph_t* find_glyph( std::uint32_t cp )
	{
		const auto* const begin = std::begin( n_mc_assets::k_glyphs );
		const auto* const end   = std::end( n_mc_assets::k_glyphs );
		const auto* const it =
			std::lower_bound( begin, end, cp, []( const n_mc_assets::glyph_t& g, std::uint32_t v ) { return g.m_codepoint < v; } );
		return it != end && it->m_codepoint == cp ? it : nullptr;
	}

	// ponytail: missing codepoints draw '?', MC falls back to unifont (a 2 MB sheet) instead
	inline const n_mc_assets::glyph_t* glyph( std::uint32_t cp )
	{
		const auto* const g = find_glyph( cp );
		return g ? g : find_glyph( '?' );
	}

	inline int advance( std::uint32_t cp )
	{
		if ( cp == ' ' )
			return n_mc_assets::k_space_advance;
		if ( cp == 0x200C || cp == '\n' )
			return 0;
		const auto* const g = glyph( cp );
		return g ? g->m_advance : 0;
	}

	inline int width( const text_t& t )
	{
		int w = 0;
		for ( const std::uint32_t cp : t.m_cp )
			w += advance( cp );
		return w;
	}

	inline int width( std::string_view utf8 ) { return width( plain( utf8 ) ); }

	inline text_t slice( const text_t& t, size_t from, size_t to )
	{
		text_t out;
		out.m_cp.assign( t.m_cp.begin( ) + from, t.m_cp.begin( ) + to );
		out.m_rgb.assign( t.m_rgb.begin( ) + from, t.m_rgb.begin( ) + to );
		return out;
	}

	// StringSplitter.splitLines: break at the last space that fits, else before the char that overflows; one space / newline eaten
	inline std::vector< text_t > wrap( const text_t& t, int max_width )
	{
		std::vector< text_t > lines;
		size_t start = 0;

		do {
			int w             = 0;
			size_t end        = t.size( );
			size_t last_space = std::string::npos;

			for ( size_t i = start; i < t.size( ); ++i ) {
				const std::uint32_t cp = t.m_cp[ i ];
				if ( cp == '\n' ) {
					end = i;
					break;
				}
				if ( cp == ' ' )
					last_space = i;

				w += advance( cp );
				if ( w > max_width && i > start ) {
					end = last_space != std::string::npos ? last_space : i;
					break;
				}
			}

			lines.push_back( slice( t, start, end ) );
			start = end;
			if ( start < t.size( ) && ( t.m_cp[ start ] == ' ' || t.m_cp[ start ] == '\n' ) )
				++start;
		} while ( start < t.size( ) );

		return lines;
	}

	inline std::uint32_t hex_rgb( std::string_view s, bool& ok )
	{
		std::uint32_t v = 0;
		ok              = s.size( ) >= 6;
		for ( size_t i = 0; ok && i < 6; ++i ) {
			const char c = s[ i ];
			const int d  = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
			ok           = d >= 0;
			v            = v << 4 | static_cast< std::uint32_t >( d < 0 ? 0 : d );
		}
		return v;
	}

	// panorama chat html -> coloured text: <font color="#rrggbb"> runs, <br>, entities, every other tag dropped
	inline text_t from_html( std::string_view html, std::uint32_t base = k_white )
	{
		text_t out;
		std::vector< std::uint32_t > colors{ base };

		const auto lower_starts = []( std::string_view s, std::string_view p ) {
			if ( s.size( ) < p.size( ) )
				return false;
			for ( size_t i = 0; i < p.size( ); ++i )
				if ( static_cast< char >( std::tolower( static_cast< unsigned char >( s[ i ] ) ) ) != p[ i ] )
					return false;
			return true;
		};

		for ( size_t i = 0; i < html.size( ); ) {
			if ( html[ i ] == '<' ) {
				const size_t close = html.find( '>', i );
				if ( close == std::string_view::npos )
					break;

				const std::string_view tag = html.substr( i + 1, close - i - 1 );
				if ( lower_starts( tag, "font" ) ) {
					std::uint32_t rgb = colors.back( );
					if ( const size_t at = tag.find( '#' ); at != std::string_view::npos ) {
						bool ok              = false;
						const std::uint32_t v = hex_rgb( tag.substr( at + 1 ), ok );
						if ( ok )
							rgb = v;
					}
					colors.push_back( rgb );
				}
				else if ( lower_starts( tag, "/font" ) ) {
					if ( colors.size( ) > 1 )
						colors.pop_back( );
				}
				else if ( lower_starts( tag, "br" ) )
					out.push( '\n', colors.back( ) );

				i = close + 1;
				continue;
			}

			if ( html[ i ] == '&' ) {
				static constexpr std::pair< std::string_view, char > k_entities[] = {
					{ "&amp;", '&' }, { "&lt;", '<' }, { "&gt;", '>' }, { "&quot;", '"' }, { "&#39;", '\'' }, { "&apos;", '\'' }, { "&nbsp;", ' ' }
				};

				bool hit = false;
				for ( const auto& [ name, c ] : k_entities ) {
					if ( html.substr( i, name.size( ) ) == name ) {
						out.push( static_cast< unsigned char >( c ), colors.back( ) );
						i += name.size( );
						hit = true;
						break;
					}
				}
				if ( hit )
					continue;
			}

			const std::uint32_t cp = decode_one( html, i );
			if ( visible( cp ) )
				out.push( cp, colors.back( ) );
		}
		return out;
	}

	// CCSGO_HudVoiceStatus::GetColorCode. code -1 = no byte yet, team 2 = T, 3 = CT
	inline std::uint32_t notice_rgb( int code, int team )
	{
		const bool side = team == 2 || team == 3;
		if ( code < 0 && !side )
			return k_white;
		if ( code < 0 || code == 3 )
			return team == 3 ? 0xA2C6FF : team == 2 ? 0xFFDF93 : 0xBA81F0;

		// 2 = COLOR_USEOLDCOLORS hits the "unknown" red, 10..16 = items_game desc_common..desc_immortal
		static constexpr std::uint32_t k_codes[ 17 ] = { k_white,  k_white,  0xFF0000, 0,        0x40FF40, 0xBFFF90, 0xA2FF47, 0xFF4040, 0xC5CAD0,
		                                                 0xEDE47A, 0xB0C3D9, 0x5E98D9, 0x4B69FF, 0x8847FF, 0xD32CE6, 0xEB4B4B, 0xE4AE39 };
		return code < 17 ? k_codes[ code ] : k_white;
	}

	// stock chat before ColorizeNotice: bytes 1..16 switch colour (\n = 10 too), text before the first one = sender's team colour
	inline text_t from_notice( std::string_view raw, int team )
	{
		text_t out;
		int code = -1;
		for ( size_t i = 0; i < raw.size( ); ) {
			const auto b = static_cast< unsigned char >( raw[ i ] );
			if ( b > 0 && b < 17 ) {
				code = b;
				++i;
				continue;
			}

			const std::uint32_t cp = decode_one( raw, i );
			if ( visible( cp ) )
				out.push( cp, notice_rgb( code, team ) );
		}
		return out;
	}

	// en_us.json death.attack.* wording, CS weapon string from player_death
	inline std::string death_message( std::string_view victim, std::string_view attacker, std::string_view weapon, std::string_view item )
	{
		const std::string v( victim ), a( attacker );
		const bool by = !attacker.empty( );
		const auto is = [ & ]( std::initializer_list< std::string_view > names ) {
			return std::find( names.begin( ), names.end( ), weapon ) != names.end( );
		};
		const std::string using_item = item.empty( ) ? std::string( ) : " using [" + std::string( item ) + "]";

		if ( is( { "hegrenade", "planted_c4", "breachcharge", "bumpmine" } ) )
			return by ? v + " was blown up by " + a : v + " blew up";
		if ( is( { "inferno", "molotov", "incgrenade" } ) )
			return by ? v + " was burned to a crisp while fighting " + a : v + " burned to death";
		if ( is( { "flashbang", "smokegrenade", "decoy", "snowball", "tagrenade" } ) )
			return by ? v + " was pummeled by " + a : v + " died";
		if ( is( { "taser" } ) )
			return by ? v + " was killed by " + a + " using magic" : v + " was killed by magic";
		if ( !by )
			return weapon == "worldspawn" ? v + " hit the ground too hard" : v + " died";

		const bool melee = weapon.substr( 0, 5 ) == "knife" || is( { "bayonet", "fists", "melee", "axe", "hammer", "spanner" } );
		return v + ( melee ? " was slain by " : " was shot by " ) + a + using_item;
	}

	// Window.calculateScale(guiScale, false): 0 = auto
	inline int gui_scale( int w, int h, int setting )
	{
		int s = 1;
		while ( s != setting && s < w && s < h && w / ( s + 1 ) >= 320 && h / ( s + 1 ) >= 240 )
			++s;
		return s;
	}

	// Window.setGuiScale: ceil
	inline int scaled( int px, int scale ) { return ( px + scale - 1 ) / scale; }

	// CS 0..100 -> MC 0..20 (Mth.ceil of the float health)
	inline int to_mc( int value ) { return std::clamp( ( value + 4 ) / 5, 0, 20 ); }

	// ChatComponent.AlphaCalculator.timeBased
	inline float chat_fade( int age_ticks )
	{
		double t = 1.0 - age_ticks / 200.0;
		t        = std::clamp( t * 10.0, 0.0, 1.0 );
		return static_cast< float >( t * t );
	}

	// Gui.renderSelectedItemName alpha from remaining highlight ticks (40 on switch)
	inline int highlight_alpha( int remaining_ticks ) { return std::min( remaining_ticks * 256 / 10, 255 ); }

	// LegacyRandomSource (java.util.Random LCG), Gui seeds it with tickCount * 312871 for the low health shake
	struct java_random_t {
		std::uint64_t m_seed = 0;

		void set_seed( std::int64_t seed ) { m_seed = ( static_cast< std::uint64_t >( seed ) ^ 0x5DEECE66DULL ) & ( ( 1ULL << 48 ) - 1 ); }

		int next( int bits )
		{
			m_seed = ( m_seed * 0x5DEECE66DULL + 0xBULL ) & ( ( 1ULL << 48 ) - 1 );
			return static_cast< int >( static_cast< std::int64_t >( m_seed ) >> ( 48 - bits ) );
		}

		// power of two bounds only (Gui uses nextInt(2))
		int next_int_pow2( int bound ) { return static_cast< int >( ( static_cast< std::int64_t >( bound ) * next( 31 ) ) >> 31 ); }
	};

	/* Gui.renderPlayerHealth blink state. MC: blink = healthBlinkTime > tick && (healthBlinkTime - tick) / 3 % 2 == 1,
	   read BEFORE this frame's update. damage -> blink 20 ticks; displayHealth catches up after 1000 ms */
	struct hearts_t {
		int m_last_health        = -1;
		int m_display_health     = 0;
		long long m_blink_until  = 0;
		long long m_last_time_ms = 0;

		bool update( int health, long long tick, long long now_ms )
		{
			const bool blink = m_blink_until > tick && ( m_blink_until - tick ) / 3 % 2 == 1;

			if ( m_last_health >= 0 && health < m_last_health ) {
				m_last_time_ms = now_ms;
				m_blink_until  = tick + 20;
			}

			if ( m_last_health < 0 || now_ms - m_last_time_ms > 1000 ) {
				m_display_health = health;
				m_last_time_ms   = now_ms;
			}

			m_last_health = health;
			return blink;
		}
	};
}
