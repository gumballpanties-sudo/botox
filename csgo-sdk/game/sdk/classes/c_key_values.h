#pragma once

#include "../classes/c_key_values_system.h"

using get_symbol_proc_fn = bool( __cdecl* )( const char* );

class c_key_values
{
public:
	enum e_key_type : int {
		type_none = 0,
		type_string,
		type_int,
		type_float,
		type_ptr,
		type_wstring,
		type_color,
		type_uint64,
		type_compiled_int_byte,
		type_compiled_int_0,
		type_compiled_int_1,
		type_numtypes
	};

	c_key_values( const char* sz_key_name, void* p_unknown1 = nullptr, int h_case_insensitive_key_name = -1 );
	~c_key_values( );

	void* operator new( unsigned int n_alloc_size );
	void operator delete( void* p_memory );

	const char* get_name( );

	/* ctor + load_from_buffer sigs hit: check before new c_key_values, a miss can't construct one */
	static bool usable( );

	static c_key_values* from_string( const char* sz_name, const char* sz_value );
	void load_from_buffer( char const* sz_resource_name, const char* sz_buffer, void* p_file_system = nullptr, const char* sz_path_id = nullptr,
	                       get_symbol_proc_fn pfn_evaluate_symbol_proc = nullptr );
	bool load_from_file( void* p_file_system, const char* sz_resource_name, const char* sz_path_id = nullptr,
	                     get_symbol_proc_fn pfn_evaluate_symbol_proc = nullptr );

	c_key_values* find_key( const char* sz_key_name, const bool b_create );

	int get_int( const char* sz_key_name, const int i_default_value );
	float get_float( const char* sz_key_name, const float fl_default_value );
	const char* get_string( const char* sz_key_name, const char* sz_default_value );

	void set_string( const char* sz_key_name, const char* sz_string_value );
	void set_int( const char* sz_key_name, const int i_value );
	void set_uint64( const char* sz_key_name, const int n_low_value, const int n_high_value );

	inline void set_bool( const char* sz_key_name, const bool b_value )
	{
		set_int( sz_key_name, b_value ? 1 : 0 );
	}

private:
	unsigned int m_key_name                 : 24;
	unsigned int m_key_name_case_sensitive1 : 8;
	char* m_sz_value;
	wchar_t* m_wsz_value;

	union {
		int m_i_value;
		float m_fl_value;
		void* m_p_value;
		unsigned char m_color[ 4 ];
	}; // 0x0c

	signed char m_data_type;
	bool m_has_escape_sequences;
	unsigned short m_key_name_case_sensitive2;
	void* m_unknown14;
	bool m_has_case_insensitive_key_symbol;
	c_key_values* m_peer;
	c_key_values* m_sub;
	c_key_values* m_chain;
	get_symbol_proc_fn m_expression_get_symbol_proc;
};
static_assert( sizeof( c_key_values ) == 0x2C );
