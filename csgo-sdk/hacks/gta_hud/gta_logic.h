#pragma once
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>

/* gta sa hud rules, pure (tools/gta_hud_test.cpp). numbers from gta_sa.exe 1.0 US: CHud::DrawPlayerInfo 0x58EAF0,
   DrawWanted 0x58D9A0, DrawSuccessFailedMessage 0x58C6A0, DrawBustedWastedMessage 0x58CA50, DrawAreaName 0x58AA50,
   DrawAmmo 0x5893B0, CPlayerInfo::Process 0x56F8D0 (money roll), GetWeaponRadiusOnScreen 0x609CD0, weapon.dat STD rows.
   csgo twins: wanted = round kills, mission passed / failed = round won / lost, zone = place name, clock = round clock */
namespace n_gta_logic
{
	/* ---- frame clock: RsGlobal.frameLimit = 30 (0x619620), CTimer::m_FrameCounter stand-in ---- */
	inline constexpr int k_fps = 30;
	inline unsigned int frame_of( long long ms ) { return static_cast< unsigned int >( ms * k_fps / 1000 ); }
	inline bool each_frames( unsigned int frame, unsigned int n ) { return ( frame & n ) == 0; } // Timer.h EachFrames

	// _ftol (0x821B40): every int position in the exe truncates toward zero
	inline int ftol( float v ) { return static_cast< int >( v ); }

	/* ---- weapons ---- */
	enum e_kind : int { kind_fist, kind_melee, kind_pistol, kind_shotgun, kind_smg, kind_rifle, kind_sniper, kind_heavy, kind_thrown };

	struct sa_weapon_t {
		const char* m_model; // default.ide name = k_icons name, "" = hud fist
		int m_kind;
		float m_accuracy;    // weapon.dat R, skill STD
		float m_range;       // weapon.dat D (weaponRange), skill STD
	};

	inline constexpr sa_weapon_t k_fist{ "", kind_fist, 1.f, 1.f };
	inline constexpr sa_weapon_t k_knife{ "knifecur", kind_melee, 1.f, 1.f };
	inline constexpr sa_weapon_t k_colt45{ "colt45", kind_pistol, 1.0f, 35.f };
	inline constexpr sa_weapon_t k_silenced{ "silenced", kind_pistol, 1.25f, 35.f };
	inline constexpr sa_weapon_t k_deagle{ "desert_eagle", kind_pistol, 1.0f, 35.f };
	inline constexpr sa_weapon_t k_chromegun{ "chromegun", kind_shotgun, 1.2f, 40.f };
	inline constexpr sa_weapon_t k_sawnoff{ "sawnoff", kind_shotgun, 0.8f, 35.f };
	inline constexpr sa_weapon_t k_spas{ "shotgspa", kind_shotgun, 1.8f, 40.f };
	inline constexpr sa_weapon_t k_uzi{ "micro_uzi", kind_smg, 1.05f, 35.f };
	inline constexpr sa_weapon_t k_tec9{ "tec9", kind_smg, 1.05f, 35.f };
	inline constexpr sa_weapon_t k_mp5{ "mp5lng", kind_smg, 1.0f, 45.f };
	inline constexpr sa_weapon_t k_ak47{ "ak47", kind_rifle, 0.5f, 70.f };
	inline constexpr sa_weapon_t k_m4{ "m4", kind_rifle, 0.65f, 90.f };
	inline constexpr sa_weapon_t k_cuntgun{ "cuntgun", kind_sniper, 1.0f, 100.f };
	inline constexpr sa_weapon_t k_sniper{ "sniper", kind_sniper, 1.0f, 100.f };
	inline constexpr sa_weapon_t k_minigun{ "minigun", kind_heavy, 1.0f, 75.f };
	inline constexpr sa_weapon_t k_grenade{ "grenade", kind_thrown, 1.f, 40.f };
	inline constexpr sa_weapon_t k_teargas{ "teargas", kind_thrown, 1.f, 40.f };
	inline constexpr sa_weapon_t k_molotov{ "molotov", kind_thrown, 1.f, 40.f };
	inline constexpr sa_weapon_t k_satchel{ "satchel", kind_thrown, 1.f, 40.f };

	// csgo item definition index -> closest sa gun (look + class)
	inline const sa_weapon_t& sa_weapon( int def, bool knife )
	{
		switch ( def ) {
		case 1:
		case 64: return k_deagle;                                  // deagle, revolver
		case 2:
		case 3:
		case 4:
		case 32:
		case 36:
		case 63: return k_colt45;                                  // elite fiveseven glock p2000 p250 cz75
		case 61: return k_silenced;                                // usp-s
		case 30:
		case 34: return k_tec9;                                    // tec9, mp9
		case 17: return k_uzi;                                     // mac10
		case 19:
		case 23:
		case 24:
		case 26:
		case 33: return k_mp5;                                     // p90 mp5sd ump bizon mp7
		case 7:
		case 13:
		case 39: return k_ak47;                                    // ak galil sg553
		case 8:
		case 10:
		case 16:
		case 60: return k_m4;                                      // aug famas m4a4 m4a1-s
		case 40: return k_cuntgun;                                 // ssg08
		case 9:
		case 11:
		case 38: return k_sniper;                                  // awp g3sg1 scar20
		case 25: return k_spas;                                    // xm1014
		case 27:
		case 35: return k_chromegun;                               // mag7, nova
		case 29: return k_sawnoff;
		case 14:
		case 28: return k_minigun;                                 // m249, negev
		case 44: return k_grenade;
		case 43:
		case 45:
		case 47: return k_teargas;                                 // flash, smoke, decoy
		case 46:
		case 48: return k_molotov;
		case 49: return k_satchel;                                 // c4
		default: break;
		}
		return knife ? k_knife : k_fist;
	}

	// CHud::DrawAmmo: false = hidden. thrown: count only (sa ammoClip 1). guns: "outside-clip", clip size <= 1 = total
	inline bool ammo_text( const sa_weapon_t& w, int clip, int reserve, int clip_max, char* out, size_t size )
	{
		if ( w.m_kind == kind_fist || w.m_kind == kind_melee )
			return false; // m_nSlot <= 1
		if ( w.m_kind == kind_thrown ) {
			std::snprintf( out, size, "%d", std::max( reserve, 0 ) );
			return true;
		}
		clip    = std::max( clip, 0 );
		reserve = std::max( reserve, 0 );
		if ( reserve >= 9999 )
			return false; // totalAmmo - clip >= 9999
		if ( clip_max <= 1 || clip_max >= 1000 )
			std::snprintf( out, size, "%d", clip + reserve );
		else
			std::snprintf( out, size, "%d-%d", std::min( reserve, 9999 ), clip );
		return true;
	}

	// GetWeaponRadiusOnScreen: counter = m_fAttackButtonCounter (csgo: inaccuracy over standing, see gta_hud.cpp)
	inline float weapon_radius( const sa_weapon_t& w, float counter, bool ducking )
	{
		if ( w.m_kind == kind_fist || w.m_kind == kind_melee )
			return 0.f;
		const float accuracy = 0.5f / w.m_accuracy;
		if ( w.m_kind == kind_shotgun )
			return std::max( 0.2f, accuracy );
		const float range  = std::min( 1.f, 15.f / w.m_range );
		const float radius = ( counter * 0.5f + 1.f ) * range * accuracy;
		return std::max( 0.2f, ducking ? radius / 2.f : radius );
	}

	// DrawCrossHairs 0x58E1E1: circle only for PISTOL..COUNTRYRIFLE + minigun (sniper rifle = scope)
	inline bool circle_crosshair( const sa_weapon_t& w )
	{
		return w.m_kind == kind_pistol || w.m_kind == kind_shotgun || w.m_kind == kind_smg || w.m_kind == kind_rifle || w.m_kind == kind_heavy ||
		       ( w.m_kind == kind_sniper && std::strcmp( w.m_model, "cuntgun" ) == 0 );
	}

	/* ---- money: CPlayerInfo::Process rolls m_nDisplayMoney toward m_nMoney once per game frame ---- */
	inline int money_step( int display, int money )
	{
		const int d = money - display;
		if ( !d )
			return display;
		const int a    = std::abs( d );
		const int step = a > 100000 ? 12345 : a > 10000 ? 1234 : a > 1000 ? 123 : a > 50 ? 42 : 1;
		return d < 0 ? display - step : display + step;
	}

	// "$%08d" green, "-$%07d" red
	inline bool money_text( int display, char* out, size_t size )
	{
		if ( display < 0 )
			std::snprintf( out, size, "-$%07d", -display );
		else
			std::snprintf( out, size, "$%08d", display );
		return display < 0;
	}

	/* ---- CHud::GetYPosBasedOnHealth: max health < 101 raises the column by offset (csgo max = 100) ---- */
	inline float y_for_health( float pos, float offset_px, int max_health ) { return max_health < 101 ? pos - offset_px : pos; }

	// RenderHealthBar: full width = 109 * maxHealth / GetFatAndMuscleModifier(STAT_MOD_10) (= 176, 0x559E1A)
	inline int health_bar_width( float sx109, int max_health ) { return static_cast< unsigned short >( sx109 * static_cast< float >( max_health ) / 176.f ); }

	/* ---- wanted stars (DrawWanted): 0 skip, 1 gold, 2 dim gold (level before parole), 3 black shadow ---- */
	inline int wanted_star( int i, int level, int parole, bool recent, unsigned int frame )
	{
		const bool blink = ( frame & 4 ) != 0;
		if ( i < level )
			return !recent || blink ? 1 : 0;
		if ( i < parole && blink )
			return 2;
		return 3;
	}

	/* ---- name fade (DrawAreaName / eNameState) ---- */
	enum e_name_state : int { name_dont_show, name_show, name_fade_in, name_fade_out, name_switch };

	struct zone_t {
		int m_state = name_dont_show;
		int m_fade  = 0;  // m_ZoneFadeTimer
		int m_timer = 0;  // m_ZoneNameTimer
		char m_print[ 96 ]{ }; // m_ZoneToPrint
		char m_last[ 96 ]{ };  // m_pLastZoneName

		// name = current zone (m_pZoneName), blocked = m_Message / big message up. returns alpha, < 0 = not drawn
		float step( const char* name, int dt_ms, bool blocked )
		{
			if ( name && *name && std::strcmp( name, m_last ) != 0 ) {
				switch ( m_state ) {
				case name_dont_show:
					m_state = name_fade_in, m_timer = 0, m_fade = 0;
					std::snprintf( m_print, sizeof( m_print ), "%s", name );
					break;
				case name_show:
				case name_fade_in:
				case name_fade_out: m_state = name_switch, m_timer = 0; break;
				case name_switch: m_timer = 0; break;
				default: break;
				}
				std::snprintf( m_last, sizeof( m_last ), "%s", name );
			}
			if ( !m_state )
				return -1.f;
			float alpha = 255.f;
			switch ( m_state ) {
			case name_show:
				m_fade = 1000;
				if ( m_timer > 3000 )
					m_state = name_fade_out, m_fade = 1000;
				break;
			case name_fade_in:
				m_fade += dt_ms;
				if ( m_fade > 1000 )
					m_fade = 1000, m_state = name_show;
				alpha = static_cast< float >( m_fade ) / 1000.f * 255.f;
				break;
			case name_fade_out:
				m_fade -= dt_ms;
				if ( m_fade < 0 )
					m_fade = 0, m_state = name_dont_show;
				alpha = static_cast< float >( m_fade ) / 1000.f * 255.f;
				break;
			case name_switch:
				m_fade -= dt_ms;
				if ( m_fade < 0 ) {
					m_fade = 0, m_state = name_fade_in;
					std::snprintf( m_print, sizeof( m_print ), "%s", m_last );
				}
				alpha = static_cast< float >( m_fade ) / 1000.f * 255.f;
				break;
			default: break;
			}
			if ( blocked ) {
				m_state = name_fade_out;
				return -1.f;
			}
			m_timer += dt_ms;
			return alpha;
		}
	};

	/* ---- big messages ---- */
	// STYLE_WHITE_MIDDLE (wasted): first frame arms, then alpha += 0.4 / ms up to 255
	struct wasted_t {
		bool m_on   = false;
		float m_x   = 0.f; // BigMessageX
		float m_alpha = 0.f;

		void set( bool on )
		{
			if ( on && !m_on )
				m_x = 0.f;
			m_on = on;
			if ( !on )
				m_x = 0.f, m_alpha = 0.f;
		}
		// < 0 = not drawn this frame
		float step( float dt_ms )
		{
			if ( !m_on ) {
				m_x = 0.f;
				return -1.f;
			}
			if ( m_x == 0.f ) {
				m_x = 1.f, m_alpha = 0.f;
				return -1.f;
			}
			m_alpha = std::min( m_alpha + dt_ms * 0.4f, 255.f );
			return m_alpha;
		}
		bool showing( ) const { return m_x != 0.f; }
	};

	// STYLE_MIDDLE (mission passed / failed), DrawSuccessFailedMessage
	struct success_t {
		char m_text[ 128 ]{ };
		float m_x = 0.f, m_in_use = 0.f, m_alpha = 0.f;
		float m_y = -1.f; // static 0xBAB218

		void set( const char* text )
		{
			if ( m_x != 0.f )
				return; // SetBigMessage: busy slot keeps its text
			std::snprintf( m_text, sizeof( m_text ), "%s", text );
		}
		// draw_alpha = alpha the text is drawn with (set before the update in the exe). false = nothing this frame
		bool step( float w, float h, float sy, float dt_ms, int lines, float& draw_alpha )
		{
			const float base = static_cast< float >( static_cast< int >( h ) / 2 ) - 10.f * sy;
			if ( m_y < 0.f )
				m_y = base;
			if ( !m_text[ 0 ] ) {
				m_x = 0.f;
				return false;
			}
			if ( m_x == 0.f ) {
				m_in_use = -60.f, m_x = 1.f, m_alpha = 0.f;
				m_y      = lines > 1 ? base - 15.f * sy : base; // styles 2 / 3 / 5 never set here
				return false;
			}
			draw_alpha = m_alpha;
			if ( m_in_use >= static_cast< float >( static_cast< int >( w ) - 20 ) ) {
				m_x += dt_ms / 20.f; // CTimer::ms_fTimeStep
				if ( m_x >= 120.f )
					m_x = 120.f, m_alpha -= dt_ms * 0.3f;
			}
			else {
				const float inc = dt_ms * 0.3f;
				m_in_use += inc;
				m_alpha = std::min( m_alpha + inc, 255.f );
			}
			if ( m_alpha <= 0.f )
				m_alpha = 0.f, m_text[ 0 ] = 0;
			return true;
		}
	};

	/* ---- chat (sa has none: csgo chat in the subtitle look). csgo colour bytes -> CFont ~x~ tokens ---- */
	inline const char* team_token( int team ) { return team == 2 ? "~y~" : team == 3 ? "~b~~h~~h~" : "~w~"; }

	// html tags dropped (botox lines), 0x01..0x10 = csgo chat colours, non-ascii + '~' dropped (sa font = printable ascii)
	inline std::string chat_line( std::string_view t, bool html, int sender_team )
	{
		std::string out = "~w~";
		for ( size_t i = 0; i < t.size( ); ++i ) {
			const auto b = static_cast< unsigned char >( t[ i ] );
			if ( html && b == '<' ) {
				const size_t close = t.find( '>', i );
				if ( close == std::string_view::npos )
					break;
				i = close;
				continue;
			}
			if ( html && b == '&' ) {
				static constexpr std::pair< const char*, char > k_entities[] = { { "&amp;", '&' }, { "&lt;", '<' }, { "&gt;", '>' }, { "&quot;", '"' }, { "&#39;", '\'' } };
				bool hit = false;
				for ( const auto& [ name, c ] : k_entities )
					if ( t.substr( i, std::strlen( name ) ) == name ) {
						out += c, i += std::strlen( name ) - 1, hit = true;
						break;
					}
				if ( hit )
					continue;
			}
			if ( b >= 1 && b <= 16 ) {
				switch ( b ) {
				case 3: out += team_token( sender_team ); break;
				case 2:
				case 7:
				case 15: out += "~r~"; break;
				case 4:
				case 5:
				case 6: out += "~g~"; break;
				case 9:
				case 16: out += "~y~"; break;
				case 10:
				case 11:
				case 12: out += "~b~~h~~h~"; break;
				case 14: out += "~p~"; break;
				default: out += "~w~"; break;
				}
				continue;
			}
			if ( b >= 32 && b < 127 && b != '~' )
				out += static_cast< char >( b );
		}
		return out;
	}

	/* ---- clock: sa "%02d:%02d" game hours:minutes, csgo round clock minutes:seconds ---- */
	// -1 = no clock (local time). round time left first: community servers sit in warmup with an expired end (pinned
	// 00:00) or an endless one (99:xx). warmup countdown only with no round clock and under 100 min
	inline int round_clock_ms( bool warmup, float warmup_end, int round_s, float start, float now )
	{
		if ( round_s > 0 ) {
			const float round = static_cast< float >( round_s );
			const float left  = std::clamp( round - std::max( now - start, 0.f ), 0.f, round );
			if ( left > 0.f || !warmup )
				return static_cast< int >( left * 1000.f );
		}
		if ( warmup && warmup_end - now > 0.f && warmup_end - now < 6000.f )
			return static_cast< int >( ( warmup_end - now ) * 1000.f );
		return -1;
	}

	inline void clock_text( int ms, char* out, size_t size )
	{
		const int s = std::max( ms, 0 ) / 1000;
		std::snprintf( out, size, "%02d:%02d", std::min( s / 60, 99 ), s % 60 );
	}
}
