#pragma once
#include "gta_data.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

/* CFont port (gta_sa.exe 1.0 US, gta-reversed Font.cpp): fonts.dat widths, font1 / font2 glyph grid (16 cols, rows of
   1/12.8), edge = 8 copies in the drop color, word wrap / centre / right justify, ~x~ colour tokens + ~n~.
   pure: no d3d / imgui, text in -> quads out (tools/gta_hud_test.cpp runs it). slant + ps2 button tokens skipped */
namespace n_gta_font
{
	struct rgba_t {
		std::uint8_t r = 255, g = 255, b = 255, a = 255;
	};

	enum e_hud_colour : int {
		hud_red,
		hud_green,
		hud_dark_blue,
		hud_light_blue,
		hud_light_gray,
		hud_black,
		hud_gold,
		hud_purple,
		hud_dark_gray,
		hud_dark_red,
		hud_dark_green,
		hud_cream,
		hud_night_blue,
		hud_blue,
		hud_yellow,
		hud_colour_count
	};

	// CHudColours::CHudColours 0x58FDA0
	inline constexpr rgba_t k_hud_colours[ hud_colour_count ] = {
		{ 180, 25, 29, 255 },   { 54, 104, 44, 255 },  { 50, 60, 127, 255 },  { 172, 203, 241, 255 }, { 225, 225, 225, 255 },
		{ 0, 0, 0, 255 },       { 144, 98, 16, 255 },  { 168, 110, 252, 255 }, { 150, 150, 150, 255 }, { 104, 15, 17, 255 },
		{ 38, 71, 31, 255 },    { 226, 192, 99, 255 }, { 74, 90, 107, 255 },  { 20, 25, 200, 255 },   { 255, 255, 0, 255 },
	};

	inline rgba_t hud_rgba( int colour, std::uint8_t alpha )
	{
		rgba_t c = k_hud_colours[ colour ];
		c.a      = alpha;
		return c;
	}

	enum e_style : int { font_gothic, font_subtitles, font_menu, font_pricedown };
	enum e_align : int { align_center, align_left, align_right }; // eFontAlignment
	enum e_tex_style : int { tex_subtitles_gothic, tex_pricedown, tex_menu };

	// CFontDetails, defaults = CFont::Initialise
	struct details_t {
		rgba_t m_color{ 255, 255, 255, 0 };
		float m_scale_x = 1.f, m_scale_y = 1.f;
		bool m_justify = false, m_centre = false, m_right = false, m_prop = true;
		float m_wrap_end = 640.f, m_centre_x = 640.f, m_right_wrap = 0.f; // SCREEN_WIDTH, set by the caller
		int m_texture   = 0;                                               // 0 FONT2 (gothic / menu), 1 FONT1 (subtitles / pricedown)
		int m_tex_style = tex_subtitles_gothic;
		std::uint8_t m_drop = 0, m_edge = 0, m_edge_space = 0;
		rgba_t m_drop_color{ 0, 0, 0, 255 };

		// 0x719490
		void set_style( int style )
		{
			m_texture   = style == font_gothic || style == font_menu ? 0 : 1;
			m_tex_style = style == font_menu ? tex_menu : style == font_pricedown ? tex_pricedown : tex_subtitles_gothic;
		}
		void set_align( int align ) { m_centre = align == align_center, m_right = align == align_right; }
		void set_edge( int v ) { m_drop = 0, m_edge = m_edge_space = static_cast< std::uint8_t >( v ); }
		void set_drop_shadow( int v ) { m_edge = m_edge_space = 0, m_drop = static_cast< std::uint8_t >( v ); }
	};

	// one glyph, corners clockwise from (left, y) = AddToBuffer u1v1 u2v2 u4v4 u3v3
	struct quad_t {
		float m_x[ 4 ], m_y[ 4 ], m_u[ 4 ], m_v[ 4 ];
		rgba_t m_color;
		int m_texture;
	};

	struct context_t {
		float m_w = 640.f, m_h = 448.f; // screen px
		std::vector< quad_t >* m_out = nullptr;
		bool m_new_line = false; // CFont::m_NewLine
		float sx( float v ) const { return v * m_w / 640.f; }
		float sy( float v ) const { return v * m_h / 448.f; }
	};

	// 0x7192C0
	inline std::uint8_t find_sub_font_character( std::uint8_t id, int style )
	{
		if ( style == tex_pricedown ) {
			switch ( id ) {
			case 1: return 208;
			case 4: return 93;
			case 7: return 206;
			case 8:
			case 9: return static_cast< std::uint8_t >( id + 86 );
			case 14: return 207;
			case 26: return 154;
			default: break;
			}
		}
		if ( id == 6 )
			return 10;
		if ( id >= 16 && id <= 25 )
			return static_cast< std::uint8_t >( id + 128 );
		if ( id == 31 )
			return 91;
		if ( id >= 33 && id <= 58 )
			return static_cast< std::uint8_t >( id + 122 );
		if ( id == 62 )
			return 32;
		if ( id >= 65 && id <= 90 )
			return static_cast< std::uint8_t >( id + 90 );
		if ( id >= 96 && id <= 118 )
			return static_cast< std::uint8_t >( id + 85 );
		if ( id >= 119 && id <= 140 )
			return static_cast< std::uint8_t >( id + 62 );
		if ( id >= 141 && id <= 142 )
			return 204;
		if ( id == 143 )
			return 205;
		return id;
	}

	// 0x719650
	inline std::uint8_t remap_without_graphic( std::uint8_t id ) { return id == 145 ? 64 : id > 155 ? 0 : id; }

	// tFontData::GetPropValue: [208] space value, [209] unprop. past that the exe reads the next font, clamp to unprop
	inline int prop_value( int texture, int id ) { return n_gta_data::k_font_prop[ texture & 1 ][ id < 210 ? id : 209 ]; }

	inline std::uint8_t glyph_id( std::uint8_t letter, int tex_style )
	{
		return tex_style != tex_subtitles_gothic ? find_sub_font_character( letter, tex_style ) : remap_without_graphic( letter );
	}

	// 0x719750
	inline float character_size( const details_t& d, std::uint8_t letter )
	{
		if ( letter == '?' )
			letter = 0;
		const int id = glyph_id( letter, d.m_tex_style );
		const int w  = d.m_prop ? prop_value( d.m_texture, id ) : prop_value( d.m_texture, 209 );
		return static_cast< float >( w + d.m_edge ) * d.m_scale_x;
	}

	// token end: after the closing '~' (0x718F00 return)
	inline const char* skip_token( const char* t )
	{
		const char* n = t + 1;
		if ( *n != '~' )
			while ( *n && *n != '~' )
				++n;
		return *n ? n + 1 : n;
	}

	// 0x718F00, colour part. returns past the token, colours only when !blip
	inline const char* parse_token( context_t& ctx, const char* t, rgba_t& color, bool blip, char* tag )
	{
		const char c     = t[ 1 ];
		const auto style = [ & ]( int hud ) {
			if ( !blip )
				color = hud_rgba( hud, color.a );
			if ( tag )
				*tag = c;
		};
		switch ( c ) {
		case 'B':
		case 'b': style( hud_dark_blue ); break;
		case 'G':
		case 'g': style( hud_green ); break;
		case 'H':
		case 'h':
			if ( !blip ) {
				color.r = static_cast< std::uint8_t >( std::min( color.r * 1.5f, 255.f ) );
				color.g = static_cast< std::uint8_t >( std::min( color.g * 1.5f, 255.f ) );
				color.b = static_cast< std::uint8_t >( std::min( color.b * 1.5f, 255.f ) );
			}
			if ( tag )
				*tag = c;
			break;
		case 'N':
		case 'n': ctx.m_new_line = true; break;
		case 'P':
		case 'p': style( hud_purple ); break;
		case 'R':
		case 'r': style( hud_red ); break;
		case 'S':
		case 's':
		case 'W':
		case 'w': style( hud_light_gray ); break;
		case 'Y':
		case 'y': style( hud_cream ); break;
		case 'l': style( hud_black ); break;
		default: break;
		}
		return skip_token( t );
	}

	// 0x71A0E0, full = false: one word (stops at ' ' or a token after letters)
	inline float string_width( const details_t& d, const char* s, bool full )
	{
		float width        = 0.f;
		bool last_tag      = false;
		bool last_letter   = false;
		while ( *s && !( *s == ' ' && !full ) ) {
			if ( *s == '~' ) {
				if ( !full && ( last_tag || last_letter ) )
					return width;
				const char* next = s + 1;
				if ( *next != '~' )
					while ( *next && *next != '~' )
						++next;
				s = *next ? next + 1 : next;
				if ( last_letter || *s == '~' )
					last_tag = true;
				continue;
			}
			width += character_size( d, static_cast< std::uint8_t >( *s++ - 0x20 ) );
			last_letter = true;
		}
		return width;
	}

	inline float height( const details_t& d ) { return d.m_scale_y * 18.f; } // y * 32 / 2 + y + y

	// 0x718A10
	inline void print_char( const context_t& ctx, float x, float y, std::uint8_t letter, float w, float h, const rgba_t& color, int texture, int tex_style )
	{
		if ( x < 0.f || x > ctx.m_w || y < 0.f || y > ctx.m_h || !ctx.m_out )
			return;
		if ( !letter || letter == '?' || ( tex_style == tex_pricedown && letter == 0xD0 ) )
			return;
		const float u1 = static_cast< float >( letter % 16 ) / 16.f;
		const float v  = static_cast< float >( letter / 16 ) / 12.8f;
		quad_t q{ };
		q.m_color   = color;
		q.m_texture = texture;
		const float right = x + 32.f * w;
		const float u2    = u1 + 0.0615f;
		float bottom = 0.f, v1 = 0.f, v3 = 0.f, v4 = 0.f;
		if ( letter < 0xC0 )
			bottom = y + 20.f * h, v1 = v + 0.0021f, v3 = v + 0.076025f, v4 = v3;
		else
			bottom = y + 16.f * h, v1 = v + 0.0021f, v3 = v + 0.078125f - 0.016f, v4 = v + 0.078125f - 0.015f;
		const float xs[ 4 ] = { x, right, right, x }, ys[ 4 ] = { y, y, bottom, bottom };
		const float us[ 4 ] = { u1, u2, u2, u1 }, vs[ 4 ] = { v1, v1, v4, v3 };
		std::memcpy( q.m_x, xs, sizeof( xs ) ), std::memcpy( q.m_y, ys, sizeof( ys ) );
		std::memcpy( q.m_u, us, sizeof( us ) ), std::memcpy( q.m_v, vs, sizeof( vs ) );
		ctx.m_out->push_back( q );
	}

	// 0x719B40 + RenderFontBuffer: one line segment [start, end), shadow / edge copies first
	inline void print_segment( context_t& ctx, details_t& d, float x, float y, const char* start, const char* end, float wrap, bool shadow = false )
	{
		const rgba_t original = d.m_color;
		if ( !shadow && ( d.m_drop || d.m_edge ) ) {
			d.m_color         = d.m_drop_color;
			const int amount  = d.m_drop ? d.m_drop : d.m_edge;
			const float ox    = ctx.sx( static_cast< float >( amount ) ), oy = ctx.sy( static_cast< float >( amount ) );
			const std::uint8_t keep_edge = d.m_edge, keep_drop = d.m_drop;
			d.m_edge = 0, d.m_drop = 0;
			if ( keep_drop )
				print_segment( ctx, d, x + ox, y + oy, start, end, wrap, true );
			else {
				const float offsets[ 8 ][ 2 ] = { { ox, -oy }, { -ox, -oy }, { ox, oy }, { -ox, oy }, { ox, 0.f }, { -ox, 0.f }, { 0.f, oy }, { 0.f, -oy } };
				for ( const auto& o : offsets )
					print_segment( ctx, d, x + o[ 0 ], y + o[ 1 ], start, end, wrap, true );
			}
			// the exe puts back rgb only: this segment draws with the drop colour's alpha
			d.m_color.r = original.r, d.m_color.g = original.g, d.m_color.b = original.b;
			d.m_edge = keep_edge, d.m_drop = keep_drop;
		}
		rgba_t color = d.m_color;
		float pen    = x;
		for ( const char* t = start; t < end; ) {
			if ( *t == '~' ) {
				t = parse_token( ctx, t, color, shadow, nullptr );
				continue;
			}
			const std::uint8_t letter = glyph_id( static_cast< std::uint8_t >( *t - 32 ), d.m_tex_style );
			print_char( ctx, pen, y, letter, d.m_scale_x, d.m_scale_y, color, d.m_texture, d.m_tex_style );
			const int id = letter != '?' ? letter : 0;
			pen += static_cast< float >( ( d.m_prop ? prop_value( d.m_texture, id ) : prop_value( d.m_texture, 209 ) ) + d.m_edge_space ) * d.m_scale_x;
			if ( !letter )
				pen += wrap;
			++t;
		}
		if ( !shadow )
			d.m_color = rgba_t{ color.r, color.g, color.b, original.a };
	}

	// 0x71A220
	inline int process_string( context_t& ctx, details_t& d, bool print, float x, float y, const char* text )
	{
		ctx.m_new_line          = false;
		bool first_word         = true;
		char tag                = 0;
		float width             = d.m_centre || d.m_right ? 0.f : x;
		float processed         = 0.f;
		const char* line_start  = text;
		int spaces              = 0;
		char saved_tag[ 256 ]{ };
		const auto reset_line   = [ & ] {
			width      = d.m_centre || d.m_right ? 0.f : x;
			processed  = 0.f;
			first_word = true;
			tag        = 0;
			line_start = text;
			spaces     = 0;
		};
		int lines = 0;
		while ( *text ) {
			const float word = string_width( d, text, false );
			if ( *text == '~' ) {
				rgba_t dummy{ };
				text = parse_token( ctx, text, dummy, true, &tag );
			}
			const float limit = d.m_centre ? d.m_centre_x : d.m_right ? x - d.m_right_wrap : d.m_wrap_end;
			const float total = word + width;
			if ( ( total <= limit || first_word ) && !ctx.m_new_line ) {
				width = total;
				while ( *text && *text != ' ' && *text != '~' ) // GetNextSpace
					++text;
				if ( *text ) {
					if ( !first_word )
						++spaces;
					if ( *text != '~' ) {
						++text;
						width += character_size( d, 0 );
					}
					processed  = width;
					first_word = false;
					continue;
				}
				const float draw_x = d.m_centre ? x - width / 2.f : d.m_right ? x - width : x;
				++lines;
				if ( print )
					print_segment( ctx, d, draw_x, y, line_start, text, 0.f );
				continue;
			}
			const char* print_end = ctx.m_new_line ? text - 3 : text;
			float draw_x = x, wrap = 0.f;
			if ( d.m_justify && !d.m_centre && spaces > 0 )
				wrap = ( d.m_wrap_end - processed ) / static_cast< float >( spaces );
			if ( d.m_centre )
				draw_x = x - width / 2.f;
			else if ( d.m_right )
				draw_x = x - ( width - character_size( d, 0 ) );
			++lines;
			if ( print && print_end > line_start )
				print_segment( ctx, d, draw_x, y, line_start, print_end, wrap );
			if ( tag ) {
				saved_tag[ 0 ] = '~', saved_tag[ 1 ] = tag, saved_tag[ 2 ] = '~';
				const size_t n = std::min( std::strlen( text ), sizeof( saved_tag ) - 4 );
				std::memcpy( saved_tag + 3, text, n );
				saved_tag[ 3 + n ] = 0;
				text               = saved_tag;
			}
			ctx.m_new_line = false;
			y += d.m_scale_y * 18.f;
			reset_line( );
		}
		return lines;
	}

	// 0x71A700 (background skipped: no hud element turns it on)
	inline void print( context_t& ctx, details_t& d, float x, float y, const char* text )
	{
		if ( !text || !*text || *text == '*' )
			return;
		process_string( ctx, d, true, x, y, text );
	}

	// 0x71A820
	inline void print_from_bottom( context_t& ctx, details_t& d, float x, float y, const char* text )
	{
		const int lines = process_string( ctx, d, false, x, y, text );
		print( ctx, d, x, y - height( d ) * static_cast< float >( lines ), text );
	}
}
