#include "image_cache.h"
#include "../../../utilities/console/console.h"
#include "../../../utilities/perf/perf_watch.h"

#include <windows.h>
#include <winhttp.h>

#include <cctype>
#include <cstdio>
#include <fstream>
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

	bool http_get( const std::string& url, std::string& out, const bool allow_empty = false, const size_t max_bytes = k_max_bytes )
	{
		const std::wstring wide( url.begin( ), url.end( ) );

		wchar_t host[ 256 ]{ }, path[ 2048 ]{ };
		URL_COMPONENTS parts{ };
		parts.dwStructSize     = sizeof( parts );
		parts.lpszHostName     = host;
		parts.dwHostNameLength = _countof( host );
		parts.lpszUrlPath      = path;
		parts.dwUrlPathLength  = _countof( path );

		if ( !WinHttpCrackUrl( wide.c_str( ), 0, 0, &parts ) )
			return false;

		HINTERNET session = WinHttpOpen( L"botox", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0 );
		if ( !session )
			return false;

		WinHttpSetTimeouts( session, 5000, 5000, 10000, 15000 );

		HINTERNET connection = WinHttpConnect( session, host, parts.nPort, 0 );
		HINTERNET request    = connection ? WinHttpOpenRequest( connection, L"GET", path, nullptr, WINHTTP_NO_REFERER,
		                                                        WINHTTP_DEFAULT_ACCEPT_TYPES,
		                                                        parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0u )
		                                  : nullptr;

		bool ok = false;

		if ( request && WinHttpSendRequest( request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0 )
		     && WinHttpReceiveResponse( request, nullptr ) ) {
			DWORD status = 0, status_size = sizeof( status );

			if ( WinHttpQueryHeaders( request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
			                          &status, &status_size, WINHTTP_NO_HEADER_INDEX )
			     && status == 200 ) {
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
		}

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

bool n_image_cache::http_get_text( const std::string& url, std::string& out )
{
	return http_get( url, out );
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
