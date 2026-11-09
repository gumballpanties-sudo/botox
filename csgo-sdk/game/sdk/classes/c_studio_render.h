#pragma once
#include "../../../utilities/memory/virtual.h"

class c_vector;

class c_studio_render
{
public:
	// 20 = Vector* overload ( 21 Vector4D* ), reversed; checked on studiorender.dll
	void set_ambient_light_colors( const c_vector* colors )
	{
		g_virtual.call< void >( this, 20, colors );
	}

	void set_local_lights( const int count, const void* lights )
	{
		g_virtual.call< void >( this, 22, count, lights );
	}

	void set_color_modulation( float const* color )
	{
		g_virtual.call< void >( this, 27, color );
	}

	void set_alpha_modulation( float alpha )
	{
		g_virtual.call< void >( this, 28, alpha );
	}
};