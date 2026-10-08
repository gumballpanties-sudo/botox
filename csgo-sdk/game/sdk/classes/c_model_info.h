#pragma once
#include "c_vector.h"

struct vcollide_t;
struct studiohdr_t;
struct virtualmodel_t;
struct cplane_t;

class c_material;
class c_utl_buffer;
class c_client_renderable;
class c_game_trace;
class c_angle;
class c_phys_collide;

struct model_t {
	unsigned char pad0[ 0x4 ];
	char m_name[ 260 ];
	int m_load_flags;
	int m_server_count;
	int m_type;
	int m_flags;
	c_vector m_mins;
	c_vector m_maxs;
	float m_radius;
	unsigned char pad1[ 0x1C ];
};

class c_model_info
{
public:
	virtual ~c_model_info( ) { }
	virtual const model_t* get_model( int nModelIndex ) const                                        = 0;
	virtual int get_model_index( const char* sz_name ) const                                             = 0;
	virtual const char* get_model_name( const model_t* p_model ) const                                   = 0;
	virtual vcollide_t* get_v_collide( const model_t* p_model ) const                                    = 0;
	virtual vcollide_t* get_v_collide( int n_model_index ) const                                         = 0;
	virtual vcollide_t* get_v_collide( const model_t* p_model, float fl_scale ) const                    = 0;
	virtual vcollide_t* get_v_collide( int n_model_index, float fl_scale ) const                         = 0;
	virtual void get_model_bounds( const model_t* p_model, c_vector& mins, c_vector& maxs ) const        = 0;
	virtual void get_model_render_bounds( const model_t* p_model, c_vector& mins, c_vector& maxs ) const = 0;
	virtual int get_model_frame_count( const model_t* p_model ) const                                    = 0;
	virtual int get_model_type( const model_t* p_model ) const                                           = 0;
	virtual void* get_model_extra_data( const model_t* p_model )                                         = 0;
	virtual bool model_has_material_proxy( const model_t* p_model ) const                                = 0;
	virtual bool is_translucent( model_t const* p_model ) const                                          = 0;
	virtual bool is_translucent_two_pass( const model_t* p_model ) const                                 = 0;

private:
	virtual void unused0( ) { }

public:
	virtual int compute_translucency_type( const model_t* p_model, int n_skin, int n_body )                                            = 0;
	virtual int get_model_material_count( const model_t* p_model ) const                                                               = 0;
	virtual void get_model_materials( const model_t* p_model, int i_count, c_material** pp_materials )                                 = 0;
	virtual bool is_model_vertex_lit( const model_t* p_model ) const                                                                   = 0;
	virtual const char* get_model_key_value_text( const model_t* p_model )                                                             = 0;
	virtual bool get_model_key_value( const model_t* p_model, c_utl_buffer& buf )                                                      = 0;
	virtual float get_model_radius( const model_t* p_model )                                                                           = 0;
	virtual const studiohdr_t* find_model( const studiohdr_t* p_studio_hdr, void** pp_cache, const char* sz_model_name ) const         = 0;
	virtual const studiohdr_t* find_model( void* p_cache ) const                                                                       = 0;
	virtual virtualmodel_t* get_virtual_model( const studiohdr_t* p_studio_hdr ) const                                                 = 0;
	virtual std::uint8_t* get_anim_block( const studiohdr_t* p_studio_hdr, int i_block ) const                                         = 0;
	virtual bool has_anim_block_been_preloaded( studiohdr_t const* p_studio_hdr, int i_block ) const                                   = 0;
	virtual void get_model_material_color_and_lighting( const model_t* p_model, const c_vector& vec_origin, const c_angle& angles,
	                                                    c_game_trace* p_trace, c_vector& vec_lighting, c_vector& mat_color )           = 0;
	virtual void get_illumination_point( const model_t* p_model, c_client_renderable* p_renderable, const c_vector& vec_origin, const c_angle& angles,
	                                     c_vector* p_lighting_center )                                                                 = 0;
	virtual int get_model_contents( int n_model_index ) const                                                                          = 0;
	virtual studiohdr_t* get_studio_model( const model_t* p_model )                                                                    = 0;
	virtual int get_model_sprite_width( const model_t* p_model ) const                                                                 = 0;
	virtual int get_model_sprite_height( const model_t* p_model ) const                                                                = 0;
	virtual void set_level_screen_fade_range( float fl_min_size, float fl_max_size )                                                   = 0;
	virtual void get_level_screen_fade_range( float* p_min_area, float* p_max_area ) const                                             = 0;
	virtual void set_view_screen_fade_range( float fl_min_size, float fl_max_size )                                                    = 0;
	virtual unsigned char compute_level_screen_fade( const c_vector& vec_abs_origin, float fl_radius, float fl_fade_scale ) const      = 0;
	virtual unsigned char compute_view_screen_fade( const c_vector& vec_abs_origin, float fl_radius, float fl_fade_scale ) const       = 0;
	virtual int get_autoplay_list( const studiohdr_t* p_studio_hdr, unsigned short** p_autoplay_list ) const                           = 0;
	virtual c_phys_collide* get_collide_for_virtual_terrain( int n_index )                                                             = 0;
	virtual bool is_using_fb_texture( const model_t* p_model, int n_skin, int n_body, c_client_renderable* p_client_renderable ) const = 0;
	virtual const model_t* find_or_load_model( const char* sz_name ) const                                                             = 0;
	virtual unsigned short get_cache_handle( const model_t* p_model ) const                                                            = 0;
	virtual int get_brush_model_plane_count( const model_t* p_model ) const                                                            = 0;
	virtual void get_brush_model_plane( const model_t* p_model, int n_index, cplane_t& plane, c_vector* p_origin ) const               = 0;
	virtual int get_surfaceprops_for_virtual_terrain( int n_index )                                                                    = 0;
	virtual bool uses_env_cubemap( const model_t* p_model ) const                                                                      = 0;
	virtual bool uses_static_lighting( const model_t* p_model ) const                                                                  = 0;
	virtual int register_dynamic_model( const char* name, bool b_client_side )                                                         = 0;
	virtual int register_combined_dynamic_model( const char* psz_name, unsigned short handle )                                         = 0;
	virtual void update_combined_dynamic_model( int n_model_index, unsigned short handle )                                             = 0;
	virtual int begin_combined_model( const char* psz_name, bool b_reuse_existing )                                                    = 0;

	virtual bool set_combine_models( int n_model_index, const void* p_models_to_combine )                                              = 0;
	virtual bool finish_combined_model( int n_model_index, void* p_callback, void* p_user_data )                                       = 0;
	virtual void release_combined_model( int n_model_index )                                                                           = 0;
	virtual bool is_dynamic_model_loading( int n_model_index )                                                                         = 0;
	virtual void add_ref_dynamic_model( int n_model_index )                                                                            = 0;
	virtual void release_dynamic_model( int n_model_index )                                                                            = 0;
};