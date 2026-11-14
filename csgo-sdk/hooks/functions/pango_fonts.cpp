#include "../../globals/includes/includes.h"
#include "../../hacks/misc/hud_font/hud_font.h"
#include "../hooks.h"

/* panorama_text_pango builds every pango request from strings: pango_font_description_from_string( "Stratum2 Bold 29px" )
   and pango_attr_family_new( "Stratum2" ). the css family only gets there if panorama's own family list ( built once at
   init from pango_font_map_list_families ) knows it, so a face added at runtime never arrives. hud_font therefore writes a
   MARKER family the list knows and these two detours swap the marker for the real family right before pango parses it.
   list_families / layout hooks only remember the fontmap ( pango_fc_font_map_cache_clear after a new face ).
   RULE pango / fontconfig writes run ONLY in on_text_thread ( see hud_font.h m_job_* ). from_string has a libpango-internal
   caller; size / glyph detours can sit inside pango's own font work: never run the queue from those */

#include <intrin.h>

/* callers in csgo bin ( pe scan 11-14 ): set_font_description = panorama_text_pango e4ff + libpangoft2 280e
   ( pango_fc_font_get_metrics, INSIDE pango's font work ); attr_family_new = panorama_text_pango 100a5 only. both
   panorama sites hold its global text mutex 0x100a3448 -> the queue runs only for a return address in that dll.
   atomics = constant init, no static guard ( /Zc:threadSafeInit- ) */
static bool from_panorama_text( const void* ret )
{
	static std::atomic< std::uintptr_t > lo{ 0 }, hi{ 0 };
	if ( !hi.load( std::memory_order_acquire ) ) {
		const auto base = reinterpret_cast< std::uintptr_t >( GetModuleHandleA( "panorama_text_pango.dll" ) );
		if ( !base )
			return false;
		const auto* nt = reinterpret_cast< const IMAGE_NT_HEADERS* >( base + reinterpret_cast< const IMAGE_DOS_HEADER* >( base )->e_lfanew );
		lo = base;
		hi.store( base + nt->OptionalHeader.SizeOfImage, std::memory_order_release );
	}
	const auto at = reinterpret_cast< std::uintptr_t >( ret );
	return at >= lo.load( std::memory_order_relaxed ) && at < hi.load( std::memory_order_relaxed );
}

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

	/* panorama's layout build ( inside its text mutex, right before pango_font_map_load_font ): the safe spot to touch
	   pango / fontconfig state. libpangoft2's get_metrics layout is skipped */
	if ( from_panorama_text( _ReturnAddress( ) ) )
		g_hud_font.on_text_thread( layout );

	original( layout, desc );
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

	/* same rule as set_font_description ( only panorama calls it today, the gate keeps it that way ) */
	if ( from_panorama_text( _ReturnAddress( ) ) )
		g_hud_font.on_text_thread( nullptr );

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
	/* no static: game thread ( add_file -> FcFreeTypeQuery ) and panorama's text thread both land here first,
	   /Zc:threadSafeInit- sets the guard before the value = the second thread would call null */
	const auto original = g_hooks.m_ft_load_glyph.get_original< int( __cdecl* )( void*, unsigned int, int ) >( );
	HOOK_SCOPE_OR_BAIL( original( face, glyph_index, load_flags ) );

	return original( face, glyph_index, g_hud_font.glyph_flags( load_flags ) );
}
