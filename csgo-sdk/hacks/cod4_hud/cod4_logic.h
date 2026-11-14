#pragma once
#include "cod4_data.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <string_view>
#include <unordered_map>

/* pure COD4 rules, ported from iw3mp.exe 1.7 (addresses = the function each block mirrors).
   no d3d / game: tools/cod4_hud_test.cpp drives every piece */
namespace n_cod4
{
	using namespace n_cod4_data;

	/* ---- ScrPlace (0x472910 setup, 0x472b60 apply). pc safe area = 0 -> viewable = whole window ---- */
	struct scr_t {
		float m_w = 640.f, m_h = 480.f;
		float m_sx = 1.f, m_sy = 1.f;     // virtual -> real, 4:3 area
		float m_fx = 1.f, m_fy = 1.f;     // virtual -> full screen
		float m_rx = 1.f, m_ry = 1.f;     // real -> virtual
		float m_sub_left = 0.f;

		static scr_t make( float w, float h )
		{
			scr_t s{ };
			s.m_w = w, s.m_h = h;
			const float virt = std::min( w, 4.f / 3.f * h );
			s.m_sx       = virt / 640.f;
			s.m_sy       = h / 480.f;
			s.m_fx       = w / 640.f;
			s.m_fy       = s.m_sy;
			s.m_rx       = 640.f / virt;
			s.m_ry       = 480.f / h;
			s.m_sub_left = ( w - virt ) * 0.5f;
			return s;
		}

		float x( float v, int align ) const
		{
			switch ( align ) {
			case 1: return v * m_sx;
			case 2: return v * m_sx + m_w * 0.5f;
			case 3: return v * m_sx + m_w;
			case 4: return v * m_fx;
			case 5: return v;
			case 6: return v * m_rx;
			case 7: return v * m_sx + m_w * 0.5f;
			default: return v * m_sx + m_sub_left;
			}
		}

		float y( float v, int align ) const
		{
			switch ( align ) {
			case 1: return v * m_sy;
			case 2: return v * m_sy + m_h * 0.5f;
			case 3: return v * m_sy + m_h;
			case 4: return v * m_fy;
			case 5: return v;
			case 6: return v * m_ry;
			case 7: return v * m_sy + m_h * 0.5f;
			default: return v * m_sy;
			}
		}

		float w( float v, int align ) const { return align == 4 ? v * m_fx : align == 5 ? v : align == 6 ? v * m_rx : v * m_sx; }
		float h( float v, int align ) const { return align == 4 ? v * m_fy : align == 5 ? v : align == 6 ? v * m_ry : v * m_sy; }
	};

	/* ---- dvars: stock defaults (iw3mp.exe registrations) + the user's overrides ---- */
	struct dvar_value_t {
		const dvar_t* m_def = nullptr;
		float m_v[ 4 ]{ };
		std::string m_s{ };
	};

	struct dvars_t {
		std::unordered_map< std::string, dvar_value_t > m_map{ };

		dvars_t( ) { reset( ); }

		void reset( )
		{
			m_map.clear( );
			for ( const dvar_t& d : k_dvars ) {
				dvar_value_t v{ &d };
				std::memcpy( v.m_v, d.m_value, sizeof( v.m_v ) );
				v.m_s = d.m_string ? d.m_string : "";
				m_map[ d.m_name ] = std::move( v );
			}
		}

		const dvar_value_t* find( std::string_view n ) const
		{
			const auto it = m_map.find( std::string( n ) );
			return it == m_map.end( ) ? nullptr : &it->second;
		}

		float f( std::string_view n ) const
		{
			const dvar_value_t* v = find( n );
			return v ? ( v->m_def->m_kind == 4 ? static_cast< float >( std::atof( v->m_s.c_str( ) ) ) : v->m_v[ 0 ] ) : 0.f;
		}
		int i( std::string_view n ) const { return static_cast< int >( f( n ) ); }
		bool b( std::string_view n ) const { return f( n ) != 0.f; }
		std::string s( std::string_view n ) const
		{
			const dvar_value_t* v = find( n );
			if ( !v )
				return { };
			if ( v->m_def->m_kind == 4 )
				return v->m_s;
			char b[ 32 ];
			snprintf( b, sizeof( b ), "%g", v->m_v[ 0 ] );
			return b;
		}
		const float* color( std::string_view n ) const
		{
			static const float k_white[ 4 ] = { 1.f, 1.f, 1.f, 1.f };
			const dvar_value_t* v = find( n );
			return v ? v->m_v : k_white;
		}

		// "name value [value value value]\n" per line, only dvars that differ from stock are kept
		void load( std::string_view text )
		{
			reset( );
			size_t p = 0;
			while ( p < text.size( ) ) {
				size_t e = text.find( '\n', p );
				if ( e == std::string_view::npos )
					e = text.size( );
				const std::string line( text.substr( p, e - p ) );
				p = e + 1;

				const size_t sp = line.find( ' ' );
				if ( sp == std::string::npos )
					continue;
				const auto it = m_map.find( line.substr( 0, sp ) );
				if ( it == m_map.end( ) )
					continue;
				const std::string rest = line.substr( sp + 1 );
				if ( it->second.m_def->m_kind == 4 )
					it->second.m_s = rest;
				else
					sscanf_s( rest.c_str( ), "%f %f %f %f", &it->second.m_v[ 0 ], &it->second.m_v[ 1 ], &it->second.m_v[ 2 ], &it->second.m_v[ 3 ] );
			}
		}

		std::string save( ) const
		{
			std::string out{ };
			for ( const dvar_t& d : k_dvars ) {
				const dvar_value_t& v = m_map.at( d.m_name );
				char b[ 160 ];
				if ( d.m_kind == 4 ) {
					if ( v.m_s == ( d.m_string ? d.m_string : "" ) )
						continue;
					out += std::string( d.m_name ) + " " + v.m_s + "\n";
					continue;
				}
				const int n = d.m_kind == 3 ? 4 : 1;
				if ( std::memcmp( v.m_v, d.m_value, sizeof( float ) * n ) == 0 )
					continue;
				if ( n == 4 )
					snprintf( b, sizeof( b ), "%s %g %g %g %g\n", d.m_name, v.m_v[ 0 ], v.m_v[ 1 ], v.m_v[ 2 ], v.m_v[ 3 ] );
				else
					snprintf( b, sizeof( b ), "%s %g\n", d.m_name, v.m_v[ 0 ] );
				out += b;
			}
			return out;
		}

		void set( std::string_view n, float v )
		{
			const auto it = m_map.find( std::string( n ) );
			if ( it != m_map.end( ) )
				it->second.m_v[ 0 ] = v;
		}
		void set_string( std::string_view n, std::string v )
		{
			const auto it = m_map.find( std::string( n ) );
			if ( it != m_map.end( ) )
				it->second.m_s = std::move( v );
		}
	};

	/* ---- fonts (0x53d9e0 UI_GetFontHandle, glyph metrics from the font asset) ---- */
	enum e_font { font_big, font_small, font_console, font_bold, font_normal, font_extra_big, font_objective }; // k_fonts order

	// fontEnum: 0..2 default by scale, 3 small, 4 bold, 5 console, 6 objective. sy = ScrPlace vertical scale
	inline int pick_font( int font_enum, float scale, float sy, const dvars_t& dv )
	{
		if ( font_enum == 3 )
			return font_small;
		if ( font_enum == 5 )
			return font_console;
		if ( font_enum == 6 )
			return font_objective;
		const float real = sy * scale;
		if ( real <= dv.f( "ui_smallFont" ) )
			return font_small;
		if ( font_enum == 4 )
			return real > dv.f( "ui_bigFont" ) ? font_bold : font_normal;
		if ( real > dv.f( "ui_extraBigFont" ) )
			return font_extra_big;
		if ( real > dv.f( "ui_bigFont" ) )
			return font_big;
		return font_normal;
	}

	inline const glyph_t* glyph( int font, unsigned int letter )
	{
		const font_t& f     = k_fonts[ font ];
		const glyph_t* b    = k_glyphs + f.m_first;
		const glyph_t* e    = b + f.m_count;
		const glyph_t* it   = std::lower_bound( b, e, letter, [ ]( const glyph_t& g, unsigned int l ) { return g.m_letter < l; } );
		if ( it != e && it->m_letter == letter )
			return it;
		// R_GetCharacterGlyph falls back to '?'
		if ( letter != '?' )
			return glyph( font, '?' );
		return b;
	}

	// inline icon bytes (0x45b390): round(ratio * 32) clamped 16..127, + 16
	inline unsigned char icon_byte( float ratio )
	{
		int v = static_cast< int >( std::nearbyint( ratio * 32.f ) );
		if ( v > 0x7E )
			return 0x8F;
		if ( v < 0x11 )
			v = 0x10;
		return static_cast< unsigned char >( v + 0x10 );
	}

	// icon size in font pixels from that byte (0x5f3090)
	inline int icon_pixels( unsigned char b, int pixel_height )
	{
		const int v = ( static_cast< int >( b ) - 0x10 ) * pixel_height + 0x10;
		return ( v + ( v >> 31 & 0x1F ) ) >> 5;
	}

	// R_TextWidth (0x5d1b40 / 0x5d1d40) in font pixels: ^0..^9 skipped, ^\x01 / ^\x02 icons = 7 bytes
	inline int text_width( std::string_view s, int font )
	{
		int w = 0, best = 0;
		for ( size_t i = 0; i < s.size( ); ) {
			const unsigned char c = static_cast< unsigned char >( s[ i ] );
			if ( c == '\n' || c == '\r' ) {
				w = 0;
				++i;
				continue;
			}
			if ( c == '^' && i + 1 < s.size( ) ) {
				const char n = s[ i + 1 ];
				if ( n != '^' && n >= '0' && n <= '9' ) {
					i += 2;
					continue;
				}
				if ( ( n == 1 || n == 2 ) && i + 8 <= s.size( ) ) {
					w += icon_pixels( static_cast< unsigned char >( s[ i + 2 ] ), k_fonts[ font ].m_pixel_height );
					best = std::max( best, w );
					i += 8;
					continue;
				}
			}
			w += glyph( font, c )->m_dx;
			best = std::max( best, w );
			++i;
		}
		return best;
	}

	// "&&1" style localize args
	inline std::string localize( std::string_view key, std::initializer_list< std::string > args = { } )
	{
		if ( !key.empty( ) && key[ 0 ] == '@' )
			key.remove_prefix( 1 );
		std::string text( key );
		for ( const loc_t& s : k_strings )
			if ( key == s.m_key ) {
				text = s.m_text;
				break;
			}
		int n = 1;
		for ( const std::string& a : args ) {
			const std::string token = "&&" + std::to_string( n++ );
			const size_t at         = text.find( token );
			if ( at != std::string::npos )
				text.replace( at, token.size( ), a );
		}
		return text;
	}

	/* ---- game message windows (0x459ee0 add, 0x45d0d0 expire, 0x45d150 fade, 0x45d230 draw up) ---- */
	struct msg_line_t {
		std::string m_text{ };
		int m_start = 0, m_end = 0; // ms
	};

	struct msg_window_t {
		std::deque< msg_line_t > m_lines{ };

		void add( std::string text, int now, int msg_time, int line_count, int fade_out )
		{
			const size_t size = static_cast< size_t >( line_count + 3 );
			while ( m_lines.size( ) >= size )
				m_lines.pop_front( );
			m_lines.push_back( { std::move( text ), now, now + msg_time } );

			// one past the visible count: the oldest still on fades out now
			const int over = static_cast< int >( m_lines.size( ) ) - line_count;
			if ( over > 0 ) {
				msg_line_t& l = m_lines[ static_cast< size_t >( over - 1 ) ];
				if ( now < l.m_end - fade_out )
					l.m_end = now + fade_out;
			}
		}

		void expire( int now )
		{
			while ( !m_lines.empty( ) && now - m_lines.front( ).m_end >= 0 )
				m_lines.pop_front( );
		}

		static float fade( const msg_line_t& l, int now, int scroll, int fade_in, int fade_out )
		{
			float a = 1.f;
			if ( l.m_end - now < fade_out )
				a = static_cast< float >( l.m_end - now ) / static_cast< float >( fade_out );

			const int age = now - l.m_start;
			if ( scroll <= fade_in ) {
				if ( fade_in && age < fade_in )
					a *= static_cast< float >( age ) / static_cast< float >( fade_in );
			}
			else if ( age < scroll ) {
				if ( age <= scroll - fade_in )
					return 0.f;
				a *= static_cast< float >( fade_in - scroll + age ) / static_cast< float >( fade_in );
			}
			return a < 0.f ? 0.f : a;
		}

		struct row_t {
			const msg_line_t* m_line;
			int m_y;
			float m_alpha;
		};

		// mode 1 (gamemsgwindowmode 1): newest one line above y, older stacking up, new lines slide in from below
		std::vector< row_t > rows_up( int now, int y, int line_height, int scroll, int fade_in, int fade_out ) const
		{
			for ( const msg_line_t& l : m_lines ) {
				const int age = now - l.m_start;
				if ( age < scroll ) {
					float f = 1.f - static_cast< float >( age ) / static_cast< float >( scroll );
					f       = std::clamp( f, 0.f, 1.f );
					y += static_cast< int >( std::nearbyint( static_cast< float >( line_height ) * f ) );
				}
			}
			std::vector< row_t > out{ };
			for ( size_t i = m_lines.size( ); i-- > 0; ) {
				y -= line_height;
				const msg_line_t& l = m_lines[ i ];
				if ( now - l.m_end < 0 )
					out.push_back( { &l, y, fade( l, now, scroll, fade_in, fade_out ) } );
			}
			return out;
		}
	};

	// gamemsgwindow line height (0x45d740): round(textscale * 48) virtual units
	inline int msg_line_height( float text_scale ) { return static_cast< int >( std::nearbyint( text_scale * 48.f ) ); }

	/* ---- low health overlay (0x43fca0 phases, 0x43fc30 lerp) ---- */
	struct health_overlay_t {
		float m_last_frac = 1.f;
		int m_hurt_time   = 0;
		int m_pulse       = 0;  // 0..3 pulse peaks, 4 = end fade
		int m_phase       = 0;  // 0 ramp up, 1 to phaseTwo, 2 to phaseThree
		bool m_active     = false;
		float m_from = 0.f, m_to = 0.f;
		int m_start = 0, m_duration = 0;

		float alpha( int now ) const
		{
			int t = now - m_start;
			if ( t < 0 )
				t = 0;
			if ( m_duration > 0 && t < m_duration )
				return ( m_to - m_from ) * ( static_cast< float >( t ) / static_cast< float >( m_duration ) ) + m_from;
			return m_to;
		}

		void update( float frac, int now, const dvars_t& dv )
		{
			static constexpr float k_peaks[ 4 ] = { 1.f, 0.8f, 0.6f, 0.3f };
			const float pulse_start = dv.f( "hud_healthOverlay_pulseStart" );

			if ( frac < m_last_frac && frac < pulse_start ) {
				m_hurt_time = now;
				m_pulse     = 0;
			}
			m_last_frac = frac;

			if ( now < m_start + m_duration || ( pulse_start <= frac && !m_active ) )
				return;
			m_active = true;
			m_from   = m_to;
			m_start  = now;

			if ( m_pulse >= 4 ) {
				m_active   = false;
				m_to       = dv.f( "hud_healthOverlay_phaseEnd_toAlpha" );
				m_duration = dv.i( "hud_healthOverlay_phaseEnd_pulseDuration" );
				m_phase    = 0;
				return;
			}

			const int one = dv.i( "hud_healthOverlay_phaseOne_pulseDuration" ), two = dv.i( "hud_healthOverlay_phaseTwo_pulseDuration" ),
			          three = dv.i( "hud_healthOverlay_phaseThree_pulseDuration" );
			if ( m_phase == 0 ) {
				m_to       = std::clamp( k_peaks[ m_pulse ], 0.f, 1.f );
				m_duration = one;
				m_phase    = 1;
			}
			else if ( m_phase == 1 ) {
				m_to       = std::clamp( k_peaks[ m_pulse ] * dv.f( "hud_healthOverlay_phaseTwo_toAlphaMultiplier" ), 0.f, 1.f );
				m_duration = two;
				m_phase    = 2;
			}
			else {
				m_to       = std::clamp( k_peaks[ m_pulse ] * dv.f( "hud_healthOverlay_phaseThree_toAlphaMultiplier" ), 0.f, 1.f );
				m_duration = three;
				m_phase    = 0;
				if ( m_hurt_time - ( one + two + three ) * 3 + dv.i( "hud_healthOverlay_regenPauseTime" ) <= now )
					++m_pulse;
			}
		}
	};

	/* ---- compass (0x424a50 CG_WorldPosToCompass, 0x424c80 rect) ---- */
	// rect: x y w h (real px). north = unit forward of the compass "up" in world xy
	inline bool world_to_compass( const float rect[ 4 ], float max_range, const float north[ 2 ], const float player[ 2 ], const float pos[ 2 ], float out[ 2 ] )
	{
		const float s  = rect[ 3 ] / max_range;
		const float dx = s * ( pos[ 0 ] - player[ 0 ] ), dy = s * ( pos[ 1 ] - player[ 1 ] );
		float x = dx * north[ 1 ] - dy * north[ 0 ];
		float y = -north[ 1 ] * dy - north[ 0 ] * dx;
		out[ 0 ] = x, out[ 1 ] = y;

		bool clamped  = false;
		const float hw = rect[ 2 ] * 0.5f, hh = rect[ 3 ] * 0.5f;
		if ( x > hw ) {
			const float k = hw / x;
			out[ 0 ] = k * x, out[ 1 ] = k * y, clamped = true;
		}
		else if ( x < -hw ) {
			const float k = -( hw / x );
			out[ 0 ] = k * x, out[ 1 ] = k * y, clamped = true;
		}
		if ( out[ 1 ] > hh ) {
			const float k = hh / out[ 1 ];
			out[ 0 ] *= k, out[ 1 ] *= k, clamped = true;
		}
		else if ( out[ 1 ] < -hh ) {
			const float k = -( hh / out[ 1 ] );
			out[ 0 ] *= k, out[ 1 ] *= k, clamped = true;
		}
		return clamped;
	}

	// AngleNormalize360 (0x55bcd0) / AngleDelta (0x4010d0)
	inline float angle_normalize_360( float a )
	{
		const float v = ( a * ( 1.f / 360.f ) - std::floor( a * ( 1.f / 360.f ) ) ) * 360.f;
		return v - 360.f >= 0.f ? v - 360.f : v;
	}
	inline float angle_delta( float a, float b )
	{
		const float v = ( a - b ) * ( 1.f / 360.f );
		return ( v - std::floor( v + 0.5f ) ) * 360.f;
	}

	/* ---- crosshair (0x42f370 offset, 0x42f440 alpha) ---- */
	// spread in degrees -> reticle side offset in virtual units
	inline float reticle_offset( float spread_deg, float tan_half_fov_y, int min_ofs )
	{
		const float v = std::tan( spread_deg * 0.0174532924f ) * 240.f / tan_half_fov_y;
		return v < static_cast< float >( min_ofs ) ? static_cast< float >( min_ofs ) : v;
	}

	struct aim_spread_in_t {
		float m_dt;            // frame seconds
		float m_turn[ 2 ];     // view pitch / yaw change this frame, degrees
		float m_speed;         // horizontal velocity
		float m_speed_full;    // g_speed stand-in (weapon max speed)
		bool m_move_keys;      // forwardmove || rightmove
		bool m_in_air, m_ducked;
	};

	// PM_UpdateAimSpread (0x417580), hip only (prone / spreadOverride / ads frac dropped). any increase = no decay that frame
	inline float aim_spread_update( float scale, const n_cod4_data::weapon_t& w, const aim_spread_in_t& in )
	{
		if ( !( scale == scale ) )
			scale = 0.f;
		if ( w.m_hip_decay == 0.f || !( in.m_dt > 0.f ) )
			return scale;
		const float decay = w.m_hip_decay * ( in.m_in_air ? 0.5f : in.m_ducked ? w.m_hip_ducked_decay : 1.f );
		float increase    = 0.f;
		if ( w.m_hip_turn_add != 0.f )
			for ( const float turn : in.m_turn )
				increase += std::fabs( turn ) * 0.01f * w.m_hip_turn_add / in.m_dt;
		// bg_aimSpreadMoveSpeedThreshold 11
		if ( w.m_hip_move_add != 0.f && in.m_move_keys && in.m_speed > 11.f )
			increase += in.m_speed * w.m_hip_move_add / std::max( in.m_speed_full, 1.f );
		if ( in.m_in_air )
			increase += 1.28f + 1.28f; // 0x4177ad adds the constant twice
		scale += increase * in.m_dt > 0.f ? increase * in.m_dt * 255.f : -decay * in.m_dt * 255.f;
		return scale > 255.f ? 255.f : scale < 0.f ? 0.f : scale;
	}

	// 0x418210, per shot from the hip
	inline float aim_spread_fire( float scale, const n_cod4_data::weapon_t& w, int shots )
	{
		scale += w.m_hip_fire_add * 255.f * static_cast< float >( shots > 0 ? shots : 0 );
		return scale > 255.f ? 255.f : scale;
	}

	inline float reticle_alpha( float alpha, float fade, float spread_scale_255, float alpha_min )
	{
		const float a = alpha * fade * ( 1.f - spread_scale_255 / 255.f );
		return a < alpha_min ? alpha_min : a;
	}

	// csgo chat -> game message window line. stock notice bytes 1..16 = colour (3 + text before the first = sender's team),
	// html = tags dropped. cod4 fonts are latin: one '?' per non-ascii codepoint, '^' dropped (no injected colour codes)
	inline std::string chat_line( std::string_view text, bool html, int sender_team, int my_team )
	{
		static constexpr char k_codes[ 17 ] = { '7', '7', '1', '7', '2', '2', '2', '1', '7', '3', '5', '5', '4', '6', '6', '1', '3' };
		const bool sides = ( sender_team == 2 || sender_team == 3 ) && ( my_team == 2 || my_team == 3 );
		const char team  = !sides ? '7' : sender_team == my_team ? '8' : '9';
		std::string out{ };
		char current = 0;
		const auto color = [ & ]( char c ) {
			if ( c != current )
				out += '^', out += c, current = c;
		};
		color( html ? '7' : team );
		for ( size_t i = 0; i < text.size( ); ) {
			const auto b = static_cast< unsigned char >( text[ i ] );
			if ( html && b == '<' ) {
				const size_t close = text.find( '>', i );
				if ( close == std::string_view::npos )
					break;
				i = close + 1;
				continue;
			}
			if ( html && b == '&' ) {
				static constexpr std::pair< std::string_view, char > k_entities[] = { { "&amp;", '&' }, { "&lt;", '<' },   { "&gt;", '>' },  { "&quot;", '"' },
					                                                                  { "&#39;", '\'' }, { "&apos;", '\'' }, { "&nbsp;", ' ' } };
				bool hit = false;
				for ( const auto& [ name, c ] : k_entities )
					if ( text.substr( i, name.size( ) ) == name ) {
						out += c, i += name.size( ), hit = true;
						break;
					}
				if ( hit )
					continue;
			}
			if ( !html && b > 0 && b < 17 ) {
				color( b == 3 ? team : k_codes[ b ] );
				++i;
				continue;
			}
			++i;
			if ( b >= 0x80 ) {
				while ( i < text.size( ) && ( static_cast< unsigned char >( text[ i ] ) & 0xC0 ) == 0x80 )
					++i;
				out += '?';
			}
			else if ( b >= 32 && b != '^' )
				out += static_cast< char >( b );
		}
		return out;
	}

	/* ---- weapon map ---- */
	inline const weapon_t* weapon_for( std::string_view cs )
	{
		for ( const weapon_t& w : k_weapons )
			if ( cs == w.m_cs )
				return &w;
		return nullptr;
	}

	inline int material_index( std::string_view name )
	{
		for ( int i = 0; i < static_cast< int >( std::size( k_materials ) ); ++i )
			if ( name == k_materials[ i ].m_name )
				return i;
		return -1;
	}

	// killIcon / hudIcon ratio -> obituary icon size (0x433b60): 1:1 1.4x1.4, 2:1 2.8x1.4, 4:1 2.8x0.7
	inline void kill_icon_ratio( int ratio, float& w, float& h )
	{
		w = ratio == 0 ? 1.4f : 2.8f;
		h = ratio == 2 ? 0.7f : 1.4f;
	}

	// icon material for the cs2 jumpshot wings ( killer off the ground ), drawn as a mesh, never a material / glyph
	inline constexpr int k_air_wings_material = -0x7fffffff - 1;

	// obituary line (0x45b3e0): "^<c>attacker^7 " + "^\x01|\x02 w h [material]" + " ^<c>victim".
	// material < 0 = a csgo weapon icon glyph (-codepoint) the hud draws outlined; extra = a second icon right after it
	// (the headshot icon next to a csgo weapon icon); air = the wings icon right before the weapon icon
	inline std::string obituary_line( std::string_view attacker, char attacker_color, std::string_view victim, char victim_color, int material, float w,
	                                  float h, bool flip, int extra = -1, bool air = false )
	{
		std::string s{ };
		if ( !attacker.empty( ) ) {
			s += '^';
			s += attacker_color;
			s += attacker;
			s += "^7 ";
		}
		if ( air ) {
			s += '^';
			s += static_cast< char >( 1 );
			s += static_cast< char >( icon_byte( 1.4f ) );
			s += static_cast< char >( icon_byte( 1.4f ) );
			s.append( reinterpret_cast< const char* >( &k_air_wings_material ), 4 );
		}
		s += '^';
		s += static_cast< char >( flip ? 2 : 1 );
		s += static_cast< char >( icon_byte( w ) );
		s += static_cast< char >( icon_byte( h ) );
		const int m = material;
		s.append( reinterpret_cast< const char* >( &m ), 4 );
		if ( extra >= 0 ) {
			s += '^';
			s += static_cast< char >( 1 );
			s += static_cast< char >( icon_byte( 1.4f ) );
			s += static_cast< char >( icon_byte( h ) );
			s.append( reinterpret_cast< const char* >( &extra ), 4 );
		}
		s += " ^";
		s += victim_color;
		s += victim;
		return s;
	}

	// rank from xp (mp/rankTable.csv minXp column, k_rank_xp ascending): stat 2351 / 2352 = that rank's min / max
	inline void rank_bounds( int xp, int& lo, int& hi )
	{
		lo = 0, hi = k_rank_xp[ 1 ];
		for ( size_t i = 0; i + 1 < std::size( k_rank_xp ); ++i )
			if ( xp >= k_rank_xp[ i ] ) {
				lo = k_rank_xp[ i ];
				hi = k_rank_xp[ i + 1 ];
			}
	}
}
