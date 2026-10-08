#pragma once

struct c_utl_string {
	char* m_buffer;
	int m_capacity;
	int m_grow_size;
	int m_length;
};