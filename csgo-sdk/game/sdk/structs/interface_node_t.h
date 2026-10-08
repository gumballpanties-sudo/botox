#pragma once

struct interface_node_t {
	void*( __cdecl* m_create_fn )( );
	const char* m_name;
	interface_node_t* m_next;
};