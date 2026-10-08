#pragma once
#include "../../../utilities/memory/virtual.h"

class c_weapon_data;

class c_weapon_system
{
public:
	c_weapon_data* get_weapon_data( short item_definition_index )
	{
		return g_virtual.call< c_weapon_data* >( this, 2, item_definition_index );
	}
};