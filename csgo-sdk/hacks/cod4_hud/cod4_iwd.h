#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

/* COD4 .iwd = zip (store / deflate). images/<name>.iwi v6: 28 byte header, mips smallest -> largest, so the
   full size level is the file's tail. pure, no d3d: tools/cod4_hud_test.cpp checks it against python zlib */
namespace n_cod4_iwd
{
	inline std::uint32_t u16( const std::uint8_t* p ) { return p[ 0 ] | p[ 1 ] << 8; }
	inline std::uint32_t u32( const std::uint8_t* p ) { return p[ 0 ] | p[ 1 ] << 8 | p[ 2 ] << 16 | static_cast< std::uint32_t >( p[ 3 ] ) << 24; }

	/* ---- raw deflate (RFC 1951) ---- */
	class inflater_t
	{
		const std::uint8_t* m_in = nullptr;
		size_t m_size = 0, m_pos = 0;
		std::uint32_t m_bits = 0;
		int m_count          = 0;
		std::vector< std::uint8_t >* m_out = nullptr;

		struct huff_t {
			std::uint16_t m_counts[ 16 ]{ };
			std::uint16_t m_symbols[ 320 ]{ };
		};

		int bit( )
		{
			if ( !m_count ) {
				if ( m_pos >= m_size )
					throw 1;
				m_bits  = m_in[ m_pos++ ];
				m_count = 8;
			}
			const int b = m_bits & 1;
			m_bits >>= 1;
			--m_count;
			return b;
		}

		int bits( int n )
		{
			int v = 0;
			for ( int i = 0; i < n; ++i )
				v |= bit( ) << i;
			return v;
		}

		static bool build( huff_t& h, const std::uint8_t* lengths, int n )
		{
			std::memset( h.m_counts, 0, sizeof( h.m_counts ) );
			for ( int i = 0; i < n; ++i )
				h.m_counts[ lengths[ i ] ]++;
			h.m_counts[ 0 ] = 0;

			std::uint16_t offs[ 16 ]{ };
			for ( int i = 1; i < 16; ++i )
				offs[ i ] = offs[ i - 1 ] + h.m_counts[ i - 1 ];
			for ( int i = 0; i < n; ++i )
				if ( lengths[ i ] )
					h.m_symbols[ offs[ lengths[ i ] ]++ ] = static_cast< std::uint16_t >( i );
			return true;
		}

		int decode( const huff_t& h )
		{
			int code = 0, first = 0, index = 0;
			for ( int len = 1; len < 16; ++len ) {
				code |= bit( );
				const int count = h.m_counts[ len ];
				if ( code - count < first )
					return h.m_symbols[ index + ( code - first ) ];
				index += count;
				first += count;
				first <<= 1;
				code <<= 1;
			}
			throw 2;
		}

		void codes( const huff_t& lit, const huff_t& dist )
		{
			static constexpr std::uint16_t k_len_base[]  = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
			static constexpr std::uint8_t k_len_extra[]  = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
			static constexpr std::uint16_t k_dist_base[] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
			static constexpr std::uint8_t k_dist_extra[] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

			for ( ;; ) {
				int sym = decode( lit );
				if ( sym < 256 )
					m_out->push_back( static_cast< std::uint8_t >( sym ) );
				else if ( sym == 256 )
					return;
				else {
					sym -= 257;
					if ( sym >= 29 )
						throw 3;
					const int len = k_len_base[ sym ] + bits( k_len_extra[ sym ] );
					const int ds  = decode( dist );
					if ( ds >= 30 )
						throw 4;
					const size_t d = k_dist_base[ ds ] + bits( k_dist_extra[ ds ] );
					if ( d > m_out->size( ) )
						throw 5;
					const size_t from = m_out->size( ) - d;
					for ( int i = 0; i < len; ++i )
						m_out->push_back( ( *m_out )[ from + i ] );
				}
			}
		}

	public:
		bool run( const std::uint8_t* in, size_t size, std::vector< std::uint8_t >& out )
		{
			m_in = in, m_size = size, m_pos = 0, m_bits = 0, m_count = 0, m_out = &out;
			try {
				int last = 0;
				do {
					last           = bit( );
					const int type = bits( 2 );
					if ( type == 0 ) {
						m_count = 0;
						if ( m_pos + 4 > m_size )
							return false;
						const std::uint32_t len = u16( m_in + m_pos );
						m_pos += 4;
						if ( m_pos + len > m_size )
							return false;
						out.insert( out.end( ), m_in + m_pos, m_in + m_pos + len );
						m_pos += len;
					}
					else if ( type == 1 ) {
						static huff_t s_lit{ }, s_dist{ };
						static const bool s_ready = [ ] {
							std::uint8_t l[ 288 ];
							for ( int i = 0; i < 144; ++i ) l[ i ] = 8;
							for ( int i = 144; i < 256; ++i ) l[ i ] = 9;
							for ( int i = 256; i < 280; ++i ) l[ i ] = 7;
							for ( int i = 280; i < 288; ++i ) l[ i ] = 8;
							build( s_lit, l, 288 );
							for ( int i = 0; i < 30; ++i ) l[ i ] = 5;
							build( s_dist, l, 30 );
							return true;
						}( );
						( void )s_ready;
						codes( s_lit, s_dist );
					}
					else if ( type == 2 ) {
						static constexpr std::uint8_t k_order[ 19 ] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
						const int nlen = bits( 5 ) + 257, ndist = bits( 5 ) + 1, ncode = bits( 4 ) + 4;
						std::uint8_t lengths[ 320 ]{ };
						for ( int i = 0; i < ncode; ++i )
							lengths[ k_order[ i ] ] = static_cast< std::uint8_t >( bits( 3 ) );
						huff_t lencode{ };
						build( lencode, lengths, 19 );
						int i = 0;
						while ( i < nlen + ndist ) {
							int sym = decode( lencode );
							if ( sym < 16 )
								lengths[ i++ ] = static_cast< std::uint8_t >( sym );
							else {
								int len = 0, rep = 0;
								if ( sym == 16 ) {
									if ( !i )
										return false;
									len = lengths[ i - 1 ];
									rep = 3 + bits( 2 );
								}
								else if ( sym == 17 )
									rep = 3 + bits( 3 );
								else
									rep = 11 + bits( 7 );
								if ( i + rep > nlen + ndist )
									return false;
								while ( rep-- )
									lengths[ i++ ] = static_cast< std::uint8_t >( len );
							}
						}
						huff_t lit{ }, dist{ };
						build( lit, lengths, nlen );
						build( dist, lengths + nlen, ndist );
						codes( lit, dist );
					}
					else
						return false;
				} while ( !last );
			}
			catch ( int ) {
				return false;
			}
			return true;
		}
	};

	/* ---- zip central directory ---- */
	struct entry_t {
		std::string m_file; // iwd path
		std::uint32_t m_local = 0, m_packed = 0, m_size = 0;
		std::uint16_t m_method = 0;
	};

	inline std::string lower( std::string s )
	{
		for ( char& c : s )
			if ( c >= 'A' && c <= 'Z' )
				c = static_cast< char >( c - 'A' + 'a' );
			else if ( c == '\\' )
				c = '/';
		return s;
	}

	inline bool read_file_part( const std::string& path, std::uint64_t offset, size_t size, std::vector< std::uint8_t >& out )
	{
		FILE* f = nullptr;
		if ( fopen_s( &f, path.c_str( ), "rb" ) || !f )
			return false;
		out.resize( size );
		const bool ok = !_fseeki64( f, static_cast< long long >( offset ), SEEK_SET ) && fread( out.data( ), 1, size, f ) == size;
		fclose( f );
		return ok;
	}

	inline long long file_size( const std::string& path )
	{
		FILE* f = nullptr;
		if ( fopen_s( &f, path.c_str( ), "rb" ) || !f )
			return -1;
		_fseeki64( f, 0, SEEK_END );
		const long long n = _ftelli64( f );
		fclose( f );
		return n;
	}

	// adds every entry of one iwd, later calls win (iw_13 over iw_00, like the game's search order)
	inline bool index_iwd( const std::string& path, std::unordered_map< std::string, entry_t >& index )
	{
		const long long size = file_size( path );
		if ( size < 22 )
			return false;
		const size_t tail = static_cast< size_t >( std::min< long long >( size, 0x10000 + 22 ) );
		std::vector< std::uint8_t > end;
		if ( !read_file_part( path, static_cast< std::uint64_t >( size ) - tail, tail, end ) )
			return false;

		size_t eocd = std::string::npos;
		for ( size_t i = tail - 22 + 1; i-- > 0; )
			if ( u32( &end[ i ] ) == 0x06054b50 ) {
				eocd = i;
				break;
			}
		if ( eocd == std::string::npos )
			return false;

		const std::uint32_t cd_size = u32( &end[ eocd + 12 ] ), cd_offset = u32( &end[ eocd + 16 ] );
		std::vector< std::uint8_t > cd;
		if ( !read_file_part( path, cd_offset, cd_size, cd ) )
			return false;

		for ( size_t p = 0; p + 46 <= cd.size( ) && u32( &cd[ p ] ) == 0x02014b50; ) {
			const std::uint32_t n = u16( &cd[ p + 28 ] ), extra = u16( &cd[ p + 30 ] ), comment = u16( &cd[ p + 32 ] );
			if ( p + 46 + n > cd.size( ) )
				break;
			entry_t e{ path, u32( &cd[ p + 42 ] ), u32( &cd[ p + 20 ] ), u32( &cd[ p + 24 ] ), static_cast< std::uint16_t >( u16( &cd[ p + 10 ] ) ) };
			index[ lower( std::string( reinterpret_cast< const char* >( &cd[ p + 46 ] ), n ) ) ] = std::move( e );
			p += 46 + n + extra + comment;
		}
		return true;
	}

	inline bool read_entry( const entry_t& e, std::vector< std::uint8_t >& out )
	{
		std::vector< std::uint8_t > head;
		if ( !read_file_part( e.m_file, e.m_local, 30, head ) || u32( head.data( ) ) != 0x04034b50 )
			return false;
		const std::uint64_t data = static_cast< std::uint64_t >( e.m_local ) + 30 + u16( &head[ 26 ] ) + u16( &head[ 28 ] );

		std::vector< std::uint8_t > packed;
		if ( !read_file_part( e.m_file, data, e.m_packed, packed ) )
			return false;

		if ( e.m_method == 0 ) {
			out = std::move( packed );
			return out.size( ) == e.m_size;
		}
		if ( e.m_method != 8 )
			return false;

		out.clear( );
		out.reserve( e.m_size );
		inflater_t inflater{ };
		return inflater.run( packed.data( ), packed.size( ), out ) && out.size( ) == e.m_size;
	}

	/* ---- iwi -> 32 bit BGRA (D3DFMT_A8R8G8B8 byte order) ---- */
	struct image_t {
		int m_w = 0, m_h = 0;
		std::vector< std::uint8_t > m_bgra;
	};

	inline void rgb565( std::uint32_t c, int out[ 3 ] )
	{
		out[ 0 ] = ( c >> 11 & 31 ) * 255 / 31; // r
		out[ 1 ] = ( c >> 5 & 63 ) * 255 / 63;  // g
		out[ 2 ] = ( c & 31 ) * 255 / 31;       // b
	}

	// one 4x4 color block (dxt1 rules when `dxt1`, 3 color + transparent mode only there)
	inline void color_block( const std::uint8_t* b, bool dxt1, std::uint8_t px[ 16 ][ 4 ] )
	{
		const std::uint32_t c0 = u16( b ), c1 = u16( b + 2 );
		int c[ 4 ][ 4 ]{ };
		rgb565( c0, c[ 0 ] );
		rgb565( c1, c[ 1 ] );
		c[ 0 ][ 3 ] = c[ 1 ][ 3 ] = 255;
		if ( c0 > c1 || !dxt1 ) {
			for ( int k = 0; k < 3; ++k ) {
				c[ 2 ][ k ] = ( 2 * c[ 0 ][ k ] + c[ 1 ][ k ] ) / 3;
				c[ 3 ][ k ] = ( c[ 0 ][ k ] + 2 * c[ 1 ][ k ] ) / 3;
			}
			c[ 2 ][ 3 ] = c[ 3 ][ 3 ] = 255;
		}
		else {
			for ( int k = 0; k < 3; ++k )
				c[ 2 ][ k ] = ( c[ 0 ][ k ] + c[ 1 ][ k ] ) / 2;
			c[ 2 ][ 3 ] = 255;
			c[ 3 ][ 0 ] = c[ 3 ][ 1 ] = c[ 3 ][ 2 ] = c[ 3 ][ 3 ] = 0;
		}
		const std::uint32_t idx = u32( b + 4 );
		for ( int i = 0; i < 16; ++i ) {
			const int* s = c[ idx >> ( i * 2 ) & 3 ];
			px[ i ][ 0 ] = static_cast< std::uint8_t >( s[ 2 ] ); // b
			px[ i ][ 1 ] = static_cast< std::uint8_t >( s[ 1 ] ); // g
			px[ i ][ 2 ] = static_cast< std::uint8_t >( s[ 0 ] ); // r
			px[ i ][ 3 ] = static_cast< std::uint8_t >( s[ 3 ] );
		}
	}

	inline size_t level0_size( int format, int w, int h )
	{
		const size_t bw = static_cast< size_t >( std::max( 1, ( w + 3 ) / 4 ) ), bh = static_cast< size_t >( std::max( 1, ( h + 3 ) / 4 ) );
		switch ( format ) {
		case 0x01: return static_cast< size_t >( w ) * h * 4;
		case 0x02: return static_cast< size_t >( w ) * h * 3;
		case 0x03: return static_cast< size_t >( w ) * h * 2;
		case 0x04: return static_cast< size_t >( w ) * h;
		case 0x0B: return bw * bh * 8;
		case 0x0C:
		case 0x0D: return bw * bh * 16;
		default: return 0;
		}
	}

	inline bool decode_iwi( const std::vector< std::uint8_t >& file, image_t& out )
	{
		if ( file.size( ) < 28 || file[ 0 ] != 'I' || file[ 1 ] != 'W' || file[ 2 ] != 'i' || file[ 3 ] != 6 )
			return false;
		const int format = file[ 4 ];
		const int w = static_cast< int >( u16( &file[ 6 ] ) ), h = static_cast< int >( u16( &file[ 8 ] ) );
		const size_t need = level0_size( format, w, h );
		if ( !w || !h || !need || need > file.size( ) - 28 )
			return false;

		const std::uint8_t* src = file.data( ) + file.size( ) - need;
		out.m_w = w, out.m_h = h;
		out.m_bgra.assign( static_cast< size_t >( w ) * h * 4, 0 );
		std::uint8_t* dst = out.m_bgra.data( );

		switch ( format ) {
		case 0x01: std::memcpy( dst, src, need ); return true;
		case 0x02:
			for ( int i = 0; i < w * h; ++i )
				dst[ i * 4 ] = src[ i * 3 ], dst[ i * 4 + 1 ] = src[ i * 3 + 1 ], dst[ i * 4 + 2 ] = src[ i * 3 + 2 ], dst[ i * 4 + 3 ] = 255;
			return true;
		case 0x03:
			for ( int i = 0; i < w * h; ++i )
				dst[ i * 4 ] = dst[ i * 4 + 1 ] = dst[ i * 4 + 2 ] = src[ i * 2 ], dst[ i * 4 + 3 ] = src[ i * 2 + 1 ];
			return true;
		case 0x04:
			for ( int i = 0; i < w * h; ++i )
				dst[ i * 4 ] = dst[ i * 4 + 1 ] = dst[ i * 4 + 2 ] = 255, dst[ i * 4 + 3 ] = src[ i ];
			return true;
		default: break;
		}

		const int bw = ( w + 3 ) / 4, bh = ( h + 3 ) / 4;
		const size_t stride = format == 0x0B ? 8 : 16;
		for ( int by = 0; by < bh; ++by ) {
			for ( int bx = 0; bx < bw; ++bx ) {
				const std::uint8_t* b = src + ( static_cast< size_t >( by ) * bw + bx ) * stride;
				std::uint8_t px[ 16 ][ 4 ];
				color_block( format == 0x0B ? b : b + 8, format == 0x0B, px );

				if ( format == 0x0C ) {
					for ( int i = 0; i < 16; ++i )
						px[ i ][ 3 ] = static_cast< std::uint8_t >( ( b[ i / 2 ] >> ( i % 2 * 4 ) & 15 ) * 17 );
				}
				else if ( format == 0x0D ) {
					int a[ 8 ]{ b[ 0 ], b[ 1 ] };
					if ( a[ 0 ] > a[ 1 ] )
						for ( int k = 1; k < 7; ++k ) a[ k + 1 ] = ( ( 7 - k ) * a[ 0 ] + k * a[ 1 ] ) / 7;
					else {
						for ( int k = 1; k < 5; ++k ) a[ k + 1 ] = ( ( 5 - k ) * a[ 0 ] + k * a[ 1 ] ) / 5;
						a[ 6 ] = 0, a[ 7 ] = 255;
					}
					std::uint64_t bitsv = 0;
					for ( int k = 0; k < 6; ++k )
						bitsv |= static_cast< std::uint64_t >( b[ 2 + k ] ) << ( 8 * k );
					for ( int i = 0; i < 16; ++i )
						px[ i ][ 3 ] = static_cast< std::uint8_t >( a[ bitsv >> ( 3 * i ) & 7 ] );
				}

				for ( int i = 0; i < 16; ++i ) {
					const int x = bx * 4 + i % 4, y = by * 4 + i / 4;
					if ( x < w && y < h )
						std::memcpy( dst + ( static_cast< size_t >( y ) * w + x ) * 4, px[ i ], 4 );
				}
			}
		}
		return true;
	}
}
