#pragma once
#include "../../datatypes/recv_var_proxy_fn.h"

enum e_send_prop_type;

struct recv_table_t;

using array_length_proxy_fn = void( __cdecl* )( void*, int, int );
using data_table_proxy_fn   = void( __cdecl* )( const recv_prop_t*, void**, void*, int );

struct recv_prop_t {
	char* m_var_name;
	e_send_prop_type m_recv_type;
	int m_flags;
	int m_string_buffer_size;
	bool m_inside_array;
	const void* m_extra_data;
	recv_prop_t* m_array_prop;
	array_length_proxy_fn* m_array_length_proxy_fn;
	recv_var_proxy_fn m_proxy_fn;
	data_table_proxy_fn* m_data_table_proxy_fn;
	recv_table_t* m_data_table;
	int m_offset;
	int m_element_stride;
	int m_elements;
	const char* m_parent_array_prop_name;
};