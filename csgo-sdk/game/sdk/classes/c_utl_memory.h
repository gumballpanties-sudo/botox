#pragma once

template< class T, class N = int >
class c_utl_memory
{
public:
	T& operator[]( N i )
	{
		return m_memory[ i ];
	}

	const T& operator[]( N i ) const
	{
		return m_memory[ i ];
	}

	T* base( )
	{
		return m_memory;
	}

	[[nodiscard]] int num_allocated( ) const
	{
		return m_allocation_count;
	}

	[[nodiscard]] bool is_externally_allocated( ) const
	{
		return m_grow_size < 0;
	}

	void grow( const int count = 1 )
	{
		if ( is_externally_allocated( ) )
			return;

		int allocation_requested = m_allocation_count + count;
		int new_allocation_count = 0;

		if ( m_grow_size )
			m_allocation_count = ( ( 1 + ( ( allocation_requested - 1 ) / m_grow_size ) ) * m_grow_size );
		else {
			if ( !m_allocation_count )
				m_allocation_count = ( 31 + sizeof( T ) ) / sizeof( T );

			while ( m_allocation_count < allocation_requested )
				m_allocation_count <<= 1;
		}

		if ( static_cast< int >( static_cast< N >( new_allocation_count ) ) < allocation_requested ) {
			if ( static_cast< int >( static_cast< N >( new_allocation_count ) ) == 0 &&
			     static_cast< int >( static_cast< N >( new_allocation_count - 1 ) ) >= allocation_requested )
				--new_allocation_count;
			else {
				if ( static_cast< int >( static_cast< N >( allocation_requested ) ) != allocation_requested )
					return;

				while ( static_cast< int >( static_cast< N >( new_allocation_count ) ) < allocation_requested )
					new_allocation_count = ( new_allocation_count + allocation_requested ) / 2;
			}
		}

		m_allocation_count = new_allocation_count;

		if ( m_memory )
			m_memory = static_cast< T* >( realloc( m_memory, m_allocation_count * sizeof( T ) ) );
		else
			m_memory = static_cast< T* >( malloc( m_allocation_count * sizeof( T ) ) );
	}

protected:
	T* m_memory;
	int m_allocation_count;
	int m_grow_size;
};

template< class T, class M = c_utl_memory< T > >
class c_utl_stack
{
public:
	M m_memory;
	int m_size;
	T* m_elements;
};
