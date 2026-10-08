#pragma once
#include "../classes/c_angle.h"
#include "../classes/c_vector.h"

struct mstudiobbox_t {
	int m_bone;
	int m_group;
	c_vector m_bb_min;
	c_vector m_bb_max;
	int m_hitbox_name_index;
	c_angle m_offset_orientation;
	float m_radius;
	unsigned char pad0[ 0x10 ];

	inline const char* get_hitbox_name( ) const
	{
		if ( !m_hitbox_name_index )
			return nullptr;
		return ( const char* )this + m_hitbox_name_index;
	}
};

struct mstudiohitboxset_t {
	int m_name_index;
	int m_hit_boxes;
	int m_hit_box_index;

	inline const char* get_name( ) const
	{
		if ( !m_name_index )
			return nullptr;
		return ( char* )this + m_name_index;
	}

	inline mstudiobbox_t* get_hitbox( int hitbox ) const
	{
		if ( hitbox < 0 || hitbox >= m_hit_boxes )
			return nullptr;

		return ( mstudiobbox_t* )( ( unsigned char* )this + m_hit_box_index ) + hitbox;
	}
};