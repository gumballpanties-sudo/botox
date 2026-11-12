#include "hud_font.h"
#include "../scaleform/scaleform.h"
#include "../scaleform/scaleform_js.h"
#include "../../../globals/config/config.h"
#include "../../../globals/fonts/fonts.h"
#include "../../../globals/includes/includes.h"
#include "../../../globals/logger/logger.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

namespace
{
	/* fontconfig 2.11 ( FcGetVersion 21101 ), resolved from the libfontconfig-1.dll panorama_text_pango loaded.
	   32 bit csgo = cdecl exports */
	struct fc_api_t {
		void*( __cdecl* get_current )( )                                                     = nullptr;
		void*( __cdecl* get_fonts )( void*, int )                                            = nullptr; /* FcSetName */
		void*( __cdecl* get_blanks )( void* )                                                = nullptr;
		void*( __cdecl* freetype_query )( const unsigned char*, unsigned int, void*, int* ) = nullptr;
		int( __cdecl* font_set_add )( void*, void* )                                         = nullptr;
		void*( __cdecl* pattern_create )( )                                                  = nullptr;
		int( __cdecl* pattern_add_string )( void*, const char*, const unsigned char* )       = nullptr;
		int( __cdecl* config_substitute )( void*, void*, int )                               = nullptr;
		void( __cdecl* default_substitute )( void* )                                         = nullptr;
		void*( __cdecl* font_match )( void*, void*, int* )                                   = nullptr;
		int( __cdecl* pattern_get_string )( const void*, const char*, int, unsigned char** ) = nullptr;
		void( __cdecl* pattern_destroy )( void* )                                            = nullptr;
		int( __cdecl* get_version )( )                                                       = nullptr;
		bool ok = false;
	};

	constexpr int k_set_application = 1; /* FcSetApplication */
	constexpr int k_match_pattern   = 0; /* FcMatchPattern */
	constexpr int k_match_scan      = 2; /* FcMatchScan */

	/* every stock hud / menu request starts with one of these ( "Stratum2", "Stratum2 Bold", "Stratum2 Bold Monodigit" ) */
	constexpr const char* k_stock_prefix = "Stratum2";

	struct pango_api_t {
		void*( __cdecl* layout_get_context )( void* )   = nullptr; /* libpango-1.0-0 */
		void*( __cdecl* context_get_font_map )( void* ) = nullptr;
		int( __cdecl* version )( )                      = nullptr;
		void( __cdecl* fc_cache_clear )( void* )        = nullptr; /* libpangoft2-1.0-0 */
	};

	const pango_api_t& pango( )
	{
		static pango_api_t api = [ ] {
			pango_api_t a{ };
			const HMODULE core = GetModuleHandleA( "libpango-1.0-0.dll" );
			const HMODULE ft2  = GetModuleHandleA( "libpangoft2-1.0-0.dll" );
			if ( core ) {
				a.layout_get_context   = reinterpret_cast< decltype( a.layout_get_context ) >( GetProcAddress( core, "pango_layout_get_context" ) );
				a.context_get_font_map = reinterpret_cast< decltype( a.context_get_font_map ) >( GetProcAddress( core, "pango_context_get_font_map" ) );
				a.version              = reinterpret_cast< decltype( a.version ) >( GetProcAddress( core, "pango_version" ) );
			}
			if ( ft2 )
				a.fc_cache_clear = reinterpret_cast< decltype( a.fc_cache_clear ) >( GetProcAddress( ft2, "pango_fc_font_map_cache_clear" ) );

			botox_dbg_log( "HUDFONT: pango %d core %p ft2 %p cache_clear %p", a.version ? a.version( ) : -1, core, ft2, a.fc_cache_clear );
			return a;
		}( );
		return api;
	}

	const fc_api_t& fc( )
	{
		static fc_api_t api = [ ] {
			fc_api_t a{ };
			const HMODULE mod = GetModuleHandleA( "libfontconfig-1.dll" );
			if ( !mod ) {
				botox_dbg_log( "HUDFONT: libfontconfig-1.dll not loaded" );
				return a;
			}

			const auto get = [ & ]( auto& fn, const char* name ) {
				fn = reinterpret_cast< std::remove_reference_t< decltype( fn ) > >( GetProcAddress( mod, name ) );
				if ( !fn )
					botox_dbg_log( "HUDFONT: export %s missing", name );
				return fn != nullptr;
			};

			a.ok = get( a.get_current, "FcConfigGetCurrent" ) & get( a.get_fonts, "FcConfigGetFonts" ) &
			       get( a.get_blanks, "FcConfigGetBlanks" ) & get( a.freetype_query, "FcFreeTypeQuery" ) &
			       get( a.font_set_add, "FcFontSetAdd" ) & get( a.pattern_create, "FcPatternCreate" ) &
			       get( a.pattern_add_string, "FcPatternAddString" ) & get( a.config_substitute, "FcConfigSubstitute" ) &
			       get( a.default_substitute, "FcDefaultSubstitute" ) & get( a.font_match, "FcFontMatch" ) &
			       get( a.pattern_get_string, "FcPatternGetString" ) & get( a.pattern_destroy, "FcPatternDestroy" );
			get( a.get_version, "FcGetVersion" );

			if ( a.ok )
				botox_dbg_log( "HUDFONT: fontconfig %d", a.get_version ? a.get_version( ) : -1 );
			return a;
		}( );
		return api;
	}

	std::string lower( std::string s )
	{
		std::transform( s.begin( ), s.end( ), s.begin( ), []( unsigned char c ) { return static_cast< char >( std::tolower( c ) ); } );
		return s;
	}

	std::string pattern_string( const fc_api_t& api, void* pattern, const char* object )
	{
		unsigned char* value = nullptr;
		if ( api.pattern_get_string( pattern, object, 0, &value ) != 0 || !value ) /* FcResultMatch = 0 */
			return { };
		return reinterpret_cast< const char* >( value );
	}

	/* FcFontMatch for a bare family -> family fontconfig lands on */
	std::string match_family( const fc_api_t& api, void* config, const std::string& family, std::string* file = nullptr )
	{
		void* pattern = api.pattern_create( );
		if ( !pattern )
			return { };

		api.pattern_add_string( pattern, "family", reinterpret_cast< const unsigned char* >( family.c_str( ) ) );
		api.config_substitute( config, pattern, k_match_pattern );
		api.default_substitute( pattern );

		int result  = 0;
		void* match = api.font_match( config, pattern, &result );
		std::string got;
		if ( match ) {
			got = pattern_string( api, match, "family" );
			if ( file )
				*file = pattern_string( api, match, "file" );
			api.pattern_destroy( match );
		}
		api.pattern_destroy( pattern );
		return got;
	}

	/* ${mode} 'poke' = every text panel: letter-spacing on + off ( text relayout -> fresh pango request -> swapped
	   family ) + transform; 'tick' = transform on rows made since ( untagged ); 'off' = transform null, tags cleared.
	   ${xf} = quoted css transform or ''. ${report} = true leaves a `bxlog_...` child under the root ( read natively ).
	   no font property is ever written: fontFamily / fontSize are write-only aliases that re-bake the size x ui-scale
	   on every write ( shrink ), the `font` shorthand setter rejects an unset size.
	   GetChildCount / GetChild / CreatePanel / .id / DeleteAsync = chud-proven api */
	const char* k_walk = R"js(
(function () {
    var mode = ${mode};
    var xf = ${xf};
    var total = 0, text = 0, wrote = 0, cleaned = 0, err = '', wn = '';
    /* older builds wrote the fontFamily alias: inline "unset unset 0.0 <family>" with the size baked from a label that
       only inherited one = 0 = invisible ( weapon names ). such a leftover survives eject / inject, drop it. scaleform's
       own writes name Stratum2 and stay */
    var leftover = function ( p ) {
        try {
            var f = p.style.font;
            if ( typeof f !== 'string' ) return false;
            var parts = f.split( ' ' );
            return parts.length > 3 && parts[ 2 ] === '0.0' && parts.slice( 3 ).join( ' ' ).indexOf( 'Stratum2' ) !== 0;
        } catch ( x ) { return false; }
    };
    var poke = function ( p ) {
        try { p.style.letterSpacing = '0.01px'; p.style.letterSpacing = null; } catch ( x ) { if ( !err ) err = 'poke:' + x; }
    };
    var walk = function ( p ) {
        ++total;
        var t = '';
        try { t = p.paneltype; } catch ( x ) {}
        var isText = t === 'Label' || t === 'TextEntry';
        if ( !isText ) { try { isText = typeof p.text === 'string'; } catch ( x ) {} }
        if ( isText ) {
            ++text;
            try {
                var tag = p.GetAttributeString( 'bxfont', '' );
                if ( mode === 'off' ) {
                    /* every text panel: the swap table is empty now, each one must lay its text out again */
                    if ( tag !== '' ) {
                        p.style.transform = null;
                        p.SetAttributeString( 'bxfont', '' );
                    }
                    if ( leftover( p ) ) { p.style.font = null; ++cleaned; }
                    poke( p );
                    ++wrote;
                } else if ( mode === 'poke' || tag !== xf ) {
                    if ( mode === 'poke' ) {
                        if ( leftover( p ) ) { p.style.font = null; ++cleaned; }
                        poke( p );
                    }
                    p.style.transform = xf === '' ? null : xf;
                    p.SetAttributeString( 'bxfont', xf );
                    ++wrote;
                }
            } catch ( x ) { if ( !err ) err = '' + x; }
        }
        var n = 0;
        try { if ( typeof p.GetChildCount === 'function' ) n = p.GetChildCount(); } catch ( x ) { if ( !err ) err = '' + x; }
        for ( var i = 0; i < n; ++i ) {
            var c = null;
            try { c = p.GetChild( i ); } catch ( x ) {}
            if ( c ) walk( c );
        }
    };
    var root = $.GetContextPanel();
    try { walk( root ); } catch ( x ) { if ( !err ) err = '' + x; }
    if ( ${report} ) {
        try {
            /* one weapon name label, state after the walk: base font | visible | text length | letter-spacing | transform | opacity */
            var names = root.FindChildrenWithClassTraverse( 'weapon-selection-item-name-text' );
            if ( names && names.length ) {
                var q = names[ 0 ];
                var g = function ( fn ) { try { var v = fn(); return v === null ? 'null' : v === undefined ? 'undef' : '' + v; } catch ( x ) { return 'throw'; } };
                wn = names.length + '|' + g( function () { return q.style.font; } ) + '|' + g( function () { return q.visible; } ) + '|' +
                     g( function () { return q.text.length; } ) + '|' + g( function () { return q.style.letterSpacing; } ) + '|' +
                     g( function () { return q.style.transform; } ) + '|' + g( function () { return q.style.opacity; } );
            }
        } catch ( x ) { wn = 'throw'; }
        try {
            var n = root.GetChildCount();
            for ( var i = n - 1; i >= 0; --i ) {
                var c = root.GetChild( i );
                if ( c && ( '' + c.id ).indexOf( 'bxlog_' ) === 0 ) c.DeleteAsync( 0.0 );
            }
            var msg = ( 'mode=' + mode + '_panels=' + total + '_text=' + text + '_wrote=' + wrote + '_cleaned=' + cleaned + '_wn=' + wn +
                        ( err ? '_err=' + err : '' ) ).replace( /[^A-Za-z0-9_=.|]/g, '-' );
            $.CreatePanel( 'Panel', root, 'bxlog_' + msg.substr( 0, 220 ), { style: 'width: 0px; height: 0px;' } );
        } catch ( x ) {}
    }
})();
)js";
}

std::string n_hud_font::impl_t::add_file( const std::string& display_name )
{
	const auto& api = fc( );
	if ( !api.ok )
		return { };

	void* config = api.get_current( );
	void* set    = config ? api.get_fonts( config, k_set_application ) : nullptr;
	if ( !set ) {
		botox_dbg_log( "HUDFONT: no application font set (config %p)", config );
		return { };
	}

	const std::string path = g_fonts.find_path( display_name );
	std::string family;

	/* not a file we know: typed a family name ( shipped 'Noto Mono', 'Stratum2 Bold' ... ), hand it on as is */
	if ( GetFileAttributesA( path.c_str( ) ) == INVALID_FILE_ATTRIBUTES ) {
		botox_dbg_log( "HUDFONT: \"%s\" is no file (%s), using as family", display_name.c_str( ), path.c_str( ) );
		family = display_name;
	} else if ( std::find( m_in_set.begin( ), m_in_set.end( ), path ) == m_in_set.end( ) ) {
		int count = 1;
		for ( int id = 0; id < count; ++id ) { /* .ttc = several faces */
			void* pattern = api.freetype_query( reinterpret_cast< const unsigned char* >( path.c_str( ) ), id, api.get_blanks( config ), &count );
			if ( !pattern ) {
				botox_dbg_log( "HUDFONT: FcFreeTypeQuery %s face %d failed", path.c_str( ), id );
				break;
			}

			api.config_substitute( config, pattern, k_match_scan ); /* what the dir scanner does to every face */

			const std::string got = pattern_string( api, pattern, "family" );
			if ( family.empty( ) )
				family = got;

			const int added = api.font_set_add( set, pattern ); /* set owns the pattern now */
			botox_dbg_log( "HUDFONT: add %s face %d/%d \"%s\" %s -> %d", path.c_str( ), id, count, got.c_str( ),
			               pattern_string( api, pattern, "style" ).c_str( ), added );
			if ( !added )
				api.pattern_destroy( pattern );
		}

		if ( !family.empty( ) ) {
			m_in_set.push_back( path );
			m_need_clear = true; /* pango's family list / fontsets predate this face */
		}
	}

	/* already in the set from an earlier pick: ask fontconfig which family lives at that path */
	if ( family.empty( ) ) {
		int count     = 0;
		void* pattern = api.freetype_query( reinterpret_cast< const unsigned char* >( path.c_str( ) ), 0, api.get_blanks( config ), &count );
		if ( pattern ) {
			family = pattern_string( api, pattern, "family" );
			api.pattern_destroy( pattern );
		}
	}

	if ( family.empty( ) ) {
		g_logger.print( "\"" + display_name + "\" could not be read (" + path + ")\n", "[hud font]" );
		return { };
	}

	/* what pango gets for this family now */
	std::string file;
	const std::string got = match_family( api, config, family, &file );
	botox_dbg_log( "HUDFONT: match \"%s\" -> \"%s\" %s", family.c_str( ), got.c_str( ), file.c_str( ) );
	if ( lower( got ) != lower( family ) )
		g_logger.print( "\"" + family + "\" added but fontconfig still picks \"" + got + "\"\n", "[hud font]" );

	return family;
}

void n_hud_font::impl_t::note_fontmap( void* fontmap, int n_families )
{
	if ( !fontmap )
		return;

	if ( m_fontmap.exchange( fontmap ) != fontmap )
		botox_dbg_log( "HUDFONT: fontmap %p (list_families, %d families)", fontmap, n_families );

	const int calls = ++m_list_calls;
	if ( calls <= 3 || ( calls % 500 ) == 0 )
		botox_dbg_log( "HUDFONT: list_families call %d -> %d families", calls, n_families );
}

void n_hud_font::impl_t::note_layout( void* layout )
{
	if ( m_fontmap.load( std::memory_order_relaxed ) || !layout )
		return;

	const auto& api = pango( );
	if ( !api.layout_get_context || !api.context_get_font_map )
		return;

	void* context = api.layout_get_context( layout );
	void* fontmap = context ? api.context_get_font_map( context ) : nullptr;
	if ( fontmap && !m_fontmap.exchange( fontmap ) )
		botox_dbg_log( "HUDFONT: fontmap %p (layout %p context %p)", fontmap, layout, context );
}

const char* n_hud_font::impl_t::rewrite_request( const char* str, bool with_size )
{
	if ( !str )
		return str;

	std::lock_guard< std::mutex > lock( m_hook_mutex );

	/* every distinct request once: what panorama really asks pango for ( cap keeps the vector small ) */
	if ( m_seen_requests.size( ) < 96 && std::find( m_seen_requests.begin( ), m_seen_requests.end( ), str ) == m_seen_requests.end( ) ) {
		m_seen_requests.emplace_back( str );
		botox_dbg_log( "HUDFONT: pango %s \"%s\"", with_size ? "desc" : "attr", str );
	}

	if ( m_hook_family.empty( ) )
		return str;

	/* leading "Stratum2[ Bold[ Monodigit]]" token -> family; the fallback list after the first comma stays */
	const std::size_t prefix = std::strlen( k_stock_prefix );
	if ( std::strncmp( str, k_stock_prefix, prefix ) != 0 )
		return str;

	const char* rest = std::strchr( str, ',' );
	m_hook_out       = m_hook_family + ( rest ? rest : "" );
	return m_hook_out.c_str( );
}

bool n_hud_font::impl_t::clear_pango_cache( )
{
	void* fontmap   = m_fontmap.load( );
	const auto& api = pango( );
	if ( !fontmap || !api.fc_cache_clear ) {
		botox_dbg_log( "HUDFONT: cache clear waits (fontmap %p cache_clear %p)", fontmap, api.fc_cache_clear );
		return false;
	}

	api.fc_cache_clear( fontmap );
	botox_dbg_log( "HUDFONT: pango_fc_font_map_cache_clear %p", fontmap );
	return true;
}

bool n_hud_font::impl_t::resolve_root( )
{
	if ( !m_uiengine && g_interfaces.m_panorama )
		m_uiengine = g_interfaces.m_panorama->access_ui_engine( );
	if ( !m_uiengine )
		return false;

	if ( m_root && m_uiengine->is_valid_panel_ptr( m_root ) )
		return true;

	m_root = nullptr;
	m_applied.clear( );

	auto itr = m_uiengine->get_last_dispatched_event_target_panel( );
	while ( itr && m_uiengine->is_valid_panel_ptr( itr ) ) {
		if ( HASH_RT( itr->get_id( ) ) == HASH_BT( "CSGOHud" ) ) {
			m_root = itr;
			return true;
		}
		itr = itr->get_parent( );
	}

	return false;
}

/* nudge only, visual: translate3d leaves layout alone. scale goes through scale_size( ) so glyphs are rasterized at
   the real size instead of a blurry bitmap scale */
std::string n_hud_font::impl_t::transform_js( ) const
{
	const float x = GET_VARIABLE( g_variables.m_def_hud_font_nudge_x, float );
	const float y = GET_VARIABLE( g_variables.m_def_hud_font_nudge_y, float );

	if ( std::fabs( x ) < 0.5f && std::fabs( y ) < 0.5f )
		return "''";

	char buffer[ 96 ]{ };
	std::snprintf( buffer, sizeof( buffer ), "'translate3d(%.0fpx, %.0fpx, 0px)'", x, y );
	return buffer;
}

int n_hud_font::impl_t::scale_size( int size ) const
{
	const float scale = m_hook_scale.load( std::memory_order_relaxed );
	if ( size <= 0 || std::fabs( scale - 1.f ) < 0.005f )
		return size;
	return static_cast< int >( size * scale + 0.5f );
}

int n_hud_font::impl_t::glyph_flags( int load_flags ) const
{
	/* freetype.h: FT_LOAD_NO_HINTING 1<<1, FT_LOAD_FORCE_AUTOHINT 1<<5, FT_LOAD_TARGET_( mode ) = mode << 16, mask 0xF0000,
	   FT_RENDER_MODE_NORMAL 0, FT_RENDER_MODE_LIGHT 1 */
	constexpr int k_no_hinting = 1 << 1, k_force_autohint = 1 << 5, k_target_mask = 0xF0000, k_target_light = 1 << 16;

	switch ( m_hook_quality.load( std::memory_order_relaxed ) ) {
	case 1: /* sharp: full hinting, native hinter */
		return ( load_flags & ~( k_no_hinting | k_force_autohint | k_target_mask ) );
	case 2: /* smooth: light hinting ( vertical only ), shapes stay true */
		return ( load_flags & ~( k_no_hinting | k_target_mask ) ) | k_target_light;
	case 3: /* unhinted: pure outline antialias */
		return load_flags | k_no_hinting;
	default:
		return load_flags;
	}
}

void n_hud_font::impl_t::walk( const char* mode, bool report )
{
	std::string js = k_walk;
	const auto replace = [ & ]( const char* token, const char* value ) {
		const auto at = js.find( token );
		if ( at != std::string::npos )
			js.replace( at, std::strlen( token ), value );
	};
	replace( "${mode}", ( std::string( "'" ) + mode + "'" ).c_str( ) );
	replace( "${xf}", m_xf.c_str( ) );
	replace( "${report}", report ? "true" : "false" );

	m_uiengine->run_script( m_root, js.c_str( ), CSGO_HUD_SCHEMA, 8, 10, false, false );
}

/* the walk's counts sit in a `bxlog_...` child id under CSGOHud: id read is the one proven native panel accessor */
void n_hud_font::impl_t::read_js_log( )
{
	const int count = m_root->get_child_count( );
	for ( int i = count - 1; i >= 0; --i ) {
		c_uipanel* child = m_root->get_child( i );
		if ( !child || !m_uiengine->is_valid_panel_ptr( child ) || !child->has_id( ) )
			continue;

		const char* id = child->get_id( );
		if ( !id || std::strncmp( id, "bxlog_", 6 ) != 0 )
			continue;

		if ( m_last_js_log != id ) {
			m_last_js_log = id;
			botox_dbg_log( "HUDFONT: js %s", id + 6 );
		}
		return;
	}
}

void n_hud_font::impl_t::apply_now( )
{
	m_added.clear( );
	m_applied.clear( );
	m_pending.clear( );
	m_apply_at_once = true;
}

void n_hud_font::impl_t::on_createmove( )
{
	const float now = g_interfaces.m_global_vars_base ? g_interfaces.m_global_vars_base->m_real_time : 0.f;

	std::string name = g_ctx.m_eject_restoring ? std::string( ) : GET_VARIABLE( g_variables.m_def_hud_font, std::string );

	/* typing in the menu: wait until the name sat still 0.5 s. a list click / apply button skips the wait */
	if ( name != m_pending ) {
		m_pending       = name;
		m_pending_since = m_apply_at_once ? now - 1.f : now;
		m_apply_at_once = false;
	}
	if ( !name.empty( ) && now - m_pending_since < 0.5f )
		name = m_added;

	const std::string xf = transform_js( );
	if ( xf != m_xf ) {
		m_xf_dirty = !m_xf.empty( ); /* first fill is not a move */
		m_xf       = xf;
	}

	/* scale lives in the pango size hooks; a change needs every label laid out again ( poke ), debounced for drags */
	const float scale = name.empty( ) ? 1.f : GET_VARIABLE( g_variables.m_def_hud_font_scale, float ) / 100.f;
	if ( std::fabs( scale - m_hook_scale.load( std::memory_order_relaxed ) ) > 0.0005f ) {
		m_hook_scale     = scale;
		m_scale_dirty_at = now + 0.15f;
	}

	/* text quality ( hinting ) works with the stock font too: rendered glyphs sit in pango's font objects, so a change
	   drops them ( cache clear ) and relays everything out */
	const int quality = g_ctx.m_eject_restoring ? 0 : GET_VARIABLE( g_variables.m_def_hud_font_quality, int );
	if ( quality != m_hook_quality.load( std::memory_order_relaxed ) ) {
		m_hook_quality   = quality;
		m_quality_dirty  = true;
		m_scale_dirty_at = now + 0.15f;
	}
	if ( m_quality_dirty && m_scale_dirty_at > 0.f && now >= m_scale_dirty_at && resolve_root( ) ) {
		m_quality_dirty = false;
		clear_pango_cache( );
		botox_dbg_log( "HUDFONT: quality %d, poke", quality );
		walk( name.empty( ) ? "off" : "poke", false );
		m_scale_dirty_at = 0.f;
		m_next_walk      = now + 1.f;
	}

	if ( name.empty( ) ) {
		{
			std::lock_guard< std::mutex > lock( m_hook_mutex );
			m_hook_family.clear( );
		}
		if ( !m_applied.empty( ) && resolve_root( ) ) {
			walk( "off", true ); /* swap table is empty: the poke lays stock Stratum2 out again */
			botox_dbg_log( "HUDFONT: off" );
		}
		m_applied.clear( );
		m_added.clear( );
		m_family.clear( );
		m_poke_at        = 0.f;
		m_xf_dirty       = false;
		m_scale_dirty_at = 0.f;
		return;
	}

	if ( !resolve_root( ) )
		return;

	if ( name != m_added ) {
		m_added  = name;
		m_family = add_file( name );
	}

	if ( m_family.empty( ) )
		return;

	/* new face in fontconfig -> pango must forget its family list / fontsets first. fontmap arrives from the pango
	   hooks, so this may wait a frame or two */
	if ( m_need_clear ) {
		if ( !clear_pango_cache( ) )
			return;
		m_need_clear = false;
	}

	read_js_log( );

	if ( m_family != m_applied ) {
		{
			std::lock_guard< std::mutex > lock( m_hook_mutex );
			m_hook_family = m_family;
		}
		botox_dbg_log( "HUDFONT: swap Stratum2* -> \"%s\", poke root %p", m_family.c_str( ), m_root );
		walk( "poke", true );
		m_applied   = m_family;
		m_poke_at   = now + 0.5f; /* labels mid-animation / re-created by valve right after: once more */
		m_next_walk = now + 1.f;
		return;
	}

	if ( m_poke_at > 0.f && now >= m_poke_at ) {
		m_poke_at = 0.f;
		walk( "poke", false );
		m_next_walk = now + 1.f;
		return;
	}

	if ( m_xf_dirty ) { /* nudge moved: every text panel takes the new transform at once */
		m_xf_dirty = false;
		walk( "poke", false );
		m_next_walk = now + 1.f;
		return;
	}

	if ( m_scale_dirty_at > 0.f && now >= m_scale_dirty_at ) { /* scale settled: relayout everything at the new size */
		m_scale_dirty_at = 0.f;
		botox_dbg_log( "HUDFONT: scale %.2f, poke", m_hook_scale.load( ) );
		walk( "poke", false );
		m_next_walk = now + 1.f;
		return;
	}

	if ( now >= m_next_walk ) { /* rows made since ( killfeed, chat ) get the transform; fonts need no walk, the hook does them */
		walk( "tick", false );
		m_next_walk = now + 1.f;
	}
}
