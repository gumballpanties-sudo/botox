#include "scaleform.h"
#include "image_cache.h"
#include "moi_hud.h"
#include "scaleform_js.h"
#include "scaleform_track.h"
#include "../../../globals/config/config.h"
#include "../../../globals/includes/includes.h"
#include "../../../globals/logger/logger.h"
#include "../../visuals/screen/resolution_spoof.h"
#include "../../chud_hud/chud_hud.h"
#include <string>
#include <format>
#include <cstdio>
#include <fstream>
#include <algorithm>
#include <cmath>
#include <shlobj.h>



static constexpr const char* colors[12] = {
	"#e8e8e8",
	"#FFFFFF",
	"#96c8ff",
	"#356eff",
	"#c864ff",
	"#ff2924",
	"#ff7124",
	"#fff724",
	"#3eff24",
	"#24ff90",
	"#ff7999",
	"#d3e798"
};

#define kRadarColor 0
#define kDashColor 1
static std::string get_color( int n, int color_type )
{
	if ( n < 0 )
		n = 0;
	if ( n > 11 )
		n = 11;

	if ( color_type == kRadarColor )
		return std::string( colors[ n ] ) + "19";
	else if ( color_type == kDashColor )
		return std::string( colors[ n ] ) + "B2";
	else
		return std::string( colors[ n ] );
}

static void replace_str( std::string& s, const std::string& search, const std::string& replace )
{
	for ( size_t pos = 0;; pos += replace.length( ) ) {
		pos = s.find( search, pos );
		if ( pos == std::string::npos )
			break;
		s.erase( pos, search.length( ) );
		s.insert( pos, replace );
	}
}

static const char* k_track = R"js(
if ( typeof SfLog !== 'object' || !SfLog )
    SfLog = [];
SfOn = true;
SfR = function ( p, k ) {
    if ( !SfOn )
        return {};
    if ( p ) {
        var e = null;
        for ( var i = 0; i < SfLog.length && !e; ++i )
            if ( SfLog[ i ].p === p )
                e = SfLog[ i ];
        if ( !e )
            SfLog.push( e = { p: p, k: {} } );
        e.k[ k ] = true;
    }
    return p.style;
};
SfLog = SfLog.filter( function ( e ) { try { return e.p.IsValid(); } catch ( x ) { return false; } } );
)js";

static const char* k_classic_off = R"js(
(function () {
    var root = $.GetContextPanel();
    SfOn = false; // first: pack callbacks still queued write nowhere from here

    /* null on an ALIAS throws ("Cannot set a property alias to undefined", uiengine.cpp:2707), so every classic
       marginLeft / fontSize / x / horizontalAlign ... stayed after the undo -> stock hud shifted until a map change.
       alias -> its base property (DECLARE_STYLE_PROPERTY_ALIAS list, renderer/styleproperties.cpp); nulling the base
       drops the inline value of every part, css takes over */
    var base = function ( k ) {
        if ( /^[xyz]$/.test( k ) ) return 'position';
        if ( /^margin(Left|Top|Bottom|Right)$/.test( k ) ) return 'margin';
        if ( /^padding(Left|Top|Bottom|Right)$/.test( k ) ) return 'padding';
        if ( /^font(Family|Size|Style|Weight)$/.test( k ) ) return 'font';
        if ( /^border(Top|Bottom)(Left|Right)Radius$/.test( k ) ) return 'borderRadius';
        if ( /^border(Top|Right|Bottom|Left)?(Style|Width|Color)?$/.test( k ) && k !== 'border' ) return 'border';
        if ( /^background(Size|Position|Repeat)$/.test( k ) ) return 'backgroundImage';
        if ( /^transition(Property|Duration|TimingFunction|Delay)$/.test( k ) ) return 'transition';
        if ( /^animation(Name|Duration|TimingFunction|IterationCount|Direction|Delay|FillMode)$/.test( k ) ) return 'animation';
        if ( /^(horizontal|vertical)Align$/.test( k ) ) return 'align';
        if ( /^uiScale[XYZ]$/.test( k ) ) return 'uiScale';
        if ( /^opacityMaskScroll/.test( k ) ) return 'opacityMask';
        if ( k === 'textShadowFast' ) return 'textShadow';
        if ( k === 'washColorFast' ) return 'washColor';
        return k;
    };
    if ( typeof SfLog === 'object' && SfLog ) {
        for ( var i = 0; i < SfLog.length; ++i ) {
            var e = SfLog[ i ], live = false;
            try { live = e.p.IsValid(); } catch ( x ) {}
            if ( !live )
                continue;
            var done = {};
            for ( var k in e.k ) {
                var b = base( k );
                if ( done[ b ] )
                    continue;
                done[ b ] = true;
                try { e.p.style[ b ] = null; } catch ( x ) {}
            }
        }
        SfLog = [];
    }

    var ids = [ 'buyzoneicon2', 'hud-HA-icon-Healthsf', 'hud-HA-icon-Armorsf', 'hud-HA-icon-Helmetsf', 'hudhabarborder',
                'hudhabarborder2', 'weaponrowoutline-Center', 'hud-hint__icon2' ];
    var dead = [];
    /* per node try: one throwing panel used to abort the whole walk (one outer try) and every created panel after
       it in tree order was never found -> hint icon survived every switch (10-04) */
    var walk = function ( p ) {
        // GetChild hands back some non-panel objects (no GetChildCount, 10-04): skip them
        if ( !p || typeof p.GetChildCount !== 'function' )
            return;
        try {
            if ( ids.indexOf( p.id ) !== -1 && dead.indexOf( p ) === -1 ) {
                dead.push( p );
                return;
            }
        } catch ( x ) {}
        var n = 0;
        try { n = p.GetChildCount(); } catch ( x ) {}
        for ( var c = 0; c < n; ++c ) {
            var ch = null;
            try { ch = p.GetChild( c ); } catch ( x ) {}
            walk( ch );
        }
    };
    walk( root );
    // the engine's own traverse as a second net (first match per id)
    for ( var f = 0; f < ids.length; ++f ) {
        try {
            var hit = root.FindChildTraverse( ids[ f ] );
            if ( hit && dead.indexOf( hit ) === -1 )
                dead.push( hit );
        } catch ( x ) {}
    }
    /* synchronous, not DeleteAsync: moi's reload right after this (same tick) kept the async-deleted hint icon alive
       and wiped its inline 110px -> 451px alert.png full size (10-04). RemoveAndDeleteChildren =
       OnDeletePanel now, so reparent into a throwaway and empty it */
    if ( dead.length ) {
        try {
            var trash = $.CreatePanel( 'Panel', root, '' );
            for ( var d = 0; d < dead.length; ++d )
                try { dead[ d ].SetParent( trash ); } catch ( x ) {}
            trash.RemoveAndDeleteChildren();
            trash.DeleteAsync( 0 );
        } catch ( x ) {
            for ( var a = 0; a < dead.length; ++a )
                try { dead[ a ].DeleteAsync( 0 ); } catch ( y ) {}
        }
    }

    // tick( ) forced it visible every frame from C++, not through SfR
    try { var bg = root.FindChildTraverse( 'WeaponPanelBottomBG' ); if ( bg ) bg.style.visibility = null; } catch ( x ) {}

    /* pack SetImage calls are no style write, SfR never saw them. stock src from code.pbin (winpanel star, deathnotice
       snippet): winpanel isn't reloaded, kept killfeed rows never are */
    var src = function ( list, url ) {
        for ( var i = 0; i < list.length; ++i )
            try { if ( list[ i ] ) list[ i ].SetImage( url ); } catch ( x ) {}
    };
    try {
        var wp = root.FindChildTraverse( 'HudWinPanel' );
        if ( wp )
            src( wp.FindChildrenWithClassTraverse( 'MVP__WinnerStar' ) || [], 'file://{images}/icons/ui/star.svg' );
    } catch ( x ) {}
    try {
        var rows = root.FindChildrenWithClassTraverse( 'DeathNotice' ) || [];
        for ( var r = 0; r < rows.length; ++r ) {
            src( [ rows[ r ].FindChildTraverse( 'HeadShot' ) ], 'file://{images}/hud/deathnotice/icon_headshot.svg' );
            src( [ rows[ r ].FindChildTraverse( 'Penetrate' ) ], 'file://{images}/hud/deathnotice/penetrate.svg' );
        }
    } catch ( x ) {}
})();
)js";

static void run( const char* js )
{
	if ( !g_scaleform.m_uiengine || !g_scaleform.m_hud_panel )
		return;

	const std::string tracked = k_track + n_scaleform_track::track_styles( js );
	const std::string cached  = g_image_cache.rewrite( tracked.c_str( ) );

	g_scaleform.m_uiengine->run_script( g_scaleform.m_hud_panel, cached.c_str( ), CSGO_HUD_SCHEMA, 8, 10, false, false );

	g_scaleform.m_classic_applied = true;
}

static c_uipanel* get_panel( uint32_t id_hash )
{
	if ( !g_scaleform.m_uiengine )
		return nullptr;

	auto itr = g_scaleform.m_uiengine->get_last_dispatched_event_target_panel( );
	while ( itr && g_scaleform.m_uiengine->is_valid_panel_ptr( itr ) ) {
		if ( HASH_RT( itr->get_id( ) ) == id_hash )
			return itr;
		itr = itr->get_parent( );
	}

	return nullptr;
}

static void dump_panel_recursive( c_ui_engine* uiengine, c_uipanel* panel, int depth, std::ofstream& out )
{
	if ( !panel || depth > 48 || !uiengine->is_valid_panel_ptr( panel ) )
		return;

	/* id only: get_layout_file( ) is on an unverified vtable slot and crashed */
	const char* id = panel->has_id( ) ? panel->get_id( ) : nullptr;

	out << std::string( depth * 2, ' ' ) << ( id && *id ? id : "<no id>" ) << ( panel->is_visible( ) ? "" : "  [hidden]" ) << "\n";

	const int count = panel->get_child_count( );
	if ( count <= 0 || count > 4096 )
		return;

	for ( int i = 0; i < count; ++i )
		dump_panel_recursive( uiengine, panel->get_child( i ), depth + 1, out );
}

void n_scaleform::impl_t::dump_panel_tree( )
{
	auto uiengine = m_uiengine ? m_uiengine : g_interfaces.m_panorama->access_ui_engine( );
	if ( !uiengine ) {
		g_logger.print( "no panorama ui engine\n", "[panels]" );
		return;
	}

	c_uipanel* root = ( m_hud_panel && uiengine->is_valid_panel_ptr( m_hud_panel ) ) ? m_hud_panel : nullptr;
	if ( !root ) {
		auto itr = uiengine->get_last_dispatched_event_target_panel( );

		// bounded climb - a stale pointer or a parent cycle must not spin forever
		for ( int guard = 0; itr && guard < 64 && uiengine->is_valid_panel_ptr( itr ); ++guard ) {
			root     = itr;
			auto par = itr->get_parent( );
			if ( !par || par == itr || !uiengine->is_valid_panel_ptr( par ) )
				break;
			itr = par;
		}
	}

	if ( !root ) {
		g_logger.print( "no panels yet — move mouse over the game, then retry\n", "[panels]" );
		return;
	}

	char desktop[ MAX_PATH ]{ };
	if ( SHGetFolderPathA( nullptr, CSIDL_DESKTOP, nullptr, 0, desktop ) != S_OK ) {
		g_logger.print( "could not resolve desktop path\n", "[panels]" );
		return;
	}

	const std::string path = std::string( desktop ) + "\\csgo_panel_dump.txt";

	std::ofstream out( path, std::ios::trunc );
	if ( !out.is_open( ) ) {
		g_logger.print( "could not open dump file\n", "[panels]" );
		return;
	}

	dump_panel_recursive( uiengine, root, 0, out );
	out.close( );

	g_logger.print( "dumped panel tree to desktop\n", "[panels]" );
}

struct hud_panel_t {
	int m_element;
	const char* m_id;
	bool m_chud;
};

static const hud_panel_t k_hud_panels[] = {
	{ hud_element_health, "HudHealthArmor", false },
	{ hud_element_ammo, "HudWeaponPanel", false },
	{ hud_element_weapon_select, "HudWeaponSelection", false },
	{ hud_element_killfeed, "HudDeathNotice", false },
	{ hud_element_radar, "HudRadar", false },
	{ hud_element_money, "HudMoney", false },
	{ hud_element_timer, "HudTeamCounter", false },
	{ hud_element_chat, "HudChat", false },
	// csgo's chat code re-shows HudChat itself to fade closed-chat lines; its child keeps the nuke
	{ hud_element_chat, "ChatContainer", false },
	{ hud_element_alerts, "HudAlerts", false },
	{ hud_element_hint, "HudHintText", false },
	{ hud_element_radio, "HudRadio", false },
	{ hud_element_vote, "HudVote", false },
	{ hud_element_spectator, "HudSpecPlayer", false },
	{ hud_element_spectator, "HudSpectator", false },
	{ hud_element_spectator, "HudSpectatorVignetting", false },
	{ hud_element_freezepanel, "HudFreezePanel", false },
	{ hud_element_winpanel, "HudWinPanel", false },

	{ hud_element_health, "ChudHealthPill", true },
	{ hud_element_ammo, "ChudAmmoPill", true },
	{ hud_element_weapon_select, "ChudWeapons", true },
	{ hud_element_killfeed, "ChudKillfeed", true },
	{ hud_element_timer, "ChudTimerPill", true },
	{ hud_element_chat, "ChudChat", true },
};

/* hud nuke: geometry props only, never visibility ( valve's SetVisible owns that and re-shows on round start ).
   size 0 so HudBlur rects shrink too. marked, so un-hide only touches what we hid. js only, no child ptr kept */
static const char* k_hud_removal = R"js(
(function () {
    var cp = $.GetContextPanel();
    var hide = [ ${hide} ], show = [ ${show} ];
    var P = function ( id ) { try { return cp.FindChildTraverse( id ); } catch ( e ) { return null; } };
    for ( var i = 0; i < hide.length; ++i ) {
        var p = P( hide[ i ] );
        if ( !p )
            continue;
        try { p.style.opacity = '0.0'; } catch ( e ) {}
        try { p.style.width = '0px'; } catch ( e ) {}
        try { p.style.height = '0px'; } catch ( e ) {}
        try { p.style.position = '-9999px -9999px 0px'; } catch ( e ) {}
        try { p.SetAttributeString( 'bxrm', '1' ); } catch ( e ) {}
    }
    for ( var k = 0; k < show.length; ++k ) {
        var q = P( show[ k ] ), ours = false;
        try { ours = q && q.GetAttributeString( 'bxrm', '' ) === '1'; } catch ( e ) {}
        if ( !ours )
            continue;
        try { q.style.opacity = null; } catch ( e ) {}
        try { q.style.width = null; } catch ( e ) {}
        try { q.style.height = null; } catch ( e ) {}
        try { q.style.position = null; } catch ( e ) {}
        try { q.SetAttributeString( 'bxrm', '' ); } catch ( e ) {}
    }
})();
)js";

static int s_hud_rm_mask   = 0;
static int s_hud_rm_chud   = -1;
static float s_hud_rm_heal = 0.f;

static bool s_c4_icon_hidden = false;

void n_scaleform::impl_t::hud_removal( )
{
	const bool enabled = GET_VARIABLE( g_variables.m_remove_hud_elements, bool );

	{
		const auto& removed = GET_VARIABLE( g_variables.m_removed_hud_elements, std::vector< bool > );

		const bool want_gone = enabled && hud_element_c4_icon < static_cast< int >( removed.size( ) ) &&
		                       removed[ hud_element_c4_icon ];

		// edge-triggered - don't dirty the convar every tick
		if ( want_gone != s_c4_icon_hidden ) {
			g_convars.set_if_present( HASH_BT( "cl_hud_bomb_under_radar" ), want_gone ? 0.f : 1.f );
			s_c4_icon_hidden = want_gone;
		}
	}

	int mask = 0;
	if ( enabled ) {
		const auto& removed = GET_VARIABLE( g_variables.m_removed_hud_elements, std::vector< bool > );
		for ( int e = 0; e < hud_element_c4_icon && e < static_cast< int >( removed.size( ) ); ++e )
			mask |= removed[ e ] ? 1 << e : 0;
	}

	// minecraft hud draws hotbar / hearts / armor / chat (killfeed lives in chat); no radar, timer, score, money, win panel, alerts
	if ( GET_VARIABLE( g_variables.m_mc_hud, bool ) )
		mask |= 1 << hud_element_health | 1 << hud_element_ammo | 1 << hud_element_weapon_select | 1 << hud_element_killfeed |
		        1 << hud_element_radar | 1 << hud_element_money | 1 << hud_element_timer | 1 << hud_element_chat | 1 << hud_element_winpanel |
		        1 << hud_element_alerts;

	if ( !mask && !s_hud_rm_mask )
		return;

	if ( !m_uiengine ) {
		m_uiengine = g_interfaces.m_panorama->access_ui_engine( );
		if ( !m_uiengine )
			return;
	}

	bool dirty = false;
	if ( !m_removal_root || !m_uiengine->is_valid_panel_ptr( m_removal_root ) ) {
		m_removal_root = get_panel( HASH_BT( "CSGOHud" ) );
		if ( !m_removal_root )
			return;
		dirty = true;
	}

	const int chud    = GET_VARIABLE( g_variables.m_chud_hud, bool ) ? 1 : 0;
	const float now   = g_interfaces.m_global_vars_base ? g_interfaces.m_global_vars_base->m_real_time : 0.f;
	const bool change = mask != s_hud_rm_mask || chud != s_hud_rm_chud;

	if ( !dirty && !change && ( !mask || now < s_hud_rm_heal ) )
		return;

	s_hud_rm_heal = now + 1.f;

	std::string hide, show;
	for ( const auto& row : k_hud_panels ) {
		if ( row.m_chud != ( chud != 0 ) )
			continue;

		auto& list = ( mask & ( 1 << row.m_element ) ) ? hide : show;
		list += std::string( list.empty( ) ? "'" : ",'" ) + row.m_id + "'";
	}

	std::string js = k_hud_removal;
	replace_str( js, "${hide}", hide );
	replace_str( js, "${show}", show );
	m_uiengine->run_script( m_removal_root, js.c_str( ), CSGO_HUD_SCHEMA, 8, 10, false, false );

	if ( s_hud_rm_mask & ~mask ) {
		if ( chud )
			g_chud.m_should_force_update = true;
		else
			m_should_force_update = true;
	}

	if ( change )
		botox_dbg_log( "HUDRM: mask=%x was=%x chud=%d hide=[%s]", mask, s_hud_rm_mask, chud, hide.c_str( ) );

	s_hud_rm_mask = mask;
	s_hud_rm_chud = chud;
}

static const char* k_hud_aspect = R"js(
try {
    var cp = $.GetContextPanel();
    cp.style.width = '${pct}%';
    cp.style.horizontalAlign = 'center';
    cp.style.transform = 'scale3d( ${scale}, 1.0, 1.0 )';
    var ids = [ 'VisiblePlayerIDs', 'VisiblePlayerPings' ];
    for ( var i = 0; i < ids.length; ++i ) {
        var p = cp.FindChildTraverse( ids[ i ] );
        if ( p ) {
            p.style.transformOrigin = '0% 0%';
            p.style.transform = 'scale3d( ${inv}, 1.0, 1.0 )';
        }
    }
} catch(e) {}
)js";

static const char* k_hud_aspect_restore = R"js(
try {
    var cp = $.GetContextPanel();
    cp.style.width = '100%';
    cp.style.horizontalAlign = 'left';
    cp.style.transform = 'none';
    var ids = [ 'VisiblePlayerIDs', 'VisiblePlayerPings' ];
    for ( var i = 0; i < ids.length; ++i ) {
        var p = cp.FindChildTraverse( ids[ i ] );
        if ( p ) {
            p.style.transformOrigin = null;
            p.style.transform = null;
        }
    }
} catch(e) {}
)js";

static void run_on( c_uipanel* panel, const char* js )
{
	if ( !g_scaleform.m_uiengine || !panel )
		return;

	const std::string cached = g_image_cache.rewrite( js );

	g_scaleform.m_uiengine->run_script( panel, cached.c_str( ), CSGO_HUD_SCHEMA, 8, 10, false, false );
}

void n_scaleform::impl_t::hud_aspect( )
{
	static float applied = 0.f;

	const bool enabled = GET_VARIABLE( g_variables.m_aspect_ratio_enable, bool ) && GET_VARIABLE( g_variables.m_aspect_ratio_hud, bool );

	// off and never applied - the common case
	if ( !enabled && applied == 0.f )
		return;

	/* REAL screen shape, never g_ctx (follows imgui display size, which the res spoofer shrinks) */
	float screen_width  = static_cast< float >( g_resolution_spoof.m_screen_width );
	float screen_height = static_cast< float >( g_resolution_spoof.m_screen_height );

	if ( screen_width <= 0.f || screen_height <= 0.f ) {
		screen_width  = g_ctx.m_width;
		screen_height = g_ctx.m_height;
	}

	if ( screen_width <= 0.f || screen_height <= 0.f )
		return;

	if ( !m_uiengine ) {
		m_uiengine = g_interfaces.m_panorama->access_ui_engine( );
		if ( !m_uiengine )
			return;
	}

	if ( !m_aspect_root || !m_uiengine->is_valid_panel_ptr( m_aspect_root ) ) {
		m_aspect_root = get_panel( HASH_BT( "CSGOHud" ) );
		applied       = 0.f;

		if ( !m_aspect_root )
			return;
	}

	const float wanted = enabled ? GET_VARIABLE( g_variables.m_aspect_ratio, float ) : 0.f;
	if ( applied == wanted )
		return;

	if ( !enabled ) {
		run_on( m_aspect_root, k_hud_aspect_restore );
		applied = 0.f;
		return;
	}

	const float screen = screen_width / screen_height;

	const float ratio = std::clamp( wanted, 0.1f, screen );

	char percent_text[ 32 ]{ };
	char scale_text[ 32 ]{ };
	char inv_text[ 32 ]{ };
	sprintf_s( percent_text, "%.4f", ( ratio / screen ) * 100.f );
	sprintf_s( scale_text, "%.4f", screen / ratio );
	sprintf_s( inv_text, "%.6f", ratio / screen );

	std::string js = k_hud_aspect;
	replace_str( js, "${pct}", percent_text );
	replace_str( js, "${scale}", scale_text );
	replace_str( js, "${inv}", inv_text );

	run_on( m_aspect_root, js.c_str( ) );

	applied = wanted;
}

static const char* k_xhair_aspect = R"js(
try {
    var r = $.GetContextPanel().FindChildTraverse('HudReticle');
    if ( r ) {
        var x = r.FindChildTraverse('Crosshair') || r;
        x.style.transform = 'scale3d( ${scale}, 1.0, 1.0 )';
    }
} catch(e) {}
)js";

static const char* k_xhair_aspect_restore = R"js(
try {
    var r = $.GetContextPanel().FindChildTraverse('HudReticle');
    if ( r ) {
        var x = r.FindChildTraverse('Crosshair') || r;
        x.style.transform = 'none';
    }
} catch(e) {}
)js";

void n_scaleform::impl_t::crosshair_aspect( )
{
	static float applied      = 0.f;
	static float last_reapply = 0.f;

	const bool enabled = GET_VARIABLE( g_variables.m_aspect_ratio_enable, bool ) && GET_VARIABLE( g_variables.m_aspect_ratio_overlay, bool ) &&
	                     !GET_VARIABLE( g_variables.m_aspect_ratio_hud, bool );

	if ( !enabled && applied == 0.f )
		return;

	// real screen shape, never g_ctx (see hud_aspect)
	float screen_width  = static_cast< float >( g_resolution_spoof.m_screen_width );
	float screen_height = static_cast< float >( g_resolution_spoof.m_screen_height );

	if ( screen_width <= 0.f || screen_height <= 0.f ) {
		screen_width  = g_ctx.m_width;
		screen_height = g_ctx.m_height;
	}

	if ( screen_width <= 0.f || screen_height <= 0.f )
		return;

	if ( !m_uiengine ) {
		m_uiengine = g_interfaces.m_panorama->access_ui_engine( );
		if ( !m_uiengine )
			return;
	}

	if ( !m_xhair_root || !m_uiengine->is_valid_panel_ptr( m_xhair_root ) ) {
		m_xhair_root = get_panel( HASH_BT( "CSGOHud" ) );
		applied      = 0.f;

		if ( !m_xhair_root )
			return;
	}

	const float wanted = enabled ? GET_VARIABLE( g_variables.m_aspect_ratio, float ) : 0.f;
	const float now    = g_interfaces.m_global_vars_base ? g_interfaces.m_global_vars_base->m_real_time : 0.f;

	if ( applied == wanted && ( !enabled || now - last_reapply < 2.f ) )
		return;

	last_reapply = now;

	if ( !enabled ) {
		run_on( m_xhair_root, k_xhair_aspect_restore );
		applied = 0.f;
		return;
	}

	const float screen = screen_width / screen_height;

	char scale_text[ 32 ]{ };
	sprintf_s( scale_text, "%.4f", std::clamp( screen / wanted, 0.25f, 4.f ) );

	std::string js = k_xhair_aspect;
	replace_str( js, "${scale}", scale_text );

	run_on( m_xhair_root, js.c_str( ) );

	applied = wanted;
}

static constexpr int k_weapontype_knife = 0, k_weapontype_sniper = 5, k_weapontype_c4 = 7, k_weapontype_fists = 12, k_weapontype_tablet = 14;

static constexpr float k_pip_default_offset = 7.f, k_black_pip_default_offset = 2.f, k_arc_default_offset = 4.f;

static const c_color k_stock_crosshair_colors[ 4 ] = { c_color( 0x82, 0xb1, 0x16 ), c_color( 0xff, 0xcc, 0x00 ),
	                                                   c_color( 0x00, 0xff, 0xff ), c_color( 0x96, 0xff, 0xff ) };

static std::string panorama_hex( const c_color& input )
{
	char text[ 16 ]{ };
	sprintf_s( text, "#%02X%02X%02X%02X", static_cast< int >( input[ 0 ] ), static_cast< int >( input[ 1 ] ), static_cast< int >( input[ 2 ] ),
	           static_cast< int >( input[ 3 ] ) );
	return text;
}

static bool crosshair_color_needs_paint( c_color& out )
{
	c_cconvar* color_convar = g_convars[ HASH_BT( "cl_crosshaircolor" ) ];
	const int value         = color_convar ? color_convar->get_int( ) : 1;

	if ( value >= 1 && value <= 4 ) {
		out = k_stock_crosshair_colors[ value - 1 ];
		return false;
	}

	c_cconvar* red   = g_convars[ HASH_BT( "cl_crosshaircolor_r" ) ];
	c_cconvar* green = g_convars[ HASH_BT( "cl_crosshaircolor_g" ) ];
	c_cconvar* blue  = g_convars[ HASH_BT( "cl_crosshaircolor_b" ) ];

	out = c_color( red ? red->get_int( ) : 50, green ? green->get_int( ) : 250, blue ? blue->get_int( ) : 50 );
	return true;
}

static float unscoped_inaccuracy( c_base_entity* weapon, c_weapon_data* data )
{
	float accuracy = weapon->get_accuracy_penalty( );

	const float max_speed   = data->m_max_speed[ 0 ] > 0.f ? data->m_max_speed[ 0 ] : 250.f;
	const c_vector velocity = g_ctx.m_local->get_velocity( );

	const float walk = max_speed * 0.34f, run = max_speed * 0.95f;

	float move_scale = run > walk ? ( velocity.length_2d( ) - walk ) / ( run - walk ) : 0.f;
	move_scale       = std::clamp( move_scale, 0.f, 1.f );

	if ( move_scale > 0.f )
		accuracy += std::pow( move_scale, 0.25f ) * data->m_inaccuracy_move[ 0 ];

	if ( !( g_ctx.m_local->get_flags( ) & fl_onground ) ) {
		const float jump = data->m_inaccuracy_jump[ 0 ];

		const float sqrt_jump_speed = std::sqrt( 301.993378f );
		const float sqrt_vertical   = std::sqrt( std::abs( velocity.m_z ) );

		const float air = ( sqrt_vertical - sqrt_jump_speed * 0.25f ) / ( sqrt_jump_speed * 0.75f ) * jump;
		accuracy += std::clamp( air, 0.f, 2.f * jump );
	}

	return std::min( accuracy, 1.f );
}

/* CWeaponCSBase::WantReticleShown. -1 = dead / nothing held = hide */
static bool game_wants_reticle( const int weapon_type )
{
	if ( weapon_type < 0 || weapon_type == k_weapontype_sniper || weapon_type == k_weapontype_c4 || weapon_type == k_weapontype_fists
	     || weapon_type == k_weapontype_tablet )
		return false;

	if ( weapon_type == k_weapontype_knife ) {
		c_cconvar* knife_show = g_convars[ HASH_BT( "weapon_reticle_knife_show" ) ];
		return knife_show && knife_show->get_int( ) != 0;
	}

	return true;
}

static const char* k_force_xhair = R"js(
try {
    var reticle = $.GetContextPanel().FindChildTraverse('HudReticle');
    if ( reticle ) {
        var xhair = reticle.FindChildTraverse('Crosshair');
        if ( xhair ) {
            xhair.style.visibility = 'visible';
            xhair.RemoveClass('crosshair--hidden');
            xhair.RemoveClass('crosshair--piphidden');
            ${arcclass}
            ${paint}

            var bx_place = function( ids, off ) {
                var d = [ [ 0, -off ], [ 0, off ], [ -off, 0 ], [ off, 0 ] ];
                for ( var i = 0; i < 4; i++ ) {
                    var p = xhair.FindChildTraverse( ids[ i ] );
                    if ( p )
                        p.style.transform = 'translate3d( ' + d[ i ][ 0 ] + 'px, ' + d[ i ][ 1 ] + 'px, 0px )';
                }
            };

            bx_place( [ 'TopPip', 'BottomPip', 'LeftPip', 'RightPip' ], ${pip} );
            bx_place( [ 'TopPipBlack', 'BottomPipBlack', 'LeftPipBlack', 'RightPipBlack' ], ${blackpip} );
            bx_place( [ 'TopArc', 'BottomArc', 'LeftArc', 'RightArc' ], ${arc} );
        }
    }
} catch(e) {}
)js";

static const char* k_force_xhair_paint = R"js(
            for ( var arc of xhair.FindChildrenWithClassTraverse('crosshair__arc') )
                arc.style.backgroundColor = '${paintcolor}';

            for ( var pip of xhair.FindChildrenWithClassTraverse('crosshair__pip--green') ) {
                try { pip.style.washColorFast = '${paintcolor}'; } catch(e) {}
                pip.style.washColor = '${paintcolor}';
            }
)js";

static const char* k_force_xhair_restore = R"js(
try {
    var reticle = $.GetContextPanel().FindChildTraverse('HudReticle');
    if ( reticle ) {
        var xhair = reticle.FindChildTraverse('Crosshair');
        if ( xhair ) {
            try { xhair.style.visibility = ''; } catch(e) { xhair.style.visibility = '${fallbackvis}'; }
            ${hideclass}

            var bx_clear = function( ids ) {
                for ( var i = 0; i < 4; i++ ) {
                    var p = xhair.FindChildTraverse( ids[ i ] );
                    if ( p )
                        p.style.transform = 'none';
                }
            };

            bx_clear( [ 'TopPip', 'BottomPip', 'LeftPip', 'RightPip' ] );
            bx_clear( [ 'TopPipBlack', 'BottomPipBlack', 'LeftPipBlack', 'RightPipBlack' ] );
            bx_clear( [ 'TopArc', 'BottomArc', 'LeftArc', 'RightArc' ] );
        }
    }
} catch(e) {}
)js";

void n_scaleform::impl_t::force_crosshair_panorama( )
{
	static bool applied             = false;
	static float applied_pip        = -1.f;
	static float applied_arc        = -1.f;
	static unsigned int applied_hex = 0;
	static float last_reapply       = 0.f;
	static unsigned int last_weapon = 0;
	static float burst_until        = 0.f;

	static bool inline_dirty = false;

	static bool we_hid = false;

	static int restored_state = -2;

	const bool enabled = GET_VARIABLE( g_variables.m_force_crosshair, bool ) && !GET_VARIABLE( g_variables.m_safe_mode, bool );

	if ( !enabled && !applied && !inline_dirty )
		return;

	c_base_entity* weapon      = nullptr;
	c_weapon_data* weapon_data = nullptr;
	int weapon_type            = -1;

	bool weapon_known = true;

	if ( g_ctx.m_local && g_ctx.m_local->is_alive( ) ) {
		const unsigned int handle = g_ctx.m_local->get_active_weapon_handle( );

		weapon = g_interfaces.m_client_entity_list->get< c_base_entity >( handle );

		weapon_data =
			weapon && g_interfaces.m_weapon_system ? g_interfaces.m_weapon_system->get_weapon_data( weapon->get_item_definition_index( ) ) : nullptr;

		if ( weapon_data )
			weapon_type = weapon_data->m_weapon_type;
		else if ( handle && handle != 0xffffffffu )
			weapon_known = false;

		if ( handle != last_weapon ) {
			last_weapon = handle;

			burst_until = g_interfaces.m_global_vars_base ? g_interfaces.m_global_vars_base->m_real_time + 0.35f : 0.f;
		}
	}

	c_cconvar* style_convar = g_convars[ HASH_BT( "cl_crosshairstyle" ) ];
	const int style         = style_convar ? style_convar->get_int( ) : 0;

	const bool want = enabled && style < 2 && weapon_type == k_weapontype_sniper && !g_ctx.m_local->is_scoped( );

	if ( !want && !applied && !inline_dirty )
		return;

	if ( !m_uiengine ) {
		m_uiengine = g_interfaces.m_panorama->access_ui_engine( );
		if ( !m_uiengine )
			return;
	}

	if ( !m_force_xhair_root || !m_uiengine->is_valid_panel_ptr( m_force_xhair_root ) ) {
		m_force_xhair_root = get_panel( HASH_BT( "CSGOHud" ) );
		applied            = false;

		inline_dirty   = false;
		we_hid         = false;
		restored_state = -2;

		if ( !m_force_xhair_root || !want )
			return;
	}

	if ( !want ) {
		if ( !weapon_known )
			return;

		const bool game_shows = style < 2 && game_wants_reticle( weapon_type );

		if ( !applied && ( game_shows ? 1 : 0 ) == restored_state )
			return;

		applied        = false;
		restored_state = game_shows ? 1 : 0;

		inline_dirty = true;

		std::string js = k_force_xhair_restore;

		replace_str( js, "${hideclass}",
		             game_shows ? ( we_hid ? "xhair.RemoveClass('crosshair--hidden');" : "" ) : "xhair.AddClass('crosshair--hidden');" );
		replace_str( js, "${fallbackvis}", game_shows ? "visible" : "collapse" );

		run_on( m_force_xhair_root, js.c_str( ) );

		we_hid = !game_shows;

		return;
	}

	// real screen shape, never g_ctx (see hud_aspect)
	float screen_height = static_cast< float >( g_resolution_spoof.m_screen_height );
	if ( screen_height <= 0.f )
		screen_height = g_ctx.m_height;

	if ( screen_height <= 0.f )
		return;

	float pip_base = 0.f;

	if ( style == 1 ) {
		c_cconvar* fixed_gap = g_convars[ HASH_BT( "cl_fixedcrosshairgap" ) ];
		pip_base             = fixed_gap ? fixed_gap->get_float( ) : 3.f;
	} else {
		const int flags = g_ctx.m_local->get_flags( );

		int gap_units = 10;
		if ( !( flags & fl_onground ) )
			gap_units = 30;
		else if ( flags & fl_ducking )
			gap_units = 3;
		else if ( g_ctx.m_local->get_velocity( ).length( ) > 100.f )
			gap_units = 20;

		pip_base = static_cast< float >( static_cast< int >( screen_height ) * gap_units / 768 );
	}

	float arc         = 0.f;
	float inaccuracy  = 0.f;
	float spread_base = 0.f;

	if ( style == 0 && weapon && weapon_data ) {
		inaccuracy  = unscoped_inaccuracy( weapon, weapon_data );
		spread_base = weapon_data->m_spread[ 0 ];

		arc = std::floor( ( inaccuracy + spread_base ) * 320.f * screen_height / 480.f + 0.5f ) + k_arc_default_offset;
	}

	/* unscoped sniper arcs land past the screen edge (`overflow: noclip` = invisible draw): hide them.
	   also guards a junk inaccuracy. */
	const bool arcs_visible = style == 0 && arc <= screen_height;

	if ( !arcs_visible )
		arc = 0.f;

	const float pip = pip_base + k_pip_default_offset;
	const float now = g_interfaces.m_global_vars_base ? g_interfaces.m_global_vars_base->m_real_time : 0.f;

	c_color paint_color;
	const bool paint = !GET_VARIABLE( g_variables.m_panorama_crosshair_color, bool ) && crosshair_color_needs_paint( paint_color );
	const unsigned int paint_hex = paint ? paint_color.get_u32( ) : 0;

	const bool bursting = now < burst_until;

	/* log on decision-shape change, never per frame */
	static int logged_state = -1;
	static float last_log   = 0.f;

	const int state = ( arcs_visible ? 1 : 0 ) | ( paint ? 2 : 0 ) | ( style << 2 ) | ( ( weapon_type + 1 ) << 6 );

	if ( state != logged_state || ( !arcs_visible && weapon_data && now - last_log > 5.f ) ) {
		logged_state = state;
		last_log     = now;
		g_console.print( std::format( "[force xhair] type {} style {} pip {:.1f} arc {:.1f} arcs {} paint {} | inacc {:.4f} spread {:.4f} penalty {:.4f}",
		                              weapon_type, style, pip, arc, arcs_visible, paint, inaccuracy, spread_base,
		                              weapon ? weapon->get_accuracy_penalty( ) : -1.f )
		                     .c_str( ) );
	}

	if ( applied && pip == applied_pip && arc == applied_arc && paint_hex == applied_hex && now - last_reapply < 2.f && !bursting )
		return;

	last_reapply = now;

	char pip_text[ 32 ]{ }, black_pip_text[ 32 ]{ }, arc_text[ 32 ]{ };
	sprintf_s( pip_text, "%.2f", pip );
	sprintf_s( black_pip_text, "%.2f", pip_base + k_black_pip_default_offset );
	sprintf_s( arc_text, "%.2f", arc );

	std::string paint_js;
	if ( paint ) {
		paint_js = k_force_xhair_paint;
		replace_str( paint_js, "${paintcolor}", panorama_hex( paint_color ) );
	}

	std::string js = k_force_xhair;
	replace_str( js, "${arcclass}", arcs_visible ? "xhair.RemoveClass('crosshair--archidden');" : "xhair.AddClass('crosshair--archidden');" );
	replace_str( js, "${paint}", paint_js );
	replace_str( js, "${pip}", pip_text );
	replace_str( js, "${blackpip}", black_pip_text );
	replace_str( js, "${arc}", arc_text );

	run_on( m_force_xhair_root, js.c_str( ) );

	applied     = true;
	applied_pip = pip;
	applied_arc = arc;
	applied_hex = paint_hex;

	inline_dirty   = true;
	we_hid         = false;
	restored_state = -2;
}

static const c_color k_stock_crosshair_dot_wash = c_color( 0x82, 0xb1, 0x16 );

static const c_color k_stock_hud_colors[ 12 ] = { c_color( 0xe8, 0xe8, 0xe8 ), c_color( 0xff, 0xff, 0xff ), c_color( 0x96, 0xc8, 0xff ),
	                                              c_color( 0x35, 0x6e, 0xff ), c_color( 0xc8, 0x64, 0xff ), c_color( 0xff, 0x29, 0x24 ),
	                                              c_color( 0xff, 0x71, 0x24 ), c_color( 0xff, 0xf7, 0x24 ), c_color( 0x3e, 0xff, 0x24 ),
	                                              c_color( 0x24, 0xff, 0x90 ), c_color( 0xff, 0x79, 0x99 ), c_color( 0xd5, 0xe2, 0x86 ) };

static c_color hsv_transform( const c_color& input, const float saturation_scale, const float value_scale )
{
	const float r = input[ 0 ] / 255.f, g = input[ 1 ] / 255.f, b = input[ 2 ] / 255.f;

	const float high = std::max( r, std::max( g, b ) );
	const float low  = std::min( r, std::min( g, b ) );
	const float span = high - low;

	float hue = 0.f;
	if ( span > 0.f ) {
		if ( high == r )
			hue = 60.f * fmodf( ( g - b ) / span, 6.f );
		else if ( high == g )
			hue = 60.f * ( ( b - r ) / span + 2.f );
		else
			hue = 60.f * ( ( r - g ) / span + 4.f );
	}

	if ( hue < 0.f )
		hue += 360.f;

	const float saturation = high > 0.f ? std::clamp( ( span / high ) * saturation_scale, 0.f, 1.f ) : 0.f;
	const float value      = std::clamp( high * value_scale, 0.f, 1.f );

	return c_color::from_hsb( hue / 360.f, saturation, value, input[ 3 ] / 255.f );
}

static const char* k_xhair_color = R"js(
try {
    var reticle = $.GetContextPanel().FindChildTraverse('HudReticle');
    if ( reticle ) {
        var xhair = reticle.FindChildTraverse('Crosshair');
        if ( xhair ) {
            for ( var arc of xhair.FindChildrenWithClassTraverse('crosshair__arc') )
                arc.style.backgroundColor = '${color}';

            for ( var dot of xhair.FindChildrenWithClassTraverse('crosshair__dot') ) {
                dot.style.backgroundColor = '${color}';
                try { dot.style.washColorFast = '${dotwash}'; } catch(e) {}
                dot.style.washColor = '${dotwash}';
            }

            for ( var pip of xhair.FindChildrenWithClassTraverse('crosshair__pip--green') ) {
                try { pip.style.washColorFast = '${color}'; } catch(e) {}
                pip.style.washColor = '${color}';
            }
        }
    }
} catch(e) {}
)js";

static const char* k_hud_color = R"js(
// iife: a top-level var is a CSGOHud global, every run overwrote gen -> no old loop ever stopped
(function() { try {
    var cp = $.GetContextPanel();
    var col = ${color}, bg = ${bgcolor}, blend = ${dashblend};

    // one try per write: a rejected value aborts the rest of the script
    var paint = function( cls, prop, value ) {
        for ( var p of cp.FindChildrenWithClassTraverse( cls ) )
            try { p.style[ prop ] = value; } catch(e) {}
    };

    var wash = function( cls, value ) {
        for ( var p of cp.FindChildrenWithClassTraverse( cls ) ) {
            try { p.style.washColorFast = value; } catch(e) {}
            try { p.style.washColor = value; } catch(e) {}
        }
    };

    paint( 'cl-hud-color', 'color', col );
    paint( 'cl-hud-background-color', 'backgroundColor', col );
    wash( 'cl-hud-wash-color', col );
    wash( 'WeaponPanel-apply-hud-color', col );
    wash( 'WeaponPanel-apply-hud-color-bg', bg );

    /* radar location text ("Bombsite A") is a hard #d3e798 in hudradar.css with NO hud-colour
       class, so it never moved. the pack's base js also washes it SRGBAdditive, which ADDS the
       text onto the world behind the plate -> over a bright wall any hex saturates to white, so
       the colour never read. normal blend while we own it, pack's additive back when off. */
    var dash = cp.FindChildTraverse('DashboardLabel');
    if ( dash ) {
        try { dash.style.color = col; } catch(e) {}
        try { dash.style.S2MixBlendMode = blend; } catch(e) {}
    }

    // ammo pips have no hud-colour class at all -> k_ammo_pip_color paints them, always-on

    /* moi hudhealtharmor.css washes #HealthBar / #ArmorBar by ID (no class) and greys both with
       saturation 0.001. own 50ms loop: the red states (on-damage 1s flash, critical <= 20%) are
       class flips a 1s heartbeat misses, and red x our wash = black. stock bars untouched, and
       the cleanup runs when moi goes off too - id panels keep inline style across a reload */
    BxHudCol = col; BxHudMoi = ${moi}; BxBarsKey = '';

    if ( typeof BxBarsDirty === 'undefined' ) BxBarsDirty = false;

    // every run starts a fresh loop, older ones see the bump and stop
    BxBarsGen = ( typeof BxBarsGen === 'number' ? BxBarsGen : 0 ) + 1;
    var gen = BxBarsGen;

    var bars = function() {
        if ( BxBarsGen !== gen ) return;

        var on = false;
        try {
            on = BxHudCol !== null && BxHudMoi;

            var ha = $.GetContextPanel().FindChildTraverse( 'HudHealthArmor' );
            var hb = ha ? ha.FindChildTraverse( 'HealthBar' ) : null;
            var ab = ha ? ha.FindChildTraverse( 'ArmorBar' ) : null;

            var set = function( p, w, s, keep_wash ) {
                if ( !p ) return;
                if ( !keep_wash ) {
                    try { p.style.washColorFast = w; } catch(e) {}
                    try { p.style.washColor = w; } catch(e) {}
                }
                try { p.style.saturation = s; } catch(e) {}
            };

            if ( on && ha ) {
                var red = ha.BHasClass( 'hud-HA--on-damage' ) || ha.BHasClass( 'hud-HA--critical' );
                var key = ( red ? 'r' : 'c' ) + BxHudCol;
                if ( key !== BxBarsKey ) {
                    set( hb, red ? null : BxHudCol, red ? null : '1.0', false );
                    set( ab, BxHudCol, '1.0', false );
                    BxBarsKey = key;
                    BxBarsDirty = true;
                }
            } else if ( !on && BxBarsDirty ) {
                set( hb, null, null, false );
                // stock ArmorBar carries cl-hud-wash-color: paint() owns that wash
                set( ab, null, null, ab && ab.BHasClass( 'cl-hud-wash-color' ) );
                BxBarsDirty = false;
            }
        } catch(e) {}

        if ( on ) $.Schedule( 0.05, bars );
    };

    if ( ( col !== null && BxHudMoi ) || BxBarsDirty )
        bars();
} catch(e) {} })();
)js";

static const char* k_ammo_pip_color = R"js(
try {
    var cp = $.GetContextPanel();

    for ( var name of [ 'AmmoAnim__BulletIcon', 'AmmoAnim__ShellIcon' ] ) {
        for ( var pip of cp.FindChildrenWithClassTraverse(name) ) {
            if ( pip.BHasClass(name + '--Red') ) {
                try { pip.style.washColorFast = null; } catch(e) {}
                try { pip.style.washColor = null; } catch(e) {}
                continue;
            }

            try { pip.style.washColorFast = '${color}'; } catch(e) {}
            try { pip.style.washColor = '${color}'; } catch(e) {}
        }
    }
} catch(e) {}
)js";

static const char* k_weapon_icon_color = R"js(
try {
    var cp = $.GetContextPanel();
    var custom = '${iconcolor}';

    for ( var icon of cp.FindChildrenWithClassTraverse('weapon-selection-item-icon-main') ) {
        /* override off -> wipe our wash and hand the icon back to the css. TeamSCALEFORM's
           weapon_select.js never colours an icon, and the c4/defuser rows have their own
           stock rule — washing them with cl_hud_color's hex tinted the bomb icon wrong. */
        if ( !custom ) {
            try { icon.style.washColorFast = null; } catch(e) {}
            try { icon.style.washColor = null; } catch(e) {}
            try { icon.style.opacity = null; } catch(e) {}
            continue;
        }

        var row = icon, selected = false;

        // icon -> weapon-selection-item-icon -> weapon-selection-item, with slack
        for ( var up = 0; up < 6 && row; up++ ) {
            if ( row.BHasClass('weapon-selection-item--selected') ) { selected = true; break; }
            row = row.GetParent();
        }

        var wash = selected ? '#FFFFFFFF' : '${iconcolor}';

        try { icon.style.washColorFast = wash; } catch(e) {}
        icon.style.washColor = wash;
        icon.style.opacity = selected ? '1.0' : '0.92';
    }
} catch(e) {}
)js";

static bool s_icon_wash_written = false;

static void apply_weapon_icon_color( )
{
	if ( !g_scaleform.m_uiengine )
		return;

	if ( g_scaleform.m_icon_wash.empty( ) && !s_icon_wash_written )
		return;

	s_icon_wash_written = !g_scaleform.m_icon_wash.empty( );

	c_uipanel* root = g_scaleform.m_hud_color_root;
	if ( !root || !g_scaleform.m_uiengine->is_valid_panel_ptr( root ) )
		root = g_scaleform.m_hud_panel;

	std::string js = k_weapon_icon_color;
	replace_str( js, "${iconcolor}", g_scaleform.m_icon_wash );

	run_on( root, js.c_str( ) );
}

void n_scaleform::impl_t::crosshair_color( )
{
	static bool applied            = false;
	static unsigned int applied_to = 0;
	static float last_reapply      = 0.f;

	const bool enabled = GET_VARIABLE( g_variables.m_panorama_crosshair_color, bool );

	// off and never applied - the common case, costs one branch
	if ( !enabled && !applied )
		return;

	if ( !m_uiengine ) {
		m_uiengine = g_interfaces.m_panorama->access_ui_engine( );
		if ( !m_uiengine )
			return;
	}

	if ( !m_xhair_color_root || !m_uiengine->is_valid_panel_ptr( m_xhair_color_root ) ) {
		m_xhair_color_root = get_panel( HASH_BT( "CSGOHud" ) );
		applied            = false;

		if ( !m_xhair_color_root )
			return;
	}

	const unsigned int wanted = GET_VARIABLE( g_variables.m_panorama_crosshair_color_value, c_color ).get_u32( );
	const float now           = g_interfaces.m_global_vars_base ? g_interfaces.m_global_vars_base->m_real_time : 0.f;

	if ( enabled && applied && applied_to == wanted && now - last_reapply < 2.f )
		return;

	last_reapply = now;

	std::string js = k_xhair_color;

	if ( enabled ) {
		const std::string hex = panorama_hex( GET_VARIABLE( g_variables.m_panorama_crosshair_color_value, c_color ) );

		replace_str( js, "${color}", hex );
		replace_str( js, "${dotwash}", "#FFFFFFFF" );
	} else {
		c_color stock;
		crosshair_color_needs_paint( stock );

		replace_str( js, "${color}", panorama_hex( stock ) );
		replace_str( js, "${dotwash}", panorama_hex( k_stock_crosshair_dot_wash ) );
	}

	run_on( m_xhair_color_root, js.c_str( ) );

	applied    = enabled;
	applied_to = wanted;
}

void n_scaleform::impl_t::hud_color( )
{
	static bool applied            = false;
	static unsigned int applied_to = 0;
	static float last_reapply      = 0.f;
	static bool applied_moi        = false;

	const bool enabled = GET_VARIABLE( g_variables.m_panorama_hud_color, bool );
	const bool moi     = g_moi_hud.serving( );

	if ( !enabled && !applied )
		return;

	if ( !m_uiengine ) {
		m_uiengine = g_interfaces.m_panorama->access_ui_engine( );
		if ( !m_uiengine )
			return;
	}

	if ( !m_hud_color_root || !m_uiengine->is_valid_panel_ptr( m_hud_color_root ) ) {
		m_hud_color_root = get_panel( HASH_BT( "CSGOHud" ) );
		applied          = false;

		if ( !m_hud_color_root )
			return;
	}

	const unsigned int wanted = GET_VARIABLE( g_variables.m_panorama_hud_color_value, c_color ).get_u32( );
	const float now           = g_interfaces.m_global_vars_base ? g_interfaces.m_global_vars_base->m_real_time : 0.f;

	if ( enabled && applied && applied_to == wanted && applied_moi == moi && now - last_reapply < 1.f )
		return;

	last_reapply = now;

	const c_color chosen = GET_VARIABLE( g_variables.m_panorama_hud_color_value, c_color );

	/* off = null every write, never the stock hex: inline outranks the state rules (--selected icon white,
	   hud-HA--critical red), so a painted stock value froze them until the panel died */
	std::string js = k_hud_color;
	replace_str( js, "${color}", enabled ? "'" + panorama_hex( chosen ) + "'" : "null" );
	replace_str( js, "${bgcolor}", enabled ? "'" + panorama_hex( hsv_transform( chosen, 0.85f, 0.75f ) ) + "'" : "null" );
	const bool classic = GET_VARIABLE( g_variables.m_scaleform, bool ) && GET_VARIABLE( g_variables.m_scaleform_style, int ) != 0;
	replace_str( js, "${dashblend}", enabled ? "'normal'" : classic ? "'SRGBAdditive'" : "null" );
	replace_str( js, "${moi}", moi ? "true" : "false" );

	run_on( m_hud_color_root, js.c_str( ) );

	m_icon_wash = enabled ? panorama_hex( hsv_transform( chosen, 0.96f, 0.18f ) ) : std::string( );
	apply_weapon_icon_color( );

	applied     = enabled;
	applied_to  = wanted;
	applied_moi = moi;
}

void n_scaleform::impl_t::ammo_pip_color( )
{
	static unsigned int applied_to = 0;
	static float last_reapply      = 0.f;

	if ( !m_uiengine ) {
		m_uiengine = g_interfaces.m_panorama->access_ui_engine( );
		if ( !m_uiengine )
			return;
	}

	if ( !m_pip_color_root || !m_uiengine->is_valid_panel_ptr( m_pip_color_root ) ) {
		m_pip_color_root = get_panel( HASH_BT( "CSGOHud" ) );
		applied_to       = 0;

		if ( !m_pip_color_root )
			return;
	}

	c_color chosen;
	if ( GET_VARIABLE( g_variables.m_panorama_hud_color, bool ) ) {
		chosen = GET_VARIABLE( g_variables.m_panorama_hud_color_value, c_color );
	} else {
		c_cconvar* hud_color_convar = g_convars[ HASH_BT( "cl_hud_color" ) ];

		const int value = hud_color_convar ? hud_color_convar->get_int( ) : 0;
		chosen          = k_stock_hud_colors[ ( value >= 0 && value <= 11 ) ? value : 0 ];
	}

	const unsigned int wanted = chosen.get_u32( );
	const float now           = g_interfaces.m_global_vars_base ? g_interfaces.m_global_vars_base->m_real_time : 0.f;

	if ( applied_to == wanted && now - last_reapply < 1.f )
		return;

	last_reapply = now;

	std::string js = k_ammo_pip_color;
	replace_str( js, "${color}", panorama_hex( chosen ) );

	run_on( m_pip_color_root, js.c_str( ) );

	applied_to = wanted;
}

void n_scaleform::impl_t::weapon_icon_color( )
{
	static unsigned int last_weapon = 0;
	static float burst_until        = 0.f;
	static float last_paint         = 0.f;

	if ( m_icon_wash.empty( ) || !g_ctx.m_local )
		return;

	const unsigned int handle = g_ctx.m_local->get_active_weapon_handle( );
	const float now           = g_interfaces.m_global_vars_base ? g_interfaces.m_global_vars_base->m_real_time : 0.f;

	if ( handle != last_weapon ) {
		last_weapon = handle;
		burst_until = now + 0.5f;
	}

	if ( now > burst_until )
		return;

	if ( now - last_paint < 0.03f )
		return;

	last_paint = now;
	apply_weapon_icon_color( );
}

/* aventum "square radar": round radar mask corners 50% -> 5% */
static const char* k_square_radar = R"js(
(function () {
    try {
        var radar = $.GetContextPanel().FindChildTraverse('HudRadar');
        if (!radar)
            return;
        var r = '${radius}';
        var ids = ['Radar__Round', 'Radar__Round--Inner', 'Radar__Round--InnerTransform', 'Radar__Round--Overlay'];
        for (var i = 0; i < ids.length; ++i) {
            var p = radar.FindChildTraverse(ids[i]);
            if (p)
                p.style.borderRadius = r;
        }
        var borders = radar.FindChildrenWithClassTraverse('Radar__Round--BorderClass');
        for (var j = 0; j < borders.length; ++j)
            borders[j].style.borderRadius = r;
    } catch (e) {}
})();
)js";

void n_scaleform::impl_t::square_radar( )
{
	static bool applied       = false;
	static float last_reapply = 0.f;

	const bool classic = GET_VARIABLE( g_variables.m_scaleform, bool ) && GET_VARIABLE( g_variables.m_scaleform_style, int ) != 0;
	const bool enabled = GET_VARIABLE( g_variables.m_square_radar, bool ) && !classic;

	if ( !enabled && !applied )
		return;

	if ( !m_uiengine ) {
		m_uiengine = g_interfaces.m_panorama->access_ui_engine( );
		if ( !m_uiengine )
			return;
	}

	if ( !m_radar_root || !m_uiengine->is_valid_panel_ptr( m_radar_root ) ) {
		m_radar_root = get_panel( HASH_BT( "CSGOHud" ) );
		applied      = false;

		if ( !m_radar_root || !enabled )
			return;
	}

	const float now = g_interfaces.m_global_vars_base ? g_interfaces.m_global_vars_base->m_real_time : 0.f;

	if ( enabled && applied && now - last_reapply < 2.f )
		return;

	last_reapply = now;

	std::string js = k_square_radar;
	replace_str( js, "${radius}", enabled ? "5% / 5%" : "50% / 50%" );
	run_on( m_radar_root, js.c_str( ) );

	applied = enabled;
}


static void scaleform_teamcount_avatar( )
{
	run( teamcount_avatar );
}

static void scaleform_weapon_selection( )
{
	run( weapon_select );

	apply_weapon_icon_color( );
}

static void scaleform_spec( )
{
	run( spectating );
}

static void scaleform_death( )
{
	run( deathnotices );
}

static void scaleform_winpanel( int team )
{
	std::string js = winpanel;
	replace_str( js, IS_CT, team == 3 ? "true" : "false" );
	replace_str( js, IS_T, team == 2 ? "true" : "false" );
	replace_str( js, PENDING_MVP, g_scaleform.m_pending_mvp ? "true" : "false" );
	replace_str( js, IS_2013, "false" );
	run( js.c_str( ) );

	g_scaleform.m_pending_mvp = false;
}


void n_scaleform::impl_t::modify_all( )
{
	m_hud_panel = get_panel( HASH_BT( "CSGOHud" ) );
	if ( !m_hud_panel )
		return;

	auto weapon_panel = m_hud_panel->find_child_traverse( "HudWeaponPanel" );
	if ( !weapon_panel )
		return;

	m_weap_pan_bg = weapon_panel->find_child_traverse( "WeaponPanelBottomBG" );
	if ( !m_weap_pan_bg )
		return;

	m_weap_sel = m_hud_panel->find_child_traverse( "HudWeaponSelection" );
	if ( !m_weap_sel )
		return;

	if ( m_saved_showloadout < 0 )
		m_saved_showloadout = g_convars.int_or( HASH_BT( "cl_showloadout" ), 1 );
	g_convars.set_if_present( HASH_BT( "cl_showloadout" ), 1.f );

	run( base );
	run( alerts );

	scaleform_teamcount_avatar( );
	scaleform_weapon_selection( );
	scaleform_spec( );

	m_inited = true;
}

void n_scaleform::impl_t::tick( )
{
	if ( m_weap_pan_bg )
		m_weap_pan_bg->set_visible( true );

	if ( auto* loadout = g_convars[ HASH_BT( "cl_showloadout" ) ]; loadout && loadout->get_int( ) != 1 )
		loadout->set_value( 1 );

	int col = g_convars.int_or( HASH_BT( "cl_hud_color" ), 0 );
	if ( col != m_old_color ) {
		std::string js = color;
		replace_str( js, RADAR_COLOR, get_color( col, kRadarColor ) );
		replace_str( js, DASHBOARD_LABEL_COLOR, get_color( col, kDashColor ) );
		run( js.c_str( ) );
		m_old_color = col;
	}

	int hs = g_convars.int_or( HASH_BT( "cl_hud_healthammo_style" ), 0 );
	bool healthammo_rerun = hs != m_old_healthammo_style;
	if ( healthammo_rerun ) {
		std::string js = healthammo;
		replace_str( js, HEALTHAMMO_STYLE, hs == 0 ? "true" : "false" );
		run( js.c_str( ) );
		m_old_healthammo_style = hs;
	}

	float a = g_convars.float_or( HASH_BT( "cl_hud_background_alpha" ), 1.f );
	if ( a > 1.f )
		a = 1.f;
	if ( a != m_old_alpha || healthammo_rerun ) {
		std::string js = alpha;
		char buf[ 16 ];
		sprintf_s( buf, "%.2f", a );
		replace_str( js, ALPHA, buf );
		run( js.c_str( ) );
		m_old_alpha = a;
	}

	int bz = ( g_ctx.m_local && g_ctx.m_local->in_buy_zone( ) ) ? 1 : 0;
	if ( bz != m_old_in_buyzone ) {
		std::string js = buyzone;
		replace_str( js, BUYZONE, bz == 1 ? "34px" : "0px" );
		run( js.c_str( ) );
		m_old_in_buyzone = bz;
	}

	if ( g_ctx.m_local ) {
		const auto weapon_handle = g_ctx.m_local->get_active_weapon_handle( );
		if ( weapon_handle ) {
			const auto active_weapon =
				reinterpret_cast< c_base_entity* >( g_interfaces.m_client_entity_list->get_client_entity_from_handle( weapon_handle ) );
			if ( active_weapon ) {
				const short def = active_weapon->get_item_definition_index( );
				if ( def != m_old_weapon_def ) {
					scaleform_weapon_selection( );
					m_old_weapon_def = def;
				}
			}
		}
	}

	if ( m_should_update_teamcount_avatar ) {
		scaleform_teamcount_avatar( );
		scaleform_weapon_selection( );
		m_should_update_teamcount_avatar = false;
	}

	if ( m_should_update_winpanel ) {
		scaleform_winpanel( m_winpanel_team );
		m_should_update_winpanel = false;
	}

	if ( m_should_update_deathnotices ) {
		scaleform_death( );
		m_should_update_deathnotices = false;
	}
}

void n_scaleform::impl_t::classic_off( )
{
	if ( !m_uiengine )
		m_uiengine = g_interfaces.m_panorama->access_ui_engine( );

	if ( m_uiengine && ( !m_hud_panel || !m_uiengine->is_valid_panel_ptr( m_hud_panel ) ) )
		m_hud_panel = get_panel( HASH_BT( "CSGOHud" ) );

	if ( !m_uiengine || !m_hud_panel )
		return;

	botox_dbg_log( "SF: classic off -> %s", GET_VARIABLE( g_variables.m_scaleform, bool ) ? "moi" : "stock" );

	run_on( m_hud_panel, k_classic_off );

	g_moi_hud.reload( m_hud_panel );

	m_classic_applied = false;
	m_inited          = false;
	m_weap_pan_bg     = nullptr;
	m_weap_sel        = nullptr;
}

void n_scaleform::impl_t::restore_showloadout( )
{
	if ( m_saved_showloadout < 0 )
		return;

	g_convars.set_if_present( HASH_BT( "cl_showloadout" ), static_cast< float >( m_saved_showloadout ) );
	botox_dbg_log( "SF: cl_showloadout back to %d", m_saved_showloadout );
	m_saved_showloadout = -1;
}

void n_scaleform::impl_t::on_level_init( )
{
	m_inited          = false;
	m_classic_applied = false;
	m_hud_panel       = nullptr;
	m_weap_sel        = nullptr;
	m_weap_pan_bg     = nullptr;

	m_removal_root = nullptr;

	m_xhair_color_root = nullptr;
	m_hud_color_root   = nullptr;
	m_pip_color_root   = nullptr;

	m_force_xhair_root = nullptr;

	m_old_color            = -1;
	m_old_alpha            = -1.f;
	m_old_healthammo_style = -1;
	m_old_in_buyzone       = -1;
	m_old_weapon_def       = -1;
	m_pending_mvp          = false;

	g_moi_hud.on_level_init( );
}

static const char* k_keep_killfeed = R"js(
try {
    var n = $.GetContextPanel().FindChildrenWithClassTraverse( 'DeathNotice_Killer' );
    for ( var i = 0; i < n.length; ++i ) {
        if ( n[ i ].BHasClass( 'FadeOut' ) || n[ i ].BHasClass( 'Available' ) )
            continue;
        n[ i ].SetAttributeString( 'SpawnTime', '${t}' );
    }
} catch(e) {}
)js";

void n_scaleform::impl_t::keep_killfeed( )
{
	static float next_refresh = 0.f;

	const bool reset = m_killfeed_round_reset;
	m_killfeed_round_reset = false;

	if ( !GET_VARIABLE( g_variables.m_keep_killfeed, bool ) || !g_interfaces.m_global_vars_base )
		return;

	const float now = g_interfaces.m_global_vars_base->m_real_time;

	// 1s refresh against a 7.5s lifetime; a reset tick must not be refreshed straight back
	if ( !reset && now < next_refresh && now > next_refresh - 2.f )
		return;

	next_refresh = now + 1.f;

	if ( !m_uiengine ) {
		m_uiengine = g_interfaces.m_panorama->access_ui_engine( );
		if ( !m_uiengine )
			return;
	}

	if ( !m_killfeed_root || !m_uiengine->is_valid_panel_ptr( m_killfeed_root ) ) {
		m_killfeed_root = get_panel( HASH_BT( "CSGOHud" ) );
		if ( !m_killfeed_root )
			return;
	}

	char time_text[ 32 ]{ };
	sprintf_s( time_text, "%.3f", reset ? 0.f : g_interfaces.m_global_vars_base->m_current_time );

	std::string js = k_keep_killfeed;
	replace_str( js, "${t}", time_text );

	run_on( m_killfeed_root, js.c_str( ) );
}

void n_scaleform::impl_t::on_createmove( )
{
	/* before the scaleform gate: the dump must work with scaleform off, game thread only */
	if ( m_should_dump_panels ) {
		dump_panel_tree( );
		m_should_dump_panels = false;
	}

	if ( m_should_force_update ) {
		m_should_force_update = false;

		m_removal_root     = nullptr;
		m_aspect_root      = nullptr;
		m_xhair_root       = nullptr;
		m_xhair_color_root = nullptr;
		m_hud_color_root   = nullptr;
		m_pip_color_root   = nullptr;
		m_radar_root       = nullptr;

		m_inited               = false;
		m_old_color            = -1;
		m_old_alpha            = -1.f;
		m_old_healthammo_style = -1;
		m_old_in_buyzone       = -1;
		m_old_weapon_def       = -1;
	}

	hud_removal( );

	hud_aspect( );
	crosshair_aspect( );

	force_crosshair_panorama( );

	crosshair_color( );
	hud_color( );
	ammo_pip_color( );
	weapon_icon_color( );

	keep_killfeed( );

	square_radar( );

	const bool classic = GET_VARIABLE( g_variables.m_scaleform, bool ) && GET_VARIABLE( g_variables.m_scaleform_style, int ) != 0;

	if ( m_should_force_reload ) {
		m_should_force_reload = false;

		if ( !m_uiengine )
			m_uiengine = g_interfaces.m_panorama->access_ui_engine( );

		botox_dbg_log( "SF: force hud update, classic applied=%d moi=%d", ( int )m_classic_applied, ( int )g_moi_hud.serving( ) );

		if ( m_classic_applied ) {
			classic_off( );
			/* DeleteAsync'd pack panels are still findable until panorama's next frame: a reinstall this tick would
			   see them in exist( ) and never recreate them. next tick reinstalls */
			return;
		}

		if ( c_uipanel* root = m_uiengine ? get_panel( HASH_BT( "CSGOHud" ) ) : nullptr ) {
			run_on( root, k_classic_off );
			g_moi_hud.reload( root );
		}
	}

	if ( m_classic_applied && !classic )
		classic_off( );

	if ( !classic )
		restore_showloadout( );

	{
		const bool moi = GET_VARIABLE( g_variables.m_scaleform, bool ) && GET_VARIABLE( g_variables.m_scaleform_style, int ) == 0;

		if ( moi || g_moi_hud.wants_root( ) ) {
			if ( !m_uiengine )
				m_uiengine = g_interfaces.m_panorama->access_ui_engine( );

			if ( m_uiengine && ( !m_moi_root || !m_uiengine->is_valid_panel_ptr( m_moi_root ) ) )
				m_moi_root = get_panel( HASH_BT( "CSGOHud" ) );
		}

		g_moi_hud.update( m_moi_root, moi );
	}

	{
		const int icon_src = !GET_VARIABLE( g_variables.m_scaleform, bool ) ? 0 : classic ? 1 : g_moi_hud.serving( ) ? 2 : 0;
		if ( icon_src != m_icon_src ) {
			m_icon_src = icon_src;
			g_moi_hud.reload_icons( );
		}
	}

	if ( !classic )
		return;

	if ( !m_uiengine ) {
		m_uiengine = g_interfaces.m_panorama->access_ui_engine( );
		if ( !m_uiengine )
			return;
	}

	if ( !m_inited ) {
		modify_all( );
		return;
	}

	tick( );
}


#include "../../../utilities/data/data.h"

constexpr static uint64_t hash_data( const char* data, size_t len )
{
	uint64_t hash = 0x543C730D;

	for ( size_t i = 0; i < len; ++i ) {
		hash ^= data[ i ];
		hash *= 0x1000931;
	}

	return hash;
}

bool n_scaleform::impl_t::get_replacement_icon( const char* name, const uint8_t*& data, size_t& len, int& w, int& h )
{
	uint64_t hash = hash_data( name, strlen( name ) );
	switch ( hash ) {
	case ct_data< hash_data( icon_m4a1_silencer_name, sizeof( icon_m4a1_silencer_name ) - 1 ) >::value:
		data = icon_m4a1_silencer;
		len  = sizeof( icon_m4a1_silencer );
		w    = icon_m4a1_silencer_w;
		h    = icon_m4a1_silencer_h;
		return true;
	case ct_data< hash_data( icon_m4a1_name, sizeof( icon_m4a1_name ) - 1 ) >::value:
		data = icon_m4a1;
		len  = sizeof( icon_m4a1 );
		w    = icon_m4a1_w;
		h    = icon_m4a1_h;
		return true;
	case ct_data< hash_data( icon_knife_tactical_name, sizeof( icon_knife_tactical_name ) - 1 ) >::value:
		data = icon_knife_tactical;
		len  = sizeof( icon_knife_tactical );
		w    = icon_knife_tactical_w;
		h    = icon_knife_tactical_h;
		return true;
	case ct_data< hash_data( icon_knife_t_name, sizeof( icon_knife_t_name ) - 1 ) >::value:
		data = icon_knife_t;
		len  = sizeof( icon_knife_t );
		w    = icon_knife_t_w;
		h    = icon_knife_t_h;
		return true;
	case ct_data< hash_data( icon_knife_survival_bowie_name, sizeof( icon_knife_survival_bowie_name ) - 1 ) >::value:
		data = icon_knife_survival_bowie;
		len  = sizeof( icon_knife_survival_bowie );
		w    = icon_knife_survival_bowie_w;
		h    = icon_knife_survival_bowie_h;
		return true;
	case ct_data< hash_data( icon_knife_push_name, sizeof( icon_knife_push_name ) - 1 ) >::value:
		data = icon_knife_push;
		len  = sizeof( icon_knife_push );
		w    = icon_knife_push_w;
		h    = icon_knife_push_h;
		return true;
	case ct_data< hash_data( icon_knife_m9_bayonet_name, sizeof( icon_knife_m9_bayonet_name ) - 1 ) >::value:
		data = icon_knife_m9_bayonet;
		len  = sizeof( icon_knife_m9_bayonet );
		w    = icon_knife_m9_bayonet_w;
		h    = icon_knife_m9_bayonet_h;
		return true;
	case ct_data< hash_data( icon_knife_karambit_name, sizeof( icon_knife_karambit_name ) - 1 ) >::value:
		data = icon_knife_karambit;
		len  = sizeof( icon_knife_karambit );
		w    = icon_knife_karambit_w;
		h    = icon_knife_karambit_h;
		return true;
	case ct_data< hash_data( icon_knife_gut_name, sizeof( icon_knife_gut_name ) - 1 ) >::value:
		data = icon_knife_gut;
		len  = sizeof( icon_knife_gut );
		w    = icon_knife_gut_w;
		h    = icon_knife_gut_h;
		return true;
	case ct_data< hash_data( icon_knife_flip_name, sizeof( icon_knife_flip_name ) - 1 ) >::value:
		data = icon_knife_flip;
		len  = sizeof( icon_knife_flip );
		w    = icon_knife_flip_w;
		h    = icon_knife_flip_h;
		return true;
	case ct_data< hash_data( icon_knife_name, sizeof( icon_knife_name ) - 1 ) >::value:
		data = icon_knife;
		len  = sizeof( icon_knife );
		w    = icon_knife_w;
		h    = icon_knife_h;
		return true;
	case ct_data< hash_data( icon_knife_butterfly_name, sizeof( icon_knife_butterfly_name ) - 1 ) >::value:
		data = icon_knife_butterfly;
		len  = sizeof( icon_knife_butterfly );
		w    = icon_knife_butterfly_w;
		h    = icon_knife_butterfly_h;
		return true;
	case ct_data< hash_data( icon_bayonet_name, sizeof( icon_bayonet_name ) - 1 ) >::value:
		data = icon_bayonet;
		len  = sizeof( icon_bayonet );
		w    = icon_bayonet_w;
		h    = icon_bayonet_h;
		return true;
	case ct_data< hash_data( icon_incgrenade_name, sizeof( icon_incgrenade_name ) - 1 ) >::value:
		data = icon_incgrenade;
		len  = sizeof( icon_incgrenade );
		w    = icon_incgrenade_w;
		h    = icon_incgrenade_h;
		return true;
	case ct_data< hash_data( icon_hkp2000_name, sizeof( icon_hkp2000_name ) - 1 ) >::value:
		data = icon_hkp2000;
		len  = sizeof( icon_hkp2000 );
		w    = icon_hkp2000_w;
		h    = icon_hkp2000_h;
		return true;
	case ct_data< hash_data( icon_hegrenade_name, sizeof( icon_hegrenade_name ) - 1 ) >::value:
		data = icon_hegrenade;
		len  = sizeof( icon_hegrenade );
		w    = icon_hegrenade_w;
		h    = icon_hegrenade_h;
		return true;
	case ct_data< hash_data( icon_flashbang_name, sizeof( icon_flashbang_name ) - 1 ) >::value:
		data = icon_flashbang;
		len  = sizeof( icon_flashbang );
		w    = icon_flashbang_w;
		h    = icon_flashbang_h;
		return true;
	case ct_data< hash_data( icon_elite_name, sizeof( icon_elite_name ) - 1 ) >::value:
		data = icon_elite;
		len  = sizeof( icon_elite );
		w    = icon_elite_w;
		h    = icon_elite_h;
		return true;
	case ct_data< hash_data( icon_decoy_name, sizeof( icon_decoy_name ) - 1 ) >::value:
		data = icon_decoy;
		len  = sizeof( icon_decoy );
		w    = icon_decoy_w;
		h    = icon_decoy_h;
		return true;
	case ct_data< hash_data( icon_deagle_name, sizeof( icon_deagle_name ) - 1 ) >::value:
		data = icon_deagle;
		len  = sizeof( icon_deagle );
		w    = icon_deagle_w;
		h    = icon_deagle_h;
		return true;
	case ct_data< hash_data( icon_awp_name, sizeof( icon_awp_name ) - 1 ) >::value:
		data = icon_awp;
		len  = sizeof( icon_awp );
		w    = 1;
		h    = 1;
		return true;
	case ct_data< hash_data( icon_ak47_name, sizeof( icon_ak47_name ) - 1 ) >::value:
		data = icon_ak47;
		len  = sizeof( icon_ak47 );
		w    = icon_ak47_w;
		h    = icon_ak47_h;
		return true;
	case ct_data< hash_data( icon_ssg08_name, sizeof( icon_ssg08_name ) - 1 ) >::value:
		data = icon_ssg08;
		len  = sizeof( icon_ssg08 );
		w    = icon_ssg08_w;
		h    = icon_ssg08_h;
		return true;
	case ct_data< hash_data( icon_smokegrenade_name, sizeof( icon_smokegrenade_name ) - 1 ) >::value:
		data = icon_smokegrenade;
		len  = sizeof( icon_smokegrenade );
		w    = icon_smokegrenade_w;
		h    = icon_smokegrenade_h;
		return true;
	case ct_data< hash_data( icon_molotov_name, sizeof( icon_molotov_name ) - 1 ) >::value:
		data = icon_molotov;
		len  = sizeof( icon_molotov );
		w    = icon_molotov_w;
		h    = icon_molotov_h;
		return true;
	case ct_data< hash_data( icon_mag7_name, sizeof( icon_mag7_name ) - 1 ) >::value:
		data = icon_mag7;
		len  = sizeof( icon_mag7 );
		w    = icon_mag7_w;
		h    = icon_mag7_h;
		return true;
	case ct_data< hash_data( icon_m4a1_silencer_off_name, sizeof( icon_m4a1_silencer_off_name ) - 1 ) >::value:
		data = icon_m4a1_silencer_off;
		len  = sizeof( icon_m4a1_silencer_off );
		w    = icon_m4a1_silencer_off_w;
		h    = icon_m4a1_silencer_off_h;
		return true;
	}

	return false;
}
