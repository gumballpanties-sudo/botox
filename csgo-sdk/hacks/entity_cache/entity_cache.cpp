#include "entity_cache.h"
#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"

std::vector< c_base_entity* > cached_players{ };
std::vector< c_base_entity* > cached_edicts{ };

void n_entity_cache::impl_t::on_add_entity( c_base_entity* entity )
{
	if ( !entity )
		return;

	{
		const auto it_found = std::ranges::find( cached_players, entity );

		if ( it_found == cached_players.cend( ) ) {
			const int index = entity->get_index( );

			if ( index >= 1 && index <= 64 )
				cached_players.push_back( entity );
		}
	}

	{
		const auto it_found = std::ranges::find( cached_edicts, entity );

		if ( it_found == cached_edicts.cend( ) ) {
			const int index = entity->get_index( );

			if ( index >= 1 && index > 64 )
				cached_edicts.push_back( entity );
		}
	}
}

void n_entity_cache::impl_t::on_remove_entity( c_base_entity* entity )
{
	if ( !entity )
		return;

	{
		const auto it_found = std::ranges::find( cached_players, entity );
		if ( it_found != cached_players.cend( ) )
			cached_players.erase( it_found );
	}

	{
		const auto it_found = std::ranges::find( cached_edicts, entity );
		if ( it_found != cached_edicts.cend( ) )
			cached_edicts.erase( it_found );
	}
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
	case e_enumeration_type::type_players: {
		for ( const auto entity : cached_players ) {
			if ( entity )
				function( entity );
		}
		break;
	}
	case e_enumeration_type::type_edicts: {
		for ( const auto entity : cached_edicts ) {
			if ( entity )
				function( entity );
		}
		break;
	}
	}
}