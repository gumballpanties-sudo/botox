#pragma once

#include <windows.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <utility>
#include <vector>

/* single file vpk v2 ( vpklib/packedstore.cpp ): header 7 x u32, tree ext / dir / name + { crc, preload 0, 0x7fff = data in this file,
   offset, size, 0xffff }, data at 28 + tree size + offset. "x.vpk" without "_dir" = CPackedStore's standalone addon branch.
   test: tools/vpk_writer_test.py */
namespace n_vpk_writer
{
	inline std::uint32_t crc32( const std::vector< char >& data )
	{
		static const auto table = [ ] {
			std::array< std::uint32_t, 256 > out{ };
			for ( std::uint32_t i = 0; i < 256; ++i ) {
				std::uint32_t c = i;
				for ( int k = 0; k < 8; ++k )
					c = c & 1 ? 0xEDB88320u ^ ( c >> 1 ) : c >> 1;
				out[ i ] = c;
			}
			return out;
		}( );

		std::uint32_t crc = 0xFFFFFFFFu;
		for ( const char byte : data )
			crc = table[ ( crc ^ static_cast< std::uint8_t >( byte ) ) & 0xFF ] ^ ( crc >> 8 );
		return ~crc;
	}

	inline bool read_disk( const std::string& path, std::vector< char >& out )
	{
		std::ifstream file( path, std::ios::binary );
		if ( !file )
			return false;
		out.assign( std::istreambuf_iterator< char >( file ), std::istreambuf_iterator< char >( ) );
		return true;
	}

	/* root = folder holding the files ( trailing slash ), files = forward slash relative lowercase paths, aliases = { path, file } extra
	   entries pointing at that file's bytes, patch = size-preserving edit of a file's bytes. a same size vpk at out is kept */
	inline bool write( const std::string& out, const std::string& root, const std::vector< std::string >& files,
	                   const std::vector< std::pair< std::string, std::string > >& aliases = { },
	                   const std::function< void( const std::string&, std::vector< char >& ) >& patch = { } )
	{
		struct entry_t {
			std::uint32_t m_crc, m_offset, m_size;
		};
		std::vector< entry_t > entries( files.size( ) );
		std::map< std::string, std::map< std::string, std::vector< std::pair< std::string, size_t > > > > tree{ };

		const auto disk = [ & ]( const std::string& rel ) {
			std::string path = root + rel;
			for ( char& c : path )
				c = c == '/' ? '\\' : c;
			return path;
		};

		const auto place = [ & ]( const std::string& rel, size_t index ) {
			const size_t slash     = rel.rfind( '/' );
			const std::string dir  = slash == std::string::npos ? " " : rel.substr( 0, slash );
			const std::string file = slash == std::string::npos ? rel : rel.substr( slash + 1 );
			const size_t dot       = file.rfind( '.' );
			const std::string ext  = dot == std::string::npos ? " " : file.substr( dot + 1 );
			tree[ ext ][ dir ].emplace_back( dot == std::string::npos ? file : file.substr( 0, dot ), index );
		};

		std::error_code ec{ };
		std::uint32_t offset = 0;
		std::map< std::string, size_t > index_of{ };
		for ( size_t i = 0; i < files.size( ); ++i ) {
			const std::uintmax_t size = std::filesystem::file_size( disk( files[ i ] ), ec );
			if ( ec )
				return false;

			entries[ i ] = { 0u, offset, static_cast< std::uint32_t >( size ) };
			offset += static_cast< std::uint32_t >( size );
			index_of[ files[ i ] ] = i;
			place( files[ i ], i );
		}

		for ( const auto& [ path, target ] : aliases ) {
			const auto it = index_of.find( target );
			if ( it == index_of.end( ) || index_of.count( path ) )
				return false;
			place( path, it->second );
		}

		const auto serialize = [ & ] {
			std::string dir_data{ };
			const auto put_str = [ & ]( const std::string& s ) { dir_data.append( s ).push_back( '\0' ); };
			const auto put     = [ & ]( auto value ) { dir_data.append( reinterpret_cast< const char* >( &value ), sizeof( value ) ); };
			for ( const auto& [ ext, dirs ] : tree ) {
				put_str( ext );
				for ( const auto& [ dir, names ] : dirs ) {
					put_str( dir );
					for ( const auto& [ name, index ] : names ) {
						put_str( name );
						put( entries[ index ].m_crc );
						put( std::uint16_t{ 0 } );
						put( std::uint16_t{ 0x7fff } );
						put( entries[ index ].m_offset );
						put( entries[ index ].m_size );
						put( std::uint16_t{ 0xffff } );
					}
					put_str( "" );
				}
				put_str( "" );
			}
			put_str( "" );
			return dir_data;
		};

		// tree size never depends on the crc values: a same size vpk from an earlier session is this one
		if ( std::filesystem::file_size( out, ec ) == 28u + serialize( ).size( ) + offset && !ec )
			return true;

		std::vector< char > data{ };
		// patch runs on every read, so the crc covers the bytes written; it may never change a file's size
		const auto read = [ & ]( size_t i ) {
			if ( !read_disk( disk( files[ i ] ), data ) )
				return false;
			if ( patch )
				patch( files[ i ], data );
			return data.size( ) == entries[ i ].m_size;
		};
		for ( size_t i = 0; i < files.size( ); ++i ) {
			if ( !read( i ) )
				return false;
			entries[ i ].m_crc = crc32( data );
		}

		const std::string dir_data      = serialize( );
		const std::uint32_t header[ 7 ] = { 0x55aa1234u, 2u, static_cast< std::uint32_t >( dir_data.size( ) ), offset, 0u, 0u, 0u };
		static_assert( sizeof( header ) == 28 );

		const std::string temp = out + ".tmp";
		{
			std::ofstream file( temp, std::ios::binary | std::ios::trunc );
			file.write( reinterpret_cast< const char* >( header ), sizeof( header ) );
			file.write( dir_data.data( ), static_cast< std::streamsize >( dir_data.size( ) ) );
			for ( size_t i = 0; i < files.size( ) && file; ++i ) {
				if ( !read( i ) )
					return false;
				file.write( data.data( ), static_cast< std::streamsize >( data.size( ) ) );
			}
			if ( !file )
				return false;
		}
		return MoveFileExA( temp.c_str( ), out.c_str( ), MOVEFILE_REPLACE_EXISTING ) != 0;
	}
}
