#pragma once
#include "../../../globals/macros/macros.h"
#include "../classes/c_vector.h"

class c_client_renderable;

struct renderable_info_t {
	c_client_renderable* m_renderable;
	void* m_alpha_property;
	int m_enum_count;
	int m_render_frame;
	unsigned short m_first_shadow;
	unsigned short m_leaft_list;
	short m_area;
	std::uint16_t m_flags;
	std::uint16_t m_flags_alt;
	c_vector m_bloated_absoulte_mins;
	c_vector m_bloated_absoulte_maxs;
	c_vector m_absoulte_mins;
	c_vector m_absoulte_maxs;
	PAD( 0x4 );
};
