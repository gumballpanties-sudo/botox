#pragma once
#include "../../../utilities/memory/virtual.h"

class c_render_view
{
public:
	void set_blend( const float blend )
	{
		g_virtual.call< void >( this, 4, blend );
	}

	float get_blend( )
	{
		return g_virtual.call< float >( this, 5 );
	}

	void set_color_modulation( float const* color )
	{
		g_virtual.call< void >( this, 6, color );
	}

	void set_color_modulation( const float r, const float g, const float b )
	{
		const float color[ 3 ] = { r, g, b };

		this->set_color_modulation( color );
	}
};
