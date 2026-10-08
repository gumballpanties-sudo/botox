#include "utilities.h"

#include "../game/sdk/includes/includes.h"
#include "../globals/includes/includes.h"

/* used ~ std::thread */
#include <algorithm>
#include <cctype>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <thread>

std::string n_utilities::impl_t::csgo_dir( )
{
	char path[ MAX_PATH ]{ };
	const HMODULE client = GetModuleHandleA( "client.dll" );
	if ( !client || !GetModuleFileNameA( client, path, sizeof( path ) ) )
		return { };

	std::error_code ec;
	const std::filesystem::path dir = std::filesystem::path( path ).parent_path( ).parent_path( );
	return std::filesystem::is_directory( dir, ec ) ? dir.string( ) : std::string( );
}

std::vector< std::string > n_utilities::impl_t::game_files( const char* sub_dir, const char* ext )
{
	std::vector< std::string > out;
	const std::string root = csgo_dir( );
	if ( root.empty( ) )
		return out;

	const std::filesystem::path base = std::filesystem::path( root );
	std::error_code ec;
	for ( auto it = std::filesystem::recursive_directory_iterator( base / sub_dir, std::filesystem::directory_options::skip_permission_denied, ec );
	      !ec && it != std::filesystem::recursive_directory_iterator( ); it.increment( ec ) ) {
		if ( !it->is_regular_file( ec ) || _stricmp( it->path( ).extension( ).string( ).c_str( ), ext ) != 0 )
			continue;

		std::string rel = std::filesystem::relative( it->path( ), base, ec ).generic_string( );
		std::transform( rel.begin( ), rel.end( ), rel.begin( ), []( unsigned char c ) { return static_cast< char >( std::tolower( c ) ); } );
		if ( !ec )
			out.push_back( std::move( rel ) );
	}

	std::sort( out.begin( ), out.end( ) );
	return out;
}

bool n_utilities::impl_t::is_player_mdl( const std::string& rel )
{
	const std::string root = csgo_dir( );
	if ( root.empty( ) || rel.size( ) < 5 )
		return false;

	const std::filesystem::path path = std::filesystem::path( root ) / rel;
	const std::string stem           = path.string( ).substr( 0, path.string( ).size( ) - 4 );

	std::error_code ec;
	if ( !std::filesystem::is_regular_file( stem + ".dx90.vtx", ec ) )
		return false;

	const auto load = []( const std::filesystem::path& p ) {
		std::ifstream f( p, std::ios::binary );
		return std::vector< char >( std::istreambuf_iterator< char >( f ), { } );
	};
	const auto i32 = []( const std::vector< char >& v, std::size_t off ) {
		int out = 0;
		if ( off + 4 <= v.size( ) )
			std::memcpy( &out, v.data( ) + off, 4 );
		return out;
	};

	// studiohdr_t numbones 0x9C / boneindex 0xA0, mstudiobone_t 216 bytes with sznameindex first
	const std::vector< char > mdl = load( path );
	if ( mdl.size( ) < 0x158 || std::memcmp( mdl.data( ), "IDST", 4 ) != 0 )
		return false;
	const int num_bones = i32( mdl, 0x9C ), bone_index = i32( mdl, 0xA0 );
	if ( num_bones <= 0 || num_bones > 256 || bone_index <= 0 || static_cast< std::size_t >( bone_index ) + num_bones * 216u > mdl.size( ) )
		return false;

	bool head = false, pelvis = false;
	std::array< bool, 256 > leg{ };
	for ( int i = 0; i < num_bones; ++i ) {
		const std::size_t bone = bone_index + i * 216u;
		const std::size_t name = bone + i32( mdl, bone );
		if ( name >= mdl.size( ) || !std::memchr( mdl.data( ) + name, 0, mdl.size( ) - name ) )
			continue;
		const char* s = mdl.data( ) + name;
		head |= _stricmp( s, "head_0" ) == 0;
		pelvis |= _stricmp( s, "pelvis" ) == 0;
		leg[ i ] = _strnicmp( s, "ankle_", 6 ) == 0 || _strnicmp( s, "leg_lower_", 10 ) == 0;
	}
	if ( !head || !pelvis )
		return false;

	// skeleton alone is not a body (hats, backpacks, wings ship the whole rig): the mesh must be skinned to a leg.
	// vvd: lod0 vertex count 0x10, vertex data 0x38, 48 byte vertex = float weight[3], u8 bone[3], u8 num_bones
	const std::vector< char > vvd = load( stem + ".vvd" );
	if ( vvd.size( ) < 64 || std::memcmp( vvd.data( ), "IDSV", 4 ) != 0 )
		return false;
	const int verts = i32( vvd, 0x10 ), start = i32( vvd, 0x38 );
	for ( int i = 0; i < verts && start > 0; ++i ) {
		const std::size_t v = start + i * 48u;
		if ( v + 48 > vvd.size( ) )
			break;
		const int n = static_cast< unsigned char >( vvd[ v + 15 ] );
		for ( int k = 0; k < n && k < 3; ++k ) {
			float w = 0.f;
			std::memcpy( &w, vvd.data( ) + v + k * 4, 4 );
			const unsigned char b = static_cast< unsigned char >( vvd[ v + 12 + k ] );
			if ( w > 0.f && b < num_bones && leg[ b ] )
				return true;
		}
	}
	return false;
}

int n_utilities::impl_t::create_thread( unsigned long __stdcall function( void* ), void* parameter )
{
	std::thread( function, parameter ).detach( );

	return 1;
}

std::string n_utilities::impl_t::to_utf8( const std::wstring& text )
{
	if ( text.empty( ) )
		return { };

	const int size = LI_FN( WideCharToMultiByte )( CP_UTF8, 0, text.data( ), static_cast< int >( text.size( ) ), nullptr, 0, nullptr, nullptr );

	if ( size <= 0 )
		return { };

	std::string out( static_cast< size_t >( size ), '\0' );
	LI_FN( WideCharToMultiByte )( CP_UTF8, 0, text.data( ), static_cast< int >( text.size( ) ), out.data( ), size, nullptr, nullptr );

	return out;
}

std::string n_utilities::impl_t::truncate_utf8( const std::string& text, const size_t max_bytes )
{
	if ( text.size( ) <= max_bytes )
		return text;

	size_t end = max_bytes;

	while ( end > 0U && ( static_cast< unsigned char >( text[ end ] ) & 0xC0 ) == 0x80 )
		--end;

	return text.substr( 0, end );
}

int n_utilities::impl_t::get_average_fps( ImGuiIO io )
{
	static int average_fps = static_cast< int >( io.Framerate + 0.5f );

	if ( static float last_checked = g_interfaces.m_global_vars_base->m_real_time;
	     fabs( last_checked - g_interfaces.m_global_vars_base->m_real_time ) > 2.0f ) {
		average_fps  = static_cast< int >( io.Framerate + 0.5f );
		last_checked = g_interfaces.m_global_vars_base->m_real_time;
	}
	return average_fps;
}

const char8_t* n_utilities::impl_t::get_weapon_icon( short item_definition_index )
{
	if ( item_definition_index == weapon_none )
		return u8"";

	switch ( item_definition_index ) {
	case weapon_deagle:
		return u8"\uE001";
	case weapon_elite:
		return u8"\uE002";
	case weapon_fiveseven:
		return u8"\uE003";
	case weapon_glock:
		return u8"\uE004";
	case weapon_ak47:
		return u8"\uE007";
	case weapon_aug:
		return u8"\uE008";
	case weapon_awp:
		return u8"\uE009";
	case weapon_famas:
		return u8"\uE00A";
	case weapon_g3sg1:
		return u8"\uE00B";
	case weapon_galilar:
		return u8"\uE00D";
	case weapon_m249:
		return u8"\uE00E";
	case weapon_m4a1:
		return u8"\uE010";
	case weapon_mac10:
		return u8"\uE011";
	case weapon_p90:
		return u8"\uE013";
	case weapon_mp5sd:
		return u8"\uE017";
	case weapon_ump45:
		return u8"\uE018";
	case weapon_xm1014:
		return u8"\uE019";
	case weapon_bizon:
		return u8"\uE01A";
	case weapon_mag7:
		return u8"\uE01B";
	case weapon_negev:
		return u8"\uE01C";
	case weapon_sawedoff:
		return u8"\uE01D";
	case weapon_tec9:
		return u8"\uE01E";
	case weapon_taser:
		return u8"\uE01F";
	case weapon_hkp2000:
		return u8"\uE020";
	case weapon_mp7:
		return u8"\uE021";
	case weapon_mp9:
		return u8"\uE022";
	case weapon_nova:
		return u8"\uE023";
	case weapon_p250:
		return u8"\uE024";
	case weapon_scar20:
		return u8"\uE026";
	case weapon_sg556:
		return u8"\uE027";
	case weapon_ssg08:
		return u8"\uE028";
	case weapon_knife:
		return u8"\uE02A";
	case weapon_flashbang:
		return u8"\uE02B";
	case weapon_hegrenade:
		return u8"\uE02C";
	case weapon_smokegrenade:
		return u8"\uE02D";
	case weapon_molotov:
		[[fallthrough]];
	case weapon_firebomb:
		return u8"\uE02E";
	case weapon_decoy:
		[[fallthrough]];
	case weapon_diversion:
		return u8"\uE02F";
	case weapon_incgrenade:
		return u8"\uE030";
	case weapon_c4:
		return u8"\uE031";
	case weapon_healthshot:
		return u8"\uE039";
	case weapon_knife_gg:
		[[fallthrough]];
	case weapon_knife_t:
		return u8"\uE03B";
	case weapon_m4a1_silencer:
		return u8"\uE03C";
	case weapon_usp_silencer:
		return u8"\uE03D";
	case weapon_cz75a:
		return u8"\uE03F";
	case weapon_revolver:
		return u8"\uE040";
	case weapon_tagrenade:
		return u8"\uE044";
	case weapon_fists:
		return u8"\uE045";
	case weapon_tablet:
		return u8"\uE048";
	case weapon_melee:
		return u8"\uE04A";
	case weapon_axe:
		return u8"\uE04B";
	case weapon_hammer:
		return u8"\uE04C";
	case weapon_spanner:
		return u8"\uE04E";
	case weapon_knife_bayonet:
		return u8"\uE1F4";
	case weapon_knife_css:
		return u8"\uE1F7";
	case weapon_knife_flip:
		return u8"\uE1F9";
	case weapon_knife_gut:
		return u8"\uE1FA";
	case weapon_knife_karambit:
		return u8"\uE1FB";
	case weapon_knife_m9_bayonet:
		return u8"\uE1FC";
	case weapon_knife_tactical:
		return u8"\uE1FD";
	case weapon_knife_falchion:
		return u8"\uE200";
	case weapon_knife_survival_bowie:
		return u8"\uE202";
	case weapon_knife_butterfly:
		return u8"\uE203";
	case weapon_knife_push:
		return u8"\uE204";
	case weapon_knife_cord:
		return u8"\uE205";
	case weapon_knife_canis:
		return u8"\uE206";
	case weapon_knife_ursus:
		return u8"\uE207";
	case weapon_knife_gypsy_jackknife:
		return u8"\uE208";
	case weapon_knife_outdoor:
		return u8"\uE209";
	case weapon_knife_stiletto:
		return u8"\uE20A";
	case weapon_knife_widowmaker:
		return u8"\uE20B";
	case weapon_knife_skeleton:
		return u8"\uE20D";
	default:
		g_console.print(
			std::vformat( "! get_weapon_icon failed to find icon index: {} !", std::make_format_args( item_definition_index ) ).c_str( ) );

		return u8"";
	}
}

bool n_utilities::impl_t::is_weapon_valid( )
{
	auto weapon_handle = g_ctx.m_local->get_active_weapon_handle( );
	if ( !weapon_handle )
		return false;

	const auto active_weapon =
		reinterpret_cast< c_base_entity* >( g_interfaces.m_client_entity_list->get_client_entity_from_handle( weapon_handle ) );

	if ( !active_weapon )
		return false;

	const auto definition_index = active_weapon->get_item_definition_index( );

	return definition_index > 0 && !( definition_index >= 41 && definition_index <= 59 ) && !( definition_index >= 68 );
}
