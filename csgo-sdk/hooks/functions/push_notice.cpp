#include <format>
#include <string>
#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"
#include "../../hacks/chat_hud.h"
#include "../../hacks/misc/chat_extras.h"
#include "../../hacks/misc/scaleform/scaleform.h"
#include "../../hacks/chud_hud/chud_hud.h"
#include "../../hacks/mc_hud/mc_hud.h"
#include "../hooks.h"

static const char* k_tag_eb = R"js(
(function () {
    try {
        var root = $.GetContextPanel();
        var old = root.FindChildrenWithClassTraverse('BotoxEbLine');
        for (var i = 0; i < old.length; ++i)
            old[i].RemoveClass('BotoxEbLine');
        var lines = root.FindChildrenWithClassTraverse('AlertText');
        for (var j = 0; j < lines.length; ++j) {
            var p = lines[j].GetParent();
            if (p && p.BHasClass('AlertHidden') && lines[j].text.indexOf('edgebugged') !== -1) {
                lines[j].AddClass('BotoxEbLine');
                break;
            }
        }
    } catch (e) {}
})();
)js";

static const char* k_restack_eb = R"js(
(function () {
    try {
        var hit = $.GetContextPanel().FindChildrenWithClassTraverse('BotoxEbLine');
        for (var i = 0; i < hit.length; ++i) {
            if (hit[i].text.indexOf('edgebugged') === -1)
                continue;
            hit[i].html = true;
            hit[i].text = '${html}';
        }
    } catch (e) {}
})();
)js";

static void replace_js( std::string& js, const std::string& key, const std::string& value )
{
	std::string escaped;
	for ( const char c : value ) {
		if ( c == '\'' || c == '\\' )
			escaped.push_back( '\\' );
		if ( c != '\n' && c != '\r' )
			escaped.push_back( c );
	}
	if ( const size_t at = js.find( key ); at != std::string::npos )
		js.replace( at, key.length( ), escaped );
}

// CCSGO_HudChat: char* m_pHistoryString; int m_nHistorySizeBytes; buffer new char[10 * 1024]
constexpr int k_history_max = 10 * 1024;

static uint8_t* find_chat_element( )
{
	static const auto site = g_modules[ CLIENT_DLL ].find_pattern( "B9 ? ? ? ? E8 ? ? ? ? 8B 5D 08 85 C0" );
	if ( !site )
		return nullptr;

	const auto hud      = *reinterpret_cast< uint8_t** >( site + 1 );
	const auto elements = *reinterpret_cast< void*** >( hud + 0x1C );
	const int count     = *reinterpret_cast< int* >( hud + 0x28 );
	for ( int i = 0; i < count; ++i ) {
		const char* name = elements[ i ] ? g_virtual.call< const char* >( elements[ i ], 12 ) : nullptr;
		if ( name && !strcmp( name, "CCSGO_HudChat" ) )
			return static_cast< uint8_t* >( elements[ i ] );
	}
	return nullptr;
}

static bool history_ends_with( uint8_t* chat, int off, const char* tail, int tail_len )
{
	__try {
		const char* str = *reinterpret_cast< char** >( chat + off );
		const int size  = *reinterpret_cast< int* >( chat + off + 4 );
		// small ints in the scan fault every time, and each fault burns a crash witness slot
		return reinterpret_cast< std::uintptr_t >( str ) >= 0x10000 && size >= tail_len && size < k_history_max && !str[ size ] && !memcmp( str + size - tail_len, tail, tail_len );
	} __except ( EXCEPTION_EXECUTE_HANDLER ) {
		return false;
	}
}

static int find_history_offset( uint8_t* chat, const char* tail, int tail_len )
{
	for ( int off = 0; off < 0x800; off += 4 )
		if ( history_ends_with( chat, off, tail, tail_len ) )
			return off;
	return -1;
}

static bool patch_history( uint8_t* chat, int off, const char* old_html, int old_len, const char* new_html, int new_len )
{
	__try {
		char* str = *reinterpret_cast< char** >( chat + off );
		int& size = *reinterpret_cast< int* >( chat + off + 4 );
		if ( !str || size < old_len || size >= k_history_max || str[ size ] || size + new_len - old_len >= k_history_max )
			return false;

		for ( int at = size - old_len; at >= 0; --at ) {
			if ( memcmp( str + at, old_html, old_len ) )
				continue;

			memmove( str + at + new_len, str + at + old_len, size - at - old_len + 1 );
			memcpy( str + at, new_html, new_len );
			size += new_len - old_len;
			return true;
		}
	} __except ( EXCEPTION_EXECUTE_HANDLER ) {
	}
	return false;
}

static c_uipanel* find_hud( c_ui_engine* ui )
{
	static c_uipanel* hud = nullptr;
	if ( !ui )
		return nullptr;

	for ( c_uipanel* known : { hud, g_scaleform.m_hud_panel, g_chud.m_hud_panel } )
		if ( known && ui->is_valid_panel_ptr( known ) )
			return hud = known;

	hud      = nullptr;
	auto itr = ui->get_last_dispatched_event_target_panel( );
	for ( int guard = 0; itr && guard < 64 && ui->is_valid_panel_ptr( itr ); ++guard ) {
		if ( HASH_RT( itr->get_id( ) ) == HASH_BT( "CSGOHud" ) )
			return hud = itr;

		auto parent = itr->get_parent( );
		if ( !parent || parent == itr )
			break;
		itr = parent;
	}
	return nullptr;
}

void __fastcall n_detoured_functions::push_notice( void* ecx, void* edx, const char* text, int str_len, const char* null )
{
	static auto original = g_hooks.m_push_notice.get_original< void( __fastcall* )( void*, void*, const char*, int, const char* ) >( );
	HOOK_SCOPE_OR_BAIL( original( ecx, edx, text, str_len, null ) );

	if ( !text )
		return original( ecx, edx, text, str_len, null );

	// mc hud owns chat: stock panorama chat never gets the line (css hide loses to csgo's own chat fade)
	const auto forward = [ & ]( const char* line, int len, const char* raw ) {
		if ( !n_mc_hud::enabled( ) )
			original( ecx, edx, line, len, raw );
	};

	const auto to_hex = []( const c_color& color ) {
		return std::format( "#{:02x}{:02x}{:02x}", static_cast< unsigned int >( color[ 0 ] ), static_cast< unsigned int >( color[ 1 ] ),
		                    static_cast< unsigned int >( color[ 2 ] ) );
	};

	const auto detection_label = [ ]( ) {
		const std::string label = GET_VARIABLE( g_variables.m_detection_label, std::string );
		return label.empty( ) ? std::string( "botox" ) : label;
	};

	const auto detection_html = [ & ]( const std::string& event ) {
		return std::format( R"(<font color="{}">{}</font><font color="#bebebe"> | {}</font>)", to_hex( GET_VARIABLE( g_variables.m_accent, c_color ) ),
		                    detection_label( ), event );
	};

	const auto print_detection = [ & ]( const std::string& event ) {
		const std::string message = detection_html( event );

		g_chud.on_chat( std::format( "{} | {}", detection_label( ), event ) );
		g_mc_hud.on_chat( message );

		return forward( message.c_str( ), static_cast< int >( message.length( ) ), message.c_str( ) );
	};

	const auto strip_markup = []( std::string plain ) {
		for ( size_t open = plain.find( '<' ); open != std::string::npos; open = plain.find( '<', open ) ) {
			const size_t close = plain.find( '>', open );
			if ( close == std::string::npos )
				break;

			plain.erase( open, close - open + 1 );
		}
		return plain;
	};

	switch ( HASH_RT( text ) ) {
	case HASH_BT( CHAT_TOKEN_JUMPSTATS ): {
		const std::string message = std::format( R"(<font color="{}">{}</font><font color="#7d7d7d"> |</font>{})",
		                                         to_hex( GET_VARIABLE( g_variables.m_accent, c_color ) ), detection_label( ), g_jump_stats_line );
		g_chud.on_chat( strip_markup( message ) );
		g_mc_hud.on_chat( message );
		return forward( message.c_str( ), static_cast< int >( message.length( ) ), message.c_str( ) );
	}
	case HASH_BT( CHAT_TOKEN_EDGEBUG ): {
		const std::string event = g_edge_bug_chain > 1 ? std::format( "edgebugged x{}", g_edge_bug_chain ) : std::string( "edgebugged" );
		if ( !GET_VARIABLE( g_variables.m_detection_stack_eb, bool ) )
			return print_detection( event );

		const bool chained = g_edge_bug_chain > 1;
		g_chud.on_chat( std::format( "{} | {}", detection_label( ), event ), true, chained );
		g_mc_hud.on_chat( detection_html( event ), chained );

		static float line_time = -1.f;
		const float now        = g_interfaces.m_global_vars_base->m_current_time;
		const auto ui          = g_interfaces.m_panorama ? g_interfaces.m_panorama->access_ui_engine( ) : nullptr;
		c_uipanel* hud         = find_hud( ui );
		const bool alive       = line_time >= 0.f && now >= line_time && now - line_time < 13.5f;

		static std::string history_line;
		static int history_off = -1;

		const std::string message = detection_html( event );
		if ( chained && alive && hud ) {
			std::string js = k_restack_eb;
			replace_js( js, "${html}", message );
			ui->run_script( hud, js.c_str( ), "panorama/layout/hud/hud.xml", 8, 10, false, false );

			uint8_t* chat = history_off >= 0 ? find_chat_element( ) : nullptr;
			if ( chat && patch_history( chat, history_off, history_line.c_str( ), static_cast< int >( history_line.length( ) ), message.c_str( ),
			                            static_cast< int >( message.length( ) ) ) )
				history_line = message;
			return;
		}

		forward( message.c_str( ), static_cast< int >( message.length( ) ), message.c_str( ) );
		history_line = message;
		if ( history_off < 0 )
			if ( uint8_t* chat = find_chat_element( ) )
				history_off = find_history_offset( chat, message.c_str( ), static_cast< int >( message.length( ) ) );
		line_time = hud ? now : -1.f;
		if ( hud )
			ui->run_script( hud, k_tag_eb, "panorama/layout/hud/hud.xml", 8, 10, false, false );
		return;
	}
	case HASH_BT( CHAT_TOKEN_TEXTUREBUG ):
		return print_detection( "texturebugged" );
	case HASH_BT( CHAT_TOKEN_AIRSTUCK ):
		return print_detection( "airstucked" );
	case HASH_BT( CHAT_TOKEN_PIXELSURF ):
		return print_detection( "pixelsurfed" );
	case HASH_BT( CHAT_TOKEN_WALLCLIMB ):
		return print_detection( "wallclimbed" );
	case HASH_BT( CHAT_TOKEN_RAW ): {
		std::string html;
		if ( !n_chat_extras::pop_line( html ) )
			return;
		if ( const std::string plain = strip_markup( html ); !plain.empty( ) ) {
			g_chud.on_chat( plain );
			g_mc_hud.on_chat( html );
		}
		return forward( html.c_str( ), static_cast< int >( html.length( ) ), html.c_str( ) );
	}
	}

	g_chud.on_chat( strip_markup( text ) );
	// real args: ( szColorNotice, clientId, bool bAlreadyFormattedAsCensoredAndSafeHTML ), stock chat = raw color bytes
	g_mc_hud.on_notice( text, str_len, ( reinterpret_cast< uintptr_t >( null ) & 0xFF ) != 0 );

	forward( text, str_len, null );

	if ( !GET_VARIABLE( g_variables.m_chat_images, bool ) )
		return;

	for ( const auto& url : n_chat_extras::image_urls( text ) ) {
		const std::string html = n_chat_extras::image_html( url );
		forward( html.c_str( ), static_cast< int >( html.length( ) ), html.c_str( ) );
	}
}
