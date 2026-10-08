#pragma once
#include "../../../utilities/memory/virtual.h"

class c_studio_render
{
public:
	void set_color_modulation( float const* color )
	{
		g_virtual.call< void >( this, 27, color );
	}

	void set_alpha_modulation( float alpha )
	{
		g_virtual.call< void >( this, 28, alpha );
	}
};