#pragma once

enum e_field_types;

struct data_map_t;
struct type_description_t {
public:
	e_field_types m_field_type;
	const char* m_field_name;
	int m_field_offset[ 2 ];
	unsigned short m_field_size;
	short m_flags;
	char pad0[ 0xC ];
	data_map_t* m_type_description;
	char pad1[ 0x18 ];
};