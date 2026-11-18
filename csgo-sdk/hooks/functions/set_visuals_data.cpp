#include <cstdint>
#include <cstdio>

#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"
#include "../../globals/logger/logger.h"
#include "../../hacks/skins/skins.h"
#include "../hooks.h"

namespace
{
	// CBaseVisualsDataCompare at processor +8: m_nIndex +0xC, m_nSeed +0x10, m_flWear +0x14 ( live clothing SetVisualsData reads them )
	constexpr std::uintptr_t PAINT_KIT_INDEX_OFFSET  = 0x0C;
	constexpr std::uintptr_t COMPARE_WEAR_OFFSET     = 0x14;
	constexpr std::uintptr_t VISUALS_VMT_NAME_OFFSET = 0x77C;
	constexpr std::uintptr_t VISUALS_COLORS_OFFSET   = 0x98C;
}

/* CCSClothingVisualsDataProcessor vtable slot 3 ( live RTTI walk ) = GenerateCompositeMaterialKeyValues. returns a copy of the glove
   kit's customcharacter vmt ( GetVMTKeyValues MakeCopy ), CCompositeTexture copies it, then deleteThis. palette = $palettecolor1..8 */
void* __fastcall n_detoured_functions::clothing_composite_kv( void* ecx, void* edx, int param_id )
{
	static auto original = g_hooks.m_clothing_composite_kv.get_original< void*( __fastcall* )( void*, void*, int ) >( );
	HOOK_SCOPE_OR_BAIL( original( ecx, edx, param_id ) );

	const auto key_values = static_cast< c_key_values* >( original( ecx, edx, param_id ) );
	if ( !key_values || !ecx )
		return key_values;

	const auto base     = reinterpret_cast< std::uintptr_t >( ecx );
	const int paint_kit = *reinterpret_cast< int* >( base + PAINT_KIT_INDEX_OFFSET );
	const float wear    = *reinterpret_cast< float* >( base + COMPARE_WEAR_OFFSET );

	char key[ 24 ];
	char value[ 48 ];

	n_skins::paint_colors_t actual{ };
	int parsed = 0;

	for ( int i = 0; i < n_skins::GLOVE_PALETTE; i++ ) {
		std::snprintf( key, sizeof( key ), "$palettecolor%d", i + 1 );
		const char* text = key_values->get_string( key, "" );
		if ( text && std::sscanf( text, " [ %f %f %f", &actual.m_rgb[ i ][ 0 ], &actual.m_rgb[ i ][ 1 ], &actual.m_rgb[ i ][ 2 ] ) == 3 )
			parsed++;
	}

	if ( parsed == n_skins::GLOVE_PALETTE )
		n_skins::cache_actual_colors( paint_kit, actual );

	n_skins::paint_colors_t colors{ };
	if ( !n_skins::custom_glove_colors( ecx, paint_kit, colors ) )
		return key_values;

	for ( int i = 0; i < n_skins::GLOVE_PALETTE; i++ ) {
		std::snprintf( key, sizeof( key ), "$palettecolor%d", i + 1 );
		std::snprintf( value, sizeof( value ), "[ %d %d %d ]", static_cast< int >( colors.m_rgb[ i ][ 0 ] ), static_cast< int >( colors.m_rgb[ i ][ 1 ] ),
		               static_cast< int >( colors.m_rgb[ i ][ 2 ] ) );
		key_values->set_string( key, value );
	}

	botox_dbg_log( "GLV: palette kit %d param %d wear %.5f vmt had %d", paint_kit, param_id, wear, parsed );
	return key_values;
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
