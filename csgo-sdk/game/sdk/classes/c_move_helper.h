#pragma once

class c_base_entity;
class c_game_trace;
class c_vector;

class c_move_helper
{
public:
	virtual const char* get_name( void* entity ) const                                                                           = 0;
	virtual void set_host( c_base_entity* host )                                                                                 = 0;
	virtual void reset_touch_list( )                                                                                             = 0;
	virtual bool add_to_touched( const c_game_trace& trace, const c_vector& impact_velocity )                                    = 0;
	virtual void process_impacts( )                                                                                              = 0;
	virtual void con_n_printf( int n_index, char const* fmt, ... )                                                               = 0;
	virtual void start_sound( const c_vector& vorigin, int channel, char const* sample, float volume, int soundlevel, int f_flags,
	                          int i_pitch )                                                                                      = 0;
	virtual void start_sound( const c_vector& origin, const char* soundname )                                                    = 0;
	virtual void playback_event_full( int flags, int client_index, unsigned short event_index, float delay, c_vector& origin, c_vector& angles,
	                                  float fl_param1, float fl_param2, int i_param1, int i_param2, int b_param1, int b_param2 ) = 0;
	virtual bool player_falling_damage( )                                                                                        = 0;
	virtual void player_set_animation( int player_animation )                                                                    = 0;
	virtual void* get_surface_props( )                                                                                           = 0;
	virtual bool is_world_entity( const unsigned long& entity )                                                                  = 0;
};