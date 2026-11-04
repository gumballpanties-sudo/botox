#include "chat_extras.h"

#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"
#include "../../globals/logger/logger.h"
#include "../../dependencies/json/json.hpp"
#include "../chat_hud.h"
#include "scaleform/image_cache.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <format>
#include <iterator>
#include <mutex>
#include <thread>

namespace
{
	constexpr const char* k_language_codes[] = { "en", "ru", "de", "fr", "es", "pt-BR", "pl", "tr", "uk", "zh-CN", "ja", "ko",
		                                         "it", "nl", "sv", "fi", "cs", "hu", "el", "ar", "hi", "id", "vi", "ro" };

	constexpr size_t k_max_jobs  = 16;
	constexpr size_t k_max_lines = 32;

	struct job_t {
		std::string m_name;
		std::string m_text;
		std::string m_language;
	};

	std::mutex s_lock;
	std::deque< job_t > s_jobs;
	std::deque< std::string > s_finished; // html, filled by the worker
	std::deque< std::string > s_printing; // html, game thread only, read back in push_notice

	std::string html_escape( const std::string& in )
	{
		std::string out;
		out.reserve( in.size( ) );
		for ( const char c : in ) {
			switch ( c ) {
			case '<': out += "&lt;"; break;
			case '>': out += "&gt;"; break;
			case '&': out += "&amp;"; break;
			case '"': out += "&quot;"; break;
			case '\n':
			case '\r': out += ' '; break;
			default: out += c;
			}
		}
		return out;
	}

	std::string url_encode( const std::string& in )
	{
		std::string out;
		for ( const unsigned char c : in ) {
			if ( std::isalnum( c ) || c == '-' || c == '_' || c == '.' || c == '~' )
				out += static_cast< char >( c );
			else
				out += std::format( "%{:02X}", c );
		}
		return out;
	}

	std::string lower( std::string s )
	{
		for ( char& c : s )
			c = static_cast< char >( std::tolower( static_cast< unsigned char >( c ) ) );
		return s;
	}

	std::string brand( )
	{
		const c_color accent    = GET_VARIABLE( g_variables.m_accent, c_color );
		const std::string label = GET_VARIABLE( g_variables.m_detection_label, std::string );
		return std::format( R"(<font color="#{:02x}{:02x}{:02x}">{}</font><font color="#7d7d7d"> | </font>)", static_cast< unsigned int >( accent[ 0 ] ),
		                    static_cast< unsigned int >( accent[ 1 ] ), static_cast< unsigned int >( accent[ 2 ] ),
		                    html_escape( label.empty( ) ? std::string( "botox" ) : label ) );
	}

	void push_finished( std::string html )
	{
		std::lock_guard< std::mutex > lock( s_lock );
		if ( s_finished.size( ) < k_max_lines )
			s_finished.push_back( std::move( html ) );
	}

	std::string base_code( const std::string& code )
	{
		return lower( code.substr( 0, code.find( '-' ) ) );
	}

	/* CS color bytes \x01..\x10 and newlines out, outer spaces trimmed */
	std::string strip_controls( const std::string& in )
	{
		std::string out;
		for ( const char c : in )
			if ( static_cast< unsigned char >( c ) >= 0x20 )
				out += c;
		const size_t first = out.find_first_not_of( ' ' );
		return first == std::string::npos ? std::string( ) : out.substr( first, out.find_last_not_of( ' ' ) - first + 1 );
	}

	bool has_letter( const std::string& text )
	{
		return std::any_of( text.begin( ), text.end( ),
		                    []( const char c ) { return static_cast< unsigned char >( c ) >= 0x80 || std::isalpha( static_cast< unsigned char >( c ) ); } );
	}

	/* gtx guesses 1-2 latin words wildly ("ez" -> basque "no"), so those need a sure detect */
	bool short_latin( const std::string& text )
	{
		if ( std::any_of( text.begin( ), text.end( ), []( const char c ) { return static_cast< unsigned char >( c ) >= 0x80; } ) )
			return false;
		int words = 0;
		for ( size_t i = 0; i < text.size( ); ++i )
			words += text[ i ] != ' ' && ( i == 0 || text[ i - 1 ] == ' ' );
		return words <= 2;
	}

	const char* http_hint( const unsigned long code )
	{
		switch ( code ) {
		case 0: return "no code";
		case 403: return "forbidden, region/ip block";
		case 429: return "rate limited";
		case 12002: return "timeout";
		case 12007: return "dns fail, host blocked?";
		case 12029: return "cannot connect, firewall/block?";
		case 12030: return "connection aborted";
		case 12157: case 12175: return "tls fail, old windows?";
		default: return code >= 12000 ? "winhttp error" : "http status";
		}
	}

	void log_skip( const char* why, const std::string& text )
	{
		g_console.print( std::format( "TRN: skip {} | '{}'", why, text.substr( 0, 48 ) ).c_str( ) );
	}

	void log_fail( const char* backend, const char* what, const unsigned long code, const std::string& body )
	{
		g_console.print< n_console::log_level::WARNING >(
			std::format( "TRN: {} {} {} ({}) | '{}'", backend, what, code, http_hint( code ), body.substr( 0, 80 ) ).c_str( ) );
	}

	std::string str_at( const nlohmann::json& j, const size_t i )
	{
		return j.is_array( ) && j.size( ) > i && j[ i ].is_string( ) ? j[ i ].get< std::string >( ) : std::string( );
	}

	bool get_json( const char* backend, const std::string& url, nlohmann::json& root, unsigned long& code )
	{
		std::string body;
		if ( !n_image_cache::http_get_text( url, body, &code ) ) {
			log_fail( backend, "http fail", code, body );
			return false;
		}
		root = nlohmann::json::parse( body, nullptr, false );
		if ( root.is_discarded( ) ) {
			log_fail( backend, "bad json", code, body );
			return false;
		}
		return true;
	}

	/* translate.googleapis.com gtx: [[["out","in",...],...],null,"src",null,null,null,conf,...] */
	bool google( const job_t& job, std::string& out, std::string& source, double& conf, unsigned long& code )
	{
		nlohmann::json root;
		if ( !get_json( "google", std::format( "https://translate.googleapis.com/translate_a/single?client=gtx&sl=auto&tl={}&dt=t&q={}", job.m_language,
		                                       url_encode( job.m_text ) ),
		                root, code ) )
			return false;
		if ( !root.is_array( ) || root.empty( ) || !root[ 0 ].is_array( ) ) {
			log_fail( "google", "bad shape", code, root.dump( -1, ' ', false, nlohmann::json::error_handler_t::replace ) );
			return false;
		}

		for ( const auto& segment : root[ 0 ] )
			out += str_at( segment, 0 );

		source = str_at( root, 2 );
		conf   = root.size( ) > 6 && root[ 6 ].is_number( ) ? root[ 6 ].get< double >( ) : 0.0;
		return true;
	}

	/* chrome extension client, own throttle bucket: [["out","src"]], no confidence */
	bool clients5( const job_t& job, std::string& out, std::string& source, double&, unsigned long& code )
	{
		nlohmann::json root;
		if ( !get_json( "clients5", std::format( "https://clients5.google.com/translate_a/t?client=dict-chrome-ex&sl=auto&tl={}&q={}", job.m_language,
		                                         url_encode( job.m_text ) ),
		                root, code ) )
			return false;

		const nlohmann::json& first = root.is_array( ) && !root.empty( ) ? root[ 0 ] : root;
		out    = first.is_string( ) ? first.get< std::string >( ) : str_at( first, 0 );
		source = str_at( first, 1 );
		if ( out.empty( ) ) {
			log_fail( "clients5", "bad shape", code, root.dump( -1, ' ', false, nlohmann::json::error_handler_t::replace ) );
			return false;
		}
		return true;
	}

	/* mymemory, ~5000 chars/day per ip: {"responseData":{"translatedText","detectedLanguage"},"responseStatus":200|"403","quotaFinished"} */
	bool mymemory( const job_t& job, std::string& out, std::string& source, double&, unsigned long& code )
	{
		nlohmann::json root;
		if ( !get_json( "mymemory", std::format( "https://api.mymemory.translated.net/get?q={}&langpair=autodetect%7C{}", url_encode( job.m_text ), job.m_language ),
		                root, code ) )
			return false;

		const auto data = root.is_object( ) ? root.find( "responseData" ) : root.end( );
		if ( !root.is_object( ) || data == root.end( ) || !data->is_object( ) ) {
			log_fail( "mymemory", "bad shape", code, root.dump( -1, ' ', false, nlohmann::json::error_handler_t::replace ) );
			return false;
		}

		const auto& status_field = root[ "responseStatus" ];
		const int status = status_field.is_number( ) ? status_field.get< int >( ) : status_field.is_string( ) ? std::atoi( status_field.get< std::string >( ).c_str( ) ) : 0;
		const auto& details_field = root[ "responseDetails" ];
		const std::string details = details_field.is_string( ) ? details_field.get< std::string >( ) : std::string( );

		/* same source + target: answer it so the filter logs "already target lang" */
		if ( details.find( "DISTINCT" ) != std::string::npos ) {
			out    = job.m_text;
			source = job.m_language;
			return true;
		}
		if ( root[ "quotaFinished" ].is_boolean( ) && root[ "quotaFinished" ].get< bool >( ) ) {
			code = 429;
			log_fail( "mymemory", "quota", code, details );
			return false;
		}
		if ( status != 200 ) {
			code = static_cast< unsigned long >( status );
			log_fail( "mymemory", "status", code, details );
			return false;
		}

		if ( const auto text = data->find( "translatedText" ); text != data->end( ) && text->is_string( ) )
			out = text->get< std::string >( );
		if ( const auto lang = data->find( "detectedLanguage" ); lang != data->end( ) && lang->is_string( ) )
			source = lang->get< std::string >( );
		return !out.empty( );
	}

	struct backend_t {
		const char* m_name;
		bool ( *m_fn )( const job_t&, std::string&, std::string&, double&, unsigned long& );
		std::chrono::steady_clock::time_point m_cool_until{ };
	};

	/* worker thread only, one worker alive at a time */
	backend_t s_backends[] = { { "google", google }, { "clients5", clients5 }, { "mymemory", mymemory } };

	/* 429 / 403 / quota: hammering a throttled ip only extends the ban; winhttp 12xxx: blocked host = timeout per line */
	constexpr auto k_cooldown = std::chrono::minutes( 10 );

	bool translate( const job_t& job, std::string& out, std::string& source )
	{
		double conf = 0.0;
		bool got    = false;
		for ( auto& backend : s_backends ) {
			const auto now = std::chrono::steady_clock::now( );
			if ( now < backend.m_cool_until )
				continue;

			out.clear( );
			source.clear( );
			conf               = 0.0;
			unsigned long code = 0;
			if ( backend.m_fn( job, out, source, conf, code ) ) {
				got = true;
				break;
			}
			if ( code == 429 || code == 403 || code >= 12000 ) {
				backend.m_cool_until = now + k_cooldown;
				g_console.print< n_console::log_level::WARNING >( std::format( "TRN: {} cooling 10 min after {}", backend.m_name, code ).c_str( ) );
			}
		}
		if ( !got )
			return false;

		const char* why = out.empty( )                                    ? "empty result"
		                  : base_code( source ) == base_code( job.m_language ) ? "already target lang"
		                  : lower( out ) == lower( job.m_text )            ? "unchanged"
		                  : conf < 0.7 && short_latin( job.m_text )         ? "short low conf"
		                                                                    : nullptr;
		if ( why ) {
			log_skip( std::format( "{} src={} conf={:.2f}", why, source, conf ).c_str( ), job.m_text );
			return false;
		}
		return true;
	}

	void worker( )
	{
		for ( ;; ) {
			job_t job;
			{
				std::lock_guard< std::mutex > lock( s_lock );
				if ( s_jobs.empty( ) || g_ctx.m_unloading ) {
					n_chat_extras::g_worker_alive = false;
					return;
				}
				job = std::move( s_jobs.front( ) );
				s_jobs.pop_front( );
			}

			std::string translated, source;
			if ( translate( job, translated, source ) )
				push_finished( std::format( R"(<font color="#bdc3c7">{}</font><font color="#7d7d7d"> [{}->{}]: </font><font color="#00ff00">{}</font>)",
				                            html_escape( job.m_name ), html_escape( lower( source ) ), html_escape( lower( job.m_language ) ),
				                            html_escape( translated ) ) );
		}
	}

	/* protobuf wire: varint key, type 0 = varint, 2 = length delimited */
	bool read_varint( const uint8_t*& at, const uint8_t* end, uint64_t& value )
	{
		value = 0;
		for ( int shift = 0; at < end && shift < 64; shift += 7 ) {
			const uint8_t byte = *at++;
			value |= static_cast< uint64_t >( byte & 0x7f ) << shift;
			if ( !( byte & 0x80 ) )
				return true;
		}
		return false;
	}

	/* SayText: 1 ent_idx, 2 text. SayText2: 1 ent_idx, 3 msg_name, 4 params */
	struct say_text_t {
		int m_ent_idx = 0;
		std::string m_text;
		std::string m_msg_name;
		std::vector< std::string > m_params;
	};

	bool parse_say_text( const uint8_t* at, const uint8_t* end, say_text_t& out )
	{
		while ( at < end ) {
			uint64_t key = 0;
			if ( !read_varint( at, end, key ) )
				return false;

			const uint32_t field = static_cast< uint32_t >( key >> 3 );
			const uint32_t wire  = static_cast< uint32_t >( key & 7 );

			if ( wire == 0 ) {
				uint64_t value = 0;
				if ( !read_varint( at, end, value ) )
					return false;
				if ( field == 1 )
					out.m_ent_idx = static_cast< int >( value );
			} else if ( wire == 2 ) {
				uint64_t length = 0;
				if ( !read_varint( at, end, length ) || length > static_cast< uint64_t >( end - at ) )
					return false;
				std::string value( reinterpret_cast< const char* >( at ), static_cast< size_t >( length ) );
				at += length;
				if ( field == 2 )
					out.m_text = std::move( value );
				else if ( field == 3 )
					out.m_msg_name = std::move( value );
				else if ( field == 4 )
					out.m_params.push_back( std::move( value ) );
			} else
				return false;
		}
		return true;
	}

	/* community chat plugins send the whole line in one string: "*DEAD* [VIP] name : text" */
	bool split_line( const std::string& line, const int ent_idx, std::string& name, std::string& text )
	{
		size_t at = std::string::npos;

		player_info_t info{ };
		if ( g_interfaces.m_engine_client->get_player_info( ent_idx, &info ) && info.m_name[ 0 ] ) {
			name = strip_controls( info.m_name );
			at   = name.empty( ) ? std::string::npos : line.find( name );
			if ( at != std::string::npos )
				at += name.size( );
		}

		if ( at == std::string::npos ) {
			at = line.find( ": " );
			if ( at == std::string::npos )
				return false;
			if ( name.empty( ) )
				name = strip_controls( line.substr( 0, at ) );
		}

		text = line.substr( at );
		text.erase( 0, text.find_first_not_of( " :" ) );
		return !text.empty( );
	}
} // namespace

void n_chat_extras::on_chat_message( const int type, const void* msg, const int size )
{
	if ( !GET_VARIABLE( g_variables.m_chat_translator, bool ) || !msg || size <= 0 )
		return;

	const auto data = static_cast< const uint8_t* >( msg );
	say_text_t say{ };
	if ( !parse_say_text( data, data + size, say ) )
		return;

	/* ent 0 = server adverts / notices */
	if ( say.m_ent_idx <= 0 ) {
		log_skip( std::format( "ent 0 type {}", type ).c_str( ),
		          strip_controls( !say.m_text.empty( ) ? say.m_text : say.m_params.size( ) > 1 ? say.m_params[ 1 ] : say.m_msg_name ) );
		return;
	}
	if ( say.m_ent_idx == g_interfaces.m_engine_client->get_local_player( ) )
		return;

	/* stock "#Cstrike_Chat_*" = params name/text; other "#" tokens (name change, radio) skipped;
	   a raw msg_name is the plugin's own format, chat in params or the whole line in msg_name */
	std::string name, text;
	const bool token = !say.m_msg_name.empty( ) && say.m_msg_name[ 0 ] == '#';
	if ( type == 5 ) {
		if ( !split_line( strip_controls( say.m_text ), say.m_ent_idx, name, text ) ) {
			log_skip( "no split saytext", strip_controls( say.m_text ) );
			return;
		}
	} else if ( say.m_msg_name.find( "Chat" ) != std::string::npos || ( !token && say.m_params.size( ) >= 2 && !strip_controls( say.m_params[ 1 ] ).empty( ) ) ) {
		if ( say.m_params.size( ) < 2 ) {
			log_skip( "chat token no params", say.m_msg_name );
			return;
		}
		name = strip_controls( say.m_params[ 0 ] );
		text = strip_controls( say.m_params[ 1 ] );
	} else if ( !token ) {
		if ( !split_line( strip_controls( say.m_msg_name ), say.m_ent_idx, name, text ) ) {
			log_skip( "no split saytext2", strip_controls( say.m_msg_name ) );
			return;
		}
	} else
		return;

	if ( !has_letter( text ) )
		return;

	const int language = std::clamp( GET_VARIABLE( g_variables.m_chat_translator_language, int ), 0,
	                                 static_cast< int >( std::size( k_language_codes ) ) - 1 );

	std::lock_guard< std::mutex > lock( s_lock );
	if ( s_jobs.size( ) >= k_max_jobs ) {
		log_skip( "queue full", text );
		return;
	}

	s_jobs.push_back( { std::move( name ), std::move( text ), k_language_codes[ language ] } );

	if ( !g_worker_alive.exchange( true ) )
		std::thread( worker ).detach( );
}

void n_chat_extras::on_create_move( )
{
	if ( !n_interfaces::chat_element )
		return;

	std::deque< std::string > ready;
	{
		std::lock_guard< std::mutex > lock( s_lock );
		ready.swap( s_finished );
	}

	for ( const auto& html : ready ) {
		if ( s_printing.size( ) >= k_max_lines )
			s_printing.pop_front( );
		s_printing.push_back( brand( ) + html );
		n_interfaces::chat_element->chatprintf( 0, 0, CHAT_TOKEN_RAW );
	}
}

bool n_chat_extras::pop_line( std::string& html )
{
	if ( s_printing.empty( ) )
		return false;

	html = std::move( s_printing.front( ) );
	s_printing.pop_front( );
	return true;
}

std::vector< std::string > n_chat_extras::image_urls( const std::string& text )
{
	static constexpr const char* k_hosts[]      = { "steamuserimages-a.akamaihd.net/ugc/", "images.steamusercontent.com/ugc/" };
	static constexpr const char* k_extensions[] = { ".png", ".jpg", ".jpeg", ".gif", ".webp" };

	std::vector< std::string > urls;
	for ( size_t at = text.find( "http" ); at != std::string::npos && urls.size( ) < 3; at = text.find( "http", at + 4 ) ) {
		if ( text.compare( at, 7, "http://" ) && text.compare( at, 8, "https://" ) )
			continue;

		size_t end = at;
		while ( end < text.size( ) && !std::isspace( static_cast< unsigned char >( text[ end ] ) ) && text[ end ] != '"' && text[ end ] != '\'' &&
		        text[ end ] != '<' && text[ end ] != '>' && text[ end ] != ']' && text[ end ] != '[' )
			++end;

		const std::string url  = text.substr( at, end - at );
		const std::string path = lower( url.substr( 0, url.find( '?' ) ) );

		bool image = false;
		for ( const char* host : k_hosts )
			image |= path.find( host ) != std::string::npos;
		for ( const char* extension : k_extensions )
			image |= path.size( ) > strlen( extension ) && !path.compare( path.size( ) - strlen( extension ), strlen( extension ), extension );

		if ( image )
			urls.push_back( url );
	}
	return urls;
}

std::string n_chat_extras::image_html( const std::string& url )
{
	/* image_urls never lets a quote or bracket in, and push_notice text already carries its own &amp; */
	return std::format( R"(<img src="{}"></img>)", url );
}
