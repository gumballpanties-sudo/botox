#pragma once
#include "../../../utilities/memory/virtual.h"

class c_panel
{
public:
	const char* get_panel_name( int index )
	{
		return g_virtual.call< const char* >( this, 36, index );
	}
};