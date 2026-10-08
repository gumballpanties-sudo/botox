#pragma once
#include "../classes/c_utl_vector.h"
#include "../classes/c_vector.h"

#include <functional>

enum e_trace_type : int {
	trace_type_everything = 0,
	trace_type_world_only,
	trace_type_entities_only,
	trace_type_everything_filter_props,
	trace_type_filtersky
};

enum e_contents {
	contents_empty                = 0x0,
	contents_solid                = 0x1,
	contents_window               = 0x2,
	contents_aux                  = 0x4,
	contents_grate                = 0x8,
	contents_slime                = 0x10,
	contents_water                = 0x20,
	contents_blocklos             = 0x40,
	contents_opaque               = 0x80,
	contents_testfogvolume        = 0x100,
	contents_unused               = 0x200,
	contents_blocklight           = 0x400,
	contents_team1                = 0x800,
	contents_team2                = 0x1000,
	contents_ignore_nodraw_opaque = 0x2000,
	contents_moveable             = 0x4000,
	contents_areaportal           = 0x8000,
	contents_playerclip           = 0x10000,
	contents_monsterclip          = 0x20000,
	contents_current0             = 0x40000,
	contents_current90            = 0x80000,
	contents_current180           = 0x100000,
	contents_current270           = 0x200000,
	contents_current_up           = 0x400000,
	contents_current_down         = 0x800000,
	contents_origin               = 0x1000000,
	contents_monster              = 0x2000000,
	contents_debris               = 0x4000000,
	contents_detail               = 0x8000000,
	contents_translucent          = 0x10000000,
	contents_ladder               = 0x20000000,
	contents_hitbox               = 0x40000000,

	last_visible_contents = contents_opaque,
	all_visible_contents  = last_visible_contents | last_visible_contents - 1
};

enum e_mask {
	mask_all                   = 0XFFFFFFFF,
	mask_solid                 = ( contents_solid | contents_moveable | contents_window | contents_monster | contents_grate ),
	mask_playersolid           = ( contents_solid | contents_moveable | contents_playerclip | contents_window | contents_monster | contents_grate ),
	mask_npcsolid              = ( contents_solid | contents_moveable | contents_monsterclip | contents_window | contents_monster | contents_grate ),
	mask_npcfluid              = ( contents_solid | contents_moveable | contents_monsterclip | contents_window | contents_monster ),
	mask_water                 = ( contents_water | contents_moveable | contents_slime ),
	mask_opaque                = ( contents_solid | contents_moveable | contents_opaque ),
	mask_opaque_npc            = ( mask_opaque | contents_monster ),
	mask_blocklos              = ( contents_solid | contents_moveable | contents_slime ),
	mask_blocklos_npc          = ( mask_blocklos | contents_monster ),
	mask_visible               = ( mask_opaque | contents_ignore_nodraw_opaque ),
	mask_visible_npc           = ( mask_opaque_npc | contents_ignore_nodraw_opaque ),
	mask_shot                  = ( contents_solid | contents_moveable | contents_monster | contents_window | contents_debris | contents_hitbox ),
	mask_shot_brushonly        = ( contents_solid | contents_moveable | contents_window | contents_debris ),
	mask_shot_hull             = ( contents_solid | contents_moveable | contents_monster | contents_window | contents_debris | contents_grate ),
	mask_shot_player           = ( mask_shot_hull | contents_hitbox ),
	mask_shot_portal           = ( contents_solid | contents_moveable | contents_window | contents_monster ),
	mask_solid_brushonly       = ( contents_solid | contents_moveable | contents_window | contents_grate ),
	mask_playersolid_brushonly = ( contents_solid | contents_moveable | contents_window | contents_playerclip | contents_grate ),
	mask_npcsolid_brushonly    = ( contents_solid | contents_moveable | contents_window | contents_monsterclip | contents_grate ),
	mask_npcworldstatic        = ( contents_solid | contents_window | contents_monsterclip | contents_grate ),
	mask_npcworldstatic_fluid  = ( contents_solid | contents_window | contents_monsterclip ),
	mask_splitareaportal       = ( contents_water | contents_slime ),
	mask_current =
		( contents_current0 | contents_current90 | contents_current180 | contents_current270 | contents_current_up | contents_current_down ),
	mask_deadsolid = ( contents_solid | contents_playerclip | contents_window | contents_grate )
};

enum e_surf_type {
	surf_light     = 0x0001,
	surf_sky2d     = 0x0002,
	surf_sky       = 0x0004,
	surf_warp      = 0x0008,
	surf_trans     = 0x0010,
	surf_noportal  = 0x0020,
	surf_trigger   = 0x0040,
	surf_nodraw    = 0x0080,
	surf_hint      = 0x0100,
	surf_skip      = 0x0200,
	surf_nolight   = 0x0400,
	surf_bumplight = 0x0800,
	surf_noshadows = 0x1000,
	surf_nodecals  = 0x2000,
	surf_nochop    = 0x4000,
	surf_hitbox    = 0x8000
};

enum e_debug_trace_counter_behavior : int {
	trace_counter_set = 0,
	trace_counter_inc,
};

struct brush_side_info_t {
	c_vector_aligned m_plane;
	unsigned short m_bevel;
	unsigned short m_thin;
};

struct cplane_t {
	c_vector m_normal;
	float m_distance;
	unsigned char m_type;
	unsigned char m_sign_bits;
	unsigned char pad[ 0x2 ];
};

struct csurface_t {
	const char* m_name;
	short m_surface_props;
	unsigned short m_flags;
};

class c_base_trace
{
public:
	c_base_trace( ) { }

	c_vector m_start;
	c_vector m_end;
	cplane_t m_plane;
	float m_fraction;
	int m_contents;
	std::uint16_t m_disp_flags;
	bool m_all_solid;
	bool m_start_solid;
};

class c_base_entity;
class c_game_trace : public c_base_trace
{
public:
	c_game_trace( ) : m_hit_entity( nullptr ) { }

	float m_fraction_left_solid;
	csurface_t surface;
	int m_hit_group;
	short m_physics_bone;
	std::uint16_t m_world_surface_index;
	c_base_entity* m_hit_entity;
	int m_hitbox;

	inline bool did_hit( ) const
	{
		return ( m_fraction < 1.0f || m_all_solid || m_start_solid );
	}

	inline bool is_visible( ) const
	{
		return ( m_fraction > 0.97f );
	}

	c_game_trace( const c_game_trace& other )
	{
		this->m_start               = other.m_start;
		this->m_end                 = other.m_end;
		this->m_plane               = other.m_plane;
		this->m_fraction            = other.m_fraction;
		this->m_contents            = other.m_contents;
		this->m_disp_flags          = other.m_disp_flags;
		this->m_all_solid           = other.m_all_solid;
		this->m_start_solid         = other.m_start_solid;
		this->m_fraction_left_solid = other.m_fraction_left_solid;
		this->surface               = other.surface;
		this->m_hit_group           = other.m_hit_group;
		this->m_physics_bone        = other.m_physics_bone;
		this->m_world_surface_index = other.m_world_surface_index;
		this->m_hit_entity          = other.m_hit_entity;
		this->m_hitbox              = other.m_hitbox;
	}
};

struct matrix3x4_t;

using trace_t = c_game_trace;

struct ray_t {
	ray_t( const c_vector& start_position, const c_vector& end_position )
		: m_start( start_position ), m_delta( end_position - start_position ), m_world_axis_transform( nullptr ), m_is_ray( true )
	{
		this->m_is_swept = ( this->m_delta.length_squared( ) != 0.f );
	}

	ray_t( const c_vector& start_position, const c_vector& end_position, const c_vector& mins, const c_vector& maxs )
	{
		this->m_delta = end_position - start_position;

		this->m_world_axis_transform = nullptr;
		this->m_is_swept             = ( this->m_delta.length_squared( ) != 0.f );

		this->m_extents = maxs - mins;
		this->m_extents *= 0.5f;
		this->m_is_ray = ( this->m_extents.length_squared( ) < 1e-6f );

		this->m_start_offset = mins + maxs;
		this->m_start_offset *= 0.5f;
		this->m_start = start_position + this->m_start_offset;
		this->m_start_offset *= -1.0f;
	}

	c_vector_aligned m_start;
	c_vector_aligned m_delta;
	c_vector_aligned m_start_offset;
	c_vector_aligned m_extents;
	const matrix3x4_t* m_world_axis_transform;
	bool m_is_ray;
	bool m_is_swept;
};

class i_trace_filter
{
public:
	virtual bool should_hit_entity( c_base_entity* entity, int contents_mask ) = 0;
	virtual e_trace_type get_trace_type( ) const                               = 0;
};

class c_trace_filter : public i_trace_filter
{
	using filter_callback_function = std::function< bool( c_base_entity*, int ) >;

public:
	c_trace_filter( const c_base_entity* skip_entity, e_trace_type trace_type = e_trace_type::trace_type_everything )
		: m_skip( skip_entity ), m_trace_type( trace_type )
	{
	}

	c_trace_filter( filter_callback_function&& check_callback, e_trace_type trace_type = e_trace_type::trace_type_everything )
		: m_check_callback( std::move( check_callback ) ), m_trace_type( trace_type )
	{
	}

	bool should_hit_entity( c_base_entity* handle_entity, int fContentsMask ) override
	{
		if ( m_check_callback )
			return m_check_callback( handle_entity, fContentsMask );

		return handle_entity != m_skip;
	}

	e_trace_type get_trace_type( ) const override
	{
		return m_trace_type;
	}

private:
	const c_base_entity* m_skip               = nullptr;
	filter_callback_function m_check_callback = nullptr;
	e_trace_type m_trace_type                 = e_trace_type::trace_type_everything;
};

/* world-geometry-only filter: rejects every entity, engine walks brushes only. */
class c_trace_filter_world : public i_trace_filter
{
public:
	bool should_hit_entity( c_base_entity* handle_entity, int contents_mask ) override
	{
		return false;
	}

	e_trace_type get_trace_type( ) const override
	{
		return e_trace_type::trace_type_world_only;
	}
};

class c_trace_filter_trace_type_everything_filter_props : public i_trace_filter
{
public:
	bool should_hit_entity( c_base_entity* handle_entity, int contents_mask ) override
	{
		return false;
	}

	e_trace_type get_trace_type( ) const override
	{
		return e_trace_type::trace_type_everything;
	}
};

class i_trace_list_data
{
public:
	virtual ~i_trace_list_data( ) { }
	virtual void reset( )                          = 0;
	virtual bool is_empty( )                       = 0;
	virtual bool can_trace_ray( const ray_t& ray ) = 0;
};

class i_entity_enumerator
{
public:
	virtual bool enum_entity( c_base_entity* handle_entity ) = 0;
};

struct virtualmeshlist_t;
struct aabb_t;
class c_collideable;
class c_phys_collide;
class c_brush_query;
class c_engine_trace
{
public:
	virtual int get_point_contents( const c_vector& vec_abs_position, int f_contents_mask = e_mask::mask_all,
	                                c_base_entity** pp_entity = nullptr )                                                                    = 0;
	virtual int get_point_contents_world_only( const c_vector& vec_abs_position, int f_contents_mask = e_mask::mask_all )                    = 0;
	virtual int get_point_contents_collideable( c_collideable* p_collide, const c_vector& vec_abs_position )                                 = 0;
	virtual void clip_ray_to_entity( const ray_t& ray, unsigned int f_mask, c_base_entity* p_entity, trace_t* p_trace )                      = 0;
	virtual void clip_ray_to_collideable( const ray_t& ray, unsigned int f_mask, c_collideable* p_collide, trace_t* p_trace )                = 0;
	virtual void trace_ray( const ray_t& ray, unsigned int f_mask, i_trace_filter* p_trace_filter, trace_t* p_trace )                        = 0;
	virtual void setup_leaf_and_entity_list_ray( const ray_t& ray, i_trace_list_data* p_trace_data )                                         = 0;
	virtual void setup_leaf_and_entity_list_box( const c_vector& vec_box_min, const c_vector& vec_box_max, i_trace_list_data* p_trace_data ) = 0;
	virtual void trace_ray_against_leaf_and_entity_list( const ray_t& ray, i_trace_list_data* p_trace_data, unsigned int f_mask,
	                                                     i_trace_filter* p_trace_filter, trace_t* p_trace )                                  = 0;
	virtual void sweep_collideable( c_collideable* p_collide, const c_vector& vec_abs_start, const c_vector& vec_abs_end, const c_vector& vec_angles,
	                                unsigned int f_mask, i_trace_filter* p_trace_filter, trace_t* p_trace )                                  = 0;
	virtual void enumerate_entities( const ray_t& ray, bool b_triggers, i_entity_enumerator* p_enumerator )                                  = 0;
	virtual void enumerate_entities( const c_vector& vec_abs_mins, const c_vector& vec_abs_maxs, i_entity_enumerator* p_enumerator )         = 0;
	virtual c_collideable* get_collideable( c_base_entity* p_entity )                                                                        = 0;
	virtual int get_stat_by_index( int n_index, bool b_clear )                                                                               = 0;
	virtual void get_brushes_in_aabb( const c_vector& vec_mins, const c_vector& vec_maxs, c_utl_vector< int >* p_output,
	                                  int f_contents_mask = mask_all )                                                                       = 0;
	virtual c_phys_collide* get_collidable_from_displacements_in_aabb( const c_vector& vec_mins, const c_vector& vec_maxs )                  = 0;
	virtual int get_num_displacements( )                                                                                                     = 0;
	virtual void get_displacement_mesh( int n_index, virtualmeshlist_t* p_mesh_tri_list )                                                    = 0;
	virtual bool get_brush_info( int i_brush, c_utl_vector< brush_side_info_t >* p_brush_side_info_out, int* p_contents_out )                = 0;
	virtual bool point_outside_world( const c_vector& vec_point )                                                                            = 0;
	virtual int get_leaf_containing_point( const c_vector& vec_point )                                                                       = 0;
	virtual i_trace_list_data* alloc_trace_list_data( )                                                                                      = 0;
	virtual void free_trace_list_data( i_trace_list_data* p_list_data )                                                                      = 0;
	virtual int get_set_debug_trace_counter( int i_value, e_debug_trace_counter_behavior behavior )                                          = 0;
	virtual int get_meshes_from_displacements_in_aabb( const c_vector& vec_mins, const c_vector& vec_maxs, virtualmeshlist_t* p_output_meshes,
	                                                   int n_max_output_meshes )                                                             = 0;
	virtual void get_brushes_in_collideable( c_collideable* p_collideable, c_brush_query& brush_query )                                      = 0;
	virtual bool is_fully_occluded( int n_occlusion_key, const aabb_t& aabb1, const aabb_t& aabb2, const c_vector& vec_shadow )              = 0;
	virtual void suspend_occlusion_tests( )                                                                                                  = 0;
	virtual void resume_occlusion_tests( )                                                                                                   = 0;
	virtual void flush_occlusion_queries( )                                                                                                  = 0;
};
