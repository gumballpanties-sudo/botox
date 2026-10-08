#pragma once
#include "../classes/c_vector.h"
#include "mstudiohitboxset_t.h"

struct mstudiobone_t;

struct mstudioseqdesc_t {
	int m_base_ptr;
	int m_label_index;
	int m_activity_name_index;

	unsigned char pad[ 0xd4 - 0xc ];

	inline const char* get_label( ) const
	{
		return ( const char* )this + m_label_index;
	}
};

struct studiohdr_t {
	int n_id;
	int i_version;
	int i_checksum;
	char sz_name[ 64 ];
	int i_length;

	c_vector vec_eye_position;
	c_vector vec_illum_position;
	c_vector vec_hull_min;
	c_vector vec_hull_max;
	c_vector vec_view_bb_min;
	c_vector vec_view_bb_max;

	int i_flags;

	int n_bones;
	int n_bone_index;
	inline mstudiobone_t* get_bone( int i_bone ) const
	{
		if ( i_bone < 0 || i_bone >= n_bones )
			return nullptr;
		return ( mstudiobone_t* )( ( unsigned char* )this + n_bone_index ) + i_bone;
	}

	int n_bone_controllers;
	int n_bone_controller_index;
	inline void* get_bone_controller( int i_bone_controller ) const
	{
		if ( i_bone_controller < 0 || i_bone_controller >= n_bone_controllers )
			return nullptr;
		return ( ( unsigned char* )this + n_bone_controller_index ) + i_bone_controller;
	}

	int n_hitbox_sets;
	int n_hitbox_set_index;
	inline mstudiohitboxset_t* get_hitbox_set( int i_set ) const
	{
		if ( i_set < 0 || i_set >= n_hitbox_sets )
			return nullptr;
		return ( mstudiohitboxset_t* )( ( unsigned char* )this + n_hitbox_set_index ) + i_set;
	}

	inline mstudiobbox_t* get_hitbox( int i_hit_box, int i_set ) const
	{
		const mstudiohitboxset_t* p_hitbox_set = get_hitbox_set( i_set );

		if ( !p_hitbox_set )
			return nullptr;

		return p_hitbox_set->get_hitbox( i_hit_box );
	}

	inline int get_hitbox_count( int i_set ) const
	{
		const mstudiohitboxset_t* p_hitbox_set = get_hitbox_set( i_set );

		if ( !p_hitbox_set )
			return 0;

		return p_hitbox_set->m_hit_boxes;
	}

	int n_local_animations;
	int n_local_animation_index;
	inline void* get_anim_description( int i_animation ) const
	{
		if ( i_animation < 0 || i_animation >= n_local_animations )
			i_animation = 0;
		return ( ( unsigned char* )this + n_local_animation_index ) + i_animation;
	}

	int n_local_sequences;
	int n_local_sequence_index;
	inline mstudioseqdesc_t* get_sequence( int i_sequence ) const
	{
		if ( i_sequence < 0 || i_sequence >= n_local_sequences )
			return nullptr;
		return ( mstudioseqdesc_t* )( ( unsigned char* )this + n_local_sequence_index ) + i_sequence;
	}

	mutable int m_activity_list_version;
	mutable int m_events_indexed;

	int m_textures;
	int m_texture_index;
	inline void* get_texture( int n_index ) const
	{
		if ( n_index < 0 || n_index >= m_textures )
			return nullptr;
		return ( ( unsigned char* )this + m_texture_index ) + n_index;
	}

	int m_cd_textures;
	int m_cd_texture_index;
	inline char* get_cd_texture( int n_index ) const
	{
		if ( n_index < 0 || n_index >= m_cd_textures )
			return nullptr;
		return ( ( ( char* )this ) + *( ( int* )( ( unsigned char* )this + m_cd_texture_index ) + n_index ) );
	}

	int m_skin_refs;
	int m_skin_families;
	int m_skin_index;
	inline short* get_skin_ref( int n_index ) const
	{
		if ( n_index < 0 || n_index >= m_skin_refs )
			return nullptr;
		return ( short* )( ( unsigned char* )this + m_skin_index ) + n_index;
	}

	int m_body_parts;
	int m_body_part_index;
	inline void* get_body_part( int n_index ) const
	{
		if ( n_index < 0 || n_index >= m_body_parts )
			return nullptr;
		return ( ( unsigned char* )this + m_body_part_index ) + n_index;
	}

	int m_attachments;
	int m_attachment_index;
	inline void* get_attachment( int n_index ) const
	{
		if ( n_index < 0 || n_index >= m_attachments )
			return nullptr;
		return ( ( unsigned char* )this + m_attachment_index ) + n_index;
	}

	int m_transitions;
	int m_transition_index;
	int m_transition_name_index;
	inline unsigned char* get_transition( int n_index ) const
	{
		if ( n_index < 0 || n_index >= ( m_transitions * m_transitions ) )
			return nullptr;
		return ( unsigned char* )( ( unsigned char* )this + m_transition_index ) + n_index;
	}

	int m_flex_descs;
	int m_flex_desc_index;
	inline void* get_flex_description( int n_index ) const
	{
		if ( n_index < 0 || n_index >= m_flex_descs )
			return nullptr;
		return ( ( unsigned char* )this + m_flex_desc_index ) + n_index;
	}

	int m_flex_controllers;
	int m_flex_controller_index;
	inline void* get_flex_controller( int n_index ) const
	{
		if ( n_index < 0 || n_index >= m_flex_controllers )
			return nullptr;
		return ( ( unsigned char* )this + m_flex_controller_index ) + n_index;
	}

	int m_flex_rules;
	int m_flex_rule_index;
	inline void* get_flex_rule( int n_index ) const
	{
		if ( n_index < 0 || n_index >= m_flex_rules )
			return nullptr;
		return ( ( unsigned char* )this + m_flex_rule_index ) + n_index;
	}

	int m_ik_chains;
	int m_ik_chain_index;
	inline void* get_ik_chain( int n_index ) const
	{
		if ( n_index < 0 || n_index >= m_ik_chains )
			return nullptr;
		return ( ( unsigned char* )this + m_ik_chain_index ) + n_index;
	}

	int m_mouths;
	int m_mouth_index;
	inline void* get_mouth( int n_index ) const
	{
		if ( n_index < 0 || n_index >= m_mouths )
			return nullptr;
		return ( ( unsigned char* )this + m_mouth_index ) + n_index;
	}

	int m_pose_parameters;
	int m_pose_parameter_index;
	inline void* get_pose_parameter( int n_index ) const
	{
		if ( n_index < 0 || n_index >= m_pose_parameters )
			return nullptr;
		return ( ( unsigned char* )this + m_pose_parameter_index ) + n_index;
	}

	int m_surface_prop_index;
	inline const char* get_surface_prop( ) const
	{
		if ( !m_surface_prop_index )
			return nullptr;
		return ( char* )this + m_surface_prop_index;
	}

	int m_key_value_index;
	int m_key_value_size;
	inline const char* key_value_text( ) const
	{
		if ( !m_key_value_size )
			return nullptr;
		return ( char* )this + m_key_value_index;
	}

	int m_ik_autoplay_locks;
	int m_ik_autoplay_lock_index;
	inline void* get_ik_autoplay_lock( int n_index ) const
	{
		if ( n_index < 0 || n_index >= m_ik_autoplay_locks )
			return nullptr;
		return ( ( unsigned char* )this + m_ik_autoplay_lock_index ) + n_index;
	}

	float m_mass;
	int m_contents;

	int m_include_models;
	int m_include_model_index;
	inline void* get_model_group( int n_index ) const
	{
		if ( n_index < 0 || n_index >= m_include_models )
			return nullptr;
		return ( ( unsigned char* )this + m_include_model_index ) + n_index;
	}

	int m_virtual_model;

	int m_anim_block_name_index;
	inline const char* get_anim_block_name( ) const
	{
		return ( ( char* )this ) + m_anim_block_name_index;
	}

	int m_anim_blocks;
	int m_anim_block_index;
	inline void* get_anim_block( int n_index ) const
	{
		if ( n_index < 0 || n_index >= m_anim_blocks )
			return nullptr;
		return ( ( ( unsigned char* )this ) + m_anim_block_index ) + n_index;
	}

	int m_anim_block_model;

	int m_bone_table_by_name_index;
	inline const unsigned char* get_bone_table_sorted_by_name( ) const
	{
		return ( unsigned char* )this + m_bone_table_by_name_index;
	}

	int m_vertex_base;
	int m_index_base;

	unsigned char m_directional_light_dot;
	unsigned char m_root_lod;
	unsigned char m_allowed_root_lo_ds;
	unsigned char pad0[ 0x5 ];

	int m_flex_controller_ui;
	int m_flexcontroller_ui_index;

	float m_vert_anim_fixed_point_scale;
	mutable int m_surface_prop_lookup;
	int m_studio_hdr2_index;

	unsigned char pad1[ 0x4 ];
};
