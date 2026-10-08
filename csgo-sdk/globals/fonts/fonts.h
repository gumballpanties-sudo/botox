#pragma once
#include <string>
#include <vector>

struct font_file_t {
	std::string m_display_name{ };
	std::string m_full_path{ };
};

namespace n_fonts
{
	struct impl_t {
		void on_attach( );

		std::string find_path( const std::string& display_name );

		std::vector< font_file_t > m_font_files{ };
		std::vector< std::string > m_font_file_names{ };
	};
}

inline n_fonts::impl_t g_fonts{ };
