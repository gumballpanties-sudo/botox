#pragma once

class c_base_entity;
class c_vector;
class c_move_data;

class c_game_movement
{
public:
	virtual ~c_game_movement( ) { }
	virtual void process_movement( c_base_entity* entity, c_move_data* p_move ) = 0;
	virtual void reset( )                                                       = 0;
	virtual void start_track_prediction_errors( c_base_entity* entity )         = 0;
	virtual void finish_track_prediction_errors( c_base_entity* entity )        = 0;
	virtual void diff_print( char const* fmt, ... )                             = 0;
	virtual c_vector const& get_player_mins( bool ducked ) const                = 0;
	virtual c_vector const& get_player_ma( bool ducked ) const                  = 0;
	virtual c_vector const& get_player_view_offset( bool ducked ) const         = 0;
	virtual bool is_moving_player_stuck( ) const                                = 0;
	virtual c_base_entity* get_moving_player( ) const                           = 0;
	virtual void unblock_pusher( c_base_entity* entity, c_base_entity* pusher ) = 0;
	virtual void setup_movement_bounds( c_move_data* move )                     = 0;
};