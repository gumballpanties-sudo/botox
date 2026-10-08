#pragma once
#include "../../datatypes/recv_var_proxy_fn.h"

class c_standart_recv_proxies
{
public:
	recv_var_proxy_fn m_int32_to_int8;
	recv_var_proxy_fn m_int32_to_int16;
	recv_var_proxy_fn m_int32_to_int32;
	recv_var_proxy_fn m_int64_to_int64;
	recv_var_proxy_fn m_float_to_float;
	recv_var_proxy_fn m_vector_to_vector;
};