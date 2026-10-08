#pragma once
#include "../classes/c_vector.h"
#include "matrix_t.h"

struct mstudiobone_t {
	int m_name_index;
	int m_parent;
	int m_bone_controller[ 6 ];

	c_vector m_position;
	float m_world[ 4 ];
	float m_rotation[ 3 ];

	c_vector m_position_scale;
	c_vector m_euler_scale;

	matrix3x4_t m_pose_to_bone;
	float m_alignment[ 4 ];
	int m_flags;
	int m_procedure_type;
	int m_procedure_index;
	mutable int m_physics_bone;
	int m_surface_prop_index;
	int m_contents;
	int m_surface_prop;
	unsigned char pad0[ 0x1c ];

	inline const char* get_name( )
	{
		if ( !m_name_index )
			return nullptr;

		return ( char* )this + m_name_index;
	}
};
