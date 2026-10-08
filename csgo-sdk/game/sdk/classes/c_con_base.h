#pragma once

class c_con_base
{
public:
	void* m_vmt;
	c_con_base* m_next;
	bool m_registered;
	const char* m_name;
	const char* m_help_string;
	int m_flags;
	c_con_base* m_conbases;
	void* m_accessor;
};