#pragma once
#include "../../../utilities/memory/virtual.h"

class c_game_types
{
public:
	int get_current_game_type( )
	{
		return g_virtual.call< int >( this, 8 );
	}
};