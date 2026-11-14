#pragma once
#include "urt_data.h"
#include "urt_logic.h"

#include "../../dependencies/imgui/imgui.h"
#include "../misc/air_wings.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <d3d9.h>
#include <d3dx9.h>

/* render thread side of urt_hud: art (botox-assets urt/ -> C:\botox\urt\), cvar store, and the cgame 2d primitives
   (R_SetColor / R_DrawStretchPic / CG_DrawPic / CG_FillRect / UI_Text_Paint) on an imgui draw list, 640x480 virtual
   screen stretched to the window like cgs.screenXScale / screenYScale */
namespace n_urt
{
	using vec4_t = float[ 4 ];

	inline constexpr float k_white[ 4 ] = { 1.f, 1.f, 1.f, 1.f };

	// g_color_table (data 56700), ^0..^9
	inline constexpr float k_color_table[ 10 ][ 4 ] = {
		{ 0.f, 0.f, 0.f, 1.f },      { 0.8745f, 0.2902f, 0.3098f, 1.f }, { 0.f, 1.f, 0.f, 1.f },     { 1.f, 1.f, 0.6f, 1.f },
		{ 0.61f, 0.81f, 1.f, 1.f },  { 0.f, 1.f, 1.f, 1.f },             { 1.f, 0.f, 1.f, 1.f },     { 1.f, 1.f, 1.f, 1.f },
		{ 0.88f, 0.31f, 0.13f, 1.f }, { 0.29f, 0.3f, 0.19f, 1.f },
	};

	/* ---- cvars: name -> string, stock defaults from the qvm table ---- */
	struct cvars_t {
		struct value_t {
			std::string m_string{ };
			float m_value   = 0.f;
			int m_integer   = 0;
		};
		std::unordered_map< std::string, value_t > m_map{ };

		void set( const std::string& name, const std::string& v )
		{
			value_t& x  = m_map[ name ];
			x.m_string  = v;
			x.m_value   = static_cast< float >( std::atof( v.c_str( ) ) );
			x.m_integer = std::atoi( v.c_str( ) );
		}

		void reset( )
		{
			m_map.clear( );
			for ( const cvar_t& c : k_cvars )
				set( c.m_name, c.m_default );
			set( "ui_smallFont", "0.25" );
			set( "ui_bigFont", "0.4" );
		}

		const value_t& get( std::string_view name ) const
		{
			static const value_t k_empty{ };
			const auto it = m_map.find( std::string( name ) );
			return it == m_map.end( ) ? k_empty : it->second;
		}
		float f( std::string_view n ) const { return get( n ).m_value; }
		int i( std::string_view n ) const { return get( n ).m_integer; }
		const std::string& s( std::string_view n ) const { return get( n ).m_string; }

		// "r g b a" (cg_crosshairRGB style), missing parts = 1
		void rgba( std::string_view n, float out[ 4 ] ) const
		{
			out[ 0 ] = out[ 1 ] = out[ 2 ] = out[ 3 ] = 1.f;
			const char* p = s( n ).c_str( );
			for ( int k = 0; k < 4 && *p; ++k ) {
				char* end = nullptr;
				const float v = std::strtof( p, &end );
				if ( end == p )
					break;
				out[ k ] = v;
				p        = end;
			}
		}

		// saved form: name=value lines, only values that differ from the default
		std::string save( ) const
		{
			std::string out{ };
			for ( const cvar_t& c : k_cvars ) {
				const value_t& v = get( c.m_name );
				if ( v.m_string != c.m_default )
					out += std::string( c.m_name ) + "=" + v.m_string + "\n";
			}
			return out;
		}

		void load( const std::string& saved )
		{
			reset( );
			size_t a = 0;
			while ( a < saved.size( ) ) {
				size_t e = saved.find( '\n', a );
				if ( e == std::string::npos )
					e = saved.size( );
				const std::string line = saved.substr( a, e - a );
				const size_t eq        = line.find( '=' );
				if ( eq != std::string::npos && m_map.count( line.substr( 0, eq ) ) )
					set( line.substr( 0, eq ), line.substr( eq + 1 ) );
				a = e + 1;
			}
		}
	};

	inline int image_index( std::string_view shader )
	{
		std::string key( shader );
		std::transform( key.begin( ), key.end( ), key.begin( ), [ ]( unsigned char c ) { return static_cast< char >( std::tolower( c ) ); } );
		for ( const alias_t& a : k_aliases )
			if ( key == a.m_name )
				return a.m_image;
		for ( int i = 0; i < static_cast< int >( std::size( k_images ) ); ++i ) {
			std::string_view p = k_images[ i ].m_path;
			if ( p == key || ( p.size( ) == key.size( ) + 4 && p.substr( 0, key.size( ) ) == key && p[ key.size( ) ] == '.' ) )
				return i;
		}
		return -2; // not shipped
	}

	/* ---- draw state ---- */
	struct texture_t {
		IDirect3DTexture9* m_texture = nullptr;
		bool m_tried = false;
	};

	struct draw_t {
		ImDrawList* m_list = nullptr;
		float m_sx = 1.f, m_sy = 1.f; // cgs.screenXScale / screenYScale
		float m_color[ 4 ]{ 1.f, 1.f, 1.f, 1.f };
		std::vector< std::vector< std::uint8_t > >* m_files = nullptr;
		std::vector< texture_t >* m_textures = nullptr;
		int m_blend = 0; // blend the list is currently in
	};
	inline draw_t g_draw{ };

	inline IDirect3DTexture9* make_texture( int w, int h, const std::uint8_t* bgra )
	{
		IDirect3DDevice9* const device = g_interfaces.m_direct_device;
		IDirect3DTexture9* tex         = nullptr;
		if ( !device || ( FAILED( device->CreateTexture( w, h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, nullptr ) ) &&
		                  FAILED( device->CreateTexture( w, h, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &tex, nullptr ) ) ) )
			return nullptr;
		D3DLOCKED_RECT rect{ };
		if ( FAILED( tex->LockRect( 0, &rect, nullptr, 0 ) ) ) {
			tex->Release( );
			return nullptr;
		}
		for ( int y = 0; y < h; ++y )
			std::memcpy( static_cast< std::uint8_t* >( rect.pBits ) + static_cast< size_t >( y ) * rect.Pitch, bgra + static_cast< size_t >( y ) * w * 4, static_cast< size_t >( w ) * 4 );
		tex->UnlockRect( 0 );
		return tex;
	}

	inline IDirect3DTexture9* texture( int image )
	{
		if ( image < 0 || !g_draw.m_files || !g_draw.m_textures || image >= static_cast< int >( g_draw.m_files->size( ) ) )
			return nullptr;
		if ( g_draw.m_textures->size( ) != g_draw.m_files->size( ) )
			g_draw.m_textures->resize( g_draw.m_files->size( ) );
		texture_t& t = ( *g_draw.m_textures )[ image ];
		if ( !t.m_tried ) {
			t.m_tried = true;
			const auto& f = ( *g_draw.m_files )[ image ];
			IDirect3DDevice9* const device = g_interfaces.m_direct_device;
			const std::string_view path    = k_images[ image ].m_path;
			if ( path.size( ) > 4 && path.substr( path.size( ) - 4 ) == ".tga" ) {
				int w = 0, h = 0;
				std::vector< std::uint8_t > bgra{ };
				t.m_texture = n_urt_logic::decode_tga( f, w, h, bgra ) ? make_texture( w, h, bgra.data( ) ) : nullptr;
				return t.m_texture;
			}
			// csgo is d3d9ex: MANAGED can be refused, DEFAULT + DYNAMIC then (released before Reset, reset.cpp)
			const auto load = [ & ]( DWORD usage, D3DPOOL pool ) {
				return D3DXCreateTextureFromFileInMemoryEx( device, f.data( ), static_cast< UINT >( f.size( ) ), D3DX_DEFAULT_NONPOW2, D3DX_DEFAULT_NONPOW2, 1,
				                                            usage, D3DFMT_A8R8G8B8, pool, D3DX_FILTER_NONE, D3DX_FILTER_NONE, 0, nullptr, nullptr, &t.m_texture );
			};
			if ( !device || f.empty( ) || ( FAILED( load( 0, D3DPOOL_MANAGED ) ) && FAILED( load( D3DUSAGE_DYNAMIC, D3DPOOL_DEFAULT ) ) ) )
				t.m_texture = nullptr;
		}
		return t.m_texture;
	}

	inline void cb_blend( const ImDrawList*, const ImDrawCmd* cmd )
	{
		IDirect3DDevice9* const d = g_interfaces.m_direct_device;
		switch ( static_cast< int >( reinterpret_cast< std::uintptr_t >( cmd->UserCallbackData ) ) ) {
		case 1: // GL_DST_COLOR GL_ZERO
			d->SetRenderState( D3DRS_SRCBLEND, D3DBLEND_DESTCOLOR );
			d->SetRenderState( D3DRS_DESTBLEND, D3DBLEND_ZERO );
			break;
		case 2: // GL_ONE GL_ONE
			d->SetRenderState( D3DRS_SRCBLEND, D3DBLEND_SRCALPHA );
			d->SetRenderState( D3DRS_DESTBLEND, D3DBLEND_ONE );
			break;
		case 3: d->SetRenderState( D3DRS_ALPHABLENDENABLE, FALSE ); break;
		default: break;
		}
	}

	inline void set_blend( int blend )
	{
		if ( blend == g_draw.m_blend )
			return;
		g_draw.m_list->AddCallback( ImDrawCallback_ResetRenderState, nullptr );
		if ( blend )
			g_draw.m_list->AddCallback( cb_blend, reinterpret_cast< void* >( static_cast< std::uintptr_t >( blend ) ) );
		g_draw.m_blend = blend;
	}

	inline ImU32 col( const float c[ 4 ] )
	{
		const auto b = [ ]( float v ) { return static_cast< int >( std::clamp( v, 0.f, 1.f ) * 255.f + 0.5f ); };
		return IM_COL32( b( c[ 0 ] ), b( c[ 1 ] ), b( c[ 2 ] ), b( c[ 3 ] ) );
	}

	// trap_R_SetColor (null = white)
	inline void set_color( const float* c )
	{
		const float* s = c ? c : k_white;
		std::memcpy( g_draw.m_color, s, sizeof( g_draw.m_color ) );
	}

	// trap_R_DrawStretchPic: real pixels. image -1 = *white
	inline void stretch_pic( float x, float y, float w, float h, float s0, float t0, float s1, float t1, int image )
	{
		if ( image < -1 || !g_draw.m_list )
			return;
		const bool identity = image >= 0 && k_images[ image ].m_identity;
		const float* c      = identity ? k_white : g_draw.m_color;
		if ( c[ 3 ] <= 0.f && !identity )
			return;
		if ( image == -1 ) {
			set_blend( 0 );
			g_draw.m_list->AddRectFilled( ImVec2( x, y ), ImVec2( x + w, y + h ), col( c ) );
			return;
		}
		IDirect3DTexture9* const t = texture( image );
		if ( !t )
			return;
		set_blend( k_images[ image ].m_blend );
		g_draw.m_list->AddImageQuad( reinterpret_cast< ImTextureID >( t ), ImVec2( x, y ), ImVec2( x + w, y ), ImVec2( x + w, y + h ), ImVec2( x, y + h ),
		                             ImVec2( s0, t0 ), ImVec2( s1, t0 ), ImVec2( s1, t1 ), ImVec2( s0, t1 ), col( c ) );
	}

	// trap_R_DrawStretchPic rotated about the rect center (degrees, screen y down)
	inline void rotated_pic( float x, float y, float w, float h, float angle, int image )
	{
		IDirect3DTexture9* const t = image >= 0 ? texture( image ) : nullptr;
		if ( !t || g_draw.m_color[ 3 ] <= 0.f )
			return;
		const float c = std::cos( angle * 0.0174532924f ), s = std::sin( angle * 0.0174532924f );
		const float cx = x + w * 0.5f, cy = y + h * 0.5f;
		const auto at  = [ & ]( float dx, float dy ) { return ImVec2( cx + dx * c - dy * s, cy + dx * s + dy * c ); };
		set_blend( k_images[ image ].m_blend );
		g_draw.m_list->AddImageQuad( reinterpret_cast< ImTextureID >( t ), at( -w * 0.5f, -h * 0.5f ), at( w * 0.5f, -h * 0.5f ), at( w * 0.5f, h * 0.5f ),
		                             at( -w * 0.5f, h * 0.5f ), ImVec2( 0, 0 ), ImVec2( 1, 0 ), ImVec2( 1, 1 ), ImVec2( 0, 1 ),
		                             col( k_images[ image ].m_identity ? k_white : g_draw.m_color ) );
	}

	// CG_DrawPic (sub_70045): virtual coords, current color
	inline void draw_pic( float x, float y, float w, float h, int image )
	{
		stretch_pic( x * g_draw.m_sx, y * g_draw.m_sy, w * g_draw.m_sx, h * g_draw.m_sy, 0.f, 0.f, 1.f, 1.f, image );
	}

	// CG_FillRect (sub_69659)
	inline void fill_rect( float x, float y, float w, float h, const float* color )
	{
		set_color( color );
		stretch_pic( x * g_draw.m_sx, y * g_draw.m_sy, w * g_draw.m_sx, h * g_draw.m_sy, 0.f, 0.f, 0.f, 0.f, -1 );
		set_color( nullptr );
	}

	// sub_69727: gfx/2d/hgrad.tga tinted
	inline void grad_rect( float x, float y, float w, float h, const float* color )
	{
		static const int k_hgrad = image_index( "gfx/2d/hgrad.tga" );
		set_color( color );
		stretch_pic( x * g_draw.m_sx, y * g_draw.m_sy, w * g_draw.m_sx, h * g_draw.m_sy, 0.f, 0.f, 1.f, 1.f, k_hgrad );
		set_color( nullptr );
	}

	// CG_DrawSides / CG_DrawTopBottom / CG_DrawRect (sub_69995)
	inline void draw_rect( float x, float y, float w, float h, float size, const float* color )
	{
		set_color( color );
		const float sx = g_draw.m_sx, sy = g_draw.m_sy;
		const float ws = size * sx, hs = size * sy;
		stretch_pic( x * sx, y * sy, ws, h * sy, 0, 0, 0, 0, -1 );
		stretch_pic( ( x + w ) * sx - ws, y * sy, ws, h * sy, 0, 0, 0, 0, -1 );
		stretch_pic( x * sx, y * sy, w * sx, hs, 0, 0, 0, 0, -1 );
		stretch_pic( x * sx, ( y + h ) * sy - hs, w * sx, hs, 0, 0, 0, 0, -1 );
		set_color( nullptr );
	}

	// CG_FadeColor (sub_70969): white, alpha fades over the last 200 ms, nullptr = expired
	inline const float* fade_color( int start, int total, int now )
	{
		static float c[ 4 ];
		if ( start == 0 )
			return nullptr;
		const int t = now - start;
		if ( t >= total )
			return nullptr;
		c[ 0 ] = c[ 1 ] = c[ 2 ] = 1.f;
		c[ 3 ] = total - t < 200 ? static_cast< float >( total - t ) * 0.005f : 1.f;
		return c;
	}

	/* ---- UI_Text_Paint / UI_Text_Width / UI_Text_Height (sub_23577 / 23148 / 23324) ---- */
	inline const font_t& pick_font( float scale, const cvars_t& cv )
	{
		if ( scale <= cv.f( "ui_smallFont" ) )
			return k_fonts[ 0 ];
		if ( scale > cv.f( "ui_bigFont" ) )
			return k_fonts[ 2 ];
		return k_fonts[ 1 ];
	}

	inline bool color_code( const char* p ) { return p[ 0 ] == '^' && p[ 1 ] >= '0' && p[ 1 ] <= '9'; }

	// byte 1 in a line = the cs2 jumpshot wings ( killer off the ground ). names never hold it: to_q3 drops < 32.
	// square, 1.2 x the 'A' glyph tall, virtual x advance keeps it square on screen
	inline constexpr char k_air_wings = 1;
	inline float air_wings_height( const font_t& f, float use ) { return static_cast< float >( f.m_glyphs[ 'A' ].m_h ) * use * 1.2f; }
	inline float air_wings_advance( const font_t& f, float use )
	{
		const float sq = g_draw.m_sx > 0.f ? g_draw.m_sy / g_draw.m_sx : 1.f;
		return air_wings_height( f, use ) * sq + static_cast< float >( f.m_glyphs[ ' ' ].m_xskip ) * use * 0.5f;
	}

	inline int text_width( const char* text, float scale, int limit, const cvars_t& cv )
	{
		const font_t& f   = pick_font( scale, cv );
		const float use   = scale * f.m_glyph_scale;
		float out         = 0.f;
		const int len     = text ? static_cast< int >( std::strlen( text ) ) : 0;
		const int count   = limit > 0 && len > limit ? limit : len;
		for ( int n = 0; text && *text && n < count; ) {
			if ( color_code( text ) ) {
				text += 2;
				continue;
			}
			if ( *text == k_air_wings ) {
				out += air_wings_advance( f, use ) / use;
				++text, ++n;
				continue;
			}
			out += static_cast< float >( f.m_glyphs[ static_cast< unsigned char >( *text ) ].m_xskip );
			++text, ++n;
		}
		return static_cast< int >( out * use );
	}

	inline int text_height( const char* text, float scale, int limit, const cvars_t& cv )
	{
		const font_t& f = pick_font( scale, cv );
		const float use = scale * f.m_glyph_scale;
		float max       = 0.f;
		const int len   = text ? static_cast< int >( std::strlen( text ) ) : 0;
		const int count = limit > 0 && len > limit ? limit : len;
		for ( int n = 0; text && *text && n < count; ) {
			if ( color_code( text ) ) {
				text += 2;
				continue;
			}
			max = std::max( max, static_cast< float >( f.m_glyphs[ static_cast< unsigned char >( *text ) ].m_height ) );
			++text, ++n;
		}
		return static_cast< int >( max * use );
	}

	enum e_style { style_normal = 0, style_shadowed = 3, style_outlined = 4, style_shadowed_more = 6, style_shadowed_light = 7 };

	// sub_23502: one glyph, virtual coords
	inline void paint_char( float x, float y, float w, float h, float scale, const glyph_t& g )
	{
		stretch_pic( x * g_draw.m_sx, y * g_draw.m_sy, w * scale * g_draw.m_sx, h * scale * g_draw.m_sy, g.m_s, g.m_t, g.m_s2, g.m_t2, g.m_image );
	}

	// y = baseline
	inline void text_paint( float x, float y, float scale, const float* color, const char* text, float adjust, int limit, int style, const cvars_t& cv )
	{
		if ( !text )
			return;
		static constexpr float k_shadow[ 4 ] = { 0.f, 0.f, 0.f, 1.f }, k_shadow_grey[ 4 ] = { 0.2f, 0.2f, 0.2f, 1.f };
		const font_t& f = pick_font( scale, cv );
		const float use = scale * f.m_glyph_scale;
		float cur[ 4 ];
		std::memcpy( cur, color, sizeof( cur ) );
		set_color( cur );
		const int len   = static_cast< int >( std::strlen( text ) );
		const int count = limit > 0 && len > limit ? limit : len;
		for ( int n = 0; *text && n < count; ) {
			// char 31 draws glyph 37 (the qvm indexes +2960)
			const glyph_t& g = *text == 31 ? f.m_glyphs[ 37 ] : f.m_glyphs[ static_cast< unsigned char >( *text ) ];
			if ( color_code( text ) ) {
				const float a = color[ 3 ];
				std::memcpy( cur, k_color_table[ ( text[ 1 ] - '0' ) % 10 ], sizeof( cur ) );
				cur[ 3 ] = a;
				set_color( cur );
				text += 2;
				continue;
			}
			if ( *text == k_air_wings ) {
				// 'A' cap height box, grown 1.2x around its middle
				const glyph_t& cap = f.m_glyphs[ 'A' ];
				const float s      = air_wings_height( f, use );
				const float top    = y - use * static_cast< float >( cap.m_top ) - ( s - use * static_cast< float >( cap.m_h ) ) * 0.5f;
				const int a        = static_cast< int >( std::clamp( cur[ 3 ], 0.f, 1.f ) * 255.f );
				n_air_wings::draw( g_draw.m_list, x * g_draw.m_sx, top * g_draw.m_sy, s * g_draw.m_sy, IM_COL32( 255, 255, 255, a ), IM_COL32( 0, 0, 0, a ),
				                   std::max( 1.f, std::round( s * g_draw.m_sy / 22.f ) ) );
				x += air_wings_advance( f, use ) + adjust;
				++text, ++n;
				continue;
			}
			const float yadj = use * static_cast< float >( g.m_top );
			if ( style == style_shadowed || style == style_shadowed_more || style == style_shadowed_light || style == style_outlined ) {
				const float ofs = style == style_shadowed_more ? 2.f : 1.f;
				float sh[ 4 ];
				const bool black = cur[ 0 ] == 0.f && cur[ 1 ] == 0.f && cur[ 2 ] == 0.f;
				std::memcpy( sh, black ? k_shadow_grey : k_shadow, sizeof( sh ) );
				sh[ 3 ] = style == style_shadowed_light ? cur[ 3 ] * 0.7f : cur[ 3 ];
				set_color( sh );
				paint_char( x + ofs, y - yadj + ofs, static_cast< float >( g.m_w ), static_cast< float >( g.m_h ), use, g );
				if ( style == style_outlined ) {
					paint_char( x - ofs, y - yadj - ofs, static_cast< float >( g.m_w ), static_cast< float >( g.m_h ), use, g );
					paint_char( x + ofs, y - yadj - ofs, static_cast< float >( g.m_w ), static_cast< float >( g.m_h ), use, g );
					paint_char( x - ofs, y - yadj + ofs, static_cast< float >( g.m_w ), static_cast< float >( g.m_h ), use, g );
				}
				set_color( cur );
			}
			paint_char( x, y - yadj, static_cast< float >( g.m_w ), static_cast< float >( g.m_h ), use, g );
			x += static_cast< float >( g.m_xskip ) * use + adjust;
			++text, ++n;
		}
		set_color( nullptr );
	}

	// csgo names are utf-8, the urt fonts are 8 bit: non ascii -> '?', q3 color codes kept
	inline std::string to_q3( std::string_view s )
	{
		std::string out{ };
		out.reserve( s.size( ) );
		for ( size_t i = 0; i < s.size( ); ++i ) {
			const unsigned char c = static_cast< unsigned char >( s[ i ] );
			if ( c < 0x80 ) {
				if ( c >= 32 || c == 31 )
					out.push_back( static_cast< char >( c ) );
				continue;
			}
			if ( ( c & 0xC0 ) == 0xC0 )
				out.push_back( '?' );
		}
		return out;
	}

	inline std::string strip_colors( std::string_view s )
	{
		std::string out{ };
		for ( size_t i = 0; i < s.size( ); ++i ) {
			if ( i + 1 < s.size( ) && color_code( s.data( ) + i ) ) {
				++i;
				continue;
			}
			out.push_back( s[ i ] );
		}
		return out;
	}
}
