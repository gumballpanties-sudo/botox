#pragma once

class c_client_networkable;

struct recv_table_t;

enum class e_class_ids;

using create_client_class_fn = c_client_networkable*( __cdecl* )( int, int );
using create_event_fn        = c_client_networkable*( __cdecl* )( );

class c_base_client
{
public:
	create_client_class_fn m_create_fn;
	create_event_fn m_create_event_fn;
	char* m_network_name;
	recv_table_t* m_recv_table;
	c_base_client* m_next;
	e_class_ids m_class_id;
};