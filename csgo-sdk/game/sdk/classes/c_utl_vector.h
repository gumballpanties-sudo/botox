#pragma once
#include "c_utl_memory.h"

template< class T >
void destruct( T* m_memory )
{
	m_memory->~T( );
}

template< class T >
T* construct( T* m_memory )
{
	return new ( m_memory ) T;
}

template< class T >
T* copy( T* m_memory, T const& src )
{
	return new ( m_memory ) T( src );
}

template< class T, class A = c_utl_memory< T > >
class c_utl_vector
{
	using c_allocator = A;

public:
	auto begin( ) noexcept
	{
		return m_memory.base( );
	}

	auto end( ) noexcept
	{
		return m_memory.base( ) + m_size;
	}

	auto begin( ) const noexcept
	{
		return m_memory.base( );
	}

	auto end( ) const noexcept
	{
		return m_memory.base( ) + m_size;
	}

	T& operator[]( int i )
	{
		return m_memory[ i ];
	}

	const T& operator[]( int i ) const
	{
		return m_memory[ i ];
	}

	T& element( int i )
	{
		return m_memory[ i ];
	}

	T* get_elements( )
	{
		return this->m_elements;
	}

	const T& element( int i ) const
	{
		return m_memory[ i ];
	}

	T* base( )
	{
		return m_memory.base( );
	}

	int count( ) const
	{
		return m_size;
	}

	int& size( )
	{
		return m_size;
	}

	void remove_all( )
	{
		for ( int i = m_size; --i >= 0; ) {
			destruct< T >( &element( i ) );
		}

		m_size = 0;
	}

protected:
	c_allocator m_memory;
	int m_size;
	T* m_elements;
};
