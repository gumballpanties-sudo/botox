#pragma once

class c_recv_proxy_data;

using recv_var_proxy_fn = void( __cdecl* )( const c_recv_proxy_data*, void*, void* );