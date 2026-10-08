#pragma once
// credits patoke

#define DEFAULT_BUF_SIZE 256

struct fmt_str_n_t {
	fmt_str_n_t( )
	{
		m_quiet_truncation = false;
		m_szbuf[ 0 ]       = 0;
		m_length           = 0;
	}

	void* vftable;
	bool m_quiet_truncation;
	char m_szbuf[ DEFAULT_BUF_SIZE ];
	int m_length;
};
