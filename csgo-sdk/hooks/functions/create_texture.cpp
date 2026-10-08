#include "../../globals/includes/includes.h"
#include "../../hacks/visuals/screen/depth_source.h"
#include "../hooks.h"

#include <cstring>
#include <intrin.h>

long __stdcall n_detoured_functions::create_texture( IDirect3DDevice9* device, unsigned int width, unsigned int height, unsigned int levels,
                                                     unsigned long usage, D3DFORMAT format, D3DPOOL pool, IDirect3DTexture9** texture,
                                                     void** shared_handle )
{
	static auto original = g_hooks.m_create_texture.get_original< decltype( &n_detoured_functions::create_texture ) >( );
	HOOK_SCOPE_OR_BAIL( original( device, width, height, levels, usage, format, pool, texture, shared_handle ) );

	const long result = original( device, width, height, levels, usage, format, pool, texture, shared_handle );

	if ( format == static_cast< D3DFORMAT >( MAKEFOURCC( 'I', 'N', 'T', 'Z' ) ) ) {
		HMODULE caller   = nullptr;
		char name[ MAX_PATH ]{ "?" };

		if ( GetModuleHandleExA( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		                         static_cast< const char* >( _ReturnAddress( ) ), &caller ) )
			GetModuleFileNameA( caller, name, sizeof( name ) );

		const char* file = std::strrchr( name, '\\' ) ? std::strrchr( name, '\\' ) + 1 : name;
		const DWORD thread = GetCurrentThreadId( );

		g_console.print( std::vformat( "create texture: INTZ {:d}x{:d} from {:s} thread {:d}", std::make_format_args( width, height, file, thread ) ).c_str( ) );
	}

	if ( SUCCEEDED( result ) && texture && *texture )
		g_depth_source.on_created_texture( device, *texture, width, height, format );

	return result;
}
