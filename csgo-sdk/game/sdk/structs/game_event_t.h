#pragma once
#include <iostream>

class bf_write;
class bf_read;

struct game_event_t {
	virtual ~game_event_t( )              = 0;
	virtual const char* get_name( ) const = 0;

	virtual bool is_reliable( ) const                       = 0;
	virtual bool is_local( ) const                          = 0;
	virtual bool is_empty( const char* key_name = nullptr ) = 0;

	virtual bool get_bool( const char* key_name = nullptr, bool default_value = false )                 = 0;
	virtual int get_int( const char* key_name = nullptr, int default_value = 0 )                        = 0;
	virtual std::uint64_t get_uint64( const char* key_name = nullptr, unsigned long default_value = 0 ) = 0;
	virtual float get_float( const char* key_name = nullptr, float default_value = 0.0f )               = 0;
	virtual const char* get_string( const char* key_name = nullptr, const char* default_value = "" )    = 0;
	virtual const wchar_t* get_w_string( const char* key_name, const wchar_t* default_value = L"" )     = 0;

	virtual const void* get_ptr( const char* key_name = nullptr )          = 0;

	virtual void set_bool( const char* key_name, bool value )               = 0;
	virtual void set_int( const char* key_name, int value )                 = 0;
	virtual void set_uint64( const char* key_name, unsigned long value )    = 0;
	virtual void set_float( const char* key_name, float value )             = 0;
	virtual void set_string( const char* key_name, const char* value )      = 0;
	virtual void set_w_string( const char* key_name, const wchar_t* value ) = 0;
	virtual void set_ptr( const char* key_name, const void* value )         = 0;
};

class c_game_event_listener
{
public:
	virtual ~c_game_event_listener( void ) { }
	virtual void fire_game_event( game_event_t* event ) = 0;
	int debug_id;
	virtual int get_event_debug_id( void )
	{
		return debug_id;
	};
};

struct game_event_manager_t {
	virtual ~game_event_manager_t( )                                                                = 0;
	virtual int load_events_from_file( const char* filename )                                       = 0;
	virtual void reset( )                                                                           = 0;
	virtual bool add_listener( c_game_event_listener* listener, const char* name, bool serverside ) = 0;
	virtual bool find_listener( c_game_event_listener* listener, const char* name )                 = 0;
	virtual int remove_listener( c_game_event_listener* listener )                                  = 0;
	virtual game_event_t* create_event( const char* name, bool force, unsigned int unk )            = 0;
	virtual bool fire_event( game_event_t* event, bool dont_broadcast = false )                     = 0;
	virtual bool fire_event_client_side( game_event_t* event )                                      = 0;
	virtual game_event_t* duplicate_event( game_event_t* event )                                    = 0;
	virtual void free_event( game_event_t* event )                                                  = 0;
	virtual bool serialize_event( game_event_t* event, bf_write* buf )                              = 0;
	virtual game_event_t* unserialize_event( bf_read* buf )                                         = 0;
};
