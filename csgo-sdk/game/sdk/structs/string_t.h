#pragma once

struct string_t {
public:
	bool operator!( ) const
	{
		return ( m_value == nullptr );
	}
	bool operator==( const string_t& rhs ) const
	{
		return ( m_value == rhs.m_value );
	}
	bool operator!=( const string_t& rhs ) const
	{
		return ( m_value != rhs.m_value );
	}
	bool operator<( const string_t& rhs ) const
	{
		return ( reinterpret_cast< void* >( const_cast< char* >( m_value ) ) < reinterpret_cast< void* >( const_cast< char* >( rhs.m_value ) ) );
	}

	const char* c_str( ) const
	{
		return ( m_value ) ? m_value : "";
	}

protected:
	const char* m_value;
};