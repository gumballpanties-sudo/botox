#pragma once

/* kopuz never registers SMTC on windows ( its log: "SMTC setup failed 0x80070057" ), so read its own daemon api instead:
   gRPC over h2c on \\.\pipe\kopuz-<user sid>, crates/proto/proto/kopuz.proto ( fields used here identical 0.18 .. 0.19 ) */

#include <windows.h>
#include <sddl.h>

#include <algorithm>
#include <climits>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#pragma comment( lib, "advapi32" )

namespace n_kopuz
{
	struct state_t {
		bool m_playing{ };
		std::string m_title{ }, m_artist{ };
		long long m_total_ms{ }, m_position_ms{ };
		std::string m_art_request{ }; // encoded ArtworkRequest, empty = no cover
		unsigned long long m_art_version{ };
		std::wstring m_app{ }; // pipe server's exe path
	};

	namespace detail
	{
		struct reader_t {
			const unsigned char* m_at;
			const unsigned char* m_end;

			explicit reader_t( const std::string_view bytes )
				: m_at( reinterpret_cast< const unsigned char* >( bytes.data( ) ) ), m_end( m_at + bytes.size( ) )
			{
			}

			bool varint( std::uint64_t& out )
			{
				out = 0;
				for ( int shift = 0; shift < 64 && this->m_at < this->m_end; shift += 7 ) {
					const unsigned char byte = *this->m_at++;
					out |= static_cast< std::uint64_t >( byte & 0x7f ) << shift;
					if ( !( byte & 0x80 ) )
						return true;
				}
				return false;
			}

			// varint fields fill value, length-delimited fill bytes, fixed32/64 skipped. false = end or malformed
			bool next( std::uint32_t& field, std::uint64_t& value, std::string_view& bytes )
			{
				std::uint64_t key = 0;
				while ( this->m_at < this->m_end && this->varint( key ) ) {
					field = static_cast< std::uint32_t >( key >> 3 );
					value = 0;
					bytes = { };

					switch ( key & 7 ) {
					case 0:
						return this->varint( value );
					case 1:
					case 5: {
						const std::size_t skip = ( key & 7 ) == 1 ? 8 : 4;
						if ( static_cast< std::size_t >( this->m_end - this->m_at ) < skip )
							return false;
						this->m_at += skip;
						continue;
					}
					case 2: {
						std::uint64_t size = 0;
						if ( !this->varint( size ) || size > static_cast< std::uint64_t >( this->m_end - this->m_at ) )
							return false;
						bytes = { reinterpret_cast< const char* >( this->m_at ), static_cast< std::size_t >( size ) };
						this->m_at += size;
						return true;
					}
					default:
						return false;
					}
				}
				return false;
			}
		};

		inline void put_varint( std::string& out, std::uint64_t value )
		{
			for ( ; value >= 0x80; value >>= 7 )
				out += static_cast< char >( value & 0x7f | 0x80 );
			out += static_cast< char >( value );
		}

		inline void frame( std::string& out, const std::uint8_t type, const std::uint8_t flags, const std::uint32_t stream, const std::string_view payload )
		{
			const auto size = static_cast< std::uint32_t >( payload.size( ) );
			const char header[ 9 ]{ static_cast< char >( size >> 16 ),  static_cast< char >( size >> 8 ),  static_cast< char >( size ),
			                        static_cast< char >( type ),        static_cast< char >( flags ),      static_cast< char >( stream >> 24 ),
			                        static_cast< char >( stream >> 16 ), static_cast< char >( stream >> 8 ), static_cast< char >( stream ) };
			out.append( header, 9 );
			out += payload;
		}

		inline bool write_all( const HANDLE pipe, const std::string& bytes )
		{
			DWORD wrote = 0;
			return WriteFile( pipe, bytes.data( ), static_cast< DWORD >( bytes.size( ) ), &wrote, nullptr ) && wrote == bytes.size( );
		}

		inline std::wstring pipe_name( )
		{
			std::wstring out{ };
			HANDLE token = nullptr;
			if ( !OpenProcessToken( GetCurrentProcess( ), TOKEN_QUERY, &token ) )
				return out;

			DWORD_PTR buffer[ 64 ]{ };
			DWORD size = 0;
			LPWSTR sid = nullptr;
			if ( GetTokenInformation( token, TokenUser, buffer, sizeof( buffer ), &size ) &&
			     ConvertSidToStringSidW( reinterpret_cast< TOKEN_USER* >( buffer )->User.Sid, &sid ) ) {
				out = L"\\\\.\\pipe\\kopuz-" + std::wstring{ sid };
				LocalFree( sid );
			}

			CloseHandle( token );
			return out;
		}

		// one rpc on its own connection, the reply's grpc messages in order. empty = no daemon / error status / timeout
		inline std::vector< std::string > call( const std::string_view method, const std::string_view request, ULONG* server_pid = nullptr,
		                                        const ULONGLONG budget_ms = 1500 )
		{
			static const std::wstring name = pipe_name( ); // media thread only ( /Zc:threadSafeInit- )

			HANDLE pipe = CreateFileW( name.c_str( ), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr );
			if ( pipe == INVALID_HANDLE_VALUE && GetLastError( ) == ERROR_PIPE_BUSY && WaitNamedPipeW( name.c_str( ), 200 ) )
				pipe = CreateFileW( name.c_str( ), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr );
			if ( pipe == INVALID_HANDLE_VALUE )
				return { };

			if ( server_pid && !GetNamedPipeServerProcessId( pipe, server_pid ) )
				*server_pid = 0;

			// hpack: indexed :method POST / :scheme http, the rest literal without indexing, no huffman
			std::string headers = "\x83\x86";
			const auto text = [ & ]( const std::string_view value ) {
				if ( value.size( ) < 127 )
					headers += static_cast< char >( value.size( ) );
				else {
					headers += '\x7f';
					std::size_t rest = value.size( ) - 127;
					for ( ; rest >= 128; rest >>= 7 )
						headers += static_cast< char >( rest & 0x7f | 0x80 );
					headers += static_cast< char >( rest );
				}
				headers += value;
			};
			headers += '\x04';
			text( std::string( "/kopuz.v1.Kopuz/" ) + std::string( method ) );
			headers += '\x01';
			text( "localhost" );
			headers += "\x0f\x10"; // content-type, static index 31
			text( "application/grpc" );
			headers += '\x00';
			text( "te" );
			text( "trailers" );

			std::string body( 5, '\0' );
			for ( int i = 0; i < 4; i++ )
				body[ 1 + i ] = static_cast< char >( request.size( ) >> ( 24 - i * 8 ) );
			body += request;

			// big windows up front: a cover is far past the 64 KiB default and nothing here sends WINDOW_UPDATE later
			std::string out = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
			frame( out, 4, 0, 0, std::string_view( "\x00\x04\x01\x00\x00\x00", 6 ) );
			frame( out, 8, 0, 0, std::string_view( "\x00\xff\x00\x00", 4 ) );
			frame( out, 1, 0x4, 1, headers );
			frame( out, 0, 0x1, 1, body );

			std::string in{ }, data{ };
			bool done = false, failed = !write_all( pipe, out );
			const ULONGLONG deadline = GetTickCount64( ) + budget_ms;

			while ( !done && !failed && GetTickCount64( ) < deadline ) {
				DWORD available = 0;
				if ( !PeekNamedPipe( pipe, nullptr, 0, nullptr, &available, nullptr ) ) {
					failed = true;
					break;
				}

				if ( !available ) {
					Sleep( 2 );
					continue;
				}

				const std::size_t had = in.size( );
				DWORD got             = 0;
				in.resize( had + available );
				if ( !ReadFile( pipe, in.data( ) + had, available, &got, nullptr ) ) {
					failed = true;
					break;
				}
				in.resize( had + got );

				std::size_t at = 0;
				while ( !done && in.size( ) - at >= 9 ) {
					const auto* head        = reinterpret_cast< const unsigned char* >( in.data( ) + at );
					const std::size_t size  = static_cast< std::size_t >( head[ 0 ] ) << 16 | head[ 1 ] << 8 | head[ 2 ];
					if ( in.size( ) - at - 9 < size )
						break;

					const std::uint8_t type    = head[ 3 ], flags = head[ 4 ];
					const std::uint32_t stream = static_cast< std::uint32_t >( head[ 5 ] & 0x7f ) << 24 | head[ 6 ] << 16 | head[ 7 ] << 8 | head[ 8 ];
					std::string_view payload( in.data( ) + at + 9, size );
					at += 9 + size;

					if ( type == 4 && !( flags & 1 ) ) {
						std::string ack{ };
						frame( ack, 4, 1, 0, { } );
						failed = !write_all( pipe, ack );
					} else if ( type == 0 && stream == 1 ) {
						if ( flags & 0x8 ) {
							const std::size_t pad = payload.empty( ) ? 0 : static_cast< unsigned char >( payload[ 0 ] );
							if ( payload.empty( ) || pad >= payload.size( ) ) {
								failed = true;
								break;
							}
							payload = payload.substr( 1, payload.size( ) - 1 - pad );
						}
						data += payload;
					} else if ( type == 7 || ( type == 3 && stream == 1 ) ) {
						failed = true; // GOAWAY / RST_STREAM
					}

					if ( stream == 1 && ( flags & 1 ) && ( type == 0 || type == 1 ) )
						done = true;
				}
				in.erase( 0, at );
			}

			CloseHandle( pipe );

			std::vector< std::string > messages{ };
			if ( !done || failed )
				return messages;

			for ( std::size_t at = 0; data.size( ) - at >= 5; ) {
				const auto* head       = reinterpret_cast< const unsigned char* >( data.data( ) + at );
				const std::size_t size = static_cast< std::size_t >( head[ 1 ] ) << 24 | head[ 2 ] << 16 | head[ 3 ] << 8 | head[ 4 ];
				if ( head[ 0 ] || data.size( ) - at - 5 < size )
					return { };

				messages.emplace_back( data.substr( at + 5, size ) );
				at += 5 + size;
			}

			return messages;
		}

		struct track_t {
			std::string m_title{ }, m_artist{ }, m_art_request{ };
			std::uint64_t m_duration_ms{ }, m_art_version{ };
		};

		inline track_t parse_track( const std::string_view bytes )
		{
			// ArtworkTarget oneof field -> ArtworkRequest oneof field
			static constexpr std::uint32_t k_request_field[ ]{ 0, 1, 2, 0, 5, 6, 7, 8 };

			track_t out{ };
			reader_t row( bytes );
			std::uint32_t field = 0;
			std::uint64_t value = 0;
			std::string_view sub{ };

			while ( row.next( field, value, sub ) ) {
				if ( field == 3 )
					out.m_title = sub;
				else if ( field == 4 )
					out.m_artist = sub;
				else if ( field == 7 )
					out.m_duration_ms = value;
				else if ( field == 21 ) {
					reader_t art( sub );
					while ( art.next( field, value, sub ) ) {
						if ( field == 2 )
							out.m_art_version = value;
						else if ( field == 1 ) {
							reader_t target( sub );
							std::uint32_t kind = 0;
							std::string_view key{ };
							if ( target.next( kind, value, key ) && kind < std::size( k_request_field ) && k_request_field[ kind ] ) {
								out.m_art_request.clear( );
								put_varint( out.m_art_request, k_request_field[ kind ] << 3 | 2 );
								put_varint( out.m_art_request, key.size( ) );
								out.m_art_request += key;
							}
						}
					}
				}
			}

			return out;
		}

		inline long long unix_ms( const FILETIME time )
		{
			return ( static_cast< long long >( time.dwHighDateTime ) << 32 | time.dwLowDateTime ) / 10000 - 11644473600000LL;
		}

		/* PlayerState -> state, m_position_ms at daemon time `at_ms`, moving if `moving`. the reply is the snapshot of the last event,
		   its now_ms ( `snapshot_ms` ) is not send time. false = idle / ended / nothing loaded */
		inline bool parse_state( const std::string_view bytes, state_t& out, std::uint64_t& snapshot_ms, std::uint64_t& at_ms, bool& moving )
		{
			reader_t state( bytes );
			std::uint32_t field = 0;
			std::uint64_t value = 0, phase = 0, now_ms = 0, anchor_ms = 0, anchor_at = 0, fade_ms = 0;
			bool anchor_playing = false, fading = false;
			std::string_view sub{ }, row{ }, fade_row{ };

			while ( state.next( field, value, sub ) ) {
				if ( field == 2 )
					now_ms = value;
				else if ( field == 3 )
					phase = value;
				else if ( field == 14 )
					row = sub;
				else if ( field == 6 ) {
					reader_t anchor( sub );
					while ( anchor.next( field, value, sub ) ) {
						if ( field == 1 )
							anchor_ms = value;
						else if ( field == 2 )
							anchor_at = value;
						else if ( field == 3 )
							anchor_playing = value != 0;
					}
				} else if ( field == 10 ) {
					// crossfade resolving: keep showing the outgoing track at its own position
					reader_t fade( sub );
					fading = true;
					while ( fade.next( field, value, sub ) ) {
						if ( field == 3 )
							fade_ms = value;
						else if ( field == 4 )
							fade_row = sub;
					}
				}
			}

			if ( phase != 2 && phase != 3 ) // PHASE_PLAYING / PHASE_PAUSED
				return false;

			const track_t track = parse_track( fading && !fade_row.empty( ) ? fade_row : row );
			if ( track.m_title.empty( ) )
				return false;

			out.m_playing     = phase == 2;
			out.m_title       = track.m_title;
			out.m_artist      = track.m_artist;
			out.m_total_ms    = static_cast< long long >( track.m_duration_ms );
			out.m_art_request = track.m_art_request;
			out.m_art_version = track.m_art_version;

			out.m_position_ms = static_cast< long long >( fading ? fade_ms : anchor_ms );
			snapshot_ms       = now_ms;
			at_ms             = fading ? now_ms : anchor_at;
			moving            = fading ? out.m_playing : anchor_playing;
			return true;
		}
	}

	// false = kopuz not running / idle / nothing loaded. media thread only
	inline bool get_state( state_t& out )
	{
		static ULONG s_pid{ };
		static std::wstring s_app{ };
		static long long s_floor{ }, s_ceiling{ };

		ULONG pid         = 0;
		const auto reply  = detail::call( "GetPlayerState", { }, &pid );
		FILETIME now_time{ };
		GetSystemTimeAsFileTime( &now_time );
		const long long now = detail::unix_ms( now_time );

		std::uint64_t snapshot_ms = 0, at_ms = 0;
		bool moving               = false;
		if ( reply.size( ) != 1 || !detail::parse_state( reply.front( ), out, snapshot_ms, at_ms, moving ) )
			return false;

		/* daemon clock = ms since its session started. creation time is a floor, every snapshot's ( seen - now_ms ) a ceiling.
		   ponytail: fixed 200 ms startup guess ( measured 230 ms incl. poll ), a slow cold start runs the bar ahead until restart */
		if ( pid != s_pid ) {
			s_pid     = pid;
			s_floor   = 0;
			s_ceiling = LLONG_MAX;
			s_app.clear( );

			if ( const HANDLE process = OpenProcess( PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid ) ) {
				wchar_t path[ MAX_PATH ]{ };
				DWORD size = MAX_PATH;
				if ( QueryFullProcessImageNameW( process, 0, path, &size ) )
					s_app = path;

				FILETIME created{ }, exited{ }, kernel{ }, user{ };
				if ( GetProcessTimes( process, &created, &exited, &kernel, &user ) )
					s_floor = detail::unix_ms( created );

				CloseHandle( process );
			}
		}

		s_ceiling              = ( std::min )( s_ceiling, now - static_cast< long long >( snapshot_ms ) );
		const long long offset = ( std::min )( s_floor ? s_floor + 200 : s_ceiling, s_ceiling );
		const long long daemon = now - offset;

		if ( moving && daemon > static_cast< long long >( at_ms ) )
			out.m_position_ms += daemon - static_cast< long long >( at_ms );
		if ( out.m_total_ms > 0 )
			out.m_position_ms = ( std::min )( out.m_position_ms, out.m_total_ms );

		out.m_app = s_app;
		return true;
	}

	// encoded image ( jpeg / png as the source had it ), empty = none
	inline std::vector< unsigned char > get_artwork( const std::string& request )
	{
		std::vector< unsigned char > out{ };
		if ( request.empty( ) )
			return out;

		for ( const auto& message : detail::call( "GetArtwork", request, nullptr, 3000 ) ) {
			detail::reader_t chunk( message );
			std::uint32_t field = 0;
			std::uint64_t value = 0;
			std::string_view data{ };
			while ( chunk.next( field, value, data ) )
				if ( field == 2 && out.size( ) + data.size( ) <= 16u * 1024u * 1024u )
					out.insert( out.end( ), data.begin( ), data.end( ) );
		}

		return out;
	}
}
