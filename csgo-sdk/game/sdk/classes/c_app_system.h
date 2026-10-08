#pragma once

typedef void* ( *create_interface_t )( const char* name, int* return_code );
typedef void* ( *instantiate_interface_t )( );

class c_app_system
{
public:
	virtual bool connect( create_interface_t factory )                               = 0; // 0
	virtual void disconnect( )                                                       = 0; // 1
	virtual void* query_interface( const char* interface_name )                      = 0; // 2
	virtual int init( )                                                              = 0; // 3
	virtual void shutdown( )                                                         = 0; // 4
	virtual const void* get_dependencies( )                                          = 0; // 5
	virtual int get_tier( )                                                          = 0; // 6
	virtual void reconnect( create_interface_t factory, const char* interface_name ) = 0; // 7
private:
	virtual void unk_fn( ) = 0; // 8
};
