#pragma once
#include "../../../utilities/memory/virtual.h"

class c_texture;

class c_material_var
{
public:
	// imaterialvar.h: GetTextureValue is the first virtual ( 1 = IsTextureValueInternalEnvCubemap )
	c_texture* get_texture( )
	{
		return g_virtual.call< c_texture* >( this, 0 );
	}

	void set_float( float value )
	{
		g_virtual.call< void >( this, 4, value );
	}

	void set_int( int value )
	{
		g_virtual.call< void >( this, 5, value );
	}

	void set_string( const char* value )
	{
		g_virtual.call< void >( this, 6, value );
	}

	void set_vector( float x, float y )
	{
		g_virtual.call< void >( this, 10, x, y );
	}

	void set_vector( float x, float y, float z )
	{
		g_virtual.call< void >( this, 11, x, y, z );
	}

	void set_texture( c_texture* texture )
	{
		g_virtual.call< void >( this, 15, texture );
	}

	// VMatrix = float[ 4 ][ 4 ] row major
	void set_matrix( const float* matrix )
	{
		g_virtual.call< void >( this, 20, matrix );
	}

	bool matrix_is_identity( )
	{
		return g_virtual.call< bool >( this, 22 );
	}

	void set_vector_component( float value, int component )
	{
		g_virtual.call< void >( this, 26, value, component );
	}
};