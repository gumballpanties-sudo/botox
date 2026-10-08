#pragma once
#include <cstddef>
#include <functional>

class c_base_entity;

enum e_enumeration_type {
	type_none = 0,
	type_players,
	type_edicts,
	type_max
};

namespace n_entity_cache
{
	struct impl_t {
		void on_add_entity( c_base_entity* entity );
		void on_remove_entity( c_base_entity* entity );

		void reset( );

		// what enumerate would walk, for the leak watch line
		[[nodiscard]] std::size_t count( e_enumeration_type type ) const;

		void enumerate( e_enumeration_type type, const std::function< void( c_base_entity* ) >& function ) const;
	};
}

inline n_entity_cache::impl_t g_entity_cache{ };