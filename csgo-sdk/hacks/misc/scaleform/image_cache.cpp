#include "image_cache.h"
#include "../../../utilities/console/console.h"
#include "../../../utilities/perf/perf_watch.h"

#include <windows.h>
#include <winhttp.h>

#include <cctype>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <thread>

#pragma comment( lib, "winhttp.lib" )

namespace
{
	constexpr size_t k_max_bytes = 8u * 1024u * 1024u;

	const std::string& cache_dir( )
	{
		static const std::string dir = [ ] ( ) -> std::string {
			char exe_path[ MAX_PATH ]{ };
			if ( !GetModuleFileNameA( nullptr, exe_path, MAX_PATH ) )
				return { };

			std::string root  = exe_path;
			const auto slash  = root.find_last_of( "\\/" );
			if ( slash == std::string::npos )
				return { };

			root.erase( slash );

			const std::string levels[ ] = { root + "\\csgo\\materials",
			                                root + "\\csgo\\materials\\panorama",
			                                root + "\\csgo\\materials\\panorama\\images",
			                                root + "\\csgo\\materials\\panorama\\images\\botox_cache" };

			for ( const auto& level : levels ) {
				if ( !CreateDirectoryA( level.c_str( ), nullptr ) && GetLastError( ) != ERROR_ALREADY_EXISTS )
					return { };
			}

			return levels[ 3 ] + "\\";
		}( );

		return dir;
	}

	uint32_t fnv1a( const std::string& text )
	{
		uint32_t hash = 0x811C9DC5u;
		for ( const char c : text ) {
			hash ^= static_cast< uint8_t >( c );
			hash *= 0x01000193u;
		}
		return hash;
	}

	std::string local_name( const std::string& url )
	{
		const auto slash = url.find_last_of( '/' );
		std::string name = slash == std::string::npos ? url : url.substr( slash + 1 );

		if ( const auto query = name.find( '?' ); query != std::string::npos )
			name.erase( query );

		for ( char& c : name ) {
			if ( !isalnum( static_cast< uint8_t >( c ) ) && c != '.' && c != '_' && c != '-' )
				c = '_';
		}

		if ( name.empty( ) )
			name = "image.png";

		char prefix[ 16 ]{ };
		sprintf_s( prefix, "%08x_", fnv1a( url ) );

		return std::string( prefix ) + name;
	}

	bool file_exists( const std::string& path )
	{
		const DWORD attributes = GetFileAttributesA( path.c_str( ) );
		return attributes != INVALID_FILE_ATTRIBUTES && !( attributes & FILE_ATTRIBUTE_DIRECTORY );
	}

	/* error: http status when one came back, else the WinHTTP GetLastError code */
	bool http_get( const std::string& url, std::string& out, const bool allow_empty = false, const size_t max_bytes = k_max_bytes,
	               unsigned long* error = nullptr )
	{
		unsigned long dummy = 0;
		unsigned long& code = error ? *error : dummy;
		code                = 0;

		const std::wstring wide( url.begin( ), url.end( ) );

		wchar_t host[ 256 ]{ }, path[ 2048 ]{ };
		URL_COMPONENTS parts{ };
		parts.dwStructSize     = sizeof( parts );
		parts.lpszHostName     = host;
		parts.dwHostNameLength = _countof( host );
		parts.lpszUrlPath      = path;
		parts.dwUrlPathLength  = _countof( path );

		if ( !WinHttpCrackUrl( wide.c_str( ), 0, 0, &parts ) ) {
			code = GetLastError( );
			return false;
		}

		HINTERNET session = WinHttpOpen( L"botox", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0 );
		if ( !session ) {
			code = GetLastError( );
			return false;
		}

		WinHttpSetTimeouts( session, 5000, 5000, 10000, 15000 );

		HINTERNET connection = WinHttpConnect( session, host, parts.nPort, 0 );
		HINTERNET request    = connection ? WinHttpOpenRequest( connection, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
		                                                        parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0u )
		                                  : nullptr;

		bool ok = false;

		const bool sent = request && WinHttpSendRequest( request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0 );

		if ( sent && WinHttpReceiveResponse( request, nullptr ) ) {
			DWORD status = 0, status_size = sizeof( status );

			if ( WinHttpQueryHeaders( request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
			                          &status, &status_size, WINHTTP_NO_HEADER_INDEX ) )
				code = status;

			if ( status == 200 ) {
				char buffer[ 8192 ]{ };
				DWORD read = 0;
				ok         = true;

				while ( WinHttpReadData( request, buffer, sizeof( buffer ), &read ) && read ) {
					out.append( buffer, read );

					if ( out.size( ) > max_bytes ) { // a hud icon is never this big - something is wrong
						ok = false;
						break;
					}
				}
			}
		} else
			code = GetLastError( );

		if ( request )
			WinHttpCloseHandle( request );
		if ( connection )
			WinHttpCloseHandle( connection );
		WinHttpCloseHandle( session );

		return ok && ( allow_empty || !out.empty( ) );
	}

	// write next to the target then rename, so a half-downloaded file is never loadable
	bool download_to( const std::string& url, const std::string& destination, const bool allow_empty = false,
	                  const size_t max_bytes = k_max_bytes )
	{
		std::string body;
		if ( !http_get( url, body, allow_empty, max_bytes ) )
			return false;

		const std::string temp = destination + ".part";
		{
			std::ofstream file( temp, std::ios::binary | std::ios::trunc );
			if ( !file )
				return false;

			file.write( body.data( ), static_cast< std::streamsize >( body.size( ) ) );
		}

		if ( !MoveFileExA( temp.c_str( ), destination.c_str( ), MOVEFILE_REPLACE_EXISTING ) ) {
			DeleteFileA( temp.c_str( ) );
			return false;
		}

		return true;
	}

	void worker( )
	{
		/* below normal: may share a core with the main / render thread, so only run on leftover time */
		n_perf::background_thread( );

		for ( ;; ) {
			std::string url;
			{
				std::lock_guard< std::mutex > lock( g_image_cache.m_lock );

				if ( g_image_cache.m_shutdown || g_image_cache.m_queue.empty( ) ) {
					g_image_cache.m_worker_alive = false;
					return;
				}

				url = g_image_cache.m_queue.front( );
				g_image_cache.m_queue.pop_front( );
			}

			const std::string name        = local_name( url );
			const std::string destination = cache_dir( ) + name;

			if ( !file_exists( destination ) && !download_to( url, destination ) ) {
				g_console.print< n_console::log_level::WARNING >( ( "image cache: failed " + url + "\n" ).c_str( ) );
				continue;
			}

			std::lock_guard< std::mutex > lock( g_image_cache.m_lock );
			g_image_cache.m_state[ url ] = "file://{images}/botox_cache/" + name;
		}
	}

	std::string resolve( const std::string& url )
	{
		std::lock_guard< std::mutex > lock( g_image_cache.m_lock );

		if ( const auto it = g_image_cache.m_state.find( url ); it != g_image_cache.m_state.end( ) )
			return it->second;

		if ( cache_dir( ).empty( ) ) {
			g_image_cache.m_state[ url ] = { };
			return { };
		}

		const std::string name = local_name( url );

		if ( file_exists( cache_dir( ) + name ) ) {
			auto& entry = g_image_cache.m_state[ url ];
			entry       = "file://{images}/botox_cache/" + name;
			return entry;
		}

		g_image_cache.m_state[ url ] = { };
		g_image_cache.m_queue.push_back( url );

		if ( !g_image_cache.m_worker_alive && !g_image_cache.m_shutdown ) {
			g_image_cache.m_worker_alive = true;
			std::thread( worker ).detach( );
		}

		return { };
	}
}

bool n_image_cache::download_file( const std::string& url, const std::string& destination )
{
	return download_to( url, destination, true, 32u * 1024u * 1024u );
}

bool n_image_cache::http_get_text( const std::string& url, std::string& out, unsigned long* error )
{
	return http_get( url, out, false, k_max_bytes, error );
}

const std::string& n_image_cache::air_kill_icon( )
{
	static const std::string src = [ ]( ) -> std::string {
		static const char k_svg[] =
			R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="32.074" height="32.074" viewBox="0 0 32.074 32.074"><path fill="#FFFFFF" stroke="#000000" stroke-linecap="round" d="M27.009,28.318 c0.593-0.221,0.552-0.617-0.116-1.191c-1.334-1.139-1.866-2.197-1.596-3.182c0.132-0.486,0.598-1.117,1.397-1.893 c0.466-0.449,1.249-1.172,2.354-2.166c0.938-0.922,1.599-1.76,1.972-2.514c0.492-0.982,0.583-1.945,0.272-2.889 c-0.56-1.712-2.405-2.707-5.535-2.982c-0.848-0.078-2.233-0.124-4.157-0.135c-1.821-0.015-3.148-0.068-3.979-0.16 c-2.416-0.272-4.836-0.894-7.258-1.867C7.938,8.367,6.071,7.325,4.763,6.212C3.452,5.1,2.067,3.536,0.61,1.521 c-0.185,5.724,2.195,9.879,7.139,12.468c-2.309,0.238-4.547-0.446-6.719-2.053c1.658,4.485,5.049,6.988,10.169,7.512 c-1.405,1.018-3.315,1.168-5.726,0.449c2.223,2.84,6.178,4.176,11.864,4.004c-1.101,0.969-2.47,1.529-4.104,1.678 c2.391,1.627,5.613,2.031,9.664,1.215C24.326,27.83,25.695,28.34,27.009,28.318z"/></svg>)svg";

		if ( cache_dir( ).empty( ) )
			return { };

		const std::string path = cache_dir( ) + "air_kill_wings.svg";

		std::string have;
		{
			std::ifstream in( path, std::ios::binary );
			have.assign( std::istreambuf_iterator< char >( in ), std::istreambuf_iterator< char >( ) );
		}

		// rewrite on any difference: a newer dll's icon replaces the old file
		if ( have != k_svg ) {
			std::ofstream out( path, std::ios::binary | std::ios::trunc );
			out << k_svg;
			if ( !out )
				return { };
		}

		return "file://{images}/botox_cache/air_kill_wings.svg";
	}( );

	return src;
}

std::string n_image_cache::impl_t::rewrite( const char* js )
{
	std::string out = js ? js : "";

	for ( size_t pos = 0; ( pos = out.find( "https://", pos ) ) != std::string::npos; ) {
		size_t end = out.find_first_of( "\"'`) \t\r\n\\", pos );
		if ( end == std::string::npos )
			end = out.size( );

		const std::string url   = out.substr( pos, end - pos );
		const std::string local = resolve( url );

		if ( local.empty( ) ) {
			pos = end;
			continue;
		}

		out.replace( pos, url.size( ), local );
		pos += local.size( );
	}

	return out;
}
