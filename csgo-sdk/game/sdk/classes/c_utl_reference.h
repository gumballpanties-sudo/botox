#pragma once

template< typename T >
struct c_utl_reference {
	c_utl_reference* m_next;
	c_utl_reference* m_prev;
	T* m_obj;
};
