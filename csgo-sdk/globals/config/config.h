#pragma once
#include <any>
#include <deque>
#include <filesystem>
#include <vector>

#include "../../dependencies/fnv1a/fnv1a.h"
#include "../../utilities/input/key_bind_t.h"

template< typename T, std::size_t S >
inline std::vector< T > get_filled_vector( const T& fill )
{
	std::vector< T > temp( S );
	std::fill( temp.begin( ), temp.begin( ) + S, fill );
	return temp;
}

struct variable_object_t {
	variable_object_t( const unsigned int name_hash, const unsigned int type_hash, std::any&& default_value )
		: m_name_hash( name_hash ), m_type_hash( type_hash ), m_value( std::move( default_value ) ), m_default( m_value )
	{
	}

	~variable_object_t( ) = default;

	/* POINTER any_cast: nullptr on a type mismatch instead of throwing, so a GET_VARIABLE naming the wrong
	   type yields a visibly wrong value, not a crash. */
	template< typename T >
	T& get( )
	{
		if ( T* value = std::any_cast< T >( &m_value ) )
			return *value;

		static T fallback{ };
		return fallback;
	}

	template< typename T >
	void set( T value )
	{
		m_value.emplace< T >( value );
	}

	unsigned int m_name_hash = 0x0;
	unsigned int m_type_hash = 0x0;
	std::any m_value         = { };
	std::any m_default = { };
};

namespace n_config
{
	struct impl_t {
		bool on_attach( );
		/* visuals = only the visuals tab vars (visual_variables.inc), files in C:\botox\visuals */
		bool save( std::string_view file_name, const bool visuals = false );
		bool load( std::string_view file_name, const bool visuals = false );
		void remove( const std::size_t index, const bool visuals = false );
		void refresh( );

		std::size_t get_variable_index( const unsigned int name_hash );

		std::filesystem::path folder( const bool visuals ) const
		{
			return visuals ? m_path / "visuals" : m_path;
		}

		std::deque< std::string >& file_names( const bool visuals )
		{
			return visuals ? m_visual_file_names : m_file_names;
		}

		const std::filesystem::path m_path           = std::string( ( "C:\\botox" ) );
		std::deque< std::string > m_file_names       = { };
		std::deque< std::string > m_visual_file_names = { };
		std::vector< variable_object_t > m_variables = { };

		template< typename T >
		T& get( const std::uint32_t index )
		{
			return m_variables[ index ].get< T >( );
		}

		template< typename T >
		void reset( const std::uint32_t index )
		{
			auto& variable = m_variables[ index ];
			if ( const T* fallback = std::any_cast< T >( &variable.m_default ) )
				variable.get< T >( ) = *fallback;
		}

		template< typename T >
		std::uint32_t add_new_variable( const unsigned int name_hash, const unsigned int type_hash, const T default_value )
		{
			m_variables.emplace_back( name_hash, type_hash, std::make_any< T >( default_value ) );
			return m_variables.size( ) - 1U;
		}
	};
};

inline n_config::impl_t g_config{ };

#define GET_VARIABLE( variable, type ) g_config.get< type >( variable )

#define TO_STRING( func ) #func
#define ADD_VARIABLE( variable_type, variable_name, default_value )                                                                                  \
	const std::uint32_t variable_name =                                                                                                              \
		g_config.add_new_variable< variable_type >( HASH_BT( TO_STRING( variable_name ) ), HASH_BT( TO_STRING( variable_type ) ), default_value );
#define ADD_VARIABLE_VECTOR( variable_type, vector_size, variable_name, default_value )                                                              \
	const std::uint32_t variable_name = g_config.add_new_variable< std::vector< variable_type > >(                                                   \
		HASH_BT( TO_STRING( variable_name ) ), HASH_BT( "std::vector<" TO_STRING( variable_type ) ">" ),                                             \
		get_filled_vector< variable_type, vector_size >( default_value ) );

#define INVALID_VARIABLE ( std::size_t )( -1 )
