#include "c_vector.h"

struct polyhedron_t;
struct convertconvexparams_t;
struct truncatedcone_t;
struct vcollide_t;

class c_physics_collison
{
public:
	virtual ~c_physics_collison( void ) { }

	// produce a convex element from verts (convex hull around verts)
	virtual void* convex_from_verts( c_vector** p_verts, int vert_count ) = 0;
	// produce a convex element from planes (csg of planes)
	virtual void* convex_from_planes( float* p_planes, int plane_count, float merge_distance ) = 0;
	// calculate volume of a convex element
	virtual float convex_volume( void* p_convex ) = 0;

	virtual float convex_surface_area( void* p_convex ) = 0;
	// store game-specific data in a convex solid
	virtual void set_convex_game_data( void* p_convex, unsigned int game_data ) = 0;
	// if not converted, free the convex elements with this call
	virtual void convex_free( void* p_convex )                                  = 0;
	virtual void* b_box_to_convex( const c_vector& mins, const c_vector& maxs ) = 0;
	// produce a convex element from a convex polyhedron_t
	virtual void* convex_from_convexpolyhedron_t( const polyhedron_t& convexpolyhedron_t ) = 0;
	// produce a set of convex triangles from a convex polygon, normal is assumed to be on the side with forward point ordering, which should be
	// clockwise, output will need to be able to hold exactly (i_point_count-2) convexes
	virtual void convexes_from_convex_polygon( const c_vector& v_poly_normal, const c_vector* p_points, int i_point_count, void** p_output ) = 0;

	// concave objects
	// create a triangle soup
	virtual void* polysoup_create( void ) = 0;
	virtual void polysoup_destroy( void* p_soup ) = 0;
	// add a triangle to the soup
	virtual void polysoup_add_triangle( void* p_soup, const c_vector& a, const c_vector& b, const c_vector& c, int material_index7bits ) = 0;
	// convert the convex into a compiled collision model
	virtual void* convert_polysoup_to_collide( void* p_soup, bool use_mopp ) = 0;

	// convert an array of convex elements to a compiled collision model (this deletes the convex elements)
	virtual void* convert_convex_to_collide( void** p_convex, int convex_count )                                                     = 0;
	virtual void* convert_convex_to_collide_params( void** p_convex, int convex_count, const convertconvexparams_t& convert_params ) = 0;
	// free a collide that was created with convert_convex_to_collide()
	virtual void destroy_collide( void* p_collide ) = 0;

	virtual int collide_size( void* p_collide ) = 0;
	virtual int collide_write( char* p_dest, void* p_collide, bool b_swap = false ) = 0;
	virtual void* unserialize_collide( char* p_buffer, int size, int index ) = 0;

	// compute the volume of a collide
	virtual float collide_volume( void* p_collide ) = 0;
	// compute surface area for tools
	virtual float collide_surface_area( void* p_collide ) = 0;

	// get the support map for a collide in the given direction
	virtual c_vector collide_get_extent( const void* p_collide, const c_vector& collide_origin, const c_vector& collide_angles,
	                                     const c_vector& direction ) = 0;

	// get an aabb for an oriented collision model
	virtual void collide_get_aabb( c_vector* p_mins, c_vector* p_maxs, const void* p_collide, const c_vector& collide_origin,
	                               const c_vector& collide_angles ) = 0;

	virtual void collide_get_mass_center( void* p_collide, c_vector* p_out_mass_center ) = 0;
	virtual void collide_set_mass_center( void* p_collide, const c_vector& mass_center ) = 0;
	virtual c_vector collide_get_orthographic_areas( const void* p_collide )              = 0;
	virtual void collide_set_orthographic_areas( void* p_collide, const c_vector& areas ) = 0;

	// query the vcollide index in the physics model for the instance
	virtual int collide_index( const void* p_collide ) = 0;

	// convert a bbox to a collide
	virtual void* b_box_to_collide( const c_vector& mins, const c_vector& maxs )                                               = 0;
	virtual int get_convexes_used_in_collideable( const void* p_collideable, void** p_output_array, int i_output_array_limit ) = 0;

	// trace an aabb against a collide
	virtual void pad_1( ) = 0;
	virtual void pad_2( ) = 0;
	virtual void pad_3( ) = 0;

	// trace one collide against another
	virtual void trace_collide( const c_vector& start, const c_vector& end, const void* p_sweep_collide, const c_vector& sweep_angles,
	                            const void* p_collide, const c_vector& collide_origin, const c_vector& collide_angles, void* ptr ) = 0;

	// relatively slow test for box vs. truncated cone
	virtual bool is_box_intersecting_cone( const c_vector& box_abs_mins, const c_vector& box_abs_maxs, const truncatedcone_t& cone ) = 0;

	// loads a set of solids into a vcollide_t
	virtual void v_collide_load( vcollide_t* p_output, int solid_count, const char* p_buffer, int size, bool swap = false ) = 0;
	// destroyts the set of solids created by v_collide_load
	virtual void v_collide_unload( vcollide_t* p_v_collide ) = 0;
};