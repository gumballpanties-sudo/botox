#pragma once

/* players with no ( or broken ) SMTC and a window title without the song: the audible app has the song file open.
   tags via the windows property system ( mp3 / flac / m4a / opus / ogg / wma .. ), cover via shell thumbnail,
   the file's own embedded picture ( opus / ogg / flac ), or cover.jpg / folder.jpg beside it */

#include <windows.h>
#include <winternl.h>
#include <propkey.h>
#include <propvarutil.h>
#include <shobjidl.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <string>
#include <vector>

#pragma comment( lib, "ntdll" )
#pragma comment( lib, "propsys" )
#pragma comment( lib, "gdi32" )

namespace n_media_file
{
	struct tags_t {
		std::wstring m_title{ }, m_artist{ };
		long long m_total_ms{ };
	};

	namespace detail
	{
		inline std::wstring lower( std::wstring text )
		{
			for ( auto& c : text )
				c = static_cast< wchar_t >( std::towlower( c ) );
			return text;
		}

		inline bool is_media( const std::wstring& path )
		{
			static constexpr const wchar_t* k_ext[ ]{ L".mp3", L".flac", L".opus", L".ogg", L".oga", L".m4a", L".aac", L".wav", L".wma",
			                                          L".aiff", L".aif", L".ape", L".wv", L".alac", L".mka", L".mp4", L".mkv", L".webm" };
			const auto dot = path.rfind( L'.' );
			if ( dot == std::wstring::npos )
				return false;
			const auto ext = lower( path.substr( dot ) );
			return std::any_of( std::begin( k_ext ), std::end( k_ext ), [ & ]( const wchar_t* e ) { return ext == e; } );
		}

		struct handle_entry_t {
			HANDLE m_handle;
			ULONG_PTR m_handle_count;
			ULONG_PTR m_pointer_count;
			ULONG m_access;
			ULONG m_type;
			ULONG m_attributes;
			ULONG m_reserved;
		};

		struct handle_snapshot_t {
			ULONG_PTR m_count;
			ULONG_PTR m_reserved;
			handle_entry_t m_handles[ 1 ];
		};

		// media files open in one process ( ProcessHandleInformation, win8+ ), in handle order
		inline void open_media( const DWORD pid, std::vector< std::wstring >& out )
		{
			using query_t        = NTSTATUS( NTAPI* )( HANDLE, ULONG, PVOID, ULONG, PULONG );
			static const auto nt = reinterpret_cast< query_t >( GetProcAddress( GetModuleHandleW( L"ntdll.dll" ), "NtQueryInformationProcess" ) );

			const HANDLE process = OpenProcess( PROCESS_QUERY_INFORMATION | PROCESS_DUP_HANDLE, FALSE, pid );
			if ( !process || !nt ) {
				if ( process )
					CloseHandle( process );
				return;
			}

			std::vector< unsigned char > buffer( 64 * 1024 );
			ULONG need      = 0;
			NTSTATUS status = 0;
			while ( ( status = nt( process, 51, buffer.data( ), static_cast< ULONG >( buffer.size( ) ), &need ) ) == static_cast< NTSTATUS >( 0xC0000004 ) &&
			        need <= 16u * 1024u * 1024u )
				buffer.resize( need + 4096 );

			if ( status >= 0 ) {
				const auto* snapshot = reinterpret_cast< const handle_snapshot_t* >( buffer.data( ) );
				const std::size_t max = ( buffer.size( ) - offsetof( handle_snapshot_t, m_handles ) ) / sizeof( handle_entry_t );

				for ( std::size_t i = 0; i < ( std::min )( static_cast< std::size_t >( snapshot->m_count ), max ); i++ ) {
					HANDLE copy = nullptr;
					if ( !DuplicateHandle( process, snapshot->m_handles[ i ].m_handle, GetCurrentProcess( ), &copy, 0, FALSE, DUPLICATE_SAME_ACCESS ) )
						continue;

					// GetFileType first: name queries on pipes block
					wchar_t path[ 1024 ]{ };
					if ( GetFileType( copy ) == FILE_TYPE_DISK && GetFinalPathNameByHandleW( copy, path, 1024, 0 ) - 1u < 1023u ) {
						std::wstring name = path;
						if ( name.starts_with( L"\\\\?\\UNC\\" ) )
							name = L"\\\\" + name.substr( 8 );
						else if ( name.starts_with( L"\\\\?\\" ) )
							name.erase( 0, 4 );

						if ( is_media( name ) && std::find( out.begin( ), out.end( ), name ) == out.end( ) )
							out.push_back( std::move( name ) );
					}

					CloseHandle( copy );
				}
			}

			CloseHandle( process );
		}

		inline std::wstring prop_text( IPropertyStore* store, const PROPERTYKEY& key )
		{
			PROPVARIANT value{ };
			std::wstring out{ };
			if ( SUCCEEDED( store->GetValue( key, &value ) ) && value.vt != VT_EMPTY ) {
				wchar_t text[ 512 ]{ };
				if ( SUCCEEDED( PropVariantToString( value, text, 512 ) ) )
					out = text; // multi value ( artists ) joins with "; "
			}
			PropVariantClear( &value );
			return out;
		}

		inline std::vector< unsigned char > read_file( const std::wstring& path, const std::size_t limit )
		{
			std::vector< unsigned char > out{ };
			const HANDLE file = CreateFileW( path.c_str( ), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr );
			if ( file == INVALID_HANDLE_VALUE )
				return out;

			LARGE_INTEGER size{ };
			if ( GetFileSizeEx( file, &size ) && size.QuadPart > 0 ) {
				out.resize( static_cast< std::size_t >( ( std::min )( static_cast< long long >( limit ), size.QuadPart ) ) );
				DWORD got = 0;
				if ( !ReadFile( file, out.data( ), static_cast< DWORD >( out.size( ) ), &got, nullptr ) )
					got = 0;
				out.resize( got );
			}

			CloseHandle( file );
			return out;
		}

		inline std::uint32_t be32( const unsigned char* p ) { return std::uint32_t( p[ 0 ] ) << 24 | p[ 1 ] << 16 | p[ 2 ] << 8 | p[ 3 ]; }
		inline std::uint32_t le32( const unsigned char* p ) { return std::uint32_t( p[ 3 ] ) << 24 | p[ 2 ] << 16 | p[ 1 ] << 8 | p[ 0 ]; }

		// FLAC picture block ( flac METADATA_BLOCK_PICTURE / ogg comment payload ) -> image bytes
		inline std::vector< unsigned char > flac_picture( const unsigned char* p, const std::size_t size )
		{
			std::size_t at = 4; // picture type
			for ( int skip = 0; skip < 2; skip++ ) { // mime, description
				if ( size < at + 4 || size - at - 4 < be32( p + at ) )
					return { };
				at += 4 + be32( p + at );
			}
			at += 16; // width height depth colors
			if ( size < at + 4 || size - at - 4 < be32( p + at ) )
				return { };
			return { p + at + 4, p + at + 4 + be32( p + at ) };
		}

		inline std::vector< unsigned char > base64( const std::string_view text )
		{
			std::vector< unsigned char > out{ };
			std::uint32_t bits = 0;
			int count          = 0;
			for ( const char c : text ) {
				int value = c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52 : c == '+' ? 62 : c == '/' ? 63 : -1;
				if ( value < 0 )
					continue;
				bits = bits << 6 | value;
				if ( ( count += 6 ) >= 8 ) {
					count -= 8;
					out.push_back( static_cast< unsigned char >( bits >> count ) );
				}
			}
			return out;
		}

		// opus / ogg vorbis: second packet = comments, METADATA_BLOCK_PICTURE=<base64 flac picture>. flac: metadata block 6
		inline std::vector< unsigned char > embedded_picture( const std::vector< unsigned char >& file )
		{
			const unsigned char* data = file.data( );
			const std::size_t size    = file.size( );

			if ( size > 8 && !memcmp( data, "fLaC", 4 ) ) {
				for ( std::size_t at = 4; at + 4 <= size; ) {
					const bool last         = data[ at ] & 0x80;
					const int type          = data[ at ] & 0x7f;
					const std::size_t block = std::size_t( data[ at + 1 ] ) << 16 | data[ at + 2 ] << 8 | data[ at + 3 ];
					if ( size - at - 4 < block )
						break;
					if ( type == 6 )
						return flac_picture( data + at + 4, block );
					if ( last )
						break;
					at += 4 + block;
				}
				return { };
			}

			// reassemble the first stream's second packet across pages
			std::vector< unsigned char > packet{ };
			int packets = 0;
			for ( std::size_t at = 0; at + 27 <= size && !memcmp( data + at, "OggS", 4 ) && packets < 2; ) {
				const std::size_t segments = data[ at + 26 ];
				if ( size - at - 27 < segments )
					break;
				std::size_t body = at + 27 + segments;
				for ( std::size_t s = 0; s < segments && packets < 2; s++ ) {
					const std::size_t lace = data[ at + 27 + s ];
					if ( size - body < lace )
						return { };
					if ( packets == 1 )
						packet.insert( packet.end( ), data + body, data + body + lace );
					body += lace;
					if ( lace < 255 )
						packets++;
				}
				std::size_t next = at + 27 + segments;
				for ( std::size_t s = 0; s < segments; s++ )
					next += data[ at + 27 + s ];
				at = next;
			}

			std::size_t at = 0;
			if ( packet.size( ) >= 8 && !memcmp( packet.data( ), "OpusTags", 8 ) )
				at = 8;
			else if ( packet.size( ) >= 7 && !memcmp( packet.data( ), "\x03vorbis", 7 ) )
				at = 7;
			if ( !at || packet.size( ) < at + 4 || packet.size( ) - at - 4 < le32( packet.data( ) + at ) )
				return { };
			at += 4 + le32( packet.data( ) + at ); // vendor
			if ( packet.size( ) < at + 4 )
				return { };

			const std::uint32_t comments = le32( packet.data( ) + at );
			at += 4;
			for ( std::uint32_t i = 0; i < comments && packet.size( ) >= at + 4; i++ ) {
				const std::size_t length = le32( packet.data( ) + at );
				if ( packet.size( ) - at - 4 < length )
					break;
				const std::string_view comment( reinterpret_cast< const char* >( packet.data( ) + at + 4 ), length );
				at += 4 + length;

				constexpr std::string_view k_key = "metadata_block_picture=";
				if ( comment.size( ) > k_key.size( ) &&
				     std::equal( k_key.begin( ), k_key.end( ), comment.begin( ), [ ]( char a, char b ) { return a == std::tolower( static_cast< unsigned char >( b ) ); } ) ) {
					const auto block = base64( comment.substr( k_key.size( ) ) );
					return flac_picture( block.data( ), block.size( ) );
				}
			}

			return { };
		}

		// 32 bpp bottom-up bmp in memory, D3DX loads it as X8R8G8B8
		inline std::vector< unsigned char > bitmap_bytes( const HBITMAP bitmap )
		{
			BITMAP info{ };
			if ( !GetObjectW( bitmap, sizeof( info ), &info ) || info.bmWidth <= 0 || info.bmHeight <= 0 || info.bmWidth > 4096 || info.bmHeight > 4096 )
				return { };

			BITMAPINFOHEADER header{ sizeof( header ), info.bmWidth, info.bmHeight, 1, 32, BI_RGB };
			const std::size_t pixels = std::size_t( info.bmWidth ) * info.bmHeight * 4;
			BITMAPFILEHEADER file{ 0x4d42, static_cast< DWORD >( sizeof( BITMAPFILEHEADER ) + sizeof( header ) + pixels ), 0, 0,
			                       sizeof( BITMAPFILEHEADER ) + sizeof( header ) };

			std::vector< unsigned char > out( file.bfSize );
			memcpy( out.data( ), &file, sizeof( file ) );
			memcpy( out.data( ) + sizeof( file ), &header, sizeof( header ) );

			const HDC dc  = GetDC( nullptr );
			const int got = GetDIBits( dc, bitmap, 0, info.bmHeight, out.data( ) + file.bfOffBits, reinterpret_cast< BITMAPINFO* >( &header ), DIB_RGB_COLORS );
			ReleaseDC( nullptr, dc );
			return got ? out : std::vector< unsigned char >{ };
		}
	}

	// media files the process tree of `exe` has open. caller COM ( MTA ok )
	inline std::vector< std::wstring > open_media( const std::wstring& exe )
	{
		std::vector< std::wstring > out{ };

		const HANDLE snapshot = CreateToolhelp32Snapshot( TH32CS_SNAPPROCESS, 0 );
		if ( snapshot == INVALID_HANDLE_VALUE )
			return out;

		const auto want = detail::lower( exe.substr( exe.find_last_of( L"\\/" ) + 1 ) );
		int scanned     = 0;
		PROCESSENTRY32W entry{ sizeof( entry ) };
		for ( BOOL ok = Process32FirstW( snapshot, &entry ); ok && scanned < 16; ok = Process32NextW( snapshot, &entry ) ) {
			if ( entry.th32ProcessID == GetCurrentProcessId( ) || detail::lower( entry.szExeFile ) != want )
				continue;

			detail::open_media( entry.th32ProcessID, out );
			scanned++;
		}

		CloseHandle( snapshot );
		return out;
	}

	// false = no title, no artist
	inline bool read_tags( const std::wstring& path, tags_t& out )
	{
		IPropertyStore* store = nullptr;
		if ( FAILED( SHGetPropertyStoreFromParsingName( path.c_str( ), nullptr, GPS_DEFAULT, IID_PPV_ARGS( &store ) ) ) || !store )
			return false;

		out.m_title  = detail::prop_text( store, PKEY_Title );
		out.m_artist = detail::prop_text( store, PKEY_Music_Artist );
		if ( out.m_artist.empty( ) )
			out.m_artist = detail::prop_text( store, PKEY_Music_AlbumArtist );

		PROPVARIANT duration{ };
		if ( SUCCEEDED( store->GetValue( PKEY_Media_Duration, &duration ) ) && duration.vt == VT_UI8 )
			out.m_total_ms = static_cast< long long >( duration.uhVal.QuadPart / 10000 ); // 100 ns units
		PropVariantClear( &duration );

		store->Release( );

		// "a; b" -> "a, b"
		for ( std::size_t at; ( at = out.m_artist.find( L"; " ) ) != std::wstring::npos; )
			out.m_artist.replace( at, 2, L", " );

		return !out.m_title.empty( ) || !out.m_artist.empty( );
	}

	// encoded image ( jpeg / png / bmp ), empty = none
	inline std::vector< unsigned char > read_cover( const std::wstring& path )
	{
		IShellItemImageFactory* factory = nullptr;
		if ( SUCCEEDED( SHCreateItemFromParsingName( path.c_str( ), nullptr, IID_PPV_ARGS( &factory ) ) ) && factory ) {
			HBITMAP bitmap = nullptr;
			const bool ok  = SUCCEEDED( factory->GetImage( { 256, 256 }, SIIGBF_THUMBNAILONLY, &bitmap ) ) && bitmap;
			factory->Release( );

			if ( ok ) {
				auto bytes = detail::bitmap_bytes( bitmap );
				DeleteObject( bitmap );
				if ( !bytes.empty( ) )
					return bytes;
			}
		}

		// the comment block sits before the audio: 8 MiB covers any sane cover
		if ( auto picture = detail::embedded_picture( detail::read_file( path, 8u * 1024u * 1024u ) ); !picture.empty( ) )
			return picture;

		const auto folder = path.substr( 0, path.find_last_of( L"\\/" ) + 1 );
		for ( const wchar_t* name : { L"cover.jpg", L"folder.jpg", L"front.jpg", L"cover.png", L"folder.png", L"albumart.jpg" } )
			if ( auto bytes = detail::read_file( folder + name, 16u * 1024u * 1024u ); !bytes.empty( ) )
				return bytes;

		return { };
	}
}
