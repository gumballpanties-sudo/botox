#include "frame_dump.h"
#include "../../../utilities/console/console.h"

#include <Windows.h>

#include <cstdio>
#include <cstring>
#include <format>
#include <string>
#include <vector>

namespace
{
	void push_big_endian( std::vector< unsigned char >& out, const unsigned int value )
	{
		out.push_back( static_cast< unsigned char >( value >> 24 ) );
		out.push_back( static_cast< unsigned char >( value >> 16 ) );
		out.push_back( static_cast< unsigned char >( value >> 8 ) );
		out.push_back( static_cast< unsigned char >( value ) );
	}

	unsigned int crc32( const unsigned char* data, const size_t length )
	{
		static unsigned int table[ 256 ]{ };
		static bool built = false;

		if ( !built ) {
			for ( unsigned int i = 0; i < 256; ++i ) {
				unsigned int c = i;

				for ( int bit = 0; bit < 8; ++bit )
					c = ( c & 1 ) ? 0xEDB88320u ^ ( c >> 1 ) : c >> 1;

				table[ i ] = c;
			}

			built = true;
		}

		unsigned int crc = 0xFFFFFFFFu;

		for ( size_t i = 0; i < length; ++i )
			crc = table[ ( crc ^ data[ i ] ) & 0xFF ] ^ ( crc >> 8 );

		return crc ^ 0xFFFFFFFFu;
	}

	unsigned int adler32( const unsigned char* data, const size_t length )
	{
		unsigned int a = 1;
		unsigned int b = 0;

		for ( size_t i = 0; i < length; ++i ) {
			a = ( a + data[ i ] ) % 65521;
			b = ( b + a ) % 65521;
		}

		return ( b << 16 ) | a;
	}

	void push_chunk( std::vector< unsigned char >& out, const char* type, const std::vector< unsigned char >& data )
	{
		push_big_endian( out, static_cast< unsigned int >( data.size( ) ) );

		std::vector< unsigned char > body{ type, type + 4 };
		body.insert( body.end( ), data.begin( ), data.end( ) );

		out.insert( out.end( ), body.begin( ), body.end( ) );
		push_big_endian( out, crc32( body.data( ), body.size( ) ) );
	}
}

bool n_frame_dump::save( IDirect3DDevice9* device, IDirect3DSurface9* surface, const char* file_name )
{
	if ( !device || !surface || !file_name )
		return false;

	D3DSURFACE_DESC description{ };
	if ( FAILED( surface->GetDesc( &description ) ) )
		return false;

	if ( description.Format != D3DFMT_A8R8G8B8 && description.Format != D3DFMT_X8R8G8B8 ) {
		g_console.print( "frame dump: not a 32 bit surface, nothing written" );
		return false;
	}

	IDirect3DSurface9* system_surface = nullptr;

	if ( FAILED( device->CreateOffscreenPlainSurface( description.Width, description.Height, description.Format, D3DPOOL_SYSTEMMEM,
	                                                 &system_surface, nullptr ) ) ||
	     !system_surface )
		return false;

	if ( FAILED( device->GetRenderTargetData( surface, system_surface ) ) ) {
		system_surface->Release( );
		g_console.print( "frame dump: GetRenderTargetData refused" );
		return false;
	}

	D3DLOCKED_RECT locked{ };

	if ( FAILED( system_surface->LockRect( &locked, nullptr, D3DLOCK_READONLY ) ) ) {
		system_surface->Release( );
		return false;
	}

	const int width  = static_cast< int >( description.Width );
	const int height = static_cast< int >( description.Height );

	std::vector< unsigned char > raw;
	raw.reserve( static_cast< size_t >( height ) * ( 1 + static_cast< size_t >( width ) * 3 ) );

	for ( int y = 0; y < height; ++y ) {
		const unsigned char* row = static_cast< const unsigned char* >( locked.pBits ) + static_cast< size_t >( y ) * locked.Pitch;

		raw.push_back( 0 );

		for ( int x = 0; x < width; ++x ) {
			raw.push_back( row[ x * 4 + 2 ] );
			raw.push_back( row[ x * 4 + 1 ] );
			raw.push_back( row[ x * 4 + 0 ] );
		}
	}

	system_surface->UnlockRect( );
	system_surface->Release( );

	std::vector< unsigned char > stream{ 0x78, 0x01 };

	for ( size_t offset = 0; offset < raw.size( ); offset += 65535 ) {
		const size_t block  = ( raw.size( ) - offset ) < 65535 ? ( raw.size( ) - offset ) : 65535;
		const bool last     = ( offset + block ) >= raw.size( );
		const unsigned int length = static_cast< unsigned int >( block );

		stream.push_back( last ? 1 : 0 );
		stream.push_back( static_cast< unsigned char >( length ) );
		stream.push_back( static_cast< unsigned char >( length >> 8 ) );
		stream.push_back( static_cast< unsigned char >( ~length ) );
		stream.push_back( static_cast< unsigned char >( ~length >> 8 ) );

		stream.insert( stream.end( ), raw.begin( ) + offset, raw.begin( ) + offset + block );
	}

	push_big_endian( stream, adler32( raw.data( ), raw.size( ) ) );

	std::vector< unsigned char > file{ 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };

	std::vector< unsigned char > header;
	push_big_endian( header, static_cast< unsigned int >( width ) );
	push_big_endian( header, static_cast< unsigned int >( height ) );
	header.push_back( 8 );
	header.push_back( 2 );
	header.push_back( 0 );
	header.push_back( 0 );
	header.push_back( 0 );

	push_chunk( file, "IHDR", header );
	push_chunk( file, "IDAT", stream );
	push_chunk( file, "IEND", { } );

	char path[ MAX_PATH ] = { };
	HMODULE self          = nullptr;
	static int marker     = 0;

	if ( GetModuleHandleExA( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                         reinterpret_cast< const char* >( &marker ), &self ) &&
	     GetModuleFileNameA( self, path, sizeof( path ) ) ) {
		char* slash = strrchr( path, '\\' );

		if ( slash )
			*( slash + 1 ) = '\0';
	} else if ( !GetTempPathA( sizeof( path ), path ) )
		return false;

	strcat_s( path, file_name );

	std::FILE* handle = nullptr;
	if ( fopen_s( &handle, path, "wb" ) != 0 || !handle ) {
		g_console.print( "frame dump: could not open the file" );
		return false;
	}

	std::fwrite( file.data( ), 1, file.size( ), handle );
	std::fclose( handle );

	std::string log_name = path;
	int log_width        = width;
	int log_height       = height;

	g_console.print( std::vformat( "frame dump: wrote {:s} at {:d}x{:d}", std::make_format_args( log_name, log_width, log_height ) ).c_str( ) );

	return true;
}
