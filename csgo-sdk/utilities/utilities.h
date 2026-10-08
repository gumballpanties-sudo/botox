#pragma once
#include <array>
#include <initializer_list>
#include <string>
#include <vector>
#include <xutility>

struct ImGuiIO;

namespace n_utilities
{
	struct impl_t {
		std::array< std::string, 9 > m_hit_groups = {
			"body", "head", "chest", "stomach", "left arm", "right arm", "left leg", "right leg", "unknown"
		};

		bool is_weapon_valid( );

		const char8_t* get_weapon_icon( short item_definition_index );

		int get_average_fps( ImGuiIO );

		int create_thread( unsigned long __stdcall function( void* ), void* parameter );

		std::string to_utf8( const std::wstring& text );

		std::string truncate_utf8( const std::string& text, size_t max_bytes );

		// <install>\csgo\, from client.dll's path ( csgo\bin\client.dll ). empty if not found
		std::string csgo_dir( );

		// loose files under csgo\<sub_dir> with extension ext, recursive, "models/player/x.mdl" style, sorted
		std::vector< std::string > game_files( const char* sub_dir, const char* ext );

		// loose "models/player/x.mdl" is a whole player body: head_0 + pelvis rig, mesh skinned to a leg bone
		bool is_player_mdl( const std::string& rel );
		template< typename T >
		bool is_in( const T& v, std::initializer_list< T > lst )
		{
			return std::find( std::begin( lst ), std::end( lst ), v ) != std::end( lst );
		};
	};
}

inline n_utilities::impl_t g_utilities;
