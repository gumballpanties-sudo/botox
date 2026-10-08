#pragma once

class c_vector;
class c_angle;
class c_overlay_text;
class c_color;

struct matrix3x4_t;

class c_debug_overlay
{
public:
	virtual void add_entity_text_overlay( int i_entity_index, int i_line_offset, float fl_duration, int r, int g, int b, int a, const char* fmt,
	                                      ... )                                                                                                = 0;
	virtual void add_box_overlay( const c_vector& vec_origin, const c_vector& vec_abs_min, const c_vector& vec_abs_max,
	                              const c_angle& ang_orientation, int r, int g, int b, int a, float fl_duration )                              = 0;
	virtual void add_sphere_overlay( const c_vector& vec_origin, float fl_radius, int n_theta, int n_phi, int r, int g, int b, int a,
	                                 float fl_duration )                                                                                       = 0;
	virtual void add_triangle_overlay( const c_vector& p1, const c_vector& p2, const c_vector& p3, int r, int g, int b, int a, bool b_no_depth_test,
	                                   float fl_duration )                                                                                     = 0;
	virtual void add_line_overlay( const c_vector& vec_origin, const c_vector& vec_dest, int r, int g, int b, bool b_no_depth_test,
	                               float fl_duration )                                                                                         = 0;
	virtual void add_text_overlay( const c_vector& vec_origin, float fl_duration, const char* fmt, ... )                                       = 0;
	virtual void add_text_overlay( const c_vector& vec_origin, int i_line_offset, float fl_duration, const char* fmt, ... )                    = 0;
	virtual void add_screen_text_overlay( float fl_x_pos, float fl_y_pos, float fl_duration, int r, int g, int b, int a, const char* sz_text ) = 0;
	virtual void add_swept_box_overlay( const c_vector& vec_start, const c_vector& vec_end, const c_vector& vec_min, const c_vector& vec_max,
	                                    const c_angle& angles, int r, int g, int b, int a, float fl_duration )                                 = 0;
	virtual void add_grid_overlay( const c_vector& vec_origin )                                                                                = 0;
	virtual void add_coord_frame_overlay( const matrix3x4_t& mat_frame, float fl_scale, int v_color_table[ 3 ][ 3 ] = nullptr )                = 0;
	virtual int screen_position( const c_vector& vec_point, c_vector& vec_screen )                                                             = 0;
	virtual int screen_position( float fl_x_pos, float fl_y_pos, c_vector& vec_screen )                                                        = 0;
	virtual c_overlay_text* get_first( )                                                                                                       = 0;
	virtual c_overlay_text* get_next( c_overlay_text* p_current )                                                                              = 0;
	virtual void clear_dead_overlays( )                                                                                                        = 0;
	virtual void clear_all_overlays( )                                                                                                         = 0;
	virtual void add_text_overlay_rgb( const c_vector& vec_origin, int i_line_offset, float fl_duration, float r, float g, float b, float a,
	                                   const char* fmt, ... )                                                                                  = 0;
	virtual void add_text_overlay_rgb( const c_vector& vec_origin, int i_line_offset, float fl_duration, int r, int g, int b, int a, const char* fmt,
	                                   ... )                                                                                                   = 0;
	virtual void add_line_overlay_alpha( const c_vector& vec_origin, const c_vector& dest, int r, int g, int b, int a, bool b_no_depth_test,
	                                     float fl_duration )                                                                                   = 0;
	virtual void add_box_overlay2( const c_vector& vec_origin, const c_vector& vec_abs_min, const c_vector& vec_abs_max,
	                               const c_angle& ang_orientation, const c_color& face_color, const c_color& edge_color, float fl_duration )   = 0;
	virtual void add_line_overlay( const c_vector& vec_origin, const c_vector& vec_dest, int r, int g, int b, int a, float fl_thickness,
	                               float fl_duration )                                                                                         = 0;
	virtual void purge_text_overlays( )                                                                                                        = 0;
	virtual void add_capsule_overlay( const c_vector& vec_abs_min, const c_vector& vec_abs_max, const float& fl_radius, int r, int g, int b, int a,
	                                  float fl_duration )                                                                                      = 0;
	virtual void draw_pill( c_vector& vec_abs_min, c_vector& vec_abs_max, float fl_radius, int r, int g, int b, int a, float fl_duration )     = 0;
};
