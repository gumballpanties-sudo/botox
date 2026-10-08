#pragma once

#include <windows.h>
#include <gdiplus.h>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <string>

#pragma comment( lib, "gdiplus.lib" )

namespace n_moi_image
{
	constexpr int k_too_big = 2048;
	constexpr int k_target  = 1024;

	inline bool png_size( const std::string& path, int& w, int& h )
	{
		std::ifstream file( path, std::ios::binary );
		uint8_t head[ 24 ]{ };
		if ( !file.read( reinterpret_cast< char* >( head ), sizeof( head ) ) || head[ 1 ] != 'P' || head[ 2 ] != 'N' || head[ 3 ] != 'G' )
			return false;

		const auto be = [ & ]( int at ) { return ( head[ at ] << 24 ) | ( head[ at + 1 ] << 16 ) | ( head[ at + 2 ] << 8 ) | head[ at + 3 ]; };
		w = be( 16 );
		h = be( 20 );
		return w > 0 && h > 0;
	}

	inline std::wstring wide( const std::string& s )
	{
		std::wstring out( MultiByteToWideChar( CP_ACP, 0, s.c_str( ), -1, nullptr, 0 ), L'\0' );
		MultiByteToWideChar( CP_ACP, 0, s.c_str( ), -1, out.data( ), static_cast< int >( out.size( ) ) );
		out.pop_back( );
		return out;
	}

	inline bool scale_png( const std::string& path, INT w, INT h )
	{
		using namespace Gdiplus;

		ULONG_PTR token = 0;
		GdiplusStartupInput input;
		if ( GdiplusStartup( &token, &input, nullptr ) != Ok )
			return false;

		const std::string tmp = path + ".tmp";
		bool ok               = false;

		{
			Bitmap src( wide( path ).c_str( ) );
			Bitmap dst( w, h, PixelFormat32bppARGB );

			if ( src.GetLastStatus( ) == Ok && dst.GetLastStatus( ) == Ok ) {
				{
					Graphics g( &dst );
					g.SetCompositingMode( CompositingModeSourceCopy );
					g.SetInterpolationMode( InterpolationModeHighQualityBicubic );
					g.SetPixelOffsetMode( PixelOffsetModeHalf );

					ImageAttributes clamp;
					clamp.SetWrapMode( WrapModeTileFlipXY );
					ok = g.DrawImage( &src, Rect( 0, 0, w, h ), 0, 0, src.GetWidth( ), src.GetHeight( ), UnitPixel, &clamp ) == Ok;
				}

				static const CLSID k_png = { 0x557cf406, 0x1a04, 0x11d3, { 0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e } };
				ok = ok && dst.Save( wide( tmp ).c_str( ), &k_png, nullptr ) == Ok;
			}
		}

		GdiplusShutdown( token );

		ok = ok && MoveFileExA( tmp.c_str( ), path.c_str( ), MOVEFILE_REPLACE_EXISTING );
		if ( !ok )
			DeleteFileA( tmp.c_str( ) );

		return ok;
	}

	inline int shrink_if_oversize( const std::string& path )
	{
		int w = 0, h = 0;
		if ( !png_size( path, w, h ) || ( std::max )( w, h ) <= k_too_big )
			return 0;

		const double scale = static_cast< double >( k_target ) / ( std::max )( w, h );
		const int nw       = ( std::max )( 1, static_cast< int >( w * scale + 0.5 ) );
		const int nh       = ( std::max )( 1, static_cast< int >( h * scale + 0.5 ) );

		return scale_png( path, nw, nh ) ? 1 : -1;
	}
}
