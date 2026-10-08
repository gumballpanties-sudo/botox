#include "moi_hud.h"
#include "image_cache.h"
#include "moi_manifest.h"
#include "moi_transform.h"
#include "scaleform.h"
#include "../../chud_hud/chud_hud.h"
#include "../../../globals/includes/includes.h"
#include "../../../utilities/console/console.h"
#include "../../../utilities/perf/perf_watch.h"
#include "moi_image.h"

#include <windows.h>

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <sstream>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace n_moi_manifest;

void botox_dbg_log( const char* fmt, ... );

namespace
{
	constexpr int k_worker_count = 4;

	// @defines moi keeps in its own csgostyles.css (never served), prepended to the sheets that use them
	const std::vector< std::string > k_moi_defines = { "BackgroundOpacity",     "PixelGap",           "MVPFlashAnimationSpeed",
	                                                   "MVPFactAnimationSpeed", "AlertAnimationSpeed", "DefuseAnimationSpeed",
	                                                   "PermenentTeamPopup",    "middIe" };

	const std::vector< std::string > k_folded_styles = { "styles/styles.css" };

	bool is_folded( const std::string& key ) { return std::find( k_folded_styles.begin( ), k_folded_styles.end( ), key ) != k_folded_styles.end( ); }

	const std::vector< std::string > k_moi_scripts = { "alert", "alphachanger", "basehud", "spectatorweapon" };

	const std::vector< std::string > k_dedupe_ids = { "WeaponName" };

	const std::string& commit12( )
	{
		static const std::string c = std::string( k_commit ).substr( 0, 12 );
		return c;
	}

	/* csgo\materials\panorama\images\botox_cache\moi\<commit>\ = file://{images}/botox_cache/moi/<commit>/.
	   per commit: a manifest bump never mixes old and new files. "" = can't create, moi stays stock */
	const std::string& root_dir( )
	{
		static const std::string dir = [ ] ( ) -> std::string {
			char exe[ MAX_PATH ]{ };
			if ( !GetModuleFileNameA( nullptr, exe, MAX_PATH ) )
				return { };

			const fs::path path = fs::path( exe ).parent_path( ) / "csgo" / "materials" / "panorama" / "images" / "botox_cache" / "moi" / commit12( );

			std::error_code error;
			fs::create_directories( path, error );
			return error ? std::string( ) : path.string( ) + "\\";
		}( );

		return dir;
	}

	const std::string& images_url( )
	{
		static const std::string url = "file://{images}/botox_cache/moi/" + commit12( ) + "/";
		return url;
	}

	std::string backslash( std::string s )
	{
		std::replace( s.begin( ), s.end( ), '/', '\\' );
		return s;
	}

	bool file_exists( const std::string& path )
	{
		const DWORD attributes = GetFileAttributesA( path.c_str( ) );
		return attributes != INVALID_FILE_ATTRIBUTES && !( attributes & FILE_ATTRIBUTE_DIRECTORY );
	}

	bool read_file( const std::string& path, std::string& out )
	{
		std::ifstream file( path, std::ios::binary );
		if ( !file )
			return false;

		std::stringstream buffer;
		buffer << file.rdbuf( );
		out = buffer.str( );
		return true;
	}

	bool write_file( const std::string& path, const std::string& text )
	{
		std::error_code error;
		fs::create_directories( fs::path( path ).parent_path( ), error );

		std::ofstream file( path, std::ios::binary | std::ios::trunc );
		if ( !file )
			return false;

		file.write( text.data( ), static_cast< std::streamsize >( text.size( ) ) );
		return static_cast< bool >( file );
	}

	std::string src_path( const std::string& key ) { return root_dir( ) + "src\\" + backslash( key ); }
	std::string image_path( const std::string& rel ) { return root_dir( ) + "images\\" + backslash( rel ); }
	std::string killfeed_path( const std::string& name ) { return root_dir( ) + "killfeed\\" + name + ".png"; }
	std::string select_path( const std::string& name ) { return root_dir( ) + "select\\" + name + ".svg"; }
	std::string served_path( const std::string& key ) { return root_dir( ) + "served\\" + backslash( key ) + ".txt"; }

	std::string served_relative( const std::string& key )
	{
		return "materials\\panorama\\images\\botox_cache\\moi\\" + commit12( ) + "\\served\\" + backslash( key ) + ".txt";
	}
	std::string csgostyles_path( ) { return src_path( "styles/csgostyles.css" ); }

	bool is_moi_only( const std::string& key )
	{
		return std::find( std::begin( k_moi_only ), std::end( k_moi_only ), key ) != std::end( k_moi_only );
	}

	std::string resolve_image( const std::string& rel )
	{
		static const std::unordered_set< std::string > images( std::begin( k_images ), std::end( k_images ) );

		if ( !images.count( rel ) )
			return { };

		return file_exists( image_path( rel ) ) ? images_url( ) + "images/" + rel : std::string( k_raw_base ) + "csgo/materials/panorama/images/" + rel;
	}

	struct job_t {
		std::string url;
		std::string destination;
	};

	std::vector< job_t > s_jobs;
	std::atomic< size_t > s_next_job{ 0 };

	void worker( )
	{
		// below normal: shares cores with the main / render thread
		n_perf::background_thread( );

		while ( !g_moi_hud.m_shutdown ) {
			const size_t index = s_next_job.fetch_add( 1 );
			if ( index >= s_jobs.size( ) )
				break;

			const job_t& job = s_jobs[ index ];

			std::error_code error;
			fs::create_directories( fs::path( job.destination ).parent_path( ), error );

			if ( n_image_cache::download_file( job.url, job.destination ) )
				++g_moi_hud.m_done;
			else {
				++g_moi_hud.m_failed;
				g_console.print< n_console::log_level::WARNING >( ( "moi hud: download failed " + job.url + "\n" ).c_str( ) );
			}
		}

		--g_moi_hud.m_workers;
	}

	using reload_changed_file_t = void( __thiscall* )( void* layout_manager, const char* file );

	reload_changed_file_t reload_changed_file( )
	{
		// CLayoutManager::ReloadChangedFile, finds "The following file has been changed on disk..." msgbox
		static const auto fn =
			reinterpret_cast< reload_changed_file_t >( g_modules[ PANORAMA_DLL ].find_pattern( "55 8B EC 81 EC B8 04 00 00 53 56 8B D9 57 89 5D F4 E8" ) );
		return fn;
	}

	void reinit_hud_elements( )
	{
		static const auto site = g_modules[ CLIENT_DLL ].find_pattern( "B9 ? ? ? ? E8 ? ? ? ? 8B 5D 08 85 C0" );
		if ( !site || !g_interfaces.m_engine_client->is_in_game( ) )
			return;

		const auto hud      = *reinterpret_cast< uint8_t** >( site + 1 );
		const auto elements = *reinterpret_cast< void*** >( hud + 0x1C );
		const int count     = *reinterpret_cast< int* >( hud + 0x28 );

		for ( int i = 0; i < count; ++i ) {
			void* element    = elements[ i ];
			const char* name = element ? g_virtual.call< const char* >( element, 12 ) : nullptr;
			if ( !name || strncmp( name, "CCSGO_Hud", 9 ) || !strcmp( name, "CCSGO_HudRadar" ) || !strcmp( name, "CCSGO_HudChat" ) )
				continue;

			g_virtual.call< void >( element, 7 );
			g_virtual.call< void >( element, 6 );
			g_virtual.call< void >( element, 15, false );
		}
	}

	void run_js( c_uipanel* panel, const std::string& js )
	{
		auto engine = g_interfaces.m_panorama->access_ui_engine( );
		if ( !engine || !panel || !engine->is_valid_panel_ptr( panel ) )
			return;

		engine->run_script( panel, js.c_str( ), "panorama/layout/hud/hud.xml", 8, 10, false, false );
	}

	constexpr const char* k_vis_save = R"js(
(function () {
    MoiVis = [];
    var walk = function ( p ) {
        if ( p.paneltype.indexOf( 'CSGOHud' ) === 0 && p.paneltype !== 'CSGOHudWinPanel' )
            MoiVis.push( [ p, p.visible ] );
        for ( var i = 0, n = p.GetChildCount(); i < n; ++i )
            walk( p.GetChild( i ) );
    };
    try { walk( $.GetContextPanel() ); } catch ( e ) {}
})();
)js";

	constexpr const char* k_vis_restore = R"js(
(function () {
    if ( typeof MoiVis !== 'object' || !MoiVis )
        return;
    for ( var i = 0; i < MoiVis.length; ++i ) {
        var p = MoiVis[ i ][ 0 ], v = MoiVis[ i ][ 1 ];
        try { if ( p.IsValid() && p.visible !== v ) p.visible = v; } catch ( e ) {}
    }
    MoiVis = null;
})();
)js";

	constexpr const char* k_alpha_clear = R"js(
(function () {
    var cls = [ 'weaponpanelbgcenter', 'hud-centertop', 'hud-HA-bg-center', 'hud-HA-bg-h', 'money-text-bg',
                'weaponpanelbgcenterborder', 'hud-centertop-border', 'hud-HA-bg-center-border' ];
    for ( var i = 0; i < cls.length; ++i ) {
        var a = $.GetContextPanel().FindChildrenWithClassTraverse( cls[ i ] ) || [];
        for ( var k = 0; k < a.length; ++k )
            try { a[ k ].style.opacity = null; } catch ( e ) {}
    }
})();
)js";

	/* hud.xml / base_hud.xml bits (never served, reloading them rebuilds every element) + ports of moi's four
	   scripts. loops die when a newer install bumps MoiGen or moi goes off. ${on} = true / false */
	constexpr const char* k_install = R"js(
(function () {
    var root = $.GetContextPanel();
    MoiGen = ( typeof MoiGen === 'number' ? MoiGen : 0 ) + 1;
    MoiOn = ${on};
    var gen = MoiGen;

    var T = function ( f ) { try { f(); } catch ( e ) {} };
    var S = function ( p, k, v ) { try { p.style[ k ] = v; } catch ( e ) {} };
    var first = function ( p, c ) { var a = p ? p.FindChildrenWithClassTraverse( c ) : null; return a && a.length ? a[ 0 ] : null; };
    // loop lookups: a whole hud traverse per panel per tick was 50 walks a second. re-found once a reload kills it
    var P = {};
    var get = function ( k, find ) { var p = P[ k ]; if ( !p || !p.IsValid() ) p = P[ k ] = find(); return p; };

    // moi hud.xml: shield alert + tablet hints hidden, win panel drawn above the top right corner
    T( function () {
        var ids = [ 'ShieldDamageAlert', 'HudTabletHints' ];
        for ( var i = 0; i < ids.length; ++i ) {
            var p = root.FindChildTraverse( ids[ i ] );
            if ( p )
                p.SetHasClass( 'afxhidden', MoiOn );
        }
    } );

    T( function () {
        var w = root.FindChildTraverse( 'HudWinPanel' );
        var a = root.FindChildTraverse( MoiOn ? 'HudTopRight' : 'HudSpecPlayer' );
        if ( w && a )
            root.MoveChildAfter( w, a );
    } );

    if ( !MoiOn )
        return;

    // alert.js: scaleform hint box grow-in, alert / info icon, 35% chance of a short flash when it shows
    var shown = null, alerted = null, flashing = false;
    var hint = function () {
        var hudhint = get( 'hudhint', function () { return root.FindChildTraverse( 'HudHintText' ); } );
        var box = get( 'box', function () { return first( root, 'hud-hint' ); } );
        if ( !hudhint || !box )
            return;

        var bg = get( 'bg', function () { return first( box, 'hud-hint_bg' ); } );
        var prio = get( 'prio', function () { return first( box, 'hud-hint__priority-label' ); } );
        var label = get( 'label', function () { return first( box, 'hud-hint__label' ); } );
        var icon = get( 'icon', function () { return first( box, 'hud-hint__icon' ); } );
        var visible = box.BHasClass( 'hud-hint--visible' );
        var edge = visible !== shown;

        if ( bg && edge ) {
            if ( visible ) {
                /* probe 09-30: grow started in the same frame as the hint's ui-scale write (below) from
                   opacity 0 laid out width 0 the whole tween, then snapped to 544 (hide, from opacity 1,
                   tweened fine). start it one frame later */
                $.Schedule( 0.001, function () {
                    T( function () {
                        if ( MoiGen !== gen || !shown || !bg.IsValid() )
                            return;
                        S( bg, 'width', '544px' );
                        S( bg, 'opacity', '1' );
                    } );
                } );
            } else {
                S( box, 'opacity', '1' );
                S( bg, 'height', '104px' );
                S( bg, 'width', '0px' );
                S( bg, 'horizontalAlign', 'left' );
                S( bg, 'marginLeft', '56px' );
                S( bg, 'backgroundColor', 'gradient( linear, 0% 0%, 100% 0%, from( #000000CC ), color-stop( 0.65, #000000CC ), to( #00000000 ) )' );
                S( bg, 'position', '0px 52px 0px' );
                // not 0: a fully transparent panel gets skipped by layout. width 0 hides it anyway
                S( bg, 'opacity', '0.01' );
            }
            S( bg, 'transitionDuration', '0.07s' );
            S( bg, 'transitionDelay', '0.11s , 0.11s' );
            S( bg, 'transitionProperty', 'width, opacity' );
            S( bg, 'transitionTimingFunction', 'cubic-bezier(0.42, 0, 0.58, 1)' );
        }

        if ( visible && label && icon && prio ) {
            var t = label.text || '';
            var alert = t.indexOf( 'drop' ) !== -1 || t.indexOf( 'picked' ) !== -1 || t.indexOf( 'killed' ) !== -1 || t.indexOf( 'planted' ) !== -1;
            var img = alert ? 'file://{images}/hud/ui/alert.png' : 'file://{images}/hud/ui/alertinfo.png';

            if ( edge && !flashing && Math.random() <= 0.35 ) {
                flashing = true;
                icon.SetImage( 'file://{images}/hud/ui/alertflash.png' );
                $.Schedule( 0.08, function () {
                    flashing = false;
                    T( function () { if ( icon.IsValid() ) icon.SetImage( img ); } );
                } );
            } else if ( !flashing && ( edge || alert !== alerted ) )
                icon.SetImage( img );

            if ( edge || alert !== alerted ) {
                S( prio, 'visibility', alert ? 'visible' : 'collapse' );
                S( hudhint, 'uiScale', alert ? '90%' : '80%' );
            }
            alerted = alert;
        }

        shown = visible;
    };

    var hintLoop = function () {
        if ( MoiGen !== gen || !MoiOn )
            return;
        T( hint );
        $.Schedule( 1 / 30, hintLoop );
    };
    hintLoop();

    // spectatorweapon.js: econ image + name of the spectated player's weapon
    var lastId = null;
    var spec = function () {
        var sp = get( 'sp', function () { return root.FindChildTraverse( 'HudSpecPlayer' ); } );
        var el = sp ? get( 'el', function () { return sp.FindChildTraverse( 'SpectatorWeaponDisplay' ); } ) : null;
        if ( !el )
            return;
        var id = GameStateAPI.GetPlayerActiveWeaponItemId( GameStateAPI.GetHudPlayerXuid() );
        if ( id === lastId )
            return;
        lastId = id;
        el.SetImage( 'file://{images_econ}/' + InventoryAPI.GetItemInventoryImage( id ) + '.png' );
        var name = sp.FindChildTraverse( 'WeaponName' );
        if ( name )
            name.text = $.LocalizeSafe( InventoryAPI.GetItemName( id ) );
    };

    var specLoop = function () {
        if ( MoiGen !== gen || !MoiOn )
            return;
        T( spec );
        $.Schedule( 0.05, specLoop );
    };
    specLoop();

    /* PROBE 09-30, hint only (verifies the grow fix above), remove once read. per panorama frame
       ms:bg width after each show/hide flip, a tween shows middle widths. console "moi probe:" lines */
    var say = function ( s ) {
        s = ( 'moi probe: ' + s ).replace( /["';]/g, '' );
        try { GameInterfaceAPI.ConsoleCommand( 'echo "' + s + '"' ); } catch ( e ) { $.Msg( s ); }
    };
    var pr = { hvis: null, ht: 0, hw: [] };
    var probe = function () {
        if ( MoiGen !== gen || !MoiOn )
            return;
        var t = Date.now();

        var box = get( 'box', function () { return first( root, 'hud-hint' ); } );
        var bg = box ? get( 'bg', function () { return first( box, 'hud-hint_bg' ); } ) : null;
        if ( box && bg ) {
            var vis = box.BHasClass( 'hud-hint--visible' );
            if ( pr.hvis !== null && vis !== pr.hvis ) { pr.ht = t; pr.hw = []; pr.hdir = vis ? 'show' : 'hide'; }
            pr.hvis = vis;
            if ( pr.ht && t - pr.ht <= 400 )
                pr.hw.push( ( t - pr.ht ) + ':' + Math.round( bg.actuallayoutwidth ) );
            else if ( pr.ht ) {
                say( 'hint ' + pr.hdir + ' ' + pr.hw.join( ' ' ) );
                pr.ht = 0;
            }
        }

        $.Schedule( 0.001, probe );
    };
    probe();

    // alphachanger.js: cl_hud_background_alpha on moi's plates, borders off below 0.001. C++ calls on change
    MoiAlpha = function ( a ) {
        var set = function ( c, v ) { var p = first( root, c ); if ( p ) S( p, 'opacity', '' + v ); };
        set( 'weaponpanelbgcenter', a );
        set( 'hud-centertop', a );
        set( 'hud-HA-bg-center', a );
        set( 'hud-HA-bg-h', a );
        set( 'money-text-bg', a );
        var b = a > 0.001 ? a : 0;
        set( 'weaponpanelbgcenterborder', b );
        set( 'hud-centertop-border', b );
        set( 'hud-HA-bg-center-border', b );
    };

    // basehud.js: cl_hud_healthammo_style 1 = simple health plate. C++ calls on change
    MoiSimple = function ( simple ) {
        var c = first( root, 'hud-HA-bg-center' ), b = first( root, 'hud-HA-bg-center-border' );
        if ( !c || !b )
            return;
        c.SetImage( simple ? 'file://{images}/hud/healtharmor/hudsimple.png' : 'file://{images}/hud/healtharmor/healthbg.png' );
        b.SetImage( simple ? 'file://{images}/hud/healtharmor/hudsimpleborder.png' : 'file://{images}/hud/healtharmor/healthbgborder.png' );
    };
})();
)js";

	uint8_t* s_killfeed_call      = nullptr;
	uintptr_t s_killfeed_stock    = 0;
	bool s_killfeed_patched       = false;

	const char* __cdecl killfeed_pick( const char* format, const char* weapon )
	{
		return g_moi_hud.killfeed_format( format, weapon );
	}

	__declspec( naked ) void killfeed_thunk( )
	{
		__asm {
			push ecx
			push edx
			push dword ptr [ esp + 0x14 ]
			push dword ptr [ esp + 0x14 ]
			call killfeed_pick
			add esp, 8
			mov dword ptr [ esp + 0x10 ], eax
			pop edx
			pop ecx
			jmp dword ptr [ s_killfeed_stock ]
		}
	}
}

void n_moi_hud::impl_t::start_downloads( )
{
	if ( root_dir( ).empty( ) ) {
		g_console.print< n_console::log_level::WARNING >( "moi hud: can't create the cache folder, stock hud stays\n" );
		return;
	}

	s_jobs.clear( );
	s_next_job = 0;
	m_done     = 0;
	m_failed   = 0;

	const auto add = [ this ]( std::string url, std::string destination ) {
		++m_total;
		if ( file_exists( destination ) )
			++m_done;
		else
			s_jobs.push_back( { std::move( url ), std::move( destination ) } );
	};

	m_total = 0;

	for ( const char* key : k_layouts )
		add( std::string( k_raw_base ) + "csgo/panorama/" + key, src_path( key ) );
	for ( const char* key : k_styles )
		add( std::string( k_raw_base ) + "csgo/panorama/" + key, src_path( key ) );

	add( std::string( k_raw_base ) + "csgo/panorama/styles/csgostyles.css", csgostyles_path( ) );

	for ( const char* rel : k_images )
		add( std::string( k_raw_base ) + "csgo/materials/panorama/images/" + rel, image_path( rel ) );
	for ( const char* name : k_killfeed_icons )
		add( std::string( k_raw_base ) + "addons/p_deathnotice_override/materials/panorama/images/icons/equipment/" + name + ".png", killfeed_path( name ) );
	for ( const char* name : k_select_icons )
		add( std::string( k_raw_base ) + "addons/p_weaponselection_override/materials/panorama/images/icons/equipment/" + name + ".svg", select_path( name ) );

	g_console.print( std::format( "moi hud: {} files, {} to download\n", m_total, s_jobs.size( ) ).c_str( ) );

	const int workers = static_cast< int >( std::min< size_t >( k_worker_count, s_jobs.size( ) ) );
	m_workers += workers; // before the threads: a worker finishing early must not see 0 for the rest

	for ( int i = 0; i < workers; ++i )
		std::thread( worker ).detach( );
}

bool n_moi_hud::impl_t::build_served( )
{
	std::string moi_styles;
	if ( !read_file( csgostyles_path( ), moi_styles ) )
		return false;

	for ( const auto& key : k_folded_styles ) {
		std::string text;
		if ( read_file( src_path( key ), text ) )
			moi_styles += "\n" + text;
	}

	const auto defines = n_moi_transform::parse_defines( moi_styles, k_moi_defines );

	// before anything is served: panorama must never load the 4159px original (no-op once shrunk)
	for ( const char* rel : k_images ) {
		if ( const int shrunk = n_moi_image::shrink_if_oversize( image_path( rel ) ) )
			botox_dbg_log( "moi hud: %s %s", shrunk > 0 ? "shrunk" : "CAN'T shrink", rel );
	}

	std::unordered_map< std::string, std::string > served;

	const auto serve = [ & ]( const std::string& key ) {
		if ( is_folded( key ) )
			return true;

		std::string text;
		if ( !read_file( src_path( key ), text ) || text.empty( ) )
			return is_moi_only( key );

		text = n_moi_transform::rewrite_images( text, resolve_image );
		text = key.ends_with( ".xml" ) ? n_moi_transform::dedupe_ids(
		                                     n_moi_transform::strip_includes( n_moi_transform::strip_script_includes( text, k_moi_scripts ), k_folded_styles ),
		                                     k_dedupe_ids )
		                               : n_moi_transform::patch_css( text, defines );

		if ( !write_file( served_path( key ), text ) )
			return false;

		served[ key ] = served_relative( key );
		return true;
	};

	for ( const char* key : k_styles ) {
		if ( !serve( key ) )
			return false;
	}

	for ( const char* key : k_layouts ) {
		if ( !serve( key ) )
			return false;
	}

	m_served.swap( served );

	m_killfeed.clear( );
	for ( const char* name : k_killfeed_icons ) {
		if ( file_exists( killfeed_path( name ) ) )
			m_killfeed.insert( name );
	}

	return true;
}

void n_moi_hud::impl_t::reload_all( c_uipanel* hud_root )
{
	const auto reload = reload_changed_file( );
	auto engine       = g_interfaces.m_panorama->access_ui_engine( );
	void* manager     = engine ? static_cast< void* >( engine->ui_layout_manager( ) ) : nullptr;

	if ( !reload || !manager ) {
		g_console.print< n_console::log_level::WARNING >( "moi hud: no CLayoutManager::ReloadChangedFile, hud won't switch until a restart\n" );
		return;
	}

	const bool on = m_serving;

	const auto one = [ & ]( const std::string& key ) {
		// moi-only files have no stock copy: reloading them off = a failed read panorama retries forever. folded = never loaded
		if ( is_folded( key ) || ( !on && is_moi_only( key ) ) )
			return;

		reload( manager, ( "panorama\\" + backslash( key ) ).c_str( ) );
		reload( manager, ( "panorama/" + key ).c_str( ) );
	};

	run_js( hud_root, k_vis_save );

	for ( const char* key : k_styles )
		one( key );
	for ( const char* key : k_layouts )
		one( key );

	reinit_hud_elements( );

	run_js( hud_root, k_vis_restore );

	g_scaleform.m_should_force_update = true;
	g_chud.m_should_force_update      = true;
	m_js_applied                      = false;
}

void n_moi_hud::impl_t::apply_js( c_uipanel* hud_root, bool on )
{
	std::string js = k_install;

	const std::string token = "${on}";
	if ( const size_t at = js.find( token ); at != std::string::npos ) // npos = replace throws on the game thread
		js.replace( at, token.size( ), on ? "true" : "false" );

	run_js( hud_root, n_moi_transform::rewrite_images( js, resolve_image ) );
}

void n_moi_hud::impl_t::push_cvars( c_uipanel* hud_root, bool force )
{
	const float alpha = std::clamp( g_convars.float_or( HASH_BT( "cl_hud_background_alpha" ), 1.f ), 0.f, 1.f );
	const int simple  = g_convars.int_or( HASH_BT( "cl_hud_healthammo_style" ), 0 ) != 0 ? 1 : 0;

	if ( force || alpha != m_old_alpha ) {
		run_js( hud_root, std::format( "try {{ if ( typeof MoiAlpha === 'function' ) MoiAlpha( {:.2f} ); }} catch ( e ) {{}}", alpha ) );
		m_old_alpha = alpha;
	}

	if ( force || simple != m_old_simple ) {
		run_js( hud_root, std::format( "try {{ if ( typeof MoiSimple === 'function' ) MoiSimple( {} ); }} catch ( e ) {{}}", simple ) );
		m_old_simple = simple;
	}
}

bool n_moi_hud::impl_t::patch_killfeed( bool on )
{
	if ( !s_killfeed_call ) {
		auto site = g_modules[ CLIENT_DLL ].find_pattern( "E8 ? ? ? ? 8D 8C 24 ? ? ? ? 83 C4 0C 8D 51 01" );
		if ( !site || site[ 0 ] != 0xE8 ) {
			g_console.print< n_console::log_level::WARNING >( "moi hud: killfeed call site not found, stock killfeed icons\n" );
			return false;
		}

		s_killfeed_call  = site;
		s_killfeed_stock = reinterpret_cast< uintptr_t >( site + 5 ) + *reinterpret_cast< int32_t* >( site + 1 );
	}

	if ( on == s_killfeed_patched )
		return true;

	const uintptr_t target = on ? reinterpret_cast< uintptr_t >( &killfeed_thunk ) : s_killfeed_stock;
	const int32_t rel      = static_cast< int32_t >( target - reinterpret_cast< uintptr_t >( s_killfeed_call + 5 ) );

	DWORD old = 0;
	if ( !VirtualProtect( s_killfeed_call + 1, 4, PAGE_EXECUTE_READWRITE, &old ) )
		return false;

	*reinterpret_cast< int32_t* >( s_killfeed_call + 1 ) = rel;

	VirtualProtect( s_killfeed_call + 1, 4, old, &old );
	FlushInstructionCache( GetCurrentProcess( ), s_killfeed_call, 5 );

	s_killfeed_patched = on;
	return true;
}

void n_moi_hud::impl_t::update( c_uipanel* hud_root, bool wanted )
{
	if ( wanted && !m_started ) {
		m_started = true;
		start_downloads( );
	}

	if ( !wanted ) {
		if ( m_serving ) {
			m_serving = false;
			patch_killfeed( false );
			run_js( hud_root, k_alpha_clear );
			reload_all( hud_root );
			m_js_off_pending = true;
			g_console.print( "moi hud: off, stock files reloaded\n" );
			botox_dbg_log( "SF: moi off, root=%d", hud_root != nullptr );
		}

		if ( m_js_off_pending && hud_root ) {
			apply_js( hud_root, false );
			m_js_off_pending = false;
		}

		m_checked_at = -1;

		if ( m_started && m_workers == 0 && m_failed > 0 )
			m_started = false;

		return;
	}

	m_js_off_pending = false;

	const bool finished = m_workers == 0 && m_done + m_failed >= m_total;

	if ( !m_serving ) {
		if ( !finished || m_done == m_checked_at )
			return;
		m_checked_at = m_done;

		bool text_ready = file_exists( csgostyles_path( ) );
		for ( const char* key : k_layouts )
			text_ready = text_ready && ( file_exists( src_path( key ) ) || is_moi_only( key ) );
		for ( const char* key : k_styles )
			text_ready = text_ready && ( file_exists( src_path( key ) ) || is_moi_only( key ) );

		if ( !text_ready || !build_served( ) ) {
			g_console.print< n_console::log_level::WARNING >( "moi hud: hud files missing after download, stock hud stays (toggle to retry)\n" );
			botox_dbg_log( "moi hud: not served, text_ready=%d done=%d failed=%d total=%d", ( int )text_ready, m_done.load( ), m_failed.load( ), m_total );
			return;
		}
		botox_dbg_log( "moi hud: on, %d served, %d/%d cached, %d failed", ( int )m_served.size( ), m_done.load( ), m_total, m_failed.load( ) );

		m_serving = true;
		patch_killfeed( true );
		reload_all( hud_root );
		g_console.print( std::format( "moi hud: on, {} files served, {}/{} cached, {} failed\n", m_served.size( ), m_done.load( ), m_total,
		                              m_failed.load( ) )
		                     .c_str( ) );
	}

	// weapon select svgs: built once when downloads end, immutable after (decode threads read it)
	if ( finished && !m_select.load( std::memory_order_acquire ) ) {
		// ponytail: never freed, one map per session (~100kb), a decode thread may hold it at unload
		auto* map = new std::unordered_map< std::string, std::string >( );
		for ( const char* name : k_select_icons ) {
			std::string svg;
			if ( read_file( select_path( name ), svg ) && !svg.empty( ) )
				( *map )[ name ] = std::move( svg );
		}
		m_select.store( map, std::memory_order_release );
	}

	if ( !hud_root )
		return;

	if ( !m_js_applied ) {
		apply_js( hud_root, true );
		push_cvars( hud_root, true );
		m_js_applied = true;
	} else
		push_cvars( hud_root, false );
}

void n_moi_hud::impl_t::on_level_init( )
{
	m_js_applied = false;
	m_old_alpha  = -1.f;
	m_old_simple = -1;
}

void n_moi_hud::impl_t::on_release( )
{
	m_serving = false;
	patch_killfeed( false );
}

const char* n_moi_hud::impl_t::redirect( const char* path )
{
	if ( !m_serving.load( std::memory_order_relaxed ) || !path )
		return nullptr;

	const std::string key = n_moi_transform::panorama_key( path );
	if ( key.empty( ) )
		return nullptr;

	const auto it = m_served.find( key );
	if ( it == m_served.end( ) )
		return nullptr;

	static std::unordered_set< std::string > logged;
	if ( logged.insert( key ).second )
		g_console.print( ( "moi hud: served " + std::string( path ) + "\n" ).c_str( ) );

	return it->second.c_str( );
}

void n_moi_hud::impl_t::redirect_failed( const char* path, const char* served )
{
	static std::unordered_set< std::string > logged;
	if ( logged.insert( path ? path : "" ).second )
		g_console.print< n_console::log_level::WARNING >(
			std::format( "moi hud: can't read {}, stock {} used\n", served ? served : "?", path ? path : "?" ).c_str( ) );
}

const std::string* n_moi_hud::impl_t::select_icon( const char* name )
{
	if ( !name || !m_serving.load( std::memory_order_relaxed ) )
		return nullptr;

	const auto* map = m_select.load( std::memory_order_acquire );
	if ( !map )
		return nullptr;

	const auto it = map->find( name );
	return it == map->end( ) ? nullptr : &it->second;
}

void n_moi_hud::impl_t::reload_icons( )
{
	auto engine = g_interfaces.m_panorama->access_ui_engine( );
	auto images = engine ? engine->ui_image_manager( ) : nullptr;
	if ( !images )
		return;

	for ( const char* name : k_select_icons )
		images->reload_changed_file( ( std::string( "materials\\panorama\\images\\icons\\equipment\\" ) + name + ".vsvg" ).c_str( ) );

	botox_dbg_log( "SF: icons re-decoded, %d names", ( int )std::size( k_select_icons ) );
}

const char* n_moi_hud::impl_t::killfeed_format( const char* stock_format, const char* weapon )
{
	if ( !m_serving.load( std::memory_order_relaxed ) || !weapon || !m_killfeed.count( weapon ) )
		return stock_format;

	static const std::string format = images_url( ) + "killfeed/%s.png";
	return format.c_str( );
}
