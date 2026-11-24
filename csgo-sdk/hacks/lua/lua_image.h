#pragma once
#include <windows.h>
#include <io.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

/* render.load_image: pixel size from the file header, nothing decoded ( game thread; the render thread decodes with gdi+ on the
   first draw ). stamp changes when the file does ( last write 100 ns + size ). same file in botox csgo / tf2 / css,
   gate tools/lua_image_check.py */
namespace n_lua_image
{
	/* 32 bit csgo holds the file twice + the decoded bitmap */
	constexpr std::uint64_t k_max_file_bytes = 32ull << 20;

	/* nullptr = ok, else why not */
	inline const char* probe( const char* path, int& width, int& height, std::uint64_t& stamp )
	{
		width = height = 0;
		stamp           = 0;

		FILE* file = nullptr;
		if ( fopen_s( &file, path, "rb" ) || !file )
			return "can't open it";

		const auto done = [ file ]( const char* error ) {
			fclose( file );
			return error;
		};

		BY_HANDLE_FILE_INFORMATION info = { };
		const HANDLE handle             = reinterpret_cast< HANDLE >( _get_osfhandle( _fileno( file ) ) );
		if ( handle == INVALID_HANDLE_VALUE || !GetFileInformationByHandle( handle, &info ) )
			return done( "can't open it" );

		const std::uint64_t bytes = std::uint64_t( info.nFileSizeHigh ) << 32 | info.nFileSizeLow;
		const std::uint64_t write = std::uint64_t( info.ftLastWriteTime.dwHighDateTime ) << 32 | info.ftLastWriteTime.dwLowDateTime;
		if ( bytes > k_max_file_bytes )
			return done( "file over 32 mb" );

		const auto read = [ file ]( const long at, unsigned char* out, const std::size_t size ) {
			return fseek( file, at, SEEK_SET ) == 0 && fread( out, 1, size, file ) == size;
		};
		const auto be16 = []( const unsigned char* p ) { return std::int64_t( p[ 0 ] ) << 8 | p[ 1 ]; };
		const auto le16 = []( const unsigned char* p ) { return std::int64_t( p[ 1 ] ) << 8 | p[ 0 ]; };
		const auto be32 = []( const unsigned char* p ) { return std::int64_t( p[ 0 ] ) << 24 | p[ 1 ] << 16 | p[ 2 ] << 8 | p[ 3 ]; };
		const auto le32 = []( const unsigned char* p ) {
			return std::int64_t( std::int32_t( std::uint32_t( p[ 3 ] ) << 24 | p[ 2 ] << 16 | p[ 1 ] << 8 | p[ 0 ] ) );
		};

		unsigned char h[ 26 ] = { };
		const std::size_t got = fread( h, 1, sizeof( h ), file );
		std::int64_t w = 0, hh = 0;

		if ( got >= 24 && !std::memcmp( h, "\x89PNG\r\n\x1a\n", 8 ) && !std::memcmp( h + 12, "IHDR", 4 ) ) {
			w  = be32( h + 16 );
			hh = be32( h + 20 );
		} else if ( got >= 10 && ( !std::memcmp( h, "GIF87a", 6 ) || !std::memcmp( h, "GIF89a", 6 ) ) ) {
			w  = le16( h + 6 );
			hh = le16( h + 8 );
		} else if ( got >= 26 && h[ 0 ] == 'B' && h[ 1 ] == 'M' ) {
			/* 12 = os/2 core header ( 16 bit size ), else BITMAPINFOHEADER and up; negative height = top down */
			const bool core = le32( h + 14 ) == 12;
			w               = core ? le16( h + 18 ) : le32( h + 18 );
			hh              = core ? le16( h + 20 ) : le32( h + 22 );
			if ( hh < 0 )
				hh = -hh;
		} else if ( got >= 4 && h[ 0 ] == 0xFF && h[ 1 ] == 0xD8 ) {
			/* segments up to the first frame header SOFn ( C0..CF minus C4 DHT, C8 JPG, CC DAC ). real files reach it in a few
			   dozen steps ( a huge icc profile = one step per 64 kb ); the cap stops a file of fill bytes walking byte by byte */
			long at = 2;
			for ( int step = 0; step < 4096; step++ ) {
				unsigned char m[ 9 ];
				if ( !read( at, m, 2 ) || m[ 0 ] != 0xFF )
					break;

				const unsigned char marker = m[ 1 ];
				if ( marker == 0xFF ) { // fill byte
					at += 1;
					continue;
				}
				if ( marker == 0xD8 || marker == 0x01 || ( marker >= 0xD0 && marker <= 0xD7 ) ) { // no length
					at += 2;
					continue;
				}
				if ( marker == 0xD9 || marker == 0xDA || !read( at + 2, m + 2, 2 ) )
					break;

				const std::int64_t length = be16( m + 2 );
				if ( length < 2 )
					break;

				if ( marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC ) {
					if ( length >= 7 && read( at + 4, m + 4, 5 ) ) {
						hh = be16( m + 5 );
						w  = be16( m + 7 );
					}
					break;
				}
				at += 2 + static_cast< long >( length );
			}
		}

		if ( w <= 0 || hh <= 0 || w > 0x7FFFFFFF || hh > 0x7FFFFFFF )
			return done( "not a png / jpg / gif / bmp ( or cut short )" );

		width  = static_cast< int >( w );
		height = static_cast< int >( hh );
		stamp  = write * 31 + bytes;
		return done( nullptr );
	}
}
