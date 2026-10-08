#pragma once
#include "../../../utilities/memory/virtual.h"

class c_base_client;

class c_base_client_dll
{
public:
	c_base_client* get_all_classes( )
	{
		return g_virtual.call< c_base_client* >( this, 8 );
	}
};