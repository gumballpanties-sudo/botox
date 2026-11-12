#include "../../globals/includes/includes.h"
#include "../../hacks/misc/hud_font/hud_font.h"
#include "../hooks.h"

/* panorama_text_pango builds every pango request from strings: pango_font_description_from_string( "Stratum2 Bold 29px" )
   and pango_attr_family_new( "Stratum2" ). the css family only gets there if panorama's own family list ( built once at
   init from pango_font_map_list_families ) knows it, so a face added at runtime never arrives. hud_font therefore writes a
   MARKER family the list knows and these two detours swap the marker for the real family right before pango parses it.
   list_families / layout hooks only remember the fontmap ( pango_fc_font_map_cache_clear after a new face ) */

void __cdecl n_detoured_functions::pango_font_map_list_families( void* fontmap, void*** families, int* n_families )
{
	static auto original = g_hooks.m_pango_list_families.get_original< void( __cdecl* )( void*, void***, int* ) >( );
	HOOK_SCOPE_OR_BAIL( original( fontmap, families, n_families ) );

	original( fontmap, families, n_families );

	g_hud_font.note_fontmap( fontmap, n_families ? *n_families : -1 );
}

void __cdecl n_detoured_functions::pango_layout_set_font_description( void* layout, const void* desc )
{
	static auto original = g_hooks.m_pango_layout_set_font_description.get_original< void( __cdecl* )( void*, const void* ) >( );
	HOOK_SCOPE_OR_BAIL( original( layout, desc ) );

	original( layout, desc );

	g_hud_font.note_layout( layout );
}

void* __cdecl n_detoured_functions::pango_font_description_from_string( const char* str )
{
	static auto original = g_hooks.m_pango_desc_from_string.get_original< void*( __cdecl* )( const char* ) >( );
	HOOK_SCOPE_OR_BAIL( original( str ) );

	return original( g_hud_font.rewrite_request( str, true ) );
}

void* __cdecl n_detoured_functions::pango_attr_family_new( const char* family )
{
	static auto original = g_hooks.m_pango_attr_family_new.get_original< void*( __cdecl* )( const char* ) >( );
	HOOK_SCOPE_OR_BAIL( original( family ) );

	return original( g_hud_font.rewrite_request( family, false ) );
}

/* font scale: the size panorama asks for ( pango units ) x slider -> glyphs rasterized at the real size, no blur */
void __cdecl n_detoured_functions::pango_font_description_set_size( void* desc, int size )
{
	static auto original = g_hooks.m_pango_desc_set_size.get_original< void( __cdecl* )( void*, int ) >( );
	HOOK_SCOPE_OR_BAIL( original( desc, size ) );

	original( desc, g_hud_font.scale_size( size ) );
}

void* __cdecl n_detoured_functions::pango_attr_size_new( int size )
{
	static auto original = g_hooks.m_pango_attr_size_new.get_original< void*( __cdecl* )( int ) >( );
	HOOK_SCOPE_OR_BAIL( original( size ) );

	return original( g_hud_font.scale_size( size ) );
}

/* text quality: panorama + pango_ft2 load every glyph through here ( libfreetype-6 export ); the hinting target is
   in the load flags */
int __cdecl n_detoured_functions::ft_load_glyph( void* face, unsigned int glyph_index, int load_flags )
{
	static auto original = g_hooks.m_ft_load_glyph.get_original< int( __cdecl* )( void*, unsigned int, int ) >( );
	HOOK_SCOPE_OR_BAIL( original( face, glyph_index, load_flags ) );

	return original( face, glyph_index, g_hud_font.glyph_flags( load_flags ) );
}
