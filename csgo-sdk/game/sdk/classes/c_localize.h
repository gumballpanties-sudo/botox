#pragma once
#include "../../../utilities/memory/virtual.h"

class c_localize
{
public:
	wchar_t* find( const char* token_name )
	{
		return g_virtual.call< wchar_t* >( this, 11, token_name );
	}

	const wchar_t* find_safe( const char* token_name )
	{
		return g_virtual.call< wchar_t* >( this, 12, token_name );
	}
};