#include "netvars.h"
#include "../../game/sdk/includes/includes.h"
#include "../includes/includes.h"

#include <unordered_map>

std::unordered_map< unsigned int, n_netvars::impl_t::netvar_object_t > props = { };

bool n_netvars::impl_t::on_attach( )
{
	props.clear( );

	for ( auto client_class = g_interfaces.m_base_client->get_all_classes( ); client_class; client_class = client_class->m_next ) {
		if ( !client_class->m_recv_table )
			continue;

		this->store_props( client_class->m_network_name, client_class->m_recv_table, 0U );
	}

	return !( props.empty( ) );
}

void n_netvars::impl_t::store_props( const char* class_name, recv_table_t* recv_table, const unsigned int offset )
{
	const unsigned int class_hash      = fnv1a::hash( class_name );
	const unsigned int split_hash      = fnv1a::hash( "->", class_hash );
	const unsigned int base_class_hash = fnv1a::hash( "baseclass", split_hash );

	for ( int i = { }; i < recv_table->m_props_count; ++i ) {
		const auto current_prop = &recv_table->m_props[ i ];
		if ( !current_prop )
			continue;

		const unsigned int total_hash = fnv1a::hash( current_prop->m_var_name, split_hash );
		if ( total_hash == base_class_hash )
			continue;

		if ( isdigit( current_prop->m_var_name[ 0 ] ) )
			continue;

		if ( const auto child_table = current_prop->m_data_table; child_table && child_table->m_props_count > 0 &&
		                                                          child_table->m_net_table_name[ 0 ] == 'D' &&
		                                                          current_prop->m_recv_type == e_send_prop_type::dpt_datatable ) {
			this->store_props( class_name, child_table, static_cast< unsigned int >( current_prop->m_offset ) + offset );
		}

		const unsigned int total_offset = static_cast< unsigned int >( current_prop->m_offset ) + offset;
		if ( !props[ total_hash ].m_offset )
			props[ total_hash ] = { current_prop, total_offset };
	}
}

unsigned int n_netvars::impl_t::find_in_data_map( data_map_t* map, const unsigned int field_hash )
{
	while ( map ) {
		for ( int i = 0; i < map->m_data_fields; i++ ) {
			if ( !map->m_data_desc[ i ].m_field_name )
				continue;

			if ( fnv1a::hash( map->m_data_desc[ i ].m_field_name ) == field_hash )
				return map->m_data_desc[ i ].m_field_offset[ 0 ];

			if ( map->m_data_desc[ i ].m_field_type == e_field_types::field_embedded ) {
				if ( map->m_data_desc[ i ].m_type_description ) {
					if ( const auto offset = this->find_in_data_map( map->m_data_desc[ i ].m_type_description, field_hash ); offset != 0U )
						return offset;
				}
			}
		}

		map = map->m_base_map;
	}

	return 0U;
}

n_netvars::impl_t::netvar_object_t n_netvars::impl_t::operator[]( unsigned int hash )
{
	return props[ hash ];
}