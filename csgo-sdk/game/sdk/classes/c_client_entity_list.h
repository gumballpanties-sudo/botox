#pragma once

class c_base_entity;

class c_client_networkable;
class c_client_unknown;
class c_client_entity;

class c_client_entity_list
{
public:
	virtual c_client_networkable* get_client_networkable( int index )                            = 0;
	virtual c_client_networkable* get_client_networkable_from_handle( unsigned int networkable ) = 0;
	virtual c_client_unknown* get_client_unknown_from_handle( unsigned int unknown )             = 0;
	virtual c_client_entity* get_client_entity( int index )                                      = 0;
	virtual c_client_entity* get_client_entity_from_handle( unsigned int entity )                = 0;
	virtual int number_of_entities( bool include_non_networkable )                               = 0;
	virtual int get_highest_entity_index( )                                                      = 0;
	virtual void set_max_entities( int max )                                                     = 0;
	virtual int get_max_entities( )                                                              = 0;

	template< class t = c_client_entity >
	inline t* get( const int index )
	{
		return reinterpret_cast< t* >( get_client_entity( index ) );
	}

	template< class t = c_client_entity >
	inline t* get( const unsigned int h_entity )
	{
		return reinterpret_cast< t* >( get_client_entity_from_handle( h_entity ) );
	}
};
