#pragma once
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>
#include <cstdint>

/* pure cgame logic, no game / d3d deps (tools/urt_hud_test.cpp builds it alone). function names = the qvm's */
namespace n_urt_logic
{
	/* ---- csgo weapon -> urt weapon id (bg weapon table order) ---- */
	enum e_weapon {
		wp_none = 0, wp_knife, wp_beretta, wp_deagle, wp_spas, wp_mp5k, wp_ump45, wp_hk69, wp_lr300, wp_g36, wp_psg1, wp_he, wp_flash, wp_smoke,
		wp_sr8, wp_ak103, wp_bomb, wp_negev, wp_18, wp_m4, wp_glock, wp_colt, wp_mac11, wp_frf1, wp_benelli, wp_p90, wp_magnum, wp_tod50,
	};

	// by role: pistols / shotguns / smgs / rifles / snipers / mgs onto the closest urt gun
	inline int weapon_for( std::string_view cs )
	{
		struct map_t { std::string_view m_cs; int m_urt; };
		static constexpr map_t k_map[] = {
			{ "weapon_knife", wp_knife },        { "weapon_knife_t", wp_knife },      { "weapon_bayonet", wp_knife },     { "weapon_glock", wp_glock },
			{ "weapon_hkp2000", wp_beretta },    { "weapon_usp_silencer", wp_beretta }, { "weapon_elite", wp_beretta },   { "weapon_p250", wp_colt },
			{ "weapon_fiveseven", wp_colt },     { "weapon_cz75a", wp_colt },         { "weapon_tec9", wp_glock },        { "weapon_deagle", wp_deagle },
			{ "weapon_revolver", wp_magnum },    { "weapon_nova", wp_spas },          { "weapon_sawedoff", wp_spas },     { "weapon_mag7", wp_spas },
			{ "weapon_xm1014", wp_benelli },     { "weapon_mac10", wp_mac11 },        { "weapon_mp9", wp_mp5k },          { "weapon_mp7", wp_mp5k },
			{ "weapon_mp5sd", wp_mp5k },         { "weapon_ump45", wp_ump45 },        { "weapon_p90", wp_p90 },           { "weapon_bizon", wp_p90 },
			{ "weapon_ak47", wp_ak103 },         { "weapon_galilar", wp_ak103 },      { "weapon_m4a1", wp_m4 },           { "weapon_m4a1_silencer", wp_lr300 },
			{ "weapon_famas", wp_lr300 },        { "weapon_aug", wp_g36 },            { "weapon_sg556", wp_g36 },         { "weapon_awp", wp_sr8 },
			{ "weapon_ssg08", wp_frf1 },         { "weapon_g3sg1", wp_psg1 },         { "weapon_scar20", wp_psg1 },       { "weapon_negev", wp_negev },
			{ "weapon_m249", wp_negev },         { "weapon_hegrenade", wp_he },       { "weapon_molotov", wp_he },        { "weapon_incgrenade", wp_he },
			{ "weapon_flashbang", wp_flash },    { "weapon_smokegrenade", wp_smoke }, { "weapon_decoy", wp_smoke },       { "weapon_c4", wp_bomb },
		};
		for ( const map_t& m : k_map )
			if ( m.m_cs == cs )
				return m.m_urt;
		if ( cs.find( "knife" ) != std::string_view::npos || cs.find( "bayonet" ) != std::string_view::npos )
			return wp_knife;
		return wp_none;
	}

	// bg weapon table +4 short name: icons/weapons/<name> (weaponInfo +68, the select wheel), icons/ammo/<name> (+72, the ammo box)
	inline const char* weapon_short( int id )
	{
		static constexpr const char* k_short[] = { "none",   "knife",       "beretta", "deserteagle", "spas12",        "mp5k",       "ump45",
			                                       "hk69",   "lr",          "g36",     "psg1",        "grenade_he",    "grenade_flash", "grenade_smoke",
			                                       "sr8",    "ak103",       "bomb",    "negev",       "none",          "m4",         "glock",
			                                       "colt1911", "mac11",     "frf1",    "benellim9",   "p90",           "44magnum",   "tod50" };
		return id >= 0 && id < static_cast< int >( std::size( k_short ) ) ? k_short[ id ] : "none";
	}

	/* ---- CG_Obituary (sub_92169) ---- */
	struct flavor_t { const char* m_verb; const char* m_suffix; const char* m_tag; };

	// player vs player, per urt weapon (flavor 0 / 1 where urt has two)
	inline flavor_t flavor_for( int id, int flavor )
	{
		switch ( id ) {
		case wp_knife: return { "was sliced a new orifice by", nullptr, "Knife" };
		case wp_beretta: return { "was pistol whipped by", nullptr, "Beretta" };
		case wp_magnum: return { "was ripped apart by", "'s Magnum.", "Magnum" };
		case wp_glock: return { "got plastic surgery with", "'s Glock.", "Glock" };
		case wp_deagle: return { "got a whole lot of hole from", "'s DE round.", "Desert Eagle" };
		case wp_colt: return { "was given a new breathing hole by", "'s Colt 1911.", "Colt 1911" };
		case wp_spas: return { "was turned into peppered steak by", "'s SPAS blast.", "SPAS" };
		case wp_benelli: return { "was turned into grated cheese by", "'s Benelli.", "Benelli" };
		case wp_ump45: return { "danced the UMP tango to", "'s sweet sweet music.", "UMP45" };
		case wp_mp5k: return { "was MP5K spammed without mercy by", nullptr, "MP5K" };
		case wp_mac11: return { "was minced to death by", "'s Mac 11.", "Mac 11" };
		case wp_p90: return { "was filled with P90 rounds by", nullptr, "P90" };
		case wp_lr300: return { "played 'catch the shiny bullet' with", "'s LR-300 rounds.", "LR300" };
		case wp_g36: return { "was on the wrong end of", "'s G36.", "G36" };
		case wp_ak103: return { "was torn asunder by", "'s crass AK103.", "AK103" };
		case wp_m4: return { "got a lead enema from", "'s retro M4.", "M4" };
		case wp_psg1: return { "was taken out by", "'s PSG1. Plink!", "PSG1" };
		case wp_hk69: return flavor ? flavor_t{ "suffered", "'s HK69 Grenade Enema.", "HK69" } : flavor_t{ "HEARD", "'s HK69 gren... didn't AVOID it. Sucka.", "HK69" };
		case wp_flash: return { "was flashed by ", " and dropped dead.", nullptr };
		case wp_smoke: return { "was smoked by", "and dropped dead.", "Smoke" };
		case wp_he: return flavor ? flavor_t{ "was BBQ'ed by", "'s exploding monkey.", "HE" } : flavor_t{ "has become a nasty stain thanks to", "'s HE gren.", "HE" };
		case wp_sr8: return { "managed to slow down", flavor ? "'s SR-8 round just a little. NEEP NEEP!" : "'s SR-8 round just a little.", "SR8" };
		case wp_frf1: return { flavor ? "was wiped off the map by" : "was french fried by", "'s FRF1.", "FRF1" };
		case wp_negev: return { flavor ? "got nailed to the wall by" : "got shredded to pieces by", "'s Negev.", "Negev" };
		case wp_tod50: return { "was gibbed by", nullptr, "Tod50" };
		case -1: return flavor ? flavor_t{ "got", "'s foot broken off in their ass.", "Boot" } : flavor_t{ "got himself some lovin' from", "'s boot o' passion.", "Boot" };
		default: return { "was retired from the field of combat by", nullptr, nullptr };
		}
	}

	inline std::string va( const char* fmt, ... )
	{
		char buf[ 1024 ];
		va_list ap;
		va_start( ap, fmt );
		vsnprintf( buf, sizeof( buf ), fmt, ap );
		va_end( ap );
		return buf;
	}

	inline const char* ordinal( int n )
	{
		const int t = n % 100;
		if ( t >= 11 && t <= 13 )
			return "th";
		return n % 10 == 1 ? "st" : n % 10 == 2 ? "nd" : n % 10 == 3 ? "rd" : "th";
	}

	enum e_death { death_player = 0, death_suicide, death_fall, death_bomb, death_world };

	struct obituary_t {
		std::string m_victim{ }, m_attacker{ }; // already "<team code>name^7"
		int m_kind      = death_player;
		int m_weapon    = wp_none;  // -1 = boot (zeus)
		int m_flavor    = 0;
		bool m_me_victim = false, m_me_attacker = false;
		int m_gender    = 0;        // clientinfo+472: 0 him, 1 her
		int m_attacker_health = 255; // % left, 255 = n/a
		int m_place = 1, m_ref_score = 0, m_ref_team = 0; // cg_drawKillLog >= 5
		bool m_air = false; // killer off the ground: byte 1 ( cs2 wings, urt_core.h ) left of the weapon tag / killer name
	};

	struct obituary_out_t {
		std::string m_msg[ 2 ]{ }; // message box lines (CG_Printf)
		std::string m_center{ };   // CG_CenterPrint( , 120, 16 )
	};

	inline obituary_out_t obituary( const obituary_t& o, int mode )
	{
		obituary_out_t out{ };
		if ( mode <= 0 )
			return out;
		if ( o.m_kind == death_bomb ) {
			out.m_msg[ 0 ] = va( "%s was bombed.", o.m_victim.c_str( ) );
			return out;
		}
		if ( o.m_kind == death_suicide || o.m_kind == death_fall ) {
			const char* text = o.m_kind == death_fall ? "did the lemming thing."
			                 : o.m_weapon == wp_he     ? ( o.m_gender == 1 ? "stepped on her own grenade." : "stepped on his own grenade." )
			                 : o.m_gender == 1         ? "killed herself."
			                 : o.m_gender == 0         ? "killed himself."
			                                           : "killed itself.";
			out.m_msg[ 0 ] = va( "%s %s", o.m_victim.c_str( ), text );
			return out;
		}
		if ( o.m_kind == death_world ) {
			out.m_msg[ 0 ] = va( "%s %s", o.m_victim.c_str( ), "was killed by some well placed HURT!" );
			return out;
		}

		const flavor_t f = flavor_for( o.m_weapon, o.m_flavor );
		if ( !f.m_tag )
			return out; // no tag = no kill log line in any mode (flash kills, as in 4.3)
		const char* suffix = f.m_suffix ? f.m_suffix : ".";
		const char* w      = o.m_air ? "\x01" : ""; // wings: left of the weapon tag, else of the killer name
		const auto health  = [ & ]( int slot ) {
			if ( o.m_me_victim && o.m_attacker_health != 255 )
				out.m_msg[ slot ] = va( "%s had %d%% health.", o.m_attacker.c_str( ), o.m_attacker_health );
		};
		switch ( mode ) {
		case 1:
			out.m_msg[ 0 ] = va( "%s %s %s%s^7%s", o.m_victim.c_str( ), f.m_verb, w, o.m_attacker.c_str( ), suffix );
			health( 1 );
			break;
		case 2:
			out.m_msg[ 0 ] = va( "%s %s[%s] %s^7.", o.m_attacker.c_str( ), w, f.m_tag, o.m_victim.c_str( ) );
			health( 1 );
			break;
		case 3: out.m_msg[ 0 ] = va( "%s has been killed by %s%s^7.", o.m_victim.c_str( ), w, o.m_attacker.c_str( ) ); break;
		case 4:
			if ( o.m_me_attacker )
				out.m_center = va( "%sYou killed %s", w, o.m_victim.c_str( ) );
			else if ( o.m_me_victim )
				out.m_center = va( "You have been killed by %s%s", w, o.m_attacker.c_str( ) );
			else
				out.m_msg[ 0 ] = va( "%s has been killed by %s%s^7.", o.m_victim.c_str( ), w, o.m_attacker.c_str( ) );
			break;
		default: {
			const char* color = o.m_ref_team == 1 ? "^1" : o.m_ref_team == 2 ? "^4" : "^3";
			if ( o.m_me_attacker )
				out.m_center = va( "%sYou killed %s\n%s%d%s %splace with %d", w, o.m_victim.c_str( ), color, o.m_place, ordinal( o.m_place ), "^7", o.m_ref_score );
			else if ( o.m_me_victim )
				out.m_center = va( "You have been killed by %s%s\n%s%d%s %splace with %d", w, o.m_attacker.c_str( ), color, o.m_place, ordinal( o.m_place ), "^7",
				                   o.m_ref_score );
			else
				out.m_msg[ 0 ] = va( "%s has been killed by %s%s^7.", o.m_victim.c_str( ), w, o.m_attacker.c_str( ) );
			break;
		}
		}
		return out;
	}

	/* ---- timer strings (sub_25365 / sub_24950 family): "%i:%i%i" ---- */
	inline std::string clock( int ms )
	{
		if ( ms <= 0 )
			return "0:00";
		int s       = ms / 1000;
		const int m = s / 60;
		s %= 60;
		return va( "%i:%i%i", m, s / 10, s % 10 );
	}

	// sub_24950, round based: "round of maxrounds | round clock", else the clock alone
	inline std::string round_timer( int round, int max_rounds, int round_ms )
	{
		if ( max_rounds != 0 ) {
			const int cur = std::clamp( round == 0 ? 1 : round, 1, max_rounds );
			return va( "%i of %i | %s", cur, max_rounds, clock( round_ms ).c_str( ) );
		}
		return clock( round_ms );
	}

	/* ---- crosshair (sub_28163): per weapon gaps, styles 10-12 ---- */
	struct gaps_t { float m_side = 0.f, m_top = 0.f, m_bottom = 0.f, m_size_add = 0.f; };

	inline gaps_t cross_gaps( int id, float pulse )
	{
		gaps_t g{ };
		const auto even = [ & ]( float mul, float base, float add ) { g.m_side = g.m_top = g.m_bottom = pulse * mul + base, g.m_size_add = pulse * add; };
		const auto rifle = [ & ]( float side, float top, float base, float add ) {
			g.m_side = g.m_bottom = pulse * side + base, g.m_top = pulse * top + base, g.m_size_add = pulse * add;
		};
		switch ( id ) {
		case wp_knife: case wp_magnum: g.m_side = g.m_top = g.m_bottom = 10.f; break;
		case wp_beretta: case wp_deagle: case wp_spas: case wp_mp5k: case wp_glock: case wp_colt: even( 25.f, 5.f, 10.f ); break;
		case wp_ump45: rifle( 27.5f, 35.f, 4.f, 5.f ); break;
		case wp_hk69: g.m_side = g.m_top = g.m_bottom = 8.f; break;
		case wp_lr300: rifle( 15.f, 20.f, 3.f, 5.f ); break;
		case wp_g36: even( 15.f, 3.f, 5.f ); break;
		case wp_he: case wp_smoke: g.m_side = g.m_top = g.m_bottom = 9.f; break;
		case wp_ak103: case wp_m4: case wp_mac11: rifle( 27.5f, 40.f, 3.f, 10.f ); break;
		case wp_negev: rifle( 70.f, 100.f, 4.f, 30.f ); break;
		case wp_benelli: even( 70.f, 8.f, 30.f ); break;
		case wp_p90: even( 15.f, 4.f, 5.f ); break;
		default: break; // snipers, flash, bomb: no gap
		}
		g.m_side = std::min( g.m_side, 100.f ), g.m_top = std::min( g.m_top, 100.f ), g.m_bottom = std::min( g.m_bottom, 100.f );
		return g;
	}

	// sub_29236 spread (dynamic styles 1-8), before the * scale
	inline float dynamic_spread( float base, float move, float kick, int acc_flags, float pulse, bool ducked, bool zoomed, float speed )
	{
		if ( acc_flags & 16 )
			return 0.f;
		float s = pulse * kick + base * ( ducked ? 0.75f : 1.f ) * ( zoomed ? 0.75f : 1.f );
		const float under = std::min( speed, 200.f ), over = std::max( speed - 200.f, 0.f );
		s += under * 0.01f * move * 0.4f + over * 0.01f * move * 1.6f;
		return s * 6.f;
	}

	// sub_27919 ring gap (style 9)
	inline float ring_gap( bool airborne, float speed, float pulse )
	{
		if ( airborne || speed >= 145.f )
			return 8.f;
		if ( speed <= 105.f )
			return pulse * 6.f + 2.f;
		return std::min( ( ( speed - 105.f ) * 0.025f + pulse ) * 6.f + 2.f, 8.f );
	}

	/* ---- minimap (sub_159893): box corner per cg_mappos, 0 = hidden ---- */
	inline bool map_box( int pos, int size, float l212, float l224, int& x, int& y )
	{
		const float center = 320.f - ( static_cast< float >( size ) + l212 ) * 0.5f + l212;
		switch ( pos ) {
		case 1: x = static_cast< int >( l212 ), y = 0; return true;
		case 2: x = static_cast< int >( l212 ), y = 240 - size / 2; return true;
		case 3: x = static_cast< int >( l212 ), y = static_cast< int >( static_cast< float >( 400 - size ) - l224 ); return true;
		case 4: x = static_cast< int >( l212 + 60.f ), y = static_cast< int >( static_cast< float >( 480 - size ) - l224 ); return true;
		case 5: x = static_cast< int >( center ), y = static_cast< int >( static_cast< float >( 480 - size ) - l224 ); return true;
		case 6: x = 640 - size, y = static_cast< int >( static_cast< float >( 440 - size ) - l224 ); return true;
		case 7: x = 640 - size, y = 240 - size / 2; return true;
		case 8: x = 640 - size, y = 75; return true;
		case 9: x = 520 - size, y = 0; return true;
		case 10: x = static_cast< int >( center ), y = 0; return true;
		case 11: x = static_cast< int >( center ), y = 240 - size / 2; return true;
		default: return false;
		}
	}

	// arrow atlas cell: 64 yaw steps, 8x8
	inline int arrow_cell( float yaw )
	{
		int n = static_cast< int >( ( -yaw - 90.f ) * 0.177778f );
		if ( n > 63 )
			n -= 64;
		if ( n < 0 )
			n += 64;
		return n;
	}

	// sub_159275 pixel: green by height, 0 = transparent. bgra
	inline unsigned int map_pixel( float z, float min_z, float span_z )
	{
		const float h = ( z - min_z ) / span_z * 256.f;
		unsigned int g = h >= 2147483648.f ? static_cast< unsigned int >( static_cast< int >( h - 2147483648.f ) ) + 0x80000000u : static_cast< unsigned int >( static_cast< int >( h ) );
		if ( g > 255u )
			g = 255u;
		return g ? ( g << 8 ) | 0xFF000000u : 0u;
	}

	/* ---- chat / message ring (sub_146863 / sub_147294) ---- */
	struct ring_t {
		std::string m_text[ 8 ]{ };
		int m_time[ 8 ]{ };
		int m_head = 0, m_tail = 0;
	};

	// 96 char chunks, break on a space up to 10 back, "-" when cut mid word. chat chunks restart ^3, msg chunks carry the last code
	inline void ring_push( ring_t& r, std::string text, int height, int life, int now, bool chat )
	{
		const int slots = std::clamp( height, 0, 8 );
		if ( slots <= 0 || life <= 0 ) {
			r.m_head = r.m_tail = 0;
			return;
		}
		if ( chat )
			std::replace( text.begin( ), text.end( ), '\x1f', '%' );
		char code = '7';
		bool first = true;
		size_t at  = 0;
		while ( at < text.size( ) || first ) {
			size_t len = text.size( ) - at;
			bool hyphen = false;
			if ( len > 96 ) {
				size_t cut = 96;
				for ( size_t k = 95; k > 85; --k )
					if ( text[ at + k ] == ' ' ) {
						cut = k;
						break;
					}
				hyphen = cut == 96 && text[ at + 95 ] != ' ';
				len    = cut;
			}
			std::string chunk = text.substr( at, len );
			at += len;
			while ( at < text.size( ) && text[ at ] == ' ' )
				++at;
			if ( hyphen )
				chunk += '-';
			std::string line = chat ? "^3" + chunk : ( first ? chunk : std::string( "^" ) + code + chunk );
			for ( size_t k = 0; k + 1 < line.size( ); ++k )
				if ( line[ k ] == '^' && line[ k + 1 ] >= '0' && line[ k + 1 ] <= '9' )
					code = line[ k + 1 ];
			r.m_text[ r.m_tail % slots ] = std::move( line );
			r.m_time[ r.m_tail % slots ] = now;
			++r.m_tail;
			if ( r.m_tail - r.m_head > slots )
				r.m_head = r.m_tail - slots;
			first = false;
		}
	}

	/* ---- team skins (sub_114235): 0 free, 1 blue, 2 red, cg_skinAlly / cg_skinEnemy 1..14 = the skin table ---- */
	inline int skin_for( int team, int my_team, int ally, int enemy, bool fixed )
	{
		// urt teams: 1 red, 2 blue, 3 spectator
		const int base = team == 2 ? 1 : team == 1 ? 2 : 0;
		if ( team != 1 && team != 2 )
			return 0;
		const int v = fixed || my_team == 3 ? ( team == 2 ? ally : enemy ) : ( team == my_team ? ally : enemy );
		static constexpr int k_cases[ 15 ] = { -1, 0, 2, 1, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13 };
		return v >= 1 && v <= 14 ? k_cases[ v ] : base;
	}

	// LoadTGA (ioq3 tr_image_tga.c): types 2 / 3 / 10 / 11, 8 / 24 / 32 bpp, origin bit 5 -> bgra top row first
	inline bool decode_tga( const std::vector< std::uint8_t >& f, int& w, int& h, std::vector< std::uint8_t >& out )
	{
		if ( f.size( ) < 18 )
			return false;
		const int id_len = f[ 0 ], type = f[ 2 ], bpp = f[ 16 ], desc = f[ 17 ];
		w = f[ 12 ] | f[ 13 ] << 8, h = f[ 14 ] | f[ 15 ] << 8;
		if ( ( type != 2 && type != 3 && type != 10 && type != 11 ) || w <= 0 || h <= 0 || f[ 1 ] != 0 || ( bpp != 8 && bpp != 24 && bpp != 32 ) )
			return false;
		const int px = bpp / 8;
		size_t at    = 18 + static_cast< size_t >( id_len );
		std::vector< std::uint8_t > raw( static_cast< size_t >( w ) * h * 4 );
		const auto put = [ & ]( size_t i, const std::uint8_t* p ) {
			std::uint8_t* d = raw.data( ) + i * 4;
			if ( px == 1 )
				d[ 0 ] = d[ 1 ] = d[ 2 ] = p[ 0 ], d[ 3 ] = 255;
			else
				d[ 0 ] = p[ 0 ], d[ 1 ] = p[ 1 ], d[ 2 ] = p[ 2 ], d[ 3 ] = px == 4 ? p[ 3 ] : 255;
		};
		const size_t total = static_cast< size_t >( w ) * h;
		if ( type < 9 ) {
			if ( f.size( ) < at + total * px )
				return false;
			for ( size_t i = 0; i < total; ++i )
				put( i, f.data( ) + at + i * px );
		}
		else
			for ( size_t i = 0; i < total; ) {
				if ( at >= f.size( ) )
					return false;
				const int head = f[ at++ ], n = ( head & 0x7F ) + 1;
				if ( head & 0x80 ) {
					if ( at + px > f.size( ) )
						return false;
					for ( int k = 0; k < n && i < total; ++k )
						put( i++, f.data( ) + at );
					at += px;
				}
				else {
					if ( at + static_cast< size_t >( n ) * px > f.size( ) )
						return false;
					for ( int k = 0; k < n && i < total; ++k, at += px )
						put( i++, f.data( ) + at );
				}
			}
		out.resize( raw.size( ) );
		const bool top_first = ( desc & 0x20 ) != 0;
		for ( int y = 0; y < h; ++y )
			std::memcpy( out.data( ) + static_cast< size_t >( y ) * w * 4, raw.data( ) + static_cast< size_t >( top_first ? y : h - 1 - y ) * w * 4, static_cast< size_t >( w ) * 4 );
		return true;
	}

}
