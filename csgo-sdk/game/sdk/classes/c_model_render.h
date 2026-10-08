#pragma once
#include "../../../utilities//memory/virtual.h"

class c_material;

class c_model_render
{
public:
	void forced_material_override( c_material* material, int material_override_type = 0, int override_amount = 0 )
	{
		g_virtual.call< void >( this, 1, material, material_override_type, override_amount );
	}

	bool is_forced_material_override( )
	{
		return g_virtual.call< bool >( this, 2 );
	}

	void remove_all_decals( unsigned short instance )
	{
		g_virtual.call< void >( this, 10, instance );
	}
};