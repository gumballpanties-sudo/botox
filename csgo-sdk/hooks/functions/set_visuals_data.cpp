#include <cstdint>

#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"
#include "../../hacks/skins/skins.h"
#include "../hooks.h"

namespace
{
	constexpr std::uintptr_t PAINT_KIT_INDEX_OFFSET  = 0x0C;
	constexpr std::uintptr_t VISUALS_VMT_NAME_OFFSET = 0x77C;
	constexpr std::uintptr_t VISUALS_COLORS_OFFSET   = 0x98C;
}

void __fastcall n_detoured_functions::set_visuals_data( void* ecx, void* edx, const char* shader_name )
{
	static auto original = g_hooks.m_set_visuals_data.get_original< void( __fastcall* )( void*, void*, const char* ) >( );
	HOOK_SCOPE_OR_BAIL( original( ecx, edx, shader_name ) );

	original( ecx, edx, shader_name );

	if ( !ecx )
		return;

	const auto base         = reinterpret_cast< std::uintptr_t >( ecx );
	const auto path         = reinterpret_cast< const char* >( base + VISUALS_VMT_NAME_OFFSET );
	const int paint_kit     = *reinterpret_cast< int* >( base + PAINT_KIT_INDEX_OFFSET );
	const auto destination  = reinterpret_cast< float* >( base + VISUALS_COLORS_OFFSET );

	n_skins::paint_colors_t actual{ };

	for ( int i = 0; i < 4; i++ ) {
		actual.m_rgb[ i ][ 0 ] = destination[ i * 3 + 0 ];
		actual.m_rgb[ i ][ 1 ] = destination[ i * 3 + 1 ];
		actual.m_rgb[ i ][ 2 ] = destination[ i * 3 + 2 ];
	}

	n_skins::cache_actual_colors( paint_kit, actual );

	n_skins::paint_colors_t colors{ };
	if ( !n_skins::custom_colors_for_material( path, paint_kit, colors ) )
		return;

	for ( int i = 0; i < 4; i++ ) {
		destination[ i * 3 + 0 ] = colors.m_rgb[ i ][ 0 ];
		destination[ i * 3 + 1 ] = colors.m_rgb[ i ][ 1 ];
		destination[ i * 3 + 2 ] = colors.m_rgb[ i ][ 2 ];
	}
}
