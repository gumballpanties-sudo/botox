#pragma once

#include <cctype>
#include <string>

namespace n_scaleform_track
{
	inline bool is_name_char( char c ) { return std::isalnum( static_cast< unsigned char >( c ) ) || c == '_' || c == '$'; }

	inline std::string track_styles( std::string js )
	{
		static constexpr char k_token[] = ".style.";
		constexpr size_t k_len          = sizeof( k_token ) - 1;

		for ( size_t pos = 0; ( pos = js.find( k_token, pos ) ) != std::string::npos; ) {
			size_t end = pos + k_len;
			while ( end < js.size( ) && is_name_char( js[ end ] ) )
				++end;

			size_t start = pos;
			int depth    = 0;
			while ( start > 0 ) {
				const char c = js[ start - 1 ];
				if ( c == ')' || c == ']' )
					++depth;
				else if ( c == '(' || c == '[' ) {
					if ( depth == 0 )
						break;
					--depth;
				} else if ( depth == 0 && !is_name_char( c ) && c != '.' )
					break;
				--start;
			}

			// no property, unbalanced or dangling chain: leave the text as it was
			if ( end == pos + k_len || depth != 0 || start == pos || js[ start ] == '.' ) {
				pos = end;
				continue;
			}

			const std::string prop        = js.substr( pos + k_len, end - pos - k_len );
			const std::string replacement = "SfR( " + js.substr( start, pos - start ) + ", '" + prop + "' )." + prop;

			js.replace( start, end - start, replacement );
			pos = start + replacement.size( );
		}

		return js;
	}
}
