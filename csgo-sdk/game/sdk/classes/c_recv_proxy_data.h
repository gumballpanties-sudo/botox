#pragma once
#include "../structs/data_variant_t.h"

struct data_variant_t;
struct recv_prop_t;

class c_recv_proxy_data
{
public:
	const recv_prop_t* m_recv_prop;
	data_variant_t m_value;
	int m_element;
	int m_object_id;
};
