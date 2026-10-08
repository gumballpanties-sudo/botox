#pragma once

class c_net_channel;
class bf_read;
class bf_write;

class c_net_message
{
public:
	virtual ~c_net_message( ) { }
	virtual void set_net_channel( void* net_channel ) = 0;
	virtual void set_reliable( bool state )           = 0;
	virtual bool process( )                           = 0;
	virtual bool read_from_buffer( bf_read& buffer )  = 0;
	virtual bool write_to_buffer( bf_write& buffer )  = 0;
	virtual bool is_reliable( ) const                 = 0;
	virtual int get_type( ) const                     = 0;
	virtual int get_group( ) const                    = 0;
	virtual const char* get_name( ) const             = 0;
	virtual c_net_channel* get_net_channel( ) const   = 0;
	virtual const char* to_string( ) const            = 0;
	virtual unsigned int get_size( ) const            = 0;
};