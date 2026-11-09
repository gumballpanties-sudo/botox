#pragma once
#include "../../../utilities/memory/virtual.h"

class c_view_setup;
class c_texture;
struct view_matrix_t;

class c_render_view
{
public:
	/* 43/44 = Push3DView ( +depth tex, plain ) reversed, 46 PopView, 56 GetMatricesForView: ret sizes checked on engine.dll.
	   frustum = VPlane[ 6 ] ( 96 bytes ) */
	void push_3d_view( void* context, const c_view_setup* view, const int flags, c_texture* render_target, void* frustum )
	{
		g_virtual.call< void >( this, 44, context, view, flags, render_target, frustum );
	}

	void pop_view( void* context, void* frustum )
	{
		g_virtual.call< void >( this, 46, context, frustum );
	}

	void get_matrices_for_view( const c_view_setup* view, view_matrix_t* world_to_view, view_matrix_t* view_to_projection,
	                            view_matrix_t* world_to_projection, view_matrix_t* world_to_pixels )
	{
		g_virtual.call< void >( this, 56, view, world_to_view, view_to_projection, world_to_projection, world_to_pixels );
	}

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

	void get_color_modulation( float* color )
	{
		g_virtual.call< void >( this, 7, color );
	}

	void set_color_modulation( const float r, const float g, const float b )
	{
		const float color[ 3 ] = { r, g, b };

		this->set_color_modulation( color );
	}
};
