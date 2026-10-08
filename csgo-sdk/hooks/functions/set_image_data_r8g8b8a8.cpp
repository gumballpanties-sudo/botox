#include "../../globals/includes/includes.h"
#include "../../hacks/misc/scaleform/moi_hud.h"
#include "../../hacks/misc/scaleform/scaleform.h"
#include "../hooks.h"

#pragma warning( disable : 4996 )

#define VSVG_OR_SVG true
#define VSVG_EXT    ( ( VSVG_OR_SVG ) ? ".svg" : ".vsvg" )
static const char* get_extension( const char* filename )
{
	if ( !g_scaleform.compare_extension )
		return nullptr;

	if ( g_scaleform.compare_extension( filename, ".png" ) )
		return ".png";
	else if ( g_scaleform.compare_extension( filename, ".svg" ) )
		return ".svg";
	else if ( g_scaleform.compare_extension( filename, ".vsvg" ) )
		return VSVG_EXT;
	return nullptr;
}

bool __fastcall n_detoured_functions::set_image_data_r8g8b8a8( void* ecx, void* edx, const uint8_t* data, uint32_t len, const char* filename, int w,
                                                               int h, void* arg1, int arg2 )
{
	static auto original = g_hooks.m_set_image_data_r8g8b8a8.get_original< decltype( &n_detoured_functions::set_image_data_r8g8b8a8 ) >( );
	HOOK_SCOPE_OR_BAIL( original( ecx, edx, data, len, filename, w, h, arg1, arg2 ) );

	if ( !filename || !GET_VARIABLE( g_variables.m_scaleform, bool ) )
		return original( ecx, edx, data, len, filename, w, h, arg1, arg2 );

	const char* extension = get_extension( filename );
	if ( !data || !len || !extension )
		return original( ecx, edx, data, len, filename, w, h, arg1, arg2 );

	constexpr const char eq_prefix[] = "materials\\panorama\\images\\icons\\equipment\\";
	bool equipment                   = ( strstr( filename, eq_prefix ) == filename ) && !strcmp( extension, VSVG_EXT );

	if ( equipment ) {
		const uint8_t* replacement_data;
		size_t replacement_size;
		int replacement_w, replacement_h;

		char copy[ 256 ];
		const char* start_ptr = &filename[ sizeof( eq_prefix ) - 1 ];
		const char* end_ptr   = strstr( start_ptr, ".vsvg" );
		size_t size           = ( size_t )end_ptr - ( size_t )start_ptr;

		if ( !end_ptr || size >= sizeof( copy ) )
			return original( ecx, edx, data, len, filename, w, h, arg1, arg2 );

		strncpy( copy, start_ptr, size );
		copy[ size ] = 0;

		if ( GET_VARIABLE( g_variables.m_scaleform_style, int ) == 0 ) {
			if ( const std::string* svg = g_moi_hud.select_icon( copy ) )
				return original( ecx, edx, reinterpret_cast< const uint8_t* >( svg->data( ) ), static_cast< uint32_t >( svg->size( ) ), filename,
				                 w, h, arg1, 6 );

			return original( ecx, edx, data, len, filename, w, h, arg1, arg2 );
		}

		if ( g_scaleform.get_replacement_icon( copy, replacement_data, replacement_size, replacement_w, replacement_h ) ) {
			arg2 = 6;
			return original( ecx, edx, replacement_data, replacement_size, filename, replacement_w, replacement_h, arg1, arg2 );
		} else {
			g_console.print( std::format( "!! didn't replace {}", copy ).c_str( ) );
		}
	}

	return original( ecx, edx, data, len, filename, w, h, arg1, arg2 );
}
