#pragma once

enum e_send_prop_type;

struct data_variant_t {
	union {
		float m_float;
		long m_int;
		char* m_string;
		void* m_data;
		float m_vector[ 3 ];
		long long m_int64;
	};

	e_send_prop_type i_type;
};
