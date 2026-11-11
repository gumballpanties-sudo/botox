#pragma once
#include "../../../utilities/memory/virtual.h"
#include <cstdint>

class c_vector;

// StudioRenderContext_t forced material block ( studiorendercontext.h ); type 3 = selective = weapon skin composites
struct studio_material_override_t {
	void* m_materials[ 4 ];
	int m_type;
	int m_indices[ 4 ];
	int m_count;
};
static_assert( sizeof( studio_material_override_t ) == 0x28 );

class c_studio_render
{
public:
	studio_material_override_t& material_override( )
	{
		return *reinterpret_cast< studio_material_override_t* >( reinterpret_cast< std::uintptr_t >( this ) + 0x250 );
	}

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