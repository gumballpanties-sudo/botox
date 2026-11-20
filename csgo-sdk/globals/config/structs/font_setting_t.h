#pragma once
#include <string>

struct font_setting_t {
	std::string m_name{ };
	int m_size{ };

	bool operator==( const font_setting_t& ) const = default;
};