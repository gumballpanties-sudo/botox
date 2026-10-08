#include "c_key_values.h"
#include "../../../globals/includes/includes.h"

using constructor_t      = void( __thiscall* )( void*, const char*, void*, int );
using load_from_buffer_t = void( __thiscall* )( void*, const char*, const char*, void*, const char*, void*, void* );

static constructor_t constructor_fn( )
{
	static const auto fn = reinterpret_cast< constructor_t >( g_modules[ CLIENT_DLL ].find_pattern( "55 8B EC 56 8B F1 33 C0 8B 4D 0C 81" ) );
	return fn;
}

static load_from_buffer_t load_from_buffer_fn( )
{
	static const auto fn =
		reinterpret_cast< load_from_buffer_t >( g_modules[ CLIENT_DLL ].find_pattern( "55 8B EC 83 E4 F8 83 EC 34 53 8B 5D 0C 89" ) );
	return fn;
}

bool c_key_values::usable( )
{
	return constructor_fn( ) && load_from_buffer_fn( );
}

c_key_values::c_key_values( const char* sz_key_name, void* p_unknown1, int h_case_insensitive_key_name )
{
	if ( const auto constructor = constructor_fn( ) )
		constructor( this, sz_key_name, p_unknown1, h_case_insensitive_key_name );
}

c_key_values::~c_key_values( )
{
	static auto destructor =
		reinterpret_cast< void( __thiscall* )( void*, int ) >( g_modules[ CLIENT_DLL ].find_pattern( "56 8B F1 E8 ? ? ? ? 8B 4E 14" ) );

	if ( destructor )
		destructor( this, 1 );
}

void* c_key_values::operator new( unsigned int alloc_size )
{
	return g_interfaces.m_key_values_system->alloc_key_values_memory( alloc_size );
}

void c_key_values::operator delete( void* p_memory )
{
	( void )p_memory;
}

const char* c_key_values::get_name( )
{
	return g_interfaces.m_key_values_system->get_string_for_symbol( this->m_key_name_case_sensitive1 | ( this->m_key_name_case_sensitive2 << 8 ) );
}

c_key_values* c_key_values::from_string( const char* name, const char* value )
{
	static auto original_from_string = g_modules[ CLIENT_DLL ].find_pattern( "55 8B EC 83 E4 F8 81 EC 0C 05" );

	c_key_values* key_values = nullptr;

	if ( original_from_string == 0U )
		return nullptr;

	__asm
	{
		push 0
		mov edx, value
		mov ecx, name
		call original_from_string
		add esp, 4
		mov key_values, eax
	}

	return key_values;
}

void c_key_values::load_from_buffer( char const* sz_resource_name, const char* sz_buffer, void* p_file_system, const char* sz_path_id,
                                     get_symbol_proc_fn pfn_evaluate_symbol_proc )
{
	if ( const auto original_load_from_buffer = load_from_buffer_fn( ) )
		original_load_from_buffer( this, sz_resource_name, sz_buffer, p_file_system, sz_path_id, pfn_evaluate_symbol_proc, nullptr );
}

bool c_key_values::load_from_file( void* p_file_system, const char* sz_resource_name, const char* sz_path_id,
                                   get_symbol_proc_fn pfn_evaluate_symbol_proc )
{
	static auto original_load_from_file = reinterpret_cast< bool( __thiscall* )( void*, void*, const char*, const char*, void* ) >(
		g_modules[ CLIENT_DLL ].find_pattern( "55 8B EC 83 E4 F8 83 EC 14 53 56 8B 75 08 57 FF" ) ); // @xref: "rb"

	if ( !original_load_from_file )
		return false;

	return original_load_from_file( this, p_file_system, sz_resource_name, sz_path_id, pfn_evaluate_symbol_proc );
}

c_key_values* c_key_values::find_key( const char* key_name, const bool create )
{
	static auto original_find_key = reinterpret_cast< c_key_values*( __thiscall* )( void*, const char*, bool ) >(
		g_modules[ CLIENT_DLL ].find_pattern( "55 8B EC 83 EC 1C 53 8B D9 85 DB" ) );

	if ( !original_find_key )
		return nullptr;

	return original_find_key( this, key_name, create );
}

int c_key_values::get_int( const char* key_name, const int default_value )
{
	c_key_values* sub_key = this->find_key( key_name, false );

	if ( sub_key == nullptr )
		return default_value;

	switch ( sub_key->m_data_type ) {
	case this->e_key_type::type_string:
		return std::atoi( sub_key->m_sz_value );
	case this->e_key_type::type_wstring:
		return _wtoi( sub_key->m_wsz_value );
	case this->e_key_type::type_float:
		return static_cast< int >( sub_key->m_fl_value );
	case this->e_key_type::type_uint64:
		assert( false );
		return 0;
	default:
		break;
	}

	return sub_key->m_i_value;
}

float c_key_values::get_float( const char* key_name, const float default_value )
{
	c_key_values* sub_key = this->find_key( key_name, false );

	if ( sub_key == nullptr )
		return default_value;

	switch ( sub_key->m_data_type ) {
	case this->e_key_type::type_string:
		return static_cast< float >( std::atof( sub_key->m_sz_value ) );
	case this->e_key_type::type_wstring:
		return std::wcstof( sub_key->m_wsz_value, nullptr );
	case this->e_key_type::type_float:
		return sub_key->m_fl_value;
	case this->e_key_type::type_int:
		return static_cast< float >( sub_key->m_i_value );
	case this->e_key_type::type_uint64:
		return static_cast< float >( *reinterpret_cast< std::uint64_t* >( sub_key->m_sz_value ) );
	case this->e_key_type::type_ptr:
	default:
		return 0.0f;
	}
}

const char* c_key_values::get_string( const char* key_name, const char* default_value )
{
	static auto original_get_string = reinterpret_cast< const char*( __thiscall* )( void*, const char*, const char* ) >(
		g_modules[ CLIENT_DLL ].find_pattern( "55 8B EC 83 E4 C0 81 EC ? ? ? ? 53 8B 5D 08" ) );

	if ( !original_get_string )
		return default_value;

	return original_get_string( this, key_name, default_value );
}

void c_key_values::set_string( const char* key_name, const char* string_value )
{
	c_key_values* sub_key = this->find_key( key_name, true );

	if ( sub_key == nullptr )
		return;

	static auto original_set_string = reinterpret_cast< void( __thiscall* )( void*, const char* ) >(
		g_modules[ CLIENT_DLL ].find_pattern( "55 8B EC A1 ? ? ? ? 53 56 57 8B F9 8B 08 8B 01" ) );

	if ( original_set_string )
		original_set_string( sub_key, string_value );
}

void c_key_values::set_int( const char* key_name, const int value )
{
	c_key_values* sub_key = this->find_key( key_name, true );

	if ( sub_key == nullptr )
		return;

	sub_key->m_i_value   = value;
	sub_key->m_data_type = this->e_key_type::type_int;
}

void c_key_values::set_uint64( const char* key_name, const int low_value, const int high_value )
{
	c_key_values* sub_key = this->find_key( key_name, true );

	if ( sub_key == nullptr )
		return;

	delete[] sub_key->m_sz_value;

	delete[] sub_key->m_wsz_value;
	sub_key->m_wsz_value = nullptr;

	sub_key->m_sz_value                                        = new char[ sizeof( std::uint64_t ) ];
	*reinterpret_cast< std::uint64_t* >( sub_key->m_sz_value ) = static_cast< std::uint64_t >( high_value ) << 32ULL | low_value;
	sub_key->m_data_type                                       = this->e_key_type::type_uint64;
}
