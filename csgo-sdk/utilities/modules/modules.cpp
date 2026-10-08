#include "modules.h"
#include "../../globals/includes/includes.h"
#include "../memory/structs/pe32.h"

unsigned char* module_t::find_pattern( const char* signature )
{
	static const auto pattern_to_byte = []( const char* pattern ) {
		std::vector< int > bytes = { };

		const auto start = const_cast< char* >( pattern );
		const auto end   = const_cast< char* >( pattern ) + std::strlen( pattern );

		for ( auto current = start; current < end; current++ ) {
			if ( *current == '?' ) {
				++current;

				if ( *current == '?' )
					++current;

				bytes.push_back( -1 );
			} else
				bytes.push_back( std::strtoul( current, &current, 16 ) );
		}

		return bytes;
	};

	/* a module that never loaded comes back as a default module_t - scanning it derefs null */
	if ( !this->m_value ) {
		g_console.print< n_console::log_level::WARNING >(
			std::vformat( "module not loaded, cannot scan for {:s}", std::make_format_args( signature ) ).c_str( ) );

		return nullptr;
	}

	const auto dos_header = reinterpret_cast< PIMAGE_DOS_HEADER >( this->m_value );
	const auto nt_headers = reinterpret_cast< PIMAGE_NT_HEADERS >( reinterpret_cast< unsigned char* >( this->m_value ) + dos_header->e_lfanew );

	const auto pattern_bytes = pattern_to_byte( signature );
	const auto scan_bytes    = reinterpret_cast< unsigned char* >( this->m_value );

	const auto s = pattern_bytes.size( );
	const auto d = pattern_bytes.data( );

	/* code sections only: on another build a sig gone from .text can still hit data, and that pointer gets called */
	auto section = IMAGE_FIRST_SECTION( nt_headers );
	for ( unsigned short n = 0; n < nt_headers->FileHeader.NumberOfSections; n++, section++ ) {
		const unsigned long size = section->Misc.VirtualSize ? section->Misc.VirtualSize : section->SizeOfRawData;
		if ( !( section->Characteristics & IMAGE_SCN_MEM_EXECUTE ) || size < s )
			continue;

		const auto start = scan_bytes + section->VirtualAddress;

		for ( auto i = 0ul; i <= size - s; ++i ) {
			bool found = true;

			for ( auto j = 0ul; j < s; ++j ) {
				if ( start[ i + j ] != d[ j ] && d[ j ] != -1 ) {
					found = false;
					break;
				}
			}

			if ( found )
				return &start[ i ];
		}
	}

	const char* module_name = this->m_name.empty( ) ? "?" : this->m_name.c_str( );

	g_console.print< n_console::log_level::WARNING >(
		std::vformat( "failed to find pattern {:s} in {:s}", std::make_format_args( signature, module_name ) ).c_str( ) );

	return nullptr;
}

void* module_t::find_interface( const char* interface_name )
{
	/* call the export, never walk its internals: the jmp + register list byte layout is per build, the export is not */
	using create_interface_t      = void*( __cdecl* )( const char*, int* );
	const auto create_interface   = reinterpret_cast< create_interface_t >( this->find_export( HASH_BT( "CreateInterface" ) ) );
	void* const interface_address = create_interface ? create_interface( interface_name, nullptr ) : nullptr;

	if ( interface_address )
		g_console.print( std::vformat( "found {:s} interface @ {:p}", std::make_format_args( interface_name, interface_address ) ).c_str( ) );
	else
		g_console.print( std::vformat( "failed to capture interface {:s}", std::make_format_args( interface_name ) ).c_str( ) );

	return interface_address;
}

bool module_t::contains( const void* address )
{
	if ( !this->m_value )
		return false;

	const auto base = reinterpret_cast< std::uintptr_t >( this->m_value );
	const auto nt   = reinterpret_cast< PIMAGE_NT_HEADERS >( base + reinterpret_cast< PIMAGE_DOS_HEADER >( base )->e_lfanew );
	return reinterpret_cast< std::uintptr_t >( address ) - base < nt->OptionalHeader.SizeOfImage;
}

void* module_t::find_export( unsigned int hash )
{
	// a module that never loaded is a default module_t, every read below would deref null
	if ( !this->m_value )
		return nullptr;

	const auto converted_value = reinterpret_cast< unsigned int >( this->m_value );

	auto dos_headers                       = reinterpret_cast< IMAGE_DOS_HEADER* >( this->m_value );
	auto nt_headers                        = reinterpret_cast< IMAGE_NT_HEADERS* >( converted_value + dos_headers->e_lfanew );
	IMAGE_OPTIONAL_HEADER* optional_header = &nt_headers->OptionalHeader;

	unsigned int exportdir_address = optional_header->DataDirectory[ IMAGE_DIRECTORY_ENTRY_EXPORT ].VirtualAddress;

	if ( optional_header->DataDirectory[ IMAGE_DIRECTORY_ENTRY_EXPORT ].Size <= 0U )
		return nullptr;

	auto export_directory = reinterpret_cast< IMAGE_EXPORT_DIRECTORY* >( converted_value + exportdir_address );
	auto names_rva        = reinterpret_cast< unsigned int* >( converted_value + export_directory->AddressOfNames );
	auto functions_rva    = reinterpret_cast< unsigned int* >( converted_value + export_directory->AddressOfFunctions );
	auto name_ordinals    = reinterpret_cast< unsigned short* >( converted_value + export_directory->AddressOfNameOrdinals );

	for ( unsigned int i = 0; i < export_directory->NumberOfNames; i++ ) {
		if ( HASH_RT( reinterpret_cast< const char* >( converted_value + names_rva[ i ] ) ) == hash )
			return reinterpret_cast< void* >( converted_value + functions_rva[ name_ordinals[ i ] ] );
	}

	return nullptr;
}

bool n_modules::impl_t::on_attach( )
{
	const _PEB32* peb = reinterpret_cast< _PEB32* >( __readfsdword( 0x30 ) );

	for ( int pass = 0; pass < 3000 && this->m_modules.find( SERVERBROWSER_DLL ) == this->m_modules.end( ); pass++ ) {
		if ( pass > 0 )
			Sleep( 10 );

		for ( LIST_ENTRY* list_entry = peb->Ldr->InLoadOrderModuleList.Flink; list_entry != &peb->Ldr->InLoadOrderModuleList;
		      list_entry             = list_entry->Flink ) {
			const _LDR_DATA_TABLE_ENTRY* entry = CONTAINING_RECORD( list_entry, _LDR_DATA_TABLE_ENTRY, InLoadOrderLinks );

			if ( entry->BaseDllName.Buffer ) {
				const auto dll_buffer = std::wstring( entry->BaseDllName.Buffer );
				const std::string converted_dll_buffer( dll_buffer.begin( ), dll_buffer.end( ) );

				const auto converted_name = converted_dll_buffer.c_str( );

				this->m_modules[ HASH_RT( converted_name ) ] = module_t( entry->DllBase, converted_name );
			}
		}
	}

	/* 2018 to 2020 builds ship the client as client_panorama.dll */
	if ( this->m_modules.find( CLIENT_DLL ) == this->m_modules.end( ) ) {
		if ( const auto panorama_client = this->m_modules.find( HASH_BT( "client_panorama.dll" ) ); panorama_client != this->m_modules.end( ) )
			this->m_modules[ CLIENT_DLL ] = panorama_client->second;
	}

	for ( const unsigned int hash : { CLIENT_DLL, ENGINE_DLL, PANORAMA_DLL, SHADERAPIDX9_DLL, VSTDLIB_DLL, MATERIALSYSTEM_DLL } ) {
		module_t module = ( *this )[ hash ];

		if ( !module.get_value( ) ) {
			g_console.print< n_console::log_level::WARNING >( "module for hash not loaded" );
			continue;
		}

		const auto dos_header = reinterpret_cast< PIMAGE_DOS_HEADER >( module.get_value( ) );
		const auto nt_headers =
			reinterpret_cast< PIMAGE_NT_HEADERS >( reinterpret_cast< unsigned char* >( module.get_value( ) ) + dos_header->e_lfanew );

		g_console.print( std::format( "module {:s} @ {:p} size {:#x} checksum {:#x} stamp {:#x}", module.get_name( ), module.get_value( ),
		                              nt_headers->OptionalHeader.SizeOfImage, nt_headers->OptionalHeader.CheckSum,
		                              nt_headers->FileHeader.TimeDateStamp )
		                     .c_str( ) );
	}

	/* every interface, sig and vtable comes out of these six, so a miss fails the whole init */
	for ( const unsigned int hash : { CLIENT_DLL, ENGINE_DLL, PANORAMA_DLL, SHADERAPIDX9_DLL, VSTDLIB_DLL, MATERIALSYSTEM_DLL } ) {
		if ( !( *this )[ hash ].get_value( ) )
			return false;
	}

	return true;
}

module_t n_modules::impl_t::operator[]( unsigned int hash )
{
	return this->m_modules[ hash ];
}
