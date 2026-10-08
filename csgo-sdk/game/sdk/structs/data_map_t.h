#pragma once

struct type_description_t;

struct data_map_t {
	type_description_t* m_data_desc;
	int m_data_fields;
	const char* m_data_class_name;
	data_map_t* m_base_map;
	bool m_chains_validated;
	bool m_packed_offsets_computed;
	int m_packed_size;
};