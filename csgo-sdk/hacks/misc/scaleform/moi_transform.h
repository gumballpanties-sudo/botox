#pragma once

#include <cctype>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace n_moi_transform
{
	using define_list_t = std::vector< std::pair< std::string, std::string > >;

	inline std::string panorama_key( const char* path )
	{
		if ( !path )
			return { };

		std::string p = path;
		for ( char& c : p )
			c = c == '\\' ? '/' : static_cast< char >( tolower( static_cast< unsigned char >( c ) ) );

		for ( size_t pos = 0; ( pos = p.find( "panorama/", pos ) ) != std::string::npos; pos += 9 ) {
			const bool root = pos == 0 || p[ pos - 1 ] == '/' || p[ pos - 1 ] == '}';
			const std::string rest = p.substr( pos + 9 );

			if ( root && ( !rest.compare( 0, 7, "layout/" ) || !rest.compare( 0, 7, "styles/" ) || !rest.compare( 0, 8, "scripts/" ) ) )
				return rest;
		}

		return { };
	}

	inline std::string rewrite_images( std::string text, const std::function< std::string( const std::string& ) >& resolve )
	{
		static constexpr char k_token[] = "file://{images}/";
		constexpr size_t k_len          = sizeof( k_token ) - 1;

		for ( size_t pos = 0; ( pos = text.find( k_token, pos ) ) != std::string::npos; ) {
			size_t end = text.find_first_of( "\"'`) \t\r\n?", pos + k_len );
			if ( end == std::string::npos )
				end = text.size( );

			const std::string replacement = resolve( text.substr( pos + k_len, end - pos - k_len ) );
			if ( replacement.empty( ) ) {
				pos = end;
				continue;
			}

			text.replace( pos, end - pos, replacement );
			pos += replacement.size( );
		}

		return text;
	}

	inline std::string strip_includes( std::string xml, const std::vector< std::string >& paths )
	{
		for ( const auto& needle : paths ) {
			for ( size_t hit; ( hit = xml.find( needle ) ) != std::string::npos; ) {
				const size_t open  = xml.rfind( '<', hit );
				const size_t close = xml.find( '>', hit );
				if ( open == std::string::npos || close == std::string::npos )
					break;

				xml.erase( open, close - open + 1 );
			}
		}

		return xml;
	}

	inline std::string strip_script_includes( std::string xml, const std::vector< std::string >& names )
	{
		std::vector< std::string > paths;
		for ( const auto& name : names )
			paths.push_back( "scripts/hud/" + name + ".js" );

		return strip_includes( std::move( xml ), paths );
	}

	inline std::string dedupe_ids( std::string xml, const std::vector< std::string >& names )
	{
		const auto in_snippets = [ & ]( size_t pos ) {
			const size_t open = xml.rfind( "<snippets>", pos );
			if ( open == std::string::npos )
				return false;
			const size_t close = xml.find( "</snippets>", open );
			return close == std::string::npos || pos < close;
		};

		for ( const auto& name : names ) {
			const std::string needle = "id=\"" + name + "\"";
			int seen                 = 0;

			for ( size_t pos = 0; ( pos = xml.find( needle, pos ) ) != std::string::npos; pos += needle.size( ) ) {
				if ( ( pos && !isspace( static_cast< unsigned char >( xml[ pos - 1 ] ) ) ) || in_snippets( pos ) )
					continue;

				if ( ++seen > 1 )
					xml.insert( pos + needle.size( ) - 1, "__moi" + std::to_string( seen ) );
			}
		}

		return xml;
	}

	inline define_list_t parse_defines( const std::string& css, const std::vector< std::string >& wanted )
	{
		define_list_t out;

		for ( const auto& name : wanted ) {
			const std::string head = "@define " + name + ":";
			const size_t at        = css.find( head );
			if ( at == std::string::npos )
				continue;

			const size_t semi = css.find( ';', at );
			if ( semi == std::string::npos )
				continue;

			std::string value = css.substr( at + head.size( ), semi - at - head.size( ) );
			while ( !value.empty( ) && isspace( static_cast< unsigned char >( value.front( ) ) ) )
				value.erase( value.begin( ) );
			while ( !value.empty( ) && isspace( static_cast< unsigned char >( value.back( ) ) ) )
				value.pop_back( );

			if ( !value.empty( ) )
				out.emplace_back( name, value );
		}

		return out;
	}

	inline std::string patch_css( std::string css, const define_list_t& defines )
	{
		std::string head;

		for ( const auto& [ name, value ] : defines ) {
			if ( css.find( name ) == std::string::npos || css.find( "@define " + name + ":" ) != std::string::npos )
				continue;

			head += "@define " + name + ": " + value + ";\n";
		}

		if ( css.find( ".afxhidden" ) == std::string::npos )
			css += "\n\n.afxhidden\n{\n\topacity: 0;\n\tvisibility: collapse;\n}\n";

		return head + css;
	}
}
