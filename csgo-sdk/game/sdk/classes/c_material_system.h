#pragma once
#include "../../../utilities/memory/virtual.h"

#define TEXTURE_GROUP_LIGHTMAP                    "Lightmaps"
#define TEXTURE_GROUP_WORLD                       "World textures"
#define TEXTURE_GROUP_MODEL                       "Model textures"
#define TEXTURE_GROUP_VGUI                        "VGUI textures"
#define TEXTURE_GROUP_PARTICLE                    "Particle textures"
#define TEXTURE_GROUP_DECAL                       "Decal textures"
#define TEXTURE_GROUP_SKYBOX                      "SkyBox textures"
#define TEXTURE_GROUP_CLIENT_EFFECTS              "ClientEffect textures"
#define TEXTURE_GROUP_OTHER                       "Other textures"
#define TEXTURE_GROUP_PRECACHED                   "Precached"
#define TEXTURE_GROUP_CUBE_MAP                    "CubeMap textures"
#define TEXTURE_GROUP_RENDER_TARGET               "RenderTargets"
#define TEXTURE_GROUP_UNACCOUNTED                 "Unaccounted textures"
#define TEXTURE_GROUP_STATIC_VERTEX_BUFFER        "Static Vertex"
#define TEXTURE_GROUP_STATIC_INDEX_BUFFER         "Static Indices"
#define TEXTURE_GROUP_STATIC_VERTEX_BUFFER_DISP   "Displacement Verts"
#define TEXTURE_GROUP_STATIC_VERTEX_BUFFER_COLOR  "Lighting Verts"
#define TEXTURE_GROUP_STATIC_VERTEX_BUFFER_WORLD  "World Verts"
#define TEXTURE_GROUP_STATIC_VERTEX_BUFFER_MODELS "Model Verts"
#define TEXTURE_GROUP_STATIC_VERTEX_BUFFER_OTHER  "Other Verts"
#define TEXTURE_GROUP_DYNAMIC_INDEX_BUFFER        "Dynamic Indices"
#define TEXTURE_GROUP_DYNAMIC_VERTEX_BUFFER       "Dynamic Verts"
#define TEXTURE_GROUP_DEPTH_BUFFER                "DepthBuffer"
#define TEXTURE_GROUP_VIEW_MODEL                  "ViewModel"
#define TEXTURE_GROUP_PIXEL_SHADERS               "Pixel Shaders"
#define TEXTURE_GROUP_VERTEX_SHADERS              "Vertex Shaders"
#define TEXTURE_GROUP_RENDER_TARGET_SURFACE       "RenderTarget Surfaces"
#define TEXTURE_GROUP_MORPH_TARGETS               "Morph Targets"

#define TEXTURE_SKYBOX_BAGGAGE     "cs_baggage_skybox_"
#define TEXTURE_SKYBOX_TIBET       "cs_tibet"
#define TEXTURE_SKYBOX_EMBASSY     "embassy"
#define TEXTURE_SKYBOX_ITALY       "italy"
#define TEXTURE_SKYBOX_JUNGLE      "jungle"
#define TEXTURE_SKYBOX_NUKEBLANK   "nukeblank"
#define TEXTURE_SKYBOX_OFFICE      "office"
#define TEXTURE_SKYBOX_DAYLIGHT01  "sky_cs15_daylight01_hdr"
#define TEXTURE_SKYBOX_DAYLIGHT02  "sky_cs15_daylight02_hdr"
#define TEXTURE_SKYBOX_DAYLIGHT03  "sky_cs15_daylight03_hdr"
#define TEXTURE_SKYBOX_DAYLIGHT04  "sky_cs15_daylight04_hdr"
#define TEXTURE_SKYBOX_CLOUDY01    "sky_csgo_cloudy01"
#define TEXTURE_SKYBOX_NIGHT02     "sky_csgo_night02"
#define TEXTURE_SKYBOX_NIGHT02B    "sky_csgo_night02b"
#define TEXTURE_SKYBOX_DUST        "sky_dust"
#define TEXTURE_SKYBOX_VENICE      "sky_venice"
#define TEXTURE_SKYBOX_VERTIGO     "vertigo"
#define TEXTURE_SKYBOX_VERTIGO_HDR "vertigoblue_hdr"
#define TEXTURE_SKYBOX_VIETNAM     "vietnam"

#include "../structs/shader_stencil_state_t.h"

class c_material;
class c_material_var;
class c_key_values;

struct material_render_target_depth_t;
struct rect_t;

class c_vtftexture;
class c_texture;

struct async_texture_context_t {
	c_texture* m_texture;
	unsigned int m_internal_flags;
	int m_desired_temp_dimension_limit;
	int m_actual_dimension_limit;
	c_vtftexture* m_vtf_texture;
};

class c_texture_regenerator
{
public:
	virtual void regenerate_texture_bytes( c_texture* texture, c_vtftexture* vtf_texture, rect_t* rect ) = 0;

	virtual void release( ) = 0;

	virtual bool has_preallocated_scratch_texture( ) const
	{
		return false;
	}

	virtual c_vtftexture* get_preallocated_scratch_texture( )
	{
		return NULL;
	}
};

class c_texture
{
public:
	virtual const char* get_name( void ) const                                     = 0;
	virtual int get_mapping_width( ) const                                         = 0;
	virtual int get_mapping_height( ) const                                        = 0;
	virtual int get_actual_width( ) const                                          = 0;
	virtual int get_actual_height( ) const                                         = 0;
	virtual int get_num_animation_frames( ) const                                  = 0;
	virtual bool is_translucent( ) const                                           = 0;
	virtual bool is_mipmapped( ) const                                             = 0;
	virtual void get_low_res_color_sample( float s, float t, float* color ) const  = 0;
	virtual void* get_resource_data( uint32_t data_type, size_t* num_bytes ) const = 0;
	virtual void increment_reference_count( void )                                 = 0;
	virtual void decrement_reference_count( void )                                 = 0;
	inline void add_ref( )
	{
		increment_reference_count( );
	}
	inline void release( )
	{
		decrement_reference_count( );
	}
	virtual void set_texture_regenerator( c_texture_regenerator* texture_regen, bool release_existing = true )                            = 0;
	virtual void download( rect_t* rect = 0, int additional_creation_flags = 0 )                                                          = 0;
	virtual int get_approximate_vid_bytes( void ) const                                                                                   = 0;
	virtual bool is_error( ) const                                                                                                        = 0;
	virtual bool is_volume_texture( ) const                                                                                               = 0;
	virtual int get_mapping_depth( ) const                                                                                                = 0;
	virtual int get_actual_depth( ) const                                                                                                 = 0;
	virtual void* get_image_format( ) const                                                                                               = 0;
	virtual bool is_render_target( ) const                                                                                                = 0;
	virtual bool is_cube_map( ) const                                                                                                     = 0;
	virtual bool is_normal_map( ) const                                                                                                   = 0;
	virtual bool is_procedural( ) const                                                                                                   = 0;
	virtual bool is_default_pool( ) const                                                                                                 = 0;
	virtual void delete_if_unreferenced( )                                                                                                = 0;
	virtual void swap_contents( c_texture* other )                                                                                        = 0;
	virtual unsigned int get_flags( void ) const                                                                                          = 0;
	virtual void force_lod_override( int num_lods_override_up_or_down )                                                                   = 0;
	virtual void force_exclude_override( int exclude_override )                                                                           = 0;
	virtual void add_downsized_sub_target( const char* name, int downsize_pow2, material_render_target_depth_t depth )                    = 0;
	virtual void set_actuve_sub_target( const char* name )                                                                                = 0;
	virtual int get_reference_count( ) const                                                                                              = 0;
	virtual bool is_temp_excluded( ) const                                                                                                = 0;
	virtual bool can_be_temp_excluded( ) const                                                                                            = 0;
	virtual bool finish_async_download( async_texture_context_t* context, void* data, int num_read_bytes, bool abort, float max_time_ms ) = 0;
	virtual bool is_force_excluded( ) const                                                                                               = 0;
	virtual bool clear_force_exclusion( )                                                                                                 = 0;
};

class c_call_queue
{
public:
	void queue_functor_internal( void* functor )
	{
		return g_virtual.call< void >( this, 0, functor );
	}
};

class c_material_render_context
{
public:
	void get_render_target_dimensions( int* width, int* height )
	{
		return g_virtual.call< void >( this, 8, width, height );
	}

	void draw_screen_space_rectangle( c_material* material, int dest_x, int dest_y, int width, int height, float texture_x0, float texture_y0,
	                                  float texture_x1, float texture_y1, int texture_width, int texture_height, void* client_renderable = nullptr,
	                                  int x_dice = 1, int y_dice = 1 )
	{
		return g_virtual.call< void >( this, 114, material, dest_x, dest_y, width, height, texture_x0, texture_y0, texture_x1, texture_y1,
		                               texture_width, texture_height, client_renderable, x_dice, y_dice );
	}

	void get_view_port( int* x, int* y, int* width, int* height )
	{
		return g_virtual.call< void >( this, 41, x, y, width, height );
	}

	void copy_render_target_to_texture_ex( c_texture* texture, int unk, rect_t* dst, rect_t* src )
	{
		return g_virtual.call< void >( this, 122, texture, unk, dst, src );
	}

	void set_frame_buffer_copy_texture( c_texture* texture, int texture_index )
	{
		return g_virtual.call< void >( this, 20, texture, texture_index );
	}

	void set_render_target( c_texture* texture )
	{
		return g_virtual.call< void >( this, 6, texture );
	}

	void clear_buffers( bool clear_color, bool clear_depth, bool clear_stencil = false )
	{
		return g_virtual.call< void >( this, 12, clear_color, clear_depth, clear_stencil );
	}

	void set_view_port( int x, int y, int width, int height )
	{
		return g_virtual.call< void >( this, 40, x, y, width, height );
	}

	void clear_color_4ub( unsigned char r, unsigned char g, unsigned char b, unsigned char a )
	{
		return g_virtual.call< void >( this, 79, r, g, b, a );
	}

	// the no-argument overload: saves the current target + viewport so pop puts them back
	void push_render_target_and_viewport( )
	{
		return g_virtual.call< void >( this, 116 );
	}

	void pop_render_target_and_viewport( )
	{
		return g_virtual.call< void >( this, 120 );
	}

	void set_stencil_state( const shader_stencil_state_t& state )
	{
		return g_virtual.call< void >( this, 128, &state );
	}

	c_call_queue* get_call_queue( )
	{
		return g_virtual.call< c_call_queue* >( this, 151 );
	}
};

class c_material_system
{
public:
	c_material* create_material( const char* name, c_key_values* key_values )
	{
		return g_virtual.call< c_material* >( this, 83, name, key_values );
	}

	c_material* find_material( char const* material_name, const char* texture_group_name = TEXTURE_GROUP_MODEL, bool complain = true,
	                           const char* complain_prefix = nullptr )
	{
		return g_virtual.call< c_material* >( this, 84, material_name, texture_group_name, complain, complain_prefix );
	}

	unsigned short first_material( )
	{
		return g_virtual.call< unsigned short >( this, 86 );
	}

	unsigned short next_material( unsigned short handle )
	{
		return g_virtual.call< unsigned short >( this, 87, handle );
	}

	unsigned short invalid_material( )
	{
		return g_virtual.call< unsigned short >( this, 88 );
	}

	c_material* get_material( unsigned short handle )
	{
		return g_virtual.call< c_material* >( this, 89, handle );
	}

	int get_num_materials( )
	{
		return g_virtual.call< int >( this, 90 );
	}

	c_texture* find_texture( char const* texture_name, const char* texture_group_name, bool complain = true, int additional_creation_flags = 0 )
	{
		return g_virtual.call< c_texture* >( this, 91, texture_name, texture_group_name, complain, additional_creation_flags );
	}

	c_material_render_context* get_render_context( )
	{
		return g_virtual.call< c_material_render_context* >( this, 115 );
	}
};