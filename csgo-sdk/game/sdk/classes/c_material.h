#pragma once

class c_material_var;
class c_vector;
class c_key_values;

enum e_preview_image_ret_val;
enum e_image_format;
enum e_material_var_flags;
enum e_material_property_type;

class c_material
{
public:
	virtual const char* get_name( ) const                                                                                              = 0;
	virtual const char* get_texture_group_name( ) const                                                                                = 0;
	virtual e_preview_image_ret_val get_preview_image_properties( int* width, int* height, e_image_format* image_format,
	                                                              bool* translucent ) const                                            = 0;
	virtual e_preview_image_ret_val get_preview_image( unsigned char* data, int width, int height, e_image_format image_format ) const = 0;
	virtual int get_mapping_width( )                                                                                                   = 0;
	virtual int get_mapping_height( )                                                                                                  = 0;
	virtual int get_num_animation_frames( )                                                                                            = 0;
	virtual bool in_material_page( )                                                                                                   = 0;
	virtual void get_material_offset( float* offset )                                                                                  = 0;
	virtual void get_material_scale( float* scale )                                                                                    = 0;
	virtual c_material* get_material_page( )                                                                                           = 0;
	virtual c_material_var* find_var( const char* name, bool* found, bool complain = true )                                            = 0;
	virtual void increment_reference_count( )                                                                                          = 0;
	virtual void decrement_reference_count( )                                                                                          = 0;
	inline void add_reference( )
	{
		increment_reference_count( );
	}
	inline void release( )
	{
		decrement_reference_count( );
	}
	virtual int get_enumeration_id( ) const                                                          = 0;
	virtual void get_low_res_color_sample( float s, float t, float* color ) const                    = 0;
	virtual void recompute_state_snapshots( )                                                        = 0;
	virtual bool is_translucent( )                                                                   = 0;
	virtual bool is_alpha_tested( )                                                                  = 0;
	virtual bool is_vertex_lit( )                                                                    = 0;
	virtual unsigned long long get_vertex_format( ) const                                            = 0;
	virtual bool has_proxy( ) const                                                                  = 0;
	virtual bool uses_env_cubemap( )                                                                 = 0;
	virtual bool needs_tangent_space( )                                                              = 0;
	virtual bool needs_power_of_two_frame_buffer_texture( bool check_specific_to_this_frame = true ) = 0;
	virtual bool needs_full_frame_buffer_texture( bool check_specific_to_this_frame = true )         = 0;
	virtual bool needs_software_skinning( )                                                          = 0;
	virtual void alpha_modulate( float alpha )                                                       = 0;
	virtual void color_modulate( float r, float g, float b )                                         = 0;
	virtual void set_material_var_flag( e_material_var_flags flag, bool enable )                     = 0;
	virtual bool get_material_var_flag( e_material_var_flags flag )                                  = 0;
	virtual void get_reflectivity( c_vector& reflect )                                               = 0;
	virtual bool get_property_flag( e_material_property_type type )                                  = 0;
	virtual bool is_two_sided( )                                                                     = 0;
	virtual void set_shader( const char* shader_name )                                               = 0;
	virtual int get_num_passes( )                                                                    = 0;
	virtual int get_texture_memory_bytes( )                                                          = 0;
	virtual void refresh( )                                                                          = 0;
	virtual bool needs_lightmap_blend_alpha( )                                                       = 0;
	virtual bool needs_software_lighting( )                                                          = 0;
	virtual int shader_param_count( ) const                                                          = 0;
	virtual c_material_var** get_shader_params( )                                                    = 0;
	virtual bool is_error_material( ) const                                                          = 0;
	virtual void set_use_fixed_function_baked_lighting( bool enable )                                = 0;
	virtual float get_alpha_modulation( )                                                            = 0;
	virtual void get_color_modulation( float* r, float* g, float* b )                                = 0;
	virtual bool is_translucent_under_modulation( float alpha_modulation = 1.0f ) const              = 0;
	virtual c_material_var* find_var_fast( char const* name, unsigned int* token )                   = 0;
	virtual void set_shader_and_params( c_key_values* key_values )                                   = 0;
	virtual const char* get_shader_name( ) const                                                     = 0;
	virtual void delete_if_unreferenced( )                                                           = 0;
	virtual bool is_sprite_card( )                                                                   = 0;
	virtual void call_bind_proxy( void* proxy_data )                                                 = 0;
	virtual void refresh_preserving_material_vars( )                                                 = 0;
	virtual bool was_reloaded_from_whitelist( )                                                      = 0;
	virtual bool set_temp_excluded( bool set, int excluded_dimension_limit )                         = 0;
	virtual int get_reference_count( ) const                                                         = 0;
};