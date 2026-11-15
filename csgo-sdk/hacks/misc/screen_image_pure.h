#pragma once
// visuals > images: one list item + its one-line text form. Identical copy in botox csgo
// (hacks/misc/screen_image_pure.h), botox-tf2 and botox-css (src/hacks/hud/screen_image_pure.h).
// line = show,scale,rotation,alpha,pixel_art,anchor,x,y,offset_x,offset_y,path
// path is last and keeps its commas; a list = lines joined by '\n'. Test: tests/screen_image_test.cpp
#include <charconv>
#include <cmath>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace n_screen_image
{
    struct image_t {
        bool show       = true;
        std::string path;           // file or http(s) link
        float scale     = 1.f;
        float rotation  = 0.f;      // degrees
        float alpha     = 100.f;    // percent
        bool pixel_art  = false;    // point filter
        int anchor      = 0;        // 0 free ( x / y ), 1 next to watermark ( offsets )
        float x = 100.f, y = 100.f;
        float offset_x = 8.f, offset_y = 0.f;
    };

    inline void put( std::string& out, const float value )
    {
        char buffer[ 32 ];
        const auto result = std::to_chars( buffer, buffer + sizeof( buffer ), value );   // shortest exact round trip
        out.append( buffer, result.ptr );
        out += ',';
    }

    inline std::string to_line( const image_t& image )
    {
        std::string out = image.show ? "1," : "0,";
        put( out, image.scale );
        put( out, image.rotation );
        put( out, image.alpha );
        out += image.pixel_art ? "1," : "0,";
        out += image.anchor == 1 ? "1," : "0,";
        put( out, image.x );
        put( out, image.y );
        put( out, image.offset_x );
        put( out, image.offset_y );

        for ( const char c : image.path )
            if ( c != '\n' && c != '\r' )
                out += c;

        return out;
    }

    /* false = not a line this wrote ( field missing, not a number, inf / nan ): out untouched */
    inline bool from_line( std::string_view line, image_t& out )
    {
        while ( !line.empty( ) && ( line.back( ) == '\r' || line.back( ) == '\n' ) )
            line.remove_suffix( 1 );

        std::string_view fields[ 10 ];
        for ( auto& field : fields ) {
            const size_t comma = line.find( ',' );
            if ( comma == std::string_view::npos )
                return false;

            field = line.substr( 0, comma );
            line.remove_prefix( comma + 1 );
        }

        const auto number = []( const std::string_view text, auto& value ) {
            const auto result = std::from_chars( text.data( ), text.data( ) + text.size( ), value );
            return !text.empty( ) && result.ec == std::errc( ) && result.ptr == text.data( ) + text.size( );
        };

        image_t image;
        int show = 0, pixel_art = 0, anchor = 0;
        if ( !number( fields[ 0 ], show ) || !number( fields[ 1 ], image.scale ) || !number( fields[ 2 ], image.rotation ) ||
             !number( fields[ 3 ], image.alpha ) || !number( fields[ 4 ], pixel_art ) || !number( fields[ 5 ], anchor ) ||
             !number( fields[ 6 ], image.x ) || !number( fields[ 7 ], image.y ) || !number( fields[ 8 ], image.offset_x ) ||
             !number( fields[ 9 ], image.offset_y ) )
            return false;

        for ( const float value : { image.scale, image.rotation, image.alpha, image.x, image.y, image.offset_x, image.offset_y } )
            if ( !std::isfinite( value ) )
                return false;

        image.show      = show != 0;
        image.pixel_art = pixel_art != 0;
        image.anchor    = anchor == 1 ? 1 : 0;
        image.path.assign( line.data( ), line.size( ) );

        out = std::move( image );
        return true;
    }

    inline std::string to_list( const std::vector< image_t >& images )
    {
        std::string out;
        for ( const auto& image : images ) {
            if ( !out.empty( ) )
                out += '\n';
            out += to_line( image );
        }
        return out;
    }

    /* bad lines are dropped, the rest still load */
    inline std::vector< image_t > from_list( std::string_view text )
    {
        std::vector< image_t > out;
        while ( !text.empty( ) ) {
            const size_t end = text.find( '\n' );
            image_t image;
            if ( from_line( text.substr( 0, end ), image ) )
                out.push_back( std::move( image ) );
            if ( end == std::string_view::npos )
                break;
            text.remove_prefix( end + 1 );
        }
        return out;
    }
}
