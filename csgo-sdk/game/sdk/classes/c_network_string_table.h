#pragma once

using table_id_t = int;

class c_network_string_table
{
public:
	virtual ~c_network_string_table( void ) { }

	virtual const char* get_table_name( void ) const                                                    = 0;
	virtual table_id_t get_table_id( void ) const                                                       = 0;
	virtual int get_num_strings( void ) const                                                           = 0;
	virtual int get_max_strings( void ) const                                                           = 0;
	virtual int get_entry_bits( void ) const                                                            = 0;
	virtual void set_tick( int tick )                                                                   = 0;
	virtual bool changed_since_tick( int tick ) const                                                   = 0;
	virtual int add_string( bool b_is_server, const char* value, int length = -1, const void* userdata = nullptr ) = 0;
	virtual const char* get_string( int string_number ) const                                           = 0;
	virtual void set_string_user_data( int string_number, int length, const void* userdata )             = 0;
	virtual const void* get_string_user_data( int string_number, int* length ) const                     = 0;
	virtual int find_string_index( const char* string )                                                  = 0;
	virtual void set_string_changed_callback( void* object, void* change_func )                          = 0;
};

class c_network_string_table_container
{
public:
	virtual ~c_network_string_table_container( void ) { }

	virtual c_network_string_table* create_string_table( const char* table_name, int max_entries, int userdata_fixed_size = 0,
	                                                     int userdata_network_bits = 0, int flags = 0 ) = 0;
	virtual void remove_all_tables( void )                                                              = 0;
	virtual c_network_string_table* find_table( const char* table_name ) const                          = 0;
	virtual c_network_string_table* get_table( table_id_t string_table ) const                          = 0;
	virtual int get_num_tables( void ) const                                                            = 0;
	virtual void set_allow_client_side_add_string( c_network_string_table* table, bool b_allow )         = 0;
	virtual void create_dictionary( const char* pch_map_name )                                          = 0;
};
