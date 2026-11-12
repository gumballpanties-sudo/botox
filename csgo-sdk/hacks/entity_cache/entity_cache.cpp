#include "entity_cache.h"
#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"

/* 11-12 20:32 crash: spectator list read m_hObserverTarget off a cached player ptr whose pages were gone ( freed without a remove we saw ).
   index rides with the ptr: enumerate hands out only ptrs the live entity list still holds at that index, no deref of a stale one */
struct cached_t {
	c_base_entity* m_entity;
	int m_index;
};

std::vector< cached_t > cached_players{ };
std::vector< cached_t > cached_edicts{ };

namespace
{
	bool has( const std::vector< cached_t >& v, const c_base_entity* e )
	{
		return std::ranges::find( v, e, &cached_t::m_entity ) != v.cend( );
	}

	void drop( std::vector< cached_t >& v, const c_base_entity* e )
	{
		if ( const auto it = std::ranges::find( v, e, &cached_t::m_entity ); it != v.cend( ) )
			v.erase( it );
	}

	void walk( const std::vector< cached_t >& v, const std::function< void( c_base_entity* ) >& function )
	{
		// index loop: a push_back from inside function( ) ( realloc ) leaves this walk valid
		for ( std::size_t i = 0; i < v.size( ); ++i ) {
			const cached_t c = v[ i ];
			if ( c.m_entity && reinterpret_cast< c_base_entity* >( g_interfaces.m_client_entity_list->get_client_entity( c.m_index ) ) == c.m_entity )
				function( c.m_entity );
		}
	}
}

void n_entity_cache::impl_t::on_add_entity( c_base_entity* entity )
{
	if ( !entity )
		return;

	const int index = entity->get_index( );

	if ( index >= 1 && index <= 64 ) {
		if ( !has( cached_players, entity ) )
			cached_players.push_back( { entity, index } );
	} else if ( index > 64 ) {
		if ( !has( cached_edicts, entity ) )
			cached_edicts.push_back( { entity, index } );
	}
}

void n_entity_cache::impl_t::on_remove_entity( c_base_entity* entity )
{
	if ( !entity )
		return;

	drop( cached_players, entity );
	drop( cached_edicts, entity );
}

void n_entity_cache::impl_t::reset( )
{
	cached_players.clear( );
	cached_edicts.clear( );
}

std::size_t n_entity_cache::impl_t::count( const e_enumeration_type type ) const
{
	switch ( type ) {
	case e_enumeration_type::type_players:
		return cached_players.size( );
	case e_enumeration_type::type_edicts:
		return cached_edicts.size( );
	default:
		return 0U;
	}
}

void n_entity_cache::impl_t::enumerate( e_enumeration_type type, const std::function< void( c_base_entity* ) >& function ) const
{
	switch ( type ) {
	case e_enumeration_type::type_players:
		walk( cached_players, function );
		break;
	case e_enumeration_type::type_edicts:
		walk( cached_edicts, function );
		break;
	}
}
