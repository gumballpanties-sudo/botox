#pragma once
#include <string.h>

namespace fnv1a
{
	constexpr static unsigned int basis = 0x811c9dc5;
	constexpr static unsigned int prime = 0x1000193;

	consteval unsigned int hash_const( const char* string, const unsigned int value = basis ) noexcept
	{
		return ( string[ 0 ] == '\0' ) ? value : hash_const( &string[ 1 ], ( value ^ static_cast< unsigned int >( string[ 0 ] ) ) * prime );
	}

	/* strlen used to sit in the LOOP CONDITION, so every runtime hash was O(n^2) over the string and
	   re-walked it once per character. HASH_RT runs per vgui panel per frame (paint_traverse), per
	   particle collection per frame and over the whole convar list on attach — walk to the
	   terminator once instead. null-safe: hashing a null name used to fault inside strlen. */
	inline unsigned int hash( const char* string, unsigned int value = basis )
	{
		if ( !string )
			return value;

		for ( ; *string; ++string ) {
			value ^= static_cast< unsigned int >( *string );
			value *= prime;
		}

		return value;
	}
} // namespace fnv1a
