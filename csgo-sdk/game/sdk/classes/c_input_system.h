#pragma once
#include "../../../utilities/memory/virtual.h"

class c_input_system
{
public:
	void enable_input( bool enable )
	{
		g_virtual.call< void >( this, 11, enable );
	}

	// key window msg lparam -> ButtonCode_t, same code the engine binds by
	int scan_code_to_button_code( long long_param )
	{
		return g_virtual.call< int >( this, 47, static_cast< int >( long_param ) );
	}
};